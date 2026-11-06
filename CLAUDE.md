# CLAUDE.md

Notes for working on this project: what was found out the hard way. The README describes the
pipeline and usage; this file records the facts and pitfalls behind it.

## Goal

A native executable (arm64 macOS first, no Wine or Rosetta; later WebAssembly) of Wing Commander
III's Windows version, `WC3W.EXE` from The Kilrathi Saga, as GOG's 30th Anniversary Edition (2026)
has it, made by statically recompiling it to C++. The user doesn't care about naming or
understanding the code, only about running it.

Split off from wc3-recomp (`../wc3-recomp`, the DOS version, which is fully playable): its
CLAUDE.md has the background on the generator (p-code to C++, the shadow return stack, x87,
patched constants) and the SDL host. That project must not be affected by this one: nothing here
is shared at build time; carry fixes across by hand.

Never commit game files or anything derived from their bytes: `WC3W.EXE`, the DLLs, the `.TRE` and
`.IFF` files, the movies, the mapped images (`out/*/*.bin`), Ghidra projects, screenshots. Everything
under `out/` is generated and ignored.

## The game files

- GOG's installer (`setup_..._(95013).exe` + 4 `.bin` parts, 12 GB) is Inno Setup: `innoextract
  --gog` unpacks it on macOS (`-I Kilrathi_Saga/WC3W.EXE` for single files, `-l` lists). It has the
  DOS version too (`DOS/DATA.DAT` etc.): byte-identical to GOG's older release that wc3-recomp uses.
- `Kilrathi_Saga/`: `WC3W.EXE` (the game), `XANLIB.DLL` (Origin's audio streaming), the data as
  loose `.TRE` archives (CD1-CD4 MISS/MOVIE/MUSIC, `GAMEFLOW`, `MISSIONS`, `MOVIES`, `OBJECTS`) and
  `WC3.CON` (which TREs each CD has, as in the DOS version), `DATA/` (loose IFF files the remaster
  added or replaced: `GAMEFLOW/SCEN*.IFF`, `PROFILE/*.IFF` for pilots' in-flight comms).
  `MUSICED.EXE` + `mfc42.dll`: the music editor, not needed.
- `wc3en.dll` + `wc3w_en*.ini`: the **Wing Commander III Enhancement Patch** by Matt Wells (MIT,
  `NOTICE.txt`; open source, the binary's paths name `Wing_Commander_III_Enhancement_Patch` on
  GitHub). `WC3W.EXE`'s import of `DirectDrawCreate` is renamed from `DDRAW.dll` to `WC3EN.dll`,
  the patch's only way in: it exports `DirectDrawCreate` (its export name is "ddraw.dll"), presents
  the game's 8-bit frames through Direct3D 11 (DXGI) and patches the game's code in memory
  (`VirtualProtect`; `Mod_Display`, `Mod_Movies`, `Mod_Music`, `Mod_Controller`, `Mod_Keyboard`,
  `Mod_Mouse`, `Mod_ObjectRotation`, `Mod_GeneralFixes`). What it adds: HD movies (MP4, libvlc,
  `Master_FMVs_Upscaled/` or `_Source/` with `movies.ini` mapping the game's names and branches),
  subtitles (`.srt` en/de/fr), music through libvlc (`wc3w_en_alt_music.ini`), flight at up to 60 fps
  (originally 24), widescreen space views, controllers through WinRT `Windows.Gaming.Input`, mouse
  remapping, saves under `%APPDATA%\wc3w_en`. Built with MSVC 2022 for Windows 10: not something to
  recompile; its features are to be ported (as replacements of the functions it hooks, which keep
  their addresses in a recompilation) once the plain game runs.
- `libvlc*.dll`, `plugins/`: VLC 3 for the patch, not needed (FFmpeg or macOS's own decoders later).

## The modules (src/pe.ts)

- `WC3W.EXE`: PE32, i386, linked 19 Sep 1996 with MSVC 4.2 (linker 4.20), image base 400000h,
  entry 492590h, SizeOfImage C5000h; `.text` 401000h (99D52h bytes), `.rdata` 49B000h, `.data`
  49F000h, `.idata` 4B5000h, `.rsrc` 4B6000h, `.reloc` (19832 relocations: it could be moved, but
  needn't be). Stack reserve 1 MB. The C library is linked in statically (MSVC's: `RtlUnwind`,
  `RaiseException`, `UnhandledExceptionFilter` mean structured exception handling through
  `FS:[0]`, which the runtime will have to provide).
- `XANLIB.DLL`: linked 30 Jul 1996, its own base 90000000h; mapped at 200000h here (it has 250
  relocations), below the program, so that all code and data stay in the low addresses (the DOS
  runtime's `M[]` is 64 MB). Exports C++ classes (`WAVEPLAYER`, `XDMOVIE`, `__STREAMHANDLER`,
  `__WAVESTREAMHANDLER`, `__XANSTREAMHANDLER`) and a `DriverProc`; plays through `waveOut`.
- `npm run imports` lists what both import: 138 functions from Windows (KERNEL32 69 + 15, USER32 30
  + 1, GDI32 8, WINMM 2 joystick + 12 waveOut) and DirectDrawCreate. Loaded at run time with
  `LoadLibrary`/`GetProcAddress`: `dsound.dll` (`DirectSoundCreate`), `user32.dll` (the C library's
  `MessageBoxA`, `GetActiveWindow`, `GetLastActivePopup` for its error box). DirectDraw and
  DirectSound are COM: interfaces with vtables the runtime fills with its own functions. The
  string `MCGA.DLL` is a leftover of the DOS code.
- Ghidra: each module's mapped image (`out/wc3w/wc3w.bin`, `out/xanlib/xanlib.bin`) imported with
  the BinaryLoader at its base, `x86:LE:32:default`, `windows` cspec, so the generator sees one
  memory block as in wc3-recomp. `WcPeSetup.java` names the import address table's slots
  (`__imp_KERNEL32_Sleep`; calls go `CALL [slot]`), makes the entry point and exports functions.
  Analysis takes minutes, not hours (the DOS image took long). It found 2618 functions in WC3W.EXE
  (486 KB of the 630 KB `.text`: the rest is reached through pointers, as in wc3-recomp) and 157 in
  XanLib; 131 of WC3W.EXE's 132 import slots are referenced (297 places), all of XanLib's.

## Plan

1. Analysis in Ghidra (`npm run decompile`): done.
2. The generator's Windows mode: a call through an import slot becomes a call of the runtime's
   implementation (the runtime fills the slots with addresses it recognises); no segments but FS
   (the thread information block, for SEH); the rest as in wc3-recomp.
3. The runtime: CPU core and SDL host from wc3-recomp; a PE "loader" (the images are mapped
   already: fill the import slots, set up the process: TIB, stack, heap, command line); the Windows
   API on SDL and the standard library; DirectDraw (an 8-bit 640x480 surface, palette, Lock/Flip)
   and DirectSound on SDL; waveOut for XanLib.
4. The enhancement patch's features, ported.

## Conventions

As in wc3-recomp: TypeScript run directly by Node (≥ 23.6), ESM, `npm run typecheck`; Java Ghidra
scripts in `ghidra_scripts/`; C++23 runtime, tab-indented, short comments; the standard library
rather than POSIX. Ghidra 12.2 (Homebrew; JAVA_HOME from the `ghidraRun` wrapper).
