/* The host with SDL3: a window showing DirectDraw's picture (640x480, 8 bits) at 4:3, the keyboard
 * as Windows key messages, the mouse as mouse messages (relative: the game hides the pointer and
 * draws its own, and moves it with SetCursorPos). The program runs on threads of its own with
 * stacks as large as the recompiled code needs; the main thread serves the window.
 * Cmd+F toggles full screen, Cmd+Q or closing the window quits.
 * WC_KEYS=ms:key,...: keys and clicks at those times (ms after the start): "esc", "enter", "space",
 * "tab", "up", "f1", a letter or digit (pressed for 50 ms; "+key" only presses, "-key" only
 * releases), "lmb"/"rmb" (a click), "@x.y" (the mouse to x, y of 640x480).
 * WC_WAV=file: record the sound (also headless).
 * WC_HEADLESS=1: no window. WC_SCREENSHOT=file.png: the picture after WC_SCREENSHOT_AT seconds
 * (default 10), then the program ends (also headless); with WC_DUMP=file the memory too. */
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <SDL3/SDL.h>

#include "host.h"

uint64_t ddraw_frame(uint8_t *pixels, uint8_t palette[256 * 3], int *w, int *h);
void win_input(uint32_t message, uint32_t wparam, uint32_t lparam); /* user32.cpp */
void win_mouse(int dx, int dy, int buttons);                       /* user32.cpp */
void win_mouse_at(int x, int y);                                   /* user32.cpp */
void wc_dump_memory(const char *why);                              /* cpu.cpp: WC_DUMP */
void sound_mix(int16_t *out, unsigned frames);                     /* sound.cpp */
static const unsigned MIX_RATE = 44100;

static SDL_Window *window;
static SDL_Renderer *renderer;
static SDL_Texture *picture;
static void (*game_fn)(void);

const char *host_base_dir(void)
{
	const char *dir = SDL_GetBasePath();
	return dir ? dir : "./";
}

/* ---- keyboard: SDL scancodes (USB HID positions) to Windows virtual keys and PC set 1 ---- */

static struct { uint8_t vk, scan, ext; } keys[SDL_SCANCODE_COUNT];

