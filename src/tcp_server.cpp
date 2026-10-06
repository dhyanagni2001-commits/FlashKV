#include "aof.hpp"
#include "commands.hpp"
#include "database.hpp"
#include "resp.hpp"

#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <memory>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <unordered_map>
#include <vector>

namespace {

constexpr int CONNECTION_BACKLOG = 511;
constexpr std::size_t READ_CHUNK_SIZE = 16 * 1024;

// Reads per client per loop iteration, so one busy client
// cannot starve the others.
constexpr int MAX_READS_PER_EVENT = 16;

// Closes clients that send oversized partial commands.
constexpr std::size_t MAX_QUERY_BUFFER = 1024 * 1024 * 1024;

// Stop reading from a client whose replies are not being consumed.
constexpr std::size_t OUTPUT_HIGH_WATERMARK = 4 * 1024 * 1024;

volatile std::sig_atomic_t shutdownRequested = 0;

void handleShutdownSignal(int) {
    shutdownRequested = 1;
}

struct ServerConfig {
    std::string bindAddress = "127.0.0.1";
    int port = 6379;
    bool appendOnly = true;
    std::string aofPath = "flashkv.aof";
    std::size_t maxMemory = 0;
    EvictionPolicy evictionPolicy = EvictionPolicy::NoEviction;
    int hz = 10;
};

struct Client {
    std::string input;
    std::string output;
    std::size_t outputOffset = 0;
    bool closeAfterWrite = false;

    std::size_t pendingOutput() const {
        return output.size() - outputOffset;
    }
};

void printUsage(const char* program) {
    std::cout
        << "Usage: " << program << " [options]\n\n"
        << "Options:\n"
        << "  --bind <address>            Address to listen on "
           "(default 127.0.0.1)\n"
        << "  --port <port>               TCP port (default 6379)\n"
        << "  --appendonly <yes|no>       Enable AOF persistence "
           "(default yes)\n"
        << "  --aof <path>                AOF file path "
           "(default flashkv.aof)\n"
        << "  --maxmemory <bytes>         Memory limit, e.g. 100mb "
           "(default 0 = unlimited)\n"
        << "  --maxmemory-policy <name>   noeviction | allkeys-lru "
           "(default noeviction)\n"
        << "  --hz <n>                    Background task frequency "
           "(default 10)\n"
        << "  --help                      Show this message\n";
}

bool parseArguments(
    int argc,
    char** argv,
    ServerConfig& config
) {
    for (int i = 1; i < argc; ++i) {
        std::string option = argv[i];

        if (option == "--help" || option == "-h") {
            printUsage(argv[0]);
            std::exit(0);
        }

        if (i + 1 >= argc) {
            std::cerr << "Missing value for " << option << '\n';
            return false;
        }

        std::string value = argv[++i];

        try {
            if (option == "--bind") {
                config.bindAddress = value;
            } else if (option == "--port") {
                config.port = std::stoi(value);

                if (config.port <= 0 || config.port > 65535) {
                    throw std::out_of_range("port");
                }
            } else if (option == "--appendonly") {
                if (value != "yes" && value != "no") {
                    throw std::invalid_argument("appendonly");
                }

                config.appendOnly = value == "yes";
            } else if (option == "--aof") {
                config.aofPath = value;
            } else if (option == "--maxmemory") {
                auto bytes = parseMemorySize(value);

                if (!bytes.has_value()) {
                    throw std::invalid_argument("maxmemory");
                }

                config.maxMemory = *bytes;
            } else if (option == "--maxmemory-policy") {
                auto policy = parseEvictionPolicy(value);

                if (!policy.has_value()) {
                    throw std::invalid_argument("maxmemory-policy");
                }

                config.evictionPolicy = *policy;
            } else if (option == "--hz") {
                config.hz = std::stoi(value);

                if (config.hz < 1 || config.hz > 500) {
                    throw std::out_of_range("hz");
                }
            } else {
                std::cerr << "Unknown option: " << option << '\n';
                return false;
            }
        } catch (const std::exception&) {
            std::cerr << "Invalid value for " << option
                      << ": " << value << '\n';
            return false;
        }
    }

    return true;
}

bool setNonBlocking(int socket) {
    int flags = fcntl(socket, F_GETFL, 0);

    return flags != -1 &&
           fcntl(socket, F_SETFL, flags | O_NONBLOCK) != -1;
}

int createListener(const ServerConfig& config) {
    int listener = socket(AF_INET, SOCK_STREAM, 0);

    if (listener == -1) {
        std::cerr << "Failed to create socket: "
                  << std::strerror(errno) << '\n';
        return -1;
    }

    int enabled = 1;

    setsockopt(
        listener,
        SOL_SOCKET,
        SO_REUSEADDR,
        &enabled,
        sizeof(enabled)
    );

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<uint16_t>(config.port));

