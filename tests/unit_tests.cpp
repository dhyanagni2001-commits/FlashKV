/*
 * Dependency-free unit tests for the storage engine, the RESP
 * parser and the command layer. Run directly or through ctest.
 */

#include "aof.hpp"
#include "commands.hpp"
#include "database.hpp"
#include "reply_format.hpp"
#include "resp.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

struct TestCase {
    const char* name;
    std::function<void()> body;
};

std::vector<TestCase>& registry() {
    static std::vector<TestCase> tests;
    return tests;
}

struct Registrar {
    Registrar(const char* name, std::function<void()> body) {
        registry().push_back({name, std::move(body)});
    }
};

struct Failure {
    std::string message;
};

template <typename A, typename B>
void expectEqual(
    const A& actual,
    const B& expected,
    const char* expression,
    int line
) {
    if (!(actual == expected)) {
        std::ostringstream message;
        message << "line " << line << ": " << expression;
        throw Failure{message.str()};
    }
}

#define TEST(name)                                       \
    void name();                                         \
    Registrar registrar_##name(#name, name);             \
    void name()

#define EXPECT_EQ(actual, expected) \
    expectEqual((actual), (expected), #actual " == " #expected, __LINE__)

#define EXPECT_TRUE(condition) \
    expectEqual(static_cast<bool>(condition), true, #condition, __LINE__)

#define EXPECT_THROWS(statement, ExceptionType)                       \
    do {                                                              \
        bool thrown = false;                                          \
        try {                                                         \
            statement;                                                \
        } catch (const ExceptionType&) {                              \
            thrown = true;                                            \
        }                                                             \
        expectEqual(thrown, true, #statement " throws " #ExceptionType, \
                    __LINE__);                                        \
    } while (false)

// A controllable clock for deterministic expiration tests.
struct FakeClock {
    long long nowMs = 1'700'000'000'000;

    void attach(Database& database) {
        database.setClock([this] { return nowMs; });
    }
};

std::string run(CommandProcessor& processor, const std::string& line) {
    auto arguments = tokenizeCommandLine(line);
    return processor.execute(*arguments).payload;
}

// ---------------------------------------------------------------
// Strings and keyspace
// ---------------------------------------------------------------

TEST(set_get_delete) {
    Database db;

    db.set("name", "Dhyan");
    EXPECT_EQ(db.get("name").value(), "Dhyan");
    EXPECT_TRUE(db.exists("name"));
    EXPECT_TRUE(db.del("name"));
    EXPECT_TRUE(!db.get("name").has_value());
    EXPECT_TRUE(!db.del("name"));
}

TEST(increment_and_overflow) {
    Database db;

    EXPECT_EQ(db.incrementBy("counter", 5), 5);
    EXPECT_EQ(db.incrementBy("counter", -7), -2);

    db.set("text", "abc");
    EXPECT_THROWS(db.incrementBy("text", 1), NotIntegerError);

    db.set("big", "9223372036854775807");
    EXPECT_THROWS(db.incrementBy("big", 1), std::overflow_error);
}

TEST(wrong_type_errors) {
    Database db;

    db.listPush("list", {"a"}, ListSide::Right);
    EXPECT_THROWS(db.get("list"), WrongTypeError);
    EXPECT_THROWS(db.hashSet("list", {{"f", "v"}}), WrongTypeError);

    // SET overwrites any type.
    db.set("list", "now a string");
    EXPECT_EQ(db.type("list").value(), ValueType::String);
}

TEST(glob_matching) {
    EXPECT_TRUE(globMatch("*", "anything"));
    EXPECT_TRUE(globMatch("user:*", "user:42"));
    EXPECT_TRUE(!globMatch("user:*", "session:42"));
    EXPECT_TRUE(globMatch("h?llo", "hello"));
    EXPECT_TRUE(globMatch("h[ae]llo", "hallo"));
    EXPECT_TRUE(!globMatch("h[^e]llo", "hello"));
    EXPECT_TRUE(globMatch("h[a-c]llo", "hbllo"));
    EXPECT_TRUE(globMatch("a\\*b", "a*b"));
    EXPECT_TRUE(!globMatch("a\\*b", "axb"));
    EXPECT_TRUE(globMatch("*a*b*c*", "xxaxxbxxcxx"));
}

TEST(keys_pattern_is_sorted) {
    Database db;

    db.set("user:2", "b");
    db.set("user:1", "a");
    db.set("order:1", "x");

    auto matches = db.keys("user:*");
    EXPECT_EQ(matches.size(), 2u);
    EXPECT_EQ(matches[0], "user:1");
    EXPECT_EQ(matches[1], "user:2");
}

// ---------------------------------------------------------------
// Expiration
// ---------------------------------------------------------------

TEST(lazy_expiration) {
    Database db;
    FakeClock clock;
    clock.attach(db);

    db.set("session", "abc");
    EXPECT_TRUE(db.expire("session", 10));
    EXPECT_EQ(db.ttl("session"), 10);

    clock.nowMs += 9'000;
    EXPECT_EQ(db.ttl("session"), 1);
    EXPECT_TRUE(db.exists("session"));

    clock.nowMs += 1'000;
    EXPECT_TRUE(!db.exists("session"));
    EXPECT_EQ(db.ttl("session"), -2);
    EXPECT_EQ(db.stats().expiredKeys, 1u);
}

TEST(persist_and_overwrite_clear_ttl) {
    Database db;
    FakeClock clock;
    clock.attach(db);

    db.set("a", "1");
    db.expire("a", 5);
    EXPECT_TRUE(db.persist("a"));
    EXPECT_EQ(db.ttl("a"), -1);

    db.expire("a", 5);
    db.set("a", "2");
    EXPECT_EQ(db.ttl("a"), -1);
    EXPECT_EQ(db.volatileKeyCount(), 0u);
}

TEST(expire_in_past_deletes) {
    Database db;

    db.set("a", "1");
    EXPECT_TRUE(db.expire("a", 0));
    EXPECT_TRUE(!db.exists("a"));
}

TEST(active_expiration_reclaims_without_access) {
    Database db;
    FakeClock clock;
    clock.attach(db);

    for (int i = 0; i < 1000; ++i) {
        std::string key = "temp:" + std::to_string(i);
        db.set(key, "value");
        db.expire(key, 1);
    }

    for (int i = 0; i < 100; ++i) {
        db.set("keep:" + std::to_string(i), "value");
    }

    EXPECT_EQ(db.size(), 1100u);

    clock.nowMs += 2'000;

    // Large budget so the cycle can finish in one call.
    std::size_t removed = db.activeExpireCycle(20, 1'000'000);

    EXPECT_EQ(removed, 1000u);
    EXPECT_EQ(db.size(), 100u);
    EXPECT_EQ(db.volatileKeyCount(), 0u);
}

// ---------------------------------------------------------------
// Lists, hashes, sets
// ---------------------------------------------------------------

TEST(list_operations) {
    Database db;

    EXPECT_EQ(db.listPush("q", {"b", "c"}, ListSide::Right), 2u);
    EXPECT_EQ(db.listPush("q", {"a"}, ListSide::Left), 3u);

    auto all = db.listRange("q", 0, -1);
    EXPECT_EQ(all.size(), 3u);
    EXPECT_EQ(all[0], "a");
    EXPECT_EQ(all[2], "c");

    EXPECT_EQ(db.listRange("q", -2, -1).front(), "b");
    EXPECT_TRUE(db.listRange("q", 5, 10).empty());
    EXPECT_EQ(db.listIndex("q", -1).value(), "c");

    EXPECT_EQ(db.listPop("q", ListSide::Left).value(), "a");
    EXPECT_EQ(db.listPop("q", ListSide::Right).value(), "c");
    EXPECT_EQ(db.listPop("q", ListSide::Right).value(), "b");

    // Empty lists are removed.
    EXPECT_TRUE(!db.exists("q"));
}

TEST(hash_operations) {
    Database db;

    EXPECT_EQ(db.hashSet("user", {{"name", "Ada"}, {"age", "36"}}), 2u);
    EXPECT_EQ(db.hashSet("user", {{"age", "37"}}), 0u);
    EXPECT_EQ(db.hashGet("user", "age").value(), "37");
    EXPECT_EQ(db.hashLength("user"), 2u);
    EXPECT_TRUE(db.hashExists("user", "name"));

    auto all = db.hashGetAll("user");
    EXPECT_EQ(all[0].first, "age");

    EXPECT_EQ(db.hashDelete("user", {"name", "age", "missing"}), 2u);
    EXPECT_TRUE(!db.exists("user"));
}

TEST(set_operations) {
    Database db;

    EXPECT_EQ(db.setAdd("tags", {"c", "a", "b", "a"}), 3u);
    EXPECT_EQ(db.setCardinality("tags"), 3u);
    EXPECT_TRUE(db.setIsMember("tags", "b"));
    EXPECT_EQ(db.setMembers("tags")[0], "a");
    EXPECT_EQ(db.setRemove("tags", {"a", "b", "c"}), 3u);
    EXPECT_TRUE(!db.exists("tags"));
}

// ---------------------------------------------------------------
// Memory accounting and eviction
// ---------------------------------------------------------------

TEST(memory_accounting_returns_to_zero) {
    Database db;

    db.set("s", "value");
    db.listPush("l", {"a", "b", "c"}, ListSide::Right);
    db.hashSet("h", {{"f", "v"}});
    db.setAdd("z", {"m"});
    db.append("s", "more");
    db.incrementBy("n", 100);

    EXPECT_TRUE(db.usedMemory() > 0);

    db.listPop("l", ListSide::Left);
    db.hashSet("h", {{"f", "longer value"}});

    for (const auto& key : db.keys("*")) {
        db.del(key);
    }

    EXPECT_EQ(db.usedMemory(), 0u);
}

TEST(lru_eviction_removes_least_recently_used) {
    Database db;

    db.set("a", std::string(100, 'x'));
    db.set("b", std::string(100, 'x'));
    db.set("c", std::string(100, 'x'));

    // Reading "a" makes "b" the least recently used key.
    db.get("a");

    db.setEvictionPolicy(EvictionPolicy::AllKeysLru);
    db.setMaxMemory(db.usedMemory() - 1);

    std::vector<std::string> evicted;
    EXPECT_TRUE(db.evictIfNeeded(&evicted));

    EXPECT_EQ(evicted.size(), 1u);
    EXPECT_EQ(evicted[0], "b");
    EXPECT_TRUE(db.exists("a"));
    EXPECT_TRUE(db.exists("c"));
    EXPECT_EQ(db.stats().evictedKeys, 1u);
}

TEST(inspect_does_not_change_lru_order) {
    Database db;

    db.set("old", "1");
    db.set("new", "2");

    auto info = db.inspect("old");
    EXPECT_TRUE(info.has_value());
    EXPECT_EQ(info->type, ValueType::String);
    EXPECT_EQ(std::get<StringValue>(*info->value), "1");

    // "old" must still be the least recently used key.
    EXPECT_EQ(db.keysByRecency().back(), "old");
    EXPECT_EQ(db.stats().keyspaceHits, 0u);
}

TEST(noeviction_refuses_writes_over_limit) {
    Database db;
    CommandProcessor processor(db);

    db.setMaxMemory(300);

    for (int i = 0; i < 10; ++i) {
        run(processor, "SET key" + std::to_string(i) + " value");
    }

    EXPECT_EQ(run(processor, "SET another value").substr(0, 4), "-OOM");

    // Reads and deletes are still allowed.
    EXPECT_EQ(run(processor, "GET key0"), "$5\r\nvalue\r\n");
    EXPECT_EQ(run(processor, "DEL key0"), ":1\r\n");
}

// ---------------------------------------------------------------
// RESP protocol
// ---------------------------------------------------------------

TEST(resp_parse_complete_and_partial) {
    std::string request = "*2\r\n$3\r\nGET\r\n$4\r\nname\r\n";

    auto complete = parseRespCommand(request);
    EXPECT_EQ(complete.status, RespParseStatus::Complete);
    EXPECT_EQ(complete.consumedBytes, request.size());
    EXPECT_EQ(complete.arguments[1], "name");

    for (std::size_t cut = 1; cut < request.size(); ++cut) {
        auto partial = parseRespCommand(request.substr(0, cut));
        EXPECT_EQ(partial.status, RespParseStatus::Incomplete);
    }
}

TEST(resp_binary_safe_values) {
    std::string value("a\r\nb\0c", 6);
    std::string request = respArray({"SET", "k", value});

    auto result = parseRespCommand(request);
    EXPECT_EQ(result.status, RespParseStatus::Complete);
    EXPECT_EQ(result.arguments[2], value);
}

TEST(resp_rejects_malformed_input) {
    EXPECT_EQ(parseRespCommand("*1\r\n+PING\r\n").status,
              RespParseStatus::Error);
    EXPECT_EQ(parseRespCommand("*x\r\n").status,
              RespParseStatus::Error);
    EXPECT_EQ(parseRespCommand("*1\r\n$4\r\nPINGxx").status,
              RespParseStatus::Error);
}

TEST(inline_commands) {
    auto result = parseRespCommand("SET greeting \"hello world\"\r\nGET");
    EXPECT_EQ(result.status, RespParseStatus::Complete);
    EXPECT_EQ(result.arguments.size(), 3u);
    EXPECT_EQ(result.arguments[2], "hello world");

    auto blank = parseRespCommand("\r\n");
    EXPECT_EQ(blank.status, RespParseStatus::Complete);
    EXPECT_TRUE(blank.arguments.empty());

    EXPECT_EQ(parseRespCommand("PING").status,
              RespParseStatus::Incomplete);
}

TEST(tokenizer_quotes_and_escapes) {
    auto tokens = tokenizeCommandLine(
        "SET 'single quoted' \"tab\\there\" \"\\x41\""
    );

    EXPECT_TRUE(tokens.has_value());
    EXPECT_EQ((*tokens)[1], "single quoted");
    EXPECT_EQ((*tokens)[2], "tab\there");
    EXPECT_EQ((*tokens)[3], "A");

    EXPECT_TRUE(!tokenizeCommandLine("SET \"unterminated").has_value());
}

TEST(reply_formatting_matches_redis_cli) {
    EXPECT_EQ(formatReply("+OK\r\n"), "OK");
    EXPECT_EQ(formatReply(":42\r\n"), "(integer) 42");
    EXPECT_EQ(formatReply("$-1\r\n"), "(nil)");
    EXPECT_EQ(formatReply("$5\r\nhello\r\n"), "\"hello\"");
    EXPECT_EQ(formatReply("-ERR bad\r\n"), "(error) ERR bad");
    EXPECT_EQ(formatReply("*0\r\n"), "(empty array)");
    EXPECT_EQ(
        formatReply("*2\r\n$1\r\na\r\n$-1\r\n"),
        "1) \"a\"\n2) (nil)"
    );
}

// ---------------------------------------------------------------
// Command layer
// ---------------------------------------------------------------

TEST(command_errors) {
    Database db;
    CommandProcessor processor(db);

    EXPECT_EQ(run(processor, "NOPE"), "-ERR unknown command 'NOPE'\r\n");
    EXPECT_EQ(
        run(processor, "GET"),
        "-ERR wrong number of arguments for 'get' command\r\n"
    );

    run(processor, "LPUSH list a");
    EXPECT_EQ(run(processor, "GET list").substr(0, 10), "-WRONGTYPE");
}

TEST(set_options) {
    Database db;
    FakeClock clock;
    clock.attach(db);
    CommandProcessor processor(db);

    EXPECT_EQ(run(processor, "SET k v NX"), "+OK\r\n");
    EXPECT_EQ(run(processor, "SET k v2 NX"), "$-1\r\n");
    EXPECT_EQ(run(processor, "SET missing v XX"), "$-1\r\n");
    EXPECT_EQ(run(processor, "SET k v3 XX EX 10"), "+OK\r\n");
    EXPECT_EQ(run(processor, "TTL k"), ":10\r\n");
    EXPECT_EQ(run(processor, "SET k v PX 1500"), "+OK\r\n");
    EXPECT_EQ(run(processor, "PTTL k"), ":1500\r\n");
    EXPECT_EQ(run(processor, "SET k v EX 0").substr(0, 4), "-ERR");
    EXPECT_EQ(run(processor, "SET k v BOGUS"), "-ERR syntax error\r\n");
}

TEST(mget_and_mset) {
    Database db;
    CommandProcessor processor(db);

    EXPECT_EQ(run(processor, "MSET a 1 b 2"), "+OK\r\n");
    run(processor, "LPUSH list x");

    EXPECT_EQ(
        run(processor, "MGET a missing b list"),
        "*4\r\n$1\r\n1\r\n$-1\r\n$1\r\n2\r\n$-1\r\n"
    );
}

// ---------------------------------------------------------------
// AOF persistence
// ---------------------------------------------------------------

struct TempFile {
    std::filesystem::path path;

    TempFile() {
        path = std::filesystem::temp_directory_path() /
               ("flashkv-test-" + std::to_string(getpid()) + ".aof");
        std::filesystem::remove(path);
    }

    ~TempFile() {
        std::filesystem::remove(path);
    }
};

bool replayInto(
    Database& db,
    const std::string& path,
    std::string& error
) {
    CommandProcessor replayer(db);
    AppendOnlyFile aof(path);

    return aof.load(
        [&replayer](const auto& arguments, std::string& replayError) {
            auto reply = replayer.execute(arguments);
            replayError = reply.payload;
            return reply.payload[0] != '-';
        },
        error
    );
}

TEST(aof_round_trip_all_types) {
    TempFile file;

    {
        Database db;
        AppendOnlyFile aof(file.path.string());
        std::string error;
        EXPECT_TRUE(aof.open(error));

        CommandProcessor processor(db, &aof);

        run(processor, "SET name Dhyan");
        run(processor, "INCRBY visits 41");
        run(processor, "INCR visits");
        run(processor, "RPUSH queue a b c");
        run(processor, "LPOP queue");
        run(processor, "HSET user name Ada lang cpp");
        run(processor, "SADD tags x y");
        run(processor, "SET temp value EX 1000");
        run(processor, "SET gone value");
        run(processor, "DEL gone");
        EXPECT_TRUE(aof.flush());
    }

    Database restored;
    std::string error;
    EXPECT_TRUE(replayInto(restored, file.path.string(), error));

    EXPECT_EQ(restored.get("name").value(), "Dhyan");
    EXPECT_EQ(restored.get("visits").value(), "42");
    EXPECT_EQ(restored.listRange("queue", 0, -1).size(), 2u);
    EXPECT_EQ(restored.hashGet("user", "lang").value(), "cpp");
    EXPECT_EQ(restored.setCardinality("tags"), 2u);
    EXPECT_TRUE(restored.ttl("temp") > 990);
    EXPECT_TRUE(!restored.exists("gone"));
}

TEST(aof_eviction_is_persisted) {
    TempFile file;

    {
        Database db;
        AppendOnlyFile aof(file.path.string());
        std::string error;
        aof.open(error);

        CommandProcessor processor(db, &aof);
        db.setEvictionPolicy(EvictionPolicy::AllKeysLru);
        db.setMaxMemory(400);

        for (int i = 0; i < 20; ++i) {
            run(processor, "SET key" + std::to_string(i) + " value");
        }

        aof.flush();
        EXPECT_TRUE(db.stats().evictedKeys > 0);
    }

    Database restored;
    std::string error;
    EXPECT_TRUE(replayInto(restored, file.path.string(), error));

    // Evicted keys must not come back after a restart.
    EXPECT_TRUE(!restored.exists("key0"));
    EXPECT_TRUE(restored.exists("key19"));
}

TEST(aof_truncated_tail_is_repaired) {
    TempFile file;

    {
        std::ofstream output(file.path, std::ios::binary);
        output << respArray({"SET", "a", "1"});
        output << "*3\r\n$3\r\nSET\r\n$1\r\nb";
    }

    Database restored;
    std::string error;
    EXPECT_TRUE(replayInto(restored, file.path.string(), error));
    EXPECT_EQ(restored.get("a").value(), "1");
    EXPECT_TRUE(!restored.exists("b"));

    EXPECT_EQ(
        std::filesystem::file_size(file.path),
        respArray({"SET", "a", "1"}).size()
    );
}

TEST(memory_size_parsing) {
    EXPECT_EQ(parseMemorySize("100").value(), 100u);
    EXPECT_EQ(parseMemorySize("1kb").value(), 1024u);
    EXPECT_EQ(parseMemorySize("2MB").value(), 2u * 1024 * 1024);
    EXPECT_EQ(parseMemorySize("1g").value(), 1000u * 1000 * 1000);
    EXPECT_TRUE(!parseMemorySize("ten").has_value());
    EXPECT_TRUE(!parseMemorySize("10tb").has_value());
}

} // namespace

int main() {
    int failures = 0;

    for (const auto& test : registry()) {
        try {
            test.body();
            std::cout << "PASS  " << test.name << '\n';
        } catch (const Failure& failure) {
            ++failures;
            std::cout << "FAIL  " << test.name << " ("
                      << failure.message << ")\n";
        } catch (const std::exception& error) {
            ++failures;
            std::cout << "FAIL  " << test.name
                      << " (unexpected exception: " << error.what()
                      << ")\n";
        }
    }

    std::cout << '\n'
              << registry().size() - failures << '/'
              << registry().size() << " tests passed\n";

    return failures == 0 ? 0 : 1;
}
