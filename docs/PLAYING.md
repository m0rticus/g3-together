# Playing G3 Together

G3 Together adds co-op to Gothic 3 for two players. More players are planned but have not been tested.
One player is the **host**: their world is the shared campaign, and their save is everyone's campaign save.
The other player joins them.

This fork supports **Steam only**. Direct IP, LAN/VPN joining and automatic IP fallback are not supported.

This is a test version. See the known issues and limitations below.

## Requirements (the same on both PCs)

- Gothic 3 from Steam with **Community Patch 1.75.14**.
- **Parallel Universe Patch 1.1.1**. Steam, CP 1.75.14 and PUP 1.1.1 are the tested baseline;
  compatibility without PUP has not been established.
- The same G3 Together version (the same `Script_G3Together.dll`) and the same game files.
- Matching difficulty, alternative balancing, alternative AI, XP factor, minimum hit duration, Quick Loot,
  and the entire `[Game]` section of `Ini\ge3.ini`. The mod checks these when a player joins and refuses
  mismatched installations. The joining player's `<Gothic 3>\g3together.log` records the reason on a line
  containing `REFUSED by host`. Loose files in Data subfolders are not checked.
- No other co-op mods in `scripts`, such as Myrtana Accord's `Script_G3Coop.dll`.
- Prefer a separate **copy** of the Gothic 3 installation for co-op.
- Steam running and logged in on both PCs, with both players on each other's friends list.

## Installation

**Setup:** once Gothic 3 and both patches are installed, unpack the whole package anywhere and double-click
`Setup.cmd`. It does the copying and the INI for you: it finds Gothic 3 (Steam, GOG or the
folder you name), checks Community Patch 1.75.14 and Parallel Universe Patch 1.1.1, moves another co-op mod out of
`scripts`, copies the mod into `scripts` and writes `g3together.ini` from your role. Your display name comes from Steam automatically.
Steam and the verified `steam_api.dll` are required; setup stops if that DLL cannot be installed.
Replaced files are kept in `G3Together-backup` in the game folder. The steps below do the same by hand.

