# Changelog

This log summarizes the releases and integration builds that changed gameplay, networking, saves, NPCs, combat, quests, Steam connection, or supported installations.

## Build 90

**Performance overhaul (Mode 5 bonewire, lazy pose/equipment capture, shared frame scans, scoped visibility/collision), 2v1 arena co-op, NavZone CanEvade crash fix, idle jitter fix, and protocol 82**

- **NavZone `CanEvade` crash fix (`src/navzone_guard.cpp`, `src/navzone_policy.h`, `src/world_pvs.cpp`, `src/remote.cpp`, `src/guards.cpp`)**: Guarded NavZone dynamic collision circles against stale or null-entity DCCs so `CanEvade` cannot crash on destroyed or unlinked entities.
- **Client NPC idle animation jitter (`src/snapshot.cpp`, `src/snapshot.h`)**: Stabilized client NPC idle animation playback under slow-host and initial-area cadence.
- **2v1 arena co-op & passive bystanders (`src/arena.cpp`, `src/arena_policy.h`, `src/ai.cpp`, `src/combat*.cpp`, `src/fightmem.cpp`, `src/guards*.cpp`, `src/relation_policy.h`)**: Shared arena fights as 2v1 co-op encounters while keeping uninvolved bystanders passive.
- **Lazy host NPC pose/equipment capture (`src/npc_send.cpp`, `src/npc_send.h`, `src/npcs.cpp`, `src/npcsend_*.h`)**: Deferred host skeleton pose and back-equipment capture until recipients need them, removing per-tick map/scan overhead.
- **Mode 5 bonewire & pose capture optimization (`src/npcwire_pose_mode5.h`, `src/npcwire_pose*`, `src/npcwire_bone*`, `src/pose.h`, `src/net_npc_pose.cpp`, `src/npcplayback_skeleton.cpp`, `src/saving_policy.h`)**: Added Mode 5 pose delta encoding, layout caching, and periodic pose ACK requests, bumping protocol from 81 to 82.
- **Shared per-frame NPC/entity scans & phase diagnostics (`src/frame_scan.cpp`, `src/frame_scan.h`, `src/host_frame_diag.h`, `src/Script_G3Together.cpp`, `src/combat_hostility.cpp`, subsystem ticks)**: Shared per-frame NPC/entity scans across subsystems and added non-NPC phase diagnostics.
- **Scoped `ForceAlwaysVisible`, sphere collision & detour overhead (`src/npcplayback.cpp`, `src/npcplayback_hooks.cpp`, `src/npcplayback_skeleton.cpp`, `src/npc_send.cpp`, `src/npcs.cpp`, `src/world.cpp`, `src/world_collision.cpp`, `src/world_pvs.cpp`)**: Scoped `ForceAlwaysVisible` to active offscreen capture/presentation lifetimes and reduced sphere-collision and detour lookup overhead.

## Build 89

**Default "Do not show tips" enabled on client join (protocol 81 unchanged)**

- **Client tutorial tips default (`src/client_tips.cpp`, `src/client_tips.h`, `src/client_tips_policy.h`, `src/persist.cpp`, `src/persist_client.cpp`, `src/Script_G3Together.cpp`)**: Automatically enables `gCPlayerMemory_PS::HideTips` and checks the native GUI tutorial `"Do not show tips"` checkbox on client join, new-game entry, and template load so tutorial popups are suppressed by default without modifying protocol 81.

## Build 88

**Player equipment sync, NPC handoff/PVS stability, client quest/sector sync, weapon loot anti-duplication, and protocol 81**

