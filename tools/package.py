"""Build a production player package: the mod DLL, English setup, player guides,
manual copy preflight, hang diagnosis, log collection and licences.

    python tools/package.py <built Script_G3Together.dll> <output folder> [<steam_api.dll>]

Writes <output folder>/G3Together-<git hash>.zip. Contains no game files. With a steam_api.dll (Valve's 32-bit one from the
Steamworks.NET Standalone release 2025.164.1, checked by its SHA-256) it goes in as G3Together/steam_api.dll.
Steam is the only supported transport; without that DLL the setup must download it before installing the mod.
"""
import hashlib
import argparse
import subprocess
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
STEAM_DLL_SHA256 = 'fdafb7c65ec7d4182b42bf92cc8671c68e4dc547cc6e613899d2618a47eb65da'

HOST_INI = """# G3 Together: this player hosts the campaign.
# This package speaks protocol 77 with host-managed saves and guarded bone packing: both players need this same package.
role = host
# Player name is taken from Steam automatically.
# Steam friends find this game. Steam and G3Together/steam_api.dll are required; there is no direct-IP fallback.
transport = steam
# Match this case-sensitive ID on both players; use a different ID for each independent group.
# 1-63 ASCII letters, digits, underscores or hyphens. This is not a password or a Steam lobby number.
session_id = default
# Mod messages only: auto (the language of your Gothic 3 texts), en, de or pl; without this line: en.
# Each player can choose independently.
language = auto
# Key that jumps to the other player (default PAGEDOWN): F1-F24, PAGEUP, HOME, END, INSERT, DELETE or a letter
partner_key = PAGEDOWN
# Player exchanges use the normal trade window: free items and explicit gold gifts, with no approval keys.
# Your own look for the other players (a character template such as Diego; default: their puppet_template)
# look = Diego
# For a bug report about NPCs: remove the # below, restart, reproduce (writes g3trace.log, see PLAYING.md)
# trace = on
# Town reputation: shared (one record per town for both; default), together (shared guilt), personal (each their own)
# reputation = shared
# How far around the host its game runs the world, in cm (default 8000; the game itself uses 4000). Larger keeps the
# NPCs near the other player on this PC longer, so they change hands less often.
# host_roi = 8000
# Suppress only the reviewed world-data Goblin/Ogre instances in Ardea; no kill/loot/XP.
# An empty value disables this host-only rule. Quest creatures and other regions are unaffected.
# ardea_remove_npcs = Goblin,Ogre
# spheres (default): the host runs the world in a sphere of host_roi around each player, so the NPCs near the other player
# stay on this PC (up to the host's view distance). elastic: one sphere around both players instead. off: each PC runs its
# own surroundings (before 0.7.0).
# host_sim = off
# auto (default) shows the other player where they probably are now, not where their last state came from (a little ahead
# along their movement); off as before. Each player decides for their own screen.
# puppet_lead = off
# on (default) shows the other player's and the NPCs' blows from where they are at the sender, not from their start; off as
# before. Each player decides for their own screen.
# motion_seek = off
# While you listen to the other player's dialog you see the spoken lines as subtitles; on also shows their answers in a box.
# listener_textbox = on
# Balance (both players need the same values, the join compares them): friendly_fire = percent of the damage players and
# their companions deal to each other (0-100, default 0). Zero also prevents allied hit reactions and harmful effects.
# npc_damage and npc_hp = factors on the damage NPCs deal and on
# how much NPCs endure (0.1-10, default 1.0; companions excepted).
friendly_fire = 0
# npc_damage = 1.0
# npc_hp = 1.0
# When the host loads a save or starts a campaign, the joining player is brought back to the menu and enters your world
# again automatically (its own saved character; a fresh hero only if it has none). This is each client's own client_autoload
# choice in its INI (default on); the host's INI ignores it.
"""
CLIENT_INI = """# G3 Together: this player joins the host.
# This package speaks protocol 77 with host-managed saves and guarded bone packing: both players need this same package.
role = client
# Finds the host among Steam friends; Steam is required. With several hosts: steam_host = <SteamID64 or Steam name>.
# Saved characters, personal state and companion ownership use SteamID64; display names may change.
transport = steam
# Must match the host; different IDs never connect, even through invites or steam_lobby.
session_id = default
# Player name is taken from Steam automatically.
# Mod messages only: auto (the language of your Gothic 3 texts), en, de or pl; without this line: en.
# Each player can choose independently.
language = auto
partner_key = PAGEDOWN
# Player exchanges are free and automatically confirmed.
# Your own look for the other players (a character template such as Diego; default: their puppet_template)
# look = Diego
# For a bug report about NPCs: remove the # below, restart, reproduce (writes g3trace.log, see PLAYING.md)
# trace = on
# auto (default) shows the other player where they probably are now, not where their last state came from (a little ahead
# along their movement); off as before. Each player decides for their own screen.
# puppet_lead = off
# on (default) shows the other player's and the NPCs' blows from where they are at the sender, not from their start; off as
# before. Each player decides for their own screen.
# motion_seek = off
# While you listen to the other player's dialog you see the spoken lines as subtitles; on also shows their answers in a box.
# listener_textbox = on
# Balance (both players need the same values, the join compares them): friendly_fire = percent of the damage players and
# their companions deal to each other (0-100, default 0). Zero also prevents allied hit reactions and harmful effects.
# npc_damage and npc_hp = factors on the damage NPCs deal and on
# how much NPCs endure (0.1-10, default 1.0; companions excepted).
friendly_fire = 0
# npc_damage = 1.0
# npc_hp = 1.0
# on (default): when the host loads a save or starts a campaign you return to the menu and, after a 4 s countdown, enter the
# host's world again by yourself with your own account character (a fresh hero only if you have none, never the host's hero).
# Esc during the countdown (or choosing New Game yourself) keeps you in the menu until the host loads again; off: always
# choose New Game yourself.
# client_autoload = off
"""


