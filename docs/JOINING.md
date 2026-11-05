# G3 Together: joining a host

## Before you start

- Both PCs should use the tested baseline: Gothic 3 from Steam, **Community Patch 1.75.14** and
  **Parallel Universe Patch 1.1.1**. Use the same G3 Together DLL and the same game files.
  Compatibility without PUP has not been established.
- Difficulty, alternative balancing, alternative AI, XP factor, minimum hit duration, Quick Loot
  and the `[Game]` section in `Ini\ge3.ini` must match the host.
- There must be no other co-op mod in the `scripts` folder.
- Steam must be running and logged in on both PCs; both players must be Steam friends.

## Installation

**Setup:** once Gothic 3 and both patches are installed, unpack the whole package anywhere and double-click
`Setup.cmd` (`Einrichten.cmd` for German, `Instalacja.cmd` for Polish). It does the copying and the INI for you: it finds Gothic 3 (Steam, GOG or the
folder you name), checks Community Patch 1.75.14 and Parallel Universe Patch 1.1.1, moves another co-op mod out of
`scripts`, copies the mod into `scripts` and writes `g3together.ini` from your role. Names come from Steam automatically.
Steam is required; setup stops if the verified `steam_api.dll` cannot be installed. Replaced files are kept in `G3Together-backup`.
The setup was checked on test
folders, not yet by players; the steps below do the same by hand.

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

Your PC must use `role = client`. Your player name comes from Steam; keep that Steam display name unchanged: the host uses it to
restore your companions after loading a save.

Set `session_id` to exactly the host's ID (setup asks for it too). IDs are case-sensitive and contain 1-63 ASCII
letters, digits, underscores or hyphens. Use a different ID for each independent host/client pair; leave it out
only to use the shared `default` group. The main menu and log show your ID. A Steam invite, `+connect_lobby`,
`steam_lobby` or `steam_host` does not bypass this match. An invalid ID stops networking instead of falling back.
IDs are public labels, not authentication secrets. Both players need the updated build; older relay builds cannot join.

`language = auto` shows the mod's messages in the language of your Gothic 3 texts (German, Polish, otherwise
English). To fix one, set `language = en`, `de` or `pl`; without the line the messages are English. Each player can
choose independently; this does not change Gothic 3's own language.

## Joining

The host starts Gothic 3 and begins a new campaign or loads the co-op campaign. Start your game
afterwards. If the host has loaded or saved a campaign, stay in the main menu and wait for the
campaign to transfer and load. If the host starts a completely new game, start a new game too.

If joining is refused, open `g3together.log` in the Gothic 3 folder and find the line containing
`REFUSED by host`. Compare versions and game options with the host. The reason also stays in the top line of the main menu.

**Steam is the only connection path** (see [PLAYING.md](PLAYING.md#over-steam-experimental)). Your game looks for the host
among your Steam friends; until it finds one, the top line of the main menu says so
after about 15 seconds. The host must be on your Steam friends list and have Steam and its game running.
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
- The host saves the campaign.
- See **Known issues** in the [full player guide](PLAYING.md) for problems and workarounds.

## If something goes wrong

Open PowerShell in the extracted package folder, where this file is located, and run the following
command. Python is required:

```powershell
python "tools\collect_logs.py" "<Gothic 3 folder>"
```

Send the resulting `g3together-logs-<timestamp>.zip` to the host. It is created in the package folder.
Without Python, collect `g3together.log` from the Gothic 3 folder and the `.log` files from
`Documents\gothic3`, including its `paru` subfolder, manually.
