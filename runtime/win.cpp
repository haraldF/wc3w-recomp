/* The process the program runs in: its memory (the image from image.bin, the main thread's stack
 * and information block, VirtualAlloc's regions, a small heap), the thunks that stand for the
 * Windows functions it imports, calls from the runtime back into it (window procedures, threads),
 * and its start: the DLLs' entry points, then the program's. */
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

#include "host.h"
#include "win.h"

std::string win_game_dir;
uint32_t win_cmdline, win_cmdline_w, win_environment, win_environment_w;

/* ---- the implementations, by DLL and name ---- */

static std::vector<win_api> &registry()
{
	static std::vector<win_api> r;
	return r;
}

win_register::win_register(const char *dll, const char *name, int nargs, win_fn fn)
{
	registry().push_back({ dll, name, nargs, fn });
}

/* "KERNEL32.dll" and "kernel32" are the same DLL */
static std::string dll_key(std::string dll)
{
	std::transform(dll.begin(), dll.end(), dll.begin(), [](unsigned char c) { return (char)toupper(c); });
	if (dll.size() > 4 && dll.compare(dll.size() - 4, 4, ".DLL") == 0) dll.resize(dll.size() - 4);
	return dll;
}

static const win_api *find_api(const std::string &dll, const std::string &name)
{
	std::string d = dll_key(dll);
	for (const win_api &a : registry()) {
		if (dll_key(a.dll) == d && name == a.name) return &a;
	}
	return nullptr;
}

bool dll_known(const std::string &dll)
{
	std::string d = dll_key(dll);
	for (const win_api &a : registry()) {
		if (dll_key(a.dll) == d) return true;
	}
	return false;
}

/* ---- thunks ---- */

struct thunk {
	std::string name; /* DLL!function, for reports */
	int nargs;
	win_fn fn;        /* null: not implemented (yet) */
};
static std::vector<thunk> thunks;

static void init_thunks(void)
{
	for (uint32_t i = 0; i < wc_import_count; i++) {
		const win_api *a = find_api(wc_imports[i].dll, wc_imports[i].name);
		thunks.push_back({ std::string(wc_imports[i].dll) + "!" + wc_imports[i].name, a ? a->nargs : 0, a ? a->fn : nullptr });
	}
}

uint32_t thunk_native(const char *name, int nargs, win_fn fn)
{
	thunks.push_back({ name, nargs, fn });
	return THUNK_BASE + THUNK_SIZE * (uint32_t)(thunks.size() - 1);
}

uint32_t thunk_for(const std::string &dll, const std::string &name)
{
	std::string full = dll_key(dll) + ".DLL!" + name;
	for (size_t i = 0; i < thunks.size(); i++) {
		if (thunks[i].fn && dll_key(thunks[i].name.substr(0, thunks[i].name.find('!'))) == dll_key(dll) &&
			thunks[i].name.substr(thunks[i].name.find('!') + 1) == name) {
			return THUNK_BASE + THUNK_SIZE * (uint32_t)i;
		}
	}
	const win_api *a = find_api(dll, name);
	if (!a) return 0;
	thunks.push_back({ full, a->nargs, a->fn });
	return THUNK_BASE + THUNK_SIZE * (uint32_t)(thunks.size() - 1);
}

void win_thunk(uint32_t addr)
{
	uint32_t i = (addr - THUNK_BASE) / THUNK_SIZE;
	if (addr < THUNK_BASE || (addr - THUNK_BASE) % THUNK_SIZE || i >= thunks.size()) {
		cpu_fail("call of an unknown thunk", addr);
	}
	const thunk t = thunks[i]; /* a copy: the function may add thunks (COM interfaces) */
	uint32_t esp = (uint32_t)rd(R_SP, 4);
	if (!t.fn) {
		fflush(stdout);
		fprintf(stderr, "wc: unimplemented Windows function %s, called from %06x\n", t.name.c_str(), rd32(esp));
		cpu_report(stderr);
		host_exit(6);
	}
	if (wc_trace) {
		fprintf(stderr, "wc: %s(", t.name.c_str());
		for (int k = 0; k < t.nargs; k++) fprintf(stderr, "%s%x", k ? ", " : "", rd32(esp + 4 + 4 * k));
		fprintf(stderr, ") from %06x", rd32(esp));
	}
	uint32_t eax = t.fn();
	if (wc_trace) fprintf(stderr, " = %x\n", eax);
	wr(R_AX, 4, eax);
	wr(R_SP, 4, esp + 4 + 4 * (uint32_t)t.nargs);
}

