# wc3w-recomp

Statically recompiles the Windows version of Wing Commander III: Heart of the Tiger (`WC3W.EXE`
from The Kilrathi Saga, 1996, as GOG's 30th Anniversary Edition brings it) to C++ with Ghidra,
driven from TypeScript, to build a native program (arm64 macOS first; no Wine, no Rosetta).

It is a sibling of [wc3-recomp](../wc3-recomp), which does the same for the DOS version; the
generator (`ghidra_scripts/WcRecomp.java`) and the Ghidra driver (`src/ghidra.ts`) started as copies
of its own, and the runtime will take its CPU core and SDL host. The two projects share nothing at
build time: a change here never affects the DOS one.

State: the modules load, their imports are listed and Ghidra analyzes them; next are the
generator's Windows mode and a runtime with the Windows API the game uses (see `CLAUDE.md`).

## Building it yourself

This repository contains no game files and nothing derived from them: you need the 30th
Anniversary Edition from GOG. Its installer is for Windows, but `innoextract` unpacks it anywhere;
only the `Kilrathi_Saga` folder is needed (the HD movie folders `Master_FMVs_*` not yet):

```bash
brew install node ghidra innoextract
innoextract --gog -d ../game3.30 path/to/setup_wing_commander_iii_heart_of_the_tigertm_30th_anniversary_edition_v1.0_\(95013\).exe
npm ci
```

Everything is worked out on its `WC3W.EXE` (SHA-256
`1fffd1840356ea09b4e475c6648eda98e9485e1e421336dc927a25db274cd91f`) and `XANLIB.DLL`
(`5b7f494a7f78d08739892e489b6f29b488b1939d034a0f074beafceb01290c4f`); other files are refused.

With the game at `../game3.30/Kilrathi_Saga` (or `-- --game path` for every step):

```bash
npm run imports      # map the modules (out/wc3w, out/xanlib) and list the Windows functions they import
npm run decompile    # import both into a Ghidra project (out/ghidra) and analyze them (long)
```

## License

BSD 2-Clause (`LICENSE`), for this repository's own code.
