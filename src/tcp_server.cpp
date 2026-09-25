#include "aof.hpp"
#include "database.hpp"
#include "resp.hpp"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cctype>
#include <csignal>
#include <iostream>
#include <netinet/in.h>
#include <poll.h>
#include <string>
#include <system_error>
#include <sys/socket.h>
#include <unistd.h>
#include <unordered_map>
#include <vector>

bool parseIntegerArgument(
    const std::string& text,
    long long& value
) {
    const char* begin = text.data();
    const char* end = text.data() + text.size();

    auto result = std::from_chars(
        begin,
        end,
        value
    );

    return result.ec == std::errc{} &&
           result.ptr == end;
}

long long currentUnixSeconds() {
    return std::chrono::duration_cast<
        std::chrono::seconds
    >(
        std::chrono::system_clock::now()
            .time_since_epoch()
    ).count();
}

std::string executeCommand(
    Database& database,
    AppendOnlyFile& aof,
    const std::vector<std::string>& arguments
) {
    if (arguments.empty()) {
        return respError("empty command");
    }

    std::string command = arguments[0];

    std::transform(
        command.begin(),
        command.end(),
        command.begin(),
        [](unsigned char character) {
            return static_cast<char>(
                std::toupper(character)
            );
        }
    );

    if (command == "PING") {
        if (arguments.size() == 1) {
            return respSimpleString("PONG");
        }

        if (arguments.size() == 2) {
            return respBulkString(arguments[1]);
        }

        return respError(
            "wrong number of arguments for 'PING'"
        );
    }

    if (command == "ECHO") {
        if (arguments.size() != 2) {
            return respError(
                "wrong number of arguments for 'ECHO'"
            );
        }

        return respBulkString(arguments[1]);
    }

    if (command == "SET") {
        if (arguments.size() != 3) {
            return respError(
                "wrong number of arguments for 'SET'"
            );
        }

        if (!aof.append(arguments)) {
            return respError(
                "failed to persist SET command"
            );
        }

        database.set(
            arguments[1],
            arguments[2]
        );

        return respSimpleString("OK");
    }

    if (command == "GET") {
        if (arguments.size() != 2) {
            return respError(
                "wrong number of arguments for 'GET'"
            );
        }

        auto value = database.get(arguments[1]);

        if (!value.has_value()) {
            return respNull();
        }

        return respBulkString(value.value());
    }

    if (command == "DEL") {
        if (arguments.size() < 2) {
            return respError(
                "wrong number of arguments for 'DEL'"
            );
        }

        if (!aof.append(arguments)) {
            return respError(
                "failed to persist DEL command"
            );
        }

        long long deletedCount = 0;

        for (
            std::size_t index = 1;
            index < arguments.size();
            ++index
        ) {
            if (database.del(arguments[index])) {
                ++deletedCount;
            }
        }

        return respInteger(deletedCount);
    }

    if (command == "EXISTS") {
        if (arguments.size() < 2) {
            return respError(
                "wrong number of arguments for 'EXISTS'"
            );
        }

        long long existingCount = 0;

        for (
            std::size_t index = 1;
            index < arguments.size();
            ++index
        ) {
            if (database.exists(arguments[index])) {
                ++existingCount;
            }
        }

        return respInteger(existingCount);
    }

    if (command == "EXPIRE") {
        if (arguments.size() != 3) {
            return respError(
                "wrong number of arguments for 'EXPIRE'"
            );
        }

        long long seconds = 0;

        if (
            !parseIntegerArgument(
                arguments[2],
                seconds
            )
        ) {
            return respError(
                "value is not an integer or out of range"
            );
        }

        if (!database.exists(arguments[1])) {
            return respInteger(0);
        }

        if (seconds <= 0) {
            std::vector<std::string> deleteCommand{
                "DEL",
                arguments[1]
            };

            if (!aof.append(deleteCommand)) {
                return respError(
                    "failed to persist expiration"
                );
            }

            database.del(arguments[1]);
            return respInteger(1);
        }

        const long long expirationTime =
            currentUnixSeconds() + seconds;

        std::vector<std::string> expireAtCommand{
            "EXPIREAT",
            arguments[1],
            std::to_string(expirationTime)
        };

        if (!aof.append(expireAtCommand)) {
            return respError(
                "failed to persist EXPIRE command"
            );
        }

        database.expire(
            arguments[1],
            seconds
        );

        return respInteger(1);
    }

    if (command == "TTL") {
        if (arguments.size() != 2) {
            return respError(
                "wrong number of arguments for 'TTL'"
            );
        }

        return respInteger(
            database.ttl(arguments[1])
        );
    }

    return respError(
        "unknown command '" +
        arguments[0] +
        "'"
    );
}

bool sendAll(
    int clientSocket,
    const std::string& response
) {
    std::size_t totalSent = 0;

    while (totalSent < response.size()) {
        ssize_t bytesSent = send(
            clientSocket,
            response.data() + totalSent,
            response.size() - totalSent,
            0
        );

        if (bytesSent < 0 && errno == EINTR) {
            continue;
        }

        if (bytesSent <= 0) {
            return false;
        }

        totalSent +=
            static_cast<std::size_t>(bytesSent);
    }

    return true;
}