uint32_t win_call(uint32_t addr, std::initializer_list<uint32_t> args)
{
	uint32_t esp = (uint32_t)rd(R_SP, 4), sp = esp;
	for (auto a = std::rbegin(args); a != std::rend(args); ++a) {
		sp -= 4;
		wr32(sp, *a);
	}
	sp -= 4;
	wr32(sp, RETURN_THUNK);
	wr(R_SP, 4, sp);
	push_ret(RETURN_THUNK);
	run(addr);
	if (pop_ret()) cpu_fail("the program unwound past a call from the runtime to", addr);
	uint32_t eax = (uint32_t)rd(R_AX, 4);
	wr(R_SP, 4, esp);
	return eax;
}

/* ---- strings ---- */

std::string guest_str(uint32_t addr)
{
	return guest_str_n(addr, -1);
}

std::string guest_str_n(uint32_t addr, int32_t n)
{
	std::string s;
	for (uint32_t a = addr; n < 0 || (int32_t)(a - addr) < n; a++) {
		char c = (char)M[a & (MEM_SIZE - 1)];
		if (n < 0 && !c) break;
		s += c;
	}
	return s;
}

std::u16string guest_wstr_n(uint32_t addr, int32_t n)
{
	std::u16string s;
	for (uint32_t a = addr; n < 0 || (int32_t)((a - addr) / 2) < n; a += 2) {
		char16_t c = (char16_t)mrd32(a, 2);
		if (n < 0 && !c) break;
		s += c;
	}
	return s;
}

uint32_t put_str(uint32_t addr, const std::string &s, uint32_t size)
{
	if (!size) return 0;
	uint32_t n = std::min((uint32_t)s.size(), size - 1);
	memcpy(M + addr, s.data(), n);
	M[addr + n] = 0;
	return n;
}

void set_last_error(uint32_t e) { wr32(thread_tib() + 0x34, e); }
uint32_t last_error(void) { return rd32(thread_tib() + 0x34); }

/* ---- memory ---- */

static std::map<uint32_t, uint32_t> regions; /* base -> size */

uint32_t region_alloc(uint32_t addr, uint32_t size)
{
	size = (size + 0xffff) & ~0xffffu;
	if (!size) return 0;
	if (addr) {
		addr &= ~0xffffu;
		if (addr < REGIONS_LO || addr + size > MEM_SIZE || addr + size < addr) return 0;
		auto next = regions.lower_bound(addr);
		if (next != regions.end() && next->first < addr + size) return 0;
		if (next != regions.begin() && std::prev(next)->first + std::prev(next)->second > addr) return 0;
	}
	else {
		addr = REGIONS_LO;
		for (auto &[b, s] : regions) {
			if (b >= addr + size) break;
			addr = std::max(addr, b + s);
		}
		if (addr + size > MEM_SIZE) return 0;
	}
	regions[addr] = size;
	memset(M + addr, 0, size);
	return addr;
}

bool region_free(uint32_t addr)
{
	return regions.erase(addr) > 0;
}

/* The heap: arenas of 1 MB from the regions; blocks with a 16-byte header (their size), freed
 * blocks kept by size for reuse. Enough for the few small blocks that need it. */
static uint32_t arena, arena_left;
static std::multimap<uint32_t, uint32_t> free_blocks; /* size -> address */

