/*
 * Interactive local shell: runs the full FlashKV command set
 * against an in-memory database without a network server.
 */

#include "commands.hpp"
#include "database.hpp"
#include "reply_format.hpp"
#include "resp.hpp"

#include <algorithm>
#include <cctype>
#include <iostream>
#include <string>

int main() {
    Database database;
    CommandProcessor processor(database);

    std::cout << "FlashKV " << FLASHKV_VERSION << " local shell\n";
    std::cout << "Type Redis commands (SET, GET, LPUSH, HSET, ...). "
                 "EXIT or QUIT to leave.\n";

    std::string input;

    while (true) {
        std::cout << "flashkv> " << std::flush;

        if (!std::getline(std::cin, input)) {
            break;
        }

        auto arguments = tokenizeCommandLine(input);

        if (!arguments.has_value()) {
            std::cout << "Invalid argument(s): unbalanced quotes\n";
            continue;
        }

        if (arguments->empty()) {
            continue;
        }

        std::string command = (*arguments)[0];

        std::transform(
            command.begin(),
            command.end(),
            command.begin(),
            [](unsigned char character) {
                return static_cast<char>(std::toupper(character));
            }
        );

        if (command == "EXIT" || command == "QUIT") {
            break;
        }

        // Expired keys are also reclaimed in the background.
        database.activeExpireCycle();

        CommandReply reply = processor.execute(*arguments);
        std::cout << formatReply(reply.payload, command == "INFO") << '\n';
    }

    std::cout << "FlashKV stopped\n";
    return 0;
}
