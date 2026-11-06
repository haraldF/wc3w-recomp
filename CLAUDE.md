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
- `XANLIB.DLL`: linked 30 Jul 1996, its own base 90000000h; moved to 200000h (it has 250
  relocations), below the program. Exports C++ classes (`WAVEPLAYER`, `XDMOVIE`, `__STREAMHANDLER`,
  `__WAVESTREAMHANDLER`, `__XANSTREAMHANDLER`) and a `DriverProc`; plays through `waveOut`.
- `npm run imports` lists what both import: 138 functions from Windows (KERNEL32 69 + 15, USER32 30
  + 1, GDI32 8, WINMM 2 joystick + 12 waveOut) and DirectDrawCreate. Loaded at run time with
  `LoadLibrary`/`GetProcAddress`: `dsound.dll` (`DirectSoundCreate`), `user32.dll` (the C library's
  `MessageBoxA`, `GetActiveWindow`, `GetLastActivePopup` for its error box). The string
  `MCGA.DLL` is a leftover of the DOS code.

## One image (src/cli.ts unpack)

- Both modules are mapped and linked into one image, `out/image.bin`, from 200000h to 4C5000h
  (`program.json` has the modules), imported into Ghidra as one program (BinaryLoader,
  `x86:LE:32:default`, `windows` cspec), so the generator sees one memory block as in wc3-recomp.
  WC3W.EXE's import slots for XanLib hold XanLib's export addresses (calls go straight to its
  recompiled code); every slot for a Windows function holds a thunk address, F0000000h + 16 * its
  index in `wc_imports` (generated `wc_program.cpp`), which `run()` hands to `win_thunk()`.
  `WcPeSetup.java` names the slots (`__imp_KERNEL32_Sleep`) and makes the entry points and exports
  functions. Analysis takes minutes (the DOS image took long).
- Code reached through pointers: the base relocations name every absolute address in the image;
  those pointing into code sections (`out/code-pointers.txt`, 2754) that no Ghidra function holds
  (1423) are decoded as fragments by the generator (1342 of them, 36000 instructions). This found
  what the DOS version needed `discover` rounds for: the game ran to the hangar without one.

## The generator (ghidra_scripts/WcRecomp.java)

- wc3-recomp's, without its drivers loaded at run time. Segment bases are all 0 but FS (the running
  thread's information block); the DOS fixes (LDS, far calls, LSL) stay but don't apply.
- x87: `F2XM1`, `FSCALE` and `FYL2X` (Ghidra's CALLOTHERs `f2xm1`, `fscale`, `log2`) for the C
  library's `pow`/`exp`/`log`, which the DOS version never used. 80-bit memory operands (`FLD m80`,
  `FSTP m80`: the C library's constants) are converted to and from the doubles the ST registers
  carry (`x87_ld80`, `x87_st80`), both as LOADs and as address varnodes: read as 8 bytes, `pow`
  compared against a NaN and returned 0, so the gamma table (41DB50h) was all 0 and every palette
  black.

## The runtime (runtime/)

- `cpu.cpp`, `wc_rt.h`: wc3-recomp's CPU core without real mode, PC devices and drivers. `M[]` is
  128 MB. `TRACE_INS` calls `cpu_poll` every 65536 instructions (thread switches).
- `win.cpp`: the process. Memory: the main thread's information block at 10000h (FS), the PEB at
  11000h, command line and environment at 12000h, its stack F0000h-1F0000h, the image, then
  VirtualAlloc's regions from 500000h (64 KB granularity, zeroed) and a small heap from them
  (GlobalAlloc, LocalAlloc, the runtime's COM objects). The x87 control word starts at 027Fh as
  Windows sets it: left 0 (all exceptions unmasked), the C library's `pow` raised C000008Fh.
  `win_thunk` calls an implementation (`API(dll, name, nargs)` in each file), sets EAX and pops the
  return address and the arguments (stdcall); one that isn't there stops the program (exit 6:
  "unimplemented Windows function"). `win_call` calls the program (window procedures, threads,
  callbacks) with RETURN_THUNK as the return address on the shadow stack. A thunk may add thunks
  (a COM vtable): `win_thunk` copies the entry first (a reference into the vector dangled, and the
  argument count read from freed memory broke the stack after `DirectDrawCreate`).
- `kernel32.cpp`: the game folder is drive C: (its root the current directory; names matched
  case-insensitively; no other drives), files as `FILE *`, Windows 95 (`GetVersion` C0000004h),
  code page 1252 as Latin-1, resources from the image's resource directories.
- `thread.cpp`: each program thread has a host thread (with the large stack) but one runs at a
  time: a switch saves and restores `R[]`, the shadow return stack and the segment bases. Switches
  at Sleep, a critical section another thread holds, an empty message queue, and every 65536
  instructions. The game starts a thread (41B700h) for each movie, ends it with `TerminateThread`
  (the host thread then ends with an exception thrown where it waits).
- `user32.cpp`: one window; messages (the host's keyboard as WM_KEYDOWN/WM_SYSKEYDOWN with virtual
  keys and scan codes, WM_CHAR from TranslateMessage, the mouse as relative movement onto a 640x480
  position that SetCursorPos also sets); ShowWindow sends WM_ACTIVATEAPP and WM_ACTIVATE.
- `ddraw.cpp`: DirectDraw 1/2 as COM objects in the program's memory with vtables of thunks: the
  640x480x8 primary surface with a back buffer (Flip swaps their memory), off-screen surfaces,
  palettes, Blt/BltFast with colour fill and source colour keys. The primary's picture goes to the
  host at Flip, Unlock and Blt. The game: "Card is not bank switched. Using double buffer mode.";
  DDCAPS of 316 bytes; a 320x240 off-screen surface for movies. Movies are interlaced (every other
  line black) as the 1996 version shows them.
- Missing: sound (DirectSound: the game asks for it and plays silently without; XanLib's waveOut),
  the joystick (none reported), the self-modifying code: the game makes 476080h-47D7AEh writable
  with `VirtualProtect` (PAGE_EXECUTE_READWRITE): what it patches there isn't known yet (the DOS
  version's texture mapper patched its own displacements; the recompiled code would ignore such
  patches).

## Debugging

`WC_TRACE=1` (Windows calls, their arguments and results), `WC_TRACE=2` (every instruction) with
`WC_TRACE_AT=addr,lo-hi,...` (only those, with ST0-ST2 and the x87 control word), `WC_WATCH=addr`,
`WC_HEADLESS=1`, `WC_SCREENSHOT=file.png` after `WC_SCREENSHOT_AT` seconds (with `WC_DUMP=file`
the memory too), `WC_KEYS=ms:key,...` (real time: e.g. `3000:esc,6000:esc,9000:esc` skips the intro
to the hangar). Test in a scratch copy of the game folder (the game writes saves there): copy the
small files, link the `.TRE` files.

## Plan

1. Sound: DirectSound and waveOut on SDL's audio.
2. The self-modifying code at 476080h.
3. Playing through: `npm run discover` for code still missing (exit 4), unimplemented functions
   (exit 6).
4. The joystick through SDL's gamepads (`joyGetPosEx`).
5. The enhancement patch's features, ported.
## Conventions

As in wc3-recomp: TypeScript run directly by Node (≥ 23.6), ESM, `npm run typecheck`; Java Ghidra
scripts in `ghidra_scripts/`; C++23 runtime, tab-indented, short comments; the standard library
rather than POSIX. Ghidra 12.2 (Homebrew; JAVA_HOME from the `ghidraRun` wrapper).