- **Event-driven player weapon & armor sync (`src/player_equip.h`, `src/remote_equip.cpp`, `src/remote.cpp`, `src/net*`)**: Added reliable `MsgPlayerEquipment` replication across all weapon and armor slots (with join/rejoin/load resync and pre-entry caching), bumping protocol from 80 to 81.
- **NPC handoff blinking & PVS hysteresis (`src/npclease_client.cpp`, `src/npclease_host.cpp`, `src/npc_send.cpp`, `src/npcs.cpp`, `src/world_pvs.cpp`, `src/world_collision_policy.h`)**: Inherited live host mirror visibility vetting on client takeover, held handback visibility across live rejections, ordered `npclease::Tick()` after `PassiveMirrorTick()`/`CorpseTick()` and before `ReconcileClientPresence()`, aligned host offer admission with recall readiness via `HostPresentForOffer`, and added hysteresis (`NeedsRemoteSimulationRegion`) around `mainFarClip` to prevent remote PVS thrashing.
- **Client-triggered NPC guide, party & revolution quests (`src/guide_policy.h`, `src/infocmd.cpp`, `src/infocmd_sector.cpp`, `src/npc_companions.cpp`)**: Guarded `SyncMirrorPartyAndGuide` unclaim with `ShouldReleaseLocalPartyClaim` and reset `relationAcknowledged` on party mismatch, limited `Navigation.Routine` dialog snapshots to explicit `SetRoutine` commands, resolved target enclave proxies for `StartRevolution`, made host `Guide` idempotent, deferred routine restarts while lease-fenced or in talk, and preserved `PartyMemberType`.
- **Host-authoritative world sector sync (`src/world_sectors.cpp`, `src/world_sectors.h`, `src/sector_sync_policy.h`, `src/persist*.cpp`, `src/net*.cpp`)**: Synced world sector enable/disable states across gameplay hooks, `world_sectors` save extensions, pre-admission ENet handshakes, and client admission reconciliation.
- **Repeat knockout looting & infinite bow fix (`src/npc_equipment_policy.h`, `src/npc_contact.cpp`, `src/npc_send.cpp`, `src/personal_ledger.cpp`)**: Excluded `Merchandise` back slots from host equipment replication, froze mirror back/held item creation once personal corpse state exists or while unconscious, and preserved deduplicated re-armed weapons across repeated knockouts.

## Build 87

**Client NPC visibility/playback, guard crime isolation, back-slot lootability, and protocol 80**

- **Client NPC visibility & PVS coverage (`src/world_pvs.cpp`, `src/npc_companions.cpp`)**: Reused ready main-PVS cell coverage while remote host simulation regions populate, throttled anchor retries, avoided hiding/resetting live visible mirror placement across same-owner stream resets, and cached ready cell boxes (cleared on region teardown/reset) to stop client animal blinking across lease and PVS transitions.
- **Client NPC idle playback (`src/npcsend_policy.h`, `src/snapshot.cpp`)**: Eliminated client NPC idle animation buffer starvation and catch-up jitter.
- **Town NPC theft/talk guard (`src/fightmem.cpp`, `src/guards.cpp`, `src/guards_policy.h`)**: Prevented stale `CurrentAttacker` puppets from hijacking host NPC talk and crime status updates.
- **Back-slot weapon/bow lootability (`src/npc_equipment_policy.h`, `src/net*`, `src/npcs.cpp`, `src/npc_identity.cpp`)**: Added `MsgNpcEquipment` (bumping protocol from 79 to 80) to replicate NPC back-equipped slots (`BackLeft`/`BackRight`/`Bow`/`Crossbow`) to client mirrors without overwriting personal loot, ignored `Merchandise` stacks in mirror lookups, unlinked non-zero hand slots safely via `UnlinkStackFromSlot`, and ensured corpses are prepared before loot/quick-loot scans.

## Build 86

**8 audit slices, ~180 verified multiplayer fixes, and cross-slice hardening**

### Network, Steam, and transport
- Fixed mode-4 bone-0 OOB reads in `npcwire_pose_encode.h`, plus `PoseRiceReader` EOF/overflow bounds.
- Fixed `OnNpcResync` iterator/reference invalidation and corrected `CloseClientLink` to preserve the host refusal reason (`Phase::Refused`).
- Corrected `Sender()` Steam identity probing at offset 12 and reset client control/pose tables on world or link teardown.
- Fixed direct-UDP session synchronization, retry freshness, Steam relay routing, and test-copy transport diagnostics.
- Preserved protocol 79 while hardening pose/control/stream packet handling and explicit peer incompatibility.

