#include "commands.hpp"
#include "resp.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdio>
#include <limits>
#include <system_error>
#include <utility>

namespace {

std::string uppercase(std::string value) {
    std::transform(
        value.begin(),
        value.end(),
        value.begin(),
        [](unsigned char character) {
            return static_cast<char>(std::toupper(character));
        }
    );

    return value;
}

std::string lowercase(std::string value) {
    std::transform(
        value.begin(),
        value.end(),
        value.begin(),
        [](unsigned char character) {
            return static_cast<char>(std::tolower(character));
        }
    );

    return value;
}

bool parseInteger(
    const std::string& text,
    long long& value
) {
    if (text.empty()) {
        return false;
    }

    const char* begin = text.data();
    const char* end = text.data() + text.size();

    auto result = std::from_chars(begin, end, value);

    return result.ec == std::errc{} &&
           result.ptr == end;
}

std::string notAnInteger() {
    return respError("value is not an integer or out of range");
}

std::string syntaxError() {
    return respError("syntax error");
}

std::string nullableBulk(const std::optional<std::string>& value) {
    return value.has_value() ? respBulkString(*value) : respNull();
}

} // namespace

std::optional<std::size_t> parseMemorySize(std::string_view text) {
    std::size_t digits = 0;

    while (digits < text.size() &&
           std::isdigit(static_cast<unsigned char>(text[digits]))) {
        ++digits;
    }

    if (digits == 0) {
        return std::nullopt;
    }

    unsigned long long amount = 0;

    auto result = std::from_chars(
        text.data(),
        text.data() + digits,
        amount
    );

    if (result.ec != std::errc{}) {
        return std::nullopt;
    }

    std::string unit = lowercase(std::string(text.substr(digits)));
    unsigned long long multiplier = 0;

    if (unit.empty() || unit == "b") {
        multiplier = 1;
    } else if (unit == "k") {
        multiplier = 1000;
    } else if (unit == "kb") {
        multiplier = 1024;
    } else if (unit == "m") {
        multiplier = 1000 * 1000;
    } else if (unit == "mb") {
        multiplier = 1024 * 1024;
    } else if (unit == "g") {
        multiplier = 1000 * 1000 * 1000;
    } else if (unit == "gb") {
        multiplier = 1024ULL * 1024 * 1024;
    } else {
        return std::nullopt;
    }

    unsigned long long bytes = 0;

    if (__builtin_mul_overflow(amount, multiplier, &bytes) ||
        bytes > std::numeric_limits<std::size_t>::max()) {
        return std::nullopt;
    }

    return static_cast<std::size_t>(bytes);
}

std::string formatBytesHuman(std::size_t bytes) {
    constexpr const char* UNITS[] = {"B", "K", "M", "G", "T"};

    double value = static_cast<double>(bytes);
    std::size_t unit = 0;

    while (value >= 1024.0 && unit + 1 < std::size(UNITS)) {
        value /= 1024.0;
        ++unit;
    }

    if (unit == 0) {
        return std::to_string(bytes) + "B";
    }

    char formatted[32];
    std::snprintf(formatted, sizeof(formatted), "%.2f%s", value, UNITS[unit]);

    return formatted;
}

CommandProcessor::CommandProcessor(
    Database& database,
    AppendOnlyFile* aof
)
    : database(database),
      aof(aof) {
    serverStats.startTimeMs = database.now();
}

ServerStats& CommandProcessor::stats() {
    return serverStats;
}

