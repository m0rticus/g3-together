// SPDX-License-Identifier: GPL-3.0-or-later
#include "../src/npclease_client.h"
#include "../src/npclease_save.h"
#include "../src/npclease_guards.h"

#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <thread>
#include <vector>

using namespace g3t::npclease;

static_assert(MaxOverlayText == 2 * (20 + MaxLeases * detail::SavedWireSize) + sizeof(SavePrefix),
              "Save text bound must follow binary row size");

static void TestHitReadiness()
{
    ClientReadiness ready;
    assert(!ready.LossReason());
    const auto check = [&](bool ClientReadiness::* field, bool value, char const* reason) {
        auto changed = ready;
        changed.*field = value;
        assert(std::strcmp(changed.LossReason(), reason) == 0);
    };
    check(&ClientReadiness::running, false, "session/world unavailable");
    check(&ClientReadiness::enabled, false, "authority fallback disabled");
    check(&ClientReadiness::checkpointBusy, true, "checkpoint barrier");
    check(&ClientReadiness::processed, false, "native processing unavailable");
    check(&ClientReadiness::actor, false, "native animation actor unavailable");
    check(&ClientReadiness::eligible, false, "native eligibility changed");
    check(&ClientReadiness::expired, true, "conservative lease expired");

    Checkpoint state;
    state.templateId[0] = 1;
    std::strcpy(state.name, "Wolf");
    state.maxHp = state.hp = 1000;
    Lease host;
    ClientWindow client;
    assert(host.Offer(2, 7, 1, 100, state));
    assert(client.Prepare(1, 100, LeaseMs, 120));
    assert(host.Prepared(1, 130));
    assert(client.Commit(1, 150));
    AuthorityGuard guard{true, true, 0, client.deadline};
    for (uint32_t revision = 1; revision <= 32; ++revision) {
        const uint32_t now = 150 + revision * 50;
        state.hp -= 10;
        state.aiMode = revision % 2 ? 3 : 2;
        state.action = revision % 2 ? 4 : 0;
        state.busy = revision % 2;
        state.attitude = 4;
        state.targetPlayer = 2;
        state.position[0] += 100;
        assert(!ready.LossReason()); // stumble/fight/flee state is not native availability
        assert(host.AcceptState(2, 7, 1, 1, revision, state));
        assert(host.phase == Phase::Client && host.owner == 2 && host.epoch == 1);
        assert(client.mode == ClientWindow::Mode::Active && client.Active(1, now));
        assert(guard.Local(now) && !guard.Fenced(now));
        assert(!host.AcceptState(2, 7, 1, 1, revision, state));
        assert(!host.AcceptState(2, 7, 1, 1, revision - 1, state));
        assert(!host.AcceptState(2, 8, 1, 1, revision + 1, state));
        assert(host.phase == Phase::Client && host.owner == 2);
        assert(client.Renew(1, now, LeaseMs, now + 10));
        guard.deadline = client.deadline;
        assert(!client.Renew(1, now, LeaseMs, now + 20));
        assert(!client.Renew(1, now - 50, LeaseMs, now + 20));
    }
    const auto accepted = host.state;
    Checkpoint lostState = state;
    lostState.hp -= 10;
    // A dropped state/renew leaves the current owner live only until its existing permit.
    assert(host.state.hp == accepted.hp && host.state.hp != lostState.hp);
    assert(client.Active(1, client.deadline - 1) && guard.Local(client.deadline - 1));
    assert(!client.Active(1, client.deadline) && guard.Fenced(client.deadline));
    assert(!client.Renew(1, client.deadline, LeaseMs, client.deadline));
    assert(!host.AcceptState(2, 7, 1, 2, 33, lostState));
    // A later accepted checkpoint can skip a lost revision without changing authority.
    assert(host.AcceptState(2, 7, 1, 1, 34, lostState));
    assert(host.phase == Phase::Client && host.owner == 2 && host.state.hp == lostState.hp);
    state = lostState;
    assert(CommitEffect(state, NativeDeath));
    assert(!CommitEffect(state, NativeDeath));
    state.hp = 0;
    assert(host.AcceptState(2, 7, 1, 1, 35, state));
    assert(!host.AcceptState(2, 7, 1, 1, 36, accepted));
    assert(host.Cut(2, 7, 1, 1, 36, state));
    client.Freeze();
    assert(!client.Renew(1, 1900, LeaseMs, 1910));
    assert(host.HandBack(2, 1920));
    assert(host.owner == 1 && host.state.dead && host.state.hp == 0);
    assert(host.state.effects == NativeDeath);
    assert(!host.AcceptState(2, 7, 1, 1, 37, accepted));
    ClientReadiness expired = ready;
    expired.expired = true;
    assert(expired.LossReason());
    assert(!client.Active(1, client.deadline));
}

