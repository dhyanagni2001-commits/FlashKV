#pragma once

#include <cstddef>
#include <deque>
#include <functional>
#include <list>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

using StringValue = std::string;
using ListValue = std::deque<std::string>;
using HashValue = std::unordered_map<std::string, std::string>;
using SetValue = std::unordered_set<std::string>;
using Value = std::variant<StringValue, ListValue, HashValue, SetValue>;

enum class ValueType {
    String,
    List,
    Hash,
    Set
};

enum class ListSide {
    Left,
    Right
};

enum class EvictionPolicy {
    NoEviction,
    AllKeysLru
};

/*
 * Thrown when a command targets a key holding a different
 * data type, mirroring Redis' WRONGTYPE error.
 */
class WrongTypeError : public std::runtime_error {
public:
    WrongTypeError()
        : std::runtime_error(
              "Operation against a key holding the wrong kind of value"
          ) {
    }
};

class NotIntegerError : public std::runtime_error {
public:
    NotIntegerError()
        : std::runtime_error(
              "value is not an integer or out of range"
          ) {
    }
};

struct DatabaseStats {
    unsigned long long expiredKeys = 0;
    unsigned long long evictedKeys = 0;
    unsigned long long keyspaceHits = 0;
    unsigned long long keyspaceMisses = 0;
};

const char* valueTypeName(ValueType type);

std::optional<EvictionPolicy> parseEvictionPolicy(
    std::string_view name
);

const char* evictionPolicyName(EvictionPolicy policy);

/*
 * Redis-style glob matching supporting *, ?, [abc], [a-z],
 * [^abc] and backslash escapes.
 */
bool globMatch(
    std::string_view pattern,
    std::string_view text
);

/*
 * Read-only view of a key. `value` stays valid only until the
 * database is next modified.
 */
struct KeyInfo {
    ValueType type;
    long long ttlMs;
    std::size_t memory;
    const Value* value;
};

class Database {
public:
    // Returns the current Unix time in milliseconds.
    using ClockFunction = std::function<long long()>;

    static constexpr long long NO_EXPIRY = -1;

    Database();

    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    // Keyspace
    bool del(const std::string& key);
    bool exists(const std::string& key);
    std::optional<ValueType> type(const std::string& key);
    std::vector<std::string> keys(std::string_view pattern);
    std::size_t size() const;
    std::size_t volatileKeyCount() const;
    void clear();

    // Strings
    void set(
        const std::string& key,
        const std::string& value
    );

    std::optional<std::string> get(const std::string& key);

    long long incrementBy(
        const std::string& key,
        long long delta
    );

    std::size_t append(
        const std::string& key,
        const std::string& value
    );

    std::size_t stringLength(const std::string& key);

    // Expiration
    bool expireAtMs(
        const std::string& key,
        long long unixTimeMs
    );

    bool expire(
        const std::string& key,
        long long seconds
    );

    bool persist(const std::string& key);

    // -2 if missing, -1 if no expiry, otherwise milliseconds left.
    long long ttlMs(const std::string& key);

    // Same as ttlMs but rounded to seconds like Redis' TTL.
    long long ttl(const std::string& key);

    /*
     * Samples keys with an expiry and removes expired ones.
     * Repeats while more than 25% of a sample was expired,
     * bounded by a time budget so the event loop stays responsive.
     */
    std::size_t activeExpireCycle(
        std::size_t samplesPerLoop = 20,
        long long budgetMicroseconds = 1000
    );

    // Lists
    std::size_t listPush(
        const std::string& key,
        const std::vector<std::string>& values,
        ListSide side
    );

    std::optional<std::string> listPop(
        const std::string& key,
        ListSide side
    );

    std::vector<std::string> listRange(
        const std::string& key,
        long long start,
        long long stop
    );

    std::optional<std::string> listIndex(
        const std::string& key,
        long long index
    );

    std::size_t listLength(const std::string& key);

    // Hashes
    std::size_t hashSet(
        const std::string& key,
        const std::vector<std::pair<std::string, std::string>>& fields
    );

    std::optional<std::string> hashGet(
        const std::string& key,
        const std::string& field
    );

    std::size_t hashDelete(
        const std::string& key,
        const std::vector<std::string>& fields
    );

    std::vector<std::pair<std::string, std::string>> hashGetAll(
        const std::string& key
    );

    std::size_t hashLength(const std::string& key);

    bool hashExists(
        const std::string& key,
        const std::string& field
    );

    // Sets
    std::size_t setAdd(
        const std::string& key,
        const std::vector<std::string>& members
    );

    std::size_t setRemove(
        const std::string& key,
        const std::vector<std::string>& members
    );

    std::vector<std::string> setMembers(const std::string& key);

    bool setIsMember(
        const std::string& key,
        const std::string& member
    );

    std::size_t setCardinality(const std::string& key);

    // Memory management
    void setMaxMemory(std::size_t bytes);
    std::size_t maxMemory() const;

    void setEvictionPolicy(EvictionPolicy policy);
    EvictionPolicy evictionPolicy() const;

    std::size_t usedMemory() const;

    /*
     * Evicts least recently used keys until memory usage is
     * within maxmemory. Evicted keys are appended to `evicted`.
     * Returns false when the limit is exceeded and the policy
     * does not allow eviction.
     */
    bool evictIfNeeded(std::vector<std::string>* evicted = nullptr);

    // Keys ordered from most to least recently used.
    std::vector<std::string> keysByRecency() const;

    /*
     * Inspects a key without touching its LRU position, hit
     * counters or lazily expiring it. Expired keys are reported
     * as missing.
     */
    std::optional<KeyInfo> inspect(const std::string& key) const;

    const DatabaseStats& stats() const;

    void setClock(ClockFunction clock);
    long long now() const;

private:
    struct Entry;

    using Node = std::pair<const std::string, Entry>;
    using LruList = std::list<Node*>;

    static constexpr std::size_t NOT_VOLATILE =
        static_cast<std::size_t>(-1);

    struct Entry {
        Value value;
        long long expiresAtMs = NO_EXPIRY;
        std::size_t memory = 0;
        LruList::iterator lruPosition;
        std::size_t volatileIndex = NOT_VOLATILE;
    };

    using Map = std::unordered_map<std::string, Entry>;

    Map data;

    // Front is the most recently used key.
    LruList lru;

    // Keys with an expiry, sampled by activeExpireCycle.
    std::vector<Node*> volatileKeys;

    std::size_t memoryUsed = 0;
    std::size_t memoryLimit = 0;
    EvictionPolicy policy = EvictionPolicy::NoEviction;

    ClockFunction clock;
    std::mt19937_64 random;
    DatabaseStats statistics;

    Node* findLive(const std::string& key);

    Node* create(
        const std::string& key,
        Value value
    );

    void erase(Node* node);
    void touch(Node* node);

    void setExpiry(Node* node, long long unixTimeMs);
    void clearExpiry(Node* node);

    void addMemory(Node* node, std::size_t bytes);
    void removeMemory(Node* node, std::size_t bytes);

    template <typename T>
    T& valueAs(Node* node);
};