uint32_t heap_alloc(uint32_t size, bool zero)
{
	size = (size + 15) & ~15u;
	if (auto it = free_blocks.lower_bound(size); it != free_blocks.end() && it->first <= size * 2) {
		uint32_t a = it->second;
		free_blocks.erase(it);
		if (zero) memset(M + a, 0, rd32(a - 16));
		return a;
	}
	if (arena_left < size + 16) {
		uint32_t n = std::max(size + 16, 0x100000u);
		arena = region_alloc(0, n);
		if (!arena) return 0;
		arena_left = (n + 0xffff) & ~0xffffu;
	}
	uint32_t a = arena + 16;
	wr32(arena, size);
	arena += size + 16;
	arena_left -= size + 16;
	return a;
}

void heap_free(uint32_t addr)
{
	if (addr) free_blocks.insert({ rd32(addr - 16), addr });
}

uint32_t heap_size(uint32_t addr)
{
	return addr ? rd32(addr - 16) : 0;
}

/* ---- modules and resources ---- */

const wc_module *module_by_handle(uint32_t h)
{
	for (uint32_t i = 0; i < wc_module_count; i++) {
		if (wc_modules[i].base == h) return &wc_modules[i];
	}
	return nullptr;
}

const wc_module *module_by_name(const std::string &name)
{
	std::string n = name.substr(name.find_last_of("\\/:") == std::string::npos ? 0 : name.find_last_of("\\/:") + 1);
	std::string k = dll_key(n);
	for (uint32_t i = 0; i < wc_module_count; i++) {
		std::string f = wc_modules[i].file;
		if (dll_key(f) == k || dll_key(f.substr(0, f.rfind('.'))) == k) return &wc_modules[i];
	}
	return nullptr;
}

/* An entry of a resource directory by number or (case-insensitive) name; returns its offset field. */
static bool resource_entry(uint32_t root, uint32_t dir, uint32_t key, uint32_t *out)
{
	uint32_t named = (uint32_t)mrd32(dir + 12, 2), ids = (uint32_t)mrd32(dir + 14, 2);
	std::u16string want;
	if (key >= 0x10000) {
		std::string s = guest_str(key);
		if (!s.empty() && s[0] == '#') {
			key = (uint32_t)atoi(s.c_str() + 1);
		}
		else {
			for (char c : s) want += (char16_t)toupper((unsigned char)c);
		}
	}
	for (uint32_t i = 0; i < named + ids; i++) {
		uint32_t e = dir + 16 + 8 * i, name = rd32(e);
		if (!want.empty() && (name & 0x80000000)) {
			uint32_t str = root + (name & 0x7fffffff);
			std::u16string s = guest_wstr_n(str + 2, (int32_t)mrd32(str, 2));
			for (auto &c : s) c = (char16_t)toupper(c);
			if (s != want) continue;
		}
		else if (want.empty() && (name & 0x80000000 || name != key)) {
			continue;
		}
		*out = rd32(e + 4);
		return true;
	}
	return false;
}

uint32_t find_resource(const wc_module *m, uint32_t type, uint32_t name, uint32_t *size)
{
	if (!m || !m->resources) return 0;
	uint32_t root = m->resources, off;
	if (!resource_entry(root, root, type, &off) || !(off & 0x80000000)) return 0;
	if (!resource_entry(root, root + (off & 0x7fffffff), name, &off) || !(off & 0x80000000)) return 0;
	uint32_t langs = root + (off & 0x7fffffff); /* any language: the first */
	if (mrd32(langs + 12, 2) + mrd32(langs + 14, 2) == 0) return 0;
	off = rd32(langs + 16 + 4);
	if (off & 0x80000000) return 0;
	uint32_t data = root + off;
	if (size) *size = rd32(data + 4);
	return m->base + rd32(data);
}

/* ---- the process ---- */

uint32_t win_fs_base(uint16_t selector)
{
	return selector == FS_SELECTOR ? thread_tib() : 0;
}

