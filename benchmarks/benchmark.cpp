/*
 * FlashKV load generator, similar in spirit to redis-benchmark.
 *
 * Opens N concurrent connections, sends commands in pipelined
 * batches and reports throughput plus round-trip latency
 * percentiles for each command type. Works against any
 * RESP-compatible server, including Redis itself.
 */

#include "resp.hpp"

#include <algorithm>
#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <random>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

using SteadyClock = std::chrono::steady_clock;

struct Options {
    std::string host = "127.0.0.1";
    int port = 6379;
    int clients = 50;
    long long requests = 100000;
    int pipeline = 1;
    std::size_t dataSize = 16;
    long long keyspace = 100000;
    std::vector<std::string> tests{"PING", "SET", "GET", "INCR", "LPUSH", "HSET"};
    bool csv = false;
};

struct Result {
    std::string test;
    double seconds = 0;
    long long requests = 0;
    std::vector<double> latenciesMs;
};

void usage() {
    std::cout
        << "Usage: flashkv_benchmark [options]\n"
        << "  -h <host>       Server host (default 127.0.0.1)\n"
        << "  -p <port>       Server port (default 6379)\n"
        << "  -c <clients>    Concurrent connections (default 50)\n"
        << "  -n <requests>   Requests per test (default 100000)\n"
        << "  -P <pipeline>   Commands per round trip (default 1)\n"
        << "  -d <bytes>      SET/LPUSH/HSET value size (default 16)\n"
        << "  -r <keyspace>   Random key range (default 100000)\n"
        << "  -t <tests>      Comma list: PING,SET,GET,INCR,LPUSH,HSET\n"
        << "  --csv           Print results as CSV\n";
}

bool parseOptions(int argc, char** argv, Options& options) {
    for (int i = 1; i < argc; ++i) {
        std::string flag = argv[i];

        if (flag == "--help") {
            usage();
            std::exit(0);
        }

        if (flag == "--csv") {
            options.csv = true;
            continue;
        }

        if (i + 1 >= argc) {
            return false;
        }

        std::string value = argv[++i];

        if (flag == "-h") {
            options.host = value;
        } else if (flag == "-p") {
            options.port = std::stoi(value);
        } else if (flag == "-c") {
            options.clients = std::max(1, std::stoi(value));
        } else if (flag == "-n") {
            options.requests = std::max(1LL, std::stoll(value));
        } else if (flag == "-P") {
            options.pipeline = std::max(1, std::stoi(value));
        } else if (flag == "-d") {
            options.dataSize = static_cast<std::size_t>(std::stoul(value));
        } else if (flag == "-r") {
            options.keyspace = std::max(1LL, std::stoll(value));
        } else if (flag == "-t") {
            options.tests.clear();
            std::stringstream list(value);
            std::string test;

            while (std::getline(list, test, ',')) {
                std::transform(test.begin(), test.end(), test.begin(), ::toupper);
                options.tests.push_back(test);
            }
        } else {
            return false;
        }
    }

    return true;
}

int connectTo(const Options& options) {
    int socket = ::socket(AF_INET, SOCK_STREAM, 0);

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<uint16_t>(options.port));
    inet_pton(AF_INET, options.host.c_str(), &address.sin_addr);

    if (connect(socket, reinterpret_cast<sockaddr*>(&address),
                sizeof(address)) == -1) {
        close(socket);
        return -1;
    }

    int enabled = 1;
    setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled));

    return socket;
}

std::string buildCommand(
    const std::string& test,
    long long key,
    const std::string& value
) {
    std::string name = "key:" + std::to_string(key);

    if (test == "PING") {
        return respArray({"PING"});
    }

    if (test == "SET") {
        return respArray({"SET", name, value});
    }

    if (test == "GET") {
        return respArray({"GET", name});
    }

    if (test == "INCR") {
        return respArray({"INCR", "counter:" + std::to_string(key)});
    }

    if (test == "LPUSH") {
        return respArray({"LPUSH", "list:" + std::to_string(key % 100), value});
    }

    if (test == "HSET") {
        return respArray({"HSET", "hash:" + std::to_string(key % 100),
                          "field:" + std::to_string(key), value});
    }

    return {};
}

/*
 * Returns the length of one complete reply at `position`, or 0 if
 * the buffer does not yet hold a full reply.
 */
std::size_t completeReplyLength(const std::string& buffer, std::size_t position) {
    if (position >= buffer.size()) {
        return 0;
    }

    std::size_t lineEnd = buffer.find("\r\n", position);

    if (lineEnd == std::string::npos) {
        return 0;
    }

    char type = buffer[position];
    std::size_t headerLength = lineEnd + 2 - position;

    if (type == '+' || type == '-' || type == ':') {
        return headerLength;
    }

    long long count = std::stoll(buffer.substr(position + 1, lineEnd - position - 1));

    if (type == '$') {
        if (count < 0) {
            return headerLength;
        }

        std::size_t total = headerLength + static_cast<std::size_t>(count) + 2;
        return position + total <= buffer.size() ? total : 0;
    }

    if (type == '*') {
        std::size_t total = headerLength;

        for (long long i = 0; i < count; ++i) {
            std::size_t element = completeReplyLength(buffer, position + total);

            if (element == 0) {
                return 0;
            }

            total += element;
        }

        return total;
    }

    throw std::runtime_error("unexpected reply type");
}

