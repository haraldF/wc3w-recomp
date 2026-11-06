/* The Windows the program sees: its process (win.cpp: memory layout, threads' information block,
 * the thunks behind its imports, calls back into it) and the functions it imports from Windows,
 * implemented natively on the emulated state (kernel32.cpp, ...). */
#pragma once
#include <cstdint>
#include <initializer_list>
#include <string>

#include "wc_rt.h"

/* The address space, below the image (wc_image_lo, 200000h) and above it. */
inline constexpr uint32_t TIB_ADDR = 0x10000;      /* the main thread's information block (FS) */
inline constexpr uint32_t PEB_ADDR = 0x11000;
inline constexpr uint32_t STRINGS_ADDR = 0x12000;  /* command line, environment, module names */
inline constexpr uint32_t STACK_LO = 0xf0000, STACK_HI = 0x1f0000; /* the main thread's stack */
inline constexpr uint32_t REGIONS_LO = 0x500000;   /* VirtualAlloc's regions, up to MEM_SIZE */
inline constexpr uint16_t FS_SELECTOR = 0x3b;

/* A Windows function: reads its arguments with arg(), returns EAX; the dispatcher pops the return
 * address and nargs arguments (stdcall), as the real one would. */
using win_fn = uint32_t (*)();
struct win_api {
	const char *dll, *name;
	int nargs;
	win_fn fn;
};
/* Registers an implementation at startup (the API macro). */
struct win_register {
	win_register(const char *dll, const char *name, int nargs, win_fn fn);
};
#define API(dll, name, nargs) \
	static uint32_t api_##dll##_##name(); \
	static win_register reg_##dll##_##name(#dll, #name, nargs, api_##dll##_##name); \
	static uint32_t api_##dll##_##name()

/* The i-th argument of the Windows function being called (after the return address). */
static inline uint32_t arg(int i) { return (uint32_t)mrd32(rd(R_SP, 4) + 4 + 4 * (uint32_t)i, 4); }

/* Strings and memory of the program's */
std::string guest_str(uint32_t addr);                   /* a NUL-terminated string */
std::string guest_str_n(uint32_t addr, int32_t n);     /* n bytes, or up to the NUL if n < 0 */
std::u16string guest_wstr_n(uint32_t addr, int32_t n); /* UTF-16, likewise */
static inline uint32_t rd32(uint32_t a) { return (uint32_t)mrd32(a, 4); }
static inline void wr32(uint32_t a, uint32_t v) { mwr32(a, 4, v); }
static inline void wr16(uint32_t a, uint16_t v) { mwr32(a, 2, v); }
/* Copies s with its NUL into at most size bytes at addr (truncated); returns the length copied
 * without the NUL. */
uint32_t put_str(uint32_t addr, const std::string &s, uint32_t size);

void set_last_error(uint32_t e);
uint32_t last_error(void);

/* Memory: VirtualAlloc's regions (64 KB granularity, zeroed) and a small heap for GlobalAlloc,
 * LocalAlloc and the runtime's own objects in the program's memory. */
uint32_t region_alloc(uint32_t addr, uint32_t size);
bool region_free(uint32_t addr);
uint32_t heap_alloc(uint32_t size, bool zero = true);
void heap_free(uint32_t addr);
uint32_t heap_size(uint32_t addr);

/* Thunks: an address that runs a native function when the program calls it, for GetProcAddress
 * and COM interfaces' methods. */
uint32_t thunk_native(const char *name, int nargs, win_fn fn);
/* The thunk of a Windows function by DLL and name, 0 if it isn't implemented. */
uint32_t thunk_for(const std::string &dll, const std::string &name);
/* Whether the runtime implements functions of that DLL ("USER32", "user32.dll") */
bool dll_known(const std::string &dll);
/* The program called a thunk (from run()). */
void win_thunk(uint32_t addr);

/* Calls the program's code at addr with the arguments (pushed right to left) and returns its EAX;
 * ESP is restored, whether the callee popped its arguments (stdcall) or not. */
uint32_t win_call(uint32_t addr, std::initializer_list<uint32_t> args);

/* Threads (thread.cpp): one runs at a time. */
void thread_init_main(void);
uint32_t thread_tib(void);   /* the running thread's information block */
uint32_t thread_id(void);
bool thread_close(uint32_t handle); /* whether it is a thread's handle */
void thread_yield(void);     /* let another thread run, if one can */
void thread_sleep(uint32_t ms);

/* Hooks (hooks.txt, wc_hook): a native handler for an instruction address. It returns true when it
 * did the instruction's work itself, e.g. a whole function: then it has set EAX and popped the
 * return address (and, for stdcall, the arguments), and the generated code returns. */
void hook_register(uint32_t at, bool (*fn)(void));

/* FS's base for a selector (seg_load) */
uint32_t win_fs_base(uint16_t selector);
/* Every 65536 instructions, on the program's thread. */
void win_poll(void);

/* The process: memory and the main thread set up, the game folder (drive C:'s root), the
 * arguments after the executable's name; then win_main() runs the DLLs' entry points and the
 * program's. */
void win_start(const char *game_dir, int argc, char **argv);
[[noreturn]] void win_main(void);
extern std::string win_game_dir;
extern uint32_t win_cmdline, win_cmdline_w, win_environment, win_environment_w;

/* The module a handle (its base) or name is; nullptr if none */
const wc_module *module_by_handle(uint32_t h);
const wc_module *module_by_name(const std::string &name);
/* A resource of a module: its data's address and size, 0 if there is none. type and name are
 * either an address of a string or a number (below 10000h), as FindResource takes them. */
uint32_t find_resource(const wc_module *m, uint32_t type, uint32_t name, uint32_t *size);

/* Windows error codes and constants used here */
enum : uint32_t {
	ERROR_SUCCESS = 0, ERROR_FILE_NOT_FOUND = 2, ERROR_PATH_NOT_FOUND = 3, ERROR_ACCESS_DENIED = 5,
	ERROR_INVALID_HANDLE = 6, ERROR_NOT_ENOUGH_MEMORY = 8, ERROR_NO_MORE_FILES = 18,
	ERROR_HANDLE_EOF = 38, ERROR_FILE_EXISTS = 80, ERROR_INVALID_PARAMETER = 87,
	ERROR_ALREADY_EXISTS = 183, ERROR_RESOURCE_NAME_NOT_FOUND = 1814,
};
inline constexpr uint32_t INVALID_HANDLE_VALUE = 0xffffffffu;