int main()
{
    TestHitReadiness();
    ClientWindow client;
    assert(!client.Commit(1, 10));
    assert(!client.Prepare(1, 0, LeaseMs, LeaseMs));
    assert(client.Prepare(1, 100, LeaseMs, 130));
    const auto initialDeadline = client.deadline;
    assert(client.Prepare(1, 150, LeaseMs, 170));
    assert(client.deadline == initialDeadline); // a retried offer cannot extend the lease
    assert(!client.Commit(2, 200));
    assert(client.Commit(1, 200));
    assert(client.Commit(1, 200));
    assert(client.Active(1, 201));
    assert(!client.Active(2, 201));
    assert(client.Renew(1, 300, LeaseMs, 340));
    assert(!client.Renew(1, 250, LeaseMs, 350));
    assert(!client.Renew(2, 500, LeaseMs, 550));
    assert(!client.Renew(1, 500, LeaseMs + 1, 550));
    assert(!client.Active(1, client.deadline));
    assert(!client.Commit(1, client.deadline));
    assert(!client.Renew(1, client.deadline, LeaseMs, client.deadline));
    client.Freeze();
    assert(!client.Renew(1, 1000, LeaseMs, 1020));
    assert(!client.Commit(1, 1100));
    assert(client.Prepare(2, 1200, LeaseMs, 1250));
    assert(client.Commit(2, 1300));
    client.Freeze();
    client.Reset(3);
    assert(!client.Prepare(2, 1400, LeaseMs, 1450));
    assert(!client.Prepare(3, 1400, LeaseMs, 1450));
    for (uint32_t epoch = 4; epoch < 204; ++epoch) {
        const uint32_t start = epoch * 10000;
        assert(client.Prepare(epoch, start, LeaseMs, start + 20));
        assert(client.Commit(epoch, start + 40));
        assert(!client.Commit(epoch - 1, start + 60));
        client.Freeze();
        assert(!client.Renew(epoch, start + 100, LeaseMs, start + 110));
    }
    ClientWindow wrap;
    assert(wrap.Prepare(1, UINT32_MAX - 1000, LeaseMs, UINT32_MAX - 990));
    assert(wrap.Commit(1, UINT32_MAX - 980));
    assert(wrap.Active(1, 1000));
    assert(!wrap.Active(1, 1700));
    assert(!wrap.Renew(1, 1800, LeaseMs, 1900));
    Checkpoint falling;
    falling.templateId[0] = 1;
    std::strcpy(falling.name, "Wolf");
    falling.maxHp = 100;
    falling.hp = 0;
    falling.dead = 1;
    falling.aiMode = 9;
    falling.action = 35;
    falling.busy = 1;
    assert(Valid(falling)); // a terminal life can still have an unsafe native ragdoll cut
    SaveOverlay empty, restored;
    std::string text;
    assert(EncodeSaveText(empty, text));
    assert(DecodeSaveText(text, restored));
    assert(restored.empty());
    assert(text.size() <= MaxOverlayText);
    assert(!DecodeSaveText(text.substr(0, text.size() - 1), restored));
    assert(!DecodeSaveText(text + text, restored));
    auto corrupt = text;
    corrupt[sizeof(SavePrefix) + 8] = corrupt[sizeof(SavePrefix) + 8] == '0' ? '1' : '0';
    assert(!DecodeSaveText(corrupt, restored));
    corrupt = text;
    corrupt[sizeof(SavePrefix)] = '!';
    assert(!DecodeSaveText(corrupt, restored));
    assert(!DecodeSaveText(std::string(MaxOverlayText + 1, '0'), restored));
    Saved adopted;
    adopted.guid[0] = 4;
    adopted.epoch = 7;
    adopted.state = falling;
    adopted.adopted = 1;
    SaveOverlay withAdoption{adopted};
    std::string adoptedText;
    assert(EncodeSaveText(withAdoption, adoptedText));
    assert(DecodeSaveText(adoptedText, restored));
    assert(restored.size() == 1 && restored[0].adopted == 1 && restored[0].epoch == 7);
    auto invalidAdoption = withAdoption;
    invalidAdoption[0].adopted = 2;
    const auto unchangedText = adoptedText;
    assert(!EncodeSaveText(invalidAdoption, adoptedText));
    assert(adoptedText == unchangedText);
    auto stream = [&](std::string const& data, bool expected) {
        const char* path = "npclease-save-stream-test.txt";
        FILE* file = std::fopen(path, "w+b");
        assert(file);
        assert(fwrite(data.data(), 1, data.size(), file) == data.size());
        rewind(file);
        assert(ReadSaveStream(file, restored) == expected);
        assert(fclose(file) == 0);
        assert(std::remove(path) == 0);
    };
    stream("G3T 2\njournal data\n" + text + "downed 0\n", true);
    stream(text + text, false);
    stream("G3T 2\ndowned 0\n", false);
    stream(text.substr(0, text.size() - 1), false);
    stream(text + "npclease1 invalid\n", false);
    stream("ignored " + std::string(1024, 'a') + "\n" + text, true);
    GuardRegistry guards;
    GuardKey key{};
    key[0] = 1;
    assert(!guards.Get(key).Fenced(0));
    guards.Put(key, {true, true, 0, 3000});
    assert(guards.Get(key).Local(100));
    assert(!guards.Get(key).Fenced(100));
    assert(guards.Get(key).Fenced(3000));
    guards.Put(key, {true, false, 2, 0});
    assert(guards.Get(key).Fenced(100));
    assert(guards.Get(key).owner == 2);
    guards.Erase(key);
    assert(!guards.Get(key).Fenced(100)); // hand-back must restore ordinary mirrored NPC contact rules
    std::vector<std::thread> readers;
    for (int i = 0; i < 3; ++i) readers.emplace_back([&] {
        for (int j = 0; j < 50000; ++j) {
            const auto value = guards.Get(key);
            if (value.leased) {
                assert(value.locallyDriven ? value.owner == 0 && value.deadline == 3000 :
                    value.owner == 2 && value.deadline == 0);
            } else assert(!value.locallyDriven && !value.owner && !value.deadline);
        }
    });
    for (int j = 0; j < 50000; ++j) {
        guards.Put(key, j % 2 ? AuthorityGuard{true, true, 0, 3000} : AuthorityGuard{true, false, 2, 0});
        if (j % 3 == 0) guards.Erase(key);
        if (j % 7 == 0) guards.Clear();
    }
    for (auto& reader : readers) reader.join();
    RestoreEpochBinding binding;
    binding.Bind(10, 11);
    assert(!binding.Consume(10));
    assert(!binding.Consume(12)); // an unrelated world change must still discard the restore
    assert(binding.expected == 11);
    assert(binding.Consume(11));
    assert(!binding.Consume(11));
    assert(!binding.Consume(12));
    binding.Bind(0, 42); // the first host publication may follow a solo native import
    assert(!binding.Consume(0));
    assert(binding.Consume(42));
    binding.Bind(42, 42);
    assert(!binding.Consume(42));
    std::cout << "npclease_client_test: PASS (200 transfers, conservative expiry, loss/reorder/duplicates)\n";
}
