/* Subtitles for the HD movies: GOG's .srt files (srt.cpp), drawn into the movie's frames
 * (subtitle.cpp, FreeType) */
#pragma once
#include <filesystem>
#include <string>
#include <vector>

struct subtitle_cue {
	double start = 0, end = 0; /* seconds */
	std::u32string text;       /* lines separated by '\n'; empty: nothing shown */
};

/* An .srt file's cues: UTF-8 if it is valid UTF-8, else code page 850 (GOG's German and French) */
std::vector<subtitle_cue> srt_parse(const std::string &data);
std::vector<subtitle_cue> srt_read(const std::filesystem::path &file);
/* the cue shown at t, or nullptr (none, or an empty one) */
const subtitle_cue *srt_at(const std::vector<subtitle_cue> &cues, double t);

/* Draws text centred at the bottom of a w x h picture (0RGB: BGR0 bytes), white with a black
 * outline; false when there is no font (reported once) */
bool subtitle_draw(uint32_t *pixels, int w, int h, const std::u32string &text);