bool processClientData(
    int clientSocket,
    std::string& pendingData,
    Database& database,
    AppendOnlyFile& aof
) {
    char buffer[4096];

    ssize_t bytesReceived;

    do {
        bytesReceived = recv(
            clientSocket,
            buffer,
            sizeof(buffer),
            0
        );
    } while (
        bytesReceived < 0 &&
        errno == EINTR
    );

    if (bytesReceived <= 0) {
        return false;
    }

    pendingData.append(
        buffer,
        static_cast<std::size_t>(
            bytesReceived
        )
    );

    while (true) {
        RespParseResult result =
            parseRespCommand(pendingData);

        if (
            result.status ==
            RespParseStatus::Incomplete
        ) {
            break;
        }

        if (
            result.status ==
            RespParseStatus::Error
        ) {
            sendAll(
                clientSocket,
                respError(result.error)
            );

            return false;
        }

        pendingData.erase(
            0,
            result.consumedBytes
        );

        std::string response =
            executeCommand(
                database,
                aof,
                result.arguments
            );

        if (
            !sendAll(
                clientSocket,
                response
            )
        ) {
            return false;
        }
    }

    return true;
}

int main() {
    constexpr int PORT = 6379;
    constexpr int CONNECTION_BACKLOG = 128;

    std::signal(SIGPIPE, SIG_IGN);

    Database database;
    AppendOnlyFile aof("flashkv.aof");

    if (!aof.load(database)) {
        std::cerr
            << "Failed to load flashkv.aof\n";

        return 1;
    }

    std::cout << "AOF data loaded\n";

    int serverSocket = socket(
        AF_INET,
        SOCK_STREAM,
        0
    );

    if (serverSocket == -1) {
        std::cerr
            << "Failed to create socket\n";

        return 1;
    }

    int option = 1;

    if (
        setsockopt(
            serverSocket,
            SOL_SOCKET,
            SO_REUSEADDR,
            &option,
            sizeof(option)
        ) == -1
    ) {
        std::cerr
            << "Failed to configure socket\n";

        close(serverSocket);
        return 1;
    }

    sockaddr_in serverAddress{};

    serverAddress.sin_family = AF_INET;
    serverAddress.sin_port = htons(PORT);
    serverAddress.sin_addr.s_addr =
        htonl(INADDR_LOOPBACK);

    if (
        bind(
            serverSocket,
            reinterpret_cast<sockaddr*>(
                &serverAddress
            ),
            sizeof(serverAddress)
        ) == -1
    ) {
        std::cerr
            << "Failed to bind to port "
            << PORT
            << '\n';

        close(serverSocket);
        return 1;
    }

    if (
        listen(
            serverSocket,
            CONNECTION_BACKLOG
        ) == -1
    ) {
        std::cerr
            << "Failed to listen\n";

        close(serverSocket);
        return 1;
    }

    std::cout
        << "FlashKV listening on 127.0.0.1:"
        << PORT
        << '\n';

    /*
     * Index 0 always contains the listening socket.
     * The remaining entries represent connected clients.
     */
    std::vector<pollfd> sockets;

    sockets.push_back(
        pollfd{
            serverSocket,
            POLLIN,
            0
        }
    );

    /*
     * Each client needs its own input buffer because a RESP
     * command can arrive across multiple TCP packets.
     */
    std::unordered_map<int, std::string>
        clientBuffers;

    while (true) {
        int readyCount = poll(
            sockets.data(),
            static_cast<nfds_t>(sockets.size()),
            -1
        );

        if (readyCount < 0) {
            if (errno == EINTR) {
                continue;
            }

            std::cerr << "poll failed\n";
            break;
        }

        /*
         * The listening socket has a new connection.
         */
        if (sockets[0].revents & POLLIN) {
            sockaddr_in clientAddress{};

            socklen_t clientSize =
                sizeof(clientAddress);

            int clientSocket = accept(
                serverSocket,
                reinterpret_cast<sockaddr*>(
                    &clientAddress
                ),
                &clientSize
            );

            if (clientSocket == -1) {
                std::cerr
                    << "Failed to accept connection\n";
            } else {
                sockets.push_back(
                    pollfd{
                        clientSocket,
                        POLLIN,
                        0
                    }
                );

                clientBuffers.emplace(
                    clientSocket,
                    std::string{}
                );

                std::cout
                    << "Client connected: socket "
                    << clientSocket
                    << '\n';
            }
        }

        /*
         * Check every connected client.
         */
        std::size_t index = 1;

        while (index < sockets.size()) {
            int clientSocket =
                sockets[index].fd;

            short events =
                sockets[index].revents;

            bool keepClient = true;

            if (
                events &
                (POLLERR | POLLNVAL)
            ) {
                keepClient = false;
            }

            if (
                keepClient &&
                (events & POLLIN)
            ) {
                keepClient = processClientData(
                    clientSocket,
                    clientBuffers[clientSocket],
                    database,
                    aof
                );
            }

            if (events & POLLHUP) {
                keepClient = false;
            }

            if (!keepClient) {
                std::cout
                    << "Client disconnected: socket "
                    << clientSocket
                    << '\n';

                close(clientSocket);
                clientBuffers.erase(clientSocket);

                sockets.erase(
                    sockets.begin() +
                    static_cast<std::ptrdiff_t>(
                        index
                    )
                );

                /*
                 * Do not increment index because the next
                 * socket moved into the current position.
                 */
                continue;
            }

            ++index;
        }
    }

    for (
        std::size_t index = 1;
        index < sockets.size();
        ++index
    ) {
        close(sockets[index].fd);
    }

    close(serverSocket);
    return 0;
}