### Persistence, saves, worlds, and player state
- Fixed versioned save/restore and client autoload/rejoin edge cases.
- Reset canceled-save door settle timers and guarded hero codec/stack restoration.
- Added `world_pvs` entity-destruction notifications (`npcs::OnEntityDestroyed` and `ranged::ClearPuppetOverlay`) and made world-collision eviction readiness/eviction safer.
- Fixed clock, ending, mapmarks, snapshot, persistence, and auxiliary lifecycle paths.

### NPCs, companions, playback, and leases
- Fixed 40 NPC sync, playback, corpse, companion, summon, and lease-lifecycle bugs.
- `NewLife` now immediately sends `SendNpcReleaseTo` to out-of-range peers, preventing stuck `HostStreamClaimed` presentation.
- A settled-corpse `corpseActor` sentinel survives chunk unload/reload so corpses re-pose instead of standing up.
- Wired `npcs::HostTake` to lease revocation and deferred `guide.cpp` routine start until the NPC is unfenced.
- Sanitized despawn records and fixed companion/summon/skeleton/playback state handoffs.

### Combat, ranged attacks, downed state, and spells
- Fixed 24 audited combat, downed, ranged, shot, knockback, targeting, and spell problems.
- Preserved `pendingHostWipeEpoch` across `OnWorldDeactivate`; added top-of-function `ai::ShuttingDown()` hook guards and per-episode `LocalLifeState` separation.
- Made `FlushPendingShot` FIFO oldest-first and added explicit `targetPlayer` to `MsgNpcSpell`, with lease relinquishment when a third player takes over a client-leased NPC.
- Keyed `ranged_overlay` entries by `bCPropertyID`; fixed melee claims, hostility, fight memory, XP, downed `CombatUnavailable`, and anti-cheese `life.held`.

### Remote players, movement, HUD, and focus
- Fixed `Despawn` cleanup for null entities in `contacts` and `occupancy::Forget`, plus packet-order-guarded `!InGame` activity despawn (`activity.ticks >= latestTicks`).
- Added inventory, transform, and terminal guards around `Wear`/`Hold` and prevented stale remote/movement authority writes.
- Fixed HUD entity-slot reuse and focus diagnostic reporting.

### Items, containers, trading, doors, and occupancy
- Fixed 29 confirmed item, container, chest, pickpocket, trade, door, and occupancy bugs.
- Made `FlushPersonalObservers` snapshot iteration safe and fixed `PersonalMutation` re-entrancy/orphan retirement.
- Preserved twin-ring multi-slot equipment and Trader-to-Corpse non-merchandise stacks; moved personal admission after `PS_HUD_Loop`.
- Fixed ransack double-robbery/gold-delta races, epoch-scoped pickpocket queues, and peer-leave buyer cleanup.
- Stopped first-sight untouched-door packet spam, answered unloaded-door baselines, removed duplicate `infocmd` kind-10 door broadcasts, and hardened occupancy truncation/sender validation.

### Quests, dialogs, arena, reputation, and telemetry
- Fixed 44 confirmed quest, dialogue, infocmd, address, arena, enclave, reputation, witness, guide, telemetry, and shutdown issues.
- Made `patch.cpp` cover 8-byte `cmpxchg8b` page boundaries in `VirtualProtect`/`ReadableBytes`, relative jumps in `target + 5`, and bounded PE import-name scans.
- Cleared `knownTheft` in `RestorePlayerTheft` when a host dialog forgives theft and skipped oversize `StateEvents` names (`>= 63` bytes) without aborting baseline snapshots.
- Preserved nested `guards::Acting` theft scope for the same puppet and cleared telemetry `eCEntityProxy` maps before engine detach.
- Kept every `src/` file below 1,000 lines while hardening crash, trace, log, and watchdog diagnostics.

