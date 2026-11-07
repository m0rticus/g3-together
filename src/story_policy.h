// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <cstdio>
#include <limits>
#include <map>
#include <string>

namespace g3t::story::policy {
constexpr uint8_t Snapshot = 0x80, End = 0x40, Catchup = 0x20, KindMask = 0x1f;
template<class States>
States Changes(States const& before, States const& current)
{
    States changed;
    for (auto const& entry : current) {
        const auto previous = before.find(entry.first);
        if (previous == before.end() || previous->second != entry.second) changed.insert(entry);
    }
    for (auto const& entry : before)
        if (entry.second && !current.count(entry.first)) changed[entry.first] = 0;
    return changed;
}
inline std::string NameId(std::string const& name)
{
    uint64_t hash = 14695981039346656037ull;
    for (unsigned char c : name) hash = (hash ^ c) * 1099511628211ull;
    char text[18]{};
    std::snprintf(text, sizeof(text), "#%016llx", static_cast<unsigned long long>(hash));
    return text;
}
inline bool Number(std::string const& text, uint32_t& out)
{
    if (text.empty() || text.size() > 10) return false;
    uint32_t value = 0;
    for (char c : text) {
        if (c < '0' || c > '9' || value > ((std::numeric_limits<uint32_t>::max)() - (c - '0')) / 10) return false;
        value = value * 10 + (c - '0');
    }
    out = value;
    return true;
}
template<class Clock>
std::string ClockText(Clock const& clock)
{
    return "@clock/" + std::to_string(clock.year) + "/" + std::to_string(clock.day) + "/" +
           std::to_string(clock.hour) + "/" + std::to_string(clock.minute);
}
template<class Clock>
bool ReadClock(std::string const& text, Clock& out)
{
    if (text.compare(0, 7, "@clock/")) return false;
    uint32_t values[4]{};
    size_t begin = 7;
    for (unsigned i = 0; i < 4; ++i) {
        const size_t end = text.find('/', begin);
        if ((i < 3) != (end != std::string::npos) ||
            !Number(text.substr(begin, end == std::string::npos ? end : end - begin), values[i])) return false;
        begin = end + 1;
    }
    if (values[0] > 65535 || values[1] > 65535 || values[2] > 23 || values[3] > 59) return false;
    Clock clock{};
    clock.year = static_cast<decltype(clock.year)>(values[0]);
    clock.day = static_cast<decltype(clock.day)>(values[1]);
    clock.hour = static_cast<decltype(clock.hour)>(values[2]);
    clock.minute = static_cast<decltype(clock.minute)>(values[3]);
    out = clock;
    return true;
}
inline bool RuntimeKey(std::string const& key, std::string& quest, std::string& field)
{
    const size_t split = key.rfind('\x1f');
    if (split == std::string::npos || !split || split + 1 == key.size()) return false;
    const std::string part = key.substr(split + 1);
    uint32_t index = 0;
    if (part != "y" && part != "d" && part != "h" && part != "n" &&
        !(part[0] == 'c' && Number(part.substr(1), index) && index < 512)) return false;
    quest = key.substr(0, split); field = part;
    return true;
}
inline bool PackRuntime(std::string const& key, uint32_t value, size_t limit, std::string& out)
{
    std::string quest, field;
    if (!RuntimeKey(key, quest, field)) return false;
    const std::string packed = key + '\x1f' + std::to_string(value);
    if (packed.size() >= limit) return false;
    out = packed;
    return true;
}
inline bool UnpackRuntime(std::string const& packed, std::string& key, uint32_t& value)
{
    const size_t split = packed.rfind('\x1f');
    if (split == std::string::npos) return false;
    const std::string found = packed.substr(0, split);
    std::string quest, field;
    uint32_t number = 0;
    if (!RuntimeKey(found, quest, field) || !Number(packed.substr(split + 1), number)) return false;
    key = found; value = number;
    return true;
}
struct Baseline {
    std::map<std::string, uint32_t> values;
    bool begun = false, complete = false;
    void Begin() { values.clear(); begun = true; complete = false; }
    bool Add(std::string const& name, uint32_t value)
    {
        if (!begun || complete || name.empty()) return false;
        values[name] = value;
        return true;
    }
    bool Finish()
    {
        if (!begun || complete) return false;
        complete = true;
        return true;
    }
};
}