const std::unordered_map<std::string, CommandProcessor::CommandSpec>&
CommandProcessor::commandTable() {
    using P = CommandProcessor;

    // name -> {handler, arity, write, denyOom}
    static const std::unordered_map<std::string, CommandSpec> table{
        {"PING", {&P::ping, -1, false, false}},
        {"ECHO", {&P::echo, 2, false, false}},
        {"SELECT", {&P::select, 2, false, false}},
        {"COMMAND", {&P::commandInfo, -1, false, false}},
        {"DBSIZE", {&P::dbsize, 1, false, false}},
        {"FLUSHALL", {&P::flushall, -1, true, false}},
        {"FLUSHDB", {&P::flushall, -1, true, false}},
        {"INFO", {&P::info, -1, false, false}},
        {"CONFIG", {&P::config, -2, false, false}},
        {"MEMORY", {&P::memory, -2, false, false}},

        {"DEL", {&P::del, -2, true, false}},
        {"UNLINK", {&P::del, -2, true, false}},
        {"EXISTS", {&P::exists, -2, false, false}},
        {"TYPE", {&P::type, 2, false, false}},
        {"KEYS", {&P::keys, 2, false, false}},
        {"EXPIRE", {&P::expire, 3, true, false}},
        {"PEXPIRE", {&P::pexpire, 3, true, false}},
        {"EXPIREAT", {&P::expireat, 3, true, false}},
        {"PEXPIREAT", {&P::pexpireat, 3, true, false}},
        {"TTL", {&P::ttl, 2, false, false}},
        {"PTTL", {&P::pttl, 2, false, false}},
        {"PERSIST", {&P::persist, 2, true, false}},

        {"SET", {&P::set, -3, true, true}},
        {"GET", {&P::get, 2, false, false}},
        {"MSET", {&P::mset, -3, true, true}},
        {"MGET", {&P::mget, -2, false, false}},
        {"INCR", {&P::incr, 2, true, true}},
        {"DECR", {&P::decr, 2, true, true}},
        {"INCRBY", {&P::incrby, 3, true, true}},
        {"DECRBY", {&P::decrby, 3, true, true}},
        {"APPEND", {&P::append, 3, true, true}},
        {"STRLEN", {&P::strlen, 2, false, false}},

        {"LPUSH", {&P::lpush, -3, true, true}},
        {"RPUSH", {&P::rpush, -3, true, true}},
        {"LPOP", {&P::lpop, 2, true, false}},
        {"RPOP", {&P::rpop, 2, true, false}},
        {"LRANGE", {&P::lrange, 4, false, false}},
        {"LLEN", {&P::llen, 2, false, false}},
        {"LINDEX", {&P::lindex, 3, false, false}},

        {"HSET", {&P::hset, -4, true, true}},
        {"HGET", {&P::hget, 3, false, false}},
        {"HDEL", {&P::hdel, -3, true, false}},
        {"HGETALL", {&P::hgetall, 2, false, false}},
        {"HLEN", {&P::hlen, 2, false, false}},
        {"HEXISTS", {&P::hexists, 3, false, false}},
        {"HKEYS", {&P::hkeys, 2, false, false}},
        {"HVALS", {&P::hvals, 2, false, false}},

        {"SADD", {&P::sadd, -3, true, true}},
        {"SREM", {&P::srem, -3, true, false}},
        {"SMEMBERS", {&P::smembers, 2, false, false}},
        {"SISMEMBER", {&P::sismember, 3, false, false}},
        {"SCARD", {&P::scard, 2, false, false}},
    };

    return table;
}

