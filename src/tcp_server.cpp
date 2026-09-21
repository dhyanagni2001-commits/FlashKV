#include "database.hpp"
#include "resp.hpp"

#include <algorithm>
#include <cctype>
#include <csignal>
#include <iostream>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

std::string executeCommand(
    Database& database,
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

        database.set(arguments[1], arguments[2]);
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

        long long deletedCount = 0;

        for (std::size_t index = 1;
             index < arguments.size();
             ++index) {
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

        for (std::size_t index = 1;
             index < arguments.size();
             ++index) {
            if (database.exists(arguments[index])) {
                ++existingCount;
            }
        }

        return respInteger(existingCount);
    }

    return respError(
        "unknown command '" + arguments[0] + "'"
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

        totalSent += static_cast<std::size_t>(bytesSent);
    }

    return true;
}

void handleClient(
    int clientSocket,
    Database& database
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
            static_cast<std::size_t>(bytesReceived)
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
                    result.arguments
                );

            if (!sendAll(clientSocket, response)) {
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

    // Prevent the process from terminating if a client
    // disconnects while a response is being sent.
    std::signal(SIGPIPE, SIG_IGN);

    Database database;

    int serverSocket = socket(
        AF_INET,
        SOCK_STREAM,
        0
    );

    if (serverSocket == -1) {
        std::cerr << "Failed to create socket\n";
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
        std::cerr << "Failed to configure socket\n";
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

        handleClient(clientSocket, database);

        std::cout << "Client disconnected\n";
    }

    close(serverSocket);
    return 0;
}