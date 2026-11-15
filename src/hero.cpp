// SPDX-License-Identifier: GPL-3.0-or-later
#include "hero.h"
#include "hero_codec.h"

#include "body.h"
#include "downed.h"
#include "log.h"
#include "net.h"
#include "saving_policy.h"

#include <Game.h>
#include <Script.h>
#include <Windows.h>
#include <io.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_set>
#include <vector>

namespace g3t::hero {
namespace {
using codec::Quick;
using codec::QuickSlots;
using codec::Stack;

char const* lastRefusal = nullptr;

codec::Schema EngineSchema()
{
    codec::Schema s;
    s.minSlot = gESlot_None;
    s.maxSlot = gESlot_Helmet;
    s.categoryCount = gEItemCategory_Count;
    s.skillCategory = gEItemCategory_Skill;
    s.spellbookCategory = gEItemCategory_Spellbook;
    return s;
}

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

constexpr unsigned QualityMask = 1023u;

std::vector<Stack> Stacks(Entity& self)
{
    std::vector<Stack> out;
    for (int i = 0; i < self.Inventory.GetStackCount(); ++i) {
        Stack s;
        const Template item = self.Inventory.GetTemplate(i);
        bCPropertyID const& id = item;
        s.id = Hex(id);
        s.amount = self.Inventory.GetAmount(i);
        s.quality = self.Inventory.GetQuality(i) & QualityMask;
        s.slot = static_cast<int>(self.Inventory.GetSlotFromStack(i));
        if (s.amount <= 0 && s.slot == gESlot_None) continue;
        s.category = static_cast<int>(self.Inventory.GetCategory(i));
        if (s.category == gEItemCategory_Skill) {
            const Template skill = SkillOf(self, i);
            s.learned = skill.IsValid() && self.Inventory.HasLearnedSkill(skill) ? 1 : 0;
        }
        out.push_back(s);
    }
    std::sort(out.begin(), out.end(),
              [](Stack const& a, Stack const& b) { return a.id != b.id ? a.id < b.id : a.slot < b.slot; });
    return out;
}

eCDynamicEntity* Hero() { return gCSession::GetInstance().GetHero(); }

bool Ready(eCDynamicEntity* hero)
{
    return hero && hero->GetPropertySet(eEPropertySetType_NPC) &&
           hero->GetPropertySet(eEPropertySetType_Inventory) &&
           hero->GetPropertySet(eEPropertySetType_PlayerMemory);
}

bool CaptureAttributes(gCPlayerMemory_PS& memory, std::vector<saving::RecordAttribute>& attrs)
{
    size_t tagCount = 0;
    char const* const* tags = codec::AttributeTags(tagCount);
    for (size_t i = 0; i < tagCount; ++i) {
        const bCString name(tags[i]);
        gCAttribute* attribute = memory.GetAttribute(name);
        if (!attribute) return false;
        saving::RecordAttribute value;
        value.tag = tags[i];
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
            if (!stat || stat->GetBaseMaximum() != value.baseMaximum ||
                stat->GetMaximumModifier() != value.maximumModifier ||
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

int StackQuickSlot(Entity& self, int i)
{
    auto* inventory = static_cast<gCInventory_PS*>(self.GetInstance()->GetPropertySet(eEPropertySetType_Inventory));
    gCInventoryStack const* stack = inventory ? inventory->GetStack(i) : nullptr;
    return stack ? stack->GetQuickSlot() : -1;
}

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
        if (static_cast<int>(self.Inventory.GetCategory(i)) != gEItemCategory_Spellbook ||
            StackLearned(self, i) != 1) continue;
        const Template item = self.Inventory.GetTemplate(i);
        bCPropertyID const& id = item;
        out.push_back(Hex(id));
    }
    return out;
}

std::vector<Quick> QuickSlotsOf(Entity& self)
{
    std::vector<Quick> out;
    for (int slot = 0; slot < QuickSlots; ++slot) {
        const GEInt i = self.Inventory.FindStackIndexByQuickSlot(slot);
        if (i < 0 || i >= self.Inventory.GetStackCount() || StackQuickSlot(self, i) != slot) continue;
        const Template item = self.Inventory.GetTemplate(i);
        bCPropertyID const& id = item;
        out.push_back({slot, Hex(id), self.Inventory.GetQuality(i) & QualityMask});
    }
    return out;
}

char const* ValidateTemplates(std::vector<Stack> const& stacks, std::vector<std::string> const& spells)
{
    std::unordered_set<std::string> spellIds(spells.begin(), spells.end());
    for (Stack const& s : stacks) {
        const Template item = FromHex(s.id);
        if (!item.IsValid()) return "an invalid item template";
        eCTemplateEntityProxy proxy;
        proxy.SetEntity(static_cast<bCPropertyID const&>(item));
        eCTemplateEntity* templateEntity = proxy.GetEntity();
        if (!templateEntity || !templateEntity->GetPropertySet(eEPropertySetType_Item))
            return "a stack whose item does not exist";
        Entity templateItem(templateEntity);
        if (static_cast<int>(templateItem.Item.GetCategory()) != s.category)
            return "a stack with the wrong item category";
        if (s.category == gEItemCategory_Skill) {
            Entity skill = templateItem.Item.GetSkill();
            if (!skill.GetInstance() || !skill.GetTemplate().IsValid())
                return "a skill whose template does not exist";
        }
        if (s.category == gEItemCategory_Spellbook && spellIds.count(s.id)) {
            Entity spell = templateItem.Item.GetSpell();
            if (!spell.GetInstance() || !spell.GetTemplate().IsValid())
                return "a spell whose template does not exist";
        }
    }
    return nullptr;
}

bool ApplySpellAndQuick(Entity& self, char const* source, std::vector<std::string> const& spells,
                        std::vector<Quick> const& wanted)
{
    unsigned learned = 0, spellsMissing = 0;
    for (std::string const& id : spells) {
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
    for (int i = 0; i < self.Inventory.GetStackCount(); ++i) {
        if (StackQuickSlot(self, i) >= 0) self.Inventory.AssignStackToQuickSlot(i, -1);
    }
    unsigned assigned = 0, missing = 0;
    for (Quick const& q : wanted) {
        int index = -1;
        for (int i = 0; i < self.Inventory.GetStackCount() && index < 0; ++i) {
            if (StackQuickSlot(self, i) >= 0) continue;
            const Template item = self.Inventory.GetTemplate(i);
            bCPropertyID const& id = item;
            if (Hex(id) == q.id && (self.Inventory.GetQuality(i) & QualityMask) == q.quality) index = i;
        }
        if (index >= 0 && self.Inventory.AssignStackToQuickSlot(index, q.slot) &&
            self.Inventory.FindStackIndexByQuickSlot(q.slot) == index && StackQuickSlot(self, index) == q.slot) {
            ++assigned;
        } else {
            ++missing;
            Logf("hero: record %s could not restore quick slot %d item=%s quality=%u",
                 source, q.slot, q.id.c_str(), q.quality);
        }
    }
    const std::vector<Quick> actual = QuickSlotsOf(self);
    if (actual.size() != wanted.size()) {
        ++missing;
    } else {
        for (Quick const& q : wanted) {
            if (std::none_of(actual.begin(), actual.end(), [&q](Quick const& a) {
                    return a.slot == q.slot && a.id == q.id && a.quality == q.quality;
                })) ++missing;
        }
    }
    Logf("hero: spells %s learned=%u/%zu quick=%u missing=%u", source, learned, spells.size(), assigned, missing);
    return spellsMissing == 0 && missing == 0;
}

bool CaptureHeroSnapshot(Entity& self, eCDynamicEntity& hero, codec::ParsedRecord& out)
{
    auto* memory = static_cast<gCPlayerMemory_PS*>(hero.GetPropertySet(eEPropertySetType_PlayerMemory));
    if (!memory) return false;
    const bCVector& p = hero.GetWorldPosition();
    const bCQuaternion q = hero.GetWorldRotation();
    out.pose = {{p.GetX(), p.GetY(), p.GetZ()}, {q.GetX(), q.GetY(), q.GetZ(), q.GetW()}};
    out.downed = downed::Held() ? 1 : 0;
    out.level = static_cast<unsigned>(self.NPC.Level);
    out.levelMax = static_cast<unsigned>(self.NPC.LevelMax);
    out.xp = static_cast<int>(self.PlayerMemory.XP);
    out.lpAttribs = static_cast<int>(self.PlayerMemory.LPAttribs);
    out.lpPerks = static_cast<int>(self.PlayerMemory.LPPerks);
    out.attrs.clear();
    if (!CaptureAttributes(*memory, out.attrs)) return false;
    out.stacks = Stacks(self);
    out.spells = LearnedSpellsOf(self);
    out.quicks = QuickSlotsOf(self);
    return true;
}

void RollbackHeroSnapshot(Entity& self, eCDynamicEntity& hero, codec::ParsedRecord const& backup)
{
    downed::Restore(backup.downed != 0, "hero-record-rollback");
    self.Inventory.HoldStacks(-1, -1);
    std::unordered_set<std::string> skillSet;
    for (Stack const& s : backup.stacks)
        if (s.category == gEItemCategory_Skill) skillSet.insert(s.id);
    for (int i = self.Inventory.GetStackCount() - 1; i >= 0; --i) {
        if (static_cast<int>(self.Inventory.GetCategory(i)) == gEItemCategory_Skill) {
            const Template t = self.Inventory.GetTemplate(i);
            bCPropertyID const& id = t;
            if (skillSet.count(Hex(id))) continue;
            self.Inventory.DeleteStack(i);
            continue;
        }
        if (self.Inventory.GetSlotFromStack(i) != gESlot_None) self.Inventory.UnEquipStack(i);
        self.Inventory.DeleteStack(i);
    }
    auto makeItemOps = [&self]() {
        struct Ops {
            Entity& self;
            int StackCount() const { return self.Inventory.GetStackCount(); }
            int CreateItems(Stack const& s) const { return self.Inventory.CreateItems(FromHex(s.id), s.quality, s.amount); }
            int GetAmount(int index) const { return self.Inventory.GetAmount(index); }
            int SplitStack(int index, int amount) const { return self.Inventory.SplitStack(index, amount); }
            bool EquipStackToSlot(int index, int slot) const
            {
                return self.Inventory.EquipStackToSlot(index, static_cast<gESlot>(slot)) != GEFalse;
            }
        };
        return Ops{self};
    };
    for (size_t idx : codec::RestoreStackOrder(backup.stacks, gEItemCategory_Skill, gESlot_None)) {
        Stack const& s = backup.stacks[idx];
        const Template item = FromHex(s.id);
        if (s.category == gEItemCategory_Skill) {
            int index = FindSkillStack(self, s.id);
            if (index >= 0 && ((self.Inventory.GetQuality(index) & QualityMask) != s.quality ||
                               self.Inventory.GetAmount(index) != s.amount)) {
                self.Inventory.DeleteStack(index);
                index = -1;
            }
            if (index < 0) index = self.Inventory.CreateItems(item, s.quality, s.amount);
            if (index < 0) continue;
            const Template skill = SkillOf(self, index);
            if (skill.IsValid()) {
                const bool knows = self.Inventory.HasLearnedSkill(skill) != GEFalse;
                if (s.learned == 1 && !knows) self.Inventory.TeachSkill(skill);
                else if (s.learned == 0 && knows) {
                    self.Inventory.DeleteStack(index);
                    index = self.Inventory.CreateItems(item, s.quality, s.amount);
                }
            }
            if (index >= 0) {
                if (s.slot != gESlot_None) self.Inventory.EquipStackToSlot(index, static_cast<gESlot>(s.slot));
                else if (self.Inventory.GetSlotFromStack(index) != gESlot_None) self.Inventory.UnEquipStack(index);
            }
            continue;
        }
        codec::RestoreItemStack(s, gESlot_None, makeItemOps());
    }
    self.NPC.Level = backup.level;
    self.NPC.LevelMax = backup.levelMax;
    self.PlayerMemory.XP = backup.xp;
    self.PlayerMemory.LPAttribs = backup.lpAttribs;
    self.PlayerMemory.LPPerks = backup.lpPerks;
    if (auto* memory = static_cast<gCPlayerMemory_PS*>(hero.GetPropertySet(eEPropertySetType_PlayerMemory)))
        ApplyAttributes(*memory, backup.attrs);
    if (downed::Held()) self.PlayerMemory.SetBaseValue(bCString("HP"), 1);
    ApplySpellAndQuick(self, "rollback", backup.spells, backup.quicks);
    if (!net::IsHost() && saving::ValidRecordPose(backup.pose))
        self.MoveTo(body::Pose(hero, backup.pose.position, backup.pose.rotation));
}

bool ReadFileToMemory(char const* path, std::vector<char>& bytes)
{
    bytes.clear();
    if (!path || !*path) return false;
    FILE* file = nullptr;
    if (fopen_s(&file, path, "rb") || !file) return false;
    char buf[4096];
    while (size_t got = std::fread(buf, 1, sizeof(buf), file)) {
        if (bytes.size() + got > codec::MaxRecordBytes + 1) {
            std::fclose(file);
            return false;
        }
        bytes.insert(bytes.end(), buf, buf + got);
    }
    const bool ok = std::ferror(file) == 0;
    std::fclose(file);
    return ok;
}
} // namespace


bool SaveRecordImpl(char const* path, saving::Key const& key)
{
    if (!path || !*path || !saving::Valid(key)) {
        Logf("hero: record not written (invalid path or identity)");
        return false;
    }
    eCDynamicEntity* hero = Hero();
    if (!Ready(hero)) { Logf("hero: record %s not written (hero not ready)", path); return false; }
    const bCVector& p = hero->GetWorldPosition();
    const bCQuaternion q = hero->GetWorldRotation();
    saving::RecordPose pose = {{p.GetX(), p.GetY(), p.GetZ()}, {q.GetX(), q.GetY(), q.GetZ(), q.GetW()}};
    const float qLen2 = pose.rotation[0] * pose.rotation[0] + pose.rotation[1] * pose.rotation[1] +
                        pose.rotation[2] * pose.rotation[2] + pose.rotation[3] * pose.rotation[3];
    if (std::isfinite(qLen2) && qLen2 > 0.25f && (qLen2 < 0.99f || qLen2 > 1.01f)) {
        const float inv = 1.0f / std::sqrt(qLen2);
        for (float& component : pose.rotation) component *= inv;
    }
    if (!saving::ValidRecordPose(pose)) { Logf("hero: record %s not written (invalid pose)", path); return false; }

    Entity self(hero);
    auto* memory = static_cast<gCPlayerMemory_PS*>(hero->GetPropertySet(eEPropertySetType_PlayerMemory));
    const std::vector<Stack> stacks = Stacks(self);
    const std::vector<std::string> spells = LearnedSpellsOf(self);
    const std::vector<Quick> quick = QuickSlotsOf(self);
    std::vector<saving::RecordAttribute> attrs;
    char const* invalid = codec::ValidateLogicalContents(stacks, spells, quick, EngineSchema());
    if (!invalid) invalid = ValidateTemplates(stacks, spells);
    const unsigned level = static_cast<unsigned>(self.NPC.Level), levelMax = static_cast<unsigned>(self.NPC.LevelMax);
    const int xp = static_cast<int>(self.PlayerMemory.XP), lpa = static_cast<int>(self.PlayerMemory.LPAttribs),
              lpp = static_cast<int>(self.PlayerMemory.LPPerks);
    if (!invalid && (level > 1000 || levelMax > 1000 || xp < 0 || xp > 100000000 ||
                     lpa < 0 || lpa > 100000 || lpp < 0 || lpp > 100000)) invalid = "stats out of range";
    if (!CaptureAttributes(*memory, attrs)) invalid = "an attribute missing or out of range";
    if (invalid) { Logf("hero: record %s not written (%s)", path, invalid); return false; }

    codec::ParsedRecord record;
    record.key = key;
    record.pose = pose;
    record.downed = downed::Held() ? 1 : 0;
    record.level = level;
    record.levelMax = levelMax;
    record.xp = xp;
    record.lpAttribs = lpa;
    record.lpPerks = lpp;
    record.attrs = std::move(attrs);
    record.stacks = std::move(stacks);
    record.spells = std::move(spells);
    record.quicks = std::move(quick);

    std::vector<uint8_t> encoded;
    if (char const* encodeErr = codec::EncodeRecord(record, EngineSchema(), encoded)) {
        Logf("hero: record %s not written (%s)", path, encodeErr);
        return false;
    }

    const std::string temp = std::string(path) + ".tmp";
    FILE* file = nullptr;
    if (fopen_s(&file, temp.c_str(), "wb") || !file) {
        Logf("hero: record %s not written (cannot open temporary file)", path);
        return false;
    }
    bool ok = !encoded.empty() && std::fwrite(encoded.data(), 1, encoded.size(), file) == encoded.size();
    ok = fflush(file) == 0 && ok;
    ok = _commit(_fileno(file)) == 0 && ok;
    ok = fclose(file) == 0 && ok;
    if (ok) ok = MoveFileExA(temp.c_str(), path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
    if (!ok) {
        DeleteFileA(temp.c_str());
        Logf("hero: record %s not written; the one before stays", path);
        return false;
    }
    Logf("hero: bound record written version=%u min_reader=%u world=%s save=%s player=%llu level=%u xp=%d lp=%d/%d stacks=%zu spells=%zu quick=%zu bytes=%zu crc=%08x",
         record.schemaVersion, record.minReaderVersion, key.world.c_str(), key.save.c_str(),
         static_cast<unsigned long long>(key.player), level, xp, lpa, lpp,
         record.stacks.size(), record.spells.size(), record.quicks.size(),
         encoded.size(), saving::Crc32(encoded.data(), encoded.size()));
    return true;
}

bool SaveRecord(char const* path, saving::Key const& key) { return SaveRecordImpl(path, key); }

bool LoadRecordImpl(char const* data, size_t size, char const* label, saving::Key const& expected, bool validateOnly = false)
{
    eCDynamicEntity* hero = Hero();
    lastRefusal = nullptr;
    if (!validateOnly && !Ready(hero)) {
        lastRefusal = "hero not ready";
        Logf("hero: record not applied (hero not ready)");
        return false;
    }
    codec::ParsedRecord parsed;
    char const* refused = codec::ParseBoundRecord(data, size, expected, EngineSchema(), parsed);
    if (!refused && !validateOnly) {
        refused = ValidateTemplates(parsed.stacks, parsed.spells);
        if (!refused) {
            auto* memory = static_cast<gCPlayerMemory_PS*>(hero->GetPropertySet(eEPropertySetType_PlayerMemory));
            for (auto const& at : parsed.attrs) {
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
        Logf("hero: record %s refused (%s); the hero stays as it is", label, refused);
        return false;
    }
    if (validateOnly) {
        Logf("hero: record validated version=%u min_reader=%u world=%s save=%s player=%llu stacks=%zu spells=%zu quick=%zu",
             parsed.schemaVersion, parsed.minReaderVersion,
             parsed.key.world.c_str(), parsed.key.save.c_str(),
             static_cast<unsigned long long>(parsed.key.player),
             parsed.stacks.size(), parsed.spells.size(), parsed.quicks.size());
        return true;
    }

    Entity self(hero);
    codec::ParsedRecord backup;
    const bool haveBackup = CaptureHeroSnapshot(self, *hero, backup);
    const int savedDowned = parsed.downed;
    const saving::RecordPose& pose = parsed.pose;

    downed::Restore(savedDowned != 0, "hero-record");
    std::unordered_set<std::string> recordSkills;
    for (Stack const& s : parsed.stacks)
        if (s.category == gEItemCategory_Skill) recordSkills.insert(s.id);
    unsigned skillsRemoved = 0, skillsTaught = 0, skillsUnlearned = 0;
    auto fail = [&](char const* reason) {
        if (haveBackup) RollbackHeroSnapshot(self, *hero, backup);
        lastRefusal = reason;
        Logf("hero: record %s failed (%s); rolled_back=%d", label, reason, haveBackup ? 1 : 0);
        return false;
    };

    if (!self.Inventory.HoldStacks(-1, -1) &&
        (self.Inventory.GetItemFromSlot(gESlot_RightHand).GetInstance() ||
         self.Inventory.GetItemFromSlot(gESlot_LeftHand).GetInstance()))
        return fail("could not release held items");
    for (int i = self.Inventory.GetStackCount() - 1; i >= 0; --i) {
        if (static_cast<int>(self.Inventory.GetCategory(i)) == gEItemCategory_Skill) {
            const Template skill = self.Inventory.GetTemplate(i);
            bCPropertyID const& id = skill;
            if (recordSkills.count(Hex(id))) continue;
            if (!self.Inventory.DeleteStack(i)) return fail("could not remove a skill stack");
            ++skillsRemoved;
            continue;
        }
        if (self.Inventory.GetSlotFromStack(i) != gESlot_None && !self.Inventory.UnEquipStack(i))
            return fail("could not unequip an item");
        if (!self.Inventory.DeleteStack(i)) return fail("could not remove an item");
    }
    struct ItemOps {
        Entity& self;
        int StackCount() const { return self.Inventory.GetStackCount(); }
        int CreateItems(Stack const& s) const { return self.Inventory.CreateItems(FromHex(s.id), s.quality, s.amount); }
        int GetAmount(int index) const { return self.Inventory.GetAmount(index); }
        int SplitStack(int index, int amount) const { return self.Inventory.SplitStack(index, amount); }
        bool EquipStackToSlot(int index, int slot) const
        {
            return self.Inventory.EquipStackToSlot(index, static_cast<gESlot>(slot)) != GEFalse;
        }
    } itemOps{self};
    for (size_t idx : codec::RestoreStackOrder(parsed.stacks, gEItemCategory_Skill, gESlot_None)) {
        Stack const& s = parsed.stacks[idx];
        const Template item = FromHex(s.id);
        if (s.category == gEItemCategory_Skill) {
            int index = FindSkillStack(self, s.id);
            if (index >= 0 && ((self.Inventory.GetQuality(index) & QualityMask) != s.quality ||
                               self.Inventory.GetAmount(index) != s.amount)) {
                if (!self.Inventory.DeleteStack(index)) return fail("could not replace a skill stack");
                index = -1;
            }
            if (index < 0) index = self.Inventory.CreateItems(item, s.quality, s.amount);
            if (index < 0) return fail("could not create a skill item");
            const Template skill = SkillOf(self, index);
            if (!skill.IsValid()) return fail("a skill template is missing");
            if (s.learned >= 0) {
                const bool knows = self.Inventory.HasLearnedSkill(skill) != GEFalse;
                if (s.learned == 1 && !knows) {
                    if (!self.Inventory.TeachSkill(skill) || !self.Inventory.HasLearnedSkill(skill))
                        return fail("could not teach a skill");
                    ++skillsTaught;
                } else if (s.learned == 0 && knows) {
                    if (!self.Inventory.DeleteStack(index)) return fail("could not unlearn a skill");
                    index = self.Inventory.CreateItems(item, s.quality, s.amount);
                    if (index < 0 || self.Inventory.HasLearnedSkill(skill))
                        return fail("could not recreate an unlearned skill");
                    ++skillsUnlearned;
                }
            }
            if (s.slot != gESlot_None) {
                if (!self.Inventory.EquipStackToSlot(index, static_cast<gESlot>(s.slot)))
                    return fail("could not equip a skill item");
            } else if (self.Inventory.GetSlotFromStack(index) != gESlot_None && !self.Inventory.UnEquipStack(index)) {
                return fail("could not unequip a skill item");
            }
            continue;
        }
        if (char const* itemErr = codec::RestoreItemStack(s, gESlot_None, itemOps)) return fail(itemErr);
    }
    if (!codec::SameStacks(Stacks(self), parsed.stacks)) return fail("restored inventory does not match the record");

    self.NPC.Level = parsed.level;
    self.NPC.LevelMax = parsed.levelMax;
    self.PlayerMemory.XP = parsed.xp;
    self.PlayerMemory.LPAttribs = parsed.lpAttribs;
    self.PlayerMemory.LPPerks = parsed.lpPerks;
    auto* memory = static_cast<gCPlayerMemory_PS*>(hero->GetPropertySet(eEPropertySetType_PlayerMemory));
    if (!ApplyAttributes(*memory, parsed.attrs)) return fail("an attribute could not be restored");
    if (downed::Held()) self.PlayerMemory.SetBaseValue(bCString("HP"), 1);
    if (!ApplySpellAndQuick(self, label, parsed.spells, parsed.quicks))
        return fail("spells or quick slots could not be restored");
    if (static_cast<unsigned>(self.NPC.Level) != parsed.level ||
        static_cast<unsigned>(self.NPC.LevelMax) != parsed.levelMax ||
        static_cast<int>(self.PlayerMemory.XP) != parsed.xp ||
        static_cast<int>(self.PlayerMemory.LPAttribs) != parsed.lpAttribs ||
        static_cast<int>(self.PlayerMemory.LPPerks) != parsed.lpPerks ||
        !AttributesMatch(*memory, parsed.attrs, downed::Held()))
        return fail("restored character stats do not match the record");

    if (net::IsHost()) {
        const bCVector& p = hero->GetWorldPosition();
        const bCQuaternion q = hero->GetWorldRotation();
        const saving::RecordPose actual{{p.GetX(), p.GetY(), p.GetZ()}, {q.GetX(), q.GetY(), q.GetZ(), q.GetW()}};
        if (!saving::PoseRestored(pose, actual)) {
            Logf("hero: host load preserving native .g3savcpdat placement native=(%.1f,%.1f,%.1f) record=(%.1f,%.1f,%.1f)",
                 actual.position[0], actual.position[1], actual.position[2],
                 pose.position[0], pose.position[1], pose.position[2]);
        }
    } else {
        self.MoveTo(body::Pose(*hero, pose.position, pose.rotation));
        const bCVector& p = hero->GetWorldPosition();
        const bCQuaternion q = hero->GetWorldRotation();
        const saving::RecordPose actual{{p.GetX(), p.GetY(), p.GetZ()}, {q.GetX(), q.GetY(), q.GetZ(), q.GetW()}};
        if (!saving::PoseRestored(pose, actual)) return fail("pose could not be restored");
        const float dx = actual.position[0] - pose.position[0], dy = actual.position[1] - pose.position[1],
                    dz = actual.position[2] - pose.position[2];
        if (dx * dx + dy * dy + dz * dz > 1.0f) {
            Logf("hero: pose adjusted by engine dpos=(%.1f,%.1f,%.1f)", dx, dy, dz);
        }
    }

    Logf("hero: bound record applied version=%u world=%s save=%s player=%llu level=%u xp=%d stacks=%zu skills=%zu/%u/%u/%u",
         parsed.schemaVersion, parsed.key.world.c_str(), parsed.key.save.c_str(),
         static_cast<unsigned long long>(parsed.key.player), parsed.level, parsed.xp,
         parsed.stacks.size(), recordSkills.size(), skillsTaught, skillsUnlearned, skillsRemoved);
    return true;
}

bool LoadRecord(char const* path, saving::Key const& key)
{
    std::vector<char> bytes;
    if (!ReadFileToMemory(path, bytes)) {
        lastRefusal = !path || !*path ? "invalid path or identity" : "the file cannot be read";
        Logf("hero: record %s refused (%s)", path ? path : "(null)", lastRefusal);
        return false;
    }
    return LoadRecordImpl(bytes.data(), bytes.size(), path, key, false);
}

bool ValidateRecord(char const* path, saving::Key const& key)
{
    std::vector<char> bytes;
    if (!ReadFileToMemory(path, bytes)) {
        lastRefusal = !path || !*path ? "invalid path or identity" : "the file cannot be read";
        Logf("hero: record %s refused (%s)", path ? path : "(null)", lastRefusal);
        return false;
    }
    return LoadRecordImpl(bytes.data(), bytes.size(), path, key, true);
}

bool LoadRecordFromMemory(uint8_t const* data, size_t size, saving::Key const& key)
{
    return LoadRecordImpl(reinterpret_cast<char const*>(data), size, "<memory>", key, false);
}

bool ValidateRecordMemory(uint8_t const* data, size_t size, saving::Key const& key)
{
    return LoadRecordImpl(reinterpret_cast<char const*>(data), size, "<memory>", key, true);
}

bool LoadRecordFromMemory(std::vector<uint8_t> const& bytes, saving::Key const& key)
{
    return LoadRecordFromMemory(bytes.data(), bytes.size(), key);
}

bool ValidateRecordMemory(std::vector<uint8_t> const& bytes, saving::Key const& key)
{
    return ValidateRecordMemory(bytes.data(), bytes.size(), key);
}

char const* LastRefusal() { return lastRefusal; }
}
