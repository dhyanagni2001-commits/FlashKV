#include "database.hpp"

#include <algorithm>
#include <cctype>
#include <iostream>
#include <sstream>
#include <string>

int main() {
    Database database;
    std::string input;

    std::cout << "FlashKV server started\n";
    std::cout << "Enter SET, GET, DEL, EXISTS, or EXIT\n";

    while (true) {
        std::cout << "flashkv> ";

        if (!std::getline(std::cin, input)) {
            break;
        }

        std::istringstream stream(input);
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

        if (command == "SET") {
            std::string key;
            std::string value;

            stream >> key;
            std::getline(stream >> std::ws, value);

            if (key.empty() || value.empty()) {
                std::cout << "Usage: SET key value\n";
                continue;
            }

            database.set(key, value);
            std::cout << "OK\n";
        } else if (command == "GET") {
            std::string key;
            stream >> key;

            auto value = database.get(key);

            if (value.has_value()) {
                std::cout << value.value() << '\n';
            } else {
                std::cout << "(nil)\n";
            }
        } else if (command == "DEL") {
            std::string key;
            stream >> key;

            std::cout << (database.del(key) ? 1 : 0) << '\n';
        } else if (command == "EXISTS") {
            std::string key;
            stream >> key;

            std::cout << (database.exists(key) ? 1 : 0) << '\n';
        } else if (command == "EXIT") {
            break;
        } else if (!command.empty()) {
            std::cout << "Unknown command\n";
        }
    }

    std::cout << "FlashKV stopped\n";
    return 0;
}