/*
 * WebAssembly bindings for the FlashKV demo page.
 *
 * Exposes the real storage engine and command layer to JavaScript:
 * the browser demo runs exactly the same C++ code as flashkv_server,
 * minus the TCP event loop and AOF.
 */

#include "commands.hpp"
#include "database.hpp"
#include "reply_format.hpp"
#include "resp.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <string>

namespace {

Database& database() {
    static Database instance;
    return instance;
}

CommandProcessor& processor() {
    static CommandProcessor instance(database());
    return instance;
}

// Returned strings must outlive the call; JS copies them immediately.
std::string& outputBuffer() {
    static std::string buffer;
    return buffer;
}

void appendJsonString(std::string& json, const std::string& value) {
    json += '"';

    for (unsigned char character : value) {
        switch (character) {
        case '"': json += "\\\""; break;
        case '\\': json += "\\\\"; break;
        case '\n': json += "\\n"; break;
        case '\r': json += "\\r"; break;
        case '\t': json += "\\t"; break;
        default:
            if (character < 0x20) {
                char escaped[8];
                std::snprintf(escaped, sizeof(escaped), "\\u%04x", character);
                json += escaped;
            } else {
                json += static_cast<char>(character);
            }
        }
    }

    json += '"';
}

constexpr std::size_t PREVIEW_ITEMS = 6;
constexpr std::size_t PREVIEW_CHARS = 48;

std::string truncate(const std::string& value) {
    if (value.size() <= PREVIEW_CHARS) {
        return value;
    }

    return value.substr(0, PREVIEW_CHARS) + "…";
}

void appendPreview(std::string& json, const Value& value) {
    json += "\"length\":";

    if (auto* text = std::get_if<StringValue>(&value)) {
        json += std::to_string(text->size()) + ",\"preview\":";
        appendJsonString(json, truncate(*text));
        return;
    }

    std::vector<std::string> items;
    std::size_t length = 0;

    if (auto* list = std::get_if<ListValue>(&value)) {
        length = list->size();

        for (std::size_t i = 0; i < std::min(PREVIEW_ITEMS, length); ++i) {
            items.push_back(truncate((*list)[i]));
        }
    } else if (auto* hash = std::get_if<HashValue>(&value)) {
        length = hash->size();
        std::vector<std::string> fields;

        for (const auto& [field, fieldValue] : *hash) {
            fields.push_back(truncate(field) + ": " + truncate(fieldValue));
        }

        std::sort(fields.begin(), fields.end());
        fields.resize(std::min(PREVIEW_ITEMS, fields.size()));
        items = std::move(fields);
    } else if (auto* set = std::get_if<SetValue>(&value)) {
        length = set->size();
        std::vector<std::string> members(set->begin(), set->end());

        std::sort(members.begin(), members.end());
        members.resize(std::min(PREVIEW_ITEMS, members.size()));

        for (auto& member : members) {
            items.push_back(truncate(member));
        }
    }

    json += std::to_string(length) + ",\"preview\":[";

    for (std::size_t i = 0; i < items.size(); ++i) {
        if (i > 0) {
            json += ',';
        }

        appendJsonString(json, items[i]);
    }

    json += ']';
}

} // namespace

extern "C" {

/*
 * Executes one command line (e.g. `SET name "Ada Lovelace"`) and
 * returns the reply formatted like redis-cli. The first character
 * of the result tags the reply kind for styling:
 *   'e' error, 'n' nil, 'v' anything else.
 */
const char* flashkv_execute(const char* line) {
    std::string& output = outputBuffer();
    auto arguments = tokenizeCommandLine(line);

    if (!arguments.has_value()) {
        output = "e(error) Invalid argument(s): unbalanced quotes";
        return output.c_str();
    }

    if (arguments->empty()) {
        output.clear();
        return output.c_str();
    }

    std::string command = (*arguments)[0];

    std::transform(
        command.begin(),
        command.end(),
        command.begin(),
        [](unsigned char character) {
            return static_cast<char>(std::toupper(character));
        }
    );

    CommandReply reply = processor().execute(*arguments);
    std::string formatted = formatReply(reply.payload, command == "INFO");

    char kind = 'v';

    if (!reply.payload.empty() && reply.payload[0] == '-') {
        kind = 'e';
    } else if (reply.payload == "$-1\r\n") {
        kind = 'n';
    }

    output = kind + formatted;
    return output.c_str();
}

// Runs one background expiration cycle, as the server's cron does.
int flashkv_tick() {
    return static_cast<int>(database().activeExpireCycle());
}

/*
 * Snapshot of the keyspace (most recently used first) and stats,
 * used by the demo's live inspector. Reading it does not affect
 * LRU order or hit counters.
 */
const char* flashkv_state_json() {
    Database& db = database();
    std::string& json = outputBuffer();

    const DatabaseStats& stats = db.stats();

    json = "{\"usedMemory\":" + std::to_string(db.usedMemory()) +
           ",\"maxMemory\":" + std::to_string(db.maxMemory()) +
           ",\"policy\":\"" + evictionPolicyName(db.evictionPolicy()) +
           "\",\"expiredKeys\":" + std::to_string(stats.expiredKeys) +
           ",\"evictedKeys\":" + std::to_string(stats.evictedKeys) +
           ",\"hits\":" + std::to_string(stats.keyspaceHits) +
           ",\"misses\":" + std::to_string(stats.keyspaceMisses) +
           ",\"commands\":" +
           std::to_string(processor().stats().totalCommands) +
           ",\"keys\":[";

    bool first = true;

    for (const auto& key : db.keysByRecency()) {
        auto info = db.inspect(key);

        if (!info.has_value()) {
            continue;
        }

        if (!first) {
            json += ',';
        }

        first = false;

        json += "{\"key\":";
        appendJsonString(json, key);
        json += ",\"type\":\"";
        json += valueTypeName(info->type);
        json += "\",\"ttlMs\":" + std::to_string(info->ttlMs);
        json += ",\"memory\":" + std::to_string(info->memory) + ",";
        appendPreview(json, *info->value);
        json += '}';
    }

    json += "]}";
    return json.c_str();
}

} // extern "C"
