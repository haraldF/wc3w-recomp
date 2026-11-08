/* KERNEL32: what the C library's startup asks for (version, command line, environment, code pages,
 * standard handles), memory (VirtualAlloc, GlobalAlloc, LocalAlloc), files on drive C:, which is
 * the game folder (names case-insensitive, as on Windows), resources, time, and the
 * synchronisation calls (one thread so far: nothing to wait for). */
#include <algorithm>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <filesystem>
#include <map>
#include <system_error>
#include <thread>
#include <vector>

#include "host.h"
#include "win.h"

namespace fs = std::filesystem;

/* ---- handles ---- */

enum class kind { file, find, mutex, console };
struct object {
	kind k;
	FILE *f = nullptr;
	fs::path path;
	std::vector<fs::path> found; /* FindFirstFile: the matches still to report */
};
static std::map<uint32_t, object> handles;
static uint32_t next_handle = 0x1000;
static const uint32_t STD_IN = 0x10, STD_OUT = 0x14, STD_ERR = 0x18;

static uint32_t new_handle(object o)
{
	uint32_t h = next_handle;
	next_handle += 4;
	handles[h] = std::move(o);
	return h;
}

static object *get(uint32_t h)
{
	auto it = handles.find(h);
	return it == handles.end() ? nullptr : &it->second;
}

/* ---- paths: drive C: is the game folder ---- */

static bool same_name(const std::string &a, const std::string &b)
{
	return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(),
		[](char x, char y) { return toupper((unsigned char)x) == toupper((unsigned char)y); });
}

/* The host path of a program's path: "C:\DIR\FILE", "\DIR\FILE" or "DIR\FILE" (the current
 * directory is C:'s root), each part matched case-insensitively; a part that isn't there is kept as
 * it is (a file to create). Another drive than C: has nothing. */
static bool host_path(const std::string &name, fs::path *out)
{
	std::string n = name;
	if (n.size() >= 2 && n[1] == ':') {
		if (toupper((unsigned char)n[0]) != 'C') return false;
		n = n.substr(2);
	}
	fs::path p = win_game_dir;
	size_t i = 0;
	while (i <= n.size()) {
		size_t j = n.find_first_of("\\/", i);
		if (j == std::string::npos) j = n.size();
		std::string part = n.substr(i, j - i);
		i = j + 1;
		if (part.empty() || part == ".") continue;
		if (part == "..") {
			if (p != fs::path(win_game_dir)) p = p.parent_path();
			continue;
		}
		fs::path next = p / part;
		std::error_code ec;
		if (!fs::exists(next, ec)) {
			for (const auto &e : fs::directory_iterator(p, ec)) {
				if (same_name(e.path().filename().string(), part)) {
					next = e.path();
					break;
				}
			}
		}
		p = next;
	}
	*out = p;
	return true;
}

/* A regular file at the program's path (fixes.cpp: the DATA folder's files) */
bool win_file_exists(const std::string &name)
{
	fs::path p;
	std::error_code ec;
	return host_path(name, &p) && fs::is_regular_file(p, ec);
}

/* ---- the C library's startup ---- */

API(KERNEL32, GetVersion, 0) { return 0xc0000004; } /* Windows 95: 4.0, high bit set */
API(KERNEL32, GetCommandLineA, 0) { return win_cmdline; }
API(KERNEL32, GetEnvironmentStrings, 0) { return win_environment; }
API(KERNEL32, GetEnvironmentStringsW, 0) { return win_environment_w; }
API(KERNEL32, FreeEnvironmentStringsA, 1) { return 1; }
API(KERNEL32, FreeEnvironmentStringsW, 1) { return 1; }

API(KERNEL32, GetStartupInfoA, 1)
{
	memset(M + arg(0), 0, 68);
	wr32(arg(0), 68);
	return 0;
}

static uint32_t loaded_dll(const std::string &name);
static bool is_loaded(const std::string &name);

API(KERNEL32, GetModuleHandleA, 1)
{
	if (!arg(0)) return wc_modules[wc_module_count - 1].base; /* the EXE comes last */
	std::string name = guest_str(arg(0));
	if (const wc_module *m = module_by_name(name)) return m->base;
	if (dll_known(name) || is_loaded(name)) return loaded_dll(name); /* Windows' that the runtime has functions of */
	set_last_error(126 /* ERROR_MOD_NOT_FOUND */);
	return 0;
}

