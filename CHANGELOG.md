# Changelog

This log summarizes the releases and integration builds that changed gameplay, networking, saves, NPCs, combat, quests, Steam connection, or supported installations.

## Build 86

**8 audit slices, ~180 confirmed multiplayer fixes, and cross-slice hardening**

### Network, Steam, and transport
- Fixed 25 confirmed network, wire-codec, direct-punch, relay, and Steam transport bugs.
- Tightened frame/receive handling, ENet and Steam command routing, relay-ID/session handling, wire layouts, and test-copy transport diagnostics.
- Preserved protocol 77 while hardening large pose/control/stream packets and keeping old or incompatible peers explicit.

### Persistence, saves, worlds, and player state
- Fixed confirmed persistence, hero, world, clock, ending, map, save, and auxiliary lifecycle bugs.
- Hardened save/restore handoffs, world epochs, native world entry, personal ledgers, template publication, and post-restore state.
- Kept host-managed saves and client rejoin behavior compatible across world-change and reload boundaries.

### NPCs, companions, playback, and leases
- Fixed NPC sync, playback, corpses, companions, summons, presence, retargeting, handback, ownership, and despawn/lease lifecycle issues.
- Reduced unsafe replay and presentation state changes, improved bind-pose and inactive-NPC handling, and preserved host/client authority.
- Added clearer NPC stream, lease, ownership, corpse, relation, and hit/playback diagnostics.

### Combat, ranged attacks, downed state, and spells
- Fixed 24 audited combat, downed, ranged, shot, knockback, targeting, and spell problems.
- Improved melee contact, hit-claim, stagger, downed immunity, ground-hit, puppet-talk, and combat retarget behavior.
- Tightened ranged overlays, shot sweeps, fight memory, damage ownership, XP/kill reward paths, and player/party immunity.

### Remote players, movement, HUD, and focus
- Fixed remote puppet/body movement, position authority, transform observers, frame discontinuities, and local-hero write fences.
- Improved HUD window/list behavior, passive notices, entity-slot reuse, Alt-Tab handling, and focus/sight diagnostics.
- Preserved native controls for local heroes while keeping mirrored player and NPC presentation separate.

### Items, containers, trading, doors, and occupancy
- Fixed confirmed item, container, chest, pickpocket, trade, door, and occupancy problems.
- Improved personal loot, merchant/corpse state, chest witness/steal behavior, and story/door replay.
- Reduced stale or duplicate interaction state across reconnect and world-entry handoffs.

### Quests, dialogs, arena, reputation, and telemetry
- Fixed confirmed quest, dialogue, infocmd, address, arena, enclave, reputation, witness, guide, address, telemetry, log, trace, and shutdown issues.
- Added cross-slice diagnostics for quests, XP, worlds, doors, teleport/revive, and NPC/host/client state.
- Hardened crash reporting, shutdown/watchdog behavior, and diagnostic output without adding gameplay automation.

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
- Legacy `HitResult` compatibility and incapacitated-reset hardening.

### Build 78
- Local hit-state reset and NPC module line-budget cleanup.
- Client NPC hit clip selection, knockback, and stumble crossfade fixes.

### Build 77
- Combo knockback and mirror lifecycle integration.
- Melee hit responsiveness, stumble parity, and knockback synchronization.
- Incoming melee raycasts and host counter-attack retargeting.

### Build 76
- No standalone protocol bump; follow-up fixes continued the guarded bone-stream work.

### Build 75
- Compact pose and player-state bandwidth changes.
- 30/10 Hz pose delivery, exact key transport, and per-peer congestion-aware pacing.

### Build 74
- Targeting and replay corrections, ragdoll/downed follow-up, and compatibility cleanup.

### Build 73
- Compact NPC wire format and send-policy updates.

### Build 72
- Targeting fixes after kills and native NPC opponent replay improvements.

### Build 71
- Host-authoritative NPC synchronization, readiness/corpse fixes, and attitude synchronization.

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