static void init_keys(void)
{
	static const struct { SDL_Scancode sdl; uint8_t vk, scan, ext; } map[] = {
		{ SDL_SCANCODE_ESCAPE, 0x1b, 0x01, 0 }, { SDL_SCANCODE_MINUS, 0xbd, 0x0c, 0 }, { SDL_SCANCODE_EQUALS, 0xbb, 0x0d, 0 },
		{ SDL_SCANCODE_BACKSPACE, 0x08, 0x0e, 0 }, { SDL_SCANCODE_TAB, 0x09, 0x0f, 0 }, { SDL_SCANCODE_LEFTBRACKET, 0xdb, 0x1a, 0 },
		{ SDL_SCANCODE_RIGHTBRACKET, 0xdd, 0x1b, 0 }, { SDL_SCANCODE_RETURN, 0x0d, 0x1c, 0 }, { SDL_SCANCODE_LCTRL, 0x11, 0x1d, 0 },
		{ SDL_SCANCODE_RCTRL, 0x11, 0x1d, 1 }, { SDL_SCANCODE_SEMICOLON, 0xba, 0x27, 0 }, { SDL_SCANCODE_APOSTROPHE, 0xde, 0x28, 0 },
		{ SDL_SCANCODE_GRAVE, 0xc0, 0x29, 0 }, { SDL_SCANCODE_LSHIFT, 0x10, 0x2a, 0 }, { SDL_SCANCODE_BACKSLASH, 0xdc, 0x2b, 0 },
		{ SDL_SCANCODE_COMMA, 0xbc, 0x33, 0 }, { SDL_SCANCODE_PERIOD, 0xbe, 0x34, 0 }, { SDL_SCANCODE_SLASH, 0xbf, 0x35, 0 },
		{ SDL_SCANCODE_RSHIFT, 0x10, 0x36, 0 }, { SDL_SCANCODE_KP_MULTIPLY, 0x6a, 0x37, 0 }, { SDL_SCANCODE_LALT, 0x12, 0x38, 0 },
		{ SDL_SCANCODE_RALT, 0x12, 0x38, 1 }, { SDL_SCANCODE_SPACE, 0x20, 0x39, 0 }, { SDL_SCANCODE_CAPSLOCK, 0x14, 0x3a, 0 },
		{ SDL_SCANCODE_NUMLOCKCLEAR, 0x90, 0x45, 0 }, { SDL_SCANCODE_SCROLLLOCK, 0x91, 0x46, 0 },
		{ SDL_SCANCODE_KP_7, 0x67, 0x47, 0 }, { SDL_SCANCODE_KP_8, 0x68, 0x48, 0 }, { SDL_SCANCODE_KP_9, 0x69, 0x49, 0 },
		{ SDL_SCANCODE_KP_MINUS, 0x6d, 0x4a, 0 }, { SDL_SCANCODE_KP_4, 0x64, 0x4b, 0 }, { SDL_SCANCODE_KP_5, 0x65, 0x4c, 0 },
		{ SDL_SCANCODE_KP_6, 0x66, 0x4d, 0 }, { SDL_SCANCODE_KP_PLUS, 0x6b, 0x4e, 0 }, { SDL_SCANCODE_KP_1, 0x61, 0x4f, 0 },
		{ SDL_SCANCODE_KP_2, 0x62, 0x50, 0 }, { SDL_SCANCODE_KP_3, 0x63, 0x51, 0 }, { SDL_SCANCODE_KP_0, 0x60, 0x52, 0 },
		{ SDL_SCANCODE_KP_PERIOD, 0x6e, 0x53, 0 }, { SDL_SCANCODE_KP_ENTER, 0x0d, 0x1c, 1 }, { SDL_SCANCODE_KP_DIVIDE, 0x6f, 0x35, 1 },
		{ SDL_SCANCODE_F11, 0x7a, 0x57, 0 }, { SDL_SCANCODE_F12, 0x7b, 0x58, 0 },
		{ SDL_SCANCODE_HOME, 0x24, 0x47, 1 }, { SDL_SCANCODE_UP, 0x26, 0x48, 1 }, { SDL_SCANCODE_PAGEUP, 0x21, 0x49, 1 },
		{ SDL_SCANCODE_LEFT, 0x25, 0x4b, 1 }, { SDL_SCANCODE_RIGHT, 0x27, 0x4d, 1 }, { SDL_SCANCODE_END, 0x23, 0x4f, 1 },
		{ SDL_SCANCODE_DOWN, 0x28, 0x50, 1 }, { SDL_SCANCODE_PAGEDOWN, 0x22, 0x51, 1 }, { SDL_SCANCODE_INSERT, 0x2d, 0x52, 1 },
		{ SDL_SCANCODE_DELETE, 0x2e, 0x53, 1 },
	};
	for (const auto &m : map) keys[m.sdl] = { m.vk, m.scan, m.ext };
	static const char rows[] = "1234567890\0\0\0\0qwertyuiop\0\0\0\0asdfghjkl\0\0\0\0\0zxcvbnm";
	for (size_t i = 0; i < sizeof rows - 1; i++) {
		char c = rows[i];
		if (c >= 'a' && c <= 'z') keys[SDL_SCANCODE_A + (c - 'a')] = { (uint8_t)(c - 'a' + 'A'), (uint8_t)(0x02 + i), 0 };
		else if (c >= '1' && c <= '9') keys[SDL_SCANCODE_1 + (c - '1')] = { (uint8_t)c, (uint8_t)(0x02 + i), 0 };
		else if (c == '0') keys[SDL_SCANCODE_0] = { '0', (uint8_t)(0x02 + i), 0 };
	}
	for (int i = 0; i < 10; i++) keys[SDL_SCANCODE_F1 + i] = { (uint8_t)(0x70 + i), (uint8_t)(0x3b + i), 0 };
}

static bool held[SDL_SCANCODE_COUNT];

