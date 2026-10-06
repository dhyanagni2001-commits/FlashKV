#pragma once

#include "aof.hpp"
#include "database.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

inline constexpr const char* FLASHKV_VERSION = "0.2.0";

struct ServerStats {
    long long startTimeMs = 0;
    int port = 0;
    std::size_t connectedClients = 0;
    unsigned long long totalConnections = 0;
    unsigned long long totalCommands = 0;
};

struct CommandReply {
    std::string payload;
    bool closeConnection = false;
};

// Parses sizes such as "1048576", "100kb", "64mb" or "1gb".
std::optional<std::size_t> parseMemorySize(std::string_view text);

std::string formatBytesHuman(std::size_t bytes);

/*
 * Executes commands against a Database. Write commands are
 * propagated to the AOF when one is attached; replaying an AOF
 * uses a processor without one so commands are not re-logged.
 */
class CommandProcessor {
public:
    using Arguments = std::vector<std::string>;

    explicit CommandProcessor(
        Database& database,
        AppendOnlyFile* aof = nullptr
    );

    CommandReply execute(const Arguments& arguments);

    ServerStats& stats();

private:
    using Handler = std::string (CommandProcessor::*)(
        const Arguments& arguments
    );

    struct CommandSpec {
        Handler handler;

        // Exact argument count including the command name, or
        // -N for "at least N".
        int arity;

        bool write;

        // Refused with -OOM when over maxmemory and eviction
        // cannot free enough space.
        bool denyOom;
    };

    Database& database;
    AppendOnlyFile* aof;
    ServerStats serverStats;

    static const std::unordered_map<std::string, CommandSpec>&
    commandTable();

    void propagate(const Arguments& arguments);
    void propagateExpiry(const std::string& key, long long unixTimeMs);

    std::string expireGeneric(
        const Arguments& arguments,
        long long unitMs,
        bool absolute
    );

    std::string ping(const Arguments& arguments);
    std::string echo(const Arguments& arguments);
    std::string select(const Arguments& arguments);
    std::string commandInfo(const Arguments& arguments);
    std::string dbsize(const Arguments& arguments);
    std::string flushall(const Arguments& arguments);
    std::string info(const Arguments& arguments);
    std::string config(const Arguments& arguments);
    std::string memory(const Arguments& arguments);

    std::string del(const Arguments& arguments);
    std::string exists(const Arguments& arguments);
    std::string type(const Arguments& arguments);
    std::string keys(const Arguments& arguments);
    std::string expire(const Arguments& arguments);
    std::string pexpire(const Arguments& arguments);
    std::string expireat(const Arguments& arguments);
    std::string pexpireat(const Arguments& arguments);
    std::string ttl(const Arguments& arguments);
    std::string pttl(const Arguments& arguments);
    std::string persist(const Arguments& arguments);

    std::string set(const Arguments& arguments);
    std::string get(const Arguments& arguments);
    std::string mset(const Arguments& arguments);
    std::string mget(const Arguments& arguments);
    std::string incr(const Arguments& arguments);
    std::string decr(const Arguments& arguments);
    std::string incrby(const Arguments& arguments);
    std::string decrby(const Arguments& arguments);
    std::string append(const Arguments& arguments);
    std::string strlen(const Arguments& arguments);

    std::string lpush(const Arguments& arguments);
    std::string rpush(const Arguments& arguments);
    std::string lpop(const Arguments& arguments);
    std::string rpop(const Arguments& arguments);
    std::string lrange(const Arguments& arguments);
    std::string llen(const Arguments& arguments);
    std::string lindex(const Arguments& arguments);

    std::string hset(const Arguments& arguments);
    std::string hget(const Arguments& arguments);
    std::string hdel(const Arguments& arguments);
    std::string hgetall(const Arguments& arguments);
    std::string hlen(const Arguments& arguments);
    std::string hexists(const Arguments& arguments);
    std::string hkeys(const Arguments& arguments);
    std::string hvals(const Arguments& arguments);

    std::string sadd(const Arguments& arguments);
    std::string srem(const Arguments& arguments);
    std::string smembers(const Arguments& arguments);
    std::string sismember(const Arguments& arguments);
    std::string scard(const Arguments& arguments);
};