API(KERNEL32, GetModuleFileNameA, 3)
{
	const wc_module *m = arg(0) ? module_by_handle(arg(0)) : &wc_modules[wc_module_count - 1];
	if (!m) return 0;
	return put_str(arg(1), std::string("C:\\") + m->file, arg(2));
}

API(KERNEL32, GetStdHandle, 1)
{
	switch (arg(0)) {
	case 0xfffffff6: return STD_IN;
	case 0xfffffff5: return STD_OUT;
	case 0xfffffff4: return STD_ERR;
	}
	return INVALID_HANDLE_VALUE;
}

API(KERNEL32, SetStdHandle, 2) { return 1; }

API(KERNEL32, GetFileType, 1)
{
	uint32_t h = arg(0);
	if (h == STD_IN || h == STD_OUT || h == STD_ERR) return 2; /* FILE_TYPE_CHAR */
	return get(h) ? 1 /* FILE_TYPE_DISK */ : 0;
}

API(KERNEL32, SetHandleCount, 1) { return arg(0); }
API(KERNEL32, GetACP, 0) { return 1252; }
API(KERNEL32, GetOEMCP, 0) { return 437; }

API(KERNEL32, GetCPInfo, 2)
{
	memset(M + arg(1), 0, 20);
	wr32(arg(1), 1); /* MaxCharSize: single bytes */
	M[arg(1) + 4] = '?';
	return 1;
}

API(KERNEL32, GetLastError, 0) { return last_error(); }
API(KERNEL32, GetCurrentProcess, 0) { return 0xffffffff; }
API(KERNEL32, SetPriorityClass, 2) { return 1; }

API(KERNEL32, ExitProcess, 1)
{
	if (wc_trace) fprintf(stderr, "\n");
	host_exit((int)arg(0));
}

API(KERNEL32, TerminateProcess, 2)
{
	if (wc_trace) fprintf(stderr, "\n");
	host_exit((int)arg(1));
}

/* ---- character sets: code page 1252 as Latin-1 ---- */

API(KERNEL32, MultiByteToWideChar, 6)
{
	int32_t n = (int32_t)arg(3);
	std::string s = guest_str_n(arg(2), n);
	if (n < 0) s += '\0';
	uint32_t out = arg(4), size = arg(5);
	if (!size) return (uint32_t)s.size();
	if (s.size() > size) {
		set_last_error(122 /* ERROR_INSUFFICIENT_BUFFER */);
		return 0;
	}
	for (size_t i = 0; i < s.size(); i++) wr16(out + 2 * (uint32_t)i, (uint8_t)s[i]);
	return (uint32_t)s.size();
}

API(KERNEL32, WideCharToMultiByte, 8)
{
	int32_t n = (int32_t)arg(3);
	std::u16string s = guest_wstr_n(arg(2), n);
	if (n < 0) s += u'\0';
	uint32_t out = arg(4), size = arg(5);
	if (!size) return (uint32_t)s.size();
	if (s.size() > size) {
		set_last_error(122);
		return 0;
	}
	for (size_t i = 0; i < s.size(); i++) M[out + i] = s[i] < 256 ? (uint8_t)s[i] : '?';
	return (uint32_t)s.size();
}

/* CT_CTYPE1 classes of a Latin-1 character */
static uint16_t ctype1(unsigned c)
{
	uint16_t t = 0;
	if (isupper(c) || (c >= 0xc0 && c <= 0xde && c != 0xd7)) t |= 0x001;
	if (islower(c) || (c >= 0xdf && c != 0xf7)) t |= 0x002;
	if (c < 128 && isdigit((int)c)) t |= 0x004;
	if (c < 128 && isspace((int)c)) t |= 0x008;
	if (c < 128 && ispunct((int)c)) t |= 0x010;
	if (c < 32 || c == 127) t |= 0x020;
	if (c == ' ' || c == '\t') t |= 0x040;
	if (c < 128 && isxdigit((int)c)) t |= 0x080;
	if (t & 3) t |= 0x100;
	return t;
}

API(KERNEL32, GetStringTypeA, 5)
{
	if (arg(1) != 1) cpu_fail("GetStringTypeA of another type than CT_CTYPE1 from", rd32((uint32_t)rd(R_SP, 4)));
	std::string s = guest_str_n(arg(2), (int32_t)arg(3));
	for (size_t i = 0; i < s.size(); i++) wr16(arg(4) + 2 * (uint32_t)i, ctype1((uint8_t)s[i]));
	return 1;
}

