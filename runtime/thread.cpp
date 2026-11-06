/* The program's threads. Each runs on a host thread of its own (its C stack holds the recompiled
 * code's frames), but one at a time, like a single processor: the CPU state is global (R[], the
 * shadow return stack, the segment bases), so a switch saves the running thread's and restores the
 * next one's. Threads switch when one sleeps or waits (Sleep, a critical section another thread
 * holds, an empty message queue) and every 65536 instructions otherwise (cpu_poll), so that a
 * thread spinning on a flag lets the others run. */
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>

#include "host.h"
#include "win.h"

namespace {

using clock = std::chrono::steady_clock;

struct wthread {
	uint32_t handle, id, tib, start, param;
	uint8_t regs[sizeof R];
	std::vector<uint32_t> ret;
	int unwind = -1;
	uint32_t sb[6];
	uint32_t last_ins = 0;
	bool dead = false;    /* terminated, or returned */
	clock::time_point wake{}; /* sleeping until then */
};

std::vector<wthread *> threads;
size_t current;
std::mutex lock;
std::condition_variable turn;
struct thread_exit {}; /* thrown on a terminated thread's host thread, to end it */

void save(wthread *t)
{
	memcpy(t->regs, R, sizeof R);
	t->ret.assign(wc_ret_stack, wc_ret_stack + wc_ret_depth);
	t->unwind = wc_unwind;
	memcpy(t->sb, wc_sb, sizeof wc_sb);
	t->last_ins = wc_last_ins;
}

void restore(wthread *t)
{
	memcpy(R, t->regs, sizeof R);
	std::copy(t->ret.begin(), t->ret.end(), wc_ret_stack);
	wc_ret_depth = (int)t->ret.size();
	wc_unwind = t->unwind;
	memcpy(wc_sb, t->sb, sizeof wc_sb);
	wc_last_ins = t->last_ins;
}

/* Hands the processor to thread i and waits for it to come back (or for being terminated). */
void switch_to(size_t i)
{
	wthread *me = threads[current];
	save(me);
	std::unique_lock<std::mutex> g(lock);
	current = i;
	turn.notify_all();
	turn.wait(g, [&] { return threads[current] == me || me->dead; });
	if (me->dead) throw thread_exit{};
	restore(me);
}

bool runnable(const wthread *t, clock::time_point now) { return !t->dead && t->wake <= now; }

/* Lets another runnable thread run; sleeping (until this thread's wake time): until then */
void schedule(bool sleeping)
{
	for (;;) {
		auto now = clock::now();
		for (size_t k = 1; k < threads.size(); k++) {
			size_t i = (current + k) % threads.size();
			if (runnable(threads[i], now)) {
				switch_to(i);
				return;
			}
		}
		wthread *me = threads[current];
		if (!sleeping || me->wake <= now) return;
		/* nobody can run: sleep until the first one wakes */
		auto first = me->wake;
		for (const wthread *t : threads) {
			if (!t->dead && t->wake > now) first = std::min(first, t->wake);
		}
		std::this_thread::sleep_until(first);
	}
}

/* The host thread of a program's thread: waits for its turn, runs the thread function, ends. */
void thread_main(void *p)
{
	wthread *me = (wthread *)p;
	{
		std::unique_lock<std::mutex> g(lock);
		turn.wait(g, [&] { return threads[current] == me || me->dead; });
	}
	if (me->dead) return;
	restore(me);
	try {
		uint32_t code = win_call(me->start, { me->param });
		if (wc_trace) fprintf(stderr, "\nwc: thread %x ended (%x)\n", me->id, code);
	}
	catch (thread_exit &) {
		return; /* terminated while waiting for its turn: another thread runs */
	}
	me->dead = true;
	save(me);
	/* hand over to the next thread that can run, sleeping until one can */
	for (;;) {
		auto now = clock::now();
		auto first = clock::time_point::max();
		for (size_t i = 0; i < threads.size(); i++) {
			if (runnable(threads[i], now)) {
				std::lock_guard<std::mutex> g(lock);
				current = i;
				turn.notify_all();
				return;
			}
			if (!threads[i]->dead) first = std::min(first, threads[i]->wake);
		}
		if (first == clock::time_point::max()) host_exit(0);
		std::this_thread::sleep_until(first);
	}
}

wthread *by_handle(uint32_t h)
{
	for (wthread *t : threads) {
		if (t->handle == h) return t;
	}
	return nullptr;
}

/* A thread's information block: a page in the heap */
uint32_t new_tib(uint32_t id, uint32_t stack_lo, uint32_t stack_hi)
{
	uint32_t tib = heap_alloc(0x100);
	wr32(tib + 0x00, 0xffffffff);
	wr32(tib + 0x04, stack_hi);
	wr32(tib + 0x08, stack_lo);
	wr32(tib + 0x18, tib);
	wr32(tib + 0x20, 0x100);
	wr32(tib + 0x24, id);
	wr32(tib + 0x30, PEB_ADDR);
	return tib;
}

} // namespace