### Codebase and build quality
- Split oversized modules into focused slices and audit-specific policy/diagnostic units.
- Verified the audit cleanup against Win32 Release builds and focused engine-free tests.
- Added focused regressions for network transport, combat timing, saves, NPC state, leases, quests, UI, and replay boundaries.

## Builds 85–81

### Build 85
- Arena reload and duel fixes.
- Downed ground-hit protection and puppet-talk guards.
- Chest theft blame, animal blink, HP/item-quality sync, and gameplay telemetry.

### Build 84
- Client NPC relation/name-colour accuracy.
- HUD entity-slot crash and long-session leak fixes.
- Client arena and duel synchronization.

### Build 83
- Watched-chest theft confrontation and guard-state handling.
- Client interactable targeting improvements.
- Guard confrontation crash fixes.

### Build 82
- Guide/dialogue synchronization.
- XP baseline and ranged-combat fixes.
- Chest witness, relation, and fall-downed repairs.

### Build 81
- Shared NPC progression and versioned save-state handling.
- Combat focus and knockdown fixes.
- Thermos Batch 1 and Batch 2 cleanup integration.

## Builds 80–70

### Build 79
- Hardening and follow-ups for late-loaded NPC state and legacy `HitResult` compatibility.
- NPC presence/handback/despawn fixes, remote-sphere cave and PVS `PopFreeIndex` safety, camp/enclave reputation synchronization, dialogue holds/gestures and `FollowNpc` guiding, ghost-NPC suppression, obstacle-aligned knockback, and host streaming hot-path optimization.
- Incapacitated reset and reset/epoch edge cases hardened against stale state.

### Build 78
- Local hit-state reset and NPC module line-budget cleanup.
- Floating corpses now recover correctly after pause/unpause.
- Client NPC hit clip selection, 1x knockback, and stumble crossfade fixes.

### Build 77
- Combo knockback and mirror lifecycle integration.
- Client/melee incoming and outgoing hit parity, fixed melee contact raycasts, and host counter-attack retargeting.
- Duplicate blood-effect suppression on acknowledged hit echoes.
- Stumble/knockback synchronization and mirror lifecycle cleanup.

### Builds 76–71
- Thermos S1–S6 integration and 1k-line module splits.
- `patch::JumpTo` 24-byte `pushfd`/`popfd` guard, NPC death pop/ragdoll/NaN-bone fixes, and NPC playback jitter/post-handback stream-drop fixes.
- Immediate kill XP and shared-fight XP fixes.
- Merchant and personal-ledger save poisoning fixes.
- Corpse ground-anchor resets across world, lease, and life resets.
- Real-Steam host startup config-scope crash fix.
- Duplicate-stack hero restore and remote-player animation/foot-sliding fixes.
- Restore/epoch combat, XP, and puppet-clip state resets.

### Build 70
- Protocol 79 high-bandwidth direct UDP and four-lane Steam Sockets fallback.
- Range-coder removal and NPC/puppet synchronization fixes.

## Builds 69–60

- **69:** Protocol 78 NPC stream convergence, client presentation recovery, Steam send-rate configuration, and route diagnostics.
- **68:** Empty reliable pose-checkpoint crash fix for the ENet range coder and richer crash diagnostics.
- **67:** Steam networking robustness with ENet-owned data reliability over Steam unreliable datagrams, exact-command deduplication, per-queue retry isolation, buffer/rate ceilings, and send diagnostics.
- **66:** Client link-drop fix for 48-byte ENet CONNECT handling and unclassifiable outbound datagram routing.
- **65:** Steam invite lobby metadata race: wait for Steam lobby data before judging it.
- **64:** Steam transport send queue and NPC control fixes for real-Steam synchronization.
- **63:** Client admission guard, structural PUP verification, ghost-peer cleanup, and same-account rejoin.
- **62:** Invite-driven Steam connection using Join Game/rich presence/accepted invite/command-line flows.
- **61:** Non-blocking client template save with health/publish/retry/decide handling.
- **60:** Host-delegated game options with a versioned 105-byte Welcome/WorldHost snapshot while protocol remained 77.

## Builds 59–50

