/* CPU state, dispatch into the recompiled code (and to the Windows functions' thunks), failure
 * reporting, and what the generated code may call that a Windows program shouldn't need. */
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <set>
#include <vector>

#include "host.h"
#include "wc_rt.h"
#include "win.h"

uint8_t R[R_SIZE];
uint8_t M[MEM_SIZE + 16];
uint64_t wc_icount;
int wc_trace;
int wc_strict;
uint32_t wc_watch, wc_last_ins;
uint32_t wc_ret_stack[WC_RET_STACK];
int wc_ret_depth;
int wc_unwind = -1;
int wc_poll_pending;
uint32_t wc_sb[6];

static int find_code(uint32_t code)
{
	uint32_t lo = 0, hi = wc_code_count;
	while (lo < hi) {
		uint32_t mid = (lo + hi) / 2;
		if (wc_code_addr[mid] < code) {
			lo = mid + 1;
		}
		else {
			hi = mid;
		}
	}
	return lo < wc_code_count && wc_code_addr[lo] == code ? (int)lo : -1;
}

/* WC_DUMP=file: the whole memory, for looking at the program's state afterwards */
void wc_dump_memory(const char *why)
{
	const char *path = getenv("WC_DUMP");
	FILE *f = path ? fopen(path, "wb") : NULL;
	if (!f) return;
	fwrite(M, 1, MEM_SIZE, f);
	fclose(f);
	fprintf(stderr, "wc: memory saved to %s (%s)\n", path, why);
}

void ret_to(uint32_t code)
{
	if (wc_ret_depth > 0 && wc_ret_stack[wc_ret_depth - 1] == code) return;
	for (int i = wc_ret_depth - 2; i >= 0; i--) {
		if (wc_ret_stack[i] == code) {
			if (wc_trace >= 2) fprintf(stderr, "wc: return to %06x unwinds %d calls\n", code, wc_ret_depth - i - 1);
			wc_unwind = i;
			return;
		}
	}
	if (wc_trace) fprintf(stderr, "wc: return to %06x, which no call expects (at %06x)\n", code, wc_last_ins);
}

void run(uint32_t linear)
{
	if (linear >= RETURN_THUNK) {
		win_thunk(linear);
		return;
	}
	run_code(linear);
}

void run_code(uint32_t code)
{
	if (int i = find_code(code); i >= 0) {
		wc_code_func[i](code);
		return;
	}
	/* Exit code 4 + this line: "npm run discover" adds the address as an entry point. */
	fflush(stdout);
	fprintf(stderr, "wc: missing code at %06x\n", code);
	cpu_report(stderr);
	host_exit(4);
}

/* ---- segments: flat, but FS (the thread information block) ---- */

void seg_load(int reg, uint64_t selector)
{
	wr(R_ES + 2 * reg, 2, selector);
	wc_sb[reg] = reg == 4 ? win_fs_base((uint16_t)selector) : 0;
}

uint64_t seg_limit32(uint64_t selector)
{
	return (1ULL << 32) | 0xffffffffu;
}

uint64_t seg_limit(uint64_t selector)
{
	return 0x1ffff;
}

/* ---- x87 helpers ---- */

uint64_t x87_fint(uint64_t a, int from, int to) { return x87_fbits((double)sx(a, from), to); }
/* FIST and FISTP store the integer indefinite (only the sign bit set) for NaN and values out of range */
uint64_t x87_ftrunc(uint64_t a, int size, int to)
{
	double d = trunc(x87_fval(a, size)), lim = ldexp(1.0, 8 * to - 1);
	if (!(d >= -lim && d < lim)) return 1ULL << (8 * to - 1);
	return (uint64_t)(int64_t)d;
}
uint64_t x87_fabs(uint64_t a, int s) { return x87_fbits(fabs(x87_fval(a, s)), s); }
uint64_t x87_fsqrt(uint64_t a, int s) { return x87_fbits(sqrt(x87_fval(a, s)), s); }
uint64_t x87_fceil(uint64_t a, int s) { return x87_fbits(ceil(x87_fval(a, s)), s); }
uint64_t x87_ffloor(uint64_t a, int s) { return x87_fbits(floor(x87_fval(a, s)), s); }
/* FIST, FISTP and FRNDINT round as the control word's RC (bits 10-11) says: the C library's _ftol
 * (491110h) sets 11b, chop, so that (int)f truncates. */
