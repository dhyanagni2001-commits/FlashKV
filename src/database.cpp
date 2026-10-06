#include "database.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <limits>
#include <system_error>

namespace {

/*
 * Memory usage is an estimate: payload bytes plus a fixed
 * overhead for hash-table nodes, LRU links and containers.
 */
constexpr std::size_t ENTRY_OVERHEAD = 96;
constexpr std::size_t ELEMENT_OVERHEAD = 32;

long long systemTimeMs() {
    return std::chrono::duration_cast<
        std::chrono::milliseconds
    >(
        std::chrono::system_clock::now()
            .time_since_epoch()
    ).count();
}

std::size_t elementCost(const std::string& value) {
    return value.size() + ELEMENT_OVERHEAD;
}

std::size_t payloadMemory(const Value& value) {
    if (auto* text = std::get_if<StringValue>(&value)) {
        return text->size();
    }

    std::size_t total = 0;

    if (auto* list = std::get_if<ListValue>(&value)) {
        for (const auto& element : *list) {
            total += elementCost(element);
        }
    } else if (auto* hash = std::get_if<HashValue>(&value)) {
        for (const auto& [field, fieldValue] : *hash) {
            total += elementCost(field) + fieldValue.size();
        }
    } else if (auto* set = std::get_if<SetValue>(&value)) {
        for (const auto& member : *set) {
            total += elementCost(member);
        }
    }

    return total;
}

bool parseStrictInteger(
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

bool matchCharacterClass(
    std::string_view pattern,
    std::size_t& position,
    char character
) {
    // pattern[position] is the opening '['.
    ++position;

    bool negate = false;

    if (position < pattern.size() &&
        pattern[position] == '^') {
        negate = true;
        ++position;
    }

    bool matched = false;

    while (position < pattern.size() &&
           pattern[position] != ']') {
        char low = pattern[position];

        if (low == '\\' && position + 1 < pattern.size()) {
            low = pattern[++position];
        }

        char high = low;

        if (position + 2 < pattern.size() &&
            pattern[position + 1] == '-' &&
            pattern[position + 2] != ']') {
            high = pattern[position + 2];
            position += 2;
        }

        if (low > high) {
            std::swap(low, high);
        }

        if (character >= low && character <= high) {
            matched = true;
        }

        ++position;
    }

    if (position < pattern.size()) {
        ++position;
    }

    return matched != negate;
}

} // namespace

const char* valueTypeName(ValueType type) {
    switch (type) {
    case ValueType::String:
        return "string";
    case ValueType::List:
        return "list";
    case ValueType::Hash:
        return "hash";
    case ValueType::Set:
        return "set";
    }

    return "none";
}

std::optional<EvictionPolicy> parseEvictionPolicy(
    std::string_view name
) {
    if (name == "noeviction") {
        return EvictionPolicy::NoEviction;
    }

    if (name == "allkeys-lru") {
        return EvictionPolicy::AllKeysLru;
    }

    return std::nullopt;
}

const char* evictionPolicyName(EvictionPolicy policy) {
    return policy == EvictionPolicy::AllKeysLru
               ? "allkeys-lru"
               : "noeviction";
}

bool globMatch(
    std::string_view pattern,
    std::string_view text
) {
    constexpr std::size_t NONE =
        std::string_view::npos;

    std::size_t patternPos = 0;
    std::size_t textPos = 0;
    std::size_t starPattern = NONE;
    std::size_t starText = 0;

    while (textPos < text.size()) {
        if (patternPos < pattern.size()) {
            char token = pattern[patternPos];

            if (token == '*') {
                starPattern = ++patternPos;
                starText = textPos;
                continue;
            }

            if (token == '?') {
                ++patternPos;
                ++textPos;
                continue;
            }

            if (token == '[') {
                std::size_t next = patternPos;

                if (matchCharacterClass(
                        pattern,
                        next,
                        text[textPos]
                    )) {
                    patternPos = next;
                    ++textPos;
                    continue;
                }
            } else {
                if (token == '\\' &&
                    patternPos + 1 < pattern.size()) {
                    token = pattern[patternPos + 1];
                    ++patternPos;
                }

                if (token == text[textPos]) {
                    ++patternPos;
                    ++textPos;
                    continue;
                }
            }
        }

        // Backtrack: let the last '*' absorb one more character.
        if (starPattern != NONE) {
            patternPos = starPattern;
            textPos = ++starText;
            continue;
        }

        return false;
    }

    while (patternPos < pattern.size() &&
           pattern[patternPos] == '*') {
        ++patternPos;
    }

    return patternPos == pattern.size();
}

Database::Database()
    : clock(systemTimeMs),
      random(std::random_device{}()) {
}

// ---------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------

Database::Node* Database::findLive(const std::string& key) {
    auto entry = data.find(key);

    if (entry == data.end()) {
        return nullptr;
    }

    Node* node = &*entry;

    // Lazy expiration: expired keys are removed on access.
    if (node->second.expiresAtMs != NO_EXPIRY &&
        now() >= node->second.expiresAtMs) {
        erase(node);
        ++statistics.expiredKeys;
        return nullptr;
    }

    return node;
}

Database::Node* Database::create(
    const std::string& key,
    Value value
) {
    std::size_t memory =
        ENTRY_OVERHEAD + key.size() + payloadMemory(value);

    auto [entry, inserted] = data.try_emplace(key);
    (void)inserted;

    Node* node = &*entry;

    node->second.value = std::move(value);
    node->second.memory = memory;

    lru.push_front(node);
    node->second.lruPosition = lru.begin();

    memoryUsed += memory;

    return node;
}

void Database::erase(Node* node) {
    clearExpiry(node);
    lru.erase(node->second.lruPosition);
    memoryUsed -= node->second.memory;

    // Erase by iterator: erasing by node->first would pass a
    // reference to the key that is being destroyed.
    data.erase(data.find(node->first));
}

void Database::touch(Node* node) {
    lru.splice(
        lru.begin(),
        lru,
        node->second.lruPosition
    );
}

void Database::setExpiry(
    Node* node,
    long long unixTimeMs
) {
    if (node->second.volatileIndex == NOT_VOLATILE) {
        node->second.volatileIndex = volatileKeys.size();
        volatileKeys.push_back(node);
    }

    node->second.expiresAtMs = unixTimeMs;
}

void Database::clearExpiry(Node* node) {
    std::size_t index = node->second.volatileIndex;

    if (index != NOT_VOLATILE) {
        // Swap-and-pop keeps removal O(1).
        Node* last = volatileKeys.back();
        volatileKeys[index] = last;
        last->second.volatileIndex = index;
        volatileKeys.pop_back();

        node->second.volatileIndex = NOT_VOLATILE;
    }

    node->second.expiresAtMs = NO_EXPIRY;
}

void Database::addMemory(Node* node, std::size_t bytes) {
    node->second.memory += bytes;
    memoryUsed += bytes;
}

void Database::removeMemory(Node* node, std::size_t bytes) {
    node->second.memory -= bytes;
    memoryUsed -= bytes;
}

template <typename T>
T& Database::valueAs(Node* node) {
    T* value = std::get_if<T>(&node->second.value);

    if (value == nullptr) {
        throw WrongTypeError();
    }

    return *value;
}

// ---------------------------------------------------------------
// Keyspace
// ---------------------------------------------------------------

bool Database::del(const std::string& key) {
    Node* node = findLive(key);

    if (node == nullptr) {
        return false;
    }

    erase(node);
    return true;
}

bool Database::exists(const std::string& key) {
    return findLive(key) != nullptr;
}

std::optional<ValueType> Database::type(
    const std::string& key
) {
    Node* node = findLive(key);

    if (node == nullptr) {
        return std::nullopt;
    }

    return static_cast<ValueType>(
        node->second.value.index()
    );
}

std::vector<std::string> Database::keys(
    std::string_view pattern
) {
    long long current = now();
    std::vector<std::string> matches;

    for (const auto& [key, entry] : data) {
        bool expired =
            entry.expiresAtMs != NO_EXPIRY &&
            current >= entry.expiresAtMs;

        if (!expired && globMatch(pattern, key)) {
            matches.push_back(key);
        }
    }

    std::sort(matches.begin(), matches.end());
    return matches;
}

std::size_t Database::size() const {
    return data.size();
}

std::size_t Database::volatileKeyCount() const {
    return volatileKeys.size();
}

void Database::clear() {
    data.clear();
    lru.clear();
    volatileKeys.clear();
    memoryUsed = 0;
}

// ---------------------------------------------------------------
// Strings
// ---------------------------------------------------------------

void Database::set(
    const std::string& key,
    const std::string& value
) {
    Node* node = findLive(key);

    if (node != nullptr) {
        erase(node);
    }

    create(key, StringValue{value});
}

std::optional<std::string> Database::get(
    const std::string& key
) {
    Node* node = findLive(key);

    if (node == nullptr) {
        ++statistics.keyspaceMisses;
        return std::nullopt;
    }

    const auto& value = valueAs<StringValue>(node);

    ++statistics.keyspaceHits;
    touch(node);

    return value;
}

long long Database::incrementBy(
    const std::string& key,
    long long delta
) {
    Node* node = findLive(key);

    if (node == nullptr) {
        node = create(key, StringValue{"0"});
    }

    auto& text = valueAs<StringValue>(node);

    long long current = 0;

    if (!parseStrictInteger(text, current)) {
        throw NotIntegerError();
    }

    long long result = 0;

    if (__builtin_add_overflow(current, delta, &result)) {
        throw std::overflow_error(
            "increment or decrement would overflow"
        );
    }

    std::string updated = std::to_string(result);

    removeMemory(node, text.size());
    text = std::move(updated);
    addMemory(node, text.size());

    touch(node);
    return result;
}

std::size_t Database::append(
    const std::string& key,
    const std::string& value
) {
    Node* node = findLive(key);

    if (node == nullptr) {
        create(key, StringValue{value});
        return value.size();
    }

    auto& text = valueAs<StringValue>(node);

    text += value;
    addMemory(node, value.size());
    touch(node);

    return text.size();
}

std::size_t Database::stringLength(const std::string& key) {
    Node* node = findLive(key);

    if (node == nullptr) {
        return 0;
    }

    return valueAs<StringValue>(node).size();
}

// ---------------------------------------------------------------
// Expiration
// ---------------------------------------------------------------

bool Database::expireAtMs(
    const std::string& key,
    long long unixTimeMs
) {
    Node* node = findLive(key);

    if (node == nullptr) {
        return false;
    }

    if (unixTimeMs <= now()) {
        erase(node);
        return true;
    }

    setExpiry(node, unixTimeMs);
    return true;
}

bool Database::expire(
    const std::string& key,
    long long seconds
) {
    if (seconds <= 0) {
        return expireAtMs(key, now());
    }

    long long milliseconds = 0;
    long long deadline = 0;

    if (__builtin_mul_overflow(seconds, 1000LL, &milliseconds) ||
        __builtin_add_overflow(now(), milliseconds, &deadline)) {
        deadline = std::numeric_limits<long long>::max();
    }

    return expireAtMs(key, deadline);
}

bool Database::persist(const std::string& key) {
    Node* node = findLive(key);

    if (node == nullptr ||
        node->second.expiresAtMs == NO_EXPIRY) {
        return false;
    }

    clearExpiry(node);
    return true;
}

long long Database::ttlMs(const std::string& key) {
    Node* node = findLive(key);

    if (node == nullptr) {
        return -2;
    }

    if (node->second.expiresAtMs == NO_EXPIRY) {
        return -1;
    }

    return node->second.expiresAtMs - now();
}

long long Database::ttl(const std::string& key) {
    long long remaining = ttlMs(key);

    if (remaining < 0) {
        return remaining;
    }

    return (remaining + 500) / 1000;
}

std::size_t Database::activeExpireCycle(
    std::size_t samplesPerLoop,
    long long budgetMicroseconds
) {
    using SteadyClock = std::chrono::steady_clock;

    auto started = SteadyClock::now();
    auto budget =
        std::chrono::microseconds(budgetMicroseconds);

    long long current = now();
    std::size_t removed = 0;

    while (!volatileKeys.empty()) {
        std::size_t sampled = 0;
        std::size_t expired = 0;

        std::size_t samples =
            std::min(samplesPerLoop, volatileKeys.size());

        for (std::size_t i = 0;
             i < samples && !volatileKeys.empty();
             ++i) {
            std::uniform_int_distribution<std::size_t>
                pick(0, volatileKeys.size() - 1);

            Node* node = volatileKeys[pick(random)];
            ++sampled;

            if (current >= node->second.expiresAtMs) {
                erase(node);
                ++expired;
            }
        }

        removed += expired;
        statistics.expiredKeys += expired;

        // Stop once at most 25% of the sample was expired.
        if (expired * 4 <= sampled) {
            break;
        }

        if (SteadyClock::now() - started > budget) {
            break;
        }
    }

    return removed;
}

// ---------------------------------------------------------------
// Lists
// ---------------------------------------------------------------

std::size_t Database::listPush(
    const std::string& key,
    const std::vector<std::string>& values,
    ListSide side
) {
    Node* node = findLive(key);

    if (node == nullptr) {
        node = create(key, ListValue{});
    }

    auto& list = valueAs<ListValue>(node);

    for (const auto& value : values) {
        if (side == ListSide::Left) {
            list.push_front(value);
        } else {
            list.push_back(value);
        }

        addMemory(node, elementCost(value));
    }

    touch(node);
    return list.size();
}

std::optional<std::string> Database::listPop(
    const std::string& key,
    ListSide side
) {
    Node* node = findLive(key);

    if (node == nullptr) {
        return std::nullopt;
    }

    auto& list = valueAs<ListValue>(node);

    std::string value;

    if (side == ListSide::Left) {
        value = std::move(list.front());
        list.pop_front();
    } else {
        value = std::move(list.back());
        list.pop_back();
    }

    removeMemory(node, elementCost(value));

    // Like Redis, empty aggregates are removed.
    if (list.empty()) {
        erase(node);
    } else {
        touch(node);
    }

    return value;
}

std::vector<std::string> Database::listRange(
    const std::string& key,
    long long start,
    long long stop
) {
    Node* node = findLive(key);

    if (node == nullptr) {
        return {};
    }

    const auto& list = valueAs<ListValue>(node);
    long long length = static_cast<long long>(list.size());

    if (start < 0) {
        start += length;
    }

    if (stop < 0) {
        stop += length;
    }

    start = std::max(start, 0LL);
    stop = std::min(stop, length - 1);

    if (start > stop || start >= length) {
        return {};
    }

    touch(node);

    return {
        list.begin() + start,
        list.begin() + stop + 1
    };
}

std::optional<std::string> Database::listIndex(
    const std::string& key,
    long long index
) {
    Node* node = findLive(key);

    if (node == nullptr) {
        return std::nullopt;
    }

    const auto& list = valueAs<ListValue>(node);
    long long length = static_cast<long long>(list.size());

    if (index < 0) {
        index += length;
    }

    if (index < 0 || index >= length) {
        return std::nullopt;
    }

    touch(node);
    return list[static_cast<std::size_t>(index)];
}

std::size_t Database::listLength(const std::string& key) {
    Node* node = findLive(key);

    if (node == nullptr) {
        return 0;
    }

    return valueAs<ListValue>(node).size();
}

// ---------------------------------------------------------------
// Hashes
// ---------------------------------------------------------------

std::size_t Database::hashSet(
    const std::string& key,
    const std::vector<std::pair<std::string, std::string>>& fields
) {
    Node* node = findLive(key);

    if (node == nullptr) {
        node = create(key, HashValue{});
    }

    auto& hash = valueAs<HashValue>(node);
    std::size_t added = 0;

    for (const auto& [field, value] : fields) {
        auto [entry, inserted] = hash.try_emplace(field);

        if (inserted) {
            ++added;
            addMemory(node, elementCost(field));
        } else {
            removeMemory(node, entry->second.size());
        }

        entry->second = value;
        addMemory(node, value.size());
    }

    touch(node);
    return added;
}

std::optional<std::string> Database::hashGet(
    const std::string& key,
    const std::string& field
) {
    Node* node = findLive(key);

    if (node == nullptr) {
        return std::nullopt;
    }

    const auto& hash = valueAs<HashValue>(node);
    auto entry = hash.find(field);

    if (entry == hash.end()) {
        return std::nullopt;
    }

    touch(node);
    return entry->second;
}

std::size_t Database::hashDelete(
    const std::string& key,
    const std::vector<std::string>& fields
) {
    Node* node = findLive(key);

    if (node == nullptr) {
        return 0;
    }

    auto& hash = valueAs<HashValue>(node);
    std::size_t removed = 0;

    for (const auto& field : fields) {
        auto entry = hash.find(field);

        if (entry == hash.end()) {
            continue;
        }

        removeMemory(
            node,
            elementCost(entry->first) + entry->second.size()
        );

        hash.erase(entry);
        ++removed;
    }

    if (hash.empty()) {
        erase(node);
    }

    return removed;
}

std::vector<std::pair<std::string, std::string>>
Database::hashGetAll(const std::string& key) {
    Node* node = findLive(key);

    if (node == nullptr) {
        return {};
    }

    const auto& hash = valueAs<HashValue>(node);

    std::vector<std::pair<std::string, std::string>> fields(
        hash.begin(),
        hash.end()
    );

    std::sort(fields.begin(), fields.end());
    touch(node);

    return fields;
}

std::size_t Database::hashLength(const std::string& key) {
    Node* node = findLive(key);

    if (node == nullptr) {
        return 0;
    }

    return valueAs<HashValue>(node).size();
}

bool Database::hashExists(
    const std::string& key,
    const std::string& field
) {
    Node* node = findLive(key);

    if (node == nullptr) {
        return false;
    }

    return valueAs<HashValue>(node).contains(field);
}

// ---------------------------------------------------------------
// Sets
// ---------------------------------------------------------------

std::size_t Database::setAdd(
    const std::string& key,
    const std::vector<std::string>& members
) {
    Node* node = findLive(key);

    if (node == nullptr) {
        node = create(key, SetValue{});
    }

    auto& set = valueAs<SetValue>(node);
    std::size_t added = 0;

    for (const auto& member : members) {
        if (set.insert(member).second) {
            ++added;
            addMemory(node, elementCost(member));
        }
    }

    touch(node);
    return added;
}

std::size_t Database::setRemove(
    const std::string& key,
    const std::vector<std::string>& members
) {
    Node* node = findLive(key);

    if (node == nullptr) {
        return 0;
    }

    auto& set = valueAs<SetValue>(node);
    std::size_t removed = 0;

    for (const auto& member : members) {
        if (set.erase(member) > 0) {
            ++removed;
            removeMemory(node, elementCost(member));
        }
    }

    if (set.empty()) {
        erase(node);
    }

    return removed;
}

std::vector<std::string> Database::setMembers(
    const std::string& key
) {
    Node* node = findLive(key);

    if (node == nullptr) {
        return {};
    }

    const auto& set = valueAs<SetValue>(node);
    std::vector<std::string> members(set.begin(), set.end());

    std::sort(members.begin(), members.end());
    touch(node);

    return members;
}

bool Database::setIsMember(
    const std::string& key,
    const std::string& member
) {
    Node* node = findLive(key);

    if (node == nullptr) {
        return false;
    }

    return valueAs<SetValue>(node).contains(member);
}

std::size_t Database::setCardinality(const std::string& key) {
    Node* node = findLive(key);

    if (node == nullptr) {
        return 0;
    }

    return valueAs<SetValue>(node).size();
}

// ---------------------------------------------------------------
// Memory management
// ---------------------------------------------------------------

void Database::setMaxMemory(std::size_t bytes) {
    memoryLimit = bytes;
}

std::size_t Database::maxMemory() const {
    return memoryLimit;
}

void Database::setEvictionPolicy(EvictionPolicy newPolicy) {
    policy = newPolicy;
}

EvictionPolicy Database::evictionPolicy() const {
    return policy;
}

std::size_t Database::usedMemory() const {
    return memoryUsed;
}

bool Database::evictIfNeeded(
    std::vector<std::string>* evicted
) {
    if (memoryLimit == 0) {
        return true;
    }

    while (memoryUsed > memoryLimit) {
        if (policy == EvictionPolicy::NoEviction ||
            lru.empty()) {
            return false;
        }

        Node* victim = lru.back();

        if (evicted != nullptr) {
            evicted->push_back(victim->first);
        }

        erase(victim);
        ++statistics.evictedKeys;
    }

    return true;
}

std::vector<std::string> Database::keysByRecency() const {
    std::vector<std::string> ordered;
    ordered.reserve(lru.size());

    for (const Node* node : lru) {
        ordered.push_back(node->first);
    }

    return ordered;
}

std::optional<KeyInfo> Database::inspect(
    const std::string& key
) const {
    auto entry = data.find(key);

    if (entry == data.end()) {
        return std::nullopt;
    }

    const Entry& found = entry->second;
    long long remaining = -1;

    if (found.expiresAtMs != NO_EXPIRY) {
        remaining = found.expiresAtMs - now();

        if (remaining <= 0) {
            return std::nullopt;
        }
    }

    return KeyInfo{
        static_cast<ValueType>(found.value.index()),
        remaining,
        found.memory,
        &found.value
    };
}

const DatabaseStats& Database::stats() const {
    return statistics;
}

void Database::setClock(ClockFunction newClock) {
    clock = std::move(newClock);
}

long long Database::now() const {
    return clock();
}
