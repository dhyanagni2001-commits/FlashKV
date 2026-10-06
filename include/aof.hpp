#pragma once

#include <functional>
#include <string>
#include <vector>

/*
 * Append-only file persistence.
 *
 * Write commands are buffered with append() and written to the
 * file by flush(), which the server calls before sending replies.
 * sync() forces the data to disk and is called once per second.
 */
class AppendOnlyFile {
public:
    using ReplayFunction = std::function<bool(
        const std::vector<std::string>& arguments,
        std::string& error
    )>;

    explicit AppendOnlyFile(std::string filePath);
    ~AppendOnlyFile();

    AppendOnlyFile(const AppendOnlyFile&) = delete;
    AppendOnlyFile& operator=(const AppendOnlyFile&) = delete;

    /*
     * Replays every command in the file. A truncated final command
     * (for example after a crash mid-write) is removed from the file
     * with a warning instead of failing the whole load.
     */
    bool load(
        const ReplayFunction& replay,
        std::string& error
    );

    // Opens the file for appending. Call after load().
    bool open(std::string& error);

    void append(const std::vector<std::string>& arguments);

    bool flush();
    bool sync();

    // False after a write error; write commands are then refused.
    bool healthy() const;

    const std::string& path() const;

private:
    std::string filePath;
    std::string buffer;
    int fileDescriptor = -1;
    bool needsSync = false;
    bool writeFailed = false;
};