API(KERNEL32, GetStringTypeW, 4)
{
	if (arg(0) != 1) cpu_fail("GetStringTypeW of another type than CT_CTYPE1 from", rd32((uint32_t)rd(R_SP, 4)));
	std::u16string s = guest_wstr_n(arg(1), (int32_t)arg(2));
	for (size_t i = 0; i < s.size(); i++) wr16(arg(3) + 2 * (uint32_t)i, s[i] < 256 ? ctype1(s[i]) : 0);
	return 1;
}

static unsigned map_case(unsigned c, uint32_t flags)
{
	if (flags & 0x100 /* LCMAP_LOWERCASE */) return c < 128 ? (unsigned)tolower((int)c) : c >= 0xc0 && c <= 0xde && c != 0xd7 ? c + 32 : c;
	if (flags & 0x200 /* LCMAP_UPPERCASE */) return c < 128 ? (unsigned)toupper((int)c) : c >= 0xe0 && c <= 0xfe && c != 0xf7 ? c - 32 : c;
	return c;
}

API(KERNEL32, LCMapStringA, 6)
{
	uint32_t flags = arg(1);
	if (flags & ~0x300u) cpu_fail("LCMapStringA with unsupported flags from", rd32((uint32_t)rd(R_SP, 4)));
	int32_t n = (int32_t)arg(3);
	std::string s = guest_str_n(arg(2), n);
	if (n < 0) s += '\0';
	if (!arg(5)) return (uint32_t)s.size();
	if (s.size() > arg(5)) return 0;
	for (size_t i = 0; i < s.size(); i++) M[arg(4) + i] = (uint8_t)map_case((uint8_t)s[i], flags);
	return (uint32_t)s.size();
}

API(KERNEL32, LCMapStringW, 6)
{
	uint32_t flags = arg(1);
	if (flags & ~0x300u) cpu_fail("LCMapStringW with unsupported flags from", rd32((uint32_t)rd(R_SP, 4)));
	int32_t n = (int32_t)arg(3);
	std::u16string s = guest_wstr_n(arg(2), n);
	if (n < 0) s += u'\0';
	if (!arg(5)) return (uint32_t)s.size();
	if (s.size() > arg(5)) return 0;
	for (size_t i = 0; i < s.size(); i++) wr16(arg(4) + 2 * (uint32_t)i, s[i] < 256 ? (uint16_t)map_case(s[i], flags) : s[i]);
	return (uint32_t)s.size();
}

/* ---- memory ---- */

API(KERNEL32, VirtualAlloc, 4)
{
	uint32_t addr = arg(0), size = arg(1), type = arg(2);
	if (addr && !(type & 0x2000 /* MEM_RESERVE */)) {
		/* committing pages of a reserved region: it's all there */
		return addr & ~0xfffu;
	}
	uint32_t a = region_alloc(addr, size);
	if (!a) set_last_error(ERROR_NOT_ENOUGH_MEMORY);
	if (wc_trace) fprintf(stderr, " [region %x..%x]", a, a + size);
	return a;
}

API(KERNEL32, VirtualFree, 3)
{
	uint32_t addr = arg(0), size = arg(1), type = arg(2);
	if (type & 0x8000 /* MEM_RELEASE */) return region_free(addr);
	if (type & 0x4000 /* MEM_DECOMMIT */) {
		memset(M + (addr & (MEM_SIZE - 1)), 0, std::min(size, MEM_SIZE - (addr & (MEM_SIZE - 1))));
		return 1;
	}
	return 0;
}

API(KERNEL32, VirtualProtect, 4)
{
	if (wc_trace) fprintf(stderr, " [protect %x..%x as %x]", arg(0), arg(0) + arg(1), arg(2));
	if (arg(3)) wr32(arg(3), 4 /* PAGE_READWRITE */);
	return 1;
}

static uint32_t global_alloc(uint32_t flags, uint32_t size)
{
	uint32_t a = heap_alloc(size ? size : 1, flags & 0x40 /* GMEM_ZEROINIT */);
	if (!a) set_last_error(ERROR_NOT_ENOUGH_MEMORY);
	return a;
}

/* Fixed memory only: handles are addresses. */
API(KERNEL32, GlobalAlloc, 2) { return global_alloc(arg(0), arg(1)); }
API(KERNEL32, LocalAlloc, 2) { return global_alloc(arg(0), arg(1)); }
API(KERNEL32, GlobalFree, 1) { heap_free(arg(0)); return 0; }
API(KERNEL32, LocalFree, 1) { heap_free(arg(0)); return 0; }

