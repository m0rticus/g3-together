// SPDX-License-Identifier: GPL-3.0-or-later
#include "../src/ragdoll_force.h"

#include <cassert>
#include <cstring>
#include <initializer_list>
#include <limits>

using namespace g3t::ragdoll;

bool Near(double a, double b, double tolerance = 0.02)
{
    return std::abs(a - b) <= tolerance;
}

int main(int argc, char** argv)
{
    assert(argc == 2);
    if (!std::strcmp(argv[1], "points")) {
        const Vector host{91193, 5109, -12699};
        const Vector client{90000, 5200, -11000};
        const Vector contact{91203, 5259, -12719};
        const Vector force{-9000, 11000, 4645};
        const Vector relative = RelativePoint(contact, host);
        assert(relative.x == 10 && relative.y == 150 && relative.z == -20);
        const Vector at = WorldPoint(relative, client);
        assert(at.x == 90010 && at.y == 5350 && at.z == -11020);
        Vector limited{};
        assert(LimitForce(force, limited));
        assert(limited.x == force.x && limited.y == force.y && limited.z == force.z);
        const Vector moved = WorldPoint(relative, {-90000, -5200, 11000});
        assert(moved.x == -89990 && moved.y == -5050 && moved.z == 10980);
        assert(limited.x == force.x && limited.y == force.y && limited.z == force.z);
    } else if (!std::strcmp(argv[1], "limit")) {
        Vector output{};
        for (float magnitude : {0.0f, 1200.0f, 5000.0f, 7000.0f, 10000.0f, 15000.0f, 17000.0f, 20000.0f}) {
            const Vector input{magnitude, 0, 0};
            assert(LimitForce(input, output));
            assert(output.x == input.x && output.y == 0 && output.z == 0);
        }
        assert(LimitForce({80000, 16000, 0}, output));
        assert(Near(output.x, 80000) && Near(output.y, 16000));
        assert(LimitForce({300000, 400000, -500000}, output));
        assert(Near(Magnitude(output), MaximumDeathForce));
        assert(Magnitude(output) <= MaximumDeathForce);
        assert(Near(output.x / output.y, 0.75, 0.000001));
        assert(Near(output.z / output.y, -1.25, 0.000001));
        const float maximum = std::numeric_limits<float>::max();
        assert(LimitForce({maximum, -maximum, maximum}, output));
        assert(Finite(output) && Near(Magnitude(output), MaximumDeathForce));
        assert(Magnitude(output) <= MaximumDeathForce);
        for (int x = -4; x <= 4; ++x)
            for (int y = -4; y <= 4; ++y)
                for (int z = -4; z <= 4; ++z) {
                    assert(LimitForce({x * 1.0e8f, y * 1.0e8f, z * 1.0e8f}, output));
                    assert(Magnitude(output) <= MaximumDeathForce);
                }
    } else if (!std::strcmp(argv[1], "invalid")) {
        Vector output{1, 2, 3};
        const float nan = std::numeric_limits<float>::quiet_NaN();
        const float inf = std::numeric_limits<float>::infinity();
        for (Vector input : {Vector{nan, 0, 0}, Vector{0, inf, 0}, Vector{0, 0, -inf}}) {
            assert(!LimitForce(input, output));
            assert(output.x == 1 && output.y == 2 && output.z == 3);
        }
        assert(!Finite({0, nan, 0}));
    } else if (!std::strcmp(argv[1], "once")) {
        DeathImpulse host;
        assert(host.Pending() && !host.Choose(Source::Pending));
        assert(host.Choose(Source::Host));
        for (int i = 0; i < 64; ++i) {
            assert(!host.Choose(Source::Host));
            assert(!host.Choose(Source::Gravity));
        }
        assert(host.HostApplied() && host.applied == 1);
        DeathImpulse gravity;
        assert(gravity.Choose(Source::Gravity));
        assert(!gravity.Choose(Source::Host) && !gravity.Choose(Source::Gravity));
        assert(!gravity.HostApplied() && gravity.applied == 0);
        const DeathImpulse reloaded = host;
        assert(!reloaded.Pending() && reloaded.applied == 1);
        host = {};
        assert(host.Choose(Source::Host) && host.applied == 1);
    } else {
        return 2;
    }
}
