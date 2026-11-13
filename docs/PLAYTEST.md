# Checklist: first human co-op playtest

## Purpose

A first session for two players, one host and one joining player: joining, combat, companions,
knock-outs, dialogs, quests, trading, looting, separate regions, saving, loading and rejoining.
This checklist also covers recording bugs and collecting logs with `tools\collect_logs.py`.

### Ardea creature removal (manual, not yet runtime-verified)

- [ ] With the default host rule, start a new shared game in Ardea: no Goblins/Ogres on either screen;
  slaves, orc raiders and citizens remain, with no leftover bodies or attacks against vanished targets.
- [ ] Load an older Ardea save containing the creatures and repeat on both screens. Removal must not award
  XP/loot, alter quest counters or manufacture a death; already-recorded effects in the old save are not undone.
- [ ] Save/reload, leave/return, late-join/reconnect and shut down normally; removed mirrors must not return.
- [ ] Confirm quest-specific Goblins/Ogres and creatures outside the Ardea box still exist.

## Before you start

### Steam overlay (manual acceptance; no repair claimed)

- [ ] Before the next normal Steam launch, check both global and Gothic 3 overlay settings and the configured shortcut.
- [ ] Test that shortcut in the main menu and in the campaign. Record whether the overlay actually opens;
  a renderer-DLL attach log alone is not a successful test. Steam's renderer log names `Gothic3Final.exe`, not the launcher.
- [ ] If unavailable, send and accept an invite using the desktop Steam friends list and confirm normal co-op joining.
  Do not inject an overlay DLL, change patches or restart a live session just to collect diagnostics.

### Real Steam world synchronization (manual acceptance)

- [ ] On both endpoints expect `enet: host created ... channels=8 compress=on`.
  After invite/hello, the client's `steam-send: first ENet datagram` must show
  `command=2 command_header_bytes=48 action=classified-and-queued`; the host's
  first command is VERIFY_CONNECT (3, 44 bytes). Both logs must then show
  `enet: event ... type=connect(1)` and progressing data counters. Repeat departure/rejoin.
  Unknown classifications must say `unclassified_enet_bytes=... first_bytes=...
  action=sent-as-bulk`, not close the link. If it ends, collect the client's
  `connect: client link ended: reason=... steam_state=... enet_state=... elapsed_ms=...`
  and the host's `connect: host link ended` plus ENet event data/lobby callback flags.
  The menu names the reason and distinguishes a link lost within five seconds.
  The isolated build-65 copies would hit the same native CONNECT rejection;
  earlier hand-built packet tests and pre-build-64 game runs did not exercise it.
- [ ] Repeatedly accept the host's Steam invite, including immediately after the lobby opens.
  The client must connect without a false session/version mismatch. If metadata is delayed,
  its log should show `metadata pending` followed by `metadata arrived after ...: ok`;
  a timeout should explicitly mention missing Steam lobby data and allow another invite.
- [ ] Use the same rebuilt DLL on both real Steam accounts. Invite/accept normally,
  enter Ardea together and confirm `admission: ... in the host's world` on the host.
  Walk, turn, draw weapons, attack and cast on each machine; both screens must show
  the other player, including the host on the client.
- [ ] Check `PLAYERSTREAM player=1` on the client: `accepted` must keep advancing,
  `world_gate=1`, `same_world=1` and `puppet=1`. A
  `player-stream: ... action=cache-before-entry` line is expected if the host's
  keyframe beats the world-entry ACK; opening admission must not lose that metadata.
  `PUPPETLATENCY` distinguishes state age from displayed lag and reports the actual
  motion name. Puppet creation alone does not prove that the host is visible.
- [ ] Compare a healthy link with a brief transport stall, then restore delivery.
  `SYNCLATENCY`, per-NPC `SYNCTRACK` and `PUPPETLATENCY` must distinguish network
  transit excess from retained presentation backlog. Noncombat catch-up should drain
  a five-second delivered backlog within eight seconds, without rewinds or lost
  attack/hit/death transitions. Incoming attacks on the local hero retain normal
  contact timing; NPC roots and bones must hold, not predict through missing data.
  The adaptive buffer is capped at 500 ms, not enlarged to hide a multi-second
  transport queue. Confirm running, turning and stopping remain smooth afterward.
- [ ] Watch the same named NPCs fighting, moving, taking damage and dying on both
  screens for at least three minutes. Then separate into different regions and
  return. Shared NPCs must follow host state; client-only processing must still work
  outside host presentation. `admission-refused ... host presents it within its own
  reach` is expected for shared NPCs, not a failed world-entry handshake.
- [ ] Repeat with a healthy 30-60 ms RTT and a two-second stall. Unreleased shared
  NPCs must not cycle through `owned`/`yielded` or die from stale local combat.
  `NPCOWNSTREAM uncertain=1` delays new acquisition/reclaim, not established
  client-only simulation. Explicit release on a healthy link still permits native
  client ownership. If host-alive state contradicts a local corpse, expect a yield
  and `NPCRECONCILE`; life/HP/pose must return to host state without death/reward
  scripts. A two-second delivered noncombat backlog should drain within four seconds.
- [ ] Inspect the five-second `steam-send:` windows. `player_state_commands` must
  advance in both directions; `critical`, `definition` and `npc_pose` report accepted
  datagrams/bytes. `plain_bytes` versus `wire_bytes` measures actual lossless savings.
  Result 25 (LimitExceeded) and result 41 (Ignored/NoDelay; 16 is Timeout) must say
  `data_action=retained-for-retry`; `queue_bytes`/`oldest_ms` and their peak fields must recover after a burst,
  not grow continuously. `npc_updates_dropped=0` describes our local queue policy,
  not a guarantee against ordinary network loss.
- [ ] Check startup `steam-send: config` for verified effective `SendBufferSize`
  and both `SendRateMin`/`SendRateMax` at least 4194304, with `NagleTime` unchanged.
  `rate_policy=manual-headroom` means no adaptive bandwidth probe is assumed:
  only queued data is sent, but a slow upload can still be overloaded.
  Data must report `data_reliability=ENet-only`, `data_flags=37`; actual
  `steam_reliable_ok` should contain native controls, not the NPC/save stream.
  Correlate `steam-send: enet` throttle, reliable queues and RTT with the same
  window's result codes and `steam-send: session` actual Steam pending bytes,
  send-rate estimate and `queue_us`. A continuously growing queue is not acceptance.
- [ ] In the same real-account windows, collect `steam: path peer=... transport=...`.
  `relay-sdr` requires Steam's relayed flag and a relay POP; `direct-ice` requires
  a connected, nonrelayed SteamID peer with an address. `unknown` is not evidence
  of either route (including old stand-ins that do not fill connection info).
  Record `available=1`, `flags`, `pop_relay`, `ping_ms`, `est_bandwidth_bytes_per_s`,
  pending bytes and `queue_us` before and after NPC entry. Addresses in the
  description are redacted; the send-rate estimate is not measured upload capacity.
  `steam-send: config-current scope=global verified=1` must still show at least
  4194304 for buffer/min/max rate. This verifies global values, not a hidden connection
  override. `steam: transport-config` reads ICE/route penalties without changing
  them; `ice_enable=-1` means Steam's user policy, not proof ICE was attempted.
  A STUN size query returning `stun_type=4 stun_read_result=-3` reports the required
  string size: `stun_list_bytes=1` means empty; greater than 1 means configured.
  Server names/addresses are not logged and no STUN dependency is added.
  The second transport-config line reports unchanged MTU, native timeouts and
  encryption policy; changing those is not a fix for sustained send-rate pressure.
  Do not add raw UDP fallback until the reduced offered load and raised config
  have been tested and the remaining bottleneck is proven to be Steam-specific.
- [ ] Confirm `net: pose bandwidth` gains anchor ACKs/deltas after entry, compact kind 49
  is used from cold start on the reliable definition channel, and reliable queues/RTT do not climb
  indefinitely. Pending definitions must not repeatedly invalidate their own ACKs.
  `npc-control: ackaccepted` reports `created`, `pending`, `fallback` and `partial`;
  `npc-control: resync` includes the recovery reason and whether the token was evicted.
  Check late join, normal departure/rejoin and host save/load in the same session.
