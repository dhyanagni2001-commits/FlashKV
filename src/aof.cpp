#include "aof.hpp"
#include "resp.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cctype>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string_view>
#include <system_error>

namespace {

bool parseInteger(
    const std::string& text,
    long long& value
) {
    const char* begin = text.data();
    const char* end = text.data() + text.size();

    auto result = std::from_chars(
        begin,
        end,
        value
    );

    return result.ec == std::errc{} &&
           result.ptr == end;
}

std::string uppercase(std::string value) {
    std::transform(
        value.begin(),
        value.end(),
        value.begin(),
        [](unsigned char character) {
            return static_cast<char>(
                std::toupper(character)
            );
        }
    );

    return value;
}

long long currentUnixSeconds() {
    return std::chrono::duration_cast<
        std::chrono::seconds
    >(
        std::chrono::system_clock::now()
            .time_since_epoch()
    ).count();
}

} // namespace

AppendOnlyFile::AppendOnlyFile(
    const std::string& filePath
)
    : path(filePath),
      output(
          filePath,
          std::ios::binary | std::ios::app
      ) {
}

bool AppendOnlyFile::append(
    const std::vector<std::string>& arguments
) {
    if (!output.is_open()) {
        return false;
    }

    std::string encoded = respArray(arguments);

    output.write(
        encoded.data(),
        static_cast<std::streamsize>(encoded.size())
    );

    output.flush();

    return output.good();
}

bool AppendOnlyFile::applyCommand(
    Database& database,
    const std::vector<std::string>& arguments
) {
    if (arguments.empty()) {
        return false;
    }

    std::string command = uppercase(arguments[0]);

    if (command == "SET" && arguments.size() == 3) {
        database.set(arguments[1], arguments[2]);
        return true;
    }

    if (command == "DEL" && arguments.size() >= 2) {
        for (std::size_t index = 1;
             index < arguments.size();
             ++index) {
            database.del(arguments[index]);
        }

        return true;
    }

    /*
     * Expirations are stored as absolute Unix timestamps.
     * This prevents a five-second TTL from restarting at
     * five seconds whenever FlashKV restarts.
     */
    if (
        command == "EXPIREAT" &&
        arguments.size() == 3
    ) {
        long long expirationTime = 0;

        if (!parseInteger(
                arguments[2],
                expirationTime
            )) {
            return false;
        }

        long long remainingSeconds =
            expirationTime - currentUnixSeconds();

        database.expire(
            arguments[1],
            remainingSeconds
        );

        return true;
    }

    return false;
}

bool AppendOnlyFile::load(Database& database) {
    std::ifstream input(
        path,
        std::ios::binary
    );

    if (!input.is_open()) {
        return true;
    }

    std::string contents{
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()
    };

    std::size_t position = 0;

    while (position < contents.size()) {
        std::string_view remaining(
            contents.data() + position,
            contents.size() - position
        );

        RespParseResult result =
            parseRespCommand(remaining);

        if (
            result.status !=
            RespParseStatus::Complete
        ) {
            std::cerr
                << "Invalid or incomplete AOF data\n";

            return false;
        }

        if (
            !applyCommand(
                database,
                result.arguments
            )
        ) {
            std::cerr
                << "Unsupported command in AOF\n";

            return false;
        }

        position += result.consumedBytes;
    }

    return true;
}