def main():
    parser = argparse.ArgumentParser(
        description="Build a production and manual-support G3 Together package."
    )
    parser.add_argument("dll", type=Path, help="built Script_G3Together.dll")
    parser.add_argument("output", type=Path, help="directory for the ZIP package")
    parser.add_argument("steam_dll", nargs="?", type=Path, help="optional tested Valve 32-bit steam_api.dll")
    args = parser.parse_args()

    dll, out, steam_dll = args.dll, args.output, args.steam_dll
    if steam_dll and hashlib.sha256(steam_dll.read_bytes()).hexdigest() != STEAM_DLL_SHA256:
        raise SystemExit(f'{steam_dll} is not the tested steam_api.dll (SHA-256 {STEAM_DLL_SHA256})')
    out.mkdir(parents=True, exist_ok=True)
    version = subprocess.run(['git', '-C', str(ROOT), 'rev-parse', '--short', 'HEAD'], capture_output=True, text=True).stdout.strip() or 'dev'
    target = out / f'G3Together-{version}.zip'
    with zipfile.ZipFile(target, 'w', zipfile.ZIP_DEFLATED) as z:
        z.write(dll, 'scripts/Script_G3Together.dll')
        z.writestr('g3together.ini.host-example', HOST_INI)
        z.writestr('g3together.ini.client-example', CLIENT_INI)
        # Explicit English public guides only.
        for guide in ('PLAYING.md', 'JOINING.md', 'PLAYTEST.md'):
            z.write(ROOT / 'docs' / guide, guide)
        for support in (
            'tools/collect_logs.py',
            'tools/copy_preflight.py',
            'tools/hangstack.py',
            'tools/setup.ps1',
            'tools/startup_videos.py',
        ):
            z.write(ROOT / support, support)
        z.write(ROOT / 'Setup.cmd', 'Setup.cmd')
        z.write(ROOT / 'COPYING', 'COPYING')
        z.write(ROOT / 'COPYING.STEAMWORKS', 'COPYING.STEAMWORKS')  # GPLv3 section 7: the Steamworks API library
        enet = ROOT / 'thirdparty/enet/LICENSE'
        if enet.exists():
            z.write(enet, 'licenses/ENet-LICENSE')
        z.write(ROOT / 'thirdparty/licenses/jericho-coop-LICENSE', 'licenses/JerichoCoop-LICENSE')  # steam.cpp is adapted from it
        if steam_dll:
            z.write(steam_dll, 'G3Together/steam_api.dll')
    digest = hashlib.sha256(target.read_bytes()).hexdigest()
    print(f'{target} sha256={digest} dll_sha256={hashlib.sha256(dll.read_bytes()).hexdigest()}')


if __name__ == '__main__':
    main()
