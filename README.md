# wc3w-recomp

Statically recompiles the Windows version of Wing Commander III: Heart of the Tiger (`WC3W.EXE`
from The Kilrathi Saga, 1996, as GOG's 30th Anniversary Edition brings it) to C++ with Ghidra,
driven from TypeScript, to build a native program (arm64 macOS first; no Wine, no Rosetta).

It is a sibling of [wc3-recomp](../wc3-recomp), which does the same for the DOS version; the
generator (`ghidra_scripts/WcRecomp.java`) and the Ghidra driver (`src/ghidra.ts`) started as copies
of its own, and the runtime will take its CPU core and SDL host. The two projects share nothing at
build time: a change here never affects the DOS one.

State: the game starts in a window, plays its intro as the 30th Anniversary Edition's HD movies
(1080p, decoded with FFmpeg; the original 320x240 ones with `WC_HD_MOVIES=0`) and reaches the
hangar (with its music) with the keyboard and mouse. Further than that is untested; see `CLAUDE.md` for what's
missing.

## Building it yourself

This repository contains no game files and nothing derived from them: you need the 30th
Anniversary Edition from GOG. Its installer is for Windows, but `innoextract` unpacks it anywhere;
only the `Kilrathi_Saga` folder is needed, with `Master_FMVs_Upscaled` for the HD movies (4.4 GB;
`Master_FMVs_Source` is the other set, not needed):

```bash
brew install node ghidra innoextract cmake ninja sdl3 ffmpeg
innoextract --gog -d ../game3.30 path/to/setup_wing_commander_iii_heart_of_the_tigertm_30th_anniversary_edition_v1.0_\(95013\).exe
npm ci
```

Everything is worked out on its `WC3W.EXE` (SHA-256
`1fffd1840356ea09b4e475c6648eda98e9485e1e421336dc927a25db274cd91f`) and `XANLIB.DLL`
(`5b7f494a7f78d08739892e489b6f29b488b1939d034a0f074beafceb01290c4f`); other files are refused.

With the game at `../game3.30/Kilrathi_Saga` (or `-- --game path` for every step):

```bash
npm run imports      # map the modules, link them into one image and list the Windows functions they import
npm run decompile    # import the image into a Ghidra project (out/ghidra) and analyze it (minutes)
npm run recomp       # translate it to C++ (out/recomp) and build out/bin/wc3w
npm run smoke        # run it headless from a new game to the first mission in space, in a fresh copy of the game folder
npm test             # unit tests of the archive and PE readers and the CPU helpers (no game files needed)
```

Then play, with a copy of the game folder (the game writes its saves into it):

```bash
out/bin/wc3w path/to/Kilrathi_Saga
```

Cmd+F toggles full screen, Cmd+Q quits. The mouse is captured while the window has it.

The player's kills and aces, the ship's and the wingmen's morale (0-15, which the game never shows)
and the wingmen's kills and aces, from the saved games (`*.WSG` in the game folder, or the files
given):

```bash
npm run print-save
```

The flight missions with their objectives, units and scripts (what a mission counts as success),
from `MISSIONS.TRE`; any archive's entries unpacked:

```bash
npm run print-mission                # the list
npm run print-mission -- MISNLG04    # one of them
npm run tre -- path/to/MISSIONS.TRE out/tre
```

## License

BSD 2-Clause (`LICENSE`), for this repository's own code.