    if (inet_pton(
            AF_INET,
            config.bindAddress.c_str(),
            &address.sin_addr
        ) != 1) {
        std::cerr << "Invalid bind address: "
                  << config.bindAddress << '\n';
        close(listener);
        return -1;
    }

    if (bind(
            listener,
            reinterpret_cast<sockaddr*>(&address),
            sizeof(address)
        ) == -1) {
        std::cerr << "Failed to bind to "
                  << config.bindAddress << ':' << config.port
                  << ": " << std::strerror(errno) << '\n';
        close(listener);
        return -1;
    }

    if (listen(listener, CONNECTION_BACKLOG) == -1 ||
        !setNonBlocking(listener)) {
        std::cerr << "Failed to listen: "
                  << std::strerror(errno) << '\n';
        close(listener);
        return -1;
    }

    return listener;
}

class Server {
public:
    Server(
        const ServerConfig& config,
        Database& database,
        AppendOnlyFile* aof
    )
        : config(config),
          database(database),
          aof(aof),
          processor(database, aof) {
        processor.stats().port = config.port;
    }

    int run(int listener) {
        using SteadyClock = std::chrono::steady_clock;

        const auto cronInterval =
            std::chrono::milliseconds(1000 / config.hz);

        auto nextCron = SteadyClock::now() + cronInterval;
        auto nextSync = SteadyClock::now() + std::chrono::seconds(1);

        std::vector<pollfd> descriptors;

        while (!shutdownRequested) {
            descriptors.clear();
            descriptors.push_back({listener, POLLIN, 0});

            for (const auto& [socket, client] : clients) {
                short events = 0;

                // Backpressure: pause reading while replies pile up.
                if (client.pendingOutput() < OUTPUT_HIGH_WATERMARK &&
                    !client.closeAfterWrite) {
                    events |= POLLIN;
                }

                if (client.pendingOutput() > 0) {
                    events |= POLLOUT;
                }

                descriptors.push_back({socket, events, 0});
            }

            auto timeout = std::chrono::duration_cast<
                std::chrono::milliseconds
            >(nextCron - SteadyClock::now()).count();

            int ready = poll(
                descriptors.data(),
                static_cast<nfds_t>(descriptors.size()),
                static_cast<int>(std::max<long long>(timeout, 0))
            );

            if (ready < 0) {
                if (errno == EINTR) {
                    continue;
                }

                std::cerr << "poll failed: "
                          << std::strerror(errno) << '\n';
                return 1;
            }

            if (descriptors[0].revents & POLLIN) {
                acceptClients(listener);
            }

            for (std::size_t i = 1; i < descriptors.size(); ++i) {
                handleClientEvents(
                    descriptors[i].fd,
                    descriptors[i].revents
                );
            }

            auto now = SteadyClock::now();

            if (now >= nextCron) {
                database.activeExpireCycle();
                nextCron = now + cronInterval;
            }

            if (aof != nullptr && now >= nextSync) {
                aof->sync();
                nextSync = now + std::chrono::seconds(1);
            }
        }

        std::cout << "\nShutting down FlashKV...\n";

        for (const auto& [socket, client] : clients) {
            close(socket);
        }

        clients.clear();
        return 0;
    }

private:
    const ServerConfig& config;
    Database& database;
    AppendOnlyFile* aof;
    CommandProcessor processor;
    std::unordered_map<int, Client> clients;

