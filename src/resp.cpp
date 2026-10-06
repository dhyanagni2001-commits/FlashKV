#include "resp.hpp"

#include <algorithm>
#include <charconv>
#include <optional>
#include <string_view>

namespace {

constexpr std::size_t MAX_ARGUMENTS = 1024 * 1024;
constexpr std::size_t MAX_BULK_SIZE = 512 * 1024 * 1024;
constexpr std::size_t MAX_INLINE_SIZE = 64 * 1024;

bool parseInteger(
    std::string_view text,
    long long& value
) {
    const char* begin = text.data();
    const char* end = text.data() + text.size();

    auto result = std::from_chars(begin, end, value);

    return result.ec == std::errc{} &&
           result.ptr == end;
}

RespParseResult incompleteResult() {
    return {
        RespParseStatus::Incomplete,
        {},
        0,
        {}
    };
}

RespParseResult errorResult(
    const std::string& message
) {
    return {
        RespParseStatus::Error,
        {},
        0,
        message
    };
}

int hexValue(char character) {
    if (character >= '0' && character <= '9') {
        return character - '0';
    }

    if (character >= 'a' && character <= 'f') {
        return character - 'a' + 10;
    }

    if (character >= 'A' && character <= 'F') {
        return character - 'A' + 10;
    }

    return -1;
}

bool isSpace(char character) {
    return character == ' ' || character == '\t' ||
           character == '\r' || character == '\n';
}

RespParseResult parseInlineCommand(
    std::string_view buffer
) {
    std::size_t lineEnd = buffer.find('\n');

    if (lineEnd == std::string_view::npos) {
        if (buffer.size() > MAX_INLINE_SIZE) {
            return errorResult(
                "Protocol error: too big inline request"
            );
        }

        return incompleteResult();
    }

    std::optional<std::vector<std::string>> arguments =
        tokenizeCommandLine(buffer.substr(0, lineEnd));

    if (!arguments.has_value()) {
        return errorResult(
            "Protocol error: unbalanced quotes in request"
        );
    }

    return {
        RespParseStatus::Complete,
        std::move(*arguments),
        lineEnd + 1,
        {}
    };
}

} // namespace

std::optional<std::vector<std::string>> tokenizeCommandLine(
    std::string_view line
) {
    std::vector<std::string> arguments;
    std::size_t position = 0;

    while (true) {
        while (position < line.size() &&
               isSpace(line[position])) {
            ++position;
        }

        if (position >= line.size()) {
            return arguments;
        }

        std::string current;
        bool inDoubleQuotes = false;
        bool inSingleQuotes = false;

        while (true) {
            if (position >= line.size()) {
                if (inDoubleQuotes || inSingleQuotes) {
                    return std::nullopt;
                }

                break;
            }

            char character = line[position];

            if (inDoubleQuotes) {
                if (character == '\\' &&
                    position + 1 < line.size()) {
                    char escaped = line[++position];

                    if (escaped == 'x' &&
                        position + 2 < line.size() &&
                        hexValue(line[position + 1]) >= 0 &&
                        hexValue(line[position + 2]) >= 0) {
                        current += static_cast<char>(
                            hexValue(line[position + 1]) * 16 +
                            hexValue(line[position + 2])
                        );
                        position += 2;
                    } else {
                        switch (escaped) {
                        case 'n': current += '\n'; break;
                        case 'r': current += '\r'; break;
                        case 't': current += '\t'; break;
                        case 'b': current += '\b'; break;
                        case 'a': current += '\a'; break;
                        default: current += escaped; break;
                        }
                    }
                } else if (character == '"') {
                    // A closing quote must end the argument.
                    if (position + 1 < line.size() &&
                        !isSpace(line[position + 1])) {
                        return std::nullopt;
                    }

                    inDoubleQuotes = false;
                    ++position;
                    break;
                } else {
                    current += character;
                }
            } else if (inSingleQuotes) {
                if (character == '\\' &&
                    position + 1 < line.size() &&
                    line[position + 1] == '\'') {
                    current += '\'';
                    ++position;
                } else if (character == '\'') {
                    if (position + 1 < line.size() &&
                        !isSpace(line[position + 1])) {
                        return std::nullopt;
                    }

                    inSingleQuotes = false;
                    ++position;
                    break;
                } else {
                    current += character;
                }
            } else if (isSpace(character)) {
                break;
            } else if (character == '"' && current.empty()) {
                inDoubleQuotes = true;
            } else if (character == '\'' && current.empty()) {
                inSingleQuotes = true;
            } else {
                current += character;
            }

            ++position;
        }

        arguments.push_back(std::move(current));
    }
}

