#pragma once

#include <string>
#include <string_view>

/*
 * Renders a RESP reply the way redis-cli does in a terminal:
 *   +OK          -> OK
 *   :5           -> (integer) 5
 *   $-1          -> (nil)
 *   $5 hello     -> "hello"
 *   -ERR oops    -> (error) ERR oops
 *   *2 ...       -> 1) "a"\n2) "b"
 *
 * With `raw` set, bulk strings are printed verbatim (used for INFO).
 */
std::string formatReply(std::string_view reply, bool raw = false);