static void key(const SDL_KeyboardEvent *e)
{
	if (e->mod & SDL_KMOD_GUI) { /* Cmd: for the Mac, not the game */
		if (e->down && !e->repeat && e->scancode == SDL_SCANCODE_F) {
			SDL_SetWindowFullscreen(window, !(SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN));
		}
		if (e->down && !e->repeat && e->scancode == SDL_SCANCODE_Q) host_exit(0);
		return;
	}
	const auto &k = keys[e->scancode];
	if (!k.vk) return;
	bool alt = (e->mod & SDL_KMOD_ALT) || k.vk == 0x12;
	uint32_t lparam = 1 | (uint32_t)k.scan << 16 | (uint32_t)k.ext << 24 | (alt ? 1u << 29 : 0);
	if (e->down) {
		if (held[e->scancode]) lparam |= 1u << 30; /* a repeat */
		held[e->scancode] = true;
		win_input(alt || k.vk == 0x79 ? 0x104 : 0x100, k.vk, lparam); /* WM_SYSKEYDOWN, WM_KEYDOWN */
	}
	else if (held[e->scancode]) {
		held[e->scancode] = false;
		win_input(alt ? 0x105 : 0x101, k.vk, lparam | 3u << 30); /* WM_SYSKEYUP, WM_KEYUP */
	}
}

/* ---- WC_KEYS ---- */

struct scripted {
	int ms;
	std::string what;
	int phase; /* 1 press, 2 release, 3 both */
};
static std::vector<scripted> script;

static void read_script(void)
{
	const char *s = getenv("WC_KEYS");
	for (const char *p = s; p && *p;) {
		const char *comma = strchr(p, ',');
		std::string item(p, comma ? (size_t)(comma - p) : strlen(p));
		p = comma ? comma + 1 : p + item.size();
		size_t colon = item.find(':');
		if (colon == std::string::npos) continue;
		scripted k{ atoi(item.c_str()), item.substr(colon + 1), 3 };
		if (k.what[0] == '+' || k.what[0] == '-') {
			k.phase = k.what[0] == '+' ? 1 : 2;
			k.what = k.what.substr(1);
		}
		if (k.phase == 3) {
			script.push_back({ k.ms, k.what, 1 });
			script.push_back({ k.ms + 50, k.what, 2 });
		}
		else {
			script.push_back(k);
		}
	}
	std::stable_sort(script.begin(), script.end(), [](const scripted &a, const scripted &b) { return a.ms < b.ms; });
}

static SDL_Scancode scancode_of(const std::string &name)
{
	static const struct { const char *name; SDL_Scancode code; } names[] = {
		{ "esc", SDL_SCANCODE_ESCAPE }, { "enter", SDL_SCANCODE_RETURN }, { "space", SDL_SCANCODE_SPACE },
		{ "tab", SDL_SCANCODE_TAB }, { "up", SDL_SCANCODE_UP }, { "down", SDL_SCANCODE_DOWN }, { "left", SDL_SCANCODE_LEFT },
		{ "right", SDL_SCANCODE_RIGHT }, { "alt", SDL_SCANCODE_LALT }, { "ctrl", SDL_SCANCODE_LCTRL },
		{ "shift", SDL_SCANCODE_LSHIFT }, { "backspace", SDL_SCANCODE_BACKSPACE },
	};
	for (const auto &n : names) {
		if (name == n.name) return n.code;
	}
	if (name.size() >= 2 && name[0] == 'f' && isdigit((unsigned char)name[1])) return (SDL_Scancode)(SDL_SCANCODE_F1 + atoi(name.c_str() + 1) - 1);
	if (name.size() == 1 && name[0] >= 'a' && name[0] <= 'z') return (SDL_Scancode)(SDL_SCANCODE_A + (name[0] - 'a'));
	if (name.size() == 1 && name[0] >= '1' && name[0] <= '9') return (SDL_Scancode)(SDL_SCANCODE_1 + (name[0] - '1'));
	if (name == "0") return SDL_SCANCODE_0;
	return SDL_SCANCODE_UNKNOWN;
}

