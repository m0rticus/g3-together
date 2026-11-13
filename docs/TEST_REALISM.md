# Realistic Test-Copy Guide

Use the staged tools in the coordinator-supplied update package.
They verify and stage only isolated QA copies. They do not install, launch or kill
Gothic 3.

## Closed-game acceptance

1. Read-only full copy check:

```powershell
python tools\verify_test_copies.py --host-root "C:\Repositories\g3-together\workspace\test-install" --client-root "C:\Repositories\g3-together\workspace\test-client" --steam-root "C:\Program Files (x86)\Steam\steamapps\common\Gothic 3" --deep --strict-shipped-ini --json test-copy-verification.json
```

Strict shipped INI mode expects explicit `connection`, `session_id` and `language`.
It may fail production-like QA defaults even when runtime behavior is unchanged.
Without strict mode, these are `WARN`, not hidden PASS.

2. Stage/dry-run only:

```powershell
python tools\update_test_copies.py --host-root "C:\Repositories\g3-together\workspace\test-install" --client-root "C:\Repositories\g3-together\workspace\test-client" --steam-root "C:\Program Files (x86)\Steam\steamapps\common\Gothic 3" --session-id pair-red --fake-dll <approved-staged-fake.dll> --approved-dll-sha256 <reviewed-sha>
```

3. Coordinator-authorized apply after both copies are closed:

```powershell
python tools\update_test_copies.py --apply --manifest-out update-transaction.json --host-root "C:\Repositories\g3-together\workspace\test-install" --client-root "C:\Repositories\g3-together\workspace\test-client" --steam-root "C:\Program Files (x86)\Steam\steamapps\common\Gothic 3" --session-id pair-red --fake-dll <approved-staged-fake.dll> --approved-dll-sha256 <reviewed-sha>
python tools\verify_test_copies.py --host-root "C:\Repositories\g3-together\workspace\test-install" --client-root "C:\Repositories\g3-together\workspace\test-client" --steam-root "C:\Program Files (x86)\Steam\steamapps\common\Gothic 3" --deep --json post-update-verification.json
python tools\update_test_copies.py --rollback update-transaction.json --host-root "C:\Repositories\g3-together\workspace\test-install" --client-root "C:\Repositories\g3-together\workspace\test-client" --steam-root "C:\Program Files (x86)\Steam\steamapps\common\Gothic 3"
python tools\update_test_copies.py --apply --rollback update-transaction.json --host-root "C:\Repositories\g3-together\workspace\test-install" --client-root "C:\Repositories\g3-together\workspace\test-client" --steam-root "C:\Program Files (x86)\Steam\steamapps\common\Gothic 3"
```

## Profile planning

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File <package>\launch-copies-invite.ps1 -Profile ideal -AcceptVia CommandLine -Session pair-red -RunDirectory C:\approved-run -PrintOnly
powershell -NoProfile -ExecutionPolicy Bypass -File <package>\launch-copies-invite.ps1 -Profile steam-typical -AcceptVia Callback -Session pair-red -RunDirectory C:\approved-run -PrintOnly
powershell -NoProfile -ExecutionPolicy Bypass -File <package>\launch-copies-invite.ps1 -Profile steam-bad -ClientOnly -AcceptVia Callback -Session pair-red -RunDirectory C:\approved-run -HostReceipt C:\approved-run\host-receipt.json -PrintOnly
```

`-PrintOnly` never starts a process. The staged fake-transport profiles are synthetic;
their delay/jitter/loss/throughput values are not measured Steam values.
The launcher does not deploy DLLs, kill processes, alter broad relays or model real
overlay/bootstrap behavior.
For `steam-bad`, the synthetic physical link maximum is 102400 B/s (100 KiB/s).
The fake API's independent readback can retain a 524288-byte requested buffer
and 262144 B/s send-rate API values. The bad profile also declares a 2000 ms
queue delay in a 100 ms active window every 7000 ms, starting at phase 6000 ms.

Use a coordinator-supplied production checkout explicitly; do not assume a
workspace path:

```powershell
python <coordinator-tools>\run_steam_realism.py --source-root <coordinator-source-root> --fake-dll <package>\G3Together\test-realism\fake_steam_api.dll --output-dir C:\approved-run\matrix --scenario all --seconds 60
```

The strict saturation case always offers400000 application B/s and is not
reduced to make it pass. For a separately labelled transport-volume check
using build69's measured212659 compressed/framing B/s steady rate:

```powershell
python <coordinator-tools>\run_steam_realism.py --source-root <build69-source-root> --fake-dll <package>\G3Together\test-realism\fake_steam_api.dll --output-dir C:\approved-run\steady --scenario steady-load --offered-bps 212659 --seconds 60
```

This opaque-volume proxy is not a replay of native NPC serializer packets.
The production convergence test is separate evidence for that measured rate.
The full repository suite needs only the four native `G3T_*_DLL` variables;
optional launcher tests skip with an explicit reason when
`G3_TEST_UPDATE_PACKAGE` is absent. Set that variable and
`G3T_REALISM_FAKE_DLL` to exercise the staged launcher and transport smokes too.

## Evidence table

- **PASS:** exact reviewed hashes or exact six-slot reconstruction plus genuine full SHA.
- **PASS WITH NOTE:** immutable Data/video hardlinks; no write is authorized.
- **WARN:** intentional ge3.ini differences or source-default configuration reliance.
- **BLOCKED:** general genuine-PUP safety, real Steam overlay/bootstrap, NAT/auth,
  crypto, two physical machines, true frame pacing and proprietary queue behavior.
- **NOT TESTED:** runtime native loading and real overlay/dual-account acceptance.

## Known updates

No Data/video update is indicated. The PUP is a NOOP: preserve QA and `g3qa32.dll`.
Do not add Script_G3Together shadow copies or replace isolated saves/templates.
The host/client template is warm-rejoin evidence only, not native compatibility proof.

## Running honesty

The installed mod SHA and fresh exercised-build SHA/time are separate receipt fields.
A disk/build hash proves no runtime acceptance. Manual milestones should record
bandwidth observations and gameplay time without claiming 400 KB/s wire proof.