- [ ] Keep both players connected past the pose channel's 16-bit sequence rollover.
  Startup must show `enet: range coder guards active empty_buffers=skip`.
  After `net: pose sequence checkpoint queued`, expect
  `guard: ENet compression empty_buffers=... action=skip-empty-preserve-checkpoint`;
  the connection and pose stream must continue on the alternate channel, including
  later rollovers and rejoin/save/load. Build 66 crashed in
  `enet_range_coder_compress+0xac` on the checkpoint's null, zero-length payload,
  not in world cleanup (`last_step=world-change` was an older downed-flow marker).
  If another crash occurs, retain the matching DLL/PDB/map and dump; expect
  the selected minidump profile/flags and nearest known function beside module+RVA.
- [ ] Require `npc-converge:` with `baseline_policy=life-only` and
  `pose_ack_policy=anchor-only`. About one control baseline per NPC life is expected;
  changing animation descriptors must not repeatedly offer 615-byte baselines.
  `anchor queued` marks independent reliable keys, normally five seconds apart per NPC.
  The startup `ordered_setup_poses`/`ordered_setup_controls` counts should stop
  growing once control/dictionary ACKs arrive; `reliable_anchors` and client
  `received_anchors` continue advancing.
  Pose ACKs should succeed even with multi-second RTT, with small control components
  and predominantly deltas instead of repeated 588-byte controls/raw bone keys.
  Compare the same GUIDs, HP, appearance and animation on both screens; investigate
  repeated unconfirmed `npc-own: takeover`/`yield` cycles separately from GUID collisions.
- [ ] Repeat in the isolated copies with the optional finite-buffer fake Steam
  build: 262144 queued bytes and 262144 bytes/s, first without artificial lag, then
  with the existing 10-ms lag. The original unlimited fake DLL cannot reproduce
  Steam's send-buffer failure. Install test tooling only in authorized copies.

Application protocol is **79**; both players need the matching build. Reserved control
and pose flags now describe motion overrides and reliable pinned anchors, and independent
Rice keys permit zero references. Bone packing also selects reversible second-order
spatial prediction when smaller. Baseline/ID envelopes retain their layouts.
No NPC cadence, coverage or new precision limit is added. A sustained
queue-growth result still requires the new real Steam logs; a successful loopback
test alone is not real-Steam acceptance.

### SteamID identity (manual acceptance; no game/Steam automation)

- [ ] In the two isolated copies, record each `identity: join` SteamID and slot. Save a distinct client inventory,
  a looted personal chest/merchant purchase and a recruited companion; note a summon if one remains alive.
  Shut down normally yourself, rename the Steam display name, then restart and load that host save.
  The same SteamID must restore the character, personal state and companion; expect `identity: name changed`.
  Stand near the companion so the existing proximity rule allows restoration.
- [ ] Load an untouched build-57 save while using the old name. Expect `identity: migrated` for legacy
  companions, then save and reload: there must be no second migration for those GUIDs.
- [ ] Rename before first legacy migration, or use two identical names. Expect a host warning, retained
  legacy rows and no reassignment. Set `legacy_companion_owner = <actual SteamID64> <old name>` on the host,
  restart/load/join/save and confirm exactly the selected account receives the companion.
- [ ] Rejoin in a different slot and confirm companions and newly saved summons follow the same SteamID,
  not whoever occupies the former slot. A newer authenticated connection under one SteamID must replace
  the old connection, with a `peer: replace` log entry.

The stand-in Steam DLL's IDs 11/12 are stable test account IDs, not real SteamIDs.
Changing a real Steam profile does not rename those stand-ins; their persona input must be changed by the
test-copy maintainer between runs. Engine-free tests cover real-looking full 64-bit IDs, but do not verify
native restoration. Do not overwrite original saves when checking migration.

### Peer departure and rejoin (manual acceptance)

- [ ] With matching builds in the isolated host and client copies, join as player 2. Close the client and
  confirm the host shows `Clients: 0` and a `peer: leave` line. Rejoin and confirm player 2 is reused,
  the count is one, and a normal save succeeds. Repeat with a real Steam invite.
- [ ] Simulate a nonresponsive client without leaving its Steam lobby; confirm the host eventually logs
  `timeout-or-disconnect` and removes the client. A normal 30–60-second load must not disconnect it.

### Focus loss/regain and held knockout (manual acceptance)

- [ ] With host standing near fighting NPCs, Alt-Tab away for 1 second, then 10
  seconds, and return. Existing weapons must not all redraw; legitimate new
  combat/target changes must still draw weapons. Repeat by Alt-Tabbing the client
  and watching both screens. NPC redraw attribution remains unverified.
- [ ] Knock out the host through an enemy lethal hit while the client remains
  able to help. Alt-Tab away and back; the host must remain lying, not repeat
  the knockdown entry. Have the client talk to its puppet, choose **I will help
  you**, and confirm one healing potion pays for a successful revive.
- [ ] Repeat with the client knocked out. Also check an already-stuck knockdown:
  it should self-heal after 2500 ms without forward animation progress (5000 ms
  maximum entry time), or at the next safe module tick after a refused stand.
  Once lying correctly, no further task/motion repair should occur. If native
  presentation does not settle, retries are at least 2500 ms apart, never every
  native 100-ms routine poll.
- [ ] Repeat during a held pause and host-managed save/load, during dialogue and
  trade while standing, and with W held across Alt-Tab. Input, protection,
  payment, death and rejoin behavior must remain unchanged.

`focus: lost/regained` records process-foreground transitions with `gap_ms`,
`away_ms`, `native_dt_ms`, pause and local hero task/state/AI/animation.
`resource-reset` / `resource-restored` record the existing module callbacks
from native device recovery; these do not perform world cleanup.
`pause-resumed` and
`frame-gap` cover resumptions without an observed foreground transition (polling
cannot see an away/back cycle entirely between module frames). `focus: npc`
shows up to eight changed NPCs; `focus: settled` reports unique NPC GUIDs with
weapon/AI changes or sampled draw/equip motion starts/restarts over the next two
foreground seconds, plus actor replacements. These are sampled observations,
not proof that focus caused a draw; a short draw can fall between 250-ms samples.
`downed: held settled` is a one-shot module-boundary repair; repeated
`stand refused` is rate-limited and includes a cumulative count. None of these
diagnostics restores weapon states, alters NPC authority or changes the protocol.

### Quit-game teardown (manual acceptance)

- [ ] On both isolated host/client copies, select **Quit game** from the main menu
  disconnected, then connected in the lobby; repeat from the in-game menu solo,
  connected, immediately after loading, and during a cooperative save transfer.
- [ ] Returning to the main menu and rejoining must keep the process alive; only
  process shutdown arms the exit watchdog. Load the last committed test save after
  an interrupted transfer and confirm it remains intact.
- [ ] Expect exit within a few seconds, no crash/hang dialog, no new Gothic3
  AppHangB1/WER event, and no new `g3together-hang-*` files for a clean quit.
  Normal native teardown is unchanged. If the verified AMD/DX9 final device
  release stops progressing after successful world/save/transport cleanup, the
  watchdog logs `native DX9/AMD Release stalled after cleanup` and ends the
  process without DLL detach. This is a narrow teardown workaround, not a driver
  repair; unsupported signatures or other hangs still produce 12-second diagnostics.
  Use isolated copy saves only, never the protected user save folder.

### Players and installations

