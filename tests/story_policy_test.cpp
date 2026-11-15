// SPDX-License-Identifier: GPL-3.0-or-later
#include "../src/story_policy.h"
#include <cassert>

using namespace g3t::story::policy;
int main()
{
    uint32_t value = 77;
    assert(Number("0", value) && value == 0);
    assert(Number("4294967295", value) && value == UINT32_MAX);
    for (auto text : {"", "-1", "4294967296", "1x", " 1", "00000000000"}) {
        value = 77;
        assert(!Number(text, value) && value == 77);
    }
    const std::string first = NameId(std::string(200, 'a'));
    assert(first.size() == 17 && first == NameId(std::string(200, 'a')));
    assert(first != NameId(std::string(199, 'a') + 'b'));
    std::string packed, key;
    const std::string original = first + '\x1f' + "c511";
    assert(PackRuntime(original, UINT32_MAX, 63, packed));
    assert(UnpackRuntime(packed, key, value) && key == original && value == UINT32_MAX);
    const std::string infoEndTs = first + '\x1f' + "i";
    assert(PackRuntime(infoEndTs, 123456u, 63, packed));
    assert(UnpackRuntime(packed, key, value) && key == infoEndTs && value == 123456u);
    for (auto field : {"c512", "c-1", "c", "i0", "ix", "z", ""}) {
        key = "unchanged";
        assert(!PackRuntime(first + '\x1f' + field, 1, 63, key) && key == "unchanged");
    }
    key = "unchanged"; value = 77;
    assert(!UnpackRuntime(original + '\x1f' + "4294967296", key, value));
    assert(key == "unchanged" && value == 77);
    assert(!PackRuntime(original, 1, 4, key));
    Baseline baseline;
    assert(!baseline.Finish() && !baseline.Add("quest", 1));
    baseline.Begin();
    assert(baseline.Add("quest", 1) && !baseline.complete); // A lost end marker never admits gameplay.
    baseline.Begin(); // A partial failed snapshot is replaced by the retry, not merged with stale values.
    assert(baseline.values.empty() && baseline.Add("other", 2));
    assert(baseline.Finish() && baseline.complete);
    assert(!baseline.Finish() && !baseline.Add("late", 3)); // Duplicate/end and late chunks cannot rewind.
    Baseline kinds[5];
    for (int i = 0; i < 5; ++i) {
        kinds[i].Begin();
        kinds[i].Add("state", i);
        assert(kinds[i].Finish());
        for (int pending = i + 1; pending < 5; ++pending) assert(!kinds[pending].complete);
    }
    const std::map<std::string, uint32_t> before{{"quest", 1}, {"cleared", 1}, {"unchanged", 2}, {"removed", 1}};
    const auto changes = Changes(before, std::map<std::string, uint32_t>{{"quest", 2}, {"cleared", 0}, {"unchanged", 2}});
    assert(before.at("quest") == 1); // Snapshot retries must stay immutable while the host advances.
    assert(changes.size() == 3 && changes.at("quest") == 2 && changes.at("cleared") == 0 && changes.at("removed") == 0);
    assert((Snapshot | Catchup | 4) & Catchup);
    assert(((Snapshot | Catchup | 4) & KindMask) == 4);
    struct Clock { uint16_t year = 0, day = 0; uint8_t hour = 0, minute = 0; };
    Clock clock{2, 42, 18, 59}, parsed{};
    assert(ReadClock(ClockText(clock), parsed) && parsed.year == 2 && parsed.day == 42 && parsed.hour == 18 && parsed.minute == 59);
    for (auto text : {"@clock/65536/1/2/3", "@clock/1/65536/2/3", "@clock/1/2/24/3",
                      "@clock/1/2/3/60", "@clock/1/2/3", "@clock/1/2/3/4/5", "@clock/-1/2/3/4", "@clock/1/2/3/"}) {
        parsed = clock;
        assert(!ReadClock(text, parsed) && parsed.year == 2 && parsed.day == 42);
    }
}
