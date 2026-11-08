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
- Difficulty and Quick Loot can differ. Shared balancing follows the host in memory for the session,
  not by editing the client's files (see below). DLL/script/archive differences still refuse,
  naming the differing item and what to install. Unrecognized `[Game]` keys remain a conservative
  identity check; known gameplay/input/UI keys are classified rather than hashing the whole section.
  The joining player's `<Gothic 3>\g3together.log` records refusals; loose files in Data subfolders
  are not checked. Root Data files/archives are compared by content, not just filename/size.
- No other co-op mods in `scripts`, such as Myrtana Accord's `Script_G3Coop.dll`.
- Prefer a separate **copy** of the Gothic 3 installation for co-op.
- Steam running and logged in on both PCs, with both players on each other's friends list.

## Installation

### Difficulty and session balancing

| Option | Class | Where its effective value applies |
|---|---|---|
| Difficulty | Per player | Each real hero's native difficulty; NPC simulation/judgement uses the NPC owner's difficulty |
| Alternative balancing / `ExtendedContent` | Host wins | Native gameplay/setup readers and AB-dependent templates/skills |
| Alternative AI / `AIMode` | Host wins | Native AI/script readers |
| `MinHitDuration`, `XPModifier` | Host wins | Native hit timing and kill XP readers |
| `PlayerStart` | Host wins | Native starting-hero read, before world entry |
| `friendly_fire`, `npc_damage`, `npc_hp` | Host wins | Mod combat scaling on both machines |
| Quick Loot / `QuickLoot`, `TestMode`, `NoMenu` | Local | Input/debug/menu preferences; not delegated |
| DLLs, scripts, Data archives, protocol, unknown `[Game]` extensions | Hard refusal | Installation/capability identity |

The client can play **Easy while the host plays Hard**. Hits on the real client hero are
judged defender-local against its own armour, native difficulty and HP; the host cannot
replace those HP decisions. A locally simulated host NPC uses the host's difficulty.
Client claims against host-presented NPCs run the native judgement with that NPC owner's
difficulty, then the host applies the claim and shared `npc_hp`/friendly-fire scaling.
This preserves the client's visible contact/block decision rather than rerunning every hit
against a different host animation. NPCs actually simulated by the client use the client's
difficulty, not a global host difficulty. Consequently a change of NPC simulation owner can
change difficulty-sensitive future AI/damage decisions; existing HP is not rerolled.
Mirrored NPC presentation never owns HP.

Do not interpret per-player difficulty as one global NPC HP/damage multiplier. Static analysis
of the supported CP scripts found difficulty-sensitive combat/AI branches (including an
alternative-balancing Hard damage branch); not a blanket difficulty multiplier on all damage
or NPC maximum HP. The inspected `GiveXP` path uses alternative balancing and `XPModifier`
for victim-derived XP, not `GetCurrentDifficulty`. Quest/raw shared XP and level-up learning
points retain their existing native/mod paths; no new XP/LP rescaling is introduced.
`npc_damage` still multiplies NPC damage to party members, and `npc_hp` still divides damage
taken by non-party NPCs; neither factor modifies native maximum HP or resets existing stats.

Different shared balancing is adopted before admission with one localized "Using the host's
balancing..." notice listing changed values and confirming your difficulty is kept.
`options:` log lines expose host/original/effective/source, including unchanged values.
The same summary appears beside the existing main-menu hint. When values already match,
there is no balancing notice. Host values are getter overlays, not edits to `ge3.ini`,
`UserOptions.ini` or `g3together.ini`; disconnect removes the overlays.
Missing client `[Game]` values use native defaults, then the host's shared values.
Invalid host values or an unsupported native reader block admission explicitly.

Balancing is frozen within each host world epoch. Change startup balancing on the host,
restart it as required by Gothic 3, then start/load the campaign: the existing re-entry path
adopts the new set. A pre-existing client world with incompatible startup settings must
re-enter through its template; if the template is missing, restart the client and create it
with its first New Game. Never overwrite the user's native world/save to accomplish adoption.

**Verification boundary:** native getter/read/serialization paths were verified statically
against the installed CP DLL bytes; this is not an observed running-engine test. The join
`options:` diagnostics are intended for the next two-player run to confirm actual behaviour.
Protocol remains **77**, with a delegation capability marker in the existing Hello options
slot and a versioned balancing snapshot appended only to Welcome and host world announcements.
Old builds are distinguishable and refuse rather than silently ignoring delegation.