- **59:** SteamID-keyed companions, authenticated identity/persona labels, duplicate-account refusal, and legacy name migration.
- **58:** Genuine Steam PUP identity support alongside QA PUP copies.
- **57:** NPC relation and name-colour consistency across bind, range re-entry, revive, and downed-player rules.
- **56:** Client-first NPC ownership with host reconciliation and explicit ownership/refusal diagnostics.
- **55:** Host sphere gate restoration after native session stop and NPC-stream diagnostics.
- **54:** Cancelable client autoload on host load.
- **53:** Reddock ordinary-human/NPC admission and native/visual fallback handling.
- **52:** Client rejoin through a local template slot without in-place native New Game restart.
- **51:** Host save thumbnails keep the scene image instead of menu overlays.
- **50:** Client creature activation, native processing registration, playback retirement, and readiness grace.

## Builds 49–40

- **49:** Refined Alt-Tab/resource-reset diagnostics and downed convergence on restore.
- **48:** Invisible-NPC repair with character predicates, natural-weapon admission, and no missing-pose hide.
- **47:** Alt-Tab downed repair and focus attribution.
- **46:** Duplicate NPC identity reconciliation and census/quarantine.
- **45:** Local-player position authority guards and read-only native matrix/pose observers.
- **44:** Lease-import ROI-clear and processing dirtiness fixes without destructive ROI reset.
- **43:** Client NPC T-pose repair through processing candidates and presentation holding.
- **42:** Client rejoin recovery and native menu/loader reconciliation.
- **41:** Client movement hitch accounting and frame/stage diagnostics.
- **40:** Cooperative quit-game and native device/shutdown safety.
- **39:** Dialogue/trade/grace immunity using existing player state flag bits.

## Builds 38–30

- **38:** NPC save capture/restore obligations removed while preserving lease runtime behavior.
- **37:** Lease readiness tied to native processing and owner-hit handling.
- **36:** NPC lease handoff with wire v2 adoption, state renewal, and save-overlay compatibility.
- **35:** Client ground/fall immunity and HarmProtected policy.
- **34:** Host-managed save publication, failure reasons, atomic retry, and recovery fixes.
- **33:** Client combat windup contact gate and snapshot clock protection.
- **32:** HUD notices no longer activate/mutate GUI windows; unready NPC presentation removed.
- **31:** NPC retarget after knockout and immediate reliable opponent controls.
- **30:** Host-managed save container with account records, personal state, world replay, and lease overlay.
- **29:** Guarded bone stream packing and control/pose bandwidth improvements.

## Builds 28–20

- **28:** Host exit PVS/cached-cell drain fixes.
- **27:** Terminal hero and synchronous native ragdoll/game-over guards.
- **26:** Lease restore epoch ordering.
- **25:** Wildlife/NPC authority leases and interim save gate.
- **24:** Paired-save checkpoint transfer, native menu handling, and host own-record overlay.
- **23:** Ardea Goblin/Ogre suppression and removal coordination.
- **22:** Held-puppet talk/loot and robbery guards.
- **21:** Terminal hero The End flow and puppet presentation.
- **20:** NPC stream paced joins, congestion-aware stretch, and keyframe policy.

## Builds 19–1 and foundation

- **19:** PUP shim recognition and client world-entry diagnostics.
- **18–17:** Compact pose, relay, bandwidth, and paired-save iterations.
- **16–15:** Saving, targeting, and downed/interaction follow-ups.
- **14–13:** Downed state, target focus, and hit aftermath corrections.
- **12–11:** Loading, end-flow, and replay fixes.
- **10–9:** Combat, saving, and targeting integration work.
- **8–7:** Ragdoll, friendly melee, and end-flow fixes.
- **6–5:** First-pass double-hit, downed, and targeting cleanup.
- **4–3:** QA cleanup and manual isolation tooling foundations.
- **2–1:** Repository co-op, GUID, lobby/session, save, and network foundations.

For the precise file-level history, use the commit subjects and diffs in `git log`; this changelog emphasizes release-visible behavior and subsystem scope.