API(KERNEL32, GlobalMemoryStatus, 1)
{
	uint32_t p = arg(0);
	wr32(p, 32);
	wr32(p + 4, 10);          /* memory load, % */
	wr32(p + 8, 64 << 20);    /* physical */
	wr32(p + 12, 48 << 20);
	wr32(p + 16, 128 << 20);  /* page file */
	wr32(p + 20, 128 << 20);
	wr32(p + 24, MEM_SIZE);   /* virtual */
	wr32(p + 28, MEM_SIZE - REGIONS_LO);
	return 0;
}

/* ---- files ---- */

API(KERNEL32, CreateFileA, 7)
{
	std::string name = guest_str(arg(0));
	uint32_t access = arg(1), disposition = arg(4);
	fs::path p;
	if (!host_path(name, &p)) {
		set_last_error(ERROR_PATH_NOT_FOUND);
		return INVALID_HANDLE_VALUE;
	}
	std::error_code ec;
	bool exists = fs::is_regular_file(p, ec);
	bool write = access & 0x40000000; /* GENERIC_WRITE */
	const char *mode = nullptr;
	switch (disposition) {
	case 1: /* CREATE_NEW */
		if (exists) {
			set_last_error(ERROR_FILE_EXISTS);
			return INVALID_HANDLE_VALUE;
		}
		mode = "w+b";
		break;
	case 2: /* CREATE_ALWAYS */
		mode = "w+b";
		break;
	case 3: /* OPEN_EXISTING */
		mode = write ? "r+b" : "rb";
		break;
	case 4: /* OPEN_ALWAYS */
		mode = exists ? (write ? "r+b" : "rb") : "w+b";
		break;
	case 5: /* TRUNCATE_EXISTING */
		mode = exists ? "w+b" : nullptr;
		break;
	}
	FILE *f = mode && (exists || mode[0] == 'w') ? fopen(p.string().c_str(), mode) : nullptr;
	if (!f) {
		set_last_error(exists ? ERROR_ACCESS_DENIED : ERROR_FILE_NOT_FOUND);
		if (wc_trace) fprintf(stderr, " [%s: not opened]", p.string().c_str());
		return INVALID_HANDLE_VALUE;
	}
	set_last_error(exists && (disposition == 2 || disposition == 4) ? ERROR_ALREADY_EXISTS : 0);
	if (wc_trace) fprintf(stderr, " [%s]", p.string().c_str());
	return new_handle({ kind::file, f, p, {} });
}

API(KERNEL32, ReadFile, 5)
{
	object *o = get(arg(0));
	if (!o || !o->f) {
		set_last_error(ERROR_INVALID_HANDLE);
		return 0;
	}
	uint32_t n = (uint32_t)fread(M + arg(1), 1, std::min(arg(2), MEM_SIZE - arg(1)), o->f);
	if (arg(3)) wr32(arg(3), n);
	return 1;
}

API(KERNEL32, WriteFile, 5)
{
	uint32_t h = arg(0), n = arg(2);
	if (h == STD_OUT || h == STD_ERR) {
		fwrite(M + arg(1), 1, n, stderr);
	}
	else {
		object *o = get(h);
		if (!o || !o->f) {
			set_last_error(ERROR_INVALID_HANDLE);
			return 0;
		}
		n = (uint32_t)fwrite(M + arg(1), 1, n, o->f);
	}
	if (arg(3)) wr32(arg(3), n);
	return 1;
}

API(KERNEL32, SetFilePointer, 4)
{
	object *o = get(arg(0));
	if (!o || !o->f) {
		set_last_error(ERROR_INVALID_HANDLE);
		return 0xffffffff;
	}
	int64_t dist = (int32_t)arg(1);
	if (arg(2)) dist = (int64_t)((uint64_t)rd32(arg(2)) << 32 | arg(1));
	if (fseeko(o->f, dist, arg(3) == 0 ? SEEK_SET : arg(3) == 1 ? SEEK_CUR : SEEK_END)) {
		set_last_error(ERROR_INVALID_PARAMETER);
		return 0xffffffff;
	}
	int64_t pos = ftello(o->f);
	if (arg(2)) wr32(arg(2), (uint32_t)(pos >> 32));
	set_last_error(0);
	return (uint32_t)pos;
}

