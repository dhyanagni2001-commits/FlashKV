#include "aof.hpp"
#include "resp.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string_view>
#include <unistd.h>
#include <utility>

AppendOnlyFile::AppendOnlyFile(std::string path)
    : filePath(std::move(path)) {
}

AppendOnlyFile::~AppendOnlyFile() {
    flush();
    sync();

    if (fileDescriptor != -1) {
        close(fileDescriptor);
    }
}

bool AppendOnlyFile::load(
    const ReplayFunction& replay,
    std::string& error
) {
    std::ifstream input(filePath, std::ios::binary);

    // A missing file simply means an empty database.
    if (!input.is_open()) {
        return true;
    }

    std::string contents{
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()
    };

    input.close();

    std::size_t position = 0;
    std::size_t commandNumber = 0;

    while (position < contents.size()) {
        std::string_view remaining(
            contents.data() + position,
            contents.size() - position
        );

        RespParseResult result = parseRespCommand(remaining);

        if (result.status == RespParseStatus::Incomplete) {
            std::cerr
                << "Warning: AOF ends with a truncated command; "
                << "discarding the last "
                << remaining.size()
                << " bytes\n";

            if (truncate(
                    filePath.c_str(),
                    static_cast<off_t>(position)
                ) == -1) {
                error = "failed to truncate AOF: " +
                        std::string(std::strerror(errno));
                return false;
            }

            return true;
        }

        if (result.status == RespParseStatus::Error) {
            error = "invalid AOF data at byte " +
                    std::to_string(position) + ": " +
                    result.error;
            return false;
        }

        ++commandNumber;

        if (!result.arguments.empty()) {
            std::string replayError;

            if (!replay(result.arguments, replayError)) {
                error = "AOF command #" +
                        std::to_string(commandNumber) + " (" +
                        result.arguments[0] + ") failed: " +
                        replayError;
                return false;
            }
        }

        position += result.consumedBytes;
    }

    return true;
}

bool AppendOnlyFile::open(std::string& error) {
    fileDescriptor = ::open(
        filePath.c_str(),
        O_WRONLY | O_APPEND | O_CREAT,
        0644
    );

    if (fileDescriptor == -1) {
        error = "failed to open " + filePath + ": " +
                std::strerror(errno);
        return false;
    }

    return true;
}

void AppendOnlyFile::append(
    const std::vector<std::string>& arguments
) {
    buffer += respArray(arguments);
}

bool AppendOnlyFile::flush() {
    if (buffer.empty()) {
        return !writeFailed;
    }

    if (fileDescriptor == -1) {
        return false;
    }

    std::size_t written = 0;

    while (written < buffer.size()) {
        ssize_t result = write(
            fileDescriptor,
            buffer.data() + written,
            buffer.size() - written
        );

        if (result < 0 && errno == EINTR) {
            continue;
        }

        if (result <= 0) {
            std::cerr << "AOF write failed: "
                      << std::strerror(errno) << '\n';

            // Keep the unwritten data so a later flush can retry.
            buffer.erase(0, written);
            writeFailed = true;
            return false;
        }

        written += static_cast<std::size_t>(result);
    }

    buffer.clear();
    needsSync = true;
    writeFailed = false;

    return true;
}

bool AppendOnlyFile::sync() {
    if (!needsSync || fileDescriptor == -1) {
        return true;
    }

    needsSync = false;
    return fsync(fileDescriptor) == 0;
}

bool AppendOnlyFile::healthy() const {
    return !writeFailed;
}

const std::string& AppendOnlyFile::path() const {
    return filePath;
}
