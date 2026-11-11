/* USER32: the program's window (one), its message queue and window procedure, the host's keyboard
 * and mouse as messages, resources (strings, icons, cursors, accelerators), the cursor, message
 * boxes. */
#include <algorithm>
#include <cstdio>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

#include "host.h"
#include "win.h"

namespace {

struct window {
	uint32_t proc = 0, style = 0, ex_style = 0, user = 0, instance = 0;
	std::string cls, title;
	int x = 0, y = 0, w = 640, h = 480;
};
struct msg {
	uint32_t hwnd, message, wparam, lparam;
};

std::map<std::string, uint32_t> classes; /* class name (upper case) -> window procedure */
std::map<uint32_t, window> windows;
uint32_t next_window = 0x10010;
std::deque<msg> queue;
int cursor_count = 0;

/* input from the host, on its thread: messages (hwnd 0) and mouse movements */
struct input_event {
	uint32_t message, wparam, lparam;
	int dx, dy, buttons; /* message 0: the mouse */
};
std::mutex input_lock;
std::vector<input_event> input;
int mouse_x = 320, mouse_y = 240, mouse_buttons;
bool vk_down[256];

enum : uint32_t {
	WM_CREATE = 0x01, WM_DESTROY = 0x02, WM_SIZE = 0x05, WM_ACTIVATE = 0x06, WM_SETFOCUS = 0x07,
	WM_CLOSE = 0x10, WM_QUIT = 0x12, WM_SHOWWINDOW = 0x18, WM_ACTIVATEAPP = 0x1c, WM_NCCREATE = 0x81,
};

std::string upper(std::string s)
{
	for (auto &c : s) c = (char)toupper((unsigned char)c);
	return s;
}

uint32_t ticks(void)
{
	return (uint32_t)(clock_us() / 1000);
}

/* A string or an atom (below 10000h) as a class name */
std::string class_name(uint32_t p)
{
	if (p < 0x10000) return "#" + std::to_string(p);
	return upper(guest_str(p));
}

uint32_t send(uint32_t hwnd, uint32_t message, uint32_t wparam, uint32_t lparam)
{
	auto it = windows.find(hwnd);
	if (it == windows.end() || !it->second.proc) return 0;
	if (wc_trace) fprintf(stderr, "\nwc: message %x (%x, %x) to window %x\n", message, wparam, lparam, hwnd);
	return win_call(it->second.proc, { hwnd, message, wparam, lparam });
}

void put_msg(uint32_t p, const msg &m)
{
	wr32(p, m.hwnd);
	wr32(p + 4, m.message);
	wr32(p + 8, m.wparam);
	wr32(p + 12, m.lparam);
	wr32(p + 16, ticks());
	wr32(p + 20, 0);
	wr32(p + 24, 0);
}

/* The first queued message for hwnd (0: any) in [lo, hi] (0, 0: any) */
std::deque<msg>::iterator find_msg(uint32_t hwnd, uint32_t lo, uint32_t hi)
{
	for (auto it = queue.begin(); it != queue.end(); ++it) {
		if ((!hwnd || it->hwnd == hwnd) && ((!lo && !hi) || (it->message >= lo && it->message <= hi))) return it;
	}
	return queue.end();
}

uint32_t main_window(void)
{
	return windows.empty() ? 0 : windows.begin()->first;
}

/* The host's input into the queue, for the program's main window */
void drain_input(void)
{
	std::vector<input_event> events;
	{
		std::lock_guard<std::mutex> g(input_lock);
		events.swap(input);
	}
	uint32_t hwnd = main_window();
	for (const input_event &e : events) {
		if (e.message) {
			if (e.message == 0x100 || e.message == 0x104) vk_down[e.wparam & 0xff] = true;
			if (e.message == 0x101 || e.message == 0x105) vk_down[e.wparam & 0xff] = false;
			queue.push_back({ hwnd, e.message, e.wparam, e.lparam });
			continue;
		}
		uint32_t mk = (e.buttons & 1 ? 1 : 0) | (e.buttons & 2 ? 2 : 0) | (e.buttons & 4 ? 0x10 : 0) |
			(vk_down[0x10] ? 4 : 0) | (vk_down[0x11] ? 8 : 0);
		if (e.dx || e.dy) {
			mouse_x = std::clamp(mouse_x + e.dx, 0, 639);
			mouse_y = std::clamp(mouse_y + e.dy, 0, 479);
			queue.push_back({ hwnd, 0x200 /* WM_MOUSEMOVE */, mk, (uint32_t)mouse_y << 16 | (uint32_t)mouse_x });
		}
		static const uint32_t down[] = { 0x201, 0x204, 0x207 }, up[] = { 0x202, 0x205, 0x208 };
		for (int b = 0; b < 3; b++) {
			int bit = 1 << b;
			if ((e.buttons ^ mouse_buttons) & bit) {
				queue.push_back({ hwnd, e.buttons & bit ? down[b] : up[b], mk, (uint32_t)mouse_y << 16 | (uint32_t)mouse_x });
			}
		}
		mouse_buttons = e.buttons;
	}
}

} // namespace

