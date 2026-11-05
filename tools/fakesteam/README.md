# Testing the Steam-only transport locally

`src\steam.cpp` normally requires two real Steam accounts. `fake_steam_api.c` is a test-only stand-in whose lobbies,
friends, session requests and messages use files and loopback sockets. It keeps the real 32-bit API layouts.
Neither the fake DLL nor `g3t_relaytest.exe` goes into the player package.

## Focused transport tests

Build with the mod, then run from the repository root:

```powershell
python tools\fakesteam\test_transport.py build\artifacts\fakesteam\Release
```

Without the private Gothic 3 SDK, build only the engine-free tests with MSVC Win32:

```powershell
cmake -S . -B build -G "Visual Studio 18 2026" -A Win32 -DG3T_TRANSPORT_TESTS_ONLY=ON
cmake --build build --config Release
python tools\fakesteam\test_transport.py build\artifacts\fakesteam\Release
```

Use Python 3.12+ to also run the regression covering the QA runner's fake-Steam network impairment path.

The tests launch and clean up their own host/client processes and temporary lobby folders. They check:

- Steam discovery, player-state exchange, a complete 2 MB save transfer, a damage claim and disconnect detection.
- All test-process UDP endpoints bind to localhost.
- A compatible direct ENet hello from outside the linked Steam relays is rejected.
- Missing Steam and a missing Steam DLL fail startup on both host and client, without fallback.
- Menu-state formatting, joined/ready client counts, automatic Steam names and UTF-8-safe name truncation.
- Gothic 3 AppID selection and explicit rejection of unexpected Steam identities.
- Reliable NPC terminal-state and ordered ownership-release delivery.
- A third observer receives only host NPC publications; client NPC snapshots/releases are refused.
- Six concurrent processes with distinct fake Steam identities share **one discovery directory**, forming three
  independent host/client pairs selected only by `session_id`. Each pair exchanges player states tagged with its ID,
  a session-specific 2 MB save (checked by CRC), NPC poses and hit claims; each host admits only its intended client.
- Mismatched/case-different IDs cannot join through search, `steam_lobby`, `+connect_lobby` or accepted invites.
- Matching lobby metadata does not admit relay messages with the wrong session ID or stale Steam lobby number.
- Real INI parsing accepts the legacy default and valid IDs and rejects empty, overlong or invalid IDs without fallback.

`g3t_snapshot_test.exe` checks the production engine-free timeline with jitter/loss, stops, discontinuities, clock changes,
timer wrap, pose-aligned animation/combat metadata, publication ordering and nested damage-perception isolation.
It also checks fixed 60 Hz deadlines, interpolation-only NPC state and visible-contact segment geometry.
It loads neither Gothic 3 nor Steam. The sampler and transport tests pass locally, including host-only NPC
authority and identified hit acknowledgements. Native gameplay is not covered by these standalone tests.

The standalone executable uses `g3t_relaytest host <seconds> [steam_host]` or `client <seconds> [steam_host]`.
Its `direct <loopback-port> <seconds>` mode is a negative test probe, not a player connection path.
`G3T_RELAY_SESSION_ID` selects its test session, and `G3T_RELAY_STEAM_LOBBY` selects an explicit test lobby.
The stand-in's `G3T_FAKESTEAM_TAMPER_SESSION` and `G3T_FAKESTEAM_TAMPER_LOBBY` are envelope-rejection negative controls;
`G3T_FAKESTEAM_TAMPER_DATA_SESSION` and `G3T_FAKESTEAM_TAMPER_DATA_LOBBY` leave control handshakes intact but corrupt data.
These tests do not establish six-real-game or real-Steam acceptance. Real Steam still requires distinct account identities.

## Game bot suites

These still require the configured private QA workspace and Python 3.12 or newer.
The Steam-only runner requires the stand-in:

```powershell
$env:G3_FAKESTEAM = "$PWD\build\artifacts\fakesteam\Release\fake_steam_api.dll"
python qa\together.py --suite stage2
```

Each isolated game copy gets its own Steam ID and each pair a private lobby folder.
`G3_NETSIM=delay_ms,jitter_ms,loss_percent` impairs the fake Steam message transport through a local relay.
It does not open a direct-IP game connection. Fake Steam adds a 9-byte framing header to ENet datagrams.
The session relay adds a further 72-byte header (native Steam lobby number plus the configured session ID), on both
data and control channels; an MTU-sized ENet datagram therefore uses at most 1281 bytes including fake Steam framing.
The old direct-UDP `G3_CHAOS` runner is rejected explicitly rather than reporting results from an unused relay.

Real Steam is not touched. The `steamstatus` QA command reports lobby, linked peers, relay counters and round-trip time.
Actual play between two real Steam accounts remains a separate human test; see `docs\PLAYTEST.md`.

For an explicitly requested real-Steam startup check, `g3t_relaytest lobbyprobe <seconds>` uses the DLL selected by
`G3T_STEAM_DLL`, creates a friends-only lobby and leaves it on completion. It checks startup/lobby state, not peer gameplay.

## NPC authority feasibility probes

`qa\stream_probe.py` runs native engine probes in disposable QA copies. Set `G3_WORKSPACE` to a separately
provisioned isolated workspace and `G3_FAKESTEAM` as above; do not overwrite a deployment reserved for manual testing.

```powershell
python qa\stream_probe.py --suite streamProbe --seconds 240
python qa\stream_probe.py --suite passiveProbe --seconds 240
```

`streamProbe` loads a distant region through a non-rendered prefetch camera and checks processed NPCs, navigation,
collision floors, and the unchanged real camera. `passiveProbe` copies native animation state between two
NPCs while blocking the recipient's routine, script, movement, and unsolicited animation entry points. It captures
the native playback options rather than constructing the SDK's opaque descriptors, and checks clip retention,
phase, and root-position drift across engine frames.

These remain opt-in QA commands (`streamprobe`, `passiveprobe`). Protocol 71 separately wires host-only authority and
passive replay into the live NPC path. Primitive probe results are not network/gameplay acceptance: real client swings,
counterattacks, dialog audio, companion/guide/arena/summon lifecycles, loading and clean shutdown need separate coverage.
`stageClientFriendlyHits` additionally checks immediate local flinches and suppression of the host's echoed reaction.
`SYNCNPC` reports actual capture ticks, missed 60 Hz deadlines and elapsed capture time.

The final-pose path additionally copies the host's completed affine bone matrices, including hidden blend-out layers
and facial bones. The primitive selects this by default; `G3_EXACT_POSE=0` is the old clip-only diagnostic, not skeletal acceptance.
The portable transport suite now includes a sustained 73-actor burst case and verifies that large pose packets use
unreliable fragmentation, not an implicit reliable transfer. The wrap test moves the pose stream to its alternate channel
before the 16-bit sequence is exhausted. Its steady-state phase starts after the save transfer.
The stand-in keeps tail pointers for its receive queues and bounds each socket pump so a continuous pose stream
cannot starve the game's main thread. The live transport's owned loopback sockets also reserve burst-sized buffers.
