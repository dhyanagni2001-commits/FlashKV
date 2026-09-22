#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <unordered_map>

class Database {
private:
    using Clock = std::chrono::steady_clock;

    struct Entry {
        std::string value;
        std::optional<Clock::time_point> expiresAt;
    };

    std::unordered_map<std::string, Entry> data;

    bool removeIfExpired(const std::string& key);

public:
    void set(
        const std::string& key,
        const std::string& value
    );

    std::optional<std::string> get(
        const std::string& key
    );

    bool del(const std::string& key);

    bool exists(const std::string& key);

    bool expire(
        const std::string& key,
        long long seconds
    );

    long long ttl(const std::string& key);
};