#include "reply_format.hpp"

#include <cstdio>
#include <string>

namespace {

std::string quote(std::string_view value) {
    std::string quoted = "\"";

    for (unsigned char character : value) {
        switch (character) {
        case '\\': quoted += "\\\\"; break;
        case '"': quoted += "\\\""; break;
        case '\n': quoted += "\\n"; break;
        case '\r': quoted += "\\r"; break;
        case '\t': quoted += "\\t"; break;
        default:
            if (character < 0x20 || character >= 0x7f) {
                char escaped[8];
                std::snprintf(escaped, sizeof(escaped), "\\x%02x", character);
                quoted += escaped;
            } else {
                quoted += static_cast<char>(character);
            }
        }
    }

    return quoted + "\"";
}

class ReplyReader {
public:
    ReplyReader(std::string_view reply, bool raw)
        : reply(reply),
          raw(raw) {
    }

    // Formats one value; `indent` is the prefix width for nested arrays.
    bool format(std::string& output, std::size_t indent) {
        if (position >= reply.size()) {
            return false;
        }

        char prefix = reply[position++];
        std::string_view line;

        if (!readLine(line)) {
            return false;
        }

        switch (prefix) {
        case '+':
            output += line;
            return true;

        case '-':
            output += "(error) ";
            output += line;
            return true;

        case ':':
            output += "(integer) ";
            output += line;
            return true;

        case '$': {
            long long length = std::stoll(std::string(line));

            if (length < 0) {
                output += "(nil)";
                return true;
            }

            auto size = static_cast<std::size_t>(length);

            if (position + size + 2 > reply.size()) {
                return false;
            }

            std::string_view value = reply.substr(position, size);
            position += size + 2;

            output += raw ? std::string(value) : quote(value);
            return true;
        }

        case '*': {
            long long count = std::stoll(std::string(line));

            if (count < 0) {
                output += "(nil)";
                return true;
            }

            if (count == 0) {
                output += "(empty array)";
                return true;
            }

            std::size_t width = std::to_string(count).size();

            for (long long i = 1; i <= count; ++i) {
                std::string label = std::to_string(i);

                if (i > 1) {
                    output += "\n" + std::string(indent, ' ');
                }

                output += std::string(width - label.size(), ' ');
                output += label + ") ";

                if (!format(output, indent + width + 2)) {
                    return false;
                }
            }

            return true;
        }

        default:
            return false;
        }
    }

private:
    std::string_view reply;
    bool raw;
    std::size_t position = 0;

    bool readLine(std::string_view& line) {
        std::size_t end = reply.find("\r\n", position);

        if (end == std::string_view::npos) {
            return false;
        }

        line = reply.substr(position, end - position);
        position = end + 2;

        return true;
    }
};

} // namespace

std::string formatReply(std::string_view reply, bool raw) {
    std::string output;
    ReplyReader reader(reply, raw);

    try {
        if (!reader.format(output, 0)) {
            return "(protocol error)";
        }
    } catch (const std::exception&) {
        return "(protocol error)";
    }

    return output;
}