bool runClient(
    const Options& options,
    const std::string& test,
    std::atomic<long long>& remaining,
    std::vector<double>& latencies,
    unsigned seed
) {
    int socket = connectTo(options);

    if (socket == -1) {
        return false;
    }

    std::mt19937_64 random(seed);
    std::uniform_int_distribution<long long> keys(0, options.keyspace - 1);
    std::string value(options.dataSize, 'x');
    std::string input;
    char chunk[64 * 1024];

    while (true) {
        long long batch = std::min<long long>(
            options.pipeline,
            remaining.fetch_sub(options.pipeline)
        );

        if (batch <= 0) {
            break;
        }

        std::string request;

        for (long long i = 0; i < batch; ++i) {
            request += buildCommand(test, keys(random), value);
        }

        auto started = SteadyClock::now();

        for (std::size_t sent = 0; sent < request.size();) {
            ssize_t written = send(socket, request.data() + sent,
                                   request.size() - sent, 0);

            if (written <= 0) {
                close(socket);
                return false;
            }

            sent += static_cast<std::size_t>(written);
        }

        long long replies = 0;
        std::size_t position = 0;

        while (replies < batch) {
            std::size_t length = completeReplyLength(input, position);

            if (length > 0) {
                if (input[position] == '-') {
                    std::cerr << "Server error: "
                              << input.substr(position, length);
                }

                position += length;
                ++replies;
                continue;
            }

            ssize_t received = recv(socket, chunk, sizeof(chunk), 0);

            if (received <= 0) {
                close(socket);
                return false;
            }

            input.append(chunk, static_cast<std::size_t>(received));
        }

        input.erase(0, position);

        latencies.push_back(
            std::chrono::duration<double, std::milli>(
                SteadyClock::now() - started
            ).count()
        );
    }

    close(socket);
    return true;
}

Result runTest(const Options& options, const std::string& test) {
    std::atomic<long long> remaining{options.requests};
    std::vector<std::vector<double>> latencies(options.clients);
    std::vector<std::thread> threads;
    std::atomic<bool> failed{false};

    auto started = SteadyClock::now();

    for (int i = 0; i < options.clients; ++i) {
        threads.emplace_back([&, i] {
            if (!runClient(options, test, remaining, latencies[i],
                           static_cast<unsigned>(i * 7919 + 17))) {
                failed = true;
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    if (failed) {
        throw std::runtime_error(
            "connection to " + options.host + ":" +
            std::to_string(options.port) + " failed"
        );
    }

    Result result;
    result.test = test;
    result.requests = options.requests;
    result.seconds = std::chrono::duration<double>(
        SteadyClock::now() - started
    ).count();

    for (auto& clientLatencies : latencies) {
        result.latenciesMs.insert(result.latenciesMs.end(),
                                  clientLatencies.begin(),
                                  clientLatencies.end());
    }

    std::sort(result.latenciesMs.begin(), result.latenciesMs.end());
    return result;
}

double percentile(const std::vector<double>& sorted, double fraction) {
    if (sorted.empty()) {
        return 0;
    }

    auto index = static_cast<std::size_t>(fraction * (sorted.size() - 1));
    return sorted[index];
}

} // namespace

int main(int argc, char** argv) {
    Options options;

    if (!parseOptions(argc, argv, options)) {
        usage();
        return 1;
    }

    if (!options.csv) {
        std::printf(
            "FlashKV benchmark: %s:%d, %d clients, %lld requests, "
            "pipeline %d, %zu-byte values\n\n",
            options.host.c_str(), options.port, options.clients,
            options.requests, options.pipeline, options.dataSize
        );
        std::printf("%-8s %14s %10s %10s %10s\n",
                    "TEST", "REQUESTS/SEC", "P50 (ms)", "P99 (ms)", "MAX (ms)");
    } else {
        std::printf("test,rps,p50_ms,p99_ms,max_ms\n");
    }

    for (const auto& test : options.tests) {
        if (buildCommand(test, 0, "").empty()) {
            std::cerr << "Unknown test: " << test << '\n';
            return 1;
        }

        Result result;

        try {
            result = runTest(options, test);
        } catch (const std::exception& error) {
            std::cerr << "Benchmark failed: " << error.what() << '\n';
            return 1;
        }

        double rps = static_cast<double>(result.requests) / result.seconds;
        double p50 = percentile(result.latenciesMs, 0.50);
        double p99 = percentile(result.latenciesMs, 0.99);
        double max = result.latenciesMs.empty() ? 0 : result.latenciesMs.back();

        if (options.csv) {
            std::printf("%s,%.0f,%.3f,%.3f,%.3f\n",
                        test.c_str(), rps, p50, p99, max);
        } else {
            std::printf("%-8s %14.0f %10.3f %10.3f %10.3f\n",
                        test.c_str(), rps, p50, p99, max);
        }
    }

    return 0;
}