- [ ] Assign exactly two roles: host and joining player. The host starts or loads the shared campaign; either player may request a host save.
- [ ] Both PCs use the tested baseline: Gothic 3 from Steam, then **Community Patch 1.75.14**, then
  [Parallel Universe Patch **1.1.1**](https://www.worldofgothic.de/dl/download_678.htm), then the same G3 Together
  build and protocol version. Compatibility without PUP has not been established. Remove other co-op mods
  from `scripts`; preferably use a separate installation copy for co-op. See [installation instructions](PLAYING.md).
- [ ] Back up both players' Gothic 3 save folders. Use a clearly named test save during the session.
- [ ] Steam must run and be logged in on both PCs; both players must be Steam friends.
  This fork has no direct-IP/VPN connection or fallback. Human Steam play over the real internet remains unverified.
- [ ] Both players record their role and local time zone for later bug reports.
- [ ] Optional: set up with `Setup.cmd` from the package instead of copying by hand, and note
  anything it got wrong.
- [ ] Before using separate game copies, run the explicit copy-safety preflight. Pass two distinct copy roots:

  ```powershell
  python "tools\copy_preflight.py" --copy "<first Gothic 3 folder>" --copy "<second Gothic 3 folder>"
  ```

  Or set `G3_WORKSPACE` or configure `workspace/project.json` with the copies and absolute `protected_save_dir`.
  The preflight is manual and read-only; it does not launch or drive Gothic 3.
- [ ] Startup videos may be disabled or restored only as an explicit manual launch preparation:

  ```powershell
  python "tools\startup_videos.py" --disable --root "<Gothic 3 folder>"
  python "tools\startup_videos.py" --restore --root "<Gothic 3 folder>"
  ```

### INI and compatibility

Configuration file: `<Gothic 3 folder>\g3together.ini`.

Host:

```ini
role = host
transport = steam
language = en
```

Joining player:

```ini
role = client
transport = steam
language = en
```

`puppet_template` is optional and defaults to `PC_Hero`. `look = <character template>` (for example
`look = Diego`) is optional too: it is how you appear to the other player. Player exchanges use the native
trade button and no approval keys. A missing INI starts the mod in host mode,
so check that the joining PC has `role = client` before starting.

The example INIs use `language = en`, matching the messages quoted below. `language = de` and `language = pl` give
German and Polish messages; `language = auto` (what the setup writes) follows the game's text language. Players can
choose independently; the setting changes only mod messages.

- [ ] Both players have identical mod DLLs and installation files. The host checks Game.dll, Script.dll, Engine.dll,
  DLLs in `scripts`, rules archives, names and sizes of other Data archives, game options and the entire `[Game]`
  section of `Ini\ge3.ini`. Loose files in Data subfolders and custom difficulty sliders are not covered.
- [ ] Compare difficulty, alternative balancing, alternative AI, minimum hit duration, XP factor and Quick Loot.
  If joining is refused, read the reason in `g3together.log`.
- [ ] With matching `friendly_fire = 0` (the default), allied melee, projectiles and harmful spells must not cost victim
  HP or blocking stamina, stagger, knock down or apply statuses. Include followers and both attack directions.
  Healing and enemy hits must still work. Knock-out/revive checks below require an enemy, not your partner.
- [ ] The client starts New Game locally on first entry of each launch and re-enters after every host load through its own
  `G3T_client_template` slot. The host sends only
  that account's saved character before admission. A missing account starts fresh, never as the host's hero.

## A 90-minute session

| Time | Stage | Steps and observations |
|---|---|---|
| 0–8 min | Start and join | The host loads or starts the test campaign and exactly one other player joins. Check that both see each other, names match, movement is visible and no compatibility rejection occurs. Each player changes armor or helmet once: the other sees the change within a few seconds. When a player starts walking, their legs should move at once on the other screen instead of gliding. Open the map: the partner is marked there with their name; on the compass their mark points their way. |
| 8–18 min | Shared combat | Fight a small encounter near the host. Both attack the same NPC. Check that hits and deaths appear on both PCs and the actual killer earns the experience. Record quest state and counters; one success does not verify every kill-quest case. |
| 18–28 min | Recruit and dismiss a companion | The joining player recruits a suitable NPC. Check that it follows that player, appears on both PCs and is not simulated twice. The same player then dismisses it; both check that it no longer acts as their companion. |
| 28–38 min | Knock-out and help | In a safe setting, let one player be knocked down in a controlled way. Their partner first stands next to them for a few seconds: nothing should happen. Then the partner talks to them and chooses "I will help you.". Check the recovery, notice and consumption of one healing potion. Without a potion, both should receive a notice and the player should remain down. Optionally at the end, before reloading: with both players down, both die and reach game over. |
| 38–50 min | Dialog, quest, trade and loot | Speak to an NPC, trigger a known quest step, trade and loot. Then open "Show me your wares!" on the partner; dismiss the first tutorial with Ok. Using the mouse and native Trade/Swap button, take an item for free, then give one back without closing. The other player takes no action. Check both inventories immediately after synchronization, unchanged gold and preserved quality. Select gold explicitly and give it in each direction without items; check exact balance changes. Repeat with zero gold, close just after a swap, reopen, and check protected gear/quest items. Verify an NPC still charges its normal prices. These native controls require manual verification. |
| 50–70 min | Distance, separate regions and regrouping | Move apart. Beyond about 35 m, check that no distance message appears on either PC (no "Far from host", no "is far away"). While separated, each player fights a different NPC in their region; record whether results agree and whether anything appears to be simulated twice. Regroup and check that no message appears ("Back near the host." is gone) and that the world state matches. Accept a kill quest and kill a target in the joining player's region: the counter in the host's quest log should increase. |
| 70–82 min | Save and load | Make heroes visibly different (stats/LP, HP/MP/SP, gold, items/equipment, spells/quick slots and position). Change a quest/NPC/door; host-save and wait for "Co-op save complete". Change both heroes, request a client save, then host-load the earlier slot. Client starts New Game again and restores its account's character. Compare both saved heroes and shared world state; no native world file is transferred. Repeat quicksaves and companion ownership. World rewind is an acceptance boundary, not established by static tests. |
| 82–90 min | Rejoin and bug report | If time remains, test a clean disconnect and rejoin, in the same process or after restarting. Record campaign, hero record, NPC ownership and companion state. If something failed, collect both players' logs as described below and give them privately to the host. |

With only 60 minutes, prioritize joining, combat, helping up, recruiting and dismissing a companion,
and host saving requested from both machines. Test separate regions and rejoining in a follow-up session.

### Both down and revive regression

Use a backed-up, isolated host test save and client New Game re-entry, with an enemy, not friendly fire. Record observations
on both sides; these are manual checks, not a claim that the scenarios have passed.

1. With the host able to help, knock down only the client. Wait more than five seconds and check the client
   stays down despite native standing/healing animations or snapshots. Approach from the front, back and sides
   with weapons away: the lying partner's focus hint must say Talk, never Loot. Use the normal interaction key
   to open the dialog, not an inventory/robbery window. Also try quick loot: no window or items may be taken.
   The host chooses "I will help you.";
   check the client stands up and exactly one host healing potion is consumed. Repeat with the host down and
   the client helping; exactly one client potion is consumed. Also check the no-potion notice and no revival.
   Repeated interaction while the owner update settles must never consume another potion for that episode,
   even after five seconds. A later knock-out is a new episode with its own single payment.
2. Host-reload the test save and client-re-enter between terminal attempts. Knock both players down nearly simultaneously;
   then repeat with the host down first and with the client down first. When each sees the other as present
   and down/dead, both local heroes must scream/ragdoll once and show native game-over/"The End" text.
   Both partner bodies must be corpses. Wait at least ten seconds and try movement, weapon draw, attacks
   and helping: neither local hero may stand up, fight or revive. The native menu/load must still work.
3. Knock one player down while the partner is in a menu/loading or otherwise not known to be present. The
   absence is not immediate `all-down`: allow at least five seconds of continuous inability to help and
   expect `alone-grace` if no eligible helper returns. Repeat with the partner returning before the grace
   expires; the waiting player should remain down and be revivable.
4. With one player down, have the helper go down while speaking the help line or while its revive message is
   pending. Record the ordering: if the helper is already unable to help before the pending revive is applied,
   it must be refused and must not stand the target up; if application completed first, the earlier successful
   revive is not retroactively refused. Neither a late nor duplicate message may revive a terminal life.
   Record potion counts and whether both reach game over if both are down.
5. Reload and repeat the above, including both helper directions. As a smoke check, perform a quick trade
   in each direction and verify inventories, then host-save/load with client re-entry and verify each player's own
   character and shared world.
6. Knock out only the client, then walk the living host several hundred metres away and keep playing there
   for several minutes. The client must stay down indefinitely. Walk back without teleporting: the client's
   body must appear lying down even if its region streamed in only now, be focusable/talkable, and offer
   "I will help you." Select it once: the client stands once and exactly one host potion is consumed.
   Repeat with the host down and the client travelling; repeat across a zone/interior/region boundary.
   If testing genuinely different loaded worlds, the helper must return to the target's world before helping.

For each terminal life, inspect `g3together.log` manually for the decision reason `all-down` or `alone-grace`
and one `death-state-dispatch`, one `local ZS_RagDollDead entry`, one return with HP 0/AIMode 9/
movement 14/ragdoll/controller enabled, `native Kill confirmed`, and `death-state-complete` on each PC.
Reentry may log `skipped-terminal` but must not execute another death entry/Kill/scream. The native
script contract is `Kill(killer, victim)`: the killer is null and the victim is the local hero.
`end-state` samples run every 100 ms for the first five seconds and on later changes. They must retain
AIMode 9 and `ZS_RagDollDead_Loop`, not `PS_Normal` or a weapon task. A `terminal mutation`
reports the named setter/state boundary, attempted field/value, refusal and caller module/RVA;
the following `terminal caller` frames identify native/mod callers beyond the Script.dll wrapper.
`StartPuppetTerminal-before-SVM`/`after-SVM` changes isolate partner speech from local death.
If a crash occurs inside the call, record the last step (`CRASH end-flow last_step=...`), both logs
and any dump; `death-state-complete` will be absent. If the
bundled dump writer rejects detailed flags, the log should report a `MiniDumpNormal` retry; record any dump
write/rewind failure or empty dump. Do not trigger a crash just to check the diagnostic.

The `downed: helpers` lines at `alone-start`, `alone-cancel`, `alone-expiry`, and terminal decisions report
each candidate's connection/world gate, activity and pose ages, flags, world hashes, campaign epochs,
last-known distance, and `reject` reason. `activity-stale` means five seconds without engine activity,
not an out-of-range position. Explicit loading/hero restoration and no-running-hero/menu flags are reported;
an engine blocked inside loading cannot send a fresh flag, so its last activity eventually becomes stale.
Zones/interiors are not transmitted (`zone=not-sent`); a missing pose or a retiring/unavailable local hero
reports distance -1 (`local_position_known=0` when the local hero cannot safely be inspected).
An active distant helper must not cause `alone-start`/expiry merely because its pose is stale or its world differs.
`downed: puppet action=hold/release` identifies the owner's episode used for arrival presentation.
The 19-byte player activity message is bandwidth kind 50, sent reliably on the player-state channel once per
second and on life/loading/menu transitions; player/NPC layouts and gameplay world gates are unchanged.

### Downed player target eligibility

Use the existing isolated host (`test-install`) and joining (`test-client`) copies with a matching
coordinated build. Do not automate game launches or encounters. Runtime acceptance is still manual.

1. Let an enemy group knock out the client while the host stays able to help. Wait more than five seconds.
   The enemies must drop the client and immediately select another valid enemy with weapons still drawn,
   without a put-away/redraw pause. Include several NPCs attacking the same player. Check both screens:
   no further HP/stamina loss, harmful statuses, hit reactions or attack knockback on the client.
2. Repeat with the host knocked out and the client standing. Include creatures/animals, bow attacks,
   hostile magic and followers/summons. NPC-vs-NPC combat and attacks on the standing player must continue.
3. Revive through "I will help you." in each direction. Exactly one potion is consumed for the matching
   downed episode; no damage/stagger/knockback while rising or in the 1.5 seconds after standing.
   The revived player then becomes an ordinary eligible enemy again. NPCs must not remain
   artificially friendly or require a reload to re-engage.
4. Knock both down and verify immediate `all-down` game over with one native Kill per life. Repeat the
   unavailable-helper `alone-grace` check above. Smoke-check friendly melee protection, one hit per swing,
   partner trade and host save/load with client re-entry.
5. In an isolated encounter, knock out one player while the helper stays alive but beyond enemy perception
   range. With no other enemy near, the attacker should relax and stow its weapon the vanilla way, not
   acquire the downed player again. Return and revive through talk, then confirm ordinary re-engagement.
6. Include pre-existing poison/burning, harmful area spells, projectiles and environmental damage
   while held, in both directions. Healing/talk/trade and the terminal all-down Kill must still work.
   Compare an ordinary Sit/Lie knockdown against native host combat: synthetic contacts must respect
   the same protected pose; other stumbles/falls and normal combat remain vanilla.

The host startup should log `downed AssessTarget guard installed at native and script entries`.
`SCRIPT ONLY` is an explicit unsupported-native-hook diagnostic, not full native-guard confirmation.
Match `DOWNEDTARGET player/episode/flags` to the existing `downed: remote` and transition records.
Expect `frame-release`, `state-release`, `attack-release` or `cancel-release`, then `released`.
`DOWNEDBIND` shows target/history/alignment/inflictor pointers and combat membership before/after cleanup.
Startup also reports `native defeated-opponent selector verified` and the native/script `OnAttack retarget guard`.
`UNAVAILABLE` means a selector/hook signature did not match and needs investigation.
Each stopped-target transaction logs `DOWNEDRETARGET` with NPC, old/new targets, reason, task/state,
AIMode, combat membership and weapon mode before/after. `native-enemy` uses vanilla's own selection;
`alternative`/`visible-player` are eligible in-range/LOS fallbacks. A successful combat-to-combat retarget
keeps weapon mode (2-4 when drawn) and combat membership; `no-enemy` enters `ZS_Attack_End`, whose
native item-stow, combat release and wait run subsequently. An `assessment-failed` is an error, not success.
Player opponent bits, NPC opponent hash and AIMode are reliable immediate control changes, captured
after release; passive client mirrors replay their buffered opponent/motion, never an attack AI task.
`excluded` is a player-id bitset, not a list of NPCs. Held puppets keep native AIMode 10 and the lying
animation on both copies: mode 8 would make normal interaction choose loot before talk assessment.
Combat exclusion still follows the owner's flags, so on revival the excluded player bit clears.
`attitude-refused`, `perception-refused` and `assessment-refused` explain rejected acquisition.
`DOWNEDHIT` records blocked in-flight judgement/status/HP/visible-contact paths before native effects.
`GROUNDHIT` records every local ground/recovery damage/contact event without sampling: path, attacker,
HP before/after, episode, held/terminal, rising/grace, AIMode/AniState and native task/state.
`vanilla-prone-reject` identifies the restored synthetic melee gate; `reject-before-effects` is held/
recovery immunity. A contact record alone does not prove damage; check its HP pair and decision.
`downed: recovery standing` starts the 1500 ms post-standing grace. Startup must report
`ground HP guards set=1 add=1 value=1` and native/table wrapping for `OnTouchDamage`.
The other PC reports `downed: puppet recovery begin` / `standing` from the owner's existing snapshots.
`downed: recovery waiting ... reason=no-current-motion` is an observation failure: protection stays
conservative until standing is known; do not accept a test in which recovery never reaches `standing`.
Repeated acquisition/hit refusals are limited to the first 40 records and one sample per five seconds;
actual binding/life transitions are logged independently. No QA command or control is needed.

### NPC relation / name colour (both PCs)

The tag colour is read every frame from the NPC's `AttitudeToPlayer2` (red 4/5, orange 3/6, green 1, tan 2, pale cream 0, dead
tan). Only the game's scripts store it, as the NPC's relation to this machine's player; knocked-down state, a puppet standing in for
the player and a client mirror's own scripts must not change it. The host sends it in the NPC record; the client writes it into the
mirror every frame (also while a mirror is held or unsampled) and answers the scripts' own question with the host's value.
`relation:` lines are written by both PCs, no QA command: `summary` (reason, down bits, per-attitude counts of live NPCs within
60 m, fix counters: `writer_queries`, `scope_cleared`, `downed_not_refused`, `mirror_answered`, `healed`), `npc` (the six nearest:
state `host`/`native`/`lease`/`mirror`/`mirror-held`/`mirror-released`, `stored`, `colour`, `hostile` = red, `truth` = the relation
the scripts would store now, `host`/`shown`/`written` on a client, `sent` on a host), `transition kind=knocked-down|revive`
(followed by summaries 1 s and 4 s later), `change` (a tracked NPC's stored value or state moved), `heal` (stored 0 on a live NPC
replaced by its truth) and `mismatch kinds=...` (`native-drift` the client's own script changed a mirror and it was repaired,
`unapplied`, `stale-none`, `unsent`, `truth-differs` informational). Startup must report `ai: name-colour writers (GetFocusNameAttitude
call site) verified`.