/* The main thread: the one win_start set up */
void thread_init_main(void)
{
	wthread *t = new wthread{};
	t->handle = 0xfffffffe;
	t->id = rd32(TIB_ADDR + 0x24);
	t->tib = TIB_ADDR;
	threads.push_back(t);
	current = 0;
}

uint32_t thread_tib(void) { return threads.empty() ? TIB_ADDR : threads[current]->tib; }
uint32_t thread_id(void) { return threads.empty() ? rd32(TIB_ADDR + 0x24) : threads[current]->id; }
bool thread_close(uint32_t h) { return by_handle(h) != nullptr; }

void thread_yield(void)
{
	if (threads.size() > 1) schedule(false);
}

void thread_sleep(uint32_t ms)
{
	threads[current]->wake = clock::now() + std::chrono::milliseconds(ms);
	schedule(true);
}

void thread_poll(void)
{
	if (threads.size() > 1) schedule(false);
}

API(KERNEL32, CreateThread, 6)
{
	uint32_t stack_size = std::max(arg(1), 0x100000u), start = arg(2), param = arg(3), flags = arg(4);
	if (flags & 4 /* CREATE_SUSPENDED */) cpu_fail("CreateThread with CREATE_SUSPENDED from", rd32((uint32_t)rd(R_SP, 4)));
	uint32_t stack = region_alloc(0, stack_size);
	if (!stack) {
		set_last_error(ERROR_NOT_ENOUGH_MEMORY);
		return 0;
	}
	static uint32_t next_id = 0x108;
	wthread *t = new wthread{};
	t->id = next_id;
	t->handle = 0x2000 + next_id;
	next_id += 4;
	t->start = start;
	t->param = param;
	t->tib = new_tib(t->id, stack, stack + stack_size);
	/* its CPU state: this one's, with its own stack and FS */
	save(t);
	t->ret.clear();
	t->unwind = -1;
	t->sb[4] = t->tib;
	uint32_t esp = stack + stack_size - 16;
	memcpy(t->regs + R_SP, &esp, 4);
	uint16_t cw = 0x27f;
	memcpy(t->regs + R_FPCW, &cw, 2);
	threads.push_back(t);
	if (arg(5)) wr32(arg(5), t->id);
	if (wc_trace) fprintf(stderr, " [thread %x at %x (%x), stack %x..%x]", t->id, start, param, stack, stack + stack_size);
	host_thread(thread_main, t);
	return t->handle;
}

API(KERNEL32, TerminateThread, 2)
{
	wthread *t = by_handle(arg(0));
	if (!t) return 0;
	if (wc_trace) fprintf(stderr, " [thread %x terminated]", t->id);
	bool self = t == threads[current];
	{
		std::lock_guard<std::mutex> g(lock);
		t->dead = true;
		turn.notify_all();
	}
	if (self) {
		/* not done by the game so far */
		cpu_fail("a thread terminated itself, from", rd32((uint32_t)rd(R_SP, 4)));
	}
	return 1;
}

/* Critical sections: owned by a thread (OwningThread at +12, RecursionCount at +8); entering one
 * another thread holds lets the others run until it's free. */
API(KERNEL32, InitializeCriticalSection, 1)
{
	memset(M + arg(0), 0, 24);
	wr32(arg(0) + 4, 0xffffffff); /* LockCount */
	return 0;
}

API(KERNEL32, EnterCriticalSection, 1)
{
	uint32_t cs = arg(0), me = thread_id();
	while (rd32(cs + 12) && rd32(cs + 12) != me) {
		uint32_t esp = (uint32_t)rd(R_SP, 4);
		thread_yield();
		wr(R_SP, 4, esp);
		if (threads.size() == 1) cpu_fail("a critical section held by a thread that is gone, entered from", rd32(esp));
	}
	wr32(cs + 12, me);
	wr32(cs + 8, rd32(cs + 8) + 1);
	return 0;
}

API(KERNEL32, LeaveCriticalSection, 1)
{
	uint32_t cs = arg(0);
	if (rd32(cs + 8) && --*(uint32_t *)(M + cs + 8) == 0) wr32(cs + 12, 0);
	return 0;
}