CommandReply CommandProcessor::execute(const Arguments& arguments) {
    if (arguments.empty()) {
        return {respError("empty command")};
    }

    ++serverStats.totalCommands;

    std::string name = uppercase(arguments[0]);

    if (name == "QUIT") {
        return {respSimpleString("OK"), true};
    }

    const auto& table = commandTable();
    auto entry = table.find(name);

    if (entry == table.end()) {
        return {respError(
            "unknown command '" + arguments[0] + "'"
        )};
    }

    const CommandSpec& spec = entry->second;
    int count = static_cast<int>(arguments.size());

    if ((spec.arity > 0 && count != spec.arity) ||
        (spec.arity < 0 && count < -spec.arity)) {
        return {respError(
            "wrong number of arguments for '" +
            lowercase(name) + "' command"
        )};
    }

    if (spec.write) {
        if (aof != nullptr && !aof->healthy()) {
            return {respErrorWithCode(
                "MISCONF",
                "Errors writing to the AOF file. "
                "Write commands are disabled."
            )};
        }

        std::vector<std::string> evicted;
        bool withinLimit = database.evictIfNeeded(&evicted);

        // Evictions must reach the AOF or a restart would
        // resurrect the evicted keys.
        for (const auto& key : evicted) {
            propagate({"DEL", key});
        }

        if (!withinLimit && spec.denyOom) {
            return {respErrorWithCode(
                "OOM",
                "command not allowed when used memory > 'maxmemory'."
            )};
        }
    }

    try {
        return {(this->*spec.handler)(arguments)};
    } catch (const WrongTypeError& error) {
        return {respErrorWithCode("WRONGTYPE", error.what())};
    } catch (const std::exception& error) {
        return {respError(error.what())};
    }
}

void CommandProcessor::propagate(const Arguments& arguments) {
    if (aof != nullptr) {
        aof->append(arguments);
    }
}

void CommandProcessor::propagateExpiry(
    const std::string& key,
    long long unixTimeMs
) {
    // Absolute timestamps keep TTLs correct across restarts.
    propagate({"PEXPIREAT", key, std::to_string(unixTimeMs)});
}

// ---------------------------------------------------------------
// Connection and server
// ---------------------------------------------------------------

std::string CommandProcessor::ping(const Arguments& arguments) {
    if (arguments.size() == 1) {
        return respSimpleString("PONG");
    }

    if (arguments.size() == 2) {
        return respBulkString(arguments[1]);
    }

    return respError("wrong number of arguments for 'ping' command");
}

std::string CommandProcessor::echo(const Arguments& arguments) {
    return respBulkString(arguments[1]);
}

std::string CommandProcessor::select(const Arguments& arguments) {
    // FlashKV has a single logical database.
    if (arguments[1] != "0") {
        return respError("DB index is out of range");
    }

    return respSimpleString("OK");
}

std::string CommandProcessor::commandInfo(const Arguments&) {
    // Enough for redis-cli, which queries COMMAND DOCS on startup.
    return respEncodedArray({});
}

std::string CommandProcessor::dbsize(const Arguments&) {
    return respInteger(static_cast<long long>(database.size()));
}

std::string CommandProcessor::flushall(const Arguments&) {
    database.clear();
    propagate({"FLUSHALL"});
    return respSimpleString("OK");
}

std::string CommandProcessor::info(const Arguments& arguments) {
    const DatabaseStats& dbStats = database.stats();
    long long uptimeSeconds =
        (database.now() - serverStats.startTimeMs) / 1000;

    std::vector<std::pair<std::string, std::string>> sections{
        {"server",
         "# Server\r\n"
         "flashkv_version:" + std::string(FLASHKV_VERSION) + "\r\n"
         "tcp_port:" + std::to_string(serverStats.port) + "\r\n"
         "uptime_in_seconds:" + std::to_string(uptimeSeconds) + "\r\n"},
        {"clients",
         "# Clients\r\n"
         "connected_clients:" +
             std::to_string(serverStats.connectedClients) + "\r\n"},
        {"memory",
         "# Memory\r\n"
         "used_memory:" + std::to_string(database.usedMemory()) + "\r\n"
         "used_memory_human:" + formatBytesHuman(database.usedMemory()) + "\r\n"
         "maxmemory:" + std::to_string(database.maxMemory()) + "\r\n"
         "maxmemory_human:" + formatBytesHuman(database.maxMemory()) + "\r\n"
         "maxmemory_policy:" +
             evictionPolicyName(database.evictionPolicy()) + "\r\n"},
        {"persistence",
         "# Persistence\r\n"
         "aof_enabled:" + std::string(aof ? "1" : "0") + "\r\n"
         "aof_last_write_status:" +
             std::string(!aof || aof->healthy() ? "ok" : "err") + "\r\n"},
        {"stats",
         "# Stats\r\n"
         "total_connections_received:" +
             std::to_string(serverStats.totalConnections) + "\r\n"
         "total_commands_processed:" +
             std::to_string(serverStats.totalCommands) + "\r\n"
         "expired_keys:" + std::to_string(dbStats.expiredKeys) + "\r\n"
         "evicted_keys:" + std::to_string(dbStats.evictedKeys) + "\r\n"
         "keyspace_hits:" + std::to_string(dbStats.keyspaceHits) + "\r\n"
         "keyspace_misses:" + std::to_string(dbStats.keyspaceMisses) + "\r\n"},
        {"keyspace",
         "# Keyspace\r\n" +
             (database.size() == 0
                  ? std::string{}
                  : "db0:keys=" + std::to_string(database.size()) +
                        ",expires=" +
                        std::to_string(database.volatileKeyCount()) +
                        "\r\n")},
    };

    std::string requested =
        arguments.size() > 1 ? lowercase(arguments[1]) : "all";

    std::string output;

    for (const auto& [name, body] : sections) {
        if (requested != "all" && requested != "everything" &&
            requested != "default" && requested != name) {
            continue;
        }

        if (!output.empty()) {
            output += "\r\n";
        }

        output += body;
    }

    return respBulkString(output);
}

