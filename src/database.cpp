#include "database.hpp"

bool Database::removeIfExpired(
    const std::string& key
) {
    auto entry = data.find(key);

    if (entry == data.end()) {
        return false;
    }

    if (entry->second.expiresAt &&
        Clock::now() >= *entry->second.expiresAt) {
        data.erase(entry);
        return true;
    }

    return false;
}

void Database::set(
    const std::string& key,
    const std::string& value
) {
    data[key] = Entry{
        value,
        std::nullopt
    };
}

std::optional<std::string> Database::get(
    const std::string& key
) {
    removeIfExpired(key);

    auto entry = data.find(key);

    if (entry == data.end()) {
        return std::nullopt;
    }

    return entry->second.value;
}

bool Database::del(const std::string& key) {
    if (removeIfExpired(key)) {
        return false;
    }

    return data.erase(key) > 0;
}

bool Database::exists(const std::string& key) {
    removeIfExpired(key);
    return data.contains(key);
}

bool Database::expire(
    const std::string& key,
    long long seconds
) {
    if (removeIfExpired(key)) {
        return false;
    }

    auto entry = data.find(key);

    if (entry == data.end()) {
        return false;
    }

    if (seconds <= 0) {
        data.erase(entry);
        return true;
    }

    entry->second.expiresAt =
        Clock::now() + std::chrono::seconds(seconds);

    return true;
}

long long Database::ttl(const std::string& key) {
    auto entry = data.find(key);

    if (entry == data.end()) {
        return -2;
    }

    if (!entry->second.expiresAt.has_value()) {
        return -1;
    }

    auto now = Clock::now();
    auto expiration = entry->second.expiresAt.value();

    if (now >= expiration) {
        data.erase(entry);
        return -2;
    }

    return std::chrono::duration_cast<
        std::chrono::seconds
    >(expiration - now).count();
}