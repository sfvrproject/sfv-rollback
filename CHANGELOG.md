# Changelog

Releases of SFVR, the Street Fighter V rollback netplay mod. Each version's
first paragraph is the summary players see in the game (Settings, Updates);
`scripts/release.ps1` reads it, and refuses to publish a version that has no
entry here. Newest first; the format follows [Keep a Changelog](https://keepachangelog.com).

## [Unreleased]

### Changed

- Smoother time sync: the side that runs ahead stretches its frames by a
  few percent (paced to an exact 60 Hz before the game reads the pads)
  instead of repeating a whole frame; only a lead of 8 frames or more still
  drops one. The pace log says how much was stretched.
- Cheaper rollback bookkeeping: the state checksum's mask is applied word by
  word only where it is set (same checksums), and a save whose next frame's
  inputs are already confirmed (the delay covers the latency) skips its state
  copy (`[netplay] skip_saves`).

### Added

- Every load is checked against the checksum taken when that frame was saved
  (`[netplay] verify_loads`), and a `costs over 600 advances` log line says
  where the netplay time goes (ticks, replays, hashing, saves, loads).

## [0.1.1] - 2026-10-29

Fixes from the first test sessions: players with the same version always
connect now, the lobby no longer hangs after a split rematch vote, and the
win pose plays out while players choose what's next.

### Fixed

- The noise mask is built into the mod, so every copy of a version checks the
  game state the same way. Under 0.1.0 a copy without `sfv_noise.txt` could not
  join a copy with it ("the host runs another build of SFVR 0.1.0").
- When builds differ, the message says which part: the game's version, or
  SFVR's state checks.
- The lobby over a match's results screen no longer waits on character
  preloads that can only run at the main menu: changing a pick there showed
  both players "READY (loading)" and held the match back.
- The win pose keeps playing behind the post-match menu and the lobby, instead
  of freezing when the game's own results menu would open.

### Changed

- Ready locks a player's pick (and the host's stage) in until the match starts;
  Leave is the way out. Backing out while the host started the match left both
  sides waiting.
- A desync shows as one small line along the bottom of the screen instead of a
  banner across the top.
- `sfv_noise.txt` is no longer part of a release (a 0.1.0 copy left behind is
  ignored); `[mod] noise_file` uses a file instead of the built-in mask, for
  learning new words.

## [0.1.0] - 2026-10-29

The first test build: rollback netplay for Versus matches through match codes,
with spectators, rematches, match recordings and updates from the project site.

### Added

- Rollback netplay (GekkoNet) for Versus matches, built on the game's own
  rollback state: host a match and send the 6-character code, or join with
  one. The rooms server is the public sfvr-server (`159.69.32.141:5555`).
- Peer-to-peer links, through Cloudflare's TURN relay when two players can't
  link directly; the other player's bar shows a cloud when relayed.
- A lobby with both players' picks (character, costume, color, variant,
  V-Skill, V-Trigger), the host's stage, ready checks, and both characters
  preloaded before the jump into battle.
- After each match: rematch, character select or leave, with the session's
  score. Up to 8 spectators per session.
- An in-game panel styled after the game's menus (F1, or Back + Start): host,
  join with an on-screen keypad, settings, and player bars over the health
  bars with ping, the other player's frame rate and the longest rollback.
- Input delay suggested from the ping (1 to 3 frames) and set per player.
- An offline boot straight to the main menu, without the game's server login.
- Desync detection on every frame, with a banner when it happens.
- Match recordings (`.sfvr`, compact: about 270 KB a match) and a log for
  every match, in the `SFVR` folder next to the game; **Pack logs to share**
  zips them to send with a report.
- Updates from the project site (https://sfvr.pages.dev), offered in the
  game and installed only when the player agrees; both players need the same
  version to play each other.

### Known issues

- Vsync can't be turned off from the game yet.
- Round intros, round calls and KOs run in lockstep (no rollback), so they
  can stretch a little on long-distance connections.

