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
  `R[]` must hold Ghidra's whole register space, the x87 registers too (control word 10A0h, ST0-7
  from 1100h, up to 128Fh): at 0x400 bytes, starting a thread wrote 27Fh 3 KB past its saved
  registers into the host's heap, and macOS crashed in its menu code (NSMenu) on a later key.
- `user32.cpp`: one window; messages (the host's keyboard as WM_KEYDOWN/WM_SYSKEYDOWN with virtual
  keys and scan codes, WM_CHAR from TranslateMessage, the mouse as relative movement onto a 640x480
  position that SetCursorPos also sets); ShowWindow sends WM_ACTIVATEAPP and WM_ACTIVATE.
- `ddraw.cpp`: DirectDraw 1/2 as COM objects in the program's memory with vtables of thunks: the
  640x480x8 primary surface with a back buffer (Flip swaps their memory), off-screen surfaces,
  palettes, Blt/BltFast with colour fill and source colour keys. The primary's picture goes to the
  host at Flip, Unlock and Blt. The game: "Card is not bank switched. Using double buffer mode.";
  DDCAPS of 316 bytes; a 320x240 off-screen surface for movies. Movies are interlaced (every other
  line black) as the 1996 version shows them.
- Sound (`sound.cpp`, `dsound.cpp`, `winmm.cpp`): a mixer of voices playing 8- or 16-bit PCM
  straight from the program's memory, into 16-bit stereo at 44.1 kHz; SDL's audio stream pulls it
  (headless a thread consumes it in real time; `WC_WAV=file` records it). DirectSound (the game
  loads `dsound.dll` itself and plays silently without it): secondary buffers are voices, the play
  cursor is where the mixer is, the write cursor 10 ms ahead; Lock hands out the buffer's own
  memory. The game streams movies and music (`CD1MUSIC.TRE`) into 8 KB looping 16-bit mono 22 kHz
  buffers, polling GetCurrentPosition in a loop (a million times in 20 s), and plays effects from
  others. waveOut (XanLib): queued headers on a voice; the played ones are reported on the
  program's thread (`win_poll`, Sleep): a callback function between two instructions, all
  registers kept. Not used by the game so far.
- HD movies (`movie.cpp`): the enhancement patch's Mod_Movies/libvlc_Movies (clone
  `github.com/mattwells77/Wing_Commander_III_Enhancement_Patch`: its `wc3w.cpp` names the game's
  addresses) done with a hook and FFmpeg. The game's movie function 41C240h (cdecl, the path first:
  `..\..\data\movies\opening.MVE`) is replaced: the scene is the file name in lower case, its
  branches (the list at 4AAA50h from the index at 4A24D4h, -1 at the end) are letters, `movies.ini`
  in the movie folder renames scenes and branches or skips them ("-1"), the file is
  `Master_FMVs_Upscaled/<name><letter>.mp4` (1920x1080 H.264, 29.97 fps, AAC 48 kHz). After the
  branches the game's 40FD20h (`wc3_handle_movie(0)`) does the conversation choice and fills the
  list again, until it starts with -1. The game's movie message handler (41C700h) is added to its
  message nodes (483090h/4830D0h) and its message loop (482920h) pumped while playing. Esc ends a
  movie (the game's handler doesn't: only its halt flag 4A24C4h is checked too); the next starts
  once Esc is up. No first file: the original player plays it. A decoder thread per branch (RGB
  frames, the sound resampled to the mixer as a `pcm_stream`); the frame shown is the newest due by
  the sound's clock. The host draws it fitted to the window with the game's own picture over it
  (colour 0 transparent: the choice text). `WC_HD_MOVIES=0`: the original movies;
  `WC_HD_MOVIES=Master_FMVs_Source`: the other set. Not done yet: the in-flight comms (460 files in
  `Master_FMVs_Upscaled/inflight`, the patch's other hooks at 422EA5h, 423085h, 433731h), subtitles,
  the patch's colours for the choice text (palette entries 252/253 and its code patches at
  414940h-414983h): conversations are untested.
