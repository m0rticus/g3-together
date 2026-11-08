# G3 Together: joining a host

## Before you start

- Both PCs should use the tested baseline: Gothic 3 from Steam, **Community Patch 1.75.14** and
  **Parallel Universe Patch 1.1.1**. Use the same G3 Together DLL and the same game files.
  Compatibility without PUP has not been established.
- **Difficulty is per player**: client Easy and host Hard can join; neither difficulty is overwritten.
  Quick Loot, test mode and menu preferences also stay local.
- **Balancing follows the host for the session**: alternative balancing (`ExtendedContent`),
  alternative AI (`AIMode`), `XPModifier`, `MinHitDuration`, `PlayerStart`, and the mod's
  `friendly_fire`, `npc_damage`, `npc_hp`. Different local values no longer refuse a join.
  The client adopts them through in-memory readers, before world entry, and restores its own
  readers on disconnect. No host values are written to `ge3.ini` or the client's other settings files.
  A differing set produces one "Using the host's balancing..." notice and a main-menu hint;
  `options:` log lines record host, original, effective and source for every setting.
- DLLs, script DLLs, rules archives and other Data archives still must match.
  A refusal names the first differing item and the game patches/mod files to install.
  Unknown `[Game]` keys conservatively remain installation identity: align those values rather
  than guessing whether an unrecognized extension changes gameplay. Whitespace/case is normalized;
  the known local/shared keys above are not hashed. Graphics, audio and key sections are not adopted.
- Host balancing is frozen within a world epoch. Restart the host when changing startup/INI
  balancing, then load/start the campaign; the client follows the new epoch through the existing
  automatic or manual re-entry path. Invalid host balancing refuses explicitly.
  Missing client `[Game]` entries use native defaults until adoption; they alone do not refuse.
  A client already in a world with conflicting startup balancing must re-enter through its
  client template. If that template is unavailable, a clear restart/template notice blocks admission.
- There must be no other co-op mod in the `scripts` folder.
- Steam must be running and logged in on both PCs; both players must be Steam friends.

## Installation

**Setup:** once Gothic 3 and both patches are installed, unpack the whole package anywhere and double-click
`Setup.cmd`. It does the copying and the INI for you: it finds Gothic 3 (Steam, GOG or the
folder you name), checks Community Patch 1.75.14 and Parallel Universe Patch 1.1.1, moves another co-op mod out of
`scripts`, copies the mod into `scripts` and writes `g3together.ini` from your role. Names come from Steam automatically.
Steam is required; setup stops if the verified `steam_api.dll` cannot be installed. Replaced files are kept in `G3Together-backup`.
The steps below also let you install by hand.

1. Install Gothic 3 from Steam, then **Community Patch 1.75.14**, then
   [Parallel Universe Patch **1.1.1**](https://www.worldofgothic.de/dl/download_678.htm)
   into the same game installation.
2. Copy `Script_G3Together.dll` from the same package as the host into `<Gothic 3 folder>\scripts\` and its verified
   32-bit `steam_api.dll` into `<Gothic 3 folder>\G3Together\`, or use setup to obtain it.
3. Copy `g3together.ini.client-example` from the package into `<Gothic 3 folder>\`, rename it to
   `g3together.ini`, and enter your details:

   ```ini
   role = client
   transport = steam
   session_id = pair-red
   language = auto
   ```

Your PC must use `role = client`. Your display name comes from Steam and is only a label.
Saved characters, personal loot/merchant state and companion ownership use your authenticated **SteamID64**,
not your display name or connection slot. Changing your Steam name does not change these records.
One SteamID can occupy only one joined player slot.

Older account-bound characters and personal records load unchanged. Old name-keyed companion entries migrate
once when their old name uniquely matches a known Steam account and no saved remote account is unidentified.
Unmatched or ambiguous entries stay in the save; the host sees a warning and `identity: migration pending` in the log.
If a name changed before migration, the host can explicitly select its owner in the host's `g3together.ini`:

```ini
legacy_companion_owner = 76561198012345678 Old Steam Name
```

Use the actual owner's SteamID64 from `identity: join`, not a player slot or lobby number. Restart the host after
editing, load the old save and let that account join; then save normally. The log records `identity: migrated`;
the next save writes SteamID-keyed ownership, without changing or replacing any character. Different old names
can have separate setting lines; conflicting assignments for the same old name are refused.
On `legacy_companion_owner` lines, `#` is part of the old name, not an inline comment.
Quote the whole value if the old name ends with spaces.

Set `session_id` to exactly the host's ID (setup asks for it too). IDs are case-sensitive and contain 1-63 ASCII
letters, digits, underscores or hyphens. Use a different ID for each independent host/client pair; leave it out
only to use the shared `default` group. The main menu and log show your ID. A Steam invite, `+connect_lobby`,
`steam_lobby` or `steam_host` does not bypass this match. An invalid ID stops networking instead of falling back.
IDs are public labels, not authentication secrets. Both players need the updated build; older relay builds cannot join.
The compact relay header is 16 bytes for `default` and up to 72 bytes for a 63-character ID. The lobby number,
session marker and exact session bytes must match before ENet data is forwarded.

`language = auto` shows the mod's messages in the language of your Gothic 3 texts (German, Polish, otherwise
English). To fix one, set `language = en`, `de` or `pl`; without the line the messages are English. Each player can
choose independently; this does not change Gothic 3's own language.