API(KERNEL32, SetEndOfFile, 1)
{
	object *o = get(arg(0));
	if (!o || !o->f) return 0;
	fflush(o->f);
	std::error_code ec;
	fs::resize_file(o->path, (uintmax_t)ftello(o->f), ec);
	return !ec;
}

API(KERNEL32, FlushFileBuffers, 1)
{
	object *o = get(arg(0));
	if (o && o->f) fflush(o->f);
	return 1;
}

API(KERNEL32, CloseHandle, 1)
{
	auto it = handles.find(arg(0));
	if (it == handles.end()) {
		if (thread_close(arg(0))) return 1;
		set_last_error(ERROR_INVALID_HANDLE);
		return 0;
	}
	if (it->second.f) {
		fclose(it->second.f);
		if (it->second.k == kind::file) host_file_saved(it->second.path.string().c_str());
	}
	handles.erase(it);
	return 1;
}

API(KERNEL32, DeleteFileA, 1)
{
	fs::path p;
	std::error_code ec;
	if (!host_path(guest_str(arg(0)), &p) || !fs::remove(p, ec)) {
		set_last_error(ERROR_FILE_NOT_FOUND);
		return 0;
	}
	return 1;
}

API(KERNEL32, GetFileAttributesA, 1)
{
	fs::path p;
	std::error_code ec;
	if (!host_path(guest_str(arg(0)), &p) || !fs::exists(p, ec)) {
		set_last_error(ERROR_FILE_NOT_FOUND);
		return 0xffffffff;
	}
	return fs::is_directory(p, ec) ? 0x10 : 0x80; /* FILE_ATTRIBUTE_DIRECTORY, _NORMAL */
}

/* "*" and "?" against a name, case-insensitively */
static bool wild_match(const char *pat, const char *name)
{
	if (!*pat) return !*name;
	if (*pat == '*') return wild_match(pat + 1, name) || (*name && wild_match(pat, name + 1));
	if (!*name) return false;
	return (*pat == '?' || toupper((unsigned char)*pat) == toupper((unsigned char)*name)) && wild_match(pat + 1, name + 1);
}

static void put_find_data(uint32_t d, const fs::path &p)
{
	std::error_code ec;
	memset(M + d, 0, 320);
	bool dir = fs::is_directory(p, ec);
	wr32(d, dir ? 0x10 : 0x80);
	if (!dir) wr32(d + 32, (uint32_t)fs::file_size(p, ec));
	put_str(d + 44, p.filename().string(), 260);
}

API(KERNEL32, FindFirstFileA, 2)
{
	std::string pattern = guest_str(arg(0));
	size_t slash = pattern.find_last_of("\\/:");
	std::string dir = slash == std::string::npos ? "" : pattern.substr(0, slash + 1);
	std::string mask = pattern.substr(slash == std::string::npos ? 0 : slash + 1);
	fs::path p;
	object o{ kind::find, nullptr, {}, {} };
	std::error_code ec;
	if (host_path(dir, &p)) {
		for (const auto &e : fs::directory_iterator(p, ec)) {
			if (wild_match(mask.c_str(), e.path().filename().string().c_str())) o.found.push_back(e.path());
		}
	}
	std::sort(o.found.begin(), o.found.end());
	if (o.found.empty()) {
		set_last_error(ERROR_FILE_NOT_FOUND);
		return INVALID_HANDLE_VALUE;
	}
	put_find_data(arg(1), o.found.front());
	o.found.erase(o.found.begin());
	return new_handle(std::move(o));
}

API(KERNEL32, FindNextFileA, 2)
{
	object *o = get(arg(0));
	if (!o || o->found.empty()) {
		set_last_error(ERROR_NO_MORE_FILES);
		return 0;
	}
	put_find_data(arg(1), o->found.front());
	o->found.erase(o->found.begin());
	return 1;
}

API(KERNEL32, FindClose, 1) { return handles.erase(arg(0)) > 0; }

/* Drives: C: only, the game folder */
API(KERNEL32, GetLogicalDriveStringsA, 2)
{
	static const char drives[] = "C:\\\0";
	if (arg(0) < sizeof drives) return sizeof drives;
	memcpy(M + arg(1), drives, sizeof drives);
	return sizeof drives - 1;
}

