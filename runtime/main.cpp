/* Loads the program's image, sets up its Windows process (win.cpp) and runs it.
 * Usage: wc3w [game folder [game arguments...]]
 *   game folder     the Kilrathi_Saga folder (default: the current directory); it is drive C:
 *   WC_TRACE=1      trace the Windows functions the program calls; 2 also every instruction
 *   WC_POKE=addr:byte,...  patch memory before starting (hex addresses), for experiments
 *   WC_WATCH=addr   report every write to that address (hex), with the callers
 *   WC_DUMP=file    save the memory when the program stops on an error
 *   WC_STRICT=1     stop where Windows would fault: memory outside 10000h..MEM_SIZE (null pointers),
 *                   a quotient too large for DIV/IDIV (otherwise wrapped and truncated, both reported)
 * The host's (host_sdl.cpp): WC_HEADLESS, WC_KEYS, WC_SCREENSHOT, WC_VIRTUAL_TIME, WC_WAV... */
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "host.h"
#include "win.h"

static void on_signal(int sig)
{
	fprintf(stderr, "\nwc: stopped by signal %d\n", sig);
	cpu_report(stderr);
	std::_Exit(128 + sig);
}

static void game(void)
{
	win_main();
}

int main(int argc, char **argv)
{
	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);
	const char *trace = getenv("WC_TRACE");
	wc_trace = trace ? atoi(trace) : 0;
	const char *watch = getenv("WC_WATCH");
	wc_watch = watch ? (uint32_t)strtoul(watch, NULL, 16) : 0;
	const char *strict = getenv("WC_STRICT");
	wc_strict = strict && *strict && strcmp(strict, "0");

	win_start(argc > 1 ? argv[1] : ".", argc > 2 ? argc - 2 : 0, argv + 2);

	const char *poke = getenv("WC_POKE");
	for (const char *p = poke; p && *p;) {
		unsigned addr, value;
		int used = 0;
		if (sscanf(p, "%x:%x%n", &addr, &value, &used) != 2 || addr >= MEM_SIZE) {
			fprintf(stderr, "wc: bad WC_POKE at \"%s\"\n", p);
			return 1;
		}
		M[addr] = (uint8_t)value;
		p += used + (p[used] == ',');
	}
	host_run(game);
}