void win_poll(void)
{
	thread_yield();
}

/* Puts s (with its NUL) into the strings area; returns its address. */
static uint32_t strings_at = STRINGS_ADDR;
static uint32_t put_string(const std::string &s, bool wide)
{
	uint32_t a = strings_at;
	for (size_t i = 0; i < s.size(); i++) {
		if (wide) wr16(a + 2 * (uint32_t)i, (uint8_t)s[i]);
		else M[a + i] = (uint8_t)s[i];
	}
	strings_at += (uint32_t)(s.size() + 1) * (wide ? 2 : 1) + (wide ? 2 : 1);
	strings_at = (strings_at + 3) & ~3u;
	return a;
}

void win_start(const char *game_dir, int argc, char **argv)
{
	win_game_dir = game_dir;
	init_thunks();

	/* the image */
	std::string path = std::string(host_base_dir()) + "image.bin";
	FILE *f = fopen(path.c_str(), "rb");
	if (!f || fread(M + wc_image_lo, 1, wc_image_hi - wc_image_lo, f) != wc_image_hi - wc_image_lo) {
		fprintf(stderr, "wc: cannot read %s\n", path.c_str());
		exit(1);
	}
	fclose(f);

	/* command line and environment (the C library parses both) */
	std::string cmd = wc_exe;
	for (int i = 0; i < argc; i++) cmd += std::string(" ") + argv[i];
	win_cmdline = put_string(cmd, false);
	win_cmdline_w = put_string(cmd, true);
	std::string env = std::string("WINDIR=C:\\WINDOWS") + '\0' + "PATH=C:\\WINDOWS" + '\0';
	win_environment = put_string(env, false);
	win_environment_w = put_string(env, true);

	/* the main thread: its information block (FS) and stack */
	wr32(TIB_ADDR + 0x00, 0xffffffff); /* no exception handler yet */
	wr32(TIB_ADDR + 0x04, STACK_HI);
	wr32(TIB_ADDR + 0x08, STACK_LO);
	wr32(TIB_ADDR + 0x18, TIB_ADDR);
	wr32(TIB_ADDR + 0x20, 0x100); /* process id */
	wr32(TIB_ADDR + 0x24, 0x104); /* thread id */
	wr32(TIB_ADDR + 0x30, PEB_ADDR);
	for (uint32_t i = 0; i < wc_module_count; i++) {
		if (!wc_modules[i].dll) wr32(PEB_ADDR + 8, wc_modules[i].base);
	}
	wr(R_CS, 2, 0x1b);
	wr(R_DS, 2, 0x23);
	wr(R_ES, 2, 0x23);
	wr(R_SS, 2, 0x23);
	seg_load(4, FS_SELECTOR);
	wr(R_SP, 4, STACK_HI - 16);
	wr(R_FPCW, 2, 0x27f); /* as Windows starts a thread: all x87 exceptions masked, 53-bit precision */
	thread_init_main();
}

[[noreturn]] void win_main(void)
{
	for (uint32_t i = 0; i < wc_module_count; i++) {
		const wc_module &m = wc_modules[i];
		if (!m.dll) continue;
		if (wc_trace) fprintf(stderr, "wc: %s's entry point\n", m.file);
		if (!win_call(m.entry, { m.base, 1 /* DLL_PROCESS_ATTACH */, 0 })) {
			fprintf(stderr, "wc: %s failed to initialise\n", m.file);
			host_exit(1);
		}
	}
	for (uint32_t i = 0; i < wc_module_count; i++) {
		const wc_module &m = wc_modules[i];
		if (m.dll) continue;
		wr(R_SP, 4, rd(R_SP, 4) - 4);
		wr32((uint32_t)rd(R_SP, 4), RETURN_THUNK);
		push_ret(RETURN_THUNK);
		run(m.entry);
	}
	fprintf(stderr, "wc: the program's entry point returned (%llx)\n", rd(R_AX, 4));
	host_exit((int)rd(R_AX, 4));
}
