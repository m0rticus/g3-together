// SPDX-License-Identifier: GPL-3.0-or-later
#include "hero_codec.h"

#include <algorithm>
#include <charconv>
#include <map>
#include <tuple>
#include <unordered_set>

namespace g3t::hero::codec {
namespace {

char const* const kTags[] = {"STR", "DEX", "INT", "ALC", "SMT", "THF", "HP", "MP", "SP"};

template <typename T>
bool ParseInteger(std::string const& text, T& value)
{
    auto const result = std::from_chars(text.data(), text.data() + text.size(), value);
    return result.ec == std::errc() && result.ptr == text.data() + text.size();
}

bool SplitLines(char const* data, size_t size, std::vector<std::string>& lines, char const*& refused)
{
    lines.clear();
    if (!data || size == 0) {
        refused = "no hero record header";
        return false;
    }
    if (size > MaxRecordBytes) {
        refused = "record exceeds maximum size";
        return false;
    }
    size_t start = 0;
    while (start < size) {
        size_t end = start;
        while (end < size && data[end] != '\n') {
            if (data[end] == '\0' || end - start >= 511) {
                refused = "a damaged or unreadable line";
                return false;
            }
            ++end;
        }
        size_t len = end - start;
        if (len > 0 && data[start + len - 1] == '\r') --len;
        lines.emplace_back(data + start, len);
        if (end == size) break;
        start = end + 1;
    }
    return true;
}

} // namespace

char const* const* AttributeTags(size_t& count)
{
    count = sizeof(kTags) / sizeof(kTags[0]);
    return kTags;
}

char const* ValidateLogicalContents(std::vector<Stack> const& stacks, std::vector<std::string> const& spells,
                                    std::vector<Quick> const& quicks, Schema const& schema)
{
    std::unordered_set<std::string> skills, spellIds, foundSpells;
    std::map<std::pair<std::string, unsigned>, size_t> stackCounts, quickCounts;
    for (std::string const& id : spells) {
        if (!saving::Id(id) || !spellIds.insert(id).second) return "an invalid or duplicate spell";
    }
    for (Stack const& s : stacks) {
        const bool isSkill = s.category == schema.skillCategory;
        if (!saving::Id(s.id) || s.amount < 1 || s.amount > 1000000 || s.quality >= 1024 ||
            s.slot < schema.minSlot || s.slot > schema.maxSlot ||
            s.category < 0 || s.category >= schema.categoryCount ||
            (isSkill ? (s.learned < 0 || s.learned > 1 || !skills.insert(s.id).second) : s.learned != -1)) {
            return "a stack out of range or a duplicate skill";
        }
        if (s.category == schema.spellbookCategory && spellIds.count(s.id)) foundSpells.insert(s.id);
        ++stackCounts[{s.id, s.quality}];
    }
    if (foundSpells.size() != spellIds.size()) return "a missing spell item";
    bool used[QuickSlots] = {};
    for (Quick const& q : quicks) {
        if (q.slot < 0 || q.slot >= QuickSlots || used[q.slot] || !saving::Id(q.id) || q.quality >= 1024 ||
            ++quickCounts[{q.id, q.quality}] > stackCounts[{q.id, q.quality}]) {
            return "a missing or duplicate quick slot item";
        }
        used[q.slot] = true;
    }
    return nullptr;
}

char const* ParseBoundRecord(char const* data, size_t size, saving::Key const& expected,
                             Schema const& schema, ParsedRecord& out)
{
    if (!saving::Valid(expected)) return "invalid path or identity";
    std::vector<std::string> lines;
    char const* refused = nullptr;
    if (!SplitLines(data, size, lines, refused)) return refused;
    if (lines.empty() || lines[0].compare(0, 8, "G3THERO ") != 0) return "no hero record header";
    if (lines[0] != "G3THERO 4") return "bound load requires a v4 record";
    if (lines.size() < 3) return "invalid or mismatched identity or pose";

    ParsedRecord candidate;
    if (!saving::ParseRecordMetadata(lines[0], lines[1], lines[2], &expected, candidate.key, candidate.pose)) {
        return "invalid or mismatched identity or pose";
    }

    bool haveLevel = false, haveXp = false, haveLp = false, haveDowned = false;
    bool inlineQuickEnded = false, ended = false;
    size_t declaredSpells = 0, declaredQuick = 0, declaredStacks = 0;

    for (size_t idx = 3; idx < lines.size() && !refused; ++idx) {
        std::string const& text = lines[idx];
        auto const words = saving::detail::RecordWords(text);
        Stack s;
        Quick q;
        saving::RecordAttribute attr;
        if (ended || words.empty()) {
            refused = "a line after the end or an empty line";
        } else if (inlineQuickEnded) {
            if (words.size() == 2 && words[0] == "end" && ParseInteger(words[1], declaredStacks)) ended = true;
            else refused = "a line after quick_end";
        } else if (words.size() == 3 && words[0] == "level" && !haveLevel &&
                   ParseInteger(words[1], candidate.level) && ParseInteger(words[2], candidate.levelMax)) {
            haveLevel = true;
        } else if (words.size() == 2 && words[0] == "xp" && !haveXp && ParseInteger(words[1], candidate.xp)) {
            haveXp = true;
        } else if (words.size() == 3 && words[0] == "lp" && !haveLp &&
                   ParseInteger(words[1], candidate.lpAttribs) && ParseInteger(words[2], candidate.lpPerks)) {
            haveLp = true;
        } else if (words[0] == "attr" && saving::ParseRecordAttribute(text, attr)) {
            candidate.attrs.push_back(attr);
        } else if ((words.size() == 6 || words.size() == 7) && words[0] == "stack" &&
                   ParseInteger(words[2], s.amount) && ParseInteger(words[3], s.quality) &&
                   ParseInteger(words[4], s.slot) && ParseInteger(words[5], s.category) &&
                   (words.size() == 6 || ParseInteger(words[6], s.learned)) &&
                   ((s.category == schema.skillCategory) == (words.size() == 7))) {
            s.id = words[1];
            candidate.stacks.push_back(s);
        } else if (saving::ParseRecordDowned(text, 4, haveDowned, candidate.downed)) {
            haveDowned = true;
        } else if (words.size() == 2 && words[0] == "spell") {
            candidate.spells.push_back(words[1]);
        } else if (words.size() == 4 && words[0] == "quick" &&
                   ParseInteger(words[1], q.slot) && ParseInteger(words[3], q.quality)) {
            q.id = words[2];
            candidate.quicks.push_back(q);
        } else if (words.size() == 3 && words[0] == "quick_end" &&
                   ParseInteger(words[1], declaredSpells) && ParseInteger(words[2], declaredQuick)) {
            inlineQuickEnded = true;
        } else {
            refused = "a line that is no record line";
        }

        if (candidate.stacks.size() > MaxRecordEntries || candidate.spells.size() > MaxRecordEntries ||
            candidate.quicks.size() > QuickSlots || candidate.attrs.size() > sizeof(kTags) / sizeof(kTags[0])) {
            refused = "too many record entries";
        }
    }
    if (refused) return refused;

    if (!haveLevel || !haveXp || !haveLp) return "level, experience or learn points missing";
    if (!inlineQuickEnded) return "no quick_end line";
    if (!ended) return "no end line (the record cannot be verified)";
    if (declaredStacks != candidate.stacks.size()) return "fewer stacks than the end line names";
    if (declaredSpells != candidate.spells.size() || declaredQuick != candidate.quicks.size())
        return "the quick_end counts do not match the inline spell and quick lines";
    if (candidate.level > 1000 || candidate.levelMax > 1000 ||
        candidate.xp < 0 || candidate.xp > 100000000 ||
        candidate.lpAttribs < 0 || candidate.lpAttribs > 100000 ||
        candidate.lpPerks < 0 || candidate.lpPerks > 100000) {
        return "level, experience or learn points out of range";
    }
    for (char const* tag : kTags) {
        if (std::count_if(candidate.attrs.begin(), candidate.attrs.end(),
                          [tag](saving::RecordAttribute const& at) { return at.tag == tag; }) != 1) {
            return "an attribute missing or twice";
        }
    }
    if (candidate.attrs.size() != sizeof(kTags) / sizeof(kTags[0])) return "an attribute this record does not know";
    for (auto const& at : candidate.attrs) {
        if (!saving::ValidRecordAttribute(at)) return "an attribute out of range";
    }
    refused = ValidateLogicalContents(candidate.stacks, candidate.spells, candidate.quicks, schema);
    if (refused) return refused;

    out = std::move(candidate);
    return nullptr;
}

bool SameStacks(std::vector<Stack> const& a, std::vector<Stack> const& b)
{
    using Key = std::tuple<std::string, unsigned, int, int, int>;
    std::map<Key, long long> totals;
    for (Stack const& s : a) totals[{s.id, s.quality, s.slot, s.category, s.learned}] += s.amount;
    for (Stack const& s : b) totals[{s.id, s.quality, s.slot, s.category, s.learned}] -= s.amount;
    for (auto const& entry : totals) {
        if (entry.second != 0) return false;
    }
    return true;
}

} // namespace g3t::hero::codec