std::string CommandProcessor::config(const Arguments& arguments) {
    std::string subcommand = uppercase(arguments[1]);

    if (subcommand == "GET" && arguments.size() == 3) {
        std::vector<std::pair<std::string, std::string>> parameters{
            {"maxmemory", std::to_string(database.maxMemory())},
            {"maxmemory-policy",
             evictionPolicyName(database.evictionPolicy())},
            {"appendonly", aof ? "yes" : "no"},
            {"port", std::to_string(serverStats.port)},
            {"save", ""},
        };

        std::vector<std::string> values;
        std::string pattern = lowercase(arguments[2]);

        for (const auto& [name, value] : parameters) {
            if (globMatch(pattern, name)) {
                values.push_back(name);
                values.push_back(value);
            }
        }

        return respArray(values);
    }

    if (subcommand == "SET" && arguments.size() == 4) {
        std::string parameter = lowercase(arguments[2]);

        if (parameter == "maxmemory") {
            auto bytes = parseMemorySize(arguments[3]);

            if (!bytes.has_value()) {
                return respError("argument must be a memory value");
            }

            database.setMaxMemory(*bytes);

            std::vector<std::string> evicted;
            database.evictIfNeeded(&evicted);

            for (const auto& key : evicted) {
                propagate({"DEL", key});
            }

            return respSimpleString("OK");
        }

        if (parameter == "maxmemory-policy") {
            auto policy = parseEvictionPolicy(lowercase(arguments[3]));

            if (!policy.has_value()) {
                return respError(
                    "invalid maxmemory-policy; "
                    "use noeviction or allkeys-lru"
                );
            }

            database.setEvictionPolicy(*policy);
            return respSimpleString("OK");
        }

        return respError(
            "Unsupported CONFIG parameter: " + arguments[2]
        );
    }

    return respError(
        "unknown subcommand or wrong number of arguments for "
        "'config' command"
    );
}

std::string CommandProcessor::memory(const Arguments& arguments) {
    if (uppercase(arguments[1]) != "USAGE" || arguments.size() != 3) {
        return respError(
            "unknown subcommand or wrong number of arguments for "
            "'memory' command"
        );
    }

    auto info = database.inspect(arguments[2]);

    if (!info.has_value()) {
        return respNull();
    }

    return respInteger(static_cast<long long>(info->memory));
}

// ---------------------------------------------------------------
// Keyspace
// ---------------------------------------------------------------

std::string CommandProcessor::del(const Arguments& arguments) {
    long long deleted = 0;

    for (std::size_t i = 1; i < arguments.size(); ++i) {
        if (database.del(arguments[i])) {
            ++deleted;
        }
    }

    if (deleted > 0) {
        propagate(arguments);
    }

    return respInteger(deleted);
}

