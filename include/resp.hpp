#pragma once

#include <cstddef>
#include <optional>
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

/*
 * Parses one command from the front of the buffer. Accepts RESP
 * arrays of bulk strings and, like Redis, plain-text inline
 * commands terminated by a newline (useful with nc or telnet).
 * An empty inline line completes with no arguments.
 */
RespParseResult parseRespCommand(std::string_view buffer);

/*
 * Splits a command line into arguments. Supports "double quoted"
 * strings with escapes (\n, \r, \t, \", \\, \xHH) and 'single
 * quoted' strings. Returns std::nullopt on unbalanced quotes.
 */
std::optional<std::vector<std::string>> tokenizeCommandLine(
    std::string_view line
);

std::string respSimpleString(const std::string& value);

// Produces "-ERR <message>".
std::string respError(const std::string& message);

// Produces "-<code> <message>", e.g. WRONGTYPE or OOM.
std::string respErrorWithCode(
    const std::string& code,
    const std::string& message
);

std::string respInteger(long long value);
std::string respBulkString(const std::string& value);
std::string respNull();

// Array of bulk strings.
std::string respArray(
    const std::vector<std::string>& arguments
);

// Array whose elements are already RESP-encoded.
std::string respEncodedArray(
    const std::vector<std::string>& encodedElements
);