static void play_script(int now_ms, int *buttons)
{
	static size_t next;
	while (next < script.size() && script[next].ms <= now_ms) {
		const scripted &k = script[next++];
		if (k.what[0] == '@') {
			int x = 0, y = 0;
			sscanf(k.what.c_str() + 1, "%d.%d", &x, &y);
			win_mouse_at(x, y);
			continue;
		}
		if (k.what == "lmb" || k.what == "rmb") {
			int bit = k.what == "lmb" ? 1 : 2;
			*buttons = k.phase == 1 ? *buttons | bit : *buttons & ~bit;
			win_mouse(0, 0, *buttons);
			continue;
		}
		SDL_KeyboardEvent e{};
		e.scancode = scancode_of(k.what);
		e.down = k.phase == 1;
		if (e.scancode == SDL_SCANCODE_UNKNOWN) {
			fprintf(stderr, "wc: WC_KEYS: unknown key \"%s\"\n", k.what.c_str());
			continue;
		}
		key(&e);
	}
}

/* ---- the picture ---- */

static uint8_t pixels[640 * 480], palette[256 * 3];
static int frame_w = 640, frame_h = 480;

static void show_frame(void)
{
	static uint64_t shown = ~0ull;
	uint64_t n = ddraw_frame(pixels, palette, &frame_w, &frame_h);
	if (n != shown) {
		shown = n;
		uint32_t *dst;
		int pitch;
		if (SDL_LockTexture(picture, NULL, (void **)&dst, &pitch)) {
			for (int y = 0; y < frame_h; y++) {
				uint32_t *row = (uint32_t *)((uint8_t *)dst + y * pitch);
				for (int x = 0; x < frame_w; x++) {
					uint8_t p = pixels[y * frame_w + x];
					row[x] = 0xff000000u | (uint32_t)palette[3 * p] << 16 | (uint32_t)palette[3 * p + 1] << 8 | palette[3 * p + 2];
				}
			}
			SDL_UnlockTexture(picture);
		}
	}
	int ww, wh;
	SDL_GetRenderOutputSize(renderer, &ww, &wh);
	float scale = SDL_min(ww / 640.0f, wh / 480.0f);
	SDL_FRect r = { (ww - 640 * scale) / 2, (wh - 480 * scale) / 2, 640 * scale, 480 * scale };
	SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
	SDL_RenderClear(renderer);
	SDL_FRect src = { 0, 0, (float)frame_w, (float)frame_h };
	SDL_RenderTexture(renderer, picture, &src, &r);
	SDL_RenderPresent(renderer);
}

/* ---- screenshots: PNG, uncompressed ---- */

static uint32_t crc32(const uint8_t *p, size_t n)
{
	uint32_t c = 0xffffffffu;
	for (size_t i = 0; i < n; i++) {
		c ^= p[i];
		for (int k = 0; k < 8; k++) c = c >> 1 ^ (0xedb88320u & (0 - (c & 1)));
	}
	return ~c;
}

static void put32(std::vector<uint8_t> &v, uint32_t x)
{
	for (int s = 24; s >= 0; s -= 8) v.push_back((uint8_t)(x >> s));
}

static void chunk(FILE *f, const char *type, const std::vector<uint8_t> &data)
{
	std::vector<uint8_t> c;
	put32(c, (uint32_t)data.size());
	c.insert(c.end(), type, type + 4);
	c.insert(c.end(), data.begin(), data.end());
	put32(c, crc32(c.data() + 4, c.size() - 4));
	fwrite(c.data(), 1, c.size(), f);
}

static void screenshot(const char *path)
{
	ddraw_frame(pixels, palette, &frame_w, &frame_h);
	FILE *f = fopen(path, "wb");
	if (!f) return;
	fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
	std::vector<uint8_t> h;
	put32(h, (uint32_t)frame_w);
	put32(h, (uint32_t)frame_h);
	h.insert(h.end(), { 8, 3, 0, 0, 0 }); /* 8 bits, indexed */
	chunk(f, "IHDR", h);
	chunk(f, "PLTE", std::vector<uint8_t>(palette, palette + 768));
	/* zlib with stored blocks, a filter byte (0) before each row */
	std::vector<uint8_t> raw;
	for (int y = 0; y < frame_h; y++) {
		raw.push_back(0);
		raw.insert(raw.end(), pixels + y * frame_w, pixels + (y + 1) * frame_w);
	}
	std::vector<uint8_t> z = { 0x78, 0x01 };
	uint32_t a = 1, b = 0;
	for (uint8_t x : raw) a = (a + x) % 65521, b = (b + a) % 65521;
	for (size_t i = 0; i < raw.size(); i += 65535) {
		size_t n = std::min<size_t>(65535, raw.size() - i);
		z.push_back(i + n == raw.size());
		z.push_back((uint8_t)n), z.push_back((uint8_t)(n >> 8));
		z.push_back((uint8_t)~n), z.push_back((uint8_t)(~n >> 8));
		z.insert(z.end(), raw.begin() + (long)i, raw.begin() + (long)(i + n));
	}
	put32(z, b << 16 | a);
	chunk(f, "IDAT", z);
	chunk(f, "IEND", {});
	fclose(f);
	fprintf(stderr, "wc: screenshot saved to %s\n", path);
}

