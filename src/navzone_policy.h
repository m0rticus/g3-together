// SPDX-License-Identifier: GPL-3.0-or-later
// Engine-free NavZone/NavPath DCC registration tracking and scrub policy.
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <deque>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace g3t::navzone_policy {

enum class DccEntryAction { Keep, RemoveNull, RemoveRetired, RemoveOrphan };

struct ZoneTarget {
    void* zone = nullptr;
    bool isPath = false;

    bool operator==(ZoneTarget const& other) const noexcept
    {
        return zone == other.zone && isPath == other.isPath;
    }
};

struct ZoneMigrationPlan {
    std::array<void*, 2> deregisterZones{};
    std::size_t deregisterZoneCount = 0;
    std::array<void*, 2> deregisterDccs{};
    std::size_t deregisterDccCount = 0;
    bool registerInNewZone = false;
};

inline ZoneMigrationPlan PlanZoneMigration(void* previousZone, void* lastZone, void* newZone,
                                           void* navDcc, void* entityDcc) noexcept
{
    ZoneMigrationPlan plan;
    auto addZone = [&](void* z) {
        if (!z || z == newZone) return;
        for (std::size_t i = 0; i < plan.deregisterZoneCount; ++i)
            if (plan.deregisterZones[i] == z) return;
        if (plan.deregisterZoneCount < plan.deregisterZones.size())
            plan.deregisterZones[plan.deregisterZoneCount++] = z;
    };
    auto addDcc = [&](void* d) {
        if (!d) return;
        for (std::size_t i = 0; i < plan.deregisterDccCount; ++i)
            if (plan.deregisterDccs[i] == d) return;
        if (plan.deregisterDccCount < plan.deregisterDccs.size())
            plan.deregisterDccs[plan.deregisterDccCount++] = d;
    };
    addZone(previousZone);
    addZone(lastZone);
    addDcc(navDcc);
    addDcc(entityDcc);
    plan.registerInNewZone = newZone != nullptr && previousZone != newZone && navDcc != nullptr;
    return plan;
}

inline DccEntryAction ClassifyDccEntry(void const* dcc, bool isRetired, bool hasOwnerEntity) noexcept
{
    if (!dcc) return DccEntryAction::RemoveNull;
    if (isRetired) return DccEntryAction::RemoveRetired;
    if (!hasOwnerEntity) return DccEntryAction::RemoveOrphan;
    return DccEntryAction::Keep;
}

class DccRegistry {
  public:
    void OnRegister(void* zone, bool isPath, void* dcc)
    {
        if (!zone || !dcc) return;
        retired_.erase(dcc);
        ZoneTarget target{zone, isPath};
        auto& zones = dccToZones_[dcc];
        if (std::find(zones.begin(), zones.end(), target) == zones.end()) zones.push_back(target);
        auto& dccs = zoneToDccs_[zone];
        if (std::find(dccs.begin(), dccs.end(), dcc) == dccs.end()) dccs.push_back(dcc);
    }

    void OnDeregister(void* zone, bool isPath, void* dcc)
    {
        if (!zone || !dcc) return;
        ZoneTarget target{zone, isPath};
        if (auto it = dccToZones_.find(dcc); it != dccToZones_.end()) {
            auto& zones = it->second;
            zones.erase(std::remove(zones.begin(), zones.end(), target), zones.end());
            if (zones.empty()) dccToZones_.erase(it);
        }
        if (auto it = zoneToDccs_.find(zone); it != zoneToDccs_.end()) {
            auto& dccs = it->second;
            dccs.erase(std::remove(dccs.begin(), dccs.end(), dcc), dccs.end());
            if (dccs.empty()) zoneToDccs_.erase(it);
        }
    }

    void OnZoneRemoved(void* zone, bool isPath)
    {
        if (!zone) return;
        auto it = zoneToDccs_.find(zone);
        if (it == zoneToDccs_.end()) return;
        ZoneTarget target{zone, isPath};
        for (void* dcc : it->second) {
            auto dit = dccToZones_.find(dcc);
            if (dit == dccToZones_.end()) continue;
            auto& zones = dit->second;
            zones.erase(std::remove(zones.begin(), zones.end(), target), zones.end());
            if (zones.empty()) dccToZones_.erase(dit);
        }
        zoneToDccs_.erase(it);
    }

    std::vector<ZoneTarget> TakeZonesForDcc(void* dcc, bool markRetired)
    {
        std::vector<ZoneTarget> result;
        if (!dcc) return result;
        if (markRetired && retired_.insert(dcc).second) {
            retiredOrder_.push_back(dcc);
            while (retiredOrder_.size() > MaxRetiredEntries) {
                retired_.erase(retiredOrder_.front());
                retiredOrder_.pop_front();
            }
        }
        auto it = dccToZones_.find(dcc);
        if (it == dccToZones_.end()) return result;
        result = std::move(it->second);
        dccToZones_.erase(it);
        for (ZoneTarget const& target : result) {
            auto zit = zoneToDccs_.find(target.zone);
            if (zit == zoneToDccs_.end()) continue;
            auto& dccs = zit->second;
            dccs.erase(std::remove(dccs.begin(), dccs.end(), dcc), dccs.end());
            if (dccs.empty()) zoneToDccs_.erase(zit);
        }
        return result;
    }

    bool IsRetired(void const* dcc) const
    {
        return dcc && retired_.find(dcc) != retired_.end();
    }

    void Clear()
    {
        dccToZones_.clear();
        zoneToDccs_.clear();
        retired_.clear();
        retiredOrder_.clear();
    }

  private:
    static constexpr std::size_t MaxRetiredEntries = 8192;
    std::unordered_map<void*, std::vector<ZoneTarget>> dccToZones_;
    std::unordered_map<void*, std::vector<void*>> zoneToDccs_;
    std::unordered_set<void const*> retired_;
    std::deque<void const*> retiredOrder_;
};

} // namespace g3t::navzone_policy