std::string CommandProcessor::exists(const Arguments& arguments) {
    long long found = 0;

    for (std::size_t i = 1; i < arguments.size(); ++i) {
        if (database.exists(arguments[i])) {
            ++found;
        }
    }

    return respInteger(found);
}

std::string CommandProcessor::type(const Arguments& arguments) {
    auto valueType = database.type(arguments[1]);

    return respSimpleString(
        valueType.has_value() ? valueTypeName(*valueType) : "none"
    );
}

std::string CommandProcessor::keys(const Arguments& arguments) {
    return respArray(database.keys(arguments[1]));
}

std::string CommandProcessor::expireGeneric(
    const Arguments& arguments,
    long long unitMs,
    bool absolute
) {
    long long amount = 0;

    if (!parseInteger(arguments[2], amount)) {
        return notAnInteger();
    }

    long long deadline = 0;

    if (__builtin_mul_overflow(amount, unitMs, &deadline) ||
        (!absolute &&
         __builtin_add_overflow(deadline, database.now(), &deadline))) {
        return respError(
            "invalid expire time in '" +
            lowercase(arguments[0]) + "' command"
        );
    }

    const std::string& key = arguments[1];

    if (!database.expireAtMs(key, deadline)) {
        return respInteger(0);
    }

    // A deadline in the past deletes the key immediately.
    if (database.exists(key)) {
        propagateExpiry(key, deadline);
    } else {
        propagate({"DEL", key});
    }

    return respInteger(1);
}

std::string CommandProcessor::expire(const Arguments& arguments) {
    return expireGeneric(arguments, 1000, false);
}

std::string CommandProcessor::pexpire(const Arguments& arguments) {
    return expireGeneric(arguments, 1, false);
}

std::string CommandProcessor::expireat(const Arguments& arguments) {
    return expireGeneric(arguments, 1000, true);
}

std::string CommandProcessor::pexpireat(const Arguments& arguments) {
    return expireGeneric(arguments, 1, true);
}

std::string CommandProcessor::ttl(const Arguments& arguments) {
    return respInteger(database.ttl(arguments[1]));
}

std::string CommandProcessor::pttl(const Arguments& arguments) {
    return respInteger(database.ttlMs(arguments[1]));
}

std::string CommandProcessor::persist(const Arguments& arguments) {
    if (!database.persist(arguments[1])) {
        return respInteger(0);
    }

    propagate(arguments);
    return respInteger(1);
}

// ---------------------------------------------------------------
// Strings
// ---------------------------------------------------------------

std::string CommandProcessor::set(const Arguments& arguments) {
    const std::string& key = arguments[1];
    const std::string& value = arguments[2];

    bool onlyIfMissing = false;
    bool onlyIfPresent = false;
    std::optional<long long> deadline;

    for (std::size_t i = 3; i < arguments.size(); ++i) {
        std::string option = uppercase(arguments[i]);

        if (option == "NX") {
            onlyIfMissing = true;
        } else if (option == "XX") {
            onlyIfPresent = true;
        } else if ((option == "EX" || option == "PX") &&
                   i + 1 < arguments.size() &&
                   !deadline.has_value()) {
            long long amount = 0;

            if (!parseInteger(arguments[++i], amount)) {
                return notAnInteger();
            }

            long long unitMs = option == "EX" ? 1000 : 1;
            long long when = 0;

            if (amount <= 0 ||
                __builtin_mul_overflow(amount, unitMs, &when) ||
                __builtin_add_overflow(when, database.now(), &when)) {
                return respError("invalid expire time in 'set' command");
            }

            deadline = when;
        } else {
            return syntaxError();
        }
    }

    if (onlyIfMissing && onlyIfPresent) {
        return syntaxError();
    }

    bool present = database.exists(key);

    if ((onlyIfMissing && present) || (onlyIfPresent && !present)) {
        return respNull();
    }

    database.set(key, value);
    propagate({"SET", key, value});

    if (deadline.has_value()) {
        database.expireAtMs(key, *deadline);
        propagateExpiry(key, *deadline);
    }

    return respSimpleString("OK");
}