/* Whether a key (virtual key) is down, as far as the program's messages have got */
bool user_key_down(uint32_t vk)
{
	return vk_down[vk & 0xff];
}

void win_input(uint32_t message, uint32_t wparam, uint32_t lparam)
{
	std::lock_guard<std::mutex> g(input_lock);
	input.push_back({ message, wparam, lparam, 0, 0, 0 });
}

/* WC_KEYS: the mouse to a place, as a movement from where it is */
void win_mouse_at(int x, int y)
{
	std::lock_guard<std::mutex> g(input_lock);
	int px = mouse_x, py = mouse_y;
	for (const input_event &e : input) px += e.dx, py += e.dy;
	input.push_back({ 0, 0, 0, x - px, y - py, mouse_buttons });
}

void win_mouse(int dx, int dy, int buttons)
{
	std::lock_guard<std::mutex> g(input_lock);
	input.push_back({ 0, 0, 0, dx, dy, buttons });
}

void user_post(uint32_t hwnd, uint32_t message, uint32_t wparam, uint32_t lparam)
{
	queue.push_back({ hwnd, message, wparam, lparam });
}

/* ---- resources ---- */

API(USER32, LoadStringA, 4)
{
	const wc_module *m = module_by_handle(arg(0));
	uint32_t id = arg(1), buf = arg(2), size = arg(3), n;
	uint32_t data = find_resource(m, 6 /* RT_STRING */, (id >> 4) + 1, &n);
	if (!data || !size) return 0;
	/* 16 strings: a length (UTF-16 units) and the characters each */
	for (uint32_t i = 0; i < (id & 15); i++) data += 2 + 2 * (uint32_t)mrd32(data, 2);
	std::string s;
	for (char16_t c : guest_wstr_n(data + 2, (int32_t)mrd32(data, 2))) s += c < 256 ? (char)c : '?';
	return put_str(buf, s, size);
}

API(USER32, LoadIconA, 2) { return 0x30001; }
API(USER32, LoadCursorA, 2) { return 0x30002; }

API(USER32, LoadAcceleratorsA, 2)
{
	uint32_t n, data = find_resource(module_by_handle(arg(0)), 9 /* RT_ACCELERATOR */, arg(1), &n);
	return data; /* the table itself: 8 bytes an entry (flags, key, command), bit 7 of flags on the last */
}

/* An accelerator: a key down (WM_KEYDOWN, WM_SYSKEYDOWN) that the table has becomes WM_COMMAND. */
API(USER32, TranslateAcceleratorA, 3)
{
	uint32_t table = arg(1), m = arg(2);
	uint32_t message = rd32(m + 4), key = rd32(m + 8);
	if (!table || (message != 0x100 && message != 0x104)) return 0;
	for (uint32_t e = table;; e += 8) {
		uint32_t flags = (uint32_t)mrd32(e, 2), vk = (uint32_t)mrd32(e + 2, 2), cmd = (uint32_t)mrd32(e + 4, 2);
		bool alt = message == 0x104;
		if ((flags & 1 /* FVIRTKEY */) && vk == key && !!(flags & 0x10 /* FALT */) == alt) {
			send(arg(0), 0x111 /* WM_COMMAND */, 0x10000 | cmd, 0);
			return 1;
		}
		if (flags & 0x80) break;
	}
	return 0;
}

/* ---- windows ---- */

API(USER32, RegisterClassA, 1)
{
	uint32_t wc = arg(0);
	classes[upper(guest_str(rd32(wc + 36)))] = rd32(wc + 4);
	return 0xc000 + (uint32_t)classes.size();
}