1. Stand near hostile orcs and creatures on both PCs; note red names on both screens. Knock the host down (all the time it lies
   there), then revive it: names stay red/white exactly as before on both screens. Repeat knocking the client down.
2. Walk the client out of the host's range and back in: hostile NPCs keep red names; none turns white while it is presented.
3. In both logs, `relation: npc` lines of one NPC keep the same `stored`/`colour` through `transition` lines; no `mismatch` with
   `stale-none`/`native-drift` after the revive summaries. A `stored=0` NPC with `truth>0` must be `heal`ed within 2 s.

### Dialogue and trade immunity (both PCs)

Use coordinator-installed matching DLLs in the host `test-install` and client `test-client` copies.
Do not treat policy tests/builds as native gameplay acceptance.

1. Keep an attacking NPC near the host, then enter dialogue with a different NPC. Throughout spoken
   lines and the option window, require unchanged HP and no stagger/knockdown/death on either screen.
2. Put the client in a merchant's trade window while an orc attacks. Require the same immunity on
   both screens. Repeat with direct partner trade, including its first tutorial and a covering menu.
3. Close the window. Require 1.5 seconds of grace, then ordinary damage/reactions from eligible
   attacks. Repeat with Esc, interrupted/aborted dialogue and dialogue-to-trade-to-dialogue transitions.