1. Install Gothic 3 from Steam, then **Community Patch 1.75.14**.
2. Install [Parallel Universe Patch **1.1.1**](https://www.worldofgothic.de/dl/download_678.htm)
   into the same game installation. Both players must use the same patch versions.
3. Use the same G3 Together package on both PCs. Copy `Script_G3Together.dll` into `<Gothic 3>\scripts\`
   and the package's verified 32-bit `steam_api.dll` into `<Gothic 3>\G3Together\`, or let setup obtain it.
4. Copy the appropriate `g3together.ini.host-example` or `g3together.ini.client-example` from the package
   into `<Gothic 3>\`, rename it to `g3together.ini`, and edit it:

   Host:

   ```ini
   role = host
   transport = steam
   ```

   Joining player:

   ```ini
   role = client
   transport = steam
   ```

   The joining player must use `role = client`. If the INI is missing or the role has another value,
   the game starts as a host.

### Mod message language

`language = auto` (written by the setup and the example INIs) shows the mod's messages in the language of your
Gothic 3 texts: German for a German game, Polish for a Polish one, English otherwise. To fix a language, set
`language = en`, `language = de` or `language = pl` in your own `g3together.ini` and restart the game. Without a
`language` line the messages are English. Each player may choose independently. This setting does not change
Gothic 3's own language. The message examples in this guide assume English.

## Connecting over the internet

Steam is the only connection path. No host address, VPN or router port forwarding is needed.

### Over Steam (experimental)

With `transport = steam` in both INIs, the joining player's game finds the host among their Steam friends, and all traffic
goes through Steam, relayed by Valve where needed.

- Both players need to own Gothic 3 on Steam, with Steam running and logged in, and must be Steam friends.
  The automatic player name is the current Steam display name; manual INI names are ignored.
- The mod needs Valve's 32-bit `steam_api.dll` in `<Gothic 3>\G3Together\`. The setup puts it there (from the package, or
  downloaded from the Steamworks.NET release 2025.164.1 after asking, checked by its SHA-256).
- The mod initializes as Gothic 3 (AppID 39500), so Steam activity uses Gothic 3 instead of Spacewar.
  Local real-Steam initialization and lobby creation were checked; two-account gameplay remains a separate check.
- Set the same `session_id` in both players' `g3together.ini`, or choose it in setup. It is a case-sensitive label
  of 1-63 ASCII letters, digits, underscores or hyphens, shown in the menu and log. Missing settings use `default`;
  empty or invalid values stop networking. Different IDs are rejected before joining a Steam lobby, and every
  relay message also checks the ID and native Steam lobby number before forwarding any gameplay/save data.
- For three independent pairs, set `session_id = pair-red`, `pair-green` or `pair-blue` on each pair's host and client.
  These IDs do not change the existing per-lobby player limit or impose a two-player cap. Use one host per ID;
  if multiple hosts deliberately share an ID, automatic search still selects the first matching friend.
- `steam_host = <the host's Steam name>` additionally restricts friend search. The host's `g3together.log` names
  its Steam lobby number; `steam_lobby = <number>` restricts joining to that lobby. These selectors, command-line
  `+connect_lobby`, and Steam invites **never override session_id**.
- Session IDs are visible grouping labels, not passwords or access controls. Steam friendship/lobby membership
  still applies. The session-scoped relay format is incompatible with older builds, even in the default group;
  update every player. The gameplay protocol itself is unchanged.
- Each concurrently running real-Steam instance needs a distinct Steam identity. Running six processes under
  one Steam account is not supported by this identity-addressed transport. Multiple copies on one PC also need
  isolated installations, profiles and saves; a Session ID isolates network traffic, not filesystem writes.
- Without Steam or the required `steam_api.dll`, co-op startup fails with a message in the menu and game.
  There is no fallback. ENet's UDP sockets bind only to localhost and accept linked Steam relays, not direct-IP players.
- The Steam overlay's invite may not open in Gothic 3. It is not needed: the joining player's game finds the host by
  itself, and "Join game" in the Steam friends list works as well while their game is running.

The main menu reports creating/hosting/searching/joined states, accepted and ready clients, linked Steam peers and
measured **transport RTT**. It is the most recent high-resolution Steam control echo, rounded to integer milliseconds,
not NPC buffering or input-to-photon latency; the host shows the maximum fresh linked-peer sample.
`pending` means no valid sample exists yet or none arrived for five seconds. Control echoes are sent once per second,
but a still-outstanding echo is not overwritten; genuinely slow replies up to five seconds remain measurable.
The transport worker services callbacks and forwards datagrams independently of rendering, with a 1 ms high-resolution
waitable timer and an immediate wake after ENet flushes. This avoids Windows 11's ordinary timer-resolution relaxation
for an occluded game. Older Windows versions without high-resolution timers explicitly log the legacy timed-wait path.
This is a scheduling target, not a hard OS deadline. ENet and gameplay handlers
stay on the engine thread, so loading/render stalls can still raise **ENet RTT** without raising transport RTT.
Errors and compatibility refusals take priority. `g3together.log` appends startup/session, lobby, peer, send-failure and
periodic connection diagnostics automatically. Review names and Steam IDs before sharing logs.

`STEAMSTATUS ping=` is the displayed transport RTT; `ping_age_ms` is the sample's age. `poll_timer` identifies the
high-resolution or legacy wait path. `poll_gap_avg_ms`,
`poll_gap_max_ms` and `pump_work_max_ms` describe the worker's scheduling and work over the last reporting interval,
not measured wire or per-packet queue time; relay intervals/ages also use QPC, reduced to integer milliseconds.
`receive_caps`/`forward_caps` count exhausted bounded relay batches;
`socket_failures`, `socket_error`, `receive_failures`, `send_failures` and `invalid_pongs` expose failures instead of
silently attributing them to lag. A clean local link should normally have low-single-digit transport RTT, but an
overloaded OS or a real Steam route can legitimately take longer.

The separate `net: rtt=` line measures smoothed ENet ACK RTT, including both engine service loops.
`transport_max_rtt_ms` repeats the menu's maximum fresh transport echo, not a per-peer native Steam statistic.
`service_gap_avg_ms`/`service_gap_max_ms` expose those frame intervals; `service_caps` counts exhausted 512-event budgets,
and `service_errors` counts ENet service failures (the first is also logged immediately).
`enqueue_flush_avg_ms`/`enqueue_flush_max_ms` bound time from the first queued send to an explicit flush; ENet may have
sent some commands sooner during service. All mod sends now flush by the end of the current mod frame, including frames
that skip player-state transmission. ENet's `loss` is a smoothed reliable-timeout statistic (loading can inflate it);
`lost` is the current accounting interval, not a lifetime total. `waiting_bytes` is receive/reassembly storage, not an
outgoing queue. `queued_commands` counts pending best-effort/control commands; `reliable_queued` counts unsent reliable
commands, `reliable_inflight` counts reliable commands awaiting ACKs, and `reliable_bytes` is ENet's in-flight reliable
payload bytes. These are instantaneous queue snapshots, not interval maxima. None of these counters proves physical
packet loss or pure one-way wire latency.

Best-effort sends explicitly allow unreliable fragmentation at ENet's actual negotiated MTU/header budget. At MTU 1200,
the fragment payload budget is 1172 bytes (1168 with an ENet checksum), not 1200. A best-effort packet above that budget
uses unreliable fragments rather than silently becoming reliable; small packets remain ordinary sequenced unreliable.
Compact NPC control batches are capped at 1172 bytes. Explicit reliable events and ENet's reliable sequence rollover
retain their semantics.

The current source uses **protocol 73**. Its host-only NPC overhaul and paired saves are experimental; native gameplay acceptance is still
in progress, so this source must not be treated as a verified release. Both players need the same build; protocol 72 peers are refused. Personal loot,
NPC merchant inventories and automatic player exchanges are retained.

The host simulates all NPCs, including remote companions, guides and summons. Separate non-rendered prefetch cameras
load distant simulation regions; the real camera, static-world culling and building LOD remain unchanged.
`host_roi` controls NPC coverage, not rendering distance. The old `host_sim = off/elastic` choices no longer transfer
simulation to a client.

NPC captures use a fixed **60 Hz deadline**, including unchanged NPCs, independently of the player's `send_rate`.
per-client interest and change detection decide which captured states are actually sent. Moving or fighting NPCs near
a player retain the high-rate stream, while changed idle poses are limited to 10 Hz and unchanged states receive a full
refresh every second. The interest radius is 80 m on entry and 100 m on exit; combat with or a party relationship to a
player also keeps an NPC relevant. The host cannot see another player's camera direction or focus in player state, so
nearby and potentially visible moving NPCs are treated conservatively. A slow host cannot produce 60 distinct samples
per second: `SYNCNPC` logs capture ticks, missed deadlines and elapsed capture time. Clients interpolate final bone poses,
facial-rig state and eight motion slots on one buffered timeline, normally at least 50 ms behind the host; the existing
latency adaptation then smooths the buffer without predicting future NPC movement. HP, stance and
life transitions follow that same presentation timeline. Missing or stale presentation cannot authorize an incoming hit.
Player-puppet smoothing remains separate.
`SYNCLATENCY` reports desired buffer and actual `shown_lag` separately: slew/holds can make them differ.
`jitter_avg_ms`/`jitter_max_ms` expose the arrival/capture-interval variation driving adaptation.
`shown_lag` and `transit_excess` use the track's minimum arrival-minus-send clock-offset estimate, **not synchronized
one-way timing**; `arrival_age` is time since the newest local receive. `underruns` counts buffer recovery events in the
currently tracked NPCs; age/lag maxima can include a deliberately held terminal pose. Do not add these estimates to an
RTT as if all components were disjoint.
The host also sends each NPC's attitude to the player, so name-tag colours (green, white, orange, red) match on both PCs. While
you talk to an NPC on the client, that one NPC's script routine runs locally so its voiced lines play; the host still holds
and animates it. Both are built for manual testing and not yet verified live.

Facial or skeleton changes retain the buffered old pose until the host's transition time. The nearby-NPC warning
requires the same living NPC to lack usable presentation for two seconds; settled corpses are not pending live poses.
Discarded pose fragments release their receive budget, so packet loss cannot gradually exhaust that budget and stop
NPC updates. The 16-bit unreliable sequence of a pose channel is reset on a second, idle pose channel, so a lost reset
packet can neither hold back nor starve the live pose stream. ENet's RTT-jitter throttle is off: it only drops more
fixed-rate pose frames. Native clip bookkeeping or a pending clip name cannot invalidate a successfully rendered host skeleton.

The host sends final skeletal matrices because native blend-out layers survive outside the eight motion slots.
Poses retain matrix shear and use sub-centimetre translation quantization. Ordinary poses travel on a separate lossy
ENet channel; essential life/party/hit changes remain reliable on the NPC channel. Owned loopback sockets
have burst capacity for crowded scenes. `NPCPOSE` and `PASSIVE` diagnostics distinguish queued/received poses,
presentation waits and completed skeletal updates. This full-pose stream costs more bandwidth than clip-only replay;
internet throughput and multi-player resource limits still need acceptance.

Protocol 73 caches bone layouts and full NPC motion baselines per world. After each client acknowledges them, updates
send the existing bone values without repeated names and compact anchored control records; full records remain the
fallback for a missing dictionary, an unacknowledged baseline or values outside the compact range. The log reports layout
sizes, resync requests, anchors and fallback failures.
Every five seconds, host and clients log `net: bandwidth` summaries. Kind entries are
`message-id=tx-packets/bytes,rx-packets/bytes`: successful ENet queue operations are counted once per recipient,
and received logical messages include those rejected by validation or the world gate. Empty internal packets have
no kind. The `npc_per_s` fields count scheduler decisions per second, not deliveries. `enet_loopback_*` is the ENet
host's local UDP packet and byte counter delta, including protocol
overhead, retransmissions and acknowledgements, but not IP headers or Steam traffic. ENet's transmit packet counter
also includes a zero-byte socket-would-block attempt. None of these figures measures Steam's actual network usage.
Snapshots of player trade goods are sent on changes and roughly every 30 seconds while active; world entry and joins
also trigger synchronization. Explicit transaction publications remain reliable and immediate.

Your own successful hit shows its local flinch immediately and is claimed reliably from the host. A host-side parry
does not cancel that landed hit. Hit identities, acknowledgements and NPC incarnations prevent duplicate claims and
suppress the attacking client's echoed flinch; other players still see the host's reaction.
Damage to your hero remains locally judged against the attack you are shown, not a host-issued HP subtraction.
NPC projectiles follow their host presentation time. Every terminal NPC death starts a local client ragdoll, including
client kills without a host ragdoll flag. A matching host death force is applied once, after its presentation time and
physics are ready, with its original direction and corpse-relative application point. Forces are bounded by vanilla's
largest explosion push; nonfinite force data is refused and logged. Without a host force, the body falls under gravity,
as vanilla does without an attacker/inflictor; a predicted flinch never invents a second death push.
Ragdoll bones use local physics, not a transmitted bone stream. Settled bodies retain their resting pose when physics
unloads and are not restarted by repeated death snapshots, late forces or reloading the same corpse.
Revival requires a new host incarnation, not an older buffered alive frame.
`CLIENTRAGDOLL action=force` records the selected source, applied count and actual force magnitude/vector.
Three `action=sample` lines record root/bone positions and velocities at startup, 250 ms and 1 s (or the next frame).
These diagnostics support manual checking; the physics changes still require native gameplay acceptance.
Friendly-fire protection applies before local hit feedback as well as before authoritative damage.

## Starting and saving

1. The host starts Gothic 3 and begins a new game or loads the co-op campaign.
2. The other player starts Gothic 3.
   - If the host has loaded or saved a campaign and you have your own character record for it, stay in the main menu.
     The campaign is transferred and loaded automatically when you join.
   - A first-time player without an own character record must start a new game or load their own local hero first.
     The mod waits rather than loading a copy of the host's character.
   - If the host has started a new game, start a new game as well.
3. **Either player saves** using the normal manual/quick-save controls. A client requests the save from the host;
   it never replaces the host's world with its own local world. Every ready player captures their own character,
   then receives the same host-world checkpoint with a common save ID.
4. Wait for **"Co-op checkpoint saved for every participating player"** before quitting. The immediate native
   save call reports no synchronous save while coordination is queued; the mod's later confirmation is authoritative.
   Loading, dialogue, transformation, being downed, unsettled trade, a disconnected partner or disk failure can refuse/cancel
   the operation. A failure after host publication may leave a usable host checkpoint, but is never reported as a
   successful save on both machines. Retry once everyone is ready. Player exchanges wait during a checkpoint.

Each checkpoint is an immutable `G3T_<save-id>` native save pair plus a `.g3t` manifest/journal.
Every participant also publishes the requested menu/quick-save slot as an alias. Each machine stores its own
`g3t-character-<world-id>-<Steam-account-id>-<save-id>.txt` record (format V4), including
attributes, current/max health and mana, XP/level/LP, inventory/gold/equipment, skills, spells, quick slots and pose.
V4 keeps base attributes and aggregate equipment/stat modifiers separately; temporary-effect ownership and
expiry timers are not reconstructed and remain a native runtime limitation.
The native world pair still contains the host's single hero; the **mandatory own-character overlay is part of the save**.
Do not load a transferred native pair without the mod or remove the character files. Back up the whole save folder.

Loading the host's earlier save loads that checkpoint on the client and restores its exact saved character before
gameplay opens; it does not overwrite that older character record with the currently running hero.
This also applies when the host reloads the latest checkpoint after continuing to play.
A missing exact record can explicitly recover the same account's latest record **from this campaign only**;
a damaged/wrong-account exact record instead stays blocked until repaired. A new menu-only player stays outside
the world until they start/load their own hero. Record deletion never gives them the host's hero.
Legacy G3T1 journals remain readable: the native contents identify a conservative legacy campaign,
and only the exact old slot's complete local hero record can be imported and claimed by one Steam account.
V3 carries its required spell/quick data inline; V2 can migrate with complete skills/end data and its required sidecar.
Original V1 and incomplete older V2 records cannot prove a complete character and stay gated rather than supplying host data.
Original legacy files stay unchanged. Legacy records without pose retain the native spawn position on migration.
Old stat records preserve effective maxima but cannot recover the original base/bonus split.
No migration can reconstruct a distinct character that an older save never recorded; supply a genuine local hero in that case.
Transient record-publication failures remain gated and retry without requiring an edit to the source record.

Coordination uses prepare/ready, host native save, verified world transfer and stored acknowledgments, not a
distributed engine freeze. Captures are separated by network latency, not literally simultaneous. Only the host's
native world bytes are distributed. The native manual/quick-save route and lower save API are hooked; no built-in
autosave/exit-save route was found in the inspected game. The mod does not add a new autosave-on-exit feature.
Custom save callers get a queued/failed synchronous result and must wait for the mod's confirmation too.
Immutable checkpoints are retained rather than pruning old quick-save history automatically; allow additional disk space.

## What is shared

Protocol 62 makes ground pickups, corpse loot, chest contents and NPC merchant inventories personal.
Taking or buying an item changes only that player's copy; each player pays from their own gold.
Chest locks and use reservations are local too; other shared doors and usable places are unchanged.
Direct trades between players still transfer real items rather than copying them.
This does not restore items already removed during an earlier session. A host-save transfer/load replaces the client's world
with the host's saved world, including native loot, merchant stock and generated-inventory flags.
Independent world-inventory persistence across saves/late joins is not implemented; this is not yet a persistent per-player world save.

- Players see each other, including movement, combat animations and falls.
- The host simulates NPCs around every player; clients replay that state without NPC AI, navigation or autonomous tasks.
- The host applies NPC damage claims; each player judges their own visible contacts. The player who kills an NPC earns the experience.
  Kills by a player's companions or summons reward that player; kills by other NPCs reward nobody. As in single-player,
  your last hit on a human or orc knocks them out (experience, no murder); only a blow on them lying there kills them.
- Companions belong to the player who recruited them, even far from the host, but their simulation always remains on
  the host. Dismissal changes the party relationship, not simulation authority.
- An NPC that guides you or that you escort (follow and escort quests) goes with the player who took the quest, also the
  joining player, and after the host loads a save that player leads again. When the escort ends far from the host, the
  NPC stands at the same place on both PCs.
- Quest state and previously completed dialogs are shared. Quest rewards go to everyone. Kill
  quests also count enemies killed in the joining player's region.
- Future experience rewards are shared: whatever either hero earns is added for the other too.
  Saved XP and level remain individual; joining/loading no longer raises one hero to the other's total.
- The main story is shared to the end: when one player hands over the artefacts at a god's shrine, the chapter changes
  for both, and when one player starts the epilogue, the other sees the same pictures and the game's end at the same time.
  If the other player is in a dialog, a menu or a fight just then, their epilogue starts as soon as that is over (a message
  says so).
- A stool, bench, campfire or other place has one user: while your partner uses it, you are kept off ("<Name> is
  using this."), and NPCs do not take it either.
- Loot and NPC merchant stock are personal; what one player takes or buys stays available in the other player's copy.
  An NPC whose pocket one player picked still cannot be picked again by the other.
- Everyone uses the same time of day. Sleeping advances time for all players, and so does waiting until morning: the one
  who waits jumps straight to 9 o'clock.
- Animals that come back after some days (scavengers, deer, wolves and the like) come back on both PCs or on neither.
- Helping up a downed player costs one healing potion. A player who would die stays down while a
  connected partner in the same world, who is not downed, can help. Talk to the downed player's
  character and choose "I will help you." to help them up. The cheapest healing potion in the
  helper's inventory is consumed.
  Without a potion, both players see a notice and the downed player waits for their partner to
  return with one.
- If everybody is known to be down or dead, the hero dies immediately as in single-player.
  Missing, disconnected, loading, stale, or another-world partner state keeps the original
  five-second grace instead. The host loads a
  save; the other player is loaded into it automatically.
- The game itself lets a human knock out another human or orc instead of killing them, so in a real fight you would never die.
  While your partner is connected, a blow that takes you to 0 hit points from someone who means to kill you (raiders,
  monsters, an intruder; not a duel, the arena, a guard's arrest or another player) counts as a death: you stay down until
  your partner helps you up, and you die if nobody can.
- A knock-out of the game's own works as in single-player: when a guard or another person beats you in a fight (without
  killing you), you stand up by yourself after a while, nobody needs to help, and the winner may rob you of half your gold.
  When an NPC of the other player's game does it, you see "<Name> robbed you of N gold." Your partner cannot loot you.
- An NPC that sees a player steal reacts as in single-player, also when the other player's PC simulates it: it goes after
  the thief. Fights and murders by either player are seen the same way: onlookers and guards come in during a beating,
  and only the murderer is pursued. Guards warn and then attack a player who draws a weapon near them or walks into a
  place they guard. A murder by a player's companion is noticed as in single-player.
- Onlookers come over to either player's fight and see a knock-out, whoever did it. The opponent and the onlookers
  remember how a fight ended, and an NPC that is still angry at one player holds it against that player only.
- Blocking works as in single-player for both players: a block costs stamina and the shield talent counts. An NPC that
  parries blocks either player's blows the same way.
- Friendly fire is off by default (`friendly_fire = 0`): players and their followers cannot damage, stagger, knock down,
  poison, freeze or otherwise harm one another. Healing and revival still work. Enemy attacks and environmental damage
  are unchanged. Both players must use the same value and restart after changing it. An explicit value from 1 to 100
  retains the previous cross-player damage scaling; setup preserves existing values.
- Melee swings use Gothic 3's NPC bystander rules on both PCs: a client swinging at a hostile NPC does not hit a
  more friendly bystander the native game would protect. This is separate from `friendly_fire`: deliberate attacks,
  self-defense and the game's duel/crime rules still apply. Both PCs need the matching protocol 73 build.
  To check it, place a rebel or companion beside an orc, aim at the orc and swing from the client: the orc should take
  one hit per swing, with no protected bystander's damage, flinch or anger. Then deliberately attack a neutral NPC
  outside another fight and check its normal reaction; also check self-defense and a duel/arena bout.
  The log's `melee-victim` lines identify the victim, attitude, contact-time focus/target and decision:
  `local-refused` means no local judgement/claim; `claim-sent` and `host-claim` share the reliable hit sequence.
- A player within about 20 m hears the other player's dialog. With the game's subtitles on, the lines' text shows as
  well, in your own game language, unless you are in a dialog yourself. You also see the answers your partner can pick
  and then the one they picked; the one who talks chooses.
- An NPC talks to one player at a time: while your partner talks to it, it is busy for you ("<NPC> is talking to
  <player>."). It gestures and faces the speaker on both PCs.
- An NPC with important news for a player (a guard, a quest giver) walks up to that player and talks to them, as it
  does in single-player, also to the joining player.
- Dead NPCs remain dead, including those killed far from the host.
- Opening a door or picking a lock opens it for everyone, including players who join later.
- NPCs respond to attacks from either player as in single-player: they fight, flee or call for help.
- Players see each other's arrows, bolts and fireballs in flight; each hit counts once. Healing
  spells also work on the other player.
- NPC spells such as sleep, fear, charm, poison and disease apply on both PCs. Summoned creatures
  are visible to the other player and join the fight.

## Reporting a bug

- Record the time, who was hosting, what you did and what each player saw.
- Both players should run `tools\collect_logs.py` from the extracted package folder and send the
  resulting ZIP. Commands are included in the [playtest checklist](PLAYTEST.md).
- For reproducible NPC or enemy problems, such as a dead orc standing upright on the other PC,
  add `trace = on` to the INI and restart. The game writes `g3trace.log` in its installation folder:
  NPC animations, tasks and the game code that triggered them. The file is limited to about
  50,000 lines and is recreated on each start. The log collector includes it. Remove the setting
  after collecting the report.

## Known issues (21 October 2026)

| What can happen | What to do |
|---|---|
| The game may crash during combat or shortly after an enemy dies. The known orc-raid crash has a guard, but other crashes remain possible. | The host should save before difficult fights. Record the time and enemy, and preserve both players' logs before restarting. The host loads the last good save; the other player is loaded into it automatically (if their own game crashed, they restart it and wait in the main menu). |
| The other player's character briefly lags behind while being hit, or shows a movement inaccurately. | Regroup and wait briefly; use **Page Down** if needed. If enemies, health or positions remain clearly different, stop the fight and record the problem. |
| Enemies or companions behave differently on the two PCs after joining. | Check that both players see the same characters and movements near the host. If the difference persists, preserve the logs and restart together from the last good save. |
| "<Name> is using this." when you want to sit down or use a campfire, anvil or other place | Your partner uses it right now; wait until they get up. If someone gets stuck at such a place, record the bug and reload together. |
| Saving while transformed into a creature has not been checked. | Transform back before saving. |
| A controlled or tamed NPC does not follow reliably or leaves the party. | Do not rely on these abilities yet. Record the NPC and time; casting the spell again is not a verified workaround. |
| Stun and hypnosis cast by the joining player only take effect through the host. | Record failures; the host can cast the spell instead. |
| A summoned creature stops moving on the other PC when players move far apart. | Use summons while together. Record the location and creature if this occurs. |
| Poison or disease is not fully visible on the other PC, and a poison death may award no experience. | Record the discrepancy. |
| A guard who talks to you about crimes counts the town's whole record (with `shared` or `together`: your partner's deeds too). | Avoid theft and deliberate attacks on peaceful residents if that bothers you. Record who acted and whom the guard addressed or attacked. |

## Known limitations

- Each player's surroundings run on that player's PC and stay the same for both; distance shows no
  message. Host and joining player only see "<name> is in another world." when the partner is in another world.
- **Jump to your partner:** **Page Down** puts you 1.5 m behind the other player in the same world,
  including a downed partner you want to help. There is a five-second cooldown. Staying together
  is preferable, but you can separate and regroup. To change the key, use `partner_key = F9` in
  the INI (F1–F24, PAGEUP, HOME, END, INSERT, DELETE or a letter).
- **Map and compass:** your partner is marked on your map, with their name, and on the compass in their direction from
  where you look.
- **Your look:** the other player sees the armor and helmet you wear. To be seen as another character, set
  `look = Diego` (a character template of the game) in your own INI; without it, the other player's
  `puppet_template` decides (default: the hero).
- **Trading with your partner:** talk to the other player's character and choose "Show me your wares!". The game's trade
  window opens with their unequipped goods and gold. Select what to take or give and use the normal **Trade/Swap**
  button. Items are free: no payment or matching barter value is required. Select gold explicitly to give it, including
  a gold-only gift. Each exchange synchronizes automatically while the window stays open; your partner does not approve
  it, and closing the window is not a confirmation step. Equipped/held gear, quest items, skills and bare hands remain
  protected; sharpened, blessed and forged qualities are preserved. On the first trade, close the game's tutorial with Ok.
  The old offer and price-adjustment keys are no longer used. NPC merchant prices are unchanged.
  Saving/loading waits for an exchange to finish synchronizing; if the connection drops, reconnect the partner first.
  The new native free-exchange flow is built for manual testing; mouse controls and gold-only gifts need gameplay verification.
- Dialog changes to NPCs, such as trader, teacher, guard and routine settings, apply to everyone,
  as do dialog-triggered duels, attacks and companion recruitment. Dialog items go to the player
  speaking.
- The host's save records which player owns each companion. After loading, a companion returns to
  its player when that player rejoins, identified by their Steam display name. Do not change that name.
  After a disconnect without loading, companions initially belong to the host.
- If the host loads a save during a session, the other players are loaded into it automatically. Until they
  are done, you do not see each other.
- Older trading and looting checks used inventory helpers, not the native trade button. They do not verify the new
  player-exchange flow; use the manual trading checks in [PLAYTEST.md](PLAYTEST.md).
- Sleep advances the clock for everyone, but only heals the player who sleeps.
- Transformations: the other player sees the creature moving, then sees the hero again after the
  transformation ends. Wolf and Bloodfly were tested using the same setup as the game's spell.
  In creature form you fight as yourself: your hits count as yours (knock-outs, kills, experience, murders), and an
  enemy's hits hurt the creature, not your hidden human body. Saving while transformed remains
  untested.
- A partner's hailstorm: its weather now stays at your partner on your PC (it used to darken the sky and strike around your
  own hero). Damage comes only from the caster's own hailstorm, as in
  single-player.
- Enemies attack both players. Their attitude toward a joining player comes from the hero on the
  PC currently simulating the NPC. For example, the host's poor standing with orcs also applies
  to the other player for NPCs simulated by the host.
- A town or camp freed (or taken) by either player is freed for both: the town's state comes from the host.
- Town and camp reputation, chosen with `reputation` in the host's INI:
  - `shared` (default): one record per town for both players; thefts by either player count, and a guard who talks to
    you about crimes counts everybody's deeds. An NPC that saw a crime holds it against the one who did it.
  - `together`: as `shared`, and an NPC that saw one of you commit a crime holds it against both of you (both are
    refused).
  - `personal`: each player keeps their own record; what your partner does never changes yours.
  A murder counts for the victim's town as in single-player, also when the other player's PC simulates the victim
  (`personal`: in the murderer's own record). Beating a human or orc knocks them out first; only a blow on them lying
  there kills them.
- Soul travel: the other player sees your ghost moving, not the body you left behind.
- Ice and fire spells that put out burning or freezing (and make such projectiles fall apart) work on the other
  player's NPCs only on your own PC. You will rarely notice this.
- An NPC with news stops walking up to you when you go more than about 25 m away from it; talk to it yourself.
- Not yet checked in a full human session: the uprising in Cape Dun, freeing a town in the middle of
  the game, quests to drive someone away (their dialog fight) and plundering. They are on the playtest checklist.
- The Adanos and Beliar endings,
  Xardas' plan and the fifth temple have not been played through together yet, and what happens to the connection after
  the game's end is not known yet.
- Swimming forward has not been tried by a person yet.
- Untested: monsters continuing to attack a downed hero, real internet
  play, and more than two players. A first short human test with two windows on one PC found two bugs:
  enemies ignored the joining player, and combat was not visible on that player's PC. Those fixes need
  further human verification.
