# Changelog

Releases of SFVR, the Street Fighter V rollback netplay mod. Each version's
first paragraph is the summary players see in the game (Settings, Updates);
`scripts/release.ps1` reads it, and refuses to publish a version that has no
entry here. Newest first; the format follows [Keep a Changelog](https://keepachangelog.com).

## [Unreleased]

### Fixed

- Non-Latin systems and games: the game's own text (character, costume,
  stage, V-Skill and V-Trigger names) came out as question marks when the game
  runs in Chinese, Japanese or Korean; the panel now shows it, in the fonts of
  Windows' display language first (Chinese systems no longer get Japanese
  glyph shapes). Recordings and match logs are saved when the game folder's
  path has characters outside ASCII (they were not), and the window title
  shows names in any script.
- The panel opened during the title screen read the game's character lists
  before they were complete, and kept them: characters showed as codes
  ("Z34"), costumes as "costume 0" and V-Skills without names (seen by a
  spectator, also in the post-match windows; a player's own lists could
  miss characters too). The lists are now read once the game is past its
  start.
- A spectator could open the game's own Results Menu over a watched
  match's results screen (any press, even the one opening the panel): the
  game now gets no input there, as the players' vote already did for them.

## [0.1.4] - 2026-10-29

Keyboard and custom button layouts now work online, names can use any
script, each character remembers its loadout, and offline modes no longer
take the last online session's stage.

### Changed

- The character list is in alphabetical order.
- Each character keeps its own loadout (costume, color, variant, V-Skill,
  V-Trigger): picking a character brings back what you last played with it,
  also after restarting the game. A character not played before starts from
  its first costume and color, V-Skill I and V-Trigger I.

### Fixed

- Offline modes after a session: until the game was restarted, every stage
  the game asked for (Arcade, Versus, Training, even the menus') was swapped
  for the session's last stage, and after the Debug tab's Battle now offline
  battles also got its Ryu and Ken. The mod's stage and characters now apply
  only to the battle it jumps into.
- The debugging hotkeys F5, F6, F9 and F10 are off: they loaded saved
  states in any battle, offline ones too. `[probe] debug_hotkeys=1` turns
  them on; the old `savestate_hotkeys` setting is no longer read.
- Player names in any script: names are read from the ini files as UTF-8,
  sent whole up to 96 bytes (cut between characters, never inside one), and
  the panel draws them with Windows' fonts for other scripts (accented Latin,
  Greek, Cyrillic, Japanese, Korean, Chinese). Lobby protocol 7.
- Button Settings now apply online: netplay sent the keys before the game
  applies them, so custom layouts and shortcut buttons (3P, 3K, two-button
  shortcuts) came out wrong. Keyboard players, who map keys to pad buttons
  and set both, were hit hardest. Each side now sends its input the way the
  battle reads it offline.
- A spectator's lobby no longer shows "measuring..." under the players: it
  has no link of its own to measure.

## [0.1.3] - 2026-10-29

No more false desync warnings during intros and round calls, and players
whose network blocks the server's usual port can now connect through port 443.

### Fixed

- False desyncs in intros, round calls and win poses: the camera's and the
  pawns' animation clocks follow how ticks fall into rendered frames, which
  differs per machine (more so since 0.1.2 stretches frames). They are still
  restored on rollbacks but no longer part of the checksum. In two matches
  against a tester they caused 300-400 desync events each while the rest of
  the state matched byte for byte.
- A test script's `server` command no longer sticks in the panel's saved
  settings.

### Changed

- When the rooms server does not answer at all, the message says what
  usually causes it (a firewall, antivirus or a network blocking its port).
- The public server is tried on port 443 at the same time as 5555, and the
  first to answer is used: some networks let only common ports through. The
  server needs sfvr-server's `deploy/port-443.sh`.
- The panel's Debug tab has a "Public server" button next to "This PC", to
  get back to the public server after trying a local one.

## [0.1.2] - 2026-10-29

Smoother matches: the game no longer repeats a frame to keep both sides in
step, and rollbacks cost less CPU.

### Changed

- Time sync stretches frames instead of repeating them: the side that runs
  ahead makes its frames a few percent longer (a wait right before the game
  reads the pads, so no input lag) until the other catches up. Only a lead of
  8 frames or more still drops a whole frame. Before, time sync repeated
  about ten frames a minute; between two PCs it now repeats none.
- Cheaper rollback bookkeeping. The state checksum reads the masked words as
  zero in place and fetches the next memory ahead: about 0.35 ms down to
  0.1 ms a frame, with the same checksums. A save whose next frame's inputs
  are already confirmed (the delay covers the latency) keeps its checksum but
  skips the state copy (`[netplay] skip_saves`).

### Added

- Every load is checked against the checksum taken when that frame was saved
  (`[netplay] verify_loads`), and a `costs over 600 advances` log line says
  where the netplay time goes (ticks, replays, hashing, saves, loads). The
  pace log says how much was stretched; the frame drop lines show the wait.

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

