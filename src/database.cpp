#include "database.hpp"

void Database::set(
    const std::string& key,
    const std::string& value
) {
    data[key] = value;
}

std::optional<std::string> Database::get(
    const std::string& key
) const {
    auto entry = data.find(key);

    if (entry == data.end()) {
        return std::nullopt;
    }

    return entry->second;
}

bool Database::del(const std::string& key) {
    return data.erase(key) > 0;
}

bool Database::exists(const std::string& key) const {
    return data.contains(key);
}