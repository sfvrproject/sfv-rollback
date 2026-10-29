# SFVR (prototype)

SFVR is rollback netplay for Street Fighter V, built on the game's **local Versus** mode
and its own rollback manager, using [GekkoNet](https://github.com/HeatXD/GekkoNet)
over [Weyvelength](https://github.com/HeatXD/Weyvelength) rooms and
peer-to-peer links. The game's online code (ProudNet, Capcom servers,
CFN) is never touched.


## Repositories

This is one of three, kept side by side in the game's folder:

| Repository | What it holds |
| --- | --- |
| `sfv-rollback` (this one) | the mod, the loader (`xinput1_3.dll`), the core library and its tests, the `sfvr` and `weyve_probe` tools, the test scripts (`scripts/`), a Windows build of the rooms server for local tests |
| `sfvr-server` | the public rooms server: the Linux install (`deploy/`, systemd, Cloudflare TURN) and its guide |
| `sfvr-site` | the project website: the download page and the update info file the updater reads |

The Weyvelength commit is pinned in two places that must match: `GIT_TAG`
in `CMakeLists.txt` here and `COMMIT` in sfvr-server's `deploy/install.sh`.

## Status

| Part | State |
| --- | --- |
| Netplay driver (`NetplaySession`): GekkoNet session, save/load by region copy, recording | done; runs in game between two instances |
| Transport: Weyvelength rooms (codes `XXXX-XXXX`) with P2P links; plain UDP (`U-` codes) for testing | done; tested between instances on one PC through a local server; two machines and TURN not yet |
| Sessions: lobby (picks shown to the other, ready, both characters preloaded), post-match vote over the win screens (Rematch, Character select, Leave; the game's own results menu held back), rematch, character select, spectators (up to 8, confirmed inputs streamed by the host, checksums compared) | done (Direct Match), with the session score |
| Match HUD (both players, ping and jitter, longest rollback, the other's frame rate and link type), desync banner, toasts, per-player input delay with a suggestion from the ping, settings locked in a session, on-screen code keypad, hot reload refused during a session, Bye when the window closes | done; tested on one PC |
| Offline boot: no server login, straight to the main menu | done (`[boot] offline=1`), about 25 s from launch |
| In-game overlay panel (Dear ImGui over Direct3D 11; F1 or Back + Start) | host, join, match code, picks (with character names), stage, cancel, leave, live match numbers, a status strip, gamepad navigation, choices saved per instance, debug tab |
| Jump straight into a battle (no menus) with the host's setup: character, costume, colour, comment, V-Skill, V-Trigger, stage | done (`battlenow`; netplay uses it) from the main menu (about 8 s, no character select scene), stage select, a running battle or its results (about 5 s); back to the main menu when a session ends |
| `.sfvr` recordings (format 3): inputs, a checksum per frame, per-region hashes around desyncs (compact, the default: about 270 KB a match) or for every frame (full), events, compressed state dumps (start, desync, every 600 frames), CRC | done |
| Deterministic playback (`VerifyReplay`), `sfvr diff` / `sfvr peers` for cross-machine desyncs | done |
| Shared noise mask (`data/sfv_noise.txt`): junk, pointer and engine-clock words left out of checksums | learned for every character at frame 0, and across V-Skill / V-Trigger I and II while fighting (`scripts/scan-loadouts.ps1`) |
| In-game rollback stress test (one process) | passes |
| Two-machine sync | in sync on one PC with both players mashing under latency and loss, through KOs and into round 2: lockstep outside the fight, the KO stop bit matched to the round director after loads; not yet tried across machines |

See the testing notes for how to run two
instances, the test scripts, and the desync findings so far.

## Layout

```
src/core/      game-agnostic: IGame, netplay session, GekkoNet bridge,
               .sfvr format, verifier, diff
src/testbed/   MockGame (deterministic fighter with switchable bugs),
               loopback network with latency/jitter/loss, match runner
src/tools/     sfvr command-line tool
src/mod/       sfv_rollback.dll: offsets, probes, SfvGame, Weyvelength and UDP transports
src/proxy/     xinput1_3.dll (and version.dll) proxy that loads sfv_rollback.dll
tests/         end-to-end tests
scripts/       PowerShell helpers to drive two game instances and soak-test netplay
data/          sfv_noise.txt, the shared checksum mask (copy next to the DLL)
```

## Build

Needs Visual Studio 2022 Build Tools (C++ x64, CMake, Ninja) and network
access on first configure (GekkoNet and MinHook are fetched at pinned commits).

```bat
build.cmd
build\Release\sfvtests.exe
```

## Try the desync tooling

```bat
build\Release\sfvr.exe demo demo
build\Release\sfvr.exe diff demo\machine-value_p1.sfvr demo\machine-value_p2.sfvr
build\Release\sfvr.exe verify demo\machine-value_p2.sfvr --mock-bug machine-value
build\Release\sfvr.exe verify demo\outside-snapshot_p1.sfvr --mock-bug outside-snapshot --stress 7
```

## Run the phase 1 probes in game

1. Copy `build\Release\xinput1_3.dll` and `build\Release\sfv_rollback.dll` to
   `StreetFighterV\Binaries\Win64\` (next to the 64 MB exe, not the launcher).
   `version.dll` is not used on Windows 11.
2. Copy `sfv_rollback.ini.example` there as `sfv_rollback.ini` (default
   `mode=probe`).
3. Start the game, play a local Versus match, press F5 and later F9.
4. Read `sfv_rollback.log` in the same folder.

The DLL checks the exe timestamp and 16-byte signatures at every address it
uses and does nothing on a mismatch. Delete the two DLLs to uninstall.
