# G3 Together

**Return to Gothic 3 with a friend.** Share the campaign, fight side by side, and help each other back up when a battle goes wrong. One player hosts the world; the other joins over the network.

[Release packages](https://github.com/m0rticus/g3-together/releases) · [Quickstart](#quickstart) · [Player guides](#player-guides) · [Build and test](#for-developers)

> **Steam-only fork.** Build from this checkout or use a package built from it. Upstream release packages still support
> direct UDP and do not contain this change. Steam is required; IP/LAN/VPN joining and IP fallback are removed.

The current source speaks **protocol 75**. Its host-authoritative NPC overhaul and paired saves are experimental and native gameplay
acceptance is still in progress; it is not a verified release. Both players need the same build; older protocol peers are refused.
The host simulates NPCs around every player, using independent non-rendered cameras to retain distant regions.
Clients replay the host's final skeletal poses, facial-rig state and eight motion slots without NPC AI, navigation,
autonomous tasks or prediction. Final poses include native blend-out layers that motion slots alone do not describe.
NPC captures use a fixed 60 Hz deadline; diagnostics report missed captures when the host is slower.
A client's successful hit plays an immediate presentation-only flinch and counts despite a host-side parry.
Hit identities suppress the corresponding echoed reaction. Hero damage is judged against locally presented contacts,
and ragdoll impulses travel only from host to client.
Player-puppet smoothing remains separate. Party identity, friendly-fire protection, automatic player exchanges,
personal loot/merchant inventories, shared experience and lethal-blow rescue rules are retained.
Player states default to 30 Hz with reliable changed metadata and recovery keyframes.
`network_budget_kib` defaults to 256 KiB/s per outgoing peer; reliable gameplay is protected,
and save bundles are paced at a quarter of that budget on a separate reliable channel.

> **Beta — back up your saves.** Human co-op testing is limited; no complete human co-op playthrough has been completed. Internet play remains unverified; only two players have been tested.

## What you can do together

- **Share the adventure.** Quest progress, dialogs and story milestones sync. Listen to nearby conversations and see your partner's dialog choices.
- **Fight as a team.** Shared enemies, visible attacks and spells, individual kill XP. Quest XP is shared; dialog gold goes to the player handing in.
- **Personal loot and shops.** Ground pickups, corpse loot, chest contents and NPC merchant stock belong to each player's copy.
  Taking or buying something does not deplete the other player's copy. Picked pockets still use shared NPC state.
- **Trade together.** Use the game's trade screen to move real items freely or give gold. Exchanges synchronize automatically, without partner approval.
  A later host-save load still replaces native world inventories; independent world-inventory save reconstruction is not implemented.
- **Look out for each other.** Find your partner on the map and compass, jump to their side, and revive them with a healing potion.
- **Keep your character and campaign.** Either player requests a paired checkpoint: the host's world plus each Steam account's own character record.
  Wait for the paired-save confirmation before leaving; native gameplay acceptance is pending.

More: [combat, companions, guards and world interactions](docs/PLAYING.md#what-is-shared).

## Quickstart

### 1. Match your installations

On **both Windows PCs**, use the tested setup in this order:

1. Gothic 3 from Steam with **Community Patch 1.75.14**.
2. [**Parallel Universe Patch 1.1.1**](https://www.worldofgothic.de/dl/download_678.htm).
3. The **same G3 Together release package**.

Game files and settings must match; the mod checks compatibility on joining. See the [full requirements](docs/PLAYING.md#requirements-the-same-on-both-pcs). Compatibility without Parallel Universe Patch is unverified.

Prefer a separate game copy for co-op. Keep other co-op mods out of `scripts`.

### 2. Run setup on both PCs

Unpack the **whole mod package built from this fork** and run `Setup.cmd`. GitHub's source-code ZIP does not include the built mod.

Choose one **host** and one **joining player**. Player names come from Steam automatically. Setup finds the game, checks patches and installs the required
Steam DLL and mod with `g3together.ini`. If the verified Steam DLL cannot be installed, setup stops.
Replaced files go into `G3Together-backup`. See also [manual installation](docs/PLAYING.md#installation).

### 3. Connect and play

Both players must own Gothic 3 on Steam, have Steam running and be Steam friends. The joining player's game finds the host through Steam:
no host address, VPN or router port forwarding is needed. Steam failures are reported without opening another connection.
ENet's internal UDP sockets bind only to localhost. See [over Steam](docs/PLAYING.md#over-steam-experimental).

Choose the same **Session ID** in setup on both players, or set `session_id = pair-red` in both
`g3together.ini` files and restart. For six concurrent instances in three independent pairs, use `pair-red` on
the first host/client, `pair-green` on the second, and `pair-blue` on the third. Only hosts with a matching ID
are eligible for joining; different IDs never exchange gameplay traffic, even through Steam invites or explicit lobby numbers.
IDs are case-sensitive, 1-63 ASCII letters, digits, `_` or `-`; they are visible labels, **not passwords**.
Omitting the setting keeps the shared `default` group. Each live instance needs a distinct Steam identity;
this does not add multiple independent instances under one Steam account. Use isolated game/profile/save folders
when running multiple copies on one PC.

The main menu shows the lobby state, joined/ready client counts and measured **transport RTT** (or `pending` before a
sample or after five seconds without a reply). This is a high-resolution Steam control echo, not NPC playback delay.
It also shows the configured Session ID. Both players need this updated build: older Steam relay builds are incompatible.
Steam activity uses **Gothic 3 (AppID 39500)**, not Spacewar.

| Host starts first… | Joining player… |
| --- | --- |
| Load a saved campaign. | With a saved own character, **wait in the main menu**. A first-time player must start a new game or load their own hero first. |
| Start a new, unsaved campaign. | **Start a new game too.** Once the host has saved, future joins can load from the main menu. |

**Either player can save**; the host creates the authoritative world checkpoint and every ready player stores it with their own character.
Keep Steam display names unchanged to restore companions (character records themselves use the Steam account ID).
Read about [first-join heroes, paired saves and recovery](docs/PLAYING.md#starting-and-saving).

## A few things to try

| Want to… | How |
| --- | --- |
| Regroup | **Page Down** jumps behind your partner in the same world; five-second cooldown. |
| Revive your partner | Talk to them: **“I will help you.”** Costs one healing potion. If nobody can help, normal death still applies. |
| Trade | Talk to your partner: **“Show me your wares!”** Select items or gold and use **Trade/Swap**. Items are free; no partner approval is needed. |

Edit your `g3together.ini` and restart to customize messages, appearance and the regroup key:

```ini
language = auto
look = Diego
partner_key = F9
```

`auto` follows the game's text language: German, Polish, otherwise English. Override with `en`, `de` or `pl`; omitting the setting gives English. Each player chooses independently, without changing the game's language.

The host chooses `reputation = shared` (default), `together` (shared guilt) or `personal`; see [reputation rules](docs/PLAYING.md#known-limitations).

**Friendly fire is off by default.** Players and their followers cannot hurt, stagger, knock down or inflict harmful
spell effects on one another; healing still works. Existing INIs with a nonzero `friendly_fire` keep that opt-in
damage setting. Set `friendly_fire = 0` on both PCs and restart for full protection; both settings must match.

## Know the limits

- **Crashes and sync issues remain possible.** Save before difficult fights; see [known issues and workarounds](docs/PLAYING.md#known-issues-21-october-2026).
- **Human testing is limited.** Setup and the manual playtest checklist still need broader human verification. Mouse trading and forward swimming need human testing.
- **Untested story paths:** Adanos and Beliar endings, Xardas' plan, the fifth temple, Cape Dun's uprising, midgame town liberation, drive-away quests and plundering.
- **Soul travel:** your partner sees your ghost, not the body left behind.
- **Fire/ice effects that extinguish burning or freezing** on NPCs simulated by your partner only take effect on your PC.

Read the [full limitations](docs/PLAYING.md#known-limitations), including saving while transformed, before a longer campaign.

## Player guides

[Player guide](docs/PLAYING.md) · [Joining guide](docs/JOINING.md) · [Playtest checklist](docs/PLAYTEST.md)

### Found a bug?

[Report a bug](https://github.com/GGM1k3y23/g3-together/issues) with the time, host, actions and what **each player** saw. Preserve both players' logs before restarting. With Python installed, run from the extracted package folder using your game path:

```powershell
python "tools\collect_logs.py" "C:\Games\Gothic 3"
```

`g3together.log` is enabled automatically and appends dated session markers, lobby/connection errors, player identities,
relay counters and periodic Steam ping/status. Transport pumping is independent of the render loop; ENet/gameplay
handlers remain on the engine thread. `STEAMSTATUS`, `net: rtt` and `SYNCLATENCY` separate transport, engine servicing,
send-to-flush residence and buffered NPC presentation. While playing, compact `SYNCBODY` and `SYNCNPC` counters report hard
corrections, action corrections, stale mirrors and ownership takeover deferrals without per-entity scans.
`SYNCCOMBAT` distinguishes locally judged hits, forwarded/applied claims and damage ignored while browsing inventory.
Share both players' ZIPs privately with the host first. User paths and IP addresses
are redacted; Steam names/IDs can remain, so review before sharing further. Do not attach saves or unredacted INIs.
[Without Python](docs/JOINING.md#if-something-goes-wrong), collect logs manually.

NPC damage to a player browsing the inventory is blocked locally; the rest of the co-op world continues.
Corpse presentation uses local ragdoll initialization or observed death-loop animations, with retries when resources are unavailable.
Physics and bone poses are independent on each machine; death and quest/XP outcomes still synchronize.

Player-puppet smoothing retains its own movement controller and prediction. NPCs instead interpolate received samples
on an adaptive buffered timeline (at least 50 ms), hold on an underrun, and cannot authorize stale contacts.
NPC HP and life state follow that same timeline. Local hero controls are not buffered.
Combat/perception hooks also cover native direct calls when their instruction prologues match, and damage judgement can
recognize a local player's hit on a mirrored NPC without enabling that NPC's independent AI.
Native hostility, immortality, duel and damage rules remain in effect; this does not force friendly NPCs to become enemies.

## For developers

Built on the unofficial [Gothic 3 SDK](https://github.com/georgeto/gothic3sdk) for the Genome engine.

### Build

Use **MSVC, 32-bit**. This example needs Visual Studio 2026 C++ tools and [CMake 4.2+](https://cmake.org/cmake/help/latest/generator/Visual%20Studio%2018%202026.html) for its generator (project minimum: 3.24).

The SDK is a separate checkout. Its default path is `workspace/tools/gothic3sdk` (inside this repository, gitignored); add `-DG3SDK_DIR="C:/path/to/gothic3sdk"` to the first command to override it. From the repository root:

```powershell
cmake -S . -B build -G "Visual Studio 18 2026" -A Win32
cmake --build build --config Release
```

### Unit tests

The engine-free unit suite uses temporary files and mocked/synthetic data. It does not boot Gothic 3, start Steam, install a mod, or inspect private workspace contents:

```powershell
python -m unittest discover -s tests -p "test_*.py"
```

### Private manual workspace

```text
g3-together/
  src/
  docs/
  tools/
  workspace/                Private and gitignored
    tools/gothic3sdk/
    builds/
    game-copies/
    logs/
```

Keep `workspace/` private and uncommitted. Use separate Gothic 3 installation, profile and save folders for manual co-op
work. Back up real saves before installing or replacing files. Do not share private workspace contents or use them as
the source of a release package.

Run `Setup.cmd` inside a release package for manual installation into a separate copy. Follow the
[installation guide](docs/PLAYING.md#installation) and use the [manual playtest checklist](docs/PLAYTEST.md).
To collect a bug report manually or with Python, use [the log collection guide](docs/JOINING.md#if-something-goes-wrong).

Before manual play, optionally check that two separate Gothic 3 copies are safe to use:

```powershell
python "tools\copy_preflight.py" --copy "<first Gothic 3 folder>" --copy "<second Gothic 3 folder>"
```

The preflight can instead use `G3_WORKSPACE` or `workspace/project.json` for the configured copies and absolute
`protected_save_dir`. It checks filesystem layout and installed files; it does not launch or drive Gothic 3.

For an explicitly hung Gothic 3 process, collect its stack data with:

```powershell
python "tools\hangstack.py" --pid <PID> --output "<output.txt>"
```

Startup videos can be disabled or restored manually while stopped:

```powershell
python "tools\startup_videos.py" --disable --root "<Gothic 3 folder>"
python "tools\startup_videos.py" --restore --root "<Gothic 3 folder>"
```

These manual tools read or modify only the files and processes explicitly named. They do not launch, drive, inspect,
or assert a game session.

## License and credits

**GPL-3.0-or-later** — see [COPYING](COPYING). Built on the unofficial [Gothic 3 SDK](https://github.com/georgeto/gothic3sdk); bundles [ENet 1.3.18](https://github.com/lsalzman/enet) under its [MIT license](thirdparty/enet/LICENSE). With an [additional permission](COPYING.STEAMWORKS) under GPLv3 section 7 for combining it with Valve's Steamworks API library (`steam_api.dll`, the optional Steam connection). The Steam transport is adapted from [Jericho Co-op](https://github.com/dolbaras/jericho-coop) ([MIT](thirdparty/licenses/jericho-coop-LICENSE)).
