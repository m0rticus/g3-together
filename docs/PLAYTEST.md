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
- [ ] The client starts New Game locally on initial entry and after every host load. The host sends only
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
   downed episode; the revived player becomes an ordinary eligible enemy again. NPCs must not remain
   artificially friendly or require a reload to re-engage.
4. Knock both down and verify immediate `all-down` game over with one native Kill per life. Repeat the
   unavailable-helper `alone-grace` check above. Smoke-check friendly melee protection, one hit per swing,
   partner trade and host save/load with client re-entry.
5. In an isolated encounter, knock out one player while the helper stays alive but beyond enemy perception
   range. With no other enemy near, the attacker should relax and stow its weapon the vanilla way, not
   acquire the downed player again. Return and revive through talk, then confirm ordinary re-engagement.

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
Repeated acquisition/hit refusals are limited to the first 40 records and one sample per five seconds;
actual binding/life transitions are logged independently. No QA command or control is needed.
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

### Host-managed save/load acceptance (protocol 77)

Before checking saves, require `persist: save and load hooked` on both copies. It reports the five
verified save/load entry points; `save UI hooked` also identifies the verified quicksave handler,
menu-new call, native availability predicate and screenshot helper. World.DeleteSaveGame at RVA 19C0D0
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
   The client chooses **New Game locally again**, waits for account-exact character/personal-state restoration
   and the host calendar plus all story/runtime/nearby shared-door baselines, applied cutover and entry confirmation.
   Compare both distinct saved heroes and shared quest/NPC/door/loot state. Recheck the first shared XP reward.
   Check quest counters/timers and no repeated quest XP, gold, reputation or item rewards. Loot a chest/corpse
   and loose item, buy/sell with a merchant, save, reload and revisit after leaving the region: no rerolled or
   duplicated stock. Shared loaded doors must match even if the host has not changed them since loading.
   Test a saved client position far from the host, and change a quest/door while that client is joining:
   no missing baseline, stale state, skipped motion mapping or reward-producing replay. Calendar/activation dates must agree.
   Clients receive no native serialized world; arbitrary dynamically created loose drops and temporary effects
   remain an acceptance boundary. Any mismatch blocks campaign acceptance.
5. Disconnect/rejoin in the same live epoch: keep the live client character. Restart/rejoin after a host load:
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
| Latency breakdown | With the coordinator's two isolated copies and no artificial relay, confirm the client's menu says transport RTT and normally reads low single digits, not 15/16/31/32 ms clock steps or a fixed 110 ms. Compare `STEAMSTATUS ping` with the separate ENet RTT after at least 60 seconds in-world. Transport should keep responding while one game loads; ENet may retain frame/ACK delay. Check worker poll gaps, socket/send failures, batch/service caps and send-to-flush maxima. Do not interpret smoothed ENet loss alone as continuing physical loss. |
| Burst/fragment queues | Record `queued_commands`, `reliable_queued`, `reliable_inflight` and `reliable_bytes` in the same quiet and NPC-heavy windows. Two-record batches remain 1199 bytes and still fragment; best-effort near-MTU snapshots must no longer introduce reliable ACK dependencies. Large full poses were already unreliable-fragmented, so high traffic can remain. Queue snapshots can miss short bursts; do not claim throughput or gameplay acceptance from the queue-only ENet tests. |
| Buffered presentation | On a clean 60 Hz link, `SYNCLATENCY buffer_avg_ms` should settle near 34 ms; actual `shown_lag` should settle without rewinds. Jitter/slower captures may increase the buffer up to 250 ms. Check NPC running/turning/stopping, melee/parries/hit reactions, targeting after kills, ragdolls, downed/revive, trade and host save/load with client re-entry. Policy tests and builds do not establish native visual/gameplay acceptance. |
| Different networks | Play from two different homes without a VPN or port forwarding: check joining, character-record save requests and an hour of play. Record latency and stutters. |
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
