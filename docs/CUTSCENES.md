# The cutscene mod

A player's own video clips played at the moments of the game they belong to -- for the remaster, the animated scenes of
the PS1 release, ripped from the player's own disc. A MOD (CLAUDE.md "MODS ARE OUTSIDE THE MANDATE"): off by default,
read only by an interactive run, and it never touches the game.

- **On:** F1 > Video > "Cutscenes (video clips)", or `cutscenes=on` under `[video]` in `ct_settings.ini`. The checkbox is
  greyed, with the reason in its tooltip, when there is no clip list or the build has no decoder.
- **The clips:** the folder `CT_CUTSCENE_DIR` names (`cutscenes` in the working directory by default): the list
  `cutscenes.txt` and the videos it names. Never in git (`.gitignore`: `/cutscenes/`, `*.mpg`).
- **The decoder:** pl_mpeg (MIT, github.com/phoboslab/pl_mpeg), one header fetched by `python tools/get_pl_mpeg.py` into
  `third_party/pl_mpeg/` at a pinned commit and refused unless its SHA-256 is the pin (`--check` grades it). Without it the
  build compiles the mod out (`CT_HAVE_PL_MPEG` undefined).
- **The video:** an MPEG program stream with MPEG-1 video and MP2 audio (any size and frame rate MPEG-1 allows; the
  remaster's are 1920x1080 at 30 fps, 48 kHz stereo, made by its `tools/fmv.py` with FFmpeg).

## What happens at a moment

Between two frames -- before the frame's input is taken -- the host (src/field.c) hands src/cutscene.c the location
`$7E:0100`, the story byte `$7F:0000` and any byte a moment names, as the frame before left them; nothing is written. When a
clip's moment arrives the host stops ticking the game, as the F1 overlay does: no frame, no sound, the game's queued sound
dropped. The clip plays over the window, fitted at its own aspect (black around it) and smoothed whatever the game's filter;
its sound goes to the game's device at the player's volume. Any key, button, click or touch skips it. Then the game goes on
from the same frame, the host's clock re-anchored (the pause is not a stall to catch up on) and the button that skipped
kept from the game until it is let go (every button held as the clip ends is: as after the bug reporter's panel). Its
frames, work RAM and battery are those of a run without the clip -- the check `cutscene-session-leaves-the-game-untouched`
grades them frame for frame -- and the game's sound processor is not ticked while a clip plays (no frame runs). The bug reporter's log gets a line for each
clip (a replay stays exact: it runs headless, where no clip plays) and report.json says whether the mod was on.

## The list

    ct-cutscenes 1
    # comment
    <id> <file> power_on
    <id> <file> at <loc> <story> [<addr>[&<mask>]=<byte> ...] [story_change]

All numbers hex. `power_on` is the first frame of a power-on session (`--boot` from cold), before the title.
`at` is the first frame on which `location == loc` and `story == story` and every named byte of `$7E:0000-$7F:FFFF`,
masked, holds its value (at most four; for a moment a scene's own flags mark inside one location and story value, e.g.
`7F0057=03`, or a bit of an event-flag byte, `7F00EE&02=02`). `story_change` makes it only the frame on which the story
byte becomes the value while the location stays: for a moment the scene marks by moving the story on, whose value then
stays (boarding the Epoch: every later visit to the hangar, and a save loaded there, holds the same location and story
and is not the moment). A moment plays ONCE: walking out of the room and back in
does not play it again. It is met again only after the story byte has been 00 -- the title, the attract and a new game,
where every reload of a save passes -- so reloading a save and replaying the scene replays its clip. (Not "below the
moment's value": the byte is not always the story in between -- the time gate's warp holds 40 in it on the cartridge for
~270 frames a trip, which would replay every later clip after any gate.)
Several moments due on one frame play in the list's order. Every moment is followed while the list is loaded, the option
on or off: turning the option on in the middle of a moment plays nothing.

Finding a moment is the cartridge's job, not a guess: play the original (`ct.exe` under the rule) from a save to the
scene and record what its RAM holds when the scene begins (the remaster's docs/fmv_manifest.json keeps each clip's
evidence: the save, the frame, the picture).

## Switches (docs/DEBUG_SURFACE.md)

`CT_CUTSCENE_DIR` the clip folder; `CT_CUTSCENE_SKIP=<n>` (test) a key's press after a clip's n-th picture;
`CT_CUTSCENE_SELFTEST=<frames>` (test) the list's moments over a scripted run of values, no video. A clip's line on stdout:
`cutscene: <id> at frame <n> (<moment>): skipped|played to its end|the window closed (<p> pictures, <s> sound frames)`, or
`...: not played -- <why>`.

## Checks (docs/TESTING.md)

`cutscene-session-leaves-the-game-untouched` and `cutscene-moments-vs-cartridge` (full),
`cutscene-moments-once-and-again` and `cutscene-decoder-is-the-pinned-pl-mpeg` (quick). The session and moments checks make
their own clips (tools/ctlib/mpeg1_clip.py: flat-grey MPEG-1 pictures and silent MP2, no one's video), so they need neither
FFmpeg nor anyone's videos. `cutscene-moments-vs-cartridge` (tools/cutscene_moments.py) meets each of the remaster's
moments from a recipe state on the cartridge and on the engine with the same pad.