API(KERNEL32, GetDriveTypeA, 1)
{
	std::string root = arg(0) ? guest_str(arg(0)) : "C:\\";
	return !root.empty() && toupper((unsigned char)root[0]) == 'C' ? 3 /* DRIVE_FIXED */ : 1 /* DRIVE_NO_ROOT_DIR */;
}

API(KERNEL32, GetDiskFreeSpaceA, 5)
{
	if (arg(1)) wr32(arg(1), 8);      /* sectors per cluster */
	if (arg(2)) wr32(arg(2), 512);    /* bytes per sector */
	if (arg(3)) wr32(arg(3), 0x40000); /* free clusters: 1 GB */
	if (arg(4)) wr32(arg(4), 0x80000);
	return 1;
}

/* ---- console ---- */

API(KERNEL32, GetConsoleMode, 2) { return 0; } /* no console */
API(KERNEL32, SetConsoleMode, 2) { return 0; }

API(KERNEL32, WriteConsoleA, 5)
{
	fwrite(M + arg(1), 1, arg(2), stderr);
	if (arg(3)) wr32(arg(3), arg(2));
	return 1;
}

/* ---- modules and resources ---- */

/* DLLs the program loads itself (LoadLibrary): a made-up handle each, its functions by name from
 * the implementations here (GetProcAddress). */
static std::vector<std::string> loaded_dlls;
static const uint32_t LOADED_DLL_BASE = 0x7e000000;

static bool is_loaded(const std::string &name)
{
	for (const auto &d : loaded_dlls) {
		if (same_name(d, name)) return true;
	}
	return false;
}

static uint32_t loaded_dll(const std::string &name)
{
	for (size_t i = 0; i < loaded_dlls.size(); i++) {
		if (same_name(loaded_dlls[i], name)) return LOADED_DLL_BASE + 0x10000 * (uint32_t)i;
	}
	loaded_dlls.push_back(name);
	return LOADED_DLL_BASE + 0x10000 * (uint32_t)(loaded_dlls.size() - 1);
}

API(KERNEL32, LoadLibraryA, 1)
{
	std::string name = guest_str(arg(0));
	if (const wc_module *m = module_by_name(name)) return m->base;
	return loaded_dll(name);
}

API(KERNEL32, FreeLibrary, 1) { return 1; }

API(KERNEL32, GetProcAddress, 2)
{
	uint32_t h = arg(0);
	if (arg(1) < 0x10000) return 0; /* by ordinal: none here */
	std::string name = guest_str(arg(1));
	std::string dll;
	if (h >= LOADED_DLL_BASE && (h - LOADED_DLL_BASE) / 0x10000 < loaded_dlls.size()) dll = loaded_dlls[(h - LOADED_DLL_BASE) / 0x10000];
	else return 0; /* the modules here export nothing the program looks up */
	uint32_t t = thunk_for(dll, name);
	if (!t) {
		if (wc_trace) fprintf(stderr, " [%s!%s not implemented]", dll.c_str(), name.c_str());
		set_last_error(127 /* ERROR_PROC_NOT_FOUND */);
	}
	return t;
}

API(KERNEL32, FindResourceA, 3)
{
	const wc_module *m = module_by_handle(arg(0) ? arg(0) : wc_modules[wc_module_count - 1].base);
	uint32_t size, data = find_resource(m, arg(2), arg(1), &size);
	if (!data) {
		set_last_error(ERROR_RESOURCE_NAME_NOT_FOUND);
		return 0;
	}
	/* the handle: a block holding the data's address and size */
	uint32_t h = heap_alloc(8);
	wr32(h, data);
	wr32(h + 4, size);
	return h;
}

API(KERNEL32, LoadResource, 2) { return arg(1); }
API(KERNEL32, LockResource, 1) { return arg(0) ? rd32(arg(0)) : 0; }

/* ---- time ---- */

static const auto start = std::chrono::steady_clock::now();

API(KERNEL32, QueryPerformanceFrequency, 1)
{
	mwr32(arg(0), 8, 1000000);
	return 1;
}

API(KERNEL32, QueryPerformanceCounter, 1)
{
	mwr32(arg(0), 8, (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count());
	return 1;
}

API(KERNEL32, Sleep, 1)
{
	uint32_t ms = arg(0);
	thread_sleep(ms);
	win_poll();
	return 0;
}

/* ---- synchronisation (critical sections: thread.cpp) ---- */

API(KERNEL32, CreateMutexA, 3)
{
	set_last_error(0);
	return new_handle({ kind::mutex, nullptr, {}, {} });
}