/* ---- sound: the mixer's output to SDL's audio stream (headless: consumed in real time), and
 * to WC_WAV ---- */

static FILE *wav;
static uint32_t wav_bytes;

static void wav_header(void)
{
	auto le = [](uint8_t *p, uint32_t v, int n) { for (int i = 0; i < n; i++) p[i] = (uint8_t)(v >> 8 * i); };
	uint8_t h[44] = { 'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 16, 0, 0, 0, 1, 0, 2, 0 };
	le(h + 4, 36 + wav_bytes, 4);
	le(h + 24, MIX_RATE, 4);
	le(h + 28, MIX_RATE * 4, 4);
	le(h + 32, 4, 2);
	le(h + 34, 16, 2);
	memcpy(h + 36, "data", 4);
	le(h + 40, wav_bytes, 4);
	fseek(wav, 0, SEEK_SET);
	fwrite(h, 1, 44, wav);
	fseek(wav, 0, SEEK_END);
}

static void produce(int16_t *buf, unsigned frames)
{
	sound_mix(buf, frames);
	if (wav) {
		fwrite(buf, 4, frames, wav);
		wav_bytes += 4 * frames;
	}
}

static void SDLCALL audio_callback(void *, SDL_AudioStream *stream, int additional, int)
{
	static std::vector<int16_t> buf;
	unsigned frames = (unsigned)additional / 4;
	if (!frames) return;
	buf.resize(2 * (size_t)frames);
	produce(buf.data(), frames);
	SDL_PutAudioStreamData(stream, buf.data(), (int)(4 * frames));
}

static bool open_audio(void)
{
	if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) return false;
	SDL_AudioSpec spec = { SDL_AUDIO_S16, 2, (int)MIX_RATE };
	SDL_AudioStream *s = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, audio_callback, nullptr);
	if (!s) {
		fprintf(stderr, "wc: no sound (%s)\n", SDL_GetError());
		return false;
	}
	SDL_ResumeAudioStreamDevice(s);
	return true;
}

/* Without an audio device: the sound is consumed in real time all the same */
static void quiet_audio(void)
{
	std::thread([] {
		auto t = std::chrono::steady_clock::now();
		uint64_t done = 0;
		std::vector<int16_t> buf;
		for (;;) {
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
			uint64_t due = (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t).count() *
				MIX_RATE / 1000000;
			unsigned frames = (unsigned)(due - done);
			buf.resize(2 * (size_t)frames);
			produce(buf.data(), frames);
			done = due;
		}
	}).detach();
}

/* ---- threads ---- */

static int game_thread(void *)
{
	game_fn();
	host_exit(0);
}

static SDL_Thread *start_thread(SDL_ThreadFunction fn, void *arg, const char *name)
{
	SDL_PropertiesID props = SDL_CreateProperties();
	SDL_SetPointerProperty(props, SDL_PROP_THREAD_CREATE_ENTRY_FUNCTION_POINTER, (void *)fn);
	SDL_SetPointerProperty(props, SDL_PROP_THREAD_CREATE_USERDATA_POINTER, arg);
	SDL_SetStringProperty(props, SDL_PROP_THREAD_CREATE_NAME_STRING, name);
	SDL_SetNumberProperty(props, SDL_PROP_THREAD_CREATE_STACKSIZE_NUMBER, 256 << 20);
	SDL_Thread *t = SDL_CreateThreadWithProperties(props);
	SDL_DestroyProperties(props);
	if (!t) {
		fprintf(stderr, "wc: cannot start a thread: %s\n", SDL_GetError());
		exit(1);
	}
	return t;
}

