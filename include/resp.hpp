#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

enum class RespParseStatus {
    Complete,
    Incomplete,
    Error
};

struct RespParseResult {
    RespParseStatus status;
    std::vector<std::string> arguments;
    std::size_t consumedBytes = 0;
    std::string error;
};

RespParseResult parseRespCommand(std::string_view buffer);

std::string respSimpleString(const std::string& value);
std::string respError(const std::string& message);
std::string respInteger(long long value);
std::string respBulkString(const std::string& value);
std::string respNull();