- The joystick (`winmm.cpp`, the host's `publish_stick`): the game calls `joyGetDevCapsA` once at
  its start (into 4B2338h; wCaps at 4B2398h: HASZ, HASR, HASPOV decide whether it reads those), so
  the host opens a stick plugged in before the game thread starts (with `SDL_JOYSTICK_MFI=0`, as in
  wc3-recomp). It then reads `joyGetPosEx` (flags CFh) all the time: X, Y; Z is the throttle
  (482AF6h: 100 - (Z - Zmin) * 100 / (Zmax - Zmin) %, so 0 is full; 429320h); R the twist, which
  turns only beyond the last eighth either side (4A7E38h = -1/1); four buttons; the hat. Axes 0..65535,
  a flight stick's in SDL's order X, Y, twist, throttle (`WC_STICK_AXES` to change, "-" reverses),
  a controller's left stick, right stick x as twist, A B X Y, d-pad as hat. In the hangar the stick
  picks hotspots (the pointer disappears, the label shows) and the trigger goes there. Flight with
  a stick untested. `WC_JOYSTICK=1`: a virtual stick for WC_KEYS (`%x.y`, `j1`..`j4`).
- Self-modifying code: the game makes 476080h-47D7AEh writable with `VirtualProtect` and its
  texture mappers patch their own displacements and immediates there with stores to constant
  addresses (478092h: `MOV ES:[478511h],EAX`, ...), as the DOS version's did. The generator's
  patched-constant handling covers them: 1560 instructions read such constants from memory
  (`mrd32(0x478511, 4)`). A memory dump in flight shows 45 dwords changed there.

## Saved games

`*.WSG`: IFF FORM SAVE. `src/print-save.ts` (`npm run print-save`) shows morale and kills; its header has what
the code does with them. PILT is morale + 1 per pilot slot (0 dead; negative: no ejection when shot
down, set by the gameflow), KILL the kills, ACE_ the aces shot down (both on the killboard); the killboard's names by slot (415C3Eh): 1 Cobra,
2 Vagabond, 4 Flash, 6 Hobbes, 7 Vaquero, 8 Maniac, 9 Flint; slot 0 is the ship's morale (only
conversations read it). PLYR has the player's kills and aces, STAT the scene (the mission counter).
`WSG_NDX.WSG` is the slots' index.

## Archives and missions

- `.TRE` (XTRE): `src/tre.ts` reads them (layout in its header). Entries are found by a hash of the
  path the game asks for (`..\..\DATA\MISSIONS\MISNL004.IFF`: rotate left 3, add the character);
  only a few keep their path in the archive, the others are named by hashing guesses (words from the
  missions and scenes name all 67 missions). Packed entries: LZW (bit 31) or RefPack (bits 31 and 30).
- `DATA/GAMEFLOW/SCENxxxx.IFF` in the game folder are the scenes as loose files (the remaster's
  versions, which the game loads instead of the archived ones). Their CODE chunks are
  a stack bytecode (FACEh-tagged opcodes) that branches on the gameflow registers (REGS) and flags.
