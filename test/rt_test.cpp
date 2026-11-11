/* The generated code's helpers (runtime/wc_rt.h, cpu.cpp) on their own, without the program:
 * division as DIV/IDIV do it, x87 rounding and conversions, 80-bit values, flags, memory and the
 * shadow return stack. Built and run by test/rt.test.ts (c++ test/rt_test.cpp runtime/cpu.cpp). */
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "host.h"
#include "win.h"

/* what cpu.cpp needs of the rest of the runtime and of the generated code */
const uint32_t wc_code_count = 0;
const uint32_t wc_code_addr[] = { 0 };
void (*const wc_code_func[])(uint32_t) = { nullptr };
void host_exit(int code) { exit(code); }
void win_thunk(uint32_t) {}
void win_poll(void) {}
uint32_t win_fs_base(uint16_t selector) { return selector == FS_SELECTOR ? TIB_ADDR : 0; }

static int failed;
#define CHECK(x) \
	do { \
		if (!(x)) { \
			fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); \
			failed++; \
		} \
	} while (0)

static uint64_t d(double x) { return x87_fbits(x, 8); }
static double round_with(uint16_t cw, double x)
{
	wr(R_FPCW, 2, cw);
	return x87_fval(x87_fround(d(x), 8), 8);
}

int main(void)
{
	/* DIV and IDIV: the dividend's size, signed values sign-extended from it */
	CHECK(udiv(100, 7, 8, 0) == 14 && urem(100, 7, 0) == 2);
	CHECK(sdiv((uint64_t)-100, 7, 8, 0) == (uint64_t)-14 && srem((uint64_t)-100, 7, 8, 0) == (uint64_t)-2);
	CHECK(sdiv(0xff9c, 7, 2, 0) == (uint64_t)-14); /* -100 in 16 bits (AX / r8) */
	/* a quotient too large (x86: #DE): reported, the value left for the caller to cut */
	CHECK(udiv(1ull << 40, 2, 8, 0x1000) == 1ull << 39);
	CHECK(sdiv((uint64_t)INT64_MIN, (uint64_t)-1, 8, 0x1004) == (uint64_t)INT64_MIN); /* wraps, no trap */
	CHECK(srem((uint64_t)INT64_MIN, (uint64_t)-1, 8, 0x1004) == 0);

	/* shifts and flags */
	CHECK(shl(1, 64) == 0 && shr(1ull << 63, 64) == 0 && shl(1, 63) == 1ull << 63);
	CHECK(sar(0x80, 1, 1) == (uint64_t)-64 && sar(0x80000000, 40, 4) == (uint64_t)-1);
	CHECK(sx(0xff, 1) == -1 && sx(0x7fff, 2) == 0x7fff && sx(0x80000000, 4) == INT32_MIN);
	CHECK(carry(0xff, 1, 1) == 1 && carry(0xfe, 1, 1) == 0 && carry(0xffffffff, 1, 4) == 1);
	CHECK(scarry(0x7f, 1, 1) == 1 && scarry(0x7e, 1, 1) == 0);
	CHECK(sborrow(0x80, 1, 1) == 1 && sborrow(0x81, 1, 1) == 0);
	set_flags_word(0x0841); /* OF, ZF, CF */
	CHECK(rd(R_CF, 1) == 1 && rd(R_ZF, 1) == 1 && rd(R_OF, 1) == 1 && rd(R_SF, 1) == 0 && rd(R_DF, 1) == 0);

	/* FIST/FRNDINT round as the control word's RC bits say */
	CHECK(round_with(0x027f, 2.5) == 2 && round_with(0x027f, 3.5) == 4 && round_with(0x027f, -2.5) == -2); /* nearest, even */
	CHECK(round_with(0x067f, 2.9) == 2 && round_with(0x067f, -2.1) == -3);  /* down */
	CHECK(round_with(0x0a7f, 2.1) == 3 && round_with(0x0a7f, -2.9) == -2);  /* up */
	CHECK(round_with(0x0e7f, 2.9) == 2 && round_with(0x0e7f, -2.9) == -2);  /* chop: the C library's (int)f */
	/* to an integer: NaN and what doesn't fit store the indefinite */
	CHECK(x87_ftrunc(d(2.9), 8, 4) == 2 && (uint32_t)x87_ftrunc(d(-2.9), 8, 4) == (uint32_t)-2);
	CHECK(x87_ftrunc(d(NAN), 8, 4) == 0x80000000u && x87_ftrunc(d(3e9), 8, 4) == 0x80000000u);
	CHECK(x87_ftrunc(d(-3e9), 8, 4) == 0x80000000u && x87_ftrunc(d(40000), 8, 2) == 0x8000u);
	CHECK(x87_ftrunc(d(1e19), 8, 8) == 1ull << 63 && x87_ftrunc(d(-1e18), 8, 8) == (uint64_t)(int64_t)-1e18);
	CHECK(x87_fint((uint64_t)-5, 4, 8) == d(-5) && x87_fint(0xfffffffb, 4, 8) == d(-5));
	CHECK(x87_fcvt(d(1.5), 8, 4) == 0x3fc00000u && x87_fval(0x3fc00000u, 4) == 1.5);
	CHECK(x87_fval(x87_fscale(d(3), d(2.9), 8), 8) == 12 && x87_fval(x87_f2xm1(d(1), 8), 8) == 1);

	/* 80-bit values in memory */
	const uint32_t a = 0x20000;
	x87_st80(a, d(1.0));
	CHECK(mrd32(a, 8) == 0x8000000000000000ull && mrd32(a + 8, 2) == 0x3fff);
	for (double x : { 0.0, 1.5, -1.5, 1e300, -1e-300, 123456789.125, (double)INFINITY, -(double)INFINITY }) {
		x87_st80(a, d(x));
		CHECK(x87_fval(x87_ld80(a), 8) == x);
	}
	x87_st80(a, d(NAN));
	CHECK(std::isnan(x87_fval(x87_ld80(a), 8)));

	/* memory: little-endian, sizes; outside it (not strict): wrapped into it, reported once */
	mwr32(a, 4, 0x11223344);
	CHECK(mrd32(a, 1) == 0x44 && mrd32(a + 1, 2) == 0x2233 && mrd32(a, 4) == 0x11223344);
	CHECK(mrd32((uint64_t)MEM_SIZE + a, 4) == 0x11223344);
	wr(R_AX, 4, 0x12345678);
	CHECK(rd(R_AL, 1) == 0x78 && rd(R_AH, 1) == 0x56 && rd(R_AX, 2) == 0x5678);
	seg_load(4, FS_SELECTOR);
	CHECK(wc_sb[4] == TIB_ADDR && rd(R_FS, 2) == FS_SELECTOR);
	seg_load(3, 0x23);
	CHECK(wc_sb[3] == 0);

	/* the shadow return stack: a return to a frame further up unwinds to it */
	push_ret(0x100);
	push_ret(0x200);
	push_ret(0x300);
	CHECK(is_return(0x300) && !is_return(0x200));
	ret_to(0x300);
	CHECK(wc_unwind == -1);
	ret_to(0x100);
	CHECK(wc_unwind == 0);
	CHECK(pop_ret() && pop_ret() && !pop_ret() && wc_unwind == -1 && wc_ret_depth == 0);

	if (failed) fprintf(stderr, "%d checks failed\n", failed);
	return failed ? 1 : 0;
}