### Installing the package

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

Gameplay notices are passive HUD text: they do not activate a GUI window, interrupt movement or
dispatch a hero animation/dialog. Knockout, revival, a chosen dialog and actual save/load still have
their normal gameplay effects; displaying their notice adds none.

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
  relay message also checks the exact ID and native Steam lobby number before forwarding any gameplay/save data.
  The compact v3 envelope is little-endian lobby number, a `0x80|length` marker, exact session bytes and unchanged
  ENet payload. With `default` it is 16 bytes instead of the old 72-byte header, while a 63-byte ID retains the 72-byte
  maximum; a different case or an ID prefix is never an exact match. Truncated, legacy, marker-invalid and lobby/session
  mismatch packets are dropped before a Steam peer is reserved.
- For three independent pairs, set `session_id = pair-red`, `pair-green` or `pair-blue` on each pair's host and client.
  These IDs do not change the existing per-lobby player limit or impose a two-player cap. Use one host per ID;
  if multiple hosts deliberately share an ID, automatic search still selects the first matching friend.
- `steam_host = <the host's SteamID64 or Steam name>` additionally restricts friend search. Prefer SteamID64
  so renaming the host does not affect search. The host's `g3together.log` names
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

Saved character and personal inventory records, companion/follower/guide ownership and newly recorded summons
are keyed by authenticated SteamID64. Display names refresh independently; reconnecting into a different player
slot or renaming a Steam profile does not transfer ownership. Old name-keyed companions migrate only on an
unambiguous match; unresolved records are retained with a host warning. See [Joining](JOINING.md) for the explicit
`legacy_companion_owner` setting. Older saves did not record remote summon SteamIDs, so missing historical
ownership cannot be inferred safely. World quests/events remain shared world state, not player-name records.
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
silently attributing them to lag. `tx_datagrams`, `tx_bytes`, `tx_envelope_bytes`, `rx_datagrams`, `rx_bytes`,
`rx_envelope_bytes`, `tx_data_datagrams` and `tx_data_bytes` are successful Steam/API relay counters; byte figures include
the exact envelope for transmitted datagrams and payload bytes for ENet data. A clean local link should normally have low-single-digit transport RTT, but an
overloaded OS or a real Steam route can legitimately take longer.
`tx_enet_protocol_bytes` and `tx_enet_ack_bytes` estimate uncompressed outgoing ENet command and ACK header bytes.
`tx_enet_parse_failures` and `tx_enet_unparsed_datagrams` expose malformed or compressed packets that cannot be counted.
The five-second `enet_header_bytes_per_s`, `enet_ack_bytes_per_s` and `protocol_overhead_pct` fields separate ENet
headers and ACKs from ENet application payload; they do not include the Steam envelope or the physical network.

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

The current source uses **protocol 77**. Its host-only NPC overhaul and host-managed saves are experimental; native gameplay acceptance is still
in progress, so this source must not be treated as a verified release. Both players need the same build; older protocol peers are refused. Personal loot,
NPC merchant inventories and automatic player exchanges are retained.

The host normally simulates NPCs, including remote companions, guides and summons. Separate non-rendered prefetch cameras
load distant simulation regions; the real camera, static-world culling and building LOD remain unchanged.
`host_roi` controls NPC coverage, not rendering distance. The old `host_sim = off/elastic` choices no longer transfer
simulation to a client.

The host logs one `npc-stream:` line per remote player every 5 s: both positions and their distance, whether the host's
simulation is `gate=open` or `paused`, how many distant regions/spheres it keeps (`regions=`, `region_dynamic=` = entities
loaded for that player), the host NPCs it has within `host_roi` of that player (`near_roi=`, `nearest_cm=`), how many came
from the interaction list or the engine's processing list, and per second what was considered, sent and released, with the
rule that kept the rest back (`top_drop=`: `outside` = farther than the 80 m interest radius from that player, `range`,
`hero`, `lease`, `pending`, `mirror`, `givenback`, `away`, `unchanged`, `paced`, `invalid`, `failed`). `world: spheres
suspended` means the host's own native session stop paused host simulation; it resumes when the session runs again.