- Missions (`src/print-mission.ts`): the script is pairs of FEED00xxh and an operand, run by
  44C780h on unit events (a kill runs the unit's "destroyed" handler, which counts it). The outcome
  is reg 12, which the script sets when the mission ends (1 success; the scene then plays the landing
  for it), and the next scene. A planetary mission is several files: MISNL004 (space) loads MISNLG04
  (the surface), which loads MISNLG4A (the way home), passing the result in reg 49.
- Alcor 4 (MISNL004/MISNLG04, the ground strike on Alcor 5; save scene 39) is a success only with
  all 16 units on the surface destroyed: the 12 Ekapshi and the 4 KTANKs at NAV 1 (unit positions
  are relative to the AREA they name). When 4 of the Ekapshi are down the script launches the last
  two (opcode 90h). The autopilot lets you leave NAV 1 with tanks alive. The user's "disaster"
  landing was that, not a port bug; with the NAV 1 tanks down it succeeded.
- `DATA/GAMEFLOW` etc. in the game folder override the archives (the hook at 483A82h, below).

## Playing it by script

`WC_KEYS` in real time (ms after the start), mouse positions in 640x480, from a scratch game folder
with the user's DOS saves (H1: right before a mission, needing its briefing):
- intro: `3000:esc,6000:esc,9000:esc` (hangar at ~8 s); Flight Control: `@320.444` + click (a click is
  `+lmb`, `-lmb` 150 ms later), the deck officer's scene Esc;
- Flight Control: Main Terminal `@360.70`, callsign letters 200 ms apart + enter, Duty Logs
  `@120.372`, slot H1 `@122.127`, Load `@130.421`, a click past "Insert CD", Return `@510.421`,
  Logoff `@320.421`;
- Flight Control's hotspots then: Attend BRIEFING `@30.200` (left edge), Run SIMULATOR
  `@320.300`, Main Terminal `@470.260`, Fly MISSION `@612.240` (right edge; before the briefing it
  goes to the hangar), Talk to RACHEL `@80.280`, Lift `@320.444` (bridge `@569.182`, living
  `@592.240`, flight `@576.302` on its deck map);
- briefing: Esc, Esc, then the wingman choice (a click on one: Hobbes where the pointer is), Esc
  through the rest; Fly MISSION, Esc past the launch: the cockpit in the bay at ~86 s; A takes off.
The whole way: `3000:esc,6000:esc,9000:esc,10000:@320.444,10500:+lmb,10650:-lmb,12500:esc,
13500:@360.70,14000:+lmb,14150:-lmb,16000:n,16200:e,16400:m,16600:e,16800:s,17000:i,17200:s,
17400:enter,18500:@120.372,19000:+lmb,19150:-lmb,21000:@122.127,21500:+lmb,21650:-lmb,
23000:@130.421,23500:+lmb,23650:-lmb,26000:+lmb,26150:-lmb,30000:@510.421,30500:+lmb,30650:-lmb,
32000:@320.421,32500:+lmb,32650:-lmb,34000:@30.200,34500:+lmb,34650:-lmb,40000:esc,46000:esc,
52000:+lmb,52150:-lmb,56000:esc,62000:esc,68000:esc,72000:@612.240,72500:+lmb,72650:-lmb,
78000:esc,88000:a` (in space at ~100 s; then `a` for the autopilot, `t`, `space`, `enter`).
Hover labels show at the bottom (y 440-470); comparing that strip across screenshots of several
hover positions (parallel runs) finds hotspots. 5 minutes of autopilot, guns and missiles ran
without a stop (about 25 frames per second, 840 sound effects).

## Hooks (hooks.txt)

Addresses where the generated code first calls `wc_hook(addr)` (the generator's fourth argument);
a handler registered with `hook_register` may do the work itself (a whole function: set EAX, pop
the return address) and return true, and the generated function returns; false runs the original
code. This is how the enhancement patch's code replacements are ported.
- 452861h (`fixes.cpp`): the pointer centred at a mission's start. The mouse steers by its distance
  from (320, 240), and the game (also on Windows) left it where Fly MISSION was clicked, at the right
  edge: the ship turned right from the start. The game knows the pointer only from WM_MOUSEMOVE
  (state at 4A7E5Ah, flight's copy at 4A9B92h), so SetCursorPos posts one, as Windows does.
- 483A82h (`fixes.cpp`): the remaster's `DATA` folder. Its patch looks for a file the game wants
  from an archive (`..\..\DATA\...`) in the game folder's `DATA` first, renaming it to `.\DATA\...`
  before the archive lookup (484CB0h), which then misses. The loose files are the remaster's fixes:
  the ship's scenes (room 6 no longer resets reg 31 first; scenes 42-45 and VICTORY change hotspots),
  profiles, some in-flight movies, MEDF_H. `WC_TRACE=1` shows each as `[DATA: ...]`.

## Debugging

`WC_TRACE=1` (Windows calls, their arguments and results), `WC_TRACE=2` (every instruction) with
`WC_TRACE_AT=addr,lo-hi,...` (only those, with ST0-ST2 and the x87 control word), `WC_WATCH=addr`,
`WC_HEADLESS=1`, `WC_WAV=file.wav`, `WC_SCREENSHOT=file.png` after `WC_SCREENSHOT_AT` seconds (with `WC_DUMP=file`
the memory too), `WC_KEYS=ms:key,...` (real time: e.g. `3000:esc,6000:esc,9000:esc` skips the intro
to the hangar). Test in a scratch copy of the game folder (the game writes saves there): copy the
small files, link the `.TRE` files.

## Plan

1. Playing through: `npm run discover` for code still missing (exit 4), unimplemented functions
   (exit 6).
2. The enhancement patch's features, ported (its controller presets and mapping utility too).
## Conventions

As in wc3-recomp: TypeScript run directly by Node (≥ 23.6), ESM, `npm run typecheck`; Java Ghidra
scripts in `ghidra_scripts/`; C++23 runtime, tab-indented, short comments; the standard library
rather than POSIX. Ghidra 12.2 (Homebrew; JAVA_HOME from the `ghidraRun` wrapper).