std::string CommandProcessor::get(const Arguments& arguments) {
    return nullableBulk(database.get(arguments[1]));
}

std::string CommandProcessor::mset(const Arguments& arguments) {
    if (arguments.size() % 2 == 0) {
        return respError("wrong number of arguments for 'mset' command");
    }

    for (std::size_t i = 1; i < arguments.size(); i += 2) {
        database.set(arguments[i], arguments[i + 1]);
    }

    propagate(arguments);
    return respSimpleString("OK");
}

std::string CommandProcessor::mget(const Arguments& arguments) {
    std::vector<std::string> values;

    for (std::size_t i = 1; i < arguments.size(); ++i) {
        try {
            values.push_back(nullableBulk(database.get(arguments[i])));
        } catch (const WrongTypeError&) {
            // Redis returns nil for keys holding other types.
            values.push_back(respNull());
        }
    }

    return respEncodedArray(values);
}

std::string CommandProcessor::incr(const Arguments& arguments) {
    long long result = database.incrementBy(arguments[1], 1);
    propagate(arguments);
    return respInteger(result);
}

std::string CommandProcessor::decr(const Arguments& arguments) {
    long long result = database.incrementBy(arguments[1], -1);
    propagate(arguments);
    return respInteger(result);
}

std::string CommandProcessor::incrby(const Arguments& arguments) {
    long long delta = 0;

    if (!parseInteger(arguments[2], delta)) {
        return notAnInteger();
    }

    long long result = database.incrementBy(arguments[1], delta);
    propagate(arguments);
    return respInteger(result);
}

std::string CommandProcessor::decrby(const Arguments& arguments) {
    long long delta = 0;

    if (!parseInteger(arguments[2], delta)) {
        return notAnInteger();
    }

    if (delta == std::numeric_limits<long long>::min()) {
        return respError("decrement would overflow");
    }

    long long result = database.incrementBy(arguments[1], -delta);
    propagate(arguments);
    return respInteger(result);
}

std::string CommandProcessor::append(const Arguments& arguments) {
    std::size_t length = database.append(arguments[1], arguments[2]);
    propagate(arguments);
    return respInteger(static_cast<long long>(length));
}

std::string CommandProcessor::strlen(const Arguments& arguments) {
    return respInteger(
        static_cast<long long>(database.stringLength(arguments[1]))
    );
}

// ---------------------------------------------------------------
// Lists
// ---------------------------------------------------------------

std::string CommandProcessor::lpush(const Arguments& arguments) {
    std::size_t length = database.listPush(
        arguments[1],
        {arguments.begin() + 2, arguments.end()},
        ListSide::Left
    );

    propagate(arguments);
    return respInteger(static_cast<long long>(length));
}

std::string CommandProcessor::rpush(const Arguments& arguments) {
    std::size_t length = database.listPush(
        arguments[1],
        {arguments.begin() + 2, arguments.end()},
        ListSide::Right
    );

    propagate(arguments);
    return respInteger(static_cast<long long>(length));
}

std::string CommandProcessor::lpop(const Arguments& arguments) {
    auto value = database.listPop(arguments[1], ListSide::Left);

    if (value.has_value()) {
        propagate(arguments);
    }

    return nullableBulk(value);
}

std::string CommandProcessor::rpop(const Arguments& arguments) {
    auto value = database.listPop(arguments[1], ListSide::Right);

    if (value.has_value()) {
        propagate(arguments);
    }

    return nullableBulk(value);
}

std::string CommandProcessor::lrange(const Arguments& arguments) {
    long long start = 0;
    long long stop = 0;

    if (!parseInteger(arguments[2], start) ||
        !parseInteger(arguments[3], stop)) {
        return notAnInteger();
    }

    return respArray(database.listRange(arguments[1], start, stop));
}

