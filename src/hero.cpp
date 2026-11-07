// SPDX-License-Identifier: GPL-3.0-or-later
#include "hero.h"

#include "body.h"
#include "downed.h"
#include "log.h"
#include "saving_policy.h"

#include <Game.h>
#include <Script.h>
#include <Windows.h>
#include <io.h>
#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <sstream>
#include <string>
#include <tuple>
#include <unordered_set>
#include <vector>

namespace g3t::hero {
namespace {
// Attribute tags of the hero's PlayerMemory. "HP" and "STR"/"SMT" are confirmed by static analysis
// (SetHitPoints -> SetBaseValue("HP"), dialog BoostAttribs); the others follow the same naming and are
// checked against the named getters by the QA dump.
char const* const Tags[] = {"STR", "DEX", "INT", "ALC", "SMT", "THF", "HP", "MP", "SP"};
bool restoring = true;
bool checking = true;     // QA negative control: recordcheckoff (a record is applied as read, as before 22.10.)
char const* lastRefusal = nullptr;  // why the last LoadRecord refused its record, nullptr if it did not (LastRefusal)
bool replaceSkills = true;  // QA negative control: skillreplaceoff (skills only taught on top, as before 23.10.)
bool writeFault = false;  // QA (stage W2): recordwritefault breaks off the next write before it replaces the record
bool restoreQuick = true;  // QA negative control: quickrestoreoff (the quick slots stay empty after a record, as before 27.10.)
bool restoreSpells = true;  // QA negative control: spellrestoreoff (learned spells come back unlearned, as before 27.10.)
constexpr int QuickSlots = 16;  // slot numbers asked for, 0 to 15 (the quick bar shows ten)

bool ReadLine(FILE* file, std::string& line, bool* malformed = nullptr)
{
    line.clear();
    int ch;
    while ((ch = fgetc(file)) != EOF && ch != '\n') {
        if (ch == 0 || line.size() >= 511) {
            if (malformed) *malformed = true;
            return false;
        }
        line.push_back(static_cast<char>(ch));
    }
    if (ferror(file)) {
        if (malformed) *malformed = true;
        return false;
    }
    if (ch == EOF && line.empty()) return false;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    return true;
}

template<typename T> bool Integer(std::string const& text, T& value)
{
    auto const result = std::from_chars(text.data(), text.data() + text.size(), value);
    return result.ec == std::errc() && result.ptr == text.data() + text.size();
}

std::vector<std::string> Words(std::string const& line)
{
    std::istringstream input(line);
    std::vector<std::string> words;
    std::string word;
    while (input >> word) words.push_back(word);
    return words;
}

struct Stack
{
    std::string id;  // template ID, 32 hex digits
    int amount = 0;
    unsigned quality = 0;
    int slot = 0;      // gESlot it is equipped to, 0 if none
    int category = 0;  // gEItemCategory (10: skill, the perk item of one skill)
    int learned = -1;  // skills: whether the hero knows that skill (a sixth field since 23.10.), -1: not in the record
};

std::string Hex(bCPropertyID const& id)
{
    char out[33];
    auto const* bytes = reinterpret_cast<unsigned char const*>(&id.GetCore());
    for (int i = 0; i < 16; ++i) Format(out + 2 * i, 3, "%02x", bytes[i]);
    return out;
}

Template FromHex(std::string const& hex)
{
    _GUID raw;
    auto* bytes = reinterpret_cast<unsigned char*>(&raw);
    for (int i = 0; i < 16; ++i) {
        unsigned v = 0;
        sscanf_s(hex.c_str() + 2 * i, "%2x", &v);
        bytes[i] = static_cast<unsigned char>(v);
    }
    return Template(bCPropertyID(bCGuid(raw)));
}

// A skill stack is the perk item (It_Perk_PotionHP); whether the hero knows it is its skill's (Perk_PotionHP) learned
// state, the one HasLearnedSkill and TeachSkill take. The hero carries the items of skills it has not learned too
// (skilldiag 23.10.: with the stack's own template both did nothing; a removed stack unlearned its skill, made again it
// stayed unlearned, TeachSkill with the skill taught it).
Template SkillOf(Entity& self, int i)
{
    Entity item = self.Inventory.GetTemplateItem(i);
    Entity skill = item.GetInstance() ? item.Item.GetSkill() : Entity();
    return skill.GetInstance() ? skill.GetTemplate() : Template();
}

int FindSkillStack(Entity& self, std::string const& hex)
{
    for (int i = 0; i < self.Inventory.GetStackCount(); ++i) {
        if (static_cast<int>(self.Inventory.GetCategory(i)) != gEItemCategory_Skill) continue;
        const Template t = self.Inventory.GetTemplate(i);
        bCPropertyID const& id = t;
        if (Hex(id) == hex) return i;
    }
    return -1;
}

std::vector<Stack> Stacks(Entity& self)
{
    std::vector<Stack> out;
    for (int i = 0; i < self.Inventory.GetStackCount(); ++i) {
        Stack s;
        const Template item = self.Inventory.GetTemplate(i);
        bCPropertyID const& id = item;
        s.id = Hex(id);
        s.amount = self.Inventory.GetAmount(i);
        s.quality = self.Inventory.GetQuality(i);
        s.slot = static_cast<int>(self.Inventory.GetSlotFromStack(i));
        s.category = static_cast<int>(self.Inventory.GetCategory(i));
        if (s.category == gEItemCategory_Skill) {
            const Template skill = SkillOf(self, i);
            s.learned = skill.IsValid() && self.Inventory.HasLearnedSkill(skill) ? 1 : 0;
        }
        out.push_back(s);
    }
    std::sort(out.begin(), out.end(), [](Stack const& a, Stack const& b) { return a.id != b.id ? a.id < b.id : a.slot < b.slot; });
    return out;
}

eCDynamicEntity* Hero() { return gCSession::GetInstance().GetHero(); }
bool Ready(eCDynamicEntity* hero)
{
    return hero && hero->GetPropertySet(eEPropertySetType_NPC) && hero->GetPropertySet(eEPropertySetType_Inventory) &&
           hero->GetPropertySet(eEPropertySetType_PlayerMemory);
}

bool CaptureAttributes(gCPlayerMemory_PS& memory, std::vector<saving::RecordAttribute>& attrs)
{
    for (char const* tag : Tags) {
        const bCString name(tag);
        gCAttribute* attribute = memory.GetAttribute(name);
        if (!attribute) return false;
        saving::RecordAttribute value;
        value.tag = tag;
        value.base = memory.GetBaseValue(name);
        value.modifier = memory.GetModifier(name);
        if (saving::StatTag(value.tag)) {
            auto* stat = dynamic_cast<gCStat*>(attribute);
            if (!stat) return false;
            value.baseMaximum = stat->GetBaseMaximum();
            value.maximumModifier = stat->GetMaximumModifier();
        }
        if (!saving::ValidRecordAttribute(value) || memory.GetValue(name) != value.base + value.modifier ||
            memory.GetMaximum(name) != (saving::StatTag(value.tag) ? value.baseMaximum + value.maximumModifier
                                                                   : value.base + value.modifier)) return false;
        attrs.push_back(value);
    }
    return true;
}

bool AttributesMatch(gCPlayerMemory_PS& memory, std::vector<saving::RecordAttribute> const& attrs, bool held)
{
    for (auto const& value : attrs) {
        const bCString name(value.tag.c_str());
        const int base = held && value.tag == "HP" ? 1 : value.base;
        gCAttribute* attribute = memory.GetAttribute(name);
        if (!attribute || memory.GetBaseValue(name) != base || memory.GetModifier(name) != value.modifier ||
            memory.GetValue(name) != base + value.modifier) return false;
        if (saving::StatTag(value.tag)) {
            auto* stat = dynamic_cast<gCStat*>(attribute);
            if (!stat || stat->GetBaseMaximum() != value.baseMaximum || stat->GetMaximumModifier() != value.maximumModifier ||
                memory.GetMaximum(name) != value.baseMaximum + value.maximumModifier) return false;
        } else if (memory.GetMaximum(name) != base + value.modifier) return false;
    }
    return true;
}

bool ApplyAttributes(gCPlayerMemory_PS& memory, std::vector<saving::RecordAttribute> const& attrs)
{
    for (auto const& value : attrs) {
        const bCString name(value.tag.c_str());
        gCAttribute* attribute = memory.GetAttribute(name);
        if (!attribute) return false;
        if (memory.GetModifier(name) != value.modifier && !memory.SetModifier(name, value.modifier)) return false;
        if (saving::StatTag(value.tag)) {
            auto* stat = dynamic_cast<gCStat*>(attribute);
            if (!stat) return false;
            stat->SetMaximumModifier(value.maximumModifier);
            stat->SetBaseMaximum(value.baseMaximum);
        }
        if (memory.GetBaseValue(name) != value.base && !memory.SetBaseValue(name, value.base)) return false;
    }
    return AttributesMatch(memory, attrs, false);
}

// A quick slot sits on its stack (gCInventoryStack::m_iQuickSlot), so a stack the record deletes and makes again comes
// back without it: after every load of the host's save the client's quick bar was empty (playtest 27.10.). V3 embeds the
// complete spell and quick data in the main record; the compatibility <record>.quick.txt is also written for inspection
// and retained for loading legacy v1/v2 records.
struct Quick
{
    int slot = 0;
    std::string id;  // template ID, 32 hex digits
    unsigned quality = 0;
};

int StackQuickSlot(Entity& self, int i)
{
    auto* inventory = static_cast<gCInventory_PS*>(self.GetInstance()->GetPropertySet(eEPropertySetType_Inventory));
    gCInventoryStack const* stack = inventory ? inventory->GetStack(i) : nullptr;
    return stack ? stack->GetQuickSlot() : -1;
}

// A learned spell is its stack's Learned byte (LearnStack sets it, LearnSpell finds the stack first); the record makes
// the stack again and the byte with it starts at 0, so the client's spells were gone after a load as well.
int StackLearned(Entity& self, int i)
{
    auto* inventory = static_cast<gCInventory_PS*>(self.GetInstance()->GetPropertySet(eEPropertySetType_Inventory));
    gCInventoryStack const* stack = inventory ? inventory->GetStack(i) : nullptr;
    return stack ? (stack->GetLearned() ? 1 : 0) : -1;
}

std::vector<std::string> LearnedSpellsOf(Entity& self)
{
    std::vector<std::string> out;
    for (int i = 0; i < self.Inventory.GetStackCount(); ++i) {
        if (static_cast<int>(self.Inventory.GetCategory(i)) != gEItemCategory_Spellbook || StackLearned(self, i) != 1) continue;
        const Template item = self.Inventory.GetTemplate(i);
        bCPropertyID const& id = item;
        out.push_back(Hex(id));
    }
    return out;
}

// A slot counts when the game's own lookup (the one its keys use) finds a stack that names that slot.
std::vector<Quick> QuickSlotsOf(Entity& self)
{
    std::vector<Quick> out;
    for (int slot = 0; slot < QuickSlots; ++slot) {
        const GEInt i = self.Inventory.FindStackIndexByQuickSlot(slot);
        if (i < 0 || i >= self.Inventory.GetStackCount() || StackQuickSlot(self, i) != slot) continue;
        const Template item = self.Inventory.GetTemplate(i);
        bCPropertyID const& id = item;
        out.push_back({slot, Hex(id), self.Inventory.GetQuality(i)});
    }
    return out;
}

char const* ValidateContents(std::vector<Stack> const& stacks, std::vector<std::string> const& spells,
                             std::vector<Quick> const& quick, bool templates = true)
{
    std::unordered_set<std::string> skills, spellIds, foundSpells;
    std::map<std::pair<std::string, unsigned>, size_t> stackCounts, quickCounts;
    for (std::string const& id : spells)
        if (!saving::Id(id) || !spellIds.insert(id).second) return "an invalid or duplicate spell";
    for (Stack const& s : stacks) {
        if (!saving::Id(s.id) || s.amount < 1 || s.amount > 1000000 || s.quality >= 1024 ||
            s.slot < gESlot_None || s.slot > gESlot_Helmet || s.category < 0 || s.category >= gEItemCategory_Count ||
            (s.category == gEItemCategory_Skill ? (s.learned < 0 || s.learned > 1 || !skills.insert(s.id).second) : s.learned != -1))
            return "a stack out of range or a duplicate skill";
        if (templates) {
            const Template item = FromHex(s.id);
            if (!item.IsValid()) return "an invalid item template";
            eCTemplateEntityProxy proxy;
            proxy.SetEntity(static_cast<bCPropertyID const&>(item));
            eCTemplateEntity* templateEntity = proxy.GetEntity();
            if (!templateEntity || !templateEntity->GetPropertySet(eEPropertySetType_Item))
                return "a stack whose item does not exist";
            Entity templateItem(templateEntity);
            if (static_cast<int>(templateItem.Item.GetCategory()) != s.category) return "a stack with the wrong item category";
            if (s.category == gEItemCategory_Skill) {
                Entity skill = templateItem.Item.GetSkill();
                if (!skill.GetInstance() || !skill.GetTemplate().IsValid()) return "a skill whose template does not exist";
            }
            if (s.category == gEItemCategory_Spellbook && spellIds.count(s.id)) {
                Entity spell = templateItem.Item.GetSpell();
                if (!spell.GetInstance() || !spell.GetTemplate().IsValid()) return "a spell whose template does not exist";
            }
        }
        if (s.category == gEItemCategory_Spellbook && spellIds.count(s.id)) foundSpells.insert(s.id);
        ++stackCounts[{s.id, s.quality}];
    }
    if (foundSpells.size() != spellIds.size()) return "a missing spell item";
    bool used[QuickSlots] = {};
    for (Quick const& q : quick) {
        if (q.slot < 0 || q.slot >= QuickSlots || used[q.slot] || !saving::Id(q.id) || q.quality >= 1024 ||
            ++quickCounts[{q.id, q.quality}] > stackCounts[{q.id, q.quality}])
            return "a missing or duplicate quick slot item";
        used[q.slot] = true;
    }
    return nullptr;
}

bool SameStacks(std::vector<Stack> const& a, std::vector<Stack> const& b)
{
    using Key = std::tuple<std::string, unsigned, int, int, int>;
    std::map<Key, long long> totals;
    for (Stack const& s : a) totals[{s.id, s.quality, s.slot, s.category, s.learned}] += s.amount;
    for (Stack const& s : b) totals[{s.id, s.quality, s.slot, s.category, s.learned}] -= s.amount;
    for (auto const& entry : totals) if (entry.second != 0) return false;
    return true;
}

std::string QuickPath(char const* record)
{
    std::string path = record;
    if (path.size() > 4 && !_stricmp(path.c_str() + path.size() - 4, ".txt")) path.resize(path.size() - 4);
    return path + ".quick.txt";
}

bool SaveQuick(Entity& self, char const* record)
{
    const std::string path = QuickPath(record), temp = path + ".tmp";
    FILE* file = nullptr;
    if (fopen_s(&file, temp.c_str(), "w") || !file) return false;
    const std::vector<Quick> quick = QuickSlotsOf(self);
    const std::vector<std::string> spells = LearnedSpellsOf(self);
    bool ok = fprintf(file, "G3TQUICK 1\n") > 0;
    for (std::string const& id : spells) ok = ok && fprintf(file, "spell %s\n", id.c_str()) > 0;
    for (Quick const& q : quick) ok = ok && fprintf(file, "quick %d %s %u\n", q.slot, q.id.c_str(), q.quality) > 0;
    ok = ok && fprintf(file, "end %zu\n", spells.size() + quick.size()) > 0;
    ok = fflush(file) == 0 && ok;
    ok = _commit(_fileno(file)) == 0 && ok;
    ok = fclose(file) == 0 && ok;
    if (ok) ok = MoveFileExA(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
    if (!ok) {
        DeleteFileA(temp.c_str());
        Logf("hero: quick slots %s not written; the ones before stay", path.c_str());
    }
    return ok;
}

// After the record went on: the learned spells first (a spell that is not learned may not go on a quick slot), then each
// slot back on a stack of its item (that quality first). A broken legacy sidecar leaves the quick bar unchanged.
bool ApplySpellAndQuick(Entity& self, char const* source, std::vector<std::string> const& spells,
                        std::vector<Quick> const& wanted, bool exactQuality);
bool ReadQuick(char const* record, std::vector<std::string>& spells, std::vector<Quick>& wanted, bool* present = nullptr)
{
    const std::string path = QuickPath(record);
    FILE* file = nullptr;
    if (present) *present = false;
    const errno_t error = fopen_s(&file, path.c_str(), "r");
    if (error || !file) {
        if (error == ENOENT) { Logf("hero: no quick slots for %s (a record of before 27.10.)", record); return true; }
        Logf("hero: quick slots %s cannot be read", path.c_str());
        return false;
    }
    if (present) *present = true;
    std::string line;
    size_t declared = 0;
    bool ended = false, malformed = false;
    char const* refused = ReadLine(file, line, &malformed) && line == "G3TQUICK 1" ? nullptr : "no header";
    while (!refused && ReadLine(file, line, &malformed)) {
        auto const words = Words(line);
        Quick q;
        if (ended) refused = "a line after the end line";
        else if (words.size() == 4 && words[0] == "quick" && Integer(words[1], q.slot) && Integer(words[3], q.quality)) {
            q.id = words[2];
            if (q.slot < 0 || q.slot >= QuickSlots || !saving::Id(q.id) || q.quality >= 1024)
                refused = "a slot out of range";
            wanted.push_back(q);
        } else if (words.size() == 2 && words[0] == "spell") {
            spells.push_back(words[1]);
            if (!saving::Id(spells.back()))
                refused = "a spell whose item is no template ID";
        } else if (words.size() == 2 && words[0] == "end" && Integer(words[1], declared)) ended = true;
        else refused = "a line that is no quick slot line";
        if (wanted.size() > QuickSlots || spells.size() > 100000) refused = "too many quick entries";
    }
    if ((malformed || ferror(file)) && !refused) refused = "the file could not be read completely";
    fclose(file);
    if (!refused && (!ended || declared != spells.size() + wanted.size())) refused = "no end line (the write broke off)";
    if (refused) { Logf("hero: quick slots %s refused (%s); the quick bar stays as it is", path.c_str(), refused); return false; }
    return true;
}

bool LoadQuick(Entity& self, char const* record)
{
    const std::string path = QuickPath(record);
    std::vector<Quick> wanted;
    std::vector<std::string> spells;
    if (!ReadQuick(record, spells, wanted)) return false;
    return ApplySpellAndQuick(self, path.c_str(), spells, wanted, false);
}

// Shared by the legacy <record>.quick.txt sidecar and G3THERO 3 inline spell/quick data. The caller validates the
// complete spell/quick set before invoking this helper, so the existing restore negative controls are preserved without
// letting a broken file mutate only part of the hero.
bool ApplySpellAndQuick(Entity& self, char const* source, std::vector<std::string> const& spells,
                        std::vector<Quick> const& wanted, bool exactQuality)
{
    unsigned learned = 0, spellsMissing = 0;
    for (std::string const& id : restoreSpells ? spells : std::vector<std::string>()) {
        int index = -1;
        for (int i = 0; i < self.Inventory.GetStackCount() && index < 0; ++i) {
            if (static_cast<int>(self.Inventory.GetCategory(i)) != gEItemCategory_Spellbook) continue;
            const Template item = self.Inventory.GetTemplate(i);
            bCPropertyID const& itemId = item;
            if (Hex(itemId) == id) index = i;
        }
        auto* inventory = static_cast<gCInventory_PS*>(self.GetInstance()->GetPropertySet(eEPropertySetType_Inventory));
        if (index >= 0 && (StackLearned(self, index) == 1 || (inventory && inventory->LearnStack(index))) &&
            StackLearned(self, index) == 1) ++learned;
        else { ++spellsMissing; Logf("hero: record %s could not restore spell %s", source, id.c_str()); }
    }
    Logf("hero: spells %s learned again: %u of %zu, %u missing%s", source, learned, spells.size(), spellsMissing,
         restoreSpells ? "" : " (spellrestoreoff)");
    if (!restoreQuick) { Logf("hero: quick slots %s not put back (quickrestoreoff)", source); return spellsMissing == 0; }
    unsigned assigned = 0, missing = 0;
    bool assignedSlots[QuickSlots] = {};
    for (Quick const& q : wanted) {
        int index = -1;
        for (int pass = 0; pass < (exactQuality ? 1 : 2) && index < 0; ++pass)
            for (int i = 0; i < self.Inventory.GetStackCount() && index < 0; ++i) {
                const Template item = self.Inventory.GetTemplate(i);
                bCPropertyID const& id = item;
                const int oldSlot = StackQuickSlot(self, i);
                if (oldSlot >= 0 && oldSlot < QuickSlots && assignedSlots[oldSlot]) continue;
                if (Hex(id) == q.id && (pass == 1 || self.Inventory.GetQuality(i) == q.quality)) index = i;
            }
        if (index >= 0 && self.Inventory.AssignStackToQuickSlot(index, q.slot) &&
            self.Inventory.FindStackIndexByQuickSlot(q.slot) == index && StackQuickSlot(self, index) == q.slot) {
            ++assigned;
            assignedSlots[q.slot] = true;
        }
        else { ++missing; Logf("hero: record %s could not restore quick slot %d item=%s quality=%u", source, q.slot, q.id.c_str(), q.quality); }
    }
    if (exactQuality) {
        const std::vector<Quick> actual = QuickSlotsOf(self);
        if (actual.size() != wanted.size()) {
            ++missing;
        } else for (Quick const& q : wanted)
            if (std::none_of(actual.begin(), actual.end(), [&q](Quick const& a) {
                    return a.slot == q.slot && a.id == q.id && a.quality == q.quality;
                })) ++missing;
    }
    Logf("hero: quick slots %s put back: %u, %u missing", source, assigned, missing);
    return spellsMissing == 0 && missing == 0;
}
}

// Written to <path>.tmp and moved over the old record only once every line and the close went through, with an end line
// that names the number of stacks (Codex review 22.10. R1: "w" emptied the record at once, and a write that broke off left
// a record whose next load wiped the inventory and set level and experience to zero).
bool SaveRecordImpl(char const* path, saving::Key const* key)
{
    if (!path || !*path || (key && !saving::Valid(*key))) {
        Logf("hero: record not written (invalid path or identity)");
        return false;
    }
    eCDynamicEntity* hero = Hero();
    if (!Ready(hero)) { Logf("hero: record %s not written (hero not ready)", path); return false; }
    saving::RecordPose pose;
    if (key) {
        const bCVector& p = hero->GetWorldPosition();
        const bCQuaternion q = hero->GetWorldRotation();
        pose = {{p.GetX(), p.GetY(), p.GetZ()}, {q.GetX(), q.GetY(), q.GetZ(), q.GetW()}};
        if (!saving::ValidRecordPose(pose)) { Logf("hero: record %s not written (invalid pose)", path); return false; }
    }
    Entity self(hero);
    auto* memory = static_cast<gCPlayerMemory_PS*>(hero->GetPropertySet(eEPropertySetType_PlayerMemory));
    const std::vector<Stack> stacks = Stacks(self);
    const std::vector<std::string> spells = LearnedSpellsOf(self);
    const std::vector<Quick> quick = QuickSlotsOf(self);
    std::vector<saving::RecordAttribute> attrs;
    if (key) {
        char const* invalid = ValidateContents(stacks, spells, quick);
        const unsigned level = static_cast<unsigned>(self.NPC.Level), levelMax = static_cast<unsigned>(self.NPC.LevelMax);
        const int xp = static_cast<int>(self.PlayerMemory.XP), lpa = static_cast<int>(self.PlayerMemory.LPAttribs),
                  lpp = static_cast<int>(self.PlayerMemory.LPPerks);
        if (!invalid && (level > 1000 || levelMax > 1000 || xp < 0 || xp > 100000000 ||
                         lpa < 0 || lpa > 100000 || lpp < 0 || lpp > 100000)) invalid = "stats out of range";
        if (!CaptureAttributes(*memory, attrs)) invalid = "an attribute missing or out of range";
        if (invalid) { Logf("hero: record %s not written (%s)", path, invalid); return false; }
    }
    const std::string temp = std::string(path) + ".tmp";
    FILE* file = nullptr;
    if (fopen_s(&file, temp.c_str(), "w") || !file) { Logf("hero: record %s not written (cannot open temporary file)", path); return false; }
    bool ok = fprintf(file, "G3THERO %d\n", key ? 4 : 3) > 0;
    if (key) {
        ok = ok && fprintf(file, "identity %s %s %llu\n", key->world.c_str(), key->save.c_str(),
                           static_cast<unsigned long long>(key->player)) > 0;
        ok = ok && fprintf(file, "pose %.9g %.9g %.9g %.9g %.9g %.9g %.9g\n", pose.position[0], pose.position[1], pose.position[2],
                           pose.rotation[0], pose.rotation[1], pose.rotation[2], pose.rotation[3]) > 0;
    }
    ok = ok && fprintf(file, "downed %d\n", downed::Held() ? 1 : 0) > 0;
    ok = ok && fprintf(file, "level %u %u\n", static_cast<unsigned>(self.NPC.Level), static_cast<unsigned>(self.NPC.LevelMax)) > 0;
    ok = ok && fprintf(file, "xp %d\nlp %d %d\n", static_cast<int>(self.PlayerMemory.XP), static_cast<int>(self.PlayerMemory.LPAttribs),
                       static_cast<int>(self.PlayerMemory.LPPerks)) > 0;
    if (key) {
        for (auto const& attr : attrs)
            ok = ok && (saving::StatTag(attr.tag)
                            ? fprintf(file, "attr %s %d %d %d %d\n", attr.tag.c_str(), attr.base, attr.modifier,
                                      attr.baseMaximum, attr.maximumModifier)
                            : fprintf(file, "attr %s %d %d\n", attr.tag.c_str(), attr.base, attr.modifier)) > 0;
    } else for (char const* tag : Tags)
        ok = ok && fprintf(file, "attr %s %d %d\n", tag, self.PlayerMemory.GetBaseValue(bCString(tag)), self.PlayerMemory.GetMaximum(bCString(tag))) > 0;
    for (Stack const& s : stacks)
        ok = ok && (s.category == gEItemCategory_Skill
                        ? fprintf(file, "stack %s %d %u %d %d %d\n", s.id.c_str(), s.amount, s.quality, s.slot, s.category, s.learned)
                        : fprintf(file, "stack %s %d %u %d %d\n", s.id.c_str(), s.amount, s.quality, s.slot, s.category)) > 0;
    for (std::string const& id : spells)
        ok = ok && fprintf(file, "spell %s\n", id.c_str()) > 0;
    for (Quick const& q : quick)
        ok = ok && fprintf(file, "quick %d %s %u\n", q.slot, q.id.c_str(), q.quality) > 0;
    ok = ok && fprintf(file, "quick_end %zu %zu\n", spells.size(), quick.size()) > 0;
    ok = ok && fprintf(file, "end %zu\n", stacks.size()) > 0;
    ok = fflush(file) == 0 && ok;
    ok = _commit(_fileno(file)) == 0 && ok;
    ok = fclose(file) == 0 && ok;
    if (ok && writeFault) ok = writeFault = false;  // QA (stage W2): the write breaks off before the record is replaced
    if (ok) ok = MoveFileExA(temp.c_str(), path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
    if (!ok) {
        DeleteFileA(temp.c_str());
        Logf("hero: record %s not written; the one before stays", path);
    }
    if (ok && !key) {
        const bool quickOk = SaveQuick(self, path);
        if (!quickOk) {
            ok = false;
            Logf("hero: record %s main data is complete but the quick compatibility sidecar failed; retry is required", path);
        }
    }
    if (ok && key)
        Logf("hero: bound record written world=%s save=%s player=%llu level=%u xp=%d lp=%d/%d str=%d dex=%d int=%d hp=%d/%d/%d mp=%d/%d/%d sp=%d/%d/%d stacks=%zu spells=%zu quick=%zu pose=%.9g,%.9g,%.9g rotation=%.9g,%.9g,%.9g,%.9g",
             key->world.c_str(), key->save.c_str(), static_cast<unsigned long long>(key->player),
             static_cast<unsigned>(self.NPC.Level), static_cast<int>(self.PlayerMemory.XP),
             static_cast<int>(self.PlayerMemory.LPAttribs), static_cast<int>(self.PlayerMemory.LPPerks),
             self.PlayerMemory.GetBaseValue(bCString("STR")), self.PlayerMemory.GetBaseValue(bCString("DEX")),
             self.PlayerMemory.GetBaseValue(bCString("INT")),
             self.PlayerMemory.GetHitPoints(), self.PlayerMemory.GetBaseValue(bCString("HP")), self.PlayerMemory.GetMaximum(bCString("HP")),
             self.PlayerMemory.GetManaPoints(), self.PlayerMemory.GetBaseValue(bCString("MP")), self.PlayerMemory.GetMaximum(bCString("MP")),
             self.PlayerMemory.GetStaminaPoints(), self.PlayerMemory.GetBaseValue(bCString("SP")), self.PlayerMemory.GetMaximum(bCString("SP")),
             stacks.size(), spells.size(), quick.size(), pose.position[0], pose.position[1], pose.position[2],
             pose.rotation[0], pose.rotation[1], pose.rotation[2], pose.rotation[3]);
    return ok;
}

bool SaveRecord(char const* path) { return SaveRecordImpl(path, nullptr); }
bool SaveRecord(char const* path, saving::Key const& key) { return SaveRecordImpl(path, &key); }

bool ReadRecordKey(char const* path, saving::Key& key)
{
    FILE* file = nullptr;
    std::string header, identity, poseLine;
    saving::Key candidate;
    saving::RecordPose pose;
    if (!path || !*path || fopen_s(&file, path, "r") || !file) return false;
    const bool ok = ReadLine(file, header) && ReadLine(file, identity) && ReadLine(file, poseLine) &&
                    saving::ParseRecordMetadata(header, identity, poseLine, nullptr, candidate, pose);
    fclose(file);
    if (ok) key = candidate;
    return ok;
}

bool LoadRecordImpl(char const* path, saving::Key const* expected, bool strictLegacy, bool validateOnly = false)
{
    eCDynamicEntity* hero = Hero();
    FILE* file = nullptr;
    lastRefusal = nullptr;
    if (!validateOnly && !Ready(hero)) { lastRefusal = "hero not ready"; Logf("hero: record not applied (hero not ready)"); return false; }
    if (!path || !*path || (expected && !saving::Valid(*expected))) {
        lastRefusal = "invalid path or identity";
        Logf("hero: record not applied (%s)", lastRefusal);
        return false;
    }
    if (strictLegacy && !saving::StrictLegacyReady(restoring, checking, restoreSpells, restoreQuick, replaceSkills)) {
        lastRefusal = "a required legacy restore component is disabled";
        Logf("hero: record %s refused (%s); the hero stays as it is", path, lastRefusal);
        return false;
    }
    if (fopen_s(&file, path, "r") || !file) {
        lastRefusal = "the file cannot be read";
        Logf("hero: record %s refused (%s)", path, lastRefusal);
        return false;
    }
    char line[256];
    if (!fgets(line, sizeof(line), file) || std::strncmp(line, "G3THERO ", 8) ||
        (line[8] != '1' && line[8] != '2' && line[8] != '3' && line[8] != '4') ||
        (line[9] != '\n' && line[9] != '\r' && line[9] != '\0')) {
        fclose(file);
        lastRefusal = "no hero record header";
        Logf("hero: record %s refused (%s); the hero stays as it is", path, lastRefusal);
        return false;
    }
    const int version = line[8] - '0';
    saving::RecordPose pose;
    saving::Key actual;
    if ((version == 4) != (expected != nullptr)) {
        fclose(file);
        lastRefusal = expected ? "bound load requires a v4 record" : "v4 record requires an identity";
        Logf("hero: record %s refused (%s); the hero stays as it is", path, lastRefusal);
        return false;
    }
    if (strictLegacy && (line[9] != '\n' || line[10] != '\0')) {
        fclose(file);
        lastRefusal = "invalid legacy record header";
        Logf("hero: record %s refused (%s); the hero stays as it is", path, lastRefusal);
        return false;
    }
    if (version == 4) {
        std::string identity, poseLine;
        if (std::strcmp(line, "G3THERO 4\n") != 0 || !ReadLine(file, identity) || !ReadLine(file, poseLine) ||
            !saving::ParseRecordMetadata(std::string(line, 9), identity, poseLine, expected, actual, pose)) {
            fclose(file);
            lastRefusal = "invalid or mismatched identity or pose";
            Logf("hero: record %s refused (%s); the hero stays as it is", path, lastRefusal);
            return false;
        }
        if (!restoring || !restoreSpells || !restoreQuick || !replaceSkills) {
            fclose(file);
            lastRefusal = "a restore component is disabled";
            Logf("hero: record %s refused (%s); the hero stays as it is", path, lastRefusal);
            return false;
        }
    } else if (!restoring) { fclose(file); return true; }
    // The whole record is read and checked before the hero changes (R1): a record that broke off, a line that is no
    // record line, a missing field or a value out of range leaves the hero as it is. Version 1 records (before 22.10.)
    // have no end line; they pass when every field is there.
    std::vector<Stack> wanted;
    unsigned level = 0, levelMax = 0;
    int xp = 0, lpa = 0, lpp = 0;
    std::vector<saving::RecordAttribute> attrs;
    bool haveLevel = false, haveXp = false, haveLp = false, ended = false;
    bool haveDowned = false;
    int savedDowned = 0;
    bool inlineQuickEnded = false;
    std::vector<std::string> inlineSpells;
    std::vector<Quick> inlineQuick;
    size_t declaredInlineSpells = 0, declaredInlineQuick = 0;
    char const* refused = nullptr;
    size_t declared = 0;
    if (version == 4 || strictLegacy) {
        std::string text;
        bool malformed = false;
        while (!refused && ReadLine(file, text, &malformed)) {
            auto const words = Words(text);
            Stack s;
            Quick q;
            int base = 0, max = 0;
            saving::RecordAttribute attr;
            if (ended || words.empty()) refused = "a line after the end or an empty line";
            else if (inlineQuickEnded) {
                if (words.size() == 2 && words[0] == "end" && Integer(words[1], declared)) ended = true;
                else refused = "a line after quick_end";
            } else if (words.size() == 3 && words[0] == "level" && !haveLevel &&
                       Integer(words[1], level) && Integer(words[2], levelMax)) haveLevel = true;
            else if (words.size() == 2 && words[0] == "xp" && !haveXp && Integer(words[1], xp)) haveXp = true;
            else if (words.size() == 3 && words[0] == "lp" && !haveLp &&
                     Integer(words[1], lpa) && Integer(words[2], lpp)) haveLp = true;
            else if (words[0] == "attr" &&
                     (version == 4 ? saving::ParseRecordAttribute(text, attr)
                                   : words.size() == 4 && Integer(words[2], base) && Integer(words[3], max) &&
                                     saving::LegacyRecordAttribute(words[1], base, max, attr))) attrs.push_back(attr);
            else if ((words.size() == 6 || words.size() == 7) && words[0] == "stack" &&
                     Integer(words[2], s.amount) && Integer(words[3], s.quality) &&
                     Integer(words[4], s.slot) && Integer(words[5], s.category) &&
                     (words.size() == 6 || Integer(words[6], s.learned)) &&
                     (version != 4 || ((s.category == gEItemCategory_Skill) == (words.size() == 7)))) {
                s.id = words[1];
                wanted.push_back(s);
            } else if (saving::ParseRecordDowned(text, version, haveDowned, savedDowned)) haveDowned = true;
            else if (version >= 3 && words.size() == 2 && words[0] == "spell") inlineSpells.push_back(words[1]);
            else if (version >= 3 && words.size() == 4 && words[0] == "quick" &&
                     Integer(words[1], q.slot) && Integer(words[3], q.quality)) {
                q.id = words[2];
                inlineQuick.push_back(q);
            } else if (version >= 3 && words.size() == 3 && words[0] == "quick_end" &&
                       Integer(words[1], declaredInlineSpells) && Integer(words[2], declaredInlineQuick)) inlineQuickEnded = true;
            else if (version < 3 && words.size() == 2 && words[0] == "end" && Integer(words[1], declared)) ended = true;
            else refused = "a line that is no record line";
            if (wanted.size() > 100000 || inlineSpells.size() > 100000 || inlineQuick.size() > QuickSlots ||
                attrs.size() > sizeof(Tags) / sizeof(Tags[0])) refused = "too many record entries";
        }
        if (malformed && !refused) refused = "a damaged or unreadable line";
    } else while (!refused && fgets(line, sizeof(line), file)) {
        char a[64] = {};
        Stack s;
        int b = 0, c = 0;
        if (ended) refused = "a line after the end line";
        else if (!std::strncmp(line, "downed ", 7)) {
            char extra = 0;
            if (version != 3 || haveDowned || sscanf_s(line, "downed %d %c", &savedDowned, &extra, 1u) != 1 ||
                (savedDowned != 0 && savedDowned != 1)) refused = "invalid downed state";
            haveDowned = true;
        } else if (version == 3 && !inlineQuickEnded && sscanf_s(line, "spell %63s", a, 64u) == 1) inlineSpells.push_back(a);
        else if (version == 3) {
            Quick q;
            if (inlineQuickEnded) {
                if (sscanf_s(line, "end %zu", &declared) == 1) ended = true;
                else refused = "a line after quick_end";
            } else if (sscanf_s(line, "quick %d %63s %u", &q.slot, a, 64u, &q.quality) == 3) {
                q.id = a;
                inlineQuick.push_back(q);
            } else if (sscanf_s(line, "quick_end %zu %zu", &declaredInlineSpells, &declaredInlineQuick) == 2) {
                if (inlineQuickEnded) refused = "a second quick_end line";
                inlineQuickEnded = true;
            } else if (sscanf_s(line, "level %u %u", &level, &levelMax) == 2) haveLevel = true;
            else if (sscanf_s(line, "xp %d", &xp) == 1) haveXp = true;
            else if (sscanf_s(line, "lp %d %d", &lpa, &lpp) == 2) haveLp = true;
            else if (sscanf_s(line, "attr %63s %d %d", a, 64u, &b, &c) == 3) {
                saving::RecordAttribute attr;
                if (saving::LegacyRecordAttribute(a, b, c, attr)) attrs.push_back(attr);
                else refused = "an attribute out of range";
            }
            else if (const int fields = std::strncmp(line, "stack ", 6) ? 0
                                        : sscanf_s(line, "stack %63s %d %u %d %d %d", a, 64u, &s.amount, &s.quality, &s.slot, &s.category, &s.learned);
                     fields == 5 || fields == 6) {
                s.id = a;
                wanted.push_back(s);
            } else if (sscanf_s(line, "end %zu", &declared) == 1) {
                if (!inlineQuickEnded) refused = "quick_end missing before end";
                else ended = true;
            } else refused = "a line that is no v3 record line";
        } else if (sscanf_s(line, "level %u %u", &level, &levelMax) == 2) haveLevel = true;
        else if (sscanf_s(line, "xp %d", &xp) == 1) haveXp = true;
        else if (sscanf_s(line, "lp %d %d", &lpa, &lpp) == 2) haveLp = true;
        else if (sscanf_s(line, "attr %63s %d %d", a, 64u, &b, &c) == 3) {
            saving::RecordAttribute attr;
            if (saving::LegacyRecordAttribute(a, b, c, attr)) attrs.push_back(attr);
            else refused = "an attribute out of range";
        }
        else if (const int fields = std::strncmp(line, "stack ", 6) ? 0
                                    : sscanf_s(line, "stack %63s %d %u %d %d %d", a, 64u, &s.amount, &s.quality, &s.slot, &s.category, &s.learned);
                 fields == 5 || fields == 6) {  // a sixth field: the skill's learned state (23.10.)
            s.id = a;
            wanted.push_back(s);
        } else if (sscanf_s(line, "end %zu", &declared) == 1) ended = true;
        else refused = "a line that is no record line";
    }
    if (ferror(file) && !refused) refused = "the file could not be read completely";
    fclose(file);
    if (!refused && (checking || version == 4 || strictLegacy)) {
        if (!haveLevel || !haveXp || !haveLp) refused = "level, experience or learn points missing";
        else if ((version >= 2 || strictLegacy) && !ended) refused = "no end line (the record cannot be verified)";
        else if (ended && declared != wanted.size()) refused = "fewer stacks than the end line names";
        else if (version >= 3 && !inlineQuickEnded) refused = "no quick_end line";
        else if (version >= 3 && (declaredInlineSpells != inlineSpells.size() || declaredInlineQuick != inlineQuick.size()))
            refused = "the quick_end counts do not match the inline spell and quick lines";
        else if (level > 1000 || levelMax > 1000 || xp < 0 || xp > 100000000 || lpa < 0 || lpa > 100000 || lpp < 0 || lpp > 100000)
            refused = "level, experience or learn points out of range";
        for (char const* tag : Tags) {
            if (refused) break;
            if (std::count_if(attrs.begin(), attrs.end(), [tag](saving::RecordAttribute const& at) { return at.tag == tag; }) != 1) refused = "an attribute missing or twice";
        }
        if (!refused && attrs.size() != sizeof(Tags) / sizeof(Tags[0])) refused = "an attribute this record does not know";
        for (auto const& at : attrs)
            if (!refused && !saving::ValidRecordAttribute(at)) refused = "an attribute out of range";
        if (!refused && version >= 3) {
            for (std::string const& id : inlineSpells)
                if (id.size() != 32 || id.find_first_not_of("0123456789abcdef") != std::string::npos)
                    refused = "a spell whose item is no template ID";
            for (Quick const& q : inlineQuick)
                if (!refused && (q.slot < 0 || q.slot >= QuickSlots || q.id.size() != 32 ||
                                 q.id.find_first_not_of("0123456789abcdef") != std::string::npos))
                    refused = "a quick slot out of range";
        }
        for (Stack const& s : wanted) {
            if (refused) break;
            if (s.id.size() != 32 || s.id.find_first_not_of("0123456789abcdef") != std::string::npos ||
                (!validateOnly && !FromHex(s.id).IsValid()))
                refused = "a stack whose item does not exist";
            else if (s.amount < 1 || s.amount > 1000000 || s.quality >= 1024 || s.slot < gESlot_None || s.slot > gESlot_Helmet ||
                     s.category < 0 || s.category >= gEItemCategory_Count || s.learned < -1 || s.learned > 1 ||
                     (s.learned != -1 && s.category != gEItemCategory_Skill))
                refused = "a stack out of range";
            else if (strictLegacy && s.category == gEItemCategory_Skill && s.learned < 0)
                refused = "legacy skill state was not recorded";
        }
        if (!refused && strictLegacy && version < 3) {
            bool present = false;
            if (!ReadQuick(path, inlineSpells, inlineQuick, &present)) refused = "legacy quick slots could not be read";
            else if (!present && !wanted.empty()) refused = "legacy spell and quick state was not recorded";
        }
        if (!refused && (version == 4 || strictLegacy)) refused = ValidateContents(wanted, inlineSpells, inlineQuick, !validateOnly);
        if (!refused && !validateOnly && (version == 4 || strictLegacy)) {
            auto* memory = static_cast<gCPlayerMemory_PS*>(hero->GetPropertySet(eEPropertySetType_PlayerMemory));
            for (auto const& at : attrs) {
                gCAttribute* attribute = memory->GetAttribute(bCString(at.tag.c_str()));
                if (!attribute || (saving::StatTag(at.tag) && !dynamic_cast<gCStat*>(attribute))) {
                    refused = "a hero attribute is missing or has the wrong type";
                    break;
                }
            }
        }
    }
    if (refused) {
        lastRefusal = refused;
        Logf("hero: record %s refused (%s); the hero stays as it is", path, refused);
        return false;
    }
    if (validateOnly) {
        Logf("hero: record validated version=%d world=%s save=%s player=%llu stacks=%zu spells=%zu quick=%zu; native template checks deferred",
             version, actual.world.c_str(), actual.save.c_str(), actual.player, wanted.size(), inlineSpells.size(), inlineQuick.size());
        return true;
    }
    if (version < 2 && checking) Logf("hero: record %s is of version 1 (no end line); every field is there", path);
    downed::Restore(savedDowned != 0, "hero-record");
    Entity self(hero);
    // Inventory first: equipment modifies attributes, so the base values go on afterwards.
    // Skills are the record's (Codex review 22.10.: a player loaded into the host's save kept the skills of the host's hero
    // there; the old code only "taught" with the perk item's template, which did nothing): a perk stack the record does not
    // have goes, a missing one is made, and each skill is learned or unlearned as the record says (version 2 records, without
    // that field, keep the hero's state).
    std::unordered_set<std::string> recordSkills;
    for (Stack const& s : wanted)
        if (s.category == gEItemCategory_Skill) recordSkills.insert(s.id);
    unsigned skillsRemoved = 0, skillsTaught = 0, skillsUnlearned = 0;
    auto fail = [path](char const* reason) {
        lastRefusal = reason;
        Logf("hero: record %s could not be fully applied (%s); hero may be partially changed", path, reason);
        return false;
    };
    if (!self.Inventory.HoldStacks(-1, -1) &&
        (self.Inventory.GetItemFromSlot(gESlot_RightHand).GetInstance() || self.Inventory.GetItemFromSlot(gESlot_LeftHand).GetInstance()))
        return fail("could not release held items");
    for (int i = self.Inventory.GetStackCount() - 1; i >= 0; --i) {
        if (static_cast<int>(self.Inventory.GetCategory(i)) == gEItemCategory_Skill) {
            const Template skill = self.Inventory.GetTemplate(i);
            bCPropertyID const& id = skill;
            if (!replaceSkills || recordSkills.count(Hex(id))) continue;  // kept; missing ones are taught below
            if (!self.Inventory.DeleteStack(i)) return fail("could not remove a skill stack");
            ++skillsRemoved;
            continue;
        }
        if (self.Inventory.GetSlotFromStack(i) != gESlot_None && !self.Inventory.UnEquipStack(i))
            return fail("could not unequip an item");
        if (!self.Inventory.DeleteStack(i)) return fail("could not remove an item");
    }
    for (Stack const& s : wanted) {
        const Template item = FromHex(s.id);
        if (s.category == gEItemCategory_Skill) {
            if (!replaceSkills) {  // QA negative control: as before 23.10.
                if (!self.Inventory.HasLearnedSkill(item)) {
                    if (!self.Inventory.TeachSkill(item)) return fail("could not teach a skill");
                    ++skillsTaught;
                }
                continue;
            }
            int index = FindSkillStack(self, s.id);
            if ((version == 4 || strictLegacy) && index >= 0 &&
                (self.Inventory.GetQuality(index) != s.quality || self.Inventory.GetAmount(index) != s.amount)) {
                if (!self.Inventory.DeleteStack(index)) return fail("could not replace a skill stack");
                index = -1;
            }
            if (index < 0) index = self.Inventory.CreateItems(item, (version == 4 || strictLegacy) ? s.quality : 0,
                                                              (version == 4 || strictLegacy) ? s.amount : 1);
            if (index < 0) { Logf("hero: record %s could not create skill item %s", path, s.id.c_str()); return fail("could not create a skill item"); }
            const Template skill = SkillOf(self, index);
            if (!skill.IsValid()) { Logf("hero: record %s missing skill for item %s", path, s.id.c_str()); return fail("a skill template is missing"); }
            if (s.learned < 0) continue;
            const bool knows = self.Inventory.HasLearnedSkill(skill) != GEFalse;
            if (s.learned == 1 && !knows) {
                if (!self.Inventory.TeachSkill(skill) || !self.Inventory.HasLearnedSkill(skill))
                    return fail("could not teach a skill");
                ++skillsTaught;
            } else if (s.learned == 0 && knows) {
                if (!self.Inventory.DeleteStack(index)) return fail("could not unlearn a skill");
                index = self.Inventory.CreateItems(item, (version == 4 || strictLegacy) ? s.quality : 0,
                                                   (version == 4 || strictLegacy) ? s.amount : 1);
                if (index < 0 || self.Inventory.HasLearnedSkill(skill)) return fail("could not recreate an unlearned skill");
                ++skillsUnlearned;
            }
            if ((version == 4 || strictLegacy) && s.slot != gESlot_None &&
                !self.Inventory.EquipStackToSlot(index, static_cast<gESlot>(s.slot))) return fail("could not equip a skill item");
            continue;
        }
        const GEInt index = self.Inventory.CreateItems(item, s.quality, s.amount);
        if (index < 0) { Logf("hero: record %s could not create item %s", path, s.id.c_str()); return fail("could not create an item"); }
        if (s.slot != gESlot_None && !self.Inventory.EquipStackToSlot(index, static_cast<gESlot>(s.slot)))
            return fail("could not equip an item");
    }
    if ((version == 4 || strictLegacy) && !SameStacks(Stacks(self), wanted)) return fail("restored inventory does not match the record");
    self.NPC.Level = level;
    self.NPC.LevelMax = levelMax;
    self.PlayerMemory.XP = xp;
    self.PlayerMemory.LPAttribs = lpa;
    self.PlayerMemory.LPPerks = lpp;
    auto* memory = static_cast<gCPlayerMemory_PS*>(hero->GetPropertySet(eEPropertySetType_PlayerMemory));
    if (!ApplyAttributes(*memory, attrs)) return fail("an attribute could not be restored");
    if (downed::Held()) self.PlayerMemory.SetBaseValue(bCString("HP"), 1);
    if (strictLegacy) Logf("hero: legacy record %s restored with inferred static modifiers and unsplit stat maxima", path);
    if (!((version >= 3 || strictLegacy)
              ? ApplySpellAndQuick(self, path, inlineSpells, inlineQuick, version == 4 || strictLegacy)
              : LoadQuick(self, path)))
        return fail("spells or quick slots could not be restored");
    if ((version == 4 || strictLegacy) &&
        (static_cast<unsigned>(self.NPC.Level) != level || static_cast<unsigned>(self.NPC.LevelMax) != levelMax ||
         static_cast<int>(self.PlayerMemory.XP) != xp || static_cast<int>(self.PlayerMemory.LPAttribs) != lpa ||
         static_cast<int>(self.PlayerMemory.LPPerks) != lpp || !AttributesMatch(*memory, attrs, downed::Held())))
        return fail("restored character stats do not match the record");
    if (version == 4) {
        self.MoveTo(body::Pose(*hero, pose.position, pose.rotation));
        const bCVector& p = hero->GetWorldPosition();
        const bCQuaternion q = hero->GetWorldRotation();
        const float dx = p.GetX() - pose.position[0], dy = p.GetY() - pose.position[1], dz = p.GetZ() - pose.position[2];
        const float dot = q.GetX() * pose.rotation[0] + q.GetY() * pose.rotation[1] +
                          q.GetZ() * pose.rotation[2] + q.GetW() * pose.rotation[3];
        if (!std::isfinite(dx) || !std::isfinite(dy) || !std::isfinite(dz) || dx * dx + dy * dy + dz * dz > 1.0f ||
            !std::isfinite(dot) || std::fabs(dot) < 0.99f) return fail("pose could not be restored");
        Logf("hero: bound record applied world=%s save=%s player=%llu level=%u xp=%d lp=%d/%d str=%d dex=%d int=%d hp=%d/%d/%d mp=%d/%d/%d sp=%d/%d/%d stacks=%zu spells=%zu quick=%zu pose=%.9g,%.9g,%.9g rotation=%.9g,%.9g,%.9g,%.9g",
             actual.world.c_str(), actual.save.c_str(), static_cast<unsigned long long>(actual.player), level, xp, lpa, lpp,
             self.PlayerMemory.GetBaseValue(bCString("STR")), self.PlayerMemory.GetBaseValue(bCString("DEX")),
             self.PlayerMemory.GetBaseValue(bCString("INT")),
             self.PlayerMemory.GetHitPoints(), self.PlayerMemory.GetBaseValue(bCString("HP")), self.PlayerMemory.GetMaximum(bCString("HP")),
             self.PlayerMemory.GetManaPoints(), self.PlayerMemory.GetBaseValue(bCString("MP")), self.PlayerMemory.GetMaximum(bCString("MP")),
             self.PlayerMemory.GetStaminaPoints(), self.PlayerMemory.GetBaseValue(bCString("SP")), self.PlayerMemory.GetMaximum(bCString("SP")),
             wanted.size(), inlineSpells.size(), inlineQuick.size(), p.GetX(), p.GetY(), p.GetZ(), q.GetX(), q.GetY(), q.GetZ(), q.GetW());
    }
    Logf("hero: record %s applied: %zu skills in it, %u taught, %u unlearned, %u removed", path, recordSkills.size(), skillsTaught,
         skillsUnlearned, skillsRemoved);
    return true;
}

bool LoadRecord(char const* path) { return LoadRecordImpl(path, nullptr, false); }
bool LoadLegacyRecordStrict(char const* path) { return LoadRecordImpl(path, nullptr, true); }
bool LoadRecord(char const* path, saving::Key const& key) { return LoadRecordImpl(path, &key, false); }
bool ValidateRecord(char const* path, saving::Key const& key) { return LoadRecordImpl(path, &key, false, true); }
bool ValidateLegacyRecord(char const* path) { return LoadRecordImpl(path, nullptr, true, true); }

void Dump(LogFn log)
{
    eCDynamicEntity* hero = Hero();
    if (!hero) { log("HERODUMP none"); return; }
    Entity self(hero);
    std::string text;
    char part[160];
    Format(part, "level=%u xp=%d lp=%d/%d", static_cast<unsigned>(self.NPC.Level), static_cast<int>(self.PlayerMemory.XP),
              static_cast<int>(self.PlayerMemory.LPAttribs), static_cast<int>(self.PlayerMemory.LPPerks));
    text += part;
    for (char const* tag : Tags) {
        Format(part, " %s=%d/%d", tag, self.PlayerMemory.GetBaseValue(bCString(tag)), self.PlayerMemory.GetMaximum(bCString(tag)));
        text += part;
    }
    Format(part, " named=STR%d,DEX%d,INT%d,HP%d/%d", self.PlayerMemory.GetStrength(), self.PlayerMemory.GetDexterity(),
              self.PlayerMemory.GetIntelligence(), self.PlayerMemory.GetHitPoints(), self.PlayerMemory.GetHitPointsMax());
    text += part;
    unsigned long long hash = 1469598103934665603ull;
    int count = 0;
    for (Stack const& s : Stacks(self)) {
        Format(part, "%s:%d:%u:%d:%d;", s.id.c_str(), s.amount, s.quality, s.slot, s.learned);
        for (char const* c = part; *c; ++c) { hash ^= static_cast<unsigned char>(*c); hash *= 1099511628211ull; }
        ++count;
    }
    Format(part, " stacks=%d inventory=%016llx quick=", count, hash);
    text += part;
    for (Quick const& q : QuickSlotsOf(self)) {
        const GEInt i = self.Inventory.FindStackIndexByQuickSlot(q.slot);
        Entity item = self.Inventory.GetTemplateItem(i);
        Format(part, "%d:%.48s,", q.slot, item.GetInstance() ? item.GetName().GetText() : "?");
        text += part;
    }
    log(("HERODUMP " + text).c_str());
}

char const* LastRefusal() { return lastRefusal; }

std::string RecordPath(char const* name)
{
    std::string folder = gCWorld::GetSaveGameFolder().GetText();
    if (!folder.empty() && folder.back() != '\\' && folder.back() != '/') folder += '\\';
    return folder + "g3t-hero-" + name + ".txt";
}

void SaveCommand(char const* name, LogFn log)
{
    char line[300];
    const std::string path = RecordPath(name);
    Format(line, "HEROSAVE name=%s ok=%d", name, SaveRecord(path.c_str()) ? 1 : 0);
    log(line);
}

void LoadCommand(char const* name, LogFn log)
{
    char line[300];
    const std::string path = RecordPath(name);
    Format(line, "HEROLOAD name=%s ok=%d restoring=%d", name, LoadRecord(path.c_str()) ? 1 : 0, restoring ? 1 : 0);
    log(line);
}

void RestoreOff(LogFn log)
{
    restoring = false;
    log("HERORESTOREOFF ok=1");
}

// QA (stage W2): the record <name> as a write that broke off leaves it: cut in the middle of its stack lines, or with
// "<name>_header" after its first line (the bot's arguments hold no spaces).
void CutCommand(char const* argument, LogFn log)
{
    std::string name = argument;
    const bool header = name.size() > 7 && !name.compare(name.size() - 7, 7, "_header");
    if (header) name.resize(name.size() - 7);
    const std::string path = RecordPath(name.c_str());
    std::vector<std::string> lines;
    FILE* file = nullptr;
    if (!fopen_s(&file, path.c_str(), "r") && file) {
        char text[256];
        while (fgets(text, sizeof(text), file)) lines.emplace_back(text);
        fclose(file);
    }
    size_t first = 0, stacks = 0;
    for (size_t i = 0; i < lines.size(); ++i)
        if (!lines[i].compare(0, 6, "stack ")) { if (!stacks++) first = i; }
    const size_t keep = header ? 1 : stacks ? first + stacks / 2 : lines.size() > 1 ? lines.size() - 1 : 0;
    bool ok = !lines.empty() && !fopen_s(&file, path.c_str(), "w") && file;
    if (ok) {
        for (size_t i = 0; i < keep && i < lines.size(); ++i) fputs(lines[i].c_str(), file);
        ok = fclose(file) == 0;
    }
    char line[300];
    Format(line, "HEROCUT name=%s ok=%d kept=%zu of=%zu", name.substr(0, 64).c_str(), ok ? 1 : 0, keep, lines.size());
    log(line);
}

void QuickRestoreOff(LogFn log)
{
    restoreQuick = false;
    log("QUICKRESTOREOFF ok=1 (a record leaves the quick bar empty, as before 27.10.)");
}

// QA (stageQuickLoad): "<item>-<slot>", the hero's stack of that item on that quick slot, as the inventory's key does.
void QuickAssign(char const* argument, LogFn log)
{
    char line[200];
    eCDynamicEntity* hero = Hero();
    const std::string text = argument ? argument : "";
    const size_t dash = text.rfind('-');
    if (!hero || dash == std::string::npos || dash == 0) { log("REFUSED quickassign: no hero or not <item>-<slot>"); return; }
    const std::string item = text.substr(0, dash);
    const int slot = std::atoi(text.c_str() + dash + 1);
    Entity self(hero);
    const GEInt stack = self.Inventory.FindStackIndex(Template(item.c_str()));
    if (stack < 0) { Format(line, "REFUSED quickassign %.64s: not carried", item.c_str()); log(line); return; }
    const GEBool assigned = self.Inventory.AssignStackToQuickSlot(stack, slot);
    Format(line, "QUICKASSIGN item=%.64s stack=%d slot=%d assigned=%d found=%d raw=%d", item.c_str(), stack, slot, assigned ? 1 : 0,
           self.Inventory.FindStackIndexByQuickSlot(slot), StackQuickSlot(self, stack));
    log(line);
}

// QA diagnostic: the raw quick slot of every stack (how many stacks have each value) and what the game's lookup finds
// for 0 to 15, so the value of "no slot" is seen.
void QuickDiag(LogFn log)
{
    eCDynamicEntity* hero = Hero();
    if (!hero) { log("QUICKDIAG none"); return; }
    Entity self(hero);
    std::map<int, int> raw;
    for (int i = 0; i < self.Inventory.GetStackCount(); ++i) ++raw[StackQuickSlot(self, i)];
    std::string text = "QUICKDIAG stacks=" + std::to_string(self.Inventory.GetStackCount()) + " raw=";
    for (auto const& [value, count] : raw) text += std::to_string(value) + "x" + std::to_string(count) + ",";
    text += " lookup=";
    for (int slot = 0; slot < QuickSlots; ++slot) {
        const GEInt i = self.Inventory.FindStackIndexByQuickSlot(slot);
        if (i < 0) continue;
        Entity item = self.Inventory.GetTemplateItem(i);
        char part[96];
        Format(part, "%d:%d:%.40s:%d,", slot, i, item.GetInstance() ? item.GetName().GetText() : "?", StackQuickSlot(self, i));
        text += part;
    }
    log(text.c_str());
}

void SpellRestoreOff(LogFn log)
{
    restoreSpells = false;
    log("SPELLRESTOREOFF ok=1 (a record leaves the hero's spells unlearned, as before 27.10.)");
}

// QA (stageSpellLoad): "<It_Spell_...>" reports that spell, "<It_Spell_...>-learn" first makes the stack if the hero has
// none and learns it as a teacher does (LearnSpell on the item, the stack's LearnStack if that found no stack).
void SpellKnow(char const* argument, LogFn log)
{
    char line[300];
    eCDynamicEntity* hero = Hero();
    std::string item = argument ? argument : "";
    const bool learn = item.size() > 6 && item.compare(item.size() - 6, 6, "-learn") == 0;
    if (learn) item.resize(item.size() - 6);
    const Template spell(item.c_str());
    if (!hero || !spell.IsValid()) { Format(line, "REFUSED spellknow %.64s: no hero or no such item", item.c_str()); log(line); return; }
    Entity self(hero);
    GEInt stack = self.Inventory.FindStackIndex(spell);
    if (learn && stack < 0) stack = self.Inventory.CreateItems(spell, 0, 1);
    // HasLearnedSpell and TeachSpell take the spell behind the item (stageSpellLoad 27.10.: with It_Spell_Healing itself
    // both said 0 while the stack was learned), as the skills take the perk behind theirs.
    Entity carried = stack >= 0 ? self.Inventory.GetTemplateItem(stack) : Entity();
    Entity behind = carried.GetInstance() ? carried.Item.GetSpell() : Entity();
    const Template magic = behind.GetInstance() ? behind.GetTemplate() : Template();
    int taught = -1, stacked = -1;
    if (learn) {
        taught = magic.IsValid() && self.Inventory.TeachSpell(magic) ? 1 : 0;
        auto* inventory = static_cast<gCInventory_PS*>(self.GetInstance()->GetPropertySet(eEPropertySetType_Inventory));
        if (stack >= 0 && StackLearned(self, stack) != 1 && inventory) stacked = inventory->LearnStack(stack) ? 1 : 0;
    }
    Format(line, "SPELLKNOW item=%.64s spell=%.48s stack=%d category=%d learned=%d has=%d has_item=%d taught=%d stack_learn=%d spells_learned=%zu",
           item.c_str(), behind.GetInstance() ? behind.GetName().GetText() : "-", stack,
           stack >= 0 ? static_cast<int>(self.Inventory.GetCategory(stack)) : -1, stack >= 0 ? StackLearned(self, stack) : -1,
           magic.IsValid() && self.Inventory.HasLearnedSpell(magic) ? 1 : 0, self.Inventory.HasLearnedSpell(spell) ? 1 : 0, taught, stacked,
           LearnedSpellsOf(self).size());
    log(line);
}

void CheckOff(LogFn log)
{
    checking = false;
    log("RECORDCHECKOFF ok=1 (records are applied as read)");
}

void WriteFault(LogFn log)
{
    writeFault = true;
    log("RECORDWRITEFAULT ok=1 (the next record write breaks off)");
}

// QA diagnostic (stage W9, 23.10.): what a skill stack of the hero is. HasLearnedSkill and TeachSkill did nothing with the
// stack's own template (learned=0 for all 71; a removed skill was not taught back), so: the item's skill, and which of the
// two the inventory calls learned or active.
void SkillDiag(LogFn log)
{
    eCDynamicEntity* hero = Hero();
    if (!hero) { log("SKILLDIAG end"); return; }
    Entity self(hero);
    char line[400];
    int shown = 0;
    for (int i = 0; i < self.Inventory.GetStackCount() && shown < 4; ++i) {
        if (static_cast<int>(self.Inventory.GetCategory(i)) != gEItemCategory_Skill) continue;
        ++shown;
        const Template stack = self.Inventory.GetTemplate(i);
        Entity item = self.Inventory.GetTemplateItem(i);
        Entity skill = item.GetInstance() ? item.Item.GetSkill() : Entity();
        const Template skillTemplate = skill.GetInstance() ? skill.GetTemplate() : Template();
        bCPropertyID const& stackId = stack;
        bCPropertyID const& skillId = skillTemplate;
        Format(line, "SKILLDIAG stack=%d name=%s item=%s skill=%s same=%d learned_stack=%d learned_skill=%d active_stack=%d active_skill=%d activations=%u",
                  i, self.Inventory.GetName(i).GetText(), item.GetInstance() ? item.GetName().GetText() : "-",
                  skill.GetInstance() ? skill.GetName().GetText() : "-", Hex(stackId) == Hex(skillId) ? 1 : 0,
                  self.Inventory.HasLearnedSkill(stack) ? 1 : 0, skill.GetInstance() && self.Inventory.HasLearnedSkill(skillTemplate) ? 1 : 0,
                  self.Inventory.IsSkillActive(stack) ? 1 : 0, skill.GetInstance() && self.Inventory.IsSkillActive(skillTemplate) ? 1 : 0,
                  skill.GetInstance() ? self.Inventory.GetSkillActivationCount(skillTemplate) : 0u);
        log(line);
    }
    log("SKILLDIAG end");
}

// QA diagnostic (stage W9): how a skill is learned and unlearned. A learned one: its stack removed (still learned?), made
// again with CreateItems (learned now?), then TeachSkill. An unlearned one: TeachSkill with its stack there, then its stack
// removed and made again (unlearned by that?). One SKILLTRY line each.
void SkillTry(LogFn log)
{
    eCDynamicEntity* hero = Hero();
    if (!hero) { log("SKILLTRY none"); return; }
    Entity self(hero);
    auto skillOf = [&self](int i) {
        Entity item = self.Inventory.GetTemplateItem(i);
        Entity skill = item.GetInstance() ? item.Item.GetSkill() : Entity();
        return skill.GetInstance() ? skill.GetTemplate() : Template();
    };
    auto indexOf = [&self](Template const& stack) {
        bCPropertyID const& want = stack;
        for (int i = 0; i < self.Inventory.GetStackCount(); ++i) {
            const Template t = self.Inventory.GetTemplate(i);
            bCPropertyID const& id = t;
            if (static_cast<int>(self.Inventory.GetCategory(i)) == gEItemCategory_Skill && Hex(id) == Hex(want)) return i;
        }
        return -1;
    };
    char line[400];
    for (int want = 1; want >= 0; --want) {
        int index = -1;
        for (int i = 0; i < self.Inventory.GetStackCount() && index < 0; ++i)
            if (static_cast<int>(self.Inventory.GetCategory(i)) == gEItemCategory_Skill && skillOf(i).IsValid() &&
                (self.Inventory.HasLearnedSkill(skillOf(i)) ? 1 : 0) == want)
                index = i;
        if (index < 0) { Format(line, "SKILLTRY case=%s none", want ? "learned" : "unlearned"); log(line); continue; }
        const Template stack = self.Inventory.GetTemplate(index);
        const Template skill = skillOf(index);
        const bCString name = self.Inventory.GetName(index);
        auto learned = [&self, &skill]() { return self.Inventory.HasLearnedSkill(skill) ? 1 : 0; };
        const int before = learned();
        int teach1 = -1, afterTeach1 = -1;
        if (!want) { teach1 = self.Inventory.TeachSkill(skill) ? 1 : 0; afterTeach1 = learned(); }
        const int deleted = self.Inventory.DeleteStack(indexOf(stack)) ? 1 : 0;
        const int afterDelete = learned();
        const int created = self.Inventory.CreateItems(stack, 0, 1);
        const int afterCreate = learned();
        const int teach2 = self.Inventory.TeachSkill(skill) ? 1 : 0;
        const int afterTeach2 = learned();
        Format(line, "SKILLTRY case=%s name=%s before=%d teach1=%d after_teach1=%d deleted=%d after_delete=%d created=%d after_create=%d teach2=%d after_teach2=%d",
                  want ? "learned" : "unlearned", name.GetText(), before, teach1, afterTeach1, deleted, afterDelete, created, afterCreate, teach2,
                  afterTeach2);
        log(line);
    }
}

void SkillReplaceOff(LogFn log)
{
    replaceSkills = false;
    log("SKILLREPLACEOFF ok=1 (a record's skills are only taught on top, as before 23.10.)");
}

// QA (stage W9): the perk stack with this template ID (32 hex digits, as in a record): there or not, and whether the hero
// knows its skill; and how many skills it knows of how many perk stacks.
void HasSkill(char const* id, LogFn log)
{
    char line[200];
    eCDynamicEntity* hero = Hero();
    const std::string hex = id ? id : "";
    if (!hero || hex.size() != 32 || hex.find_first_not_of("0123456789abcdef") != std::string::npos) { log("REFUSED hasskill: no hero or bad ID"); return; }
    Entity self(hero);
    int stacks = 0, known = 0;
    for (int i = 0; i < self.Inventory.GetStackCount(); ++i) {
        if (static_cast<int>(self.Inventory.GetCategory(i)) != gEItemCategory_Skill) continue;
        ++stacks;
        const Template skill = SkillOf(self, i);
        if (skill.IsValid() && self.Inventory.HasLearnedSkill(skill)) ++known;
    }
    const int index = FindSkillStack(self, hex);
    const Template skill = index >= 0 ? SkillOf(self, index) : Template();
    Format(line, "HEROSKILL id=%s has=%d learned=%d known=%d stacks=%d", hex.c_str(), index >= 0 ? 1 : 0,
              skill.IsValid() && self.Inventory.HasLearnedSkill(skill) ? 1 : 0, known, stacks);
    log(line);
}
}
