# Checklist: first human co-op playtest

## Purpose

A first session for two players, one host and one joining player: joining, combat, companions,
knock-outs, dialogs, quests, trading, looting, separate regions, saving, loading and rejoining.
This checklist also covers recording bugs and collecting logs with `tools\collect_logs.py`.

## Before you start

### Players and installations

- [ ] Assign exactly two roles: host and joining player. The host starts or loads the shared campaign; either player may request a paired save.
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
- [ ] If the host begins a new game, the other player does too. If the host has loaded or saved, a returning player
  joins from the main menu and waits for the campaign transfer. A first-time player without an own character record
  starts a new game or loads their own local hero first; they must never receive the host's character.

## A 90-minute session

| Time | Stage | Steps and observations |
|---|---|---|
| 0–8 min | Start and join | The host loads or starts the test campaign and exactly one other player joins. Check that both see each other, names match, movement is visible and no compatibility rejection occurs. Each player changes armor or helmet once: the other sees the change within a few seconds. When a player starts walking, their legs should move at once on the other screen instead of gliding. Open the map: the partner is marked there with their name; on the compass their mark points their way. |
| 8–18 min | Shared combat | Fight a small encounter near the host. Both attack the same NPC. Check that hits and deaths appear on both PCs and the actual killer earns the experience. Record quest state and counters; one success does not verify every kill-quest case. |
| 18–28 min | Recruit and dismiss a companion | The joining player recruits a suitable NPC. Check that it follows that player, appears on both PCs and is not simulated twice. The same player then dismisses it; both check that it no longer acts as their companion. |
| 28–38 min | Knock-out and help | In a safe setting, let one player be knocked down in a controlled way. Their partner first stands next to them for a few seconds: nothing should happen. Then the partner talks to them and chooses "I will help you.". Check the recovery, notice and consumption of one healing potion. Without a potion, both should receive a notice and the player should remain down. Optionally at the end, before reloading: with both players down, both die and reach game over. |
| 38–50 min | Dialog, quest, trade and loot | Speak to an NPC, trigger a known quest step, trade and loot. Then open "Show me your wares!" on the partner; dismiss the first tutorial with Ok. Using the mouse and native Trade/Swap button, take an item for free, then give one back without closing. The other player takes no action. Check both inventories immediately after synchronization, unchanged gold and preserved quality. Select gold explicitly and give it in each direction without items; check exact balance changes. Repeat with zero gold, close just after a swap, reopen, and check protected gear/quest items. Verify an NPC still charges its normal prices. These native controls require manual verification. |
| 50–70 min | Distance, separate regions and regrouping | Move apart. Beyond about 35 m, check that no distance message appears on either PC (no "Far from host", no "is far away"). While separated, each player fights a different NPC in their region; record whether results agree and whether anything appears to be simulated twice. Regroup and check that no message appears ("Back near the host." is gone) and that the world state matches. Accept a kill quest and kill a target in the joining player's region: the counter in the host's quest log should increase. |
| 70–82 min | Save and load | Make the heroes visibly different (attributes/LP, current HP/MP, gold, inventory/equipment, a learned spell and quick slot, and position). Change a quest/NPC/door, save on the host and wait for paired confirmation. Change both heroes/world again, then request a save on the client and wait for the same confirmation. Load the earlier host slot and let the client load/rejoin the matching checkpoint. Compare each earlier individual hero and the same earlier world, not the later character. Logs on both must show the same world/save IDs and native CRCs, different player IDs, and no host-character fallback. Repeat with a quick save. If the joining player had a companion, it should follow them after loading/rejoining. |
| 82–90 min | Rejoin and bug report | If time remains, test a clean disconnect and rejoin, in the same process or after restarting. Record campaign, hero record, NPC ownership and companion state. If something failed, collect both players' logs as described below and give them privately to the host. |

With only 60 minutes, prioritize joining, combat, helping up, recruiting and dismissing a companion,
and paired saving initiated from both machines. Test separate regions and rejoining in a follow-up session.

### Paired-save failure and migration checks (protocol 72)

Use only backed-up isolated profiles; never the protected user save folder.

1. Rejoin without restarting, then after restarting from the menu. The same account keeps its own character.
2. Load an older G3T1/V3 test checkpoint. Its exact old character record migrates to V4; original files remain.
   The one-time migration has no old pose to restore, so the native spawn position is retained.
3. On a copied test profile only, remove an exact V4 record but retain its campaign/account latest record.
   Require an explicit latest-character recovery notice. A damaged/wrong-account exact record must instead block
   admission until repaired; deleting it must never admit a host clone.
4. Join from the menu with a new account/no character record. Require a start/load-own-hero notice and a closed
   world gate. Start a new game/load that account's local hero, then confirm normal import and own-character restore.
   Reusing the same network slot/display name must not select the previous player's V4 record.
5. Request a save while the partner is disconnected, loading, talking or an exchange is unsettled. Require a refusal,
   cancellation or clearly incomplete result, never a paired-success message. An offline host can save alone.
   Attempt a player exchange while coordination is pending; it must wait/refuse without losing goods.
6. Interrupt a test checkpoint transfer or a native-pair replacement on a copied profile. Missing manifests or CRC/size
   mismatches must be refused before native load. A valid host checkpoint can remain after a peer failure.
7. Continue playing after the most recent checkpoint, then have the host reload that same checkpoint.
   The client must restore its saved hero and world, not retain the later running state. Check quick-load aliases on both copies.
8. Use equipment with attribute and maximum-health/mana bonuses. Compare base values, effective values and current HP/MP/SP
   before and after reload; bonuses must not be inherited from the host or doubled. Temporary-effect expiry remains unverified.
9. Award shared XP immediately after joining, then leave/rejoin and load a checkpoint. Only future shared gains should apply;
   no first reward may be lost, historical rewards replayed, or saved XP/level lifted to the other hero's total.
10. On a copied profile, remove one canonical native file but retain a complete manual-slot alias. Loading that alias must repair
    the canonical bundle and allow the client to join. Clear a temporary publication/disk failure and require recovery without
    touching the unchanged source character record.

The bundle is the native pair **plus** `.g3t` and the account's character record. Loading a transferred native pair
without this mod is not an individual-character restore. The protocol is coordinated, not a simultaneous distributed
engine freeze; capture timing can differ by network latency. Do not claim runtime acceptance from policy tests/builds.

### Targeting after deaths

Use the same encounter/save on both copies and compare the joining player's targeting with the host's native
targeting. Repeat at least ten kills, including several enemies together; keep another living enemy in view.

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

### Optional: 30 more minutes for the features of 0.3.0

| Stage | Steps and observations |
|---|---|
| Host not reachable | Before the session, start the joining player while the host is offline: the top menu line reports that it is searching Steam friends. No address or port is requested. |
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
| Finding the host | The host starts first, then the joining player without an address: within a few seconds it joins (note how long). Steam shows Gothic 3 and player names match their Steam names. The menu shows lobby status and ping. |
| Different networks | Play from two different homes without a VPN or port forwarding: check joining, save transfer and an hour of play. Record latency and stutters. |
| Join game | The joining player's game runs first; then "Join game" on the host in the Steam friends list: it joins. |
| Leaving | The joining player quits: the host sees it gone at once. It starts again and rejoins. |
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
- If the host loads another save during a session, the other player is loaded into it automatically.
- Untested: monsters attacking downed heroes, the actual bed dialog, and the
  mouse controls of the trade window (dragging items, its buttons).
