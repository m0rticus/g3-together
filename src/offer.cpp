// SPDX-License-Identifier: GPL-3.0-or-later
#include "offer.h"

#include "guid.h"
#include "hud.h"
#include "i18n.h"
#include "log.h"
#include "persist.h"
#include "trade.h"
#include "trade_policy.h"
#include "trade_replay.h"

#include <Game.h>
#include <Script.h>
#include <Windows.h>
#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <map>
#include <string>
#include <tuple>
#include <vector>

namespace g3t::offer {
namespace {
constexpr unsigned AnyQuality = ~0u;
ResultFn resultHandler = nullptr;
uint32_t nextSerial = 0;
struct Sent {
    net::OfferNote note;
    uint32_t at;
    uint64_t sellerAccount = 0;
    bool delivered = false, retryPaused = false;
};
std::vector<Sent> sent;
using Identity = std::tuple<uint8_t, uint64_t, uint32_t>;
struct Outcome { net::OfferResultNote result; uint64_t fingerprint; bool finalized = false; };
std::map<Identity, Outcome> outcomes;
unsigned sentCount = 0, applied = 0, refused = 0, retries = 0, rolledBack = 0;
bool inventoryHealthy = true;

Template Of(uint8_t const id[16])
{
    _GUID raw;
    std::memcpy(&raw, id, 16);
    return Template(guid::Property(raw));
}

bool Same(Entity& self, int stack, Template const& item, unsigned quality = AnyQuality)
{
    return self.Inventory.GetTemplate(stack) == static_cast<bCPropertyID const&>(item) &&
           (quality == AnyQuality || self.Inventory.GetQuality(stack) == quality);
}

int Amount(Entity& self, Template const& item, unsigned quality = AnyQuality)
{
    int amount = 0;
    for (int i = 0; i < self.Inventory.GetStackCount(); ++i)
        if (Same(self, i, item, quality) && trade::TradableStack(self, i)) amount += self.Inventory.GetAmount(i);
    return amount;
}

struct Step { Template item; unsigned quality; int amount; bool created; };

int Take(Entity& self, Template const& item, unsigned quality, int amount, std::vector<Step>& steps)
{
    int taken = 0;
    for (int i = self.Inventory.GetStackCount() - 1; i >= 0 && taken < amount; --i) {
        if (!Same(self, i, item, quality) || !trade::TradableStack(self, i)) continue;
        const int n = std::min(amount - taken, self.Inventory.GetAmount(i));
        if (n <= 0) continue;
        const unsigned q = self.Inventory.GetQuality(i);
        if (n == self.Inventory.GetAmount(i) ? self.Inventory.DeleteStack(i) : self.Inventory.DeleteItems(i, n)) {
            taken += n;
            steps.push_back({item, q, n, false});
        }
    }
    return taken;
}

void Undo(Entity& self, std::vector<Step> const& steps)
{
    bool ok = true;
    for (auto it = steps.rbegin(); it != steps.rend(); ++it) {
        std::vector<Step> none;
        if (it->created) ok = (Take(self, it->item, it->quality, it->amount, none) == it->amount) && ok;
        else ok = (self.Inventory.CreateItems(it->item, it->quality, it->amount) >= 0) && ok;
    }
    ++rolledBack;
    if (!ok) {
        inventoryHealthy = false;
        Log("trade: ERROR restoring an exchange failed; stop and reload the last settled save");
        hud::Message(hud::Warning, i18n::Text(i18n::TradeRestoreFailed));
    }
}

bool Apply(net::OfferNote const& note)
{
    if (persist::CheckpointBusy()) return false;
    eCDynamicEntity* hero = gCSession::GetInstance().GetHero();
    if (!hero || !gCSession::GetInstance().IsGameRunning() || note.sellerEpoch != trade::Epoch() ||
        !note.buyerEpoch || !note.serial || note.acknowledge || (!note.count && !note.gold) ||
        note.gold < -trade_policy::MaxGoldAmount || note.gold > trade_policy::MaxGoldAmount) return false;
    Entity self(hero);
    const Template gold(trade_policy::GoldTemplateName);
    std::map<std::string, bool> unique;
    for (int i = 0; i < note.count; ++i) {
        auto const& entry = note.entries[i];
        const Template item = Of(entry.item);
        const std::string stack = trade_policy::StackKeyBytes(entry.item, entry.quality);
        if (!item.IsValid() || static_cast<bCPropertyID const&>(item) == static_cast<bCPropertyID const&>(gold) ||
            !entry.amount || entry.amount < -trade_policy::MaxStackAmount || entry.amount > trade_policy::MaxStackAmount ||
            entry.quality >= trade_policy::MaxQuality || !unique.emplace(stack, true).second) return false;
        // Validate metadata even for a deposit, not only stacks already in this inventory.
        eCTemplateEntityProxy proxy;
        proxy.SetEntity(static_cast<bCPropertyID const&>(item));
        Entity entity(proxy.GetEntity());
        if (!trade::TradableItem(entity)) return false;
        if (entry.amount > 0 && Amount(self, item, entry.quality) < entry.amount) return false;
    }
    if (note.gold > 0 && Amount(self, gold) < note.gold) return false;
    std::vector<Step> steps;
    bool ok = true;
    for (int i = 0; i < note.count && ok; ++i) {
        auto const& entry = note.entries[i];
        const Template item = Of(entry.item);
        if (entry.amount > 0) ok = Take(self, item, entry.quality, entry.amount, steps) == entry.amount;
        else {
            ok = self.Inventory.CreateItems(item, entry.quality, -entry.amount) >= 0;
            if (ok) steps.push_back({item, entry.quality, -entry.amount, true});
        }
    }
    if (ok && note.gold > 0) ok = Take(self, gold, AnyQuality, note.gold, steps) == note.gold;
    if (ok && note.gold < 0) {
        ok = self.Inventory.CreateItems(gold, 0, -note.gold) >= 0;
        if (ok) steps.push_back({gold, 0, -note.gold, true});
    }
    if (!ok) Undo(self, steps);
    return ok;
}

uint64_t Fingerprint(net::OfferNote note)
{
    note.acknowledge = 0;
    uint64_t hash = 14695981039346656037ull;
    auto const* bytes = reinterpret_cast<uint8_t const*>(&note);
    for (size_t i = 0; i < sizeof(note); ++i) hash = (hash ^ bytes[i]) * 1099511628211ull;
    return hash;
}

void OnOffer(net::OfferNote const& note)
{
    if (note.count > net::MaxOfferEntries || note.acknowledge > 1 || note.buyer == note.seller) return;
    if (!trade::replay::SellerEpoch(note.sellerEpoch, trade::Epoch())) {
        Logf("trade: ignored exchange %u for an earlier seller session player=%u", note.serial, note.buyer);
        return;
    }
    const Identity key{note.buyer, note.buyerEpoch, note.serial};
    auto cached = outcomes.find(key);
    const uint64_t hash = Fingerprint(note);
    if (cached != outcomes.end()) {
        if (hash != cached->second.fingerprint) {
            Log("trade: refused a changed payload with an already used transaction identity");
            return;
        }
        if (note.acknowledge) cached->second.finalized = true;
        auto result = cached->second.result;
        if (note.acknowledge) result.result = net::OfferFinalized;
        net::SendOfferResult(result);
        return;
    }
    if (note.acknowledge) {
        Log("trade: acknowledgment for an unknown exchange ignored");
        return;
    }
    const bool ok = Apply(note);
    net::OfferResultNote result{};
    result.buyer = note.buyer;
    result.seller = net::LocalId();
    result.buyerEpoch = note.buyerEpoch;
    result.sellerEpoch = note.sellerEpoch;
    result.serial = note.serial;
    result.result = ok ? net::OfferAccepted : net::OfferUnavailable;
    // Send the authoritative stock before the result, on the same reliable channel.
    result.goodsSerial = trade::Publish();
    outcomes.emplace(key, Outcome{result, hash, !ok});
    if (ok) ++applied;
    else ++refused;
    Logf("trade: automatic exchange %u from player %u %s", note.serial, note.buyer, ok ? "applied" : "refused");
    net::SendOfferResult(result);
}

void OnResult(net::OfferResultNote const& result)
{
    for (auto it = sent.begin(); it != sent.end(); ++it) {
        auto& pending = *it;
        auto const& note = pending.note;
        if (note.serial != result.serial || note.seller != result.seller || note.buyerEpoch != result.buyerEpoch ||
            note.sellerEpoch != result.sellerEpoch) continue;
        if (!trade::replay::SellerAccount(net::IsHost(), pending.sellerAccount, net::PeerAccount(note.seller))) {
            Logf("trade: ignored exchange result %u from a different seller account", note.serial);
            return;
        }
        if (result.result == net::OfferFinalized) {
            if (pending.delivered) sent.erase(it);
            return;
        }
        if (result.result != net::OfferAccepted && result.result != net::OfferUnavailable) return;
        if (!pending.delivered) {
            if (!resultHandler || !resultHandler(note, result.result == net::OfferAccepted)) {
                Log("trade: ERROR local exchange delivery failed; keeping transaction unsettled");
                return;
            }
            pending.delivered = true;
            pending.note.acknowledge = 1;
            trade::Publish();
            if (result.result != net::OfferAccepted) hud::Message(hud::Warning, i18n::Text(i18n::TradeUnavailable));
        }
        net::SendOffer(pending.note);
        pending.at = GetTickCount();
        return;
    }
}
}

void Start() { net::SetOfferHandlers(&OnOffer, &OnResult); }
void SetResultHandler(ResultFn handler) { resultHandler = handler; }

bool Send(net::OfferNote note)
{
    if (!sent.empty() || !net::Connected() || !note.sellerEpoch) return false;
    const uint64_t sellerAccount = net::IsHost() ? net::PeerAccount(note.seller) : 0;
    if (!trade::replay::SellerAccount(net::IsHost(), sellerAccount, sellerAccount)) {
        Logf("trade: cannot send exchange to player=%u without an authenticated account", note.seller);
        return false;
    }
    note.buyer = net::LocalId();
    note.buyerEpoch = trade::Epoch();
    note.serial = ++nextSerial;
    if (!net::SendOffer(note)) return false;
    sent.push_back({note, GetTickCount(), sellerAccount, false});
    ++sentCount;
    return true;
}

bool Reserve(net::OfferNote const& note)
{
    if (persist::CheckpointBusy()) {
        Log("trade: exchange reservation waits for the coordinated checkpoint");
        return false;
    }
    eCDynamicEntity* hero = gCSession::GetInstance().GetHero();
    if (!hero || !Idle()) return false;
    Entity self(hero);
    std::vector<Step> steps;
    for (int i = 0; i < note.count; ++i) {
        auto const& entry = note.entries[i];
        if (entry.amount < 0 && Take(self, Of(entry.item), entry.quality, -entry.amount, steps) != -entry.amount) {
            Undo(self, steps);
            return false;
        }
    }
    if (note.gold < 0 && Take(self, Template(trade_policy::GoldTemplateName), AnyQuality, -note.gold, steps) != -note.gold) {
        Undo(self, steps);
        return false;
    }
    return true;
}

bool Deliver(net::OfferNote const& note, bool accepted)
{
    eCDynamicEntity* hero = gCSession::GetInstance().GetHero();
    if (!hero || note.buyerEpoch != trade::Epoch()) return false;
    Entity self(hero);
    std::vector<Step> steps;
    auto give = [&](Template const& item, unsigned quality, int amount) {
        if (self.Inventory.CreateItems(item, quality, amount) < 0) return false;
        steps.push_back({item, quality, amount, true});
        return true;
    };
    bool ok = true;
    for (int i = 0; i < note.count && ok; ++i) {
        auto const& entry = note.entries[i];
        if (accepted ? entry.amount > 0 : entry.amount < 0)
            ok = give(Of(entry.item), entry.quality, accepted ? entry.amount : -entry.amount);
    }
    if (ok && (accepted ? note.gold > 0 : note.gold < 0))
        ok = give(Template(trade_policy::GoldTemplateName), 0, accepted ? note.gold : -note.gold);
    if (!ok) {
        Undo(self, steps);
        hud::Message(hud::Warning, i18n::Text(i18n::TradeRestoreFailed));
    }
    return ok;
}

bool Idle()
{
    if (!inventoryHealthy || !sent.empty()) return false;
    for (auto const& entry : outcomes)
        if (!entry.second.finalized) return false;
    return true;
}

void Tick()
{
    const DWORD now = GetTickCount();
    for (auto& pending : sent) {
        if (now - pending.at < 1000) continue;
        // No timeout refund: the owner may have applied the exchange while its answer was lost.
        const bool sameSeller = trade::replay::SellerAccount(
            net::IsHost(), pending.sellerAccount, net::PeerAccount(pending.note.seller));
        if (net::Connected() && !sameSeller && !pending.retryPaused) {
            Logf("trade: exchange %u retry paused; original seller account is not in slot %u",
                 pending.note.serial, pending.note.seller);
            pending.retryPaused = true;
        }
        if (net::Connected() && sameSeller) {
            pending.retryPaused = false;
            if (net::SendOffer(pending.note)) ++retries;
        }
        pending.at = now;
    }
}

void OnWorldChange()
{
    if (!Idle()) Log("trade: world deactivated with an unsettled exchange; no mutation is replayed onto the new hero");
    sent.clear();
    outcomes.clear();
    inventoryHealthy = true;
}

void OnPeerLeft(uint8_t player)
{
    bool uncertain = false;
    for (auto const& pending : sent)
        if (pending.note.seller == player) uncertain = true;
    for (auto const& entry : outcomes)
        if (std::get<0>(entry.first) == player && !entry.second.finalized) uncertain = true;
    if (uncertain)
        Logf("trade: unsettled exchange player=%u; saving remains blocked until the exchange is resolved or the last save is restored",
             player);
}

void Report(LogFn log)
{
    char line[240];
    Format(line, "EXCHANGESTATE pending=%zu received=%zu sent=%u applied=%u refused=%u retries=%u rolled_back=%u settled=%d",
           sent.size(), outcomes.size(), sentCount, applied, refused, retries, rolledBack, Idle() ? 1 : 0);
    log(line);
}

void AddCommand(char const* argument, LogFn log)
{
    const std::string arg = argument ? argument : "";
    const size_t cut = arg.rfind('_');
    eCDynamicEntity* hero = gCSession::GetInstance().GetHero();
    const Template item(cut == std::string::npos ? "" : arg.substr(0, cut).c_str());
    const int amount = cut == std::string::npos ? 0 : std::atoi(arg.substr(cut + 1).c_str());
    if (!hero || !item.IsValid() || amount < 1 || amount > trade_policy::MaxStackAmount) { log("REFUSED heroadd: <name>_<amount>"); return; }
    Entity self(hero);
    char line[160];
    Format(line, "HEROADD name=%s amount=%d stack=%d", arg.substr(0, cut).c_str(), amount, self.Inventory.CreateItems(item, 0, amount));
    log(line);
}

void CountCommand(char const* name, LogFn log)
{
    eCDynamicEntity* hero = gCSession::GetInstance().GetHero();
    const Template item(name ? name : "");
    if (!hero || !item.IsValid()) { log("REFUSED herocount: no hero or no such item"); return; }
    Entity self(hero);
    int total = 0;
    for (int i = 0; i < self.Inventory.GetStackCount(); ++i)
        if (Same(self, i, item)) total += self.Inventory.GetAmount(i);
    char line[160];
    Format(line, "HEROCOUNT name=%s total=%d tradable=%d", name, total, Amount(self, item));
    log(line);
}
}
