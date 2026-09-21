#include "database.hpp"

#include <algorithm>
#include <cctype>
#include <iostream>
#include <netinet/in.h>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

std::string executeCommand(
    Database& database,
    std::string request
) {
    // Remove newline characters sent by the client.
    while (!request.empty() &&
           (request.back() == '\n' || request.back() == '\r')) {
        request.pop_back();
    }

    std::istringstream stream(request);
    std::string command;

    stream >> command;

    std::transform(
        command.begin(),
        command.end(),
        command.begin(),
        [](unsigned char character) {
            return static_cast<char>(std::toupper(character));
        }
    );

    if (command == "PING") {
        return "PONG\n";
    }

    if (command == "SET") {
        std::string key;
        std::string value;

        stream >> key;
        std::getline(stream >> std::ws, value);

        if (key.empty() || value.empty()) {
            return "ERROR: Usage: SET key value\n";
        }

        database.set(key, value);
        return "OK\n";
    }

    if (command == "GET") {
        std::string key;
        stream >> key;

        if (key.empty()) {
            return "ERROR: Usage: GET key\n";
        }

        auto value = database.get(key);

        if (!value.has_value()) {
            return "(nil)\n";
        }

        return value.value() + "\n";
    }

    if (command == "DEL") {
        std::string key;
        stream >> key;

        if (key.empty()) {
            return "ERROR: Usage: DEL key\n";
        }

        return database.del(key) ? "1\n" : "0\n";
    }

    if (command == "EXISTS") {
        std::string key;
        stream >> key;

        if (key.empty()) {
            return "ERROR: Usage: EXISTS key\n";
        }

        return database.exists(key) ? "1\n" : "0\n";
    }

    return "ERROR: Unknown command\n";
}

int main() {
    constexpr int PORT = 6379;

    Database database;

    int serverSocket = socket(AF_INET, SOCK_STREAM, 0);

    if (serverSocket == -1) {
        std::cerr << "Failed to create socket\n";
        return 1;
    }

    int option = 1;

    setsockopt(
        serverSocket,
        SOL_SOCKET,
        SO_REUSEADDR,
        &option,
        sizeof(option)
    );

    sockaddr_in serverAddress{};
    serverAddress.sin_family = AF_INET;
    serverAddress.sin_port = htons(PORT);
    serverAddress.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (bind(
            serverSocket,
            reinterpret_cast<sockaddr*>(&serverAddress),
            sizeof(serverAddress)
        ) == -1) {
        std::cerr << "Failed to bind to port " << PORT << '\n';
        close(serverSocket);
        return 1;
    }

    if (listen(serverSocket, 10) == -1) {
        std::cerr << "Failed to listen\n";
        close(serverSocket);
        return 1;
    }

    std::cout << "FlashKV listening on 127.0.0.1:" << PORT << '\n';

    while (true) {
        sockaddr_in clientAddress{};
        socklen_t clientSize = sizeof(clientAddress);

        int clientSocket = accept(
            serverSocket,
            reinterpret_cast<sockaddr*>(&clientAddress),
            &clientSize
        );

        if (clientSocket == -1) {
            std::cerr << "Failed to accept connection\n";
            continue;
        }

        char buffer[4096]{};

        ssize_t bytesReceived = recv(
            clientSocket,
            buffer,
            sizeof(buffer),
            0
        );

        if (bytesReceived > 0) {
            std::string request(buffer, bytesReceived);
            std::string response =
                executeCommand(database, request);

            send(
                clientSocket,
                response.data(),
                response.size(),
                0
            );
        }

        close(clientSocket);
    }
}