    void acceptClients(int listener) {
        while (true) {
            int socket = accept(listener, nullptr, nullptr);

            if (socket == -1) {
                if (errno == EINTR) {
                    continue;
                }

                if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    std::cerr << "accept failed: "
                              << std::strerror(errno) << '\n';
                }

                return;
            }

            if (!setNonBlocking(socket)) {
                close(socket);
                continue;
            }

            int enabled = 1;

            // Replies are small; send them without Nagle delays.
            setsockopt(
                socket,
                IPPROTO_TCP,
                TCP_NODELAY,
                &enabled,
                sizeof(enabled)
            );

#ifdef SO_NOSIGPIPE
            setsockopt(
                socket,
                SOL_SOCKET,
                SO_NOSIGPIPE,
                &enabled,
                sizeof(enabled)
            );
#endif

            clients.emplace(socket, Client{});

            ServerStats& stats = processor.stats();
            ++stats.totalConnections;
            stats.connectedClients = clients.size();
        }
    }

    void handleClientEvents(int socket, short events) {
        auto entry = clients.find(socket);

        if (entry == clients.end()) {
            return;
        }

        Client& client = entry->second;
        bool keep = true;

        if (events & (POLLERR | POLLNVAL)) {
            keep = false;
        }

        if (keep && (events & (POLLIN | POLLHUP))) {
            keep = readFromClient(socket, client);
        }

        if (keep && client.pendingOutput() > 0) {
            // Replies may only leave once their writes are in the AOF.
            if (aof != nullptr) {
                aof->flush();
            }

            keep = writeToClient(socket, client);
        }

        if (keep && client.closeAfterWrite &&
            client.pendingOutput() == 0) {
            keep = false;
        }

        if (!keep) {
            close(socket);
            clients.erase(entry);
            processor.stats().connectedClients = clients.size();
        }
    }

    // Returns false when the connection should be closed.
    bool readFromClient(int socket, Client& client) {
        char buffer[READ_CHUNK_SIZE];

        for (int reads = 0; reads < MAX_READS_PER_EVENT; ++reads) {
            ssize_t received = recv(socket, buffer, sizeof(buffer), 0);

            if (received > 0) {
                client.input.append(
                    buffer,
                    static_cast<std::size_t>(received)
                );

                if (static_cast<std::size_t>(received) < sizeof(buffer)) {
                    break;
                }

                continue;
            }

            if (received == 0) {
                // Peer closed; still answer anything already buffered.
                processInput(client);
                client.closeAfterWrite = true;
                return true;
            }

            if (errno == EINTR) {
                continue;
            }

            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }

            return false;
        }

        if (client.input.size() > MAX_QUERY_BUFFER) {
            std::cerr << "Closing client: query buffer limit exceeded\n";
            return false;
        }

        processInput(client);
        return true;
    }

    void processInput(Client& client) {
        std::size_t consumed = 0;

        while (!client.closeAfterWrite &&
               consumed < client.input.size()) {
            std::string_view pending(
                client.input.data() + consumed,
                client.input.size() - consumed
            );

            RespParseResult result = parseRespCommand(pending);

            if (result.status == RespParseStatus::Incomplete) {
                break;
            }

            if (result.status == RespParseStatus::Error) {
                client.output += respError(result.error);
                client.closeAfterWrite = true;
                break;
            }

            consumed += result.consumedBytes;

            // Blank inline lines are ignored, as in Redis.
            if (result.arguments.empty()) {
                continue;
            }

            CommandReply reply = processor.execute(result.arguments);
            client.output += reply.payload;

            if (reply.closeConnection) {
                client.closeAfterWrite = true;
            }
        }

        client.input.erase(0, consumed);
    }

    bool writeToClient(int socket, Client& client) {
        while (client.pendingOutput() > 0) {
            int flags = 0;

#ifdef MSG_NOSIGNAL
            flags |= MSG_NOSIGNAL;
#endif

            ssize_t sent = send(
                socket,
                client.output.data() + client.outputOffset,
                client.pendingOutput(),
                flags
            );

            if (sent > 0) {
                client.outputOffset += static_cast<std::size_t>(sent);
                continue;
            }

            if (sent < 0 && errno == EINTR) {
                continue;
            }

            if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                // Socket buffer full; POLLOUT resumes the write.
                break;
            }

            return false;
        }

        // Compact once the buffer is fully sent or mostly consumed.
        if (client.outputOffset == client.output.size()) {
            client.output.clear();
            client.outputOffset = 0;
        } else if (client.outputOffset > client.output.size() / 2) {
            client.output.erase(0, client.outputOffset);
            client.outputOffset = 0;
        }

        return true;
    }
};

} // namespace

int main(int argc, char** argv) {
    ServerConfig config;

    if (!parseArguments(argc, argv, config)) {
        printUsage(argv[0]);
        return 1;
    }

    std::signal(SIGPIPE, SIG_IGN);
    std::signal(SIGINT, handleShutdownSignal);
    std::signal(SIGTERM, handleShutdownSignal);

    Database database;
    std::unique_ptr<AppendOnlyFile> aof;

    if (config.appendOnly) {
        aof = std::make_unique<AppendOnlyFile>(config.aofPath);

        // Replay without an AOF attached so commands are not re-logged.
        CommandProcessor replayer(database);
        std::string error;

        bool loaded = aof->load(
            [&replayer](
                const std::vector<std::string>& arguments,
                std::string& replayError
            ) {
                CommandReply reply = replayer.execute(arguments);

                if (!reply.payload.empty() && reply.payload[0] == '-') {
                    replayError = reply.payload.substr(
                        1,
                        reply.payload.size() - 3
                    );
                    return false;
                }

                return true;
            },
            error
        );

        if (!loaded || !aof->open(error)) {
            std::cerr << "Failed to load " << config.aofPath
                      << ": " << error << '\n';
            return 1;
        }

        std::cout << "AOF loaded: " << database.size()
                  << " keys from " << config.aofPath << '\n';
    }

    // Applied after replay so loading never evicts.
    database.setMaxMemory(config.maxMemory);
    database.setEvictionPolicy(config.evictionPolicy);

    int listener = createListener(config);

    if (listener == -1) {
        return 1;
    }

    std::cout << "FlashKV " << FLASHKV_VERSION << " listening on "
              << config.bindAddress << ':' << config.port << '\n'
              << std::flush;

    Server server(config, database, aof.get());
    int status = server.run(listener);

    close(listener);
    return status;
}