API(USER32, CreateWindowExA, 12)
{
	std::string cls = class_name(arg(1));
	auto c = classes.find(cls);
	if (c == classes.end()) {
		set_last_error(1407 /* ERROR_CANNOT_FIND_WND_CLASS */);
		return 0;
	}
	uint32_t hwnd = next_window;
	next_window += 4;
	window &w = windows[hwnd];
	w.proc = c->second;
	w.cls = cls;
	w.title = arg(2) ? guest_str(arg(2)) : "";
	w.ex_style = arg(0);
	w.style = arg(3);
	w.x = (int32_t)arg(4), w.y = (int32_t)arg(5), w.w = (int32_t)arg(6), w.h = (int32_t)arg(7);
	w.instance = arg(10);
	uint32_t esp = (uint32_t)rd(R_SP, 4);
	/* CREATESTRUCT for WM_NCCREATE and WM_CREATE, below the arguments */
	uint32_t cs = heap_alloc(48);
	wr32(cs, arg(11));
	wr32(cs + 4, arg(10));
	wr32(cs + 8, arg(9));
	wr32(cs + 12, arg(8));
	wr32(cs + 16, arg(7));
	wr32(cs + 20, arg(6));
	wr32(cs + 24, arg(5));
	wr32(cs + 28, arg(4));
	wr32(cs + 32, arg(3));
	wr32(cs + 36, arg(2));
	wr32(cs + 40, arg(1));
	wr32(cs + 44, arg(0));
	bool ok = send(hwnd, WM_NCCREATE, 0, cs) && (int32_t)send(hwnd, WM_CREATE, 0, cs) != -1;
	heap_free(cs);
	wr(R_SP, 4, esp);
	if (!ok) {
		windows.erase(hwnd);
		return 0;
	}
	if (wc_trace) fprintf(stderr, "\nwc: window %x \"%s\" (class %s) at %d,%d, %dx%d", hwnd, w.title.c_str(), cls.c_str(), w.x, w.y, w.w, w.h);
	return hwnd;
}

API(USER32, ShowWindow, 2)
{
	uint32_t hwnd = arg(0), show = arg(1);
	if (show) {
		send(hwnd, WM_SHOWWINDOW, 1, 0);
		send(hwnd, WM_ACTIVATEAPP, 1, 0);
		send(hwnd, WM_ACTIVATE, 1 /* WA_ACTIVE */, 0);
		send(hwnd, WM_SETFOCUS, 0, 0);
	}
	return 0;
}

API(USER32, SetWindowPos, 7) { return 1; }
API(USER32, InvalidateRect, 3) { return 1; }
API(USER32, ValidateRect, 2) { return 1; }
API(USER32, EnableMenuItem, 3) { return 0; }

API(USER32, GetWindowLongA, 2)
{
	auto it = windows.find(arg(0));
	if (it == windows.end()) return 0;
	switch ((int32_t)arg(1)) {
	case -4: return it->second.proc;
	case -6: return it->second.instance;
	case -16: return it->second.style;
	case -20: return it->second.ex_style;
	case -21: return it->second.user;
	}
	return 0;
}

API(USER32, SetWindowLongA, 3)
{
	auto it = windows.find(arg(0));
	if (it == windows.end()) return 0;
	uint32_t *field = nullptr;
	switch ((int32_t)arg(1)) {
	case -4: field = &it->second.proc; break;
	case -16: field = &it->second.style; break;
	case -20: field = &it->second.ex_style; break;
	case -21: field = &it->second.user; break;
	default: return 0;
	}
	uint32_t old = *field;
	*field = arg(2);
	return old;
}

API(USER32, GetWindowPlacement, 2)
{
	auto it = windows.find(arg(0));
	uint32_t p = arg(1);
	memset(guest_ptr(p, 44), 0, 44);
	wr32(p, 44);
	wr32(p + 8, 1 /* SW_SHOWNORMAL */);
	if (it != windows.end()) {
		wr32(p + 28, (uint32_t)it->second.x);
		wr32(p + 32, (uint32_t)it->second.y);
		wr32(p + 36, (uint32_t)(it->second.x + it->second.w));
		wr32(p + 40, (uint32_t)(it->second.y + it->second.h));
	}
	return 1;
}

API(USER32, ClientToScreen, 2) { return 1; } /* the window is the screen */

API(USER32, GetSystemMetrics, 1)
{
	switch (arg(0)) {
	case 0: return 640;  /* SM_CXSCREEN */
	case 1: return 480;  /* SM_CYSCREEN */
	case 19: return 1;   /* SM_MOUSEPRESENT */
	case 43: return 3;   /* SM_CMOUSEBUTTONS */
	}
	return 0;
}

API(USER32, GetDC, 1) { return 0x20001; }
API(USER32, ReleaseDC, 2) { return 1; }

API(USER32, DefWindowProcA, 4)
{
	switch (arg(1)) {
	case WM_NCCREATE: return 1;
	case WM_CLOSE: send(arg(0), WM_DESTROY, 0, 0); return 0;
	}
	return 0;
}