By default, the host suppresses 24 reviewed world-data Goblin/Ogre instances below/alongside Ardea
that extended NPC coverage can bring into the starting village. This is non-death suppression:
no bodies, loot, XP or quest callbacks. Other regions, summons and quest-specific creatures are unchanged.
The host can select `ardea_remove_npcs = Goblin,Ogre`, `Goblin`, `Ogre`, or an empty value to disable
the rule. Names are exact; invalid/duplicate names are logged and leave the previous setting unchanged.
Clients follow the host's reliable removal records, including after rejoining/loading.
Choose the setting before making the co-op save: disabled entity flags already written into a save are retained.
`npc_authority_fallback = on` (the default on both copies) makes a client-loaded NPC live natively on the client
whenever the host is not presenting it (no fresh host stream): humans, orcs, goblins, slaves, guards, merchants and
other named/quest/dialogue actors, immortals, equipped actors and every creature class run their routine, movement,
animation, combat AI, hit reactions, damage, death, dialogue and trade natively. Only the hero, remote player puppets,
replicated summons/party/owned actors, item/spell actors, duplicate identities, authored removals and NPCs the host is
currently streaming stay host-driven. There is no per-NPC dwell: ownership starts at once, after one stream-settle grace
(at most 2 s, or the host's first NPC batch) per session. A host stream that arrives for an owned NPC wins: the client
yields (local state is discarded, nothing freezes) and a 1.5 s cool-down prevents flapping.

The host handshake runs asynchronously: the client sends its latest checkpoint (`Adopt`), the host grants an epoch
(`Offer`/`Prepared`/`Commit`) when it is not presenting that NPC, and from then on receives 5 Hz state. A refused, late
or lost handshake, a silent or paused host, a checkpoint, a save, a failed processing registration or an expired lease
window never stops a client-owned NPC. Processing registration (`RegisterProcessing`) only reports which native path
(already processed / queued / enabled) put the NPC in the engine lists. One owner per NPC: if a lower-numbered client is
also within 100 m of it, that client owns it and this one keeps it visual-only.

Hand-back: when the host walks into range the host recalls the NPC; the client cuts from its latest state (combat can
defer the cut for at most ten seconds), the host imports the latest accepted transform/HP/life/attitude/routine,
restarts natively and commits a newer epoch, and the client yields to the host stream. If the host cannot commit within
2.5 s the client reclaims the NPC instead of leaving it frozen. The host keeps its own inventory/equipment; personal loot
stays personal; import never reruns Kill, XP or treasure generation (the native death on the client runs once, the host
journals the quest effect once).
Read `npc-own:` lines in the log: every 5 s `npc-own: summary` (host_streamed / native_owned / confirmed / visual_only /
waiting / refused / frozen with top reasons, hits_applied, deaths, hand_backs, yields, takeovers, reclaims, registration
paths) and one `npc-own: event=...` line per transition per NPC (loaded, owned, confirmed, waiting, refused, hit,
reaction, death, frozen, yielded, handback, reclaimed, host-silent, registration).
`npc_authority_fallback = off` keeps the pure host model and safe unavailable presentation.

Save with both players together. Client-owned NPC state never blocks a save: only the host's native world
is saved, including any fenced/stale host copy; a client-only creature without a host instance contributes
no instance. Client-only injury/death/adoption is deliberately lost on reload. Saving does not recall,
import or reset leases, so the live encounter and normal hand-back continue unchanged.
No lease overlay (extension type 256) is written or retained in new saves. Old v1/v2 overlays and
`npclease1 ` text are ignored on load, never applied over the native world; container integrity checks remain.
Knocked-out players still block saving: "A player is knocked out. Revive them first."
Both copies must use the same rebuilt DLL. Protocol 77 and lease wire version 2 are unchanged.

NPC captures use a fixed **60 Hz deadline**, including unchanged NPCs, independently of the player's `send_rate`.
Per-client interest and change detection decide which captured states are actually sent. Nearby moving NPCs use
30 Hz; combat, focus, contact and dialogue retain the high-rate stream. Changed idle poses and distant movers use
10 Hz, and unchanged states receive an independent refresh every second. The interest radius is 80 m on entry and
100 m on exit; combat with or a party relationship to a player also keeps an NPC relevant.
The integrated player-state stream supplies fresh remote focus/target hints; standalone pose builds retain safe
zero-valued fallbacks. Remote camera direction remains unknown, so nearby movers are treated conservatively.
A slow host cannot produce 60 distinct samples
per second: `SYNCNPC` logs capture ticks, missed deadlines and elapsed capture time. Clients interpolate final bone poses,
facial-rig state and eight motion slots on one buffered timeline, at least 34 ms behind the host for fast captures,
66 ms for ordinary movement and 200 ms for slow captures; the existing
latency adaptation then smooths the buffer without predicting future NPC movement. HP, stance and
life transitions follow that same presentation timeline. Missing or stale presentation cannot authorize an incoming hit.
Player-puppet smoothing remains separate.
`SYNCLATENCY` reports desired buffer and actual `shown_lag` separately: slew/holds can make them differ.
`jitter_avg_ms`/`jitter_max_ms` expose the arrival/capture-interval variation driving adaptation.
`shown_lag` and `transit_excess` use the track's minimum arrival-minus-send clock-offset estimate, **not synchronized
one-way timing**; `arrival_age` is time since the newest local receive. `underruns` counts buffer recovery events in the
currently tracked NPCs; age/lag maxima can include a deliberately held terminal pose. Do not add these estimates to an
RTT as if all components were disjoint.
The host also sends each NPC's attitude to the player, so name-tag colours (green, white, orange, red) match on both PCs, also
while either player lies knocked out and after the revive (the colour is the NPC's relation, never changed by a knock-out). While
you talk to an NPC on the client, that one NPC's script routine runs locally so its voiced lines play; the host still holds
and animates it. Both are built for manual testing and not yet verified live.

Facial or skeleton changes retain the buffered old pose until the host's transition time. The nearby-NPC warning
requires the same living NPC to lack usable presentation for two seconds; settled corpses are not pending live poses.
Discarded pose fragments release their receive budget, so packet loss cannot gradually exhaust that budget and stop
NPC updates. The 16-bit unreliable sequence of a pose channel is reset on a second, idle pose channel, so a lost reset
packet can neither hold back nor starve the live pose stream. ENet's RTT-jitter throttle is off: it only drops more
fixed-rate pose frames. Native clip bookkeeping or a pending clip name cannot invalidate a successfully rendered host skeleton.

The host sends final skeletal matrices because native blend-out layers survive outside the eight motion slots.
Poses use sub-centimetre translation quantization; compact rotations have a strict per-bone error guard and
exact affine fallbacks for scale/shear. Ordinary poses travel on a separate lossy
ENet channel; essential life/party/hit changes remain reliable on the NPC channel. Owned loopback sockets
have burst capacity for crowded scenes. `NPCPOSE` and `PASSIVE` diagnostics distinguish queued/received poses,
presentation waits and completed skeletal updates. This full-pose stream costs more bandwidth than clip-only replay;
internet throughput and multi-player resource limits still need acceptance.

Internal bone format 74 caches bone layouts and full NPC motion baselines per world. After each client acknowledges them, updates
send the existing bone values without repeated names and compact anchored control records. Ordinary nearby moving NPCs
use a 33 ms (30 Hz) interval; distant movers and idle animation use 100 ms (10 Hz). Combat, focus, contact and dialogue
stay at capture rate. Fast and ordinary tiers have 2 s exit holds, and new interest gets a 200 ms warm-up.
Exact temporal/spatial residuals retain the Q12/Q2 affine fallback. Near-rigid bones may instead send a
smallest-three quaternion only when host reconstruction changes no basis entry by more than two Q12 units.
Every bone origin and facial/nonrigid translation retains its exact original Q2 values. Significant scale/shear
always uses the exact affine path. For originally rigid rotations the conservative nearest-rotation
bound is below 0.15 degrees (including existing basis quantization); a vertex within 200 cm
of its influenced bone has at most 0.293 cm added displacement. Actual native skinning extents and visible
acceptance remain unmeasured. Keys retain the original full-precision affine pose, and each update is encoded
from the current original capture against explicitly ACKed decoded references, so error never accumulates.
Loss, reordering, life/layout changes and missing
references fall back to keys; periodic keys are due every 5 s within the 32-pose retention window. Full records remain the
fallback for a missing dictionary, an unacknowledged baseline or values outside the compact range. The log reports layout
sizes, resync requests, anchors and fallback failures.
Scalar HP, action, target, equipment and relationship changes use exact sparse overrides against the named
control baseline rather than repeatedly offering all eight unchanged motion descriptors. Essential reliable
records preserve every original field bit-for-bit and the same reliable channel/publication ordering,
but omit acknowledged immutable descriptors; original position/quaternion and active motion floats
remain full precision. A descriptor, life or anchor change retains full-record baseline/key recovery.
The codec chooses the smallest exact or guarded candidate, including Q14/Q12 quaternion grids and
global/per-16-bone Rice parameters. It never reduces a selected update's rate or bone set.
Kind 49 reuses its outer epoch/publication/count instead of repeating a 14-byte inner envelope;
the explicit control generation and all baseline/life/sequence checks remain intact.
Pose interval hints announce slower presentation tiers before the rate drops. The mirror buffers two advertised
intervals plus bounded jitter, preserves the 34 ms fast floor, and slews its clock without rewinding.
Hints do not mask a slower measured host cadence; the larger interval still increases the buffer.
Native motion-only reconstruction remains disabled: final blend/constraint/facial output is not determined by
the eight motion descriptors, and canonical bone playback remains necessary for contact safety.
Every five seconds, host and clients log `net: bandwidth` summaries. Kind entries are
`message-id=tx-packets/bytes,rx-packets/bytes`: successful ENet queue operations are counted once per recipient,
and received logical messages include those rejected by validation or the world gate. Empty internal packets have
no kind. The `npc_per_s` fields count scheduler decisions per second, not deliveries. `enet_loopback_*` is the ENet
host's local UDP packet and byte counter delta, including protocol
overhead, retransmissions and acknowledgements, but not IP headers or Steam traffic. ENet's transmit packet counter
also includes a zero-byte socket-would-block attempt. None of these figures measures Steam's actual network usage.
`net: pose components` separates codec header/model masks, rotation/translation residual bits normalized to
bytes, control and outer headers. Bitstream padding can add less than one byte per update. Included-bone
counts describe nonzero encoded corrections, not discarded bones. `rigid_bones_per_update` shows use of the
bounded quaternion model; `host_observed_basis_error_q12` is the host's actual maximum (client `-1` means
unavailable), with derived rotation and 200 cm lever-arm bounds. Bone-origin error is always zero.
`tx_enet_relay_MB_per_s` reports decimal MB/s from ENet egress plus observed relay headers;
`tx_total_bytes_per_s` also includes the small Steam-side control payloads. Neither is a bandwidth cap.
`net: pose bandwidth` adds advertised-tier update/byte rates, bone payload bytes and bones per selected update,
key/delta counts, ACK misses, missing references and decode failures. Bone payload counts include its compact
flags/interval/reference/mask envelope. Kind 51 is the lossy generation/life-fenced pose ACK or key request.
When the integrated per-peer valve is present it never gates the normal NPC stream: every due update is sent. A real
congestion signal (ENet's packet throttle, a standing outgoing queue, RTT growth with queued data, fresh loss) multiplies
every NPC interval and staleness limit by one common factor (`npc_stretch_permille`, 1000 = normal, at most 4000), so all
NPCs slow down together instead of some being skipped. A first update for an NPC (join or world entry) is spread at
60 NPCs/s per recipient; established NPCs are unaffected. `npc_per_s`'s `paced` counter counts those deferrals and
distinguishes them from an unchanged NPC or a queue failure. `SYNCSTREAM` (host) reports the achieved update interval per tier
(min/avg/max ms, count, `overdue` beyond the 100/150/300 ms staleness limits), edges, quiet heartbeats, join-burst
duration and the stretch; `SYNCSTREAM` (client) reports update spacing and how many mirrors are older than 150/400/1000 ms.
Independent keys are due every 5 s; every delta references a frame the client acknowledged.
`net: bandwidth rates` `tx_total_bytes_per_s` is ENet bytes plus relay-header bytes per second, the envelope-inclusive figure.
Small poses are queued without intermediate flushes and share ENet datagrams; keys exceeding 1,172 bytes
keep explicit unreliable fragmentation. Rates and exact compression reduce traffic, but all-combat scenes,
long RTT beyond the 32-pose retention horizon and frequent descriptor changes can exceed 1 MB/s.
Snapshots of player trade goods are sent on changes and roughly every 30 seconds while active; world entry and joins
also trigger synchronization. Explicit transaction publications remain reliable and immediate.

Player state defaults to `send_rate = 30` (20-60 Hz supported). Full-float transforms and motion/life timing
are retained; names, equipment, appearance and focus/target references travel reliably on changes and in
five-second keyframes, rather than repeating fixed-size strings every frame. Player smoothing retains its
100 ms buffer; local controls and reliable combat events are not rate limited.
`network_budget_kib = 256` sets the pacing base of the bulk save channel per outgoing peer (64-4096 KiB/s); it is not a cap on
NPC, player-state or gameplay traffic, which are never refused (see the NPC valve above).
Save bundles use a separate ordered reliable channel, 1,148-byte data chunks, at most four chunks per frame,
and a quarter-rate bulk allowance. Congestion pauses the retained cursor without discarding data.
Story join snapshots are sent only to the joining peer and split into unfragmented batches.
The five-second `kind_bps` lines show application bytes/s; `rates` shows ENet and successful Steam API
datagrams/bytes per second and exact relay framing share. `valve` reports the NPC stretch, the congestion signals that raised it,
queue, offered rate and the bulk pacing credit. Steam API counts still exclude encryption, SDR, UDP/IP and other physical-network overhead.
Range-coder compression remains disabled: the engine-free benchmark shows benefits for compact players and
structured control data, but no saving for high-entropy data. Representative pose traffic and its CPU cost
must be measured before enabling compression globally.

Your own successful hit shows its local flinch immediately and is claimed reliably from the host. A host-side parry
does not cancel that landed hit. Hit identities, acknowledgements and NPC incarnations prevent duplicate claims and
suppress the attacking client's echoed flinch; other players still see the host's reaction.
Damage to your hero remains locally judged against the attack you are shown, not a host-issued HP subtraction.
Incoming NPC melee contacts wait for vanilla's hit point (60% of the displayed attack Hit clip), not the first
overlap during its windup. The same display clock drives motion, bones and contacts; it cannot catch up faster
than native time while that NPC fights your hero. Network jitter may extend a telegraph, never compress it.
Blocks, parries and retreat use your real hero immediately, not the host puppet's delayed stance or position.
Incoming combat motion starts are reliable without changing pose/control rates or the wire layout.
The permanent `combat: NPCATTACK` lines correlate host motion start/capture/send and contact, client receive/display/seek
and contact, and the native local block, HP and stamina result by NPC and motion instance. `host_*` timestamps
use the host clock, `client_*` the client clock; `presentation_ms` excludes the track's estimated link/clock offset.
`compensation_est_ms` adds half the measured RTT to that presentation age; it is an estimate, not a host damage delay.
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
2. The other player starts **New Game** locally. Before admission, the host restores that Steam account's
   saved character and personal loot/merchant state if present. The host calendar, authoritative story/quest-runtime
   and nearby shared-door snapshots finish before admission. The applied cutover is confirmed before NPC/motion
   entry replays. A new account gets a fresh character, never the host's hero.
3. **Either player requests a save** using manual/quick-save controls. The host collects small current
   character/personal-state records and performs **one native save**. Clients never save or receive native world files.
4. Wait for **"Co-op save complete"** before quitting. Dialog, downed/control, loading or an unsettled
   player exchange can refuse explicitly; combat alone is not a native refusal. Retry after the stated
   condition clears. A disconnected account retains its preceding saved character, not unseen progress.
5. **Only the host loads** from the normal menu/quick-load controls. A new host world epoch makes each
   connected client re-enter the host's world automatically, without restarting Gothic 3: the client loads its
   own **rejoin template** through the game's normal load path, then receives the host's world state and its
   account-exact saved character. A connection-only rejoin in the same live world keeps that player's live character.

**Automatic client entry (`client_autoload`):** the host's reliable world announcement is the load event; there is no
protocol or save-transfer change. With `client_autoload = on` (the default, set in each client's own `g3together.ini`; the
host's INI ignores it) the client waits until the host's world is playable and has stopped changing, shows
*"The host's world is ready. Entering automatically in N s with your saved character. Press Esc to stay in the menu."*
(4 seconds, in the menu or as a HUD notice; rapid host loads restart the countdown, so only the latest world is
entered) and then enters it with its own account character, or a clearly worded fresh hero if the account has no record,
never the host's hero. This works while playing (the old world is replaced once dialogue, trade, a pending
co-op save result and native loading are done), from the title screen, and for a client that joins after the host has
already loaded; while the host is in its menu or loading the client waits. **Esc** during the countdown, or choosing
**New Game** yourself, cancels it for that host world only (the next host load asks again); `client_autoload = off`
disables it, then **New Game** (also in the in-game menu) enters on demand. Before the rejoin template exists (first
launch of Gothic 3) the automatic entry is the same first New Game you would choose; it also saves the template.
A failed start is retried after 3 s and 8 s, then reported; 60 seconds without the host's admission shows a notice and
never leaves the world. A client already playing in the host's world (initial startup, same-epoch reconnect) is
never restarted, and a protocol/install mismatch never enters.

**Client rejoin template:** the first time you choose **New Game** after launching Gothic 3 (also the way to
join the very first time; with `client_autoload = on` and a joined host world that first New Game is started for you),
the game saves the fresh starting world once into your normal save folder as the
slot `G3T_client_template` (three small files; nothing else is created, replaced or deleted, and it is never
sent over the network). It is made on your own machine from your own New Game, so it matches your installed
game; it is rebuilt automatically if the game files change. Later host loads/new campaigns use it to replace
your world safely: Gothic 3's own New Game cannot restart a world that is already loaded (it froze the game),
so a second New Game in the same launch is refused with a notice. Without the template (first launch, or the
file was deleted) nothing is torn down: restart Gothic 3 once and choose New Game. Unsaved progress is
discarded, not saved over your last host-saved record. A campaign with no record for your account starts a
fresh hero from the template, never the host's hero.

A host load/new campaign queues the re-entry outside network callbacks. Native loading must finish first;
an outstanding save result or dialogue/trade can defer it, with a notice. Finish the dialogue,
close the trade window and let player exchanges settle. An unrecoverable exchange still blocks a manual
re-entry rather than silently discarding an unresolved transaction. The host may remain in its menu
or loading screen: admission waits for a ready host world.
Re-entering never writes a new character checkpoint and never loads or receives the host's native save;
only the client's own `G3T_client_template` is loaded, by the game itself.

The host's `<menu-slot>.g3t` is a self-contained **G3TSAVE3** container: native header/world,
shared journal/companions, individual G3THERO4 and bounded personal-state records, and versioned extension blocks. Whole-file
and block CRCs detect corruption. Native file completion is checked after the native save returns;
brief missing/locked-file retries do not invoke the native save again.
The menu caches are published with rollback, then a flushed temporary file atomically replaces the container.
The game's ordinary native menu pair is a recoverable cache, rebuilt from the container on load.
There are no client manifests, aliases, paired native saves, or 2 MB network save transfers.
Back up/copy the host's `.g3t` with its menu pair (or the whole isolated save folder). The `.g3t`
alone contains all data, but the game's load menu needs its native header cache to list the slot.
The host's native Delete menu also removes `.g3t`; deleting files manually must include the entire slot.
Do not treat the cache alone as a complete co-op save.
Interrupted `G3T_pending_*` staging slots cannot be loaded.
A `.g3t.publish` marker guards interrupted cache publication: an intact container repairs the pair
on load, while an interrupted first save without a complete container is refused. Save again to repair it.
File failures name missing/empty/locked native files, denied access, container write/rename, or menu-cache
publication separately. The log records the operation, paths and original Windows error.
Disk-space advice is shown only when Windows reports disk-full or quota exhaustion.

G3THERO4 preserves stats, base/aggregate equipment modifiers, current/max HP/MP/stamina,
XP/level/LP, inventory/gold/equipment, skills/spells, quick slots and position/orientation.
Temporary-effect ownership/expiry and live inventory/equipment mutation still require manual acceptance.
Each client keeps a small account-local character backup when sending a save record; it is insurance,
**not an automatic latest-character fallback**. The host's save is the source of truth.
An absent/wrong account starts new; a corrupt existing record/container blocks admission/load.

Plain native saves, G3T-prefixed old slots such as `G3T_Sandbox`, and intact G3T1/G3T2 journals
remain host-loadable. Old external client records are not silently imported or reassigned to another account;
clients start fresh if the host lacks an account-bound record. Keep original legacy backups.
Earlier G3TSAVE3 containers also load; a missing personal-state block uses native/fresh defaults, not a local latest backup.
Previously unrecorded client loot depletion cannot be reconstructed from those older containers.

**Acceptance boundary:** clients use a fresh local world, account-specific overlays and authoritative replication,
not the host's serialized world. Initial quest restoration sets status/runtime data without granting rewards again,
after restoring the host's calendar rather than evaluating saved timers against New Game time;
shared doors are queried from the host rather than assumed identical. Arbitrary native serialized objects,
client-created loose drops and temporary effects are not a complete per-player world save. The two-copy checks
in PLAYTEST.md remain mandatory before relying on long campaigns. Pick up dropped equipment before saving.
No autosave-on-exit is added.

## What is shared

Protocol 62 makes ground pickups, corpse loot, chest contents and NPC merchant inventories personal.
Taking or buying an item changes only that player's copy; each player pays from their own gold.
Chest locks and use reservations are local too; other shared doors and usable places are unchanged.
Direct trades between players still transfer real items rather than copying them.
New host saves preserve each account's generated/depleted chest/corpse stock, merchant stock and consumed
loose pickup GUIDs. Inventory quality/type and personal chest door/lock state are restored lazily before use.
Unloaded entries remain in the record. Missing blocks in legacy saves use native/fresh defaults.
Required inventory hooks are checked against the supported binaries. Dirty inventories that unload before a reliable
refresh refuse saving rather than silently use stale stock. Startup restoration waits for those hooks before changing
the hero. Consumed loose pickups cannot grant items again while native removal is still pending.
This is a bounded personal overlay, not a reconstruction of every dynamically created native world object.

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
  connected, actively playing partner who is not downed or dead can help, regardless of distance,
  region, interior, or world location. The partner can travel back; being outside the downed player's
  loaded area does not end the run. Return to the same loaded world and stand beside the downed player's
  character and choose "I will help you." to help them up. The cheapest healing potion in the
  helper's inventory is consumed once for that knock-out.
  With weapons away, the lying partner's focus is Talk, not Loot, on both PCs.
  A player waiting for help cannot be looted, quick-looted, robbed or pickpocketed;
  NPC combat exclusion does not make their remote body a corpse.
  Without a potion, both players see a notice and the downed player waits for their partner to
  return with one.
- If everybody is known to be down or dead, the hero dies immediately as in single-player.
  Missing, disconnected, loading, menu, or stale **player activity** keeps the original
  five-second continuous grace instead. Activity has a separate reliable heartbeat at least once
  per second, independent of NPC interest and world/presentation gates. Five seconds of silence
  makes activity stale; the additional five-second grace then starts. A stale position or another
  world alone does not make an active partner unavailable. The host loads a
  save; the other player starts New Game locally and receives their saved character before admission.
  On each PC, the local hero enters native death once (death scream, ragdoll and
  the localized game-over/"The End" message). Both heroes and their remote bodies
  stay dead; neither healing nor a late revive can restore gameplay. The native
  menu remains available to load a save; death does not automatically reload.
- While either player is knocked out, host-controlled NPCs and creatures drop that player as a combat
  target and choose another eligible enemy or return to their routine. Further attacks cannot damage,
  stagger, push, or apply harmful statuses to the held player, including in-flight attacks and
  ongoing poison/burning or environmental damage. Protection is local to the hero's owning PC.
  Paid revival remains protected through standing up and for another 1.5 seconds; normal hostility
  and target selection resume without making enemies permanently friendly.
  Remote puppets use the owner's existing held/pose/motion snapshots for matching recovery protection,
  rather than inferring it from their talkable native mode.
  Ordinary knockdowns retain vanilla's exact Sit/Lie knockdown pose protection for melee contacts,
  not a new invulnerability timer for all stumbles, falls, spells or environmental damage.
- Both players are immune while speaking with an NPC or using a merchant/partner trade window,
  including the first-trade tutorial or a menu covering that window. Damage, harmful status effects,
  hit reactions, knockdown and attack knockback are rejected on the player's own PC and their remote
  puppet. Protection lasts another 1.5 seconds after all dialogue/trade windows and spoken dialogue
  end, including Esc/abort. Enemies retain native hostility/targeting; this does not cancel the NPC's
  talk routine or make enemies friendly. Loading/world changes clear interaction protection.
  The existing player flags/heartbeat carry the state; protocol 77 and packet sizes are unchanged.
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
  are unchanged for standing players. Both players must use the same value and restart after changing it. An explicit value from 1 to 100
  retains the previous cross-player damage scaling; setup preserves existing values.
- Melee swings use Gothic 3's NPC bystander rules on both PCs: a client swinging at a hostile NPC does not hit a
  more friendly bystander the native game would protect. This is separate from `friendly_fire`: deliberate attacks,
  self-defense and the game's duel/crime rules still apply. Both PCs need the matching protocol 77 build.
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
