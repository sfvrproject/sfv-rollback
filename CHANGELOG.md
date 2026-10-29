# Changelog

Releases of SFVR, the Street Fighter V rollback netplay mod. Each version's
first paragraph is the summary players see in the game (Settings, Updates);
`scripts/release.ps1` reads it, and refuses to publish a version that has no
entry here. Newest first; the format follows [Keep a Changelog](https://keepachangelog.com).

## [Unreleased]

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

