#include "resp.hpp"

#include <charconv>
#include <string_view>

namespace {

constexpr std::size_t MAX_ARGUMENTS = 1024;
constexpr std::size_t MAX_BULK_SIZE = 16 * 1024 * 1024;

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

} // namespace

RespParseResult parseRespCommand(
    std::string_view buffer
) {
    if (buffer.empty()) {
        return incompleteResult();
    }

    // Redis commands are represented as RESP arrays.
    if (buffer.front() != '*') {
        return errorResult("Expected RESP array");
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

    arguments.reserve(
        static_cast<std::size_t>(argumentCount)
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