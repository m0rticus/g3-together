// SPDX-License-Identifier: GPL-3.0-or-later
// Restored engine-free checks for DamagePerception, processing spheres, replication life/cadence, and GUID codec.
#include "../src/ai.h"
#include "../src/processing.h"
#include "../src/replication.h"
#include "../src/guid_text.h"

#include <cassert>
#include <cstdarg>
#include <cstdio>
#include <cstring>

class eCEntity {};

namespace g3t {
int FormatV(char* out, size_t size, const char* format, va_list args)
{
    if (!out || !size) return 0;
    const int n = std::vsnprintf(out, size, format, args);
    out[size - 1] = 0;
    return n < 0 ? 0 : (static_cast<size_t>(n) >= size ? static_cast<int>(size - 1) : n);
}
int Format(char* out, size_t size, const char* format, ...)
{
    va_list args;
    va_start(args, format);
    const int n = FormatV(out, size, format, args);
    va_end(args);
    return n;
}
} // namespace g3t

namespace {
void TestDamageContext()
{
    using namespace g3t::ai;
    eCEntity first, second;
    assert(PerceptionBlocked(&first, true));
    {
        DamagePerception outer(&first);
        assert(!PerceptionBlocked(&first, true) && PerceptionBlocked(&second, true));
        {
            DamagePerception inner(&second);
            assert(PerceptionBlocked(&first, true) && !PerceptionBlocked(&second, true));
        }
        assert(!PerceptionBlocked(&first, true));
        assert(!PerceptionBlocked(&second, false));
    }
    assert(PerceptionBlocked(&first, true) && PerceptionBlocked(&second, true));
}

void TestNpcOnlyProcessing()
{
    using namespace g3t::processing;
    const Sphere ranges[] = {{{10000, 0, 0}, 1000}, {{20000, 0, 0}, 1000}};
    const float box[6] = {19900, -10, -10, 20100, 10, 10};
    assert(!IncludesNpc(false, false, box, ranges, 2));
    assert(IncludesNpc(true, false, box, nullptr, 0));
    assert(IncludesNpc(false, true, box, ranges, 2));
    assert(!IncludesNpc(false, true, box, ranges, 1));
    assert(!IncludesNpc(false, true, box, nullptr, 0));
}

void TestTerminalCorpseLifeAndCadence()
{
    g3t::replication::Sequence origin;
    assert(origin.Accept(0xfffffffeu) && origin.Accept(1) && !origin.Accept(0xffffffffu));

    g3t::replication::TerminalLife life;
    assert(life.Observe(1, false) && life.Observe(1, true));
    assert(!life.Observe(1, false) && life.dead && life.Observe(1, true));
    assert(life.Observe(2, false) && !life.dead && !life.Observe(1, true));
    assert(life.Observe(2, true) && !life.Observe(2, false));

    g3t::replication::Cadence60 clock;
    unsigned samples = 0;
    for (uint32_t now = 0; now < 1000; ++now) samples += clock.Due(now);
    assert(samples == 60 && clock.missed == 0);

    g3t::replication::Cadence60 slow;
    samples = 0;
    for (uint32_t now = 0; now < 1000; now += 50) samples += slow.Due(now);
    assert(samples == 20 && slow.missed > 0);

    g3t::replication::Cadence60 wrapped;
    samples = 0;
    for (uint32_t elapsed = 0; elapsed < 1000; ++elapsed) samples += wrapped.Due(0xfffffff0u + elapsed);
    assert(samples == 60);
}

void TestGuidCodec()
{
    char const* canonical = "12345678-9ABC-DEF0-1234-56789ABCDEF0";
    char text[48] = {};
    uint8_t bytes[16] = {};
    uint8_t expected[16] = {0x78, 0x56, 0x34, 0x12, 0xbc, 0x9a, 0xf0, 0xde,
                            0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc, 0xde, 0xf0};
    assert(g3t::guid::ParseBytes(canonical, bytes));
    assert(std::memcmp(bytes, expected, 16) == 0);
    g3t::guid::FormatBytes(bytes, text, sizeof(text));
    assert(std::strcmp(text, canonical) == 0);

    assert(g3t::guid::ParseBytes("12345678-9abc-def0-1234-56789abcdef0-suffix", bytes));
    assert(g3t::guid::ParseBytes("12345678-9ABC-DEF0-1234-56789ABCDEF", bytes));
    assert(!g3t::guid::ParseBytes("12345678/9ABC-DEF0-1234-56789ABCDEF0", bytes));
    assert(!g3t::guid::ParseBytes(nullptr, bytes));

    assert(g3t::guid::ParseBytesExact(canonical, bytes));
    assert(!g3t::guid::ParseBytesExact("12345678-9ABC-DEF0-1234-56789ABCDEF0-extra", bytes));
    assert(!g3t::guid::ParseBytesExact("12345678-9ABC-DEF0-1234-56789ABCDEF", bytes));
    assert(!g3t::guid::ParseBytesExact(nullptr, bytes));

    assert(g3t::guid::ParseBytes("00000000-0000-0000-0000-000000000000", bytes));
    g3t::guid::FormatBytes(bytes, text, sizeof(text));
    assert(std::strcmp(text, "00000000-0000-0000-0000-000000000000") == 0);
}
}

int main()
{
    TestDamageContext();
    TestNpcOnlyProcessing();
    TestTerminalCorpseLifeAndCadence();
    TestGuidCodec();
    return 0;
}