struct thread_start {
	void (*fn)(void *);
	void *arg;
};

static int other_thread(void *p)
{
	thread_start s = *(thread_start *)p;
	delete (thread_start *)p;
	s.fn(s.arg);
	return 0;
}

void host_thread(void (*fn)(void *), void *arg)
{
	SDL_DetachThread(start_thread(other_thread, new thread_start{ fn, arg }, "thread"));
}

static bool open_window(void)
{
	const char *headless = getenv("WC_HEADLESS");
	if (headless && *headless && *headless != '0') return false;
	if (!SDL_Init(SDL_INIT_VIDEO)) {
		fprintf(stderr, "wc: no window (%s); running headless\n", SDL_GetError());
		return false;
	}
	if (!SDL_CreateWindowAndRenderer("Wing Commander III", 960, 720, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY,
		    &window, &renderer)) {
		fprintf(stderr, "wc: no window (%s); running headless\n", SDL_GetError());
		SDL_Quit();
		return false;
	}
	SDL_SetRenderVSync(renderer, 1);
	picture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STREAMING, 640, 480);
	SDL_SetTextureScaleMode(picture, SDL_SCALEMODE_LINEAR);
	SDL_SetWindowRelativeMouseMode(window, true);
	return true;
}

[[noreturn]] void host_run(void (*game)(void))
{
	game_fn = game;
	init_keys();
	read_script();
	bool shown = open_window();
	if (const char *w = getenv("WC_WAV")) {
		wav = fopen(w, "wb");
		if (wav) wav_header();
	}
	if (!shown || !open_audio()) quiet_audio();
	start_thread(game_thread, nullptr, "game");
	auto started = std::chrono::steady_clock::now();
	const char *shot = getenv("WC_SCREENSHOT");
	const char *at = getenv("WC_SCREENSHOT_AT");
	auto shot_time = std::chrono::steady_clock::now() + std::chrono::milliseconds((int)(1000 * (at ? atof(at) : 10)));
	int buttons = 0;
	for (;;) {
		if (shot && std::chrono::steady_clock::now() >= shot_time) {
			screenshot(shot);
			wc_dump_memory("screenshot");
			host_exit(0);
		}
		play_script((int)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count(), &buttons);
		if (!shown) {
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
			continue;
		}
		SDL_Event e;
		while (SDL_PollEvent(&e)) {
			switch (e.type) {
			case SDL_EVENT_QUIT:
				host_exit(0);
			case SDL_EVENT_KEY_DOWN:
			case SDL_EVENT_KEY_UP:
				key(&e.key);
				break;
			case SDL_EVENT_MOUSE_MOTION: {
				/* window pixels to the picture's: as much as crossing the picture on the screen */
				int ww, wh;
				SDL_GetWindowSize(window, &ww, &wh);
				float scale = 640.0f / SDL_min((float)ww, wh * 4.0f / 3);
				static float rx, ry;
				rx += e.motion.xrel * scale, ry += e.motion.yrel * scale;
				int dx = (int)rx, dy = (int)ry;
				rx -= (float)dx, ry -= (float)dy;
				if (dx || dy) win_mouse(dx, dy, buttons);
				break;
			}
			case SDL_EVENT_MOUSE_BUTTON_DOWN:
			case SDL_EVENT_MOUSE_BUTTON_UP: {
				int bit = e.button.button == SDL_BUTTON_LEFT ? 1 : e.button.button == SDL_BUTTON_RIGHT ? 2 : e.button.button == SDL_BUTTON_MIDDLE ? 4 : 0;
				buttons = e.button.down ? buttons | bit : buttons & ~bit;
				win_mouse(0, 0, buttons);
				break;
			}
			}
		}
		show_frame(); /* paced by vsync */
	}
}

[[noreturn]] void host_exit(int code)
{
	if (wav) {
		wav_header();
		fclose(wav);
	}
	fflush(stdout);
	fflush(stderr);
	std::_Exit(code);
}

void host_file_saved(const char *path)
{
}