4. Swap host/client roles and repeat. Include arrows, hostile targeted/area magic, creatures,
   existing poison/burning, fall/touch damage, and the other player's attacks with friendly fire on.
   Keep the other player far away too: immunity belongs to each hero, not a shared proximity bubble.
5. Check world load, hero restore and peer disconnect/rejoin do not retain a closed window's
   protection. Genuine open windows remain protected; no stale routine task should grant immunity.
   Healing and dialog-triggered combat after grace must still work; enemies remain natively hostile.

Startup must report `native interaction pages ready`; an `ANOMALY interaction page layout mismatch`
means overlay/tutorial coverage is unavailable and is not acceptance. `PROTECTION` and
`PROTECTEDHIT` name `in-dialogue`, `in-trade` or `interaction-close-grace`. `GROUNDHIT` includes the
same `protection` reason and HP before/after. The owner sends bits5/6/7 in its existing playing flags
for dialogue/trade/grace; absent-playing loading/menu bits are not immunity. Remote protection uses
ordered kind50/pose activity, expires after five seconds without owner evidence and clears on
disconnect or world mismatch. NPCs may finish visible swings; no counted hit or reaction may result.
Old visible-contact logs prove an attack was shown, not that damage was applied.

Require `containers: player loot guard installed at native and script entries` on both copies.
`SCRIPT ONLY` explicitly means direct native loot calls are not guarded on that binary.
`DOWNEDINTERACT` records the actual puppet's role/player/episode/flags, held truth, native AIMode,
Down/Dead predicates, HP, inventory presence, focus, action and result. `publish-talk` should show
`held=1 ai=10 native_down=0 native_dead=0`; the owner still remains natively down on its own PC.
Normal interaction should record `talk-attempt` followed by `talk-assessed result=1`, then the existing
help-choice, potion-payment and accepted-revive lines for the same episode. `loot-rerouted-to-talk`
identifies an older queued loot action safely diverted before opening HUD loot.
`loot-refused`, `rob-state-refused`, `rob-transfer-refused` and `pickpocket-refused` identify protected paths.
These diagnostics observe real calls, never invoke talk assessment during a focus scan, and are bounded
to the first 40 records plus one sample per five seconds. Inventory presence is not permission to loot.
While waiting, NPCs must ignore the body rather than rob it; ordinary nonlethal guard knock-outs still
allow the existing native half-gold robbery. Check corpse/chest loot and partner trade remain normal.

### Host-managed save/load acceptance (protocol 79)

### HUD notices without movement interruption

Use matching builds in the two isolated copies; this is a manual check, not a game-launching test.
Keep the client's forward movement key held while the host goes down and `Host is down` appears.
Repeat walking and attacking, then reverse roles for `Client is down`. Revive using the ordinary help
option: one potion is consumed, the held owner stays guarded until paid revival, and the helper's
input is not reset by the help notice. Repeat with save request/result and connection lost/reconnected
notices. An actual native save/load pause or the owner's knockdown/get-up is not a notice side effect.

All existing texts, kind codes, colors, positioning and six-second fade remain unchanged.
Require `hud: passive native message list ready` in the log. An ABI mismatch or presentation-unavailable
line is explicit failure/deferment, not a successful display or permission to use a modal fallback.
Each existing `hud: [kind] text` now has a `hud-motion: notice=N` group with `before`, `after`,
`100ms` and `500ms` snapshots. Compare task/state, AI/action/animation, position/rotation/velocity,
movement control/enabled flags, `held/terminal`, `can_move`, session/device/GUI input, GUI active/focus
handles and camera identity. The immediate before/after must not change those states or handles;
position should continue advancing with input held. `display_us` measures only the native text insertion,
not diagnostic log I/O. Delayed samples report their actual elapsed time; a load/body replacement
explicitly cancels them instead of inspecting an old hero.
smoke-check partner trade, native/nonlethal knockout, terminal death, save/load, NPC leases/mirroring and
join/rejoin. No QA command or negative-control switch is needed.

Before checking saves, require `persist: save and load hooked` on both copies. It reports the five
verified save/load entry points; `save UI hooked` also identifies the verified quicksave handler,
menu-new call, native availability predicate and screenshot helper (the scene thumbnail is chosen when the save
is requested, never recaptured while the save page is drawn). World.DeleteSaveGame at RVA 19C0D0
is also signature/export-checked so native slot deletion removes the container.
`persist: unresolved <function> ...` means no save/load claim is valid; preserve that line and native identity.
PUP owns the original world-load entry: the verified `E9 ... 90` entry and `87 0C 24 51 E9 ...`
bridge lead to its `paru_s2.dll` companion. The mod leaves that entry untouched and observes the
verified HUD load call instead; `native reverified before use ... owner=PUP/paru_s2.dll` confirms
the preserved path. The other four entries, installed hooks and HUD call edges are also checked
before use. The companion is selected by the shim receiver's actual module address, not by requiring
the main `Script_paru.dll` to be visible during startup. `PUP chain ... runtime verification pending
first use` confirms startup identity; `PUP ready ...` confirms the adapter/import/semaphore preflight
before native use. An unknown initial or later hook disables co-op saves and cancels pending coordination,
with the function name and observed bytes; the mod still runs. PUP installing after this mod is
accepted only when the same verified bridge and companion identity match.
If rejected, preserve the `PUP phase=... stage=... entry=... bridge=... receiver=... live=...` line.
The jump distances vary with ASLR/private allocation; neither rel32 is compared to a captured literal.
The optional engine-free checks `G3T_GAME_DLL` and `G3T_PUP_DLL` read selected CP 1.75 Game.dll
and PUP companion files and verify unique patterns, exports/call edges, operands and ASLR without
loading or executing either DLL.

Use only backed-up isolated profiles, after coordinator deployment. Do not use the protected user save folder,
test hooks, bots or automated engine launches. Static tests/builds do not establish two-copy success.

1. Start the host world and client **New Game**. Make visibly different heroes: stats/LP, HP/MP/SP,
   gold/items/equipment, skills/spells, quick slots and positions. Change a quest, door and NPC.
2. Host menu-save. Require "Co-op save complete" on both copies. Host logs one HUD/world save,
   `lower_calls=1 lower_ok=1`, `host container published ... network_world_bytes=0`, and completion elapsed_ms.
   Client logs only the character/personal-state envelope and host result; **no client native save, native world transfer or manifest**.
   Measure the requested 1–2-second latency; it is not guaranteed by static validation.
3. Change both heroes, request a client menu-save, then repeat host/client quicksaves back to back.
   Same-slot simultaneous requests should share one transaction; a different active slot refuses busy.
   Each completed save has one host `.g3t` plus recoverable native menu caches, not new client world files.
4. Continue playing, then **host-load the earlier slot**. Require HUD and exactly one lower PUP-chained load success.
   The client re-enters the host's world on its own through the `G3T_client_template` slot (the game's own LoadGame; no menu stop, no New Game).
   It waits for account-exact character/personal-state restoration
   and the host calendar plus all story/runtime/nearby shared-door baselines, applied cutover and entry confirmation.
   Compare both distinct saved heroes and shared quest/NPC/door/loot state. Recheck the first shared XP reward.
   Check quest counters/timers and no repeated quest XP, gold, reputation or item rewards. Loot a chest/corpse
   and loose item, buy/sell with a merchant, save, reload and revisit after leaving the region: no rerolled or
   duplicated stock. Shared loaded doors must match even if the host has not changed them since loading.
   Test a saved client position far from the host, and change a quest/door while that client is joining:
   no missing baseline, stale state, skipped motion mapping or reward-producing replay. Calendar/activation dates must agree.
   Clients receive no native serialized world; arbitrary dynamically created loose drops and temporary effects
   remain an acceptance boundary. Any mismatch blocks campaign acceptance.
5. Disconnect/rejoin in the same live epoch: keep the live client character. Re-enter after a host load:
   restore the saved account record. A different account gets a fresh hero, never the previous slot occupant.
   A local backup is not imported automatically, even if it contains later progress.

6. Host-load a plain native or intact G3T1/G3T2 save, including `G3T_Sandbox`. It must reach native load without
   a manifest-prefix error. Also load an earlier G3TSAVE3 container with no personal block; use native/fresh defaults.
   Old unbound client records are not silently migrated; absent accounts start fresh.
   Client Load must explain "Only the host loads saved games" without invoking a native world load.
