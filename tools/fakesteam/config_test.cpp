// SPDX-License-Identifier: GPL-3.0-or-later
#include "config.h"
#include "log.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace g3t {
void Log(char const* text) { std::puts(text); }
void Logf(char const* format, ...)
{
    va_list args;
    va_start(args, format);
    std::vprintf(format, args);
    std::puts("");
    va_end(args);
}
}

int main(int argc, char** argv)
{
    if (argc != 2) return 2;
    auto const& config = g3t::LoadConfig();
    const bool expectedValid = std::strcmp(argv[1], "!invalid") != 0;
    const bool pass = config.sessionIdValid == expectedValid &&
        (!expectedValid || !std::strcmp(config.sessionId, argv[1]));
    std::printf("RESULT: %s session_id=%s valid=%d\n", pass ? "PASS" : "FAIL",
                config.sessionId, config.sessionIdValid);
    return pass ? 0 : 1;
}
