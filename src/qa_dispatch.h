// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstring>

namespace g3t::qa {
using QaLogFn = void (*)(char const*);

struct QaTypedRoute {
    char const* name;
    void (*withoutArgument)(QaLogFn);
    void (*withArgument)(char const*, QaLogFn);
};

template <std::size_t count>
bool DispatchTypedRoute(char const* action, char const* argument, QaLogFn log,
                        QaTypedRoute const (&routes)[count])
{
    for (const auto& route : routes) {
        if (std::strcmp(action, route.name)) continue;
        if (route.withArgument) route.withArgument(argument, log);
        else if (route.withoutArgument) route.withoutArgument(log);
        return true;
    }
    return false;
}
}
