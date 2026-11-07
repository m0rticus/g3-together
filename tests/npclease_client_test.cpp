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

int main()
{
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
    auto stream = [&](std::string const& data, bool expected) {
        FILE* file = tmpfile();
        assert(file);
        assert(fwrite(data.data(), 1, data.size(), file) == data.size());
        rewind(file);
        assert(ReadSaveStream(file, restored) == expected);
        assert(fclose(file) == 0);
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
    std::cout << "npclease_client_test: PASS (200 transfers, conservative expiry, loss/reorder/duplicates)\n";
}