## Joining

The host starts Gothic 3 and begins a new campaign or loads the co-op campaign. Accept the host's invite in Steam,
or use **Join Game** on the host in the Steam friends list. This works whether your game is running or Steam starts
it for you. Once connected, with `client_autoload = on` (default) you enter the host's world after a 4 second countdown in the
menu (Esc cancels it), or choose **New Game** yourself. The host sends your account's saved character and personal loot/merchant state, applied before
admission. If no record exists for your account, you start fresh, never as a copy of the host.
Only the host loads saved games; after a host load the client re-enters automatically (same countdown) through its own `G3T_client_template` slot
(saved by your first entry of each launch; if it is missing the client makes it again by itself, no restart; see docs/PLAYING.md).
The host calendar, quest/status/timer/counter, dialog/event and nearby shared-door snapshots finish before admission.
The client applies cutover changes and confirms entry before one-shot NPC/motion replays start.
Restoration does not replay quest rewards. Native world files never travel over the network; gameplay acceptance remains manual.

"Connected" is only the transport. The main-menu top line and the HUD say whether you are **in the host's world**
("In <host>'s world.") or **not yet** ("Connected to <host>, but not in the host's world yet: ..."). If you ever end up in a
different game than the host (you started a New Game yourself, played before connecting, or the host's admission never finished),
the mod detects a mismatch and returns you to the menu and into the host's world after the countdown (Esc cancels);
if it cannot, it tells you why (autoload off or canceled: choose New Game; template unavailable: see the reported reason;
refused: compare versions; unsupported install: the "Host-managed saves OFF" line, on both machines). The log lines to send
with a bug report are the `admission:` lines in `g3together.log` (client and host).

If joining is refused, compare versions and game options with the host. Game compatibility refusals appear in
`g3together.log` in the Gothic 3 folder on a line containing `REFUSED by host`. Steam-level refusals appear in the
menu's top line and in `connect:` lines in the log; the reason stays until the next accept.

**Steam is the only connection path** (see [PLAYING.md](PLAYING.md#over-steam-experimental)). By default your game waits
for an invite; accept one or choose **Join Game** on the host. The host must be on your Steam friends list and have Steam
and its game running. The Steam desktop friends list works even if the overlay cannot open inside Gothic 3.
After a lost connection, accept again; the client does not reconnect by itself.
If Steam cannot start, a clear error is shown; fix Steam or the required DLL and restart the game. There is no IP fallback.

## While playing

- If you are downed, wait for your partner. A connected player in the same world can help you up by
  talking to you and choosing "I will help you.". This costs them a healing potion. Without one, you stay
  down until they have a potion. If everyone is down or you are alone, you die as in single-player:
  game over, then load a save.
- Carry healing potions for your partner.
- Each player's surroundings run on that player's PC and stay the same for both, so distance from the host shows no
  message. When your partner is in another world, you see **"<name> is in another world."**
- **Page Down** puts you directly behind your partner, including when they are downed. There is a
  five-second cooldown. To choose another key, set `partner_key = F9` in the INI (F1–F24, PAGEUP,
  HOME, END, INSERT, DELETE or a letter).
- Your partner is marked on your map and compass.
- Your partner sees the armor and helmet you wear. `look = Diego` in your INI shows you as another character.
- Trade with your partner: talk to their character, "Show me your wares!", select items or gold, then use the normal
  Trade/Swap button. Items transfer freely; selected gold is a gift. Your partner does not need to approve, and the
  window can stay open. Equipped gear, quest items and skills remain protected. Gameplay verification is pending.
- Either player can request a save. Wait for **"Co-op save complete"** before leaving.
  The host alone stores the world and every participant's character in one `.g3t` container.
  Back up the host's entire slot, not only its native menu pair. Clients retain only small character backups.
  Both players need the same protocol/build, now protocol 78.
- See **Known issues** in the [full player guide](PLAYING.md) for problems and workarounds.

## If something goes wrong

If world entry stalls, `g3together.log` reports `world-entry: still waiting for ...`
every five seconds from the transport worker, independently of the game frame.
The line gives the wait age, current mod/native operation, host epoch and welcome serial.
`world-entry: activation cleanup returned` followed by `operation=engine outside mod`
and a wait for the first in-world frame places the stall after the mod's activation callback.
An operation naming a cleanup, native load or character restore instead identifies that boundary.
`world-entry: complete` means this client passed the host-world readiness gate; it is not
logged merely because the Steam lobby joined. These diagnostics do not restart a load,
skip a missing character record or open the readiness gate early.

Open PowerShell in the extracted package folder, where this file is located, and run the following
command. Python is required:

```powershell
python "tools\collect_logs.py" "<Gothic 3 folder>"
```

Send the resulting `g3together-logs-<timestamp>.zip` to the host. It is created in the package folder.
Without Python, collect `g3together.log` from the Gothic 3 folder and the `.log` files from
`Documents\gothic3`, including its `paru` subfolder, manually.
