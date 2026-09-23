#pragma once

#include "database.hpp"

#include <fstream>
#include <string>
#include <vector>

class AppendOnlyFile {
private:
    std::string path;
    std::ofstream output;

    bool applyCommand(
        Database& database,
        const std::vector<std::string>& arguments
    );

public:
    explicit AppendOnlyFile(const std::string& filePath);

    bool load(Database& database);

    bool append(
        const std::vector<std::string>& arguments
    );
};