RespParseResult parseRespCommand(
    std::string_view buffer
) {
    if (buffer.empty()) {
        return incompleteResult();
    }

    // Redis commands are normally RESP arrays; anything else is
    // treated as an inline command.
    if (buffer.front() != '*') {
        return parseInlineCommand(buffer);
    }

    std::size_t countEnd = buffer.find("\r\n", 1);

    if (countEnd == std::string_view::npos) {
        return incompleteResult();
    }

    long long argumentCount = 0;

    if (!parseInteger(
            buffer.substr(1, countEnd - 1),
            argumentCount
        )) {
        return errorResult("Invalid array length");
    }

    if (
        argumentCount < 0 ||
        argumentCount >
            static_cast<long long>(MAX_ARGUMENTS)
    ) {
        return errorResult("Invalid array length");
    }

    std::size_t position = countEnd + 2;
    std::vector<std::string> arguments;

    // Cap the reservation so a huge declared count cannot
    // allocate memory before any data has arrived.
    arguments.reserve(
        std::min<std::size_t>(
            static_cast<std::size_t>(argumentCount),
            1024
        )
    );

    for (long long index = 0;
         index < argumentCount;
         ++index) {
        if (position >= buffer.size()) {
            return incompleteResult();
        }

        if (buffer[position] != '$') {
            return errorResult(
                "Expected RESP bulk string"
            );
        }

        std::size_t lengthEnd =
            buffer.find("\r\n", position + 1);

        if (lengthEnd == std::string_view::npos) {
            return incompleteResult();
        }

        long long bulkLength = 0;

        if (!parseInteger(
                buffer.substr(
                    position + 1,
                    lengthEnd - position - 1
                ),
                bulkLength
            )) {
            return errorResult(
                "Invalid bulk string length"
            );
        }

        if (
            bulkLength < 0 ||
            bulkLength >
                static_cast<long long>(MAX_BULK_SIZE)
        ) {
            return errorResult(
                "Invalid bulk string length"
            );
        }

        std::size_t dataStart = lengthEnd + 2;
        std::size_t dataLength =
            static_cast<std::size_t>(bulkLength);

        if (
            dataStart > buffer.size() ||
            dataLength > buffer.size() - dataStart
        ) {
            return incompleteResult();
        }

        std::size_t dataEnd = dataStart + dataLength;

        if (buffer.size() - dataEnd < 2) {
            return incompleteResult();
        }

        if (buffer.substr(dataEnd, 2) != "\r\n") {
            return errorResult(
                "Bulk string missing CRLF"
            );
        }

        arguments.emplace_back(
            buffer.substr(dataStart, dataLength)
        );

        position = dataEnd + 2;
    }

    return {
        RespParseStatus::Complete,
        std::move(arguments),
        position,
        {}
    };
}

std::string respSimpleString(
    const std::string& value
) {
    return "+" + value + "\r\n";
}

std::string respError(
    const std::string& message
) {
    return "-ERR " + message + "\r\n";
}

std::string respErrorWithCode(
    const std::string& code,
    const std::string& message
) {
    return "-" + code + " " + message + "\r\n";
}

std::string respInteger(long long value) {
    return ":" + std::to_string(value) + "\r\n";
}

std::string respBulkString(
    const std::string& value
) {
    return "$" +
           std::to_string(value.size()) +
           "\r\n" +
           value +
           "\r\n";
}

std::string respNull() {
    return "$-1\r\n";
}

std::string respArray(
    const std::vector<std::string>& arguments
) {
    std::string encoded =
        "*" + std::to_string(arguments.size()) + "\r\n";

    for (const std::string& argument : arguments) {
        encoded += respBulkString(argument);
    }

    return encoded;
}

std::string respEncodedArray(
    const std::vector<std::string>& encodedElements
) {
    std::string encoded =
        "*" + std::to_string(encodedElements.size()) + "\r\n";

    for (const std::string& element : encodedElements) {
        encoded += element;
    }

    return encoded;
}