7. Test combat separately from dialog/control/dead/defeated/downed and unsettled player exchange.
   Combat/drawn weapon/water/menu pause alone are not native save restrictions. Specific refusals clear on retry.
   Host-alone save retains absent accounts' preceding records; disconnection during capture cancels safely.
8. On disposable backed-up slots only, test interrupted publication, malformed/truncated `.g3t`,
   checksum damage and missing native caches. A damaged container must refuse before native loading;
   intact containers repair caches. `G3T_pending_*` slots must refuse. Existing good containers survive precommit failures.
   Delete a slot through the host native menu and check its `.g3t` disappears; interrupted deletion may retain a
   complete orphan container, which can be recovered, not silently loaded as a half-save.
9. Verify equipment base/bonus splits and current HP/MP/SP, no doubled bonuses or host inheritance.
   Temporary-effect ownership/expiry and exact native inventory mutations remain manual acceptance.
   Check option parity, NPC mirroring, companion leases, trade, downed/revive/end, XP sharing and kill-once.
10. Keep the host away while the client owns an actual wolf/scavenger lease (distance alone is not proof).
    Host-save and client-request-save must complete with no lease warning, including live/dormant/pending
    lease states. Continue moving/fighting, then hand back: no new freeze, duplicate body or repeated XP/Kill.
    Host-load the slot and re-enter the client normally: only host-native NPC state returns, not client-only
    injury/death/adoption. Also load intact old type-256 v1/v2 overlays and `npclease1 ` sidecars: no overlay
    error or stale authority. New saves must omit type 256. Corrupt container checksums must still refuse.
    Normally save with both players together; this far-apart case verifies the deliberate durability limit.
11. Save thumbnails (host Load list). Make each of these saves on the host and open **Load**: every new entry
    must show the 3D game scene like an older native save, never the menu, a page or a black frame:
    a) in-game menu, **Save** (new slot) and overwrite of an existing slot; b) quicksave with no window open;
    c) a client-requested menu-save (host walking in the scene, then repeat with the host's own menu open);
    d) quicksave with the inventory/journal open, after any earlier menu or quicksave in the same world (shows that
    scene, not the page; without one the log says `source=fallback` and the page may show); e) two saves back to back.
    The host log shows one `persist: save thumbnail source=...` line per save, before `host save requested`:
    `source=retained menu=1` for menu saves (the picture native took when the menu opened, scene frozen by the
    pause), `source=capture` for quicksaves and client requests, `source=retained page=1` with a window open, and
    `source=fallback` only when native holds no scene image. HUD.SaveGame is never preceded by a new capture.

### Client rejoin through the template slot (replaces "without relaunch" New Game; builds 50+)

After coordinator deployment, use the two already configured isolated host/client copies; these are human
checks, not an automated engine/Steam test. Do not use the protected user save folder. The client isolated profile's
save folder gets one new slot, `G3T_client_template.*` (`.g3savcp`, `.g3savcpdat`, `.g3t`), nothing else.

1. Fresh client launch: host starts the test campaign; client chooses **New Game** (the only manual step; with autoload on it starts by itself), joins and receives only its own
   account record. About 2-4 s after the New Game return the client log must show, in this order: `template: save start job=first-entry origin=new-game`,
   `template: native HUD.SaveGame slot=G3T_client_template ok=1 lower_calls=1 lower_ok=1 ms=...`, one or more `template: settle +NNNms header=... world=missing(2) ... world_tmp=...`
   lines (the engine writes the world half ~0.3 s later), `template: ready slot=G3T_client_template origin=new-game header=... crc=... world=... crc=...` and
   `template: state=valid why=- origin=new-game ...`, all BEFORE `persist: character restore`. The isolated client profile must now hold
   `G3T_client_template.g3savcp`, `.g3savcpdat` and `.g3t` (no `.tmp`). Make the heroes visibly different and host-save; wait for **Co-op save complete** on both.
2. With the client playing, host-load that save. Expected client log: `client enter state=queued reason=host-world-changed`, `template: route=template ...`,
   `state=cleanup ... template=valid origin=new-game`, `state=native-load`, `persist: native invoke=World.LoadGameWorld`, `state=returned ... lower_calls=1 lower_ok=1`, then the usual
   `character restore` and admission. The client must land in the host's world with its own saved character, never closing Gothic 3 and
   never showing "not responding". Repeat the host load 5+ times in a row (including loading the same slot again): **no restart of Gothic 3 at any point.**
3. Host loads while the client is in the menu opened during play, in a dialogue/trade (deferral notice, then entry after closing),
   with a client save awaiting its result, and during the client's own loading screen: the entry waits for the boundary, no nested teardown, no crash.
4. Choose **New Game** from the client's in-game menu: it queues the same template entry (no native process exit, no freeze). A **New Game**
   from the title screen after a world was already entered in this launch enters through the template too (or shows the exact reason when no
   template exists and there is no world to make one from); the native New Game is never run twice in a launch.
5. Delete `G3T_client_template.*` in the isolated client profile (only those files; the client must be in the world, hero standing idle): within
   ~15 s the log shows `template: state=missing why=no-files`, `template: save start job=background origin=world attempt=...`, `settle`, `ready ... origin=world`; then host-load:
   the client enters through the new template with no restart. Repeat the deletion and host-load before the 15 s are over: the log shows
   `template: route=create ...`, `template: save start job=host-load origin=world`, `ready`, then `state=native-load ... returned`; no restart.
   (A carried-hero template only starts accounts that the host has a record for; for a new account see 6.)
6. A campaign with no record for the client's account: the template's fresh hero is used, never the host's hero; nothing overwrites the earlier record.
   With an `origin=world` template and a host save without a record for the client the log shows `template: route=... why=carried-hero-new-account`
   and one message with the reason; after a restart the first New Game rebuilds a fresh-hero template.
