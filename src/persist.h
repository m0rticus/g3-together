// SPDX-License-Identifier: GPL-3.0-or-later
// Host-authoritative checkpoint bundles: one native world, a world/save manifest, and each account's own
// mandatory character record. Native saves are deferred outside Process; client saves request the host's checkpoint.
#pragma once

namespace g3t::persist {
void Install();          // once the game DLLs are loaded; refuses (and logs) on any other build
void* SaveTarget();      // the game's SaveGame entry (checked before hooking), nullptr if not hooked
void* LoadTarget();
bool CheckpointBusy();   // no new player exchange may cross an in-flight character/world checkpoint
// Once per frame: after a client loaded the host's save, its own hero record (written when the host saved)
// is applied before opening the world gate. A missing/refused character never permits the native host hero.
// Also: which world this machine plays, and the host's world for clients
// (the world gate, net.h SetInWorld).
void Tick();
// Around the engine module's Process. BeginFrame first: false, and nothing of the mod runs in this frame, while the
// game's own load runs (its Session::Start runs frames of its own, C 22.10.); true otherwise, and EndFrame follows.
bool BeginFrame();
void EndFrame();
bool NativeLoading();      // no native entity inspection until the synchronous loader returns
bool HeroPending();        // client still has the host's fallback hero, not its own restored life
void OnWorldActivate();    // from the engine module's callbacks
void OnWorldDeactivate();
using LogFn = void (*)(char const*);
void Off(LogFn log);     // QA negative control: write no sidecar
void CampaignOff(LogFn log);  // QA negative control (host): joining players get no campaign
void WorldSyncOff(LogFn log);  // QA negative control (host): no world change is passed on; the save only at a join (as before)
void BadTransfer(LogFn log);   // QA, host (stage X1): a wrapping file chunk and a save name that leaves the save folder
void GenerationOff(LogFn log); // QA negative control (host, stage W1): a slot starts over only once it is empty (as before 23.10.)
void Pause(char const* argument, LogFn log);  // QA (host, stage W1): "<ms>": the world bookkeeping waits, as if leave and join fell in one frame
void WorldGateLate(LogFn log); // QA negative control (client, stage W3): the gate stays open into a load and opens before the hero record
void RecordWaitOff(LogFn log); // QA negative control (client, stage W7): a refused hero record opens the gate anyway (as before 23.10.)
void Waiting(LogFn log);       // QA (stage W7): RECORDWAIT line, whether the player waits for its own hero record
void LoadTicksOff(char const* argument, LogFn log);  // QA stage W6: negative control; hold/release/welcome are deterministic timing probes
void WorldReport(LogFn log);   // QA: WORLD line
void Report(LogFn log);  // QA: PERSIST line
bool Switch(char const* action, LogFn log);  // QA negative controls of the play-evening fixes (27.10.): true if it was one
}
