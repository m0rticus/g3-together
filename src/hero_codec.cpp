// SPDX-License-Identifier: GPL-3.0-or-later
#include "hero_codec.h"

#include <algorithm>
#include <charconv>
#include <cstring>
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

void AppendSection(std::vector<uint8_t>& payload, uint32_t type, uint32_t flags, std::vector<uint8_t> const& data)
{
    saving::PutLe(payload, type, 4);
    saving::PutLe(payload, flags, 4);
    saving::PutLe(payload, data.size(), 4);
    saving::PutLe(payload, saving::Crc32(data.data(), data.size()), 4);
    payload.insert(payload.end(), data.begin(), data.end());
}

char const* ParseBinaryRecord(uint8_t const* bytes, size_t size, saving::Key const& expected,
                              Schema const& schema, ParsedRecord& out)
{
    saving::HeroBinaryEnvelope env;
    if (char const* refused = saving::ParseHeroBinaryEnvelope(bytes, size, env)) return refused;
    if (!saving::Exact(env.key, expected)) return "invalid or mismatched identity or pose";

    ParsedRecord candidate;
    candidate.schemaVersion = env.schemaVersion;
    candidate.minReaderVersion = env.minReaderVersion;
    candidate.key = std::move(env.key);
    candidate.pose = env.pose;

    for (saving::HeroBinarySectionSpan const& sec : env.sections) {
        const uint32_t type = sec.type;
        const size_t at = sec.payloadOffset;
        const size_t secSize = sec.size;
        const size_t secEnd = at + secSize;
        size_t sAt = at;
        if (type == saving::HeroSecIdentity || type == saving::HeroSecPose) {
            sAt = secEnd;
        } else if (type == saving::HeroSecState) {
            uint64_t lvl = 0, lvlMax = 0, xp = 0, lpa = 0, lpp = 0;
            if (secSize != 24 || sAt + 4 > secEnd) return "level, experience or learn points missing";
            candidate.downed = bytes[sAt];
            sAt += 4;
            if (!saving::GetLe(bytes, secEnd, sAt, lvl, 4) || !saving::GetLe(bytes, secEnd, sAt, lvlMax, 4) ||
                !saving::GetLe(bytes, secEnd, sAt, xp, 4) || !saving::GetLe(bytes, secEnd, sAt, lpa, 4) ||
                !saving::GetLe(bytes, secEnd, sAt, lpp, 4)) {
                return "level, experience or learn points missing";
            }
            candidate.level = static_cast<unsigned>(lvl);
            candidate.levelMax = static_cast<unsigned>(lvlMax);
            candidate.xp = static_cast<int32_t>(static_cast<uint32_t>(xp));
            candidate.lpAttribs = static_cast<int32_t>(static_cast<uint32_t>(lpa));
            candidate.lpPerks = static_cast<int32_t>(static_cast<uint32_t>(lpp));
        } else if (type == saving::HeroSecAttributes) {
            uint64_t count = 0;
            if (!saving::GetLe(bytes, secEnd, sAt, count, 4) || count > sizeof(kTags) / sizeof(kTags[0])) {
                return "too many record entries";
            }
            for (uint64_t j = 0; j < count; ++j) {
                if (sAt >= secEnd) return "an attribute missing or twice";
                const size_t tagLen = bytes[sAt++];
                if (!tagLen || tagLen > 8 || tagLen > secEnd - sAt) return "an attribute this record does not know";
                saving::RecordAttribute attr;
                attr.tag.assign(reinterpret_cast<char const*>(bytes + sAt), tagLen);
                sAt += tagLen;
                uint64_t b = 0, m = 0, bm = 0, mm = 0;
                if (!saving::GetLe(bytes, secEnd, sAt, b, 4) || !saving::GetLe(bytes, secEnd, sAt, m, 4) ||
                    !saving::GetLe(bytes, secEnd, sAt, bm, 4) || !saving::GetLe(bytes, secEnd, sAt, mm, 4)) {
                    return "an attribute missing or twice";
                }
                attr.base = static_cast<int32_t>(static_cast<uint32_t>(b));
                attr.modifier = static_cast<int32_t>(static_cast<uint32_t>(m));
                attr.baseMaximum = static_cast<int32_t>(static_cast<uint32_t>(bm));
                attr.maximumModifier = static_cast<int32_t>(static_cast<uint32_t>(mm));
                candidate.attrs.push_back(std::move(attr));
            }
        } else if (type == saving::HeroSecInventory) {
            uint64_t count = 0;
            if (!saving::GetLe(bytes, secEnd, sAt, count, 4) || count > MaxRecordEntries ||
                count > (secEnd - sAt) / 49) {
                return "too many record entries";
            }
            candidate.stacks.reserve(static_cast<size_t>(count));
            for (uint64_t j = 0; j < count; ++j) {
                Stack s;
                s.id.assign(reinterpret_cast<char const*>(bytes + sAt), 32);
                sAt += 32;
                uint64_t amount = 0, quality = 0, slot = 0, cat = 0;
                if (!saving::GetLe(bytes, secEnd, sAt, amount, 4) || !saving::GetLe(bytes, secEnd, sAt, quality, 4) ||
                    !saving::GetLe(bytes, secEnd, sAt, slot, 4) || !saving::GetLe(bytes, secEnd, sAt, cat, 4) ||
                    sAt >= secEnd) {
                    return "damaged or truncated binary hero section";
                }
                s.amount = static_cast<int32_t>(static_cast<uint32_t>(amount));
                s.quality = static_cast<uint32_t>(quality);
                s.slot = static_cast<int32_t>(static_cast<uint32_t>(slot));
                s.category = static_cast<int32_t>(static_cast<uint32_t>(cat));
                s.learned = static_cast<int8_t>(bytes[sAt++]);
                candidate.stacks.push_back(std::move(s));
            }
        } else if (type == saving::HeroSecSpells) {
            uint64_t count = 0;
            if (!saving::GetLe(bytes, secEnd, sAt, count, 4) || count > MaxRecordEntries ||
                count > (secEnd - sAt) / 32) {
                return "too many record entries";
            }
            candidate.spells.reserve(static_cast<size_t>(count));
            for (uint64_t j = 0; j < count; ++j) {
                candidate.spells.emplace_back(reinterpret_cast<char const*>(bytes + sAt), 32);
                sAt += 32;
            }
        } else if (type == saving::HeroSecQuickSlots) {
            uint64_t count = 0;
            if (!saving::GetLe(bytes, secEnd, sAt, count, 4) || count > QuickSlots ||
                count > (secEnd - sAt) / 40) {
                return "too many record entries";
            }
            candidate.quicks.reserve(static_cast<size_t>(count));
            for (uint64_t j = 0; j < count; ++j) {
                Quick q;
                uint64_t slot = 0, quality = 0;
                if (!saving::GetLe(bytes, secEnd, sAt, slot, 4) || sAt + 32 > secEnd) {
                    return "damaged or truncated binary hero section";
                }
                q.slot = static_cast<int32_t>(static_cast<uint32_t>(slot));
                q.id.assign(reinterpret_cast<char const*>(bytes + sAt), 32);
                sAt += 32;
                if (!saving::GetLe(bytes, secEnd, sAt, quality, 4)) {
                    return "damaged or truncated binary hero section";
                }
                q.quality = static_cast<uint32_t>(quality);
                candidate.quicks.push_back(std::move(q));
            }
        } else {
            candidate.extraSections.push_back(
                {type, sec.flags, std::vector<uint8_t>(bytes + at, bytes + secEnd)});
            sAt = secEnd;
        }
        if (sAt != secEnd) return "damaged or truncated binary hero section";
    }
    if (char const* refused = ValidateStatsAndAttributes(candidate)) return refused;
    if (char const* refused = ValidateLogicalContents(candidate.stacks, candidate.spells, candidate.quicks, schema)) {
        return refused;
    }
    out = std::move(candidate);
    return nullptr;
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

char const* ValidateStatsAndAttributes(ParsedRecord const& candidate)
{
    if (candidate.downed != 0 && candidate.downed != 1) return "invalid downed state";
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
    return nullptr;
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

char const* EncodeRecord(ParsedRecord const& record, Schema const& schema, std::vector<uint8_t>& out)
{
    if (!saving::Valid(record.key)) return "invalid path or identity";
    if (!saving::ValidRecordPose(record.pose)) return "invalid hero pose";
    if (char const* refused = ValidateStatsAndAttributes(record)) return refused;
    if (char const* refused = ValidateLogicalContents(record.stacks, record.spells, record.quicks, schema)) {
        return refused;
    }
    if (record.stacks.size() > MaxRecordEntries || record.spells.size() > MaxRecordEntries ||
        record.quicks.size() > QuickSlots || record.extraSections.size() > saving::HeroMaxExtraSections) {
        return "too many record entries";
    }
    std::unordered_set<uint32_t> seenExtra;
    for (auto const& extra : record.extraSections) {
        if (extra.type <= saving::HeroSecQuickSlots || (extra.flags & saving::HeroSectionOptional) == 0 ||
            !seenExtra.insert(extra.type).second) {
            return "invalid optional hero section";
        }
    }

    std::vector<uint8_t> payload;
    std::vector<uint8_t> sec;

    // 1. Identity
    sec.clear();
    sec.insert(sec.end(), record.key.world.begin(), record.key.world.end());
    sec.insert(sec.end(), record.key.save.begin(), record.key.save.end());
    saving::PutLe(sec, record.key.player, 8);
    AppendSection(payload, saving::HeroSecIdentity, saving::HeroSectionRequired, sec);

    // 2. Pose
    sec.clear();
    for (float v : record.pose.position) saving::PutF32(sec, v);
    for (float v : record.pose.rotation) saving::PutF32(sec, v);
    AppendSection(payload, saving::HeroSecPose, saving::HeroSectionRequired, sec);

    // 3. State
    sec.clear();
    sec.push_back(static_cast<uint8_t>(record.downed ? 1 : 0));
    sec.push_back(0); sec.push_back(0); sec.push_back(0);
    saving::PutLe(sec, record.level, 4);
    saving::PutLe(sec, record.levelMax, 4);
    saving::PutLe(sec, static_cast<uint32_t>(static_cast<int32_t>(record.xp)), 4);
    saving::PutLe(sec, static_cast<uint32_t>(static_cast<int32_t>(record.lpAttribs)), 4);
    saving::PutLe(sec, static_cast<uint32_t>(static_cast<int32_t>(record.lpPerks)), 4);
    AppendSection(payload, saving::HeroSecState, saving::HeroSectionRequired, sec);

    // 4. Attributes
    sec.clear();
    saving::PutLe(sec, record.attrs.size(), 4);
    for (auto const& at : record.attrs) {
        sec.push_back(static_cast<uint8_t>(at.tag.size()));
        sec.insert(sec.end(), at.tag.begin(), at.tag.end());
        saving::PutLe(sec, static_cast<uint32_t>(at.base), 4);
        saving::PutLe(sec, static_cast<uint32_t>(at.modifier), 4);
        saving::PutLe(sec, static_cast<uint32_t>(at.baseMaximum), 4);
        saving::PutLe(sec, static_cast<uint32_t>(at.maximumModifier), 4);
    }
    AppendSection(payload, saving::HeroSecAttributes, saving::HeroSectionRequired, sec);

    // 5. Inventory
    sec.clear();
    saving::PutLe(sec, record.stacks.size(), 4);
    for (Stack const& s : record.stacks) {
        sec.insert(sec.end(), s.id.begin(), s.id.end());
        saving::PutLe(sec, static_cast<uint32_t>(static_cast<int32_t>(s.amount)), 4);
        saving::PutLe(sec, s.quality, 4);
        saving::PutLe(sec, static_cast<uint32_t>(static_cast<int32_t>(s.slot)), 4);
        saving::PutLe(sec, static_cast<uint32_t>(static_cast<int32_t>(s.category)), 4);
        sec.push_back(static_cast<uint8_t>(static_cast<int8_t>(s.learned)));
    }
    AppendSection(payload, saving::HeroSecInventory, saving::HeroSectionRequired, sec);

    // 6. Spells
    sec.clear();
    saving::PutLe(sec, record.spells.size(), 4);
    for (std::string const& id : record.spells) sec.insert(sec.end(), id.begin(), id.end());
    AppendSection(payload, saving::HeroSecSpells, saving::HeroSectionRequired, sec);

    // 7. Quick slots
    sec.clear();
    saving::PutLe(sec, record.quicks.size(), 4);
    for (Quick const& q : record.quicks) {
        saving::PutLe(sec, static_cast<uint32_t>(static_cast<int32_t>(q.slot)), 4);
        sec.insert(sec.end(), q.id.begin(), q.id.end());
        saving::PutLe(sec, q.quality, 4);
    }
    AppendSection(payload, saving::HeroSecQuickSlots, saving::HeroSectionRequired, sec);

    // 8+. Forward-compatible optional sections
    for (auto const& extra : record.extraSections) AppendSection(payload, extra.type, extra.flags, extra.bytes);

    if (saving::HeroHeaderSize + payload.size() > MaxRecordBytes) return "record exceeds maximum size";

    std::vector<uint8_t> result;
    result.reserve(saving::HeroHeaderSize + payload.size());
    result.insert(result.end(), saving::HeroMagic, saving::HeroMagic + sizeof(saving::HeroMagic));
    saving::PutLe(result, saving::HeroSchemaVersion, 4);
    saving::PutLe(result, saving::HeroMinReaderVersion, 4);
    saving::PutLe(result, saving::HeroHeaderSize, 4);
    saving::PutLe(result, payload.size(), 4);
    saving::PutLe(result, 0, 4); // recordCrc32 placeholder at [24..28)
    saving::PutLe(result, 0, 4); // flags
    saving::PutLe(result, 7 + record.extraSections.size(), 4);
    saving::PutLe(result, 0, 8); // reserved
    const uint32_t headerCrc = saving::Crc32Zeroed(result.data(), 44, 24, 4);
    saving::PutLe(result, headerCrc, 4);
    result.insert(result.end(), payload.begin(), payload.end());
    const uint32_t recordCrc = saving::Crc32Zeroed(result.data(), result.size(), 24, 4);
    saving::PutLeAt(result.data() + 24, recordCrc, 4);
    out = std::move(result);
    return nullptr;
}

char const* ParseBoundRecord(char const* data, size_t size, saving::Key const& expected,
                             Schema const& schema, ParsedRecord& out)
{
    if (!saving::Valid(expected)) return "invalid path or identity";
    if (!data || size == 0) return "no hero record header";
    if (size > MaxRecordBytes) return "record exceeds maximum size";
    if (saving::IsHeroBinaryMagic(reinterpret_cast<uint8_t const*>(data), size)) {
        return ParseBinaryRecord(reinterpret_cast<uint8_t const*>(data), size, expected, schema, out);
    }

    std::vector<std::string> lines;
    char const* refused = nullptr;
    if (!SplitLines(data, size, lines, refused)) return refused;
    if (lines.empty() || lines[0].compare(0, 8, "G3THERO ") != 0) return "no hero record header";
    if (lines[0] != "G3THERO 4") return "bound load requires a v4 record";
    if (lines.size() < 3) return "invalid or mismatched identity or pose";

    ParsedRecord candidate;
    candidate.schemaVersion = 4;
    candidate.minReaderVersion = 4;
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
    refused = ValidateStatsAndAttributes(candidate);
    if (refused) return refused;
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

std::vector<size_t> RestoreStackOrder(std::vector<Stack> const& stacks, int skillCategory, int noneSlot)
{
    std::vector<size_t> order;
    order.reserve(stacks.size());
    for (int pass = 0; pass < 3; ++pass) {
        for (size_t i = 0; i < stacks.size(); ++i) {
            Stack const& s = stacks[i];
            const int bucket = s.category == skillCategory ? 0 : (s.slot != noneSlot ? 1 : 2);
            if (bucket == pass) order.push_back(i);
        }
    }
    return order;
}

} // namespace g3t::hero::codec