7. Review both logs and the engine log (`ge3log*.log` in the isolated profile): no `FATAL`/`still cached` lines, no `world-entry: main thread stack`
   line (that line is printed only when a native world operation runs longer than 15 s and gives the main thread's module+RVA candidates).
   No native world transfer to/from the host is allowed; only the client's own template is loaded. Static policy/native-byte tests and the Release build
   do not establish these visual/native acceptance results.
8. Template failures name their reason: make the template fail on purpose (isolated client profile only, e.g. hold `G3T_client_template.g3savcpdat.tmp`
   open from another program or make the folder read-only) and the log shows `template: failed reason=template-busy|save-access-denied|template-no-world ...`,
   `template: remove reason=rollback clean=1` and one HUD/menu message `Rejoin template: <reason> (system error N)`; the slot is clean afterwards and the
   next attempt starts from scratch.

### Client autoload on host load (`client_autoload`)

Human checks with the two isolated copies after deployment (client INI default `client_autoload = on`; automated tests only prove the
decision policy and source/native-byte wiring). Never use the protected user save folder. Keep both logs; the log lines to look for are
`persist: client autoload status=...`, `approved entry`, `entry started|failed`, `canceled ... by=Esc|manual New Game`.

1. Client in the world, host loads a save. The client freezes the stale world (`client enter state=queued reason=host-world-changed`), shows the HUD
   notice *"The host's world is ready. Entering automatically in 4 s with your saved character. Press Esc to stay in the menu."* once the host
   world is playable, then `approved entry`, `state=cleanup`, `state=native-load`, `state=returned`, `entry started` and
   `admission confirmed` on the host, with no click. The hero must be the account's saved character, never the host hero.
2. Client at the title screen (start it after a previous launch made the template, or on the first launch with no template): same countdown in the
   menu text, then the template entry (or, first launch only, `via the first native New Game`, which also saves the template).
   Host starting a new campaign: *fresh hero* wording, and only the last of the three New Game epochs is entered.
3. Host loads repeatedly (a second load within about 10 s): the countdown restarts for the newest epoch; no crash; `entries` never above 3 per epoch.
4. Host quits to its menu: the client stays where it is (a played world is frozen and outside admission, with the notice *"Waiting for
   the host's world..."*) without entering; when the host loads or starts a world the countdown starts by itself.
5. Cancel: Esc during the countdown (menu text *"Automatic entry canceled..."*, log `canceled ... by=Esc`); the client stays in the menu
   (or in the frozen stale world, outside admission). **New Game** then enters on demand (`by=manual New Game`). The next host load asks again.
   Set `client_autoload = off`, restart the client: no countdown; the host load notice says to choose New Game, and New Game works.
6. Client joins after the host already loaded (start the client last): once joined, the same countdown and entry.
7. Edge states: host load while the client is in dialogue/trade (countdown, then the deferral notice until it closes), downed or dead (no delay),
   awaiting a co-op save result, or inside its own New Game loading screen (finishes first, then the newer epoch is followed). A mismatched
   install/protocol must refuse and never start an entry.
8. Failure: delete the template files while a world was already entered: the client makes the template from its current world (`template: route=create`)
   and enters; only when no world exists to make it from (title screen after an entered launch) the message names the reason (`template: route=impossible why=no-world-to-save`).
   An entered client the host never admits shows *"Connected to <host>, but not in the host's world yet..."* after 8 s, then is corrected (countdown, return to
   the menu, template entry after 60 s; at most 3 times per host world, then *"automatic entry failed repeatedly"*).
9. Never silent (log lines `admission: ...`; see PLAYING.md): (a) connect the client first, host later; (b) host first, client later; (c) while connected,
   choose New Game by hand in the client (admitted within seconds, `admission: admitted`); (d) make the client play a world while the host is in its
   menu, then let the host start a world: `admission: mismatch detected`, the 4 s countdown *"You are in a different game than the host..."*, then
   `correction started/finished` and `admitted`; (e) `client_autoload = off`: *"Connected to <host>, but not in the host's world (client_autoload is off)..."*
   in the menu and repeated on the HUD every 20 s, no automatic entry; (f) Esc during the countdown: the *"automatic entry canceled"* text stays; (g) host
   in its menu: *"The host is in the menu or still loading..."* and the host's lobby line shows `Connected: n | in your world: m`; the host's HUD shows
   *"<name> connected, waiting to enter your world."* then *"<name> is in your world."*; (h) an install whose native hooks fail shows *"Host-managed
   saves OFF: ..."* on both machines.

### Targeting after deaths

Use the same encounter/save on both copies and compare the joining player's targeting with the host's native
targeting. Repeat at least ten kills, including several enemies together; keep another living enemy in view.
Keep the joining player's weapon drawn throughout each series: do not sheathe between kills.

- [ ] With melee weapons drawn, kill the focused enemy while continuing the next swing. Repeat with the host
  and another NPC landing the killing blow, and with an enemy dying behind the joining player. The dead enemy
  must stop owning combat focus/alignment; the next eligible enemy's name, health bar and swing aim must agree.
  Selection still obeys the native view cone, range and line of sight, not simply the nearest NPC.
- [ ] Repeat with focus lock enabled, then with bow/magic aim. A death must not leave the old lock or auto-aim
  behind, interrupt an unrelated swing, force weapons away, or end a fight while living enemies still attack.
- [ ] Sheathe weapons and look at an unlooted corpse. Its name and native loot focus must remain available.
  A corpse can retain the native empty health bar; it must not show living HP. Loot it, look between corpses,
  living NPCs and items, then draw weapons again. Do not treat legitimate corpse loot focus, an item focus,
  the native HUD fade, or no eligible enemy in view as a targeting failure.
- [ ] Leave an enemy out of view, have it die, then return. Also leave/re-enter the area and reload the test save
  to exercise recreated NPC instances. No old-life combat/alignment binding should survive; a new living
  incarnation must remain normally targetable. Check XP/kill credit still goes once to the proper killer,
  with the host authoritative.

The client log records `CLIENTDEATHSTATE` (native death publication), `CLIENTDEATH` and bounded `DEATHFOCUS`
records: `before`, `selected`, `next_frame`, up to four `changed` selections, and `settled` after five seconds.
Compare `instance`, `elapsed_ms`, `hero_dead`, task/state, focus pointer, lock, target, alignment and the separate
auto-aim target pointer. `script_priority` is the game's script result for the actual focus, not a distance-based
candidate guess; native category/range/LOS rules still apply. In melee a corpse must not remain the selected
combat target. With weapons away a corpse may legitimately be selected for loot. Record exact kill times and
whether the hero was knocked down/dead, looking away, or using an item/dialog when reporting a mismatch.

`FOCUSSTATE` also records weapon/combat transitions and, while a weapon is drawn, samples once per second
for 30 seconds after the latest death. Match its `seq` to `FOCUSBIND` (all hero target/attacker proxies),
`FOCUSLOOP` (SPU target, alignment and auto-aim), `FOCUSQUEUE` (pending native action targets),
`FOCUSVICTIM`/`FOCUSVICTIMBIND` (native corpse and replicated life/bindings), and `FOCUSREL`
(NPCs targeting/attacking the hero or the latest victim). Identities include pointer/full GUID.
A raw queue/SPU/aim pointer outside the engine's processed set is reported as `not_processed`, not dereferenced;
an unsupported native queue layout is explicitly reported, never read speculatively.

`FOCUSSCAN`/`FOCUSCAND` list the nearby interaction ROI candidates, the last actual native collection
(`native_listed`), and nearby mirrors missing from that ROI. The native cached list is read only after
verifying its collector's layout; unsupported/invalid/unreadable layouts are explicit. It is the last
collection, not a new scan, so compare `scan_mode`, `autocollect`, lock and the current ROI when it is stale.
They report registration/collection, native type and cone/range/LOS/priority checks, corpse loot, flags,
HP/AIMode, each mirror's buffered opponent hash and actual native target, and equality with the hero's target.
`native_check` includes priority; it is an individual admissibility check, not a forced selection or a claim
that a candidate wins over all others. `FOCUSRULE` adds attitude, party, last-fight and latest-versus-presented
state. Normal cone/LOS/loot/user and category rules still apply. No diagnostic performs a focus scan or changes
targets, locks, scripts or queued actions.

For the missing-opponent defect, look for a living combat mirror (`ai=3`) with `host_target` nonzero but
`target=null`, the hero's `current=null`, `same_target=1`, and `script=-1`: native melee priority rejects even
two null targets. `FOCUSTARGET ... result=missing_or_ambiguous` explains a hash that cannot be safely resolved.
With replay working, the native target must resolve to that reported NPC and the false null/null equality
must disappear. If it does not explain an occurrence, retain the whole sequence through sheathing:
a corpse returning in `FOCUSQUEUE`, SPU/current target, lock or aim is a different mechanism, not proof of
this one. Existing logs did not contain candidate target identities, so the historical symptom's attribution
still requires the manual encounter; builds and replay tests alone cannot establish that.

### Optional: 30 more minutes for the features of 0.3.0

| Stage | Steps and observations |
|---|---|
| Host not reachable | Before the session, accept the host's invite (or Join Game) while the host is offline or its game is closed: the top menu line says the host is not reachable or did not answer, and nothing keeps trying by itself. Accepting again once the host is ready connects. No address or port is requested. |
| Overheard dialog | One player talks to an NPC, the other stands within 20 m with the game's subtitles on: they hear the lines and read them at the top of the screen, in their own game language. |
| Theft in front of a witness | The joining player takes an item that belongs to a town while a guard or citizen of the host's region watches: that NPC goes after the joining player, as it would after the host. |
| Robbed when knocked out | Let a guard knock the joining player out (not kill): they stand up by themselves after a while, the guard takes half their gold, and they see "<Name> robbed you of N gold.". The host cannot loot them meanwhile. |
| Reputation `together` | Only if the host sets `reputation = together`: after the theft above, the same NPC refuses to talk to the host too. |
### Optional: 20 more minutes for the features of 0.4.0

| Stage | Steps and observations |
|---|---|
| Brawl | The joining player beats a citizen of the host's region until they fall: they are knocked out, not dead, and the joining player gets the experience; the town's murder count does not change. A blow on the one lying there kills them. |
| Murder | After such a kill in a town that is not fleeing, the town treats it as a murder (guards react, the crime shows in the reputation dialog), with either player as the killer. |
| Pickpocketing | One player picks a citizen's pocket (needs the skill): the other can no longer pick the same citizen. |
| Seat | One player sits on a stool or bench; the other tries the same seat: "<Name> is using this." and they do not sit down. When the first stands up, the other can sit. |
| Animal form | A player turned into a wolf (spell or potion) fights an enemy: hits and kills count for that player, and enemy hits hurt the wolf. |
| Soul travel | One player casts soul travel near enemies and floats the ghost toward them. Known: the other player sees the ghost moving, not the body left behind. Note whether the enemies attack the ghost, and what happens to the body. |
| Rare places | If you come across an Innos or Beliar statue to pray at: one player uses it while the other watches, then the other tries the same. Note what each of you sees. Test the orc boulder in Faring (a strength test; lifted by one, it counts for both) and pirate treasures (one is 46 m from Tarok in Faring, dig with a shovel; the loot should be there once). |

### Optional: the features of 0.5.0 and scenes that need manual setup

| Stage | Steps and observations |
|---|---|
| Witnesses | The joining player beats a citizen in a town of the host's region while others watch: onlookers and guards come in during the fight, and after a murder only the joining player is pursued, not the host. |
| Drawn weapon | The joining player draws a weapon next to a guard, or walks into a guarded place: the guard warns, then attacks that player. |
| One dialog per NPC | One player talks to an NPC; the other tries to talk to it at the same time: "<NPC> is talking to <player>.". The listener within 20 m sees the speaker's answer options and the chosen line. |
| News from an NPC | After the opening raid Hamlar (Ardea) has news for each hero: he walks up to the joining player too and talks to them. |
| Escort and guide | The joining player takes a follow or escort quest (Mitch in Faring, the slave for Freman in Silden): the NPC goes with that player; the quest counts for both. |
| Wait until morning | One player chooses to wait until morning: the clock jumps to 9 o'clock for both. |
| Cape Dun uprising | The rebels' uprising in Cape Dun (Phil's quests): it starts for both and the fight counts for both. |
| Free a town | A town other than Ardea (Montera, for example) freed in the middle of the game: the town is free and the quest done on both PCs. |
| Drive away, plunder | A quest to drive someone away (its dialog fight) and a plundering quest: both players see the result, the quest counts for both. |

### Optional: the features of 0.5.1

| Stage | Steps and observations |
|---|---|
| Blocking | The joining player blocks a few blows of an enemy with a weapon or a shield: the block costs stamina (more without the shield talent), as in single-player. |
| NPC parry | The joining player hits an NPC of the host's region that parries (a guard in a duel, for example): parried blows do less damage and cost the NPC stamina, as when the host hits it. |
| Onlookers | The joining player knocks a citizen out in a town while others watch: the onlookers come over and react as when the host does it. |
| Grudge | The joining player starts a fight with an NPC and walks away: when the joining player comes back, the NPC is still angry at that player only, not at the host. |
| Chapter | One player hands over the artefacts at a god's shrine: the chapter changes for both (other NPCs talk to both players as in the new chapter). |
| The end | One player starts the epilogue: the other sees the same pictures at the same time, up to the game's end. Note what happens to the connection afterwards. |
| Swimming | The joining player swims forward in the sea: the host sees that player swim, not glide. |

### Optional: the features of 0.6.0

| Stage | Steps and observations |
|---|---|
| Polish | A player with the game's subtitles in Polish (options menu) sees the mod's messages in Polish, for example when going far from the host. |
| The end, busy | One player starts the epilogue while the other is in a dialog: the other sees a message and the epilogue starts when the dialog ends. |
| Story moves | After Ardea is freed, both players go far away: Diego, Gorn and Milten stand at the same new places on both PCs. With Diego following the joining player, he stays with that player. |
| Freed town | The joining player frees a town with shared reputation: the rebels speak to the host about it as to the joining player. |
| Sleeping animal | The joining player walks up to a sleeping animal: it wakes up as it would for the host. |

### Required connection checks: over Steam (experimental)

Both use Steam and are Steam friends; see [PLAYING.md](PLAYING.md#over-steam-experimental).

| Stage | Steps and observations |
|---|---|
| Invite and connect | The host starts first and stays silent: its menu says to invite the partner from the Steam friends list. The joining player's menu says it waits for an invite and nothing connects by itself (check `g3together.log`: no `joining lobby`, no lobby created on the host). The host right-clicks the friend > **Invite to Game** (or the joining player right-clicks the host > **Join Game**) and the joining player accepts: the host opens a lobby, the client joins (note how long); player names match their Steam names; the menu shows lobby status and ping. The `connect:` log lines on both sides show each step. |
| Latency breakdown | With the coordinator's two isolated copies and no artificial relay, confirm the client's menu says transport RTT and normally reads low single digits, not 15/16/31/32 ms clock steps or a fixed 110 ms. Compare `STEAMSTATUS ping` with the separate ENet RTT after at least 60 seconds in-world. Transport should keep responding while one game loads; ENet may retain frame/ACK delay. Check worker poll gaps, socket/send failures, batch/service caps and send-to-flush maxima. Do not interpret smoothed ENet loss alone as continuing physical loss. |
| Burst/fragment queues | Record `queued_commands`, `reliable_queued`, `reliable_inflight` and `reliable_bytes` in the same quiet and NPC-heavy windows. Two-record batches remain 1199 bytes and still fragment; best-effort near-MTU snapshots must no longer introduce reliable ACK dependencies. Large full poses were already unreliable-fragmented, so high traffic can remain. Queue snapshots can miss short bursts; do not claim throughput or gameplay acceptance from the queue-only ENet tests. |
| Buffered presentation | On a clean 60 Hz link, `SYNCLATENCY buffer_avg_ms` should settle near 34 ms; actual `shown_lag` should settle without rewinds. Jitter/transit variation may increase the buffer up to 500 ms. Inspect backlog/catch-up and per-entity oldest sample ages after a stall; received actions and life transitions must survive catch-up. Check NPC running/turning/stopping, melee/parries/hit reactions, targeting after kills, ragdolls, downed/revive, trade and host save/load with client re-entry. Policy tests and builds do not establish native visual/gameplay acceptance. |
| Different networks | Play from two different homes without a VPN or port forwarding: check joining, character-record save requests and an hour of play. Record latency and stutters. |
| Join game | Run once with the joining player's game already running and once with it closed (Steam must start Gothic 3 with the host's connect string): **Join Game** on the host in the Steam friends list, or accept the invite notification: it joins. Also accept while already connected (ignored), accept a stale invite after the host quit (clear message), and accept an invite with a different `session_id` (clear refusal). |
| Leaving | The joining player quits: the host sees it gone at once and is joinable again. It starts again, accepts the invite again and rejoins; nothing reconnects by itself. |
| Without Steam | Close Steam and start the game: co-op startup must fail with a clear message and no alternative connection. Repeat on the host. |

## Recording a bug

Each player records a separate entry for each problem:

- local time, time zone and role;
- world or location and approximate distance between players;
- the checklist step and exact actions immediately before the problem;
- expected and observed behavior, affected PCs and whether it reproduced;
- whether a dialog, fight, knock-out, save/load or join/leave was in progress;
- a screenshot or short video if it shows the problem.

Then each player can collect their logs with the included tool:

1. Open PowerShell in the extracted package folder. Python is required.
2. Run the script with the Gothic 3 installation folder:

   ```powershell
   python "tools\collect_logs.py" "<Gothic 3 folder>"
   ```

   If Windows Documents is in another location, pass it as the second argument:

   ```powershell
   python "tools\collect_logs.py" "<Gothic 3 folder>" "<Documents folder>"
   ```

3. The script prints the new ZIP's name. It is created in the package folder, usually as
   `g3together-logs-<timestamp>.zip`.

For a hung game process, capture an explicit diagnostic from the same package folder:

```powershell
python "tools\hangstack.py" --pid <PID> --output "<output.txt>"
```

`hangstack.py` is a manual diagnostic: it reads the named process and writes the requested output only. It does not
launch or drive the game, and it makes no gameplay assertions.

The tool collects `g3together.log`, `g3together.ini` and, if present, `g3trace.log` from the game folder.
It also collects available engine logs from `%USERPROFILE%\Documents\gothic3\`:

- `paru\g3debug.log` (Parallel Universe logging, if present);
- `ge3log*.log`;
- `Lastlog_GE3.log`.

The Documents folder defaults to `%USERPROFILE%\Documents`; another path can be passed as the second
argument. The collector removes Windows user paths, IP addresses and the INI's `address` line.
Review the ZIP before sharing it. Give logs and screenshots privately to the host first. Do not
share unredacted INI files, saves, campaign sidecars or a complete Documents folder.

## Known limitations

- Real internet play remains untested. A first short human test with two windows on one PC found
  two bugs that need further human verification.
- Only two players were tested.
- Companions are restored after loading by player name. Changing your name loses that association.
- If the host loads another save during a session, the client returns to its menu when safe and chooses
  New Game to re-enter with its own saved character; it does not load the host's native save automatically.
- Untested: monsters attacking downed heroes, the actual bed dialog, and the
  mouse controls of the trade window (dragging items, its buttons).
