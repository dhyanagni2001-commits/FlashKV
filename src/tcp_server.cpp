#include "aof.hpp"
#include "database.hpp"
#include "resp.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cctype>
#include <csignal>
#include <iostream>
#include <netinet/in.h>
#include <string>
#include <system_error>
#include <sys/socket.h>
#include <unistd.h>
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

    /*
     * PING
     * PING message
     */
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

    /*
     * ECHO message
     */
    if (command == "ECHO") {
        if (arguments.size() != 2) {
            return respError(
                "wrong number of arguments for 'ECHO'"
            );
        }

        return respBulkString(arguments[1]);
    }

    /*
     * SET key value
     */
    if (command == "SET") {
        if (arguments.size() != 3) {
            return respError(
                "wrong number of arguments for 'SET'"
            );
        }

        /*
         * Persist the command before changing memory.
         */
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

    /*
     * GET key
     */
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

    /*
     * DEL key [key ...]
     */
    if (command == "DEL") {
        if (arguments.size() < 2) {
            return respError(
                "wrong number of arguments for 'DEL'"
            );
        }

        /*
         * Save the complete DEL command before applying it.
         */
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

    /*
     * EXISTS key [key ...]
     */
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

    /*
     * EXPIRE key seconds
     */
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

        /*
         * Redis returns 0 when the key does not exist.
         */
        if (!database.exists(arguments[1])) {
            return respInteger(0);
        }

        /*
         * An expiration of zero or less deletes the key
         * immediately.
         */
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

        /*
         * Save an absolute expiration time in the AOF.
         *
         * If we saved only "EXPIRE key 60", restarting the
         * server would incorrectly reset the TTL to 60.
         */
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

    /*
     * TTL key
     *
     * Returns:
     *  -2 when the key does not exist
     *  -1 when the key has no expiration
     *   0 or greater for remaining seconds
     */
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

        if (bytesSent <= 0) {
            return false;
        }

        totalSent +=
            static_cast<std::size_t>(bytesSent);
    }

    return true;
}

void handleClient(
    int clientSocket,
    Database& database,
    AppendOnlyFile& aof
) {
    std::string pendingData;
    char buffer[4096];

    while (true) {
        ssize_t bytesReceived = recv(
            clientSocket,
            buffer,
            sizeof(buffer),
            0
        );

        if (bytesReceived <= 0) {
            break;
        }

        pendingData.append(
            buffer,
            static_cast<std::size_t>(
                bytesReceived
            )
        );

        /*
         * Process every complete RESP command currently
         * available in the connection buffer.
         */
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

                close(clientSocket);
                return;
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
                close(clientSocket);
                return;
            }
        }
    }

    close(clientSocket);
}

int main() {
    constexpr int PORT = 6379;
    constexpr int CONNECTION_BACKLOG = 10;

    /*
     * Prevent the process from terminating when a client
     * disconnects while a response is being sent.
     */
    std::signal(SIGPIPE, SIG_IGN);

    Database database;

    /*
     * Commands that modify data are stored here.
     */
    AppendOnlyFile aof("flashkv.aof");

    /*
     * Rebuild the in-memory database from the AOF.
     */
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
            << "Failed to listen for connections\n";

        close(serverSocket);
        return 1;
    }

    std::cout
        << "FlashKV listening on 127.0.0.1:"
        << PORT
        << '\n';

    while (true) {
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

            continue;
        }

        std::cout << "Client connected\n";

        handleClient(
            clientSocket,
            database,
            aof
        );

        std::cout << "Client disconnected\n";
    }

    close(serverSocket);
    return 0;
}