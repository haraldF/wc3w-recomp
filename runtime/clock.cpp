/* The clock everything runs by (host.h): the host's, or a virtual one that follows the program. */
#include <chrono>
#include <thread>

#include "host.h"
#include "wc_rt.h"

namespace {

const auto start = std::chrono::steady_clock::now();
bool virt;
uint64_t per_us = 200; /* virtual: instructions a microsecond (WC_VIRTUAL_MIPS) */
uint64_t skipped;      /* virtual: the microseconds the waits took */

} // namespace

void clock_set_virtual(unsigned mips)
{
	virt = true;
	if (mips) per_us = mips;
}

bool clock_virtual(void) { return virt; }

uint64_t clock_us(void)
{
	if (virt) return skipped + wc_icount / per_us;
	return (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count();
}

void clock_wait_until(uint64_t us)
{
	if (!virt) {
		std::this_thread::sleep_until(start + std::chrono::microseconds(us));
		return;
	}
	uint64_t now = clock_us();
	if (us > now) skipped += us - now;
	host_tick();
}