uint64_t x87_fround(uint64_t a, int s)
{
	double d = x87_fval(a, s);
	switch ((rd(R_FPCW, 2) >> 10) & 3) {
	case 0: d = nearbyint(d); break; /* to nearest, even (the host's default) */
	case 1: d = floor(d); break;
	case 2: d = ceil(d); break;
	default: d = trunc(d); break;
	}
	return x87_fbits(d, s);
}
uint64_t x87_fnan(uint64_t a, int s) { return std::isnan(x87_fval(a, s)); }
uint64_t x87_f2xm1(uint64_t a, int s) { return x87_fbits(exp2(x87_fval(a, s)) - 1, s); }
uint64_t x87_fscale(uint64_t a, uint64_t b, int s) { return x87_fbits(ldexp(x87_fval(a, s), (int)trunc(x87_fval(b, s))), s); }
uint64_t x87_log2(uint64_t a, int s) { return x87_fbits(log2(x87_fval(a, s)), s); }

/* 80-bit extended: a 64-bit mantissa with its integer bit, a 15-bit exponent (bias 16383), the sign */
uint64_t x87_ld80(uint64_t addr)
{
	uint64_t mant = mrd32(addr, 8);
	uint32_t se = (uint32_t)mrd32(addr + 8, 2);
	int exp = (int)(se & 0x7fff);
	double d;
	if (exp == 0 && mant == 0) d = 0;
	else if (exp == 0x7fff) d = (mant << 1) ? NAN : INFINITY;
	else d = ldexp((double)mant, exp - 16383 - 63);
	return x87_fbits(se & 0x8000 ? -d : d, 8);
}

void x87_st80(uint64_t addr, uint64_t a)
{
	double d = x87_fval(a, 8);
	uint64_t mant = 0;
	uint32_t se = std::signbit(d) ? 0x8000 : 0;
	d = fabs(d);
	if (std::isnan(d)) {
		se |= 0x7fff, mant = 0xc000000000000000ull;
	}
	else if (std::isinf(d)) {
		se |= 0x7fff, mant = 0x8000000000000000ull;
	}
	else if (d != 0) {
		int exp;
		double m = frexp(d, &exp); /* d = m * 2^exp, 0.5 <= m < 1 */
		mant = (uint64_t)ldexp(m, 64);
		se |= (uint32_t)(exp - 1 + 16383);
	}
	mwr32(addr, 8, mant);
	mwr32(addr + 8, 2, se);
}

static const struct { int reg, bit; } flag_bits[] = {
	{ R_CF, 0 }, { R_PF, 2 }, { R_AF, 4 }, { R_ZF, 6 }, { R_SF, 7 },
	{ R_TF, 8 }, { R_IF, 9 }, { R_DF, 10 }, { R_OF, 11 },
};

void set_flags_word(uint16_t f)
{
	for (const auto &b : flag_bits) {
		wr(b.reg, 1, (f >> b.bit) & 1);
	}
}

/* ---- what a Windows program has no business doing ---- */

void cpu_int(uint64_t n, uint32_t at, uint32_t next)
{
	char what[64];
	snprintf(what, sizeof what, "INT %02llxh at", (unsigned long long)n);
	cpu_fail(what, at);
}

uint64_t port_in(uint64_t port, int size)
{
	cpu_fail("IN from a port at", wc_last_ins);
}

void port_out(uint64_t port, uint64_t value, int size)
{
	cpu_fail("OUT to a port at", wc_last_ins);
}

void cpu_poll(uint32_t at)
{
	wc_poll_pending = 0;
	win_poll();
}

/* ---- reports ---- */

static void dump_regs(FILE *f)
{
	fprintf(f, "  EAX=%08llx EBX=%08llx ECX=%08llx EDX=%08llx ESI=%08llx EDI=%08llx EBP=%08llx ESP=%08llx\n",
		rd(R_AX, 4), rd(R_BX, 4), rd(R_CX, 4), rd(R_DX, 4), rd(R_SI, 4), rd(R_DI, 4), rd(R_BP, 4), rd(R_SP, 4));
	fprintf(f, "  CF=%llu ZF=%llu SF=%llu OF=%llu DF=%llu FS base=%08x\n", rd(R_CF, 1), rd(R_ZF, 1), rd(R_SF, 1),
		rd(R_OF, 1), rd(R_DF, 1), wc_sb[4]);
}

