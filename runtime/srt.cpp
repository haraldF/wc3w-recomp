/* .srt files: numbered cues, each a "00:01:37,200 --> 00:01:40,400" line and its text lines,
 * separated by empty lines. GOG's have CRLF, empty cues (a blank text) and DOS code page 850 for
 * German and French (0x81 u umlaut, 0x82 e acute, 0xE1 sharp s). No other dependencies:
 * test/srt_test.cpp builds it on its own. */
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <sstream>

#include "subtitle.h"

namespace {

/* code page 850's characters 80h-FFh */
const char32_t CP850[] = U"ÇüéâäàåçêëèïîìÄÅÉæÆôöòûùÿÖÜø£Ø×ƒ"
	U"áíóúñÑªº¿®¬½¼¡«»\u2591\u2592\u2593\u2502\u2524ÁÂÀ©\u2563\u2551\u2557\u255d¢¥\u2510"
	U"\u2514\u2534\u252c\u251c\u2500\u253cãÃ\u255a\u2554\u2569\u2566\u2560\u2550\u256c¤ðÐÊËÈıÍÎÏ\u2518\u250c\u2588\u2584¦Ì\u2580"
	U"ÓßÔÒõÕµþÞÚÛÙýÝ¯´\u00ad±\u2017¾¶§÷¸°¨·¹³²\u25a0\u00a0";
static_assert(std::size(CP850) == 129);

/* the text as UTF-8, or nothing when it isn't valid UTF-8 */
bool utf8(const std::string &s, std::u32string *out)
{
	out->clear();
	for (size_t i = 0; i < s.size();) {
		unsigned char c = (unsigned char)s[i];
		int n = c < 0x80 ? 0 : (c & 0xe0) == 0xc0 ? 1 : (c & 0xf0) == 0xe0 ? 2 : (c & 0xf8) == 0xf0 ? 3 : -1;
		if (n < 0 || i + n >= s.size()) return false;
		char32_t cp = n == 0 ? c : c & (0x3f >> n);
		for (int k = 1; k <= n; k++) {
			unsigned char d = (unsigned char)s[i + k];
			if ((d & 0xc0) != 0x80) return false;
			cp = cp << 6 | (d & 0x3f);
		}
		static const char32_t least[] = { 0, 0x80, 0x800, 0x10000 };
		if (cp < least[n] || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) return false;
		out->push_back(cp);
		i += (size_t)n + 1;
	}
	return true;
}

/* "hh:mm:ss,mmm" (or with a '.'): seconds, or -1 */
double timestamp(const std::string &s)
{
	unsigned h, m, sec, ms;
	char sep;
	if (sscanf(s.c_str(), " %u:%u:%u%c%u", &h, &m, &sec, &sep, &ms) != 5 || (sep != ',' && sep != '.')) return -1;
	return h * 3600.0 + m * 60.0 + sec + ms / 1000.0;
}

/* a "start --> end" line */
bool timing(const std::string &line, double *start, double *end)
{
	size_t arrow = line.find("-->");
	if (arrow == std::string::npos) return false;
	*start = timestamp(line.substr(0, arrow));
	*end = timestamp(line.substr(arrow + 3));
	return *start >= 0 && *end >= 0;
}

} // namespace

std::vector<subtitle_cue> srt_parse(const std::string &data)
{
	std::u32string text;
	if (!utf8(data, &text)) {
		text.clear();
		for (unsigned char c : data) text.push_back(c < 0x80 ? c : CP850[c - 0x80]);
	}
	if (!text.empty() && text[0] == 0xfeff) text.erase(0, 1);
	/* the lines, without CR; back to bytes for the timing lines (ASCII) */
	std::vector<std::u32string> lines;
	std::u32string line;
	for (char32_t c : text) {
		if (c == '\n') lines.push_back(line), line.clear();
		else if (c != '\r') line.push_back(c);
	}
	lines.push_back(line);
	auto ascii = [](const std::u32string &l) {
		std::string s;
		for (char32_t c : l) s.push_back(c < 0x80 ? (char)c : '?');
		return s;
	};
	std::vector<subtitle_cue> cues;
	for (size_t i = 0; i < lines.size(); i++) {
		subtitle_cue cue;
		if (!timing(ascii(lines[i]), &cue.start, &cue.end)) continue;
		/* the text: up to an empty line, or the next cue's number before its timing line */
		for (i++; i < lines.size() && !lines[i].empty(); i++) {
			double s, e;
			if (timing(ascii(lines[i]), &s, &e) || (i + 1 < lines.size() && timing(ascii(lines[i + 1]), &s, &e))) {
				i--;
				break;
			}
			if (!cue.text.empty()) cue.text.push_back('\n');
			cue.text += lines[i];
		}
		cues.push_back(std::move(cue));
	}
	std::stable_sort(cues.begin(), cues.end(), [](const subtitle_cue &a, const subtitle_cue &b) { return a.start < b.start; });
	return cues;
}

std::vector<subtitle_cue> srt_read(const std::filesystem::path &file)
{
	std::ifstream f(file, std::ios::binary);
	if (!f) return {};
	std::ostringstream s;
	s << f.rdbuf();
	return srt_parse(s.str());
}

const subtitle_cue *srt_at(const std::vector<subtitle_cue> &cues, double t)
{
	/* the last cue starting at or before t */
	auto it = std::upper_bound(cues.begin(), cues.end(), t, [](double x, const subtitle_cue &c) { return x < c.start; });
	if (it == cues.begin()) return nullptr;
	--it;
	return t < it->end && !it->text.empty() ? &*it : nullptr;
}