std::string CommandProcessor::llen(const Arguments& arguments) {
    return respInteger(
        static_cast<long long>(database.listLength(arguments[1]))
    );
}

std::string CommandProcessor::lindex(const Arguments& arguments) {
    long long index = 0;

    if (!parseInteger(arguments[2], index)) {
        return notAnInteger();
    }

    return nullableBulk(database.listIndex(arguments[1], index));
}

// ---------------------------------------------------------------
// Hashes
// ---------------------------------------------------------------

std::string CommandProcessor::hset(const Arguments& arguments) {
    if (arguments.size() % 2 != 0) {
        return respError("wrong number of arguments for 'hset' command");
    }

    std::vector<std::pair<std::string, std::string>> fields;

    for (std::size_t i = 2; i < arguments.size(); i += 2) {
        fields.emplace_back(arguments[i], arguments[i + 1]);
    }

    std::size_t added = database.hashSet(arguments[1], fields);
    propagate(arguments);

    return respInteger(static_cast<long long>(added));
}

std::string CommandProcessor::hget(const Arguments& arguments) {
    return nullableBulk(database.hashGet(arguments[1], arguments[2]));
}

std::string CommandProcessor::hdel(const Arguments& arguments) {
    std::size_t removed = database.hashDelete(
        arguments[1],
        {arguments.begin() + 2, arguments.end()}
    );

    if (removed > 0) {
        propagate(arguments);
    }

    return respInteger(static_cast<long long>(removed));
}

std::string CommandProcessor::hgetall(const Arguments& arguments) {
    std::vector<std::string> flattened;

    for (auto& [field, value] : database.hashGetAll(arguments[1])) {
        flattened.push_back(std::move(field));
        flattened.push_back(std::move(value));
    }

    return respArray(flattened);
}

std::string CommandProcessor::hlen(const Arguments& arguments) {
    return respInteger(
        static_cast<long long>(database.hashLength(arguments[1]))
    );
}

std::string CommandProcessor::hexists(const Arguments& arguments) {
    return respInteger(
        database.hashExists(arguments[1], arguments[2]) ? 1 : 0
    );
}

std::string CommandProcessor::hkeys(const Arguments& arguments) {
    std::vector<std::string> fields;

    for (auto& [field, value] : database.hashGetAll(arguments[1])) {
        fields.push_back(std::move(field));
    }

    return respArray(fields);
}

std::string CommandProcessor::hvals(const Arguments& arguments) {
    std::vector<std::string> values;

    for (auto& [field, value] : database.hashGetAll(arguments[1])) {
        values.push_back(std::move(value));
    }

    return respArray(values);
}

// ---------------------------------------------------------------
// Sets
// ---------------------------------------------------------------

std::string CommandProcessor::sadd(const Arguments& arguments) {
    std::size_t added = database.setAdd(
        arguments[1],
        {arguments.begin() + 2, arguments.end()}
    );

    if (added > 0) {
        propagate(arguments);
    }

    return respInteger(static_cast<long long>(added));
}

std::string CommandProcessor::srem(const Arguments& arguments) {
    std::size_t removed = database.setRemove(
        arguments[1],
        {arguments.begin() + 2, arguments.end()}
    );

    if (removed > 0) {
        propagate(arguments);
    }

    return respInteger(static_cast<long long>(removed));
}

std::string CommandProcessor::smembers(const Arguments& arguments) {
    return respArray(database.setMembers(arguments[1]));
}

std::string CommandProcessor::sismember(const Arguments& arguments) {
    return respInteger(
        database.setIsMember(arguments[1], arguments[2]) ? 1 : 0
    );
}

std::string CommandProcessor::scard(const Arguments& arguments) {
    return respInteger(
        static_cast<long long>(database.setCardinality(arguments[1]))
    );
}