static void print_callers(FILE *f)
{
	fprintf(f, "  called from (innermost first, code addresses after each call):");
	for (int i = wc_ret_depth - 1; i >= 0 && i >= wc_ret_depth - 8; i--) {
		fprintf(f, " %06x", wc_ret_stack[i]);
	}
	fprintf(f, "\n");
}

void cpu_report(FILE *f)
{
	fprintf(f, "  %llu instructions executed, last at %06x\n", (unsigned long long)wc_icount, wc_last_ins);
	dump_regs(f);
	print_callers(f);
}

[[noreturn]] void cpu_fail(const char *what, uint32_t at)
{
	fflush(stdout);
	fprintf(stderr, "wc: %s %06x\n", what, at);
	cpu_report(stderr);
	host_exit(3);
}

[[noreturn]] void cpu_div0(uint32_t at)
{
	cpu_fail("divide error at", at);
}

void cpu_div_overflow(uint32_t at)
{
	static std::set<uint32_t> seen;
	if (wc_strict) cpu_fail("divide overflow (the quotient doesn't fit) at", at);
	if (!seen.insert(at).second) return;
	fprintf(stderr, "wc: divide overflow at %06x: the quotient truncated (x86 would fault)\n", at);
	print_callers(stderr);
}

uint32_t mem_outside(uint32_t addr, int size, bool write)
{
	if (wc_strict) {
		char what[96];
		snprintf(what, sizeof what, "%s of %d bytes at %08x, outside memory, by instruction", write ? "write" : "read",
			size, addr);
		cpu_fail(what, wc_last_ins);
	}
	/* reported once for each instruction; the mixer's thread reads memory too */
	static std::mutex lock;
	static std::set<uint32_t> seen;
	uint32_t at = wc_last_ins;
	bool first;
	{
		std::lock_guard<std::mutex> g(lock);
		first = seen.insert(at).second;
	}
	if (first) {
		fprintf(stderr, "wc: %s of %d bytes at %08x, outside memory, by instruction %06x: wrapped (Windows would fault)\n",
			write ? "write" : "read", size, addr, at);
		print_callers(stderr);
	}
	return addr & (MEM_SIZE - 1);
}

/* WC_TRACE_AT=addr,lo-hi,...: trace only those instructions (hi exclusive), with the x87 registers */
static std::vector<std::pair<uint32_t, uint32_t>> trace_at;
static bool trace_at_read;

void trace_ins(uint32_t at)
{
	if (!trace_at_read) {
		trace_at_read = true;
		for (const char *p = getenv("WC_TRACE_AT"); p && *p;) {
			char *end;
			uint32_t lo = (uint32_t)strtoul(p, &end, 16), hi = lo + 1;
			if (*end == '-') hi = (uint32_t)strtoul(end + 1, &end, 16);
			trace_at.push_back({ lo, hi });
			p = *end ? end + 1 : end;
		}
	}
	if (!trace_at.empty()) {
		if (std::none_of(trace_at.begin(), trace_at.end(), [at](auto r) { return at >= r.first && at < r.second; })) return;
		fprintf(stderr, "  %06x  EAX=%08llx ECX=%08llx EDX=%08llx ESP=%08llx ST0=%g ST1=%g ST2=%g CW=%04llx\n", at, rd(R_AX, 4),
			rd(R_CX, 4), rd(R_DX, 4), rd(R_SP, 4), x87_fval(rd(0x1100, 8), 8), x87_fval(rd(0x1110, 8), 8),
			x87_fval(rd(0x1120, 8), 8), rd(R_FPCW, 2));
		return;
	}
	fprintf(stderr, "  %06x  EAX=%08llx EBX=%08llx ECX=%08llx EDX=%08llx ESI=%08llx EDI=%08llx ESP=%08llx EBP=%08llx\n",
		at, rd(R_AX, 4), rd(R_BX, 4), rd(R_CX, 4), rd(R_DX, 4), rd(R_SI, 4), rd(R_DI, 4), rd(R_SP, 4), rd(R_BP, 4));
}

void watch_hit(uint32_t addr, int size, uint64_t value)
{
	fprintf(stderr, "wc: write %0*llx to %06x (watch %06x) by instruction %06x\n", size * 2,
		(unsigned long long)value, addr, wc_watch, wc_last_ins);
	print_callers(stderr);
}