/* ---- messages ---- */

API(USER32, PeekMessageA, 5)
{
	win_poll();
	drain_input();
	auto it = find_msg(arg(1), arg(2), arg(3));
	if (it == queue.end()) return 0;
	put_msg(arg(0), *it);
	if (arg(4) & 1 /* PM_REMOVE */) queue.erase(it);
	return 1;
}

API(USER32, GetMessageA, 4)
{
	for (;;) {
		win_poll();
		drain_input();
		auto it = find_msg(arg(1), arg(2), arg(3));
		if (it != queue.end()) {
			msg m = *it;
			queue.erase(it);
			put_msg(arg(0), m);
			return m.message != WM_QUIT;
		}
		uint32_t esp = (uint32_t)rd(R_SP, 4);
		thread_sleep(5);
		wr(R_SP, 4, esp);
	}
}

/* WM_CHAR after a key down that types a character (US layout, by position) */
API(USER32, TranslateMessage, 1)
{
	uint32_t m = arg(0), message = rd32(m + 4), vk = rd32(m + 8);
	if (message != 0x100) return 0;
	bool shift = vk_down[0x10];
	int c = 0;
	if (vk >= 'A' && vk <= 'Z') c = shift ? (int)vk : (int)vk + 32;
	else if (vk >= '0' && vk <= '9') c = shift ? ")!@#$%^&*("[vk - '0'] : (int)vk;
	else if (vk == 0x20 || vk == 0x0d || vk == 0x08 || vk == 0x1b || vk == 0x09) c = (int)vk;
	else {
		static const struct { uint8_t vk; char plain, shifted; } oem[] = {
			{ 0xba, ';', ':' }, { 0xbb, '=', '+' }, { 0xbc, ',', '<' }, { 0xbd, '-', '_' }, { 0xbe, '.', '>' },
			{ 0xbf, '/', '?' }, { 0xc0, '`', '~' }, { 0xdb, '[', '{' }, { 0xdc, '\\', '|' }, { 0xdd, ']', '}' },
			{ 0xde, '\'', '"' },
		};
		for (const auto &o : oem) {
			if (o.vk == vk) c = shift ? o.shifted : o.plain;
		}
	}
	if (!c) return 0;
	queue.push_front({ rd32(m), 0x102 /* WM_CHAR */, (uint32_t)c, rd32(m + 12) });
	return 1;
}

API(USER32, DispatchMessageA, 1)
{
	uint32_t m = arg(0);
	return send(rd32(m), rd32(m + 4), rd32(m + 8), rd32(m + 12));
}

API(USER32, SendMessageA, 4) { return send(arg(0), arg(1), arg(2), arg(3)); }

API(USER32, PostMessageA, 4)
{
	user_post(arg(0), arg(1), arg(2), arg(3));
	return 1;
}

API(USER32, PostQuitMessage, 1)
{
	user_post(0, WM_QUIT, arg(0), 0);
	return 0;
}

/* ---- cursor, message boxes ---- */

API(USER32, ShowCursor, 1)
{
	cursor_count += arg(0) ? 1 : -1;
	return (uint32_t)cursor_count;
}

/* The pointer to x, y (the game's 640x480): Windows reports that with a WM_MOUSEMOVE, from which
 * the game takes the position (it never reads it otherwise). Moves still queued are from before:
 * dropped. */
void user_set_pointer(int x, int y)
{
	mouse_x = std::clamp(x, 0, 639);
	mouse_y = std::clamp(y, 0, 479);
	std::erase_if(queue, [](const msg &m) { return m.message == 0x200; });
	uint32_t mk = (mouse_buttons & 1 ? 1 : 0) | (mouse_buttons & 2 ? 2 : 0) | (mouse_buttons & 4 ? 0x10 : 0) |
		(vk_down[0x10] ? 4 : 0) | (vk_down[0x11] ? 8 : 0);
	queue.push_back({ main_window(), 0x200 /* WM_MOUSEMOVE */, mk, (uint32_t)mouse_y << 16 | (uint32_t)mouse_x });
	if (wc_trace) fprintf(stderr, " [pointer to %d, %d]", mouse_x, mouse_y);
}

API(USER32, SetCursorPos, 2)
{
	user_set_pointer((int)arg(0), (int)arg(1));
	return 1;
}

API(USER32, MessageBoxA, 4)
{
	std::string text = arg(1) ? guest_str(arg(1)) : "", caption = arg(2) ? guest_str(arg(2)) : "";
	fprintf(stderr, "wc: message box \"%s\": %s\n", caption.c_str(), text.c_str());
	return 1; /* IDOK */
}
