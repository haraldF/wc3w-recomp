/* HD movies instead of the game's own (320x240, interlaced): the game's movie function (41C240h,
 * hooks.txt) plays the scene's branches from the 30th Anniversary Edition's MP4 files (1080p H.264,
 * AAC), decoded with FFmpeg, as the enhancement patch does with libvlc (Mod_Movies.cpp,
 * libvlc_Movies.cpp; MIT, by Matt Wells):
 *  - the scene is the movie file's name ("..\..\data\movies\opening.MVE": "opening"), each branch
 *    a letter (0 "a", 1 "b", ...); movies.ini renames scenes ("name=sc_001") and branches
 *    ("a=b"), or skips them ("a=-1"); the file is <dir><name><branch>.mp4;
 *  - the game keeps the branches to play in a list (4AAA50h, -1 at the end) with the current
 *    index (4A24D4h); after them its own function (40FD20h) shows a conversation choice and fills
 *    the list again, until it starts with -1;
 *  - Esc ends the movie (the game's halt flag, 4A24C4h, too); the next starts once it's released;
 *  - a scene without its first file plays with the original player.
 * The host shows the newest frame due (movie_frame), with the game's own picture over it where it
 * isn't colour 0 (the choice text). WC_HD_MOVIES=0: the original movies; WC_HD_MOVIES=dir: another
 * folder (in the game folder) than Master_FMVs_Upscaled, e.g. Master_FMVs_Source. */
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include "sound.h"
#include "win.h"

/* ddraw.cpp: the primary surface and its back buffer to colour 0, so only what the game draws
 * during the movie (the choice text) shows over it */
void ddraw_clear(void);
bool user_key_down(uint32_t vk); /* user32.cpp */

namespace {

/* the game's movie state */
const uint32_t BRANCH_LIST = 0x4aaa50, BRANCH_INDEX = 0x4a24d4, HALT_FLAG = 0x4a24c4, FRAME_COUNT = 0x4a24bc,
	CLICK_TIME = 0x4aabf0, FREQUENCY = 0x4a9768;
const uint32_t TRANSLATE_MESSAGES = 0x482920, MESSAGE_NODE_ADD = 0x483090, MESSAGE_NODE_REMOVE = 0x4830d0,
	MOVIE_MESSAGES = 0x41c700, HANDLE_MOVIE = 0x40fd20;

/* ---- movies.ini ---- */

std::map<std::string, std::map<std::string, std::string>> ini;

std::string trim(std::string s)
{
	size_t c = s.find(';');
	if (c != std::string::npos) s.resize(c);
	size_t a = s.find_first_not_of(" \t\r"), b = s.find_last_not_of(" \t\r");
	return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

void read_ini(const std::filesystem::path &file)
{
	std::ifstream f(file);
	std::string line, section;
	while (std::getline(f, line)) {
		line = trim(line);
		if (line.size() > 2 && line[0] == '[') {
			section = line.substr(1, line.find(']') - 1);
			for (auto &c : section) c = (char)tolower((unsigned char)c);
		}
		else if (size_t eq = line.find('='); eq != std::string::npos && !section.empty()) {
			ini[section][trim(line.substr(0, eq))] = trim(line.substr(eq + 1));
		}
	}
}

/* The file of a scene's branch; empty: the branch is skipped */
std::filesystem::path branch_file(const std::filesystem::path &dir, const std::string &scene, int branch)
{
	std::string letter = branch < 26 ? std::string(1, (char)('a' + branch)) : std::string(2, (char)('a' + branch - 26));
	std::string name = scene;
	auto s = ini.find(scene);
	if (s != ini.end()) {
		if (auto n = s->second.find("name"); n != s->second.end()) name = n->second;
		if (auto b = s->second.find(letter); b != s->second.end()) letter = b->second;
	}
	if (letter == "-1") return {};
	return dir / (name + letter + ".mp4");
}

/* ---- the decoder: a thread reading the file, frames (RGB) and sound (to the mixer) ---- */

struct frame {
	double pts = 0;
	std::vector<uint32_t> pixels;
};

class player {
public:
	explicit player(const std::filesystem::path &file) : file_(file) {}
	~player() { stop(); }

	bool start()
	{
		if (avformat_open_input(&fmt_, file_.string().c_str(), nullptr, nullptr) < 0) return false;
		if (avformat_find_stream_info(fmt_, nullptr) < 0) return false;
		vstream_ = av_find_best_stream(fmt_, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
		astream_ = av_find_best_stream(fmt_, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
		if (vstream_ < 0 || !open_codec(vstream_, &vdec_)) return false;
		if (astream_ >= 0 && !open_codec(astream_, &adec_)) astream_ = -1;
		w_ = vdec_->width, h_ = vdec_->height;
		if (astream_ >= 0) sound_stream_add(&audio_);
		started_ = std::chrono::steady_clock::now();
		thread_ = std::thread([this] { decode(); });
		return true;
	}

	void stop()
	{
		quit_ = true;
		wake_.notify_all();
		if (thread_.joinable()) thread_.join();
		if (astream_ >= 0) sound_stream_remove(&audio_);
		sws_freeContext(sws_);
		sws_ = nullptr;
		swr_free(&swr_);
		avcodec_free_context(&vdec_);
		avcodec_free_context(&adec_);
		avformat_close_input(&fmt_);
	}

	/* the playing time: the sound's (what the mixer has played), else the clock's */
	double clock()
	{
		if (astream_ >= 0) {
			std::lock_guard<std::mutex> g(sound_lock);
			return (double)audio_.consumed / MIX_RATE;
		}
		return std::chrono::duration<double>(std::chrono::steady_clock::now() - started_).count();
	}

	/* The newest frame due, if there is a new one: into pixels (w x h) */
	bool current(std::vector<uint32_t> *pixels, int *w, int *h)
	{
		double t = clock();
		std::lock_guard<std::mutex> g(lock_);
		bool got = false;
		while (!frames_.empty() && frames_.front().pts <= t) {
			shown_ = std::move(frames_.front());
			frames_.pop_front();
			got = true;
		}
		if (got) {
			wake_.notify_all();
			*pixels = shown_.pixels;
			*w = w_, *h = h_;
		}
		return got;
	}

	bool finished()
	{
		std::lock_guard<std::mutex> g(lock_);
		if (!eof_ || !frames_.empty()) return false;
		std::lock_guard<std::mutex> s(sound_lock);
		return audio_.samples.empty();
	}

private:
	bool open_codec(int stream, AVCodecContext **ctx)
	{
		const AVCodec *codec = avcodec_find_decoder(fmt_->streams[stream]->codecpar->codec_id);
		if (!codec) return false;
		*ctx = avcodec_alloc_context3(codec);
		avcodec_parameters_to_context(*ctx, fmt_->streams[stream]->codecpar);
		(*ctx)->thread_count = 0; /* as many as there are cores */
		return avcodec_open2(*ctx, codec, nullptr) >= 0;
	}

	/* waits while enough is decoded ahead: 8 frames, or a quarter of a second of sound */
	void throttle()
	{
		std::unique_lock<std::mutex> g(lock_);
		wake_.wait_for(g, std::chrono::milliseconds(20), [this] {
			if (quit_ || frames_.size() < 8) return true;
			std::lock_guard<std::mutex> s(sound_lock);
			return astream_ >= 0 && audio_.samples.size() < 2 * MIX_RATE / 4;
		});
	}

	int64_t start_pts(int stream)
	{
		int64_t s = fmt_->streams[stream]->start_time;
		return s == AV_NOPTS_VALUE ? 0 : s;
	}

	void video(AVFrame *f)
	{
		sws_ = sws_getCachedContext(sws_, f->width, f->height, (AVPixelFormat)f->format, w_, h_, AV_PIX_FMT_BGR0,
			SWS_BILINEAR, nullptr, nullptr, nullptr);
		frame out;
		int64_t ts = f->best_effort_timestamp == AV_NOPTS_VALUE ? 0 : f->best_effort_timestamp;
		out.pts = (double)(ts - start_pts(vstream_)) * av_q2d(fmt_->streams[vstream_]->time_base);
		out.pixels.resize((size_t)w_ * h_);
		uint8_t *dst[1] = { (uint8_t *)out.pixels.data() };
		int pitch[1] = { 4 * w_ };
		sws_scale(sws_, f->data, f->linesize, 0, f->height, dst, pitch);
		std::lock_guard<std::mutex> g(lock_);
		frames_.push_back(std::move(out));
	}

	void sound(AVFrame *f)
	{
		if (!swr_) {
			AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
			swr_alloc_set_opts2(&swr_, &stereo, AV_SAMPLE_FMT_S16, MIX_RATE, &f->ch_layout, (AVSampleFormat)f->format,
				f->sample_rate, 0, nullptr);
			swr_init(swr_);
		}
		int max = swr_get_out_samples(swr_, f->nb_samples);
		if (max <= 0) return;
		std::vector<int16_t> buf(2 * (size_t)max);
		uint8_t *out[1] = { (uint8_t *)buf.data() };
		int n = swr_convert(swr_, out, max, (const uint8_t **)f->extended_data, f->nb_samples);
		if (n <= 0) return;
		std::lock_guard<std::mutex> g(sound_lock);
		audio_.samples.insert(audio_.samples.end(), buf.begin(), buf.begin() + 2 * n);
	}

	void receive(AVCodecContext *dec, bool is_video, AVFrame *f)
	{
		while (avcodec_receive_frame(dec, f) == 0) {
			if (is_video) video(f);
			else sound(f);
			av_frame_unref(f);
		}
	}

	void decode()
	{
		AVPacket *pkt = av_packet_alloc();
		AVFrame *f = av_frame_alloc();
		while (!quit_) {
			throttle();
			if (quit_) break;
			if (av_read_frame(fmt_, pkt) < 0) break;
			if (pkt->stream_index == vstream_) {
				avcodec_send_packet(vdec_, pkt);
				receive(vdec_, true, f);
			}
			else if (pkt->stream_index == astream_) {
				avcodec_send_packet(adec_, pkt);
				receive(adec_, false, f);
			}
			av_packet_unref(pkt);
		}
		if (!quit_) { /* the rest in the decoders */
			avcodec_send_packet(vdec_, nullptr);
			receive(vdec_, true, f);
			if (adec_) {
				avcodec_send_packet(adec_, nullptr);
				receive(adec_, false, f);
			}
		}
		av_frame_free(&f);
		av_packet_free(&pkt);
		std::lock_guard<std::mutex> g(lock_);
		eof_ = true;
	}

	std::filesystem::path file_;
	AVFormatContext *fmt_ = nullptr;
	AVCodecContext *vdec_ = nullptr, *adec_ = nullptr;
	SwsContext *sws_ = nullptr;
	SwrContext *swr_ = nullptr;
	int vstream_ = -1, astream_ = -1, w_ = 0, h_ = 0;
	pcm_stream audio_;
	std::chrono::steady_clock::time_point started_;
	std::thread thread_;
	std::mutex lock_;
	std::condition_variable wake_;
	std::deque<frame> frames_;
	frame shown_;
	std::atomic<bool> quit_{ false };
	bool eof_ = false;
};

/* what the host shows: the playing movie's frames */
std::mutex show_lock;
player *playing;
bool active; /* an HD movie scene is on (also between its branches) */
std::vector<uint32_t> shown;
int shown_w, shown_h;
uint64_t shown_count = 1;

std::filesystem::path movie_dir(void)
{
	const char *e = getenv("WC_HD_MOVIES");
	std::string sub = e && *e && strcmp(e, "1") ? e : "Master_FMVs_Upscaled";
	return std::filesystem::path(win_game_dir) / sub;
}

bool hd_movies_on(void)
{
	const char *e = getenv("WC_HD_MOVIES");
	return !(e && !strcmp(e, "0"));
}

/* the scene's name from the movie's path: its file name, lower case, without the extension */
std::string scene_of(const std::string &path)
{
	size_t slash = path.find_last_of("\\/");
	std::string name = path.substr(slash == std::string::npos ? 0 : slash + 1);
	name = name.substr(0, name.find('.'));
	for (auto &c : name) c = (char)tolower((unsigned char)c);
	return name.substr(0, 8);
}

bool halted(void) { return M[HALT_FLAG] != 0 || user_key_down(0x1b); }

/* The program's message loop (its movie handler sets the halt flag on Esc), then a pause for the
 * program's other threads */
void pump(void)
{
	win_call(TRANSLATE_MESSAGES, {});
	thread_sleep(5);
}

/* Plays the list's branches from the current index; false if one of them failed to start. The
 * index is left at the last one played. */
bool play_branches(const std::filesystem::path &dir, const std::string &scene)
{
	for (uint32_t i = rd32(BRANCH_INDEX);; i++) {
		int32_t branch = (int32_t)rd32(BRANCH_LIST + 4 * i);
		if (branch < 0 || halted()) return true;
		std::filesystem::path file = branch_file(dir, scene, branch);
		wr32(BRANCH_INDEX, i);
		if (file.empty()) continue;
		player *p = new player(file);
		if (!p->start()) {
			fprintf(stderr, "wc: HD movie %s: cannot play\n", file.string().c_str());
			delete p;
			return false;
		}
		if (wc_trace) fprintf(stderr, "wc: HD movie %s\n", file.string().c_str());
		{
			std::lock_guard<std::mutex> g(show_lock);
			playing = p;
		}
		while (!p->finished() && !halted()) pump();
		{
			std::lock_guard<std::mutex> g(show_lock);
			playing = nullptr;
		}
		delete p;
	}
}

/* play_movie(path, ...) (cdecl): returns TRUE; or the original player plays it */
bool play_movie(void)
{
	if (!hd_movies_on()) return false;
	uint32_t esp = (uint32_t)rd(R_SP, 4);
	std::string scene = scene_of(guest_str(rd32(esp + 4)));
	std::filesystem::path dir = movie_dir();
	static bool ini_read;
	if (!ini_read) {
		ini_read = true;
		read_ini(dir / "movies.ini");
	}
	int32_t first = (int32_t)rd32(BRANCH_LIST + 4 * rd32(BRANCH_INDEX));
	std::filesystem::path file = first >= 0 ? branch_file(dir, scene, first) : std::filesystem::path();
	std::error_code ec;
	if (file.empty() || !std::filesystem::exists(file, ec)) {
		if (wc_trace) fprintf(stderr, "wc: no HD movie for %s (%s)\n", scene.c_str(), file.string().c_str());
		return false;
	}
	/* as the game's player starts: the double click time (a third of a second), no frames yet */
	mwr32(CLICK_TIME, 8, mrd32(FREQUENCY, 8) / 3);
	wr32(FRAME_COUNT, 0);
	win_call(MESSAGE_NODE_ADD, { MOVIE_MESSAGES });
	ddraw_clear();
	{
		std::lock_guard<std::mutex> g(show_lock);
		active = true;
	}
	bool ok = true;
	while (!halted()) {
		if (!play_branches(dir, scene)) {
			ok = false;
			break;
		}
		if (halted()) break;
		/* the game's conversation choice (if any), which sets the next branches */
		win_call(HANDLE_MOVIE, { 0 });
		wr32(BRANCH_INDEX, 0);
		if ((int32_t)rd32(BRANCH_LIST) < 0) break;
	}
	/* Esc: not again for the next movie */
	while (user_key_down(0x1b)) pump();
	win_call(MESSAGE_NODE_REMOVE, { MOVIE_MESSAGES });
	{
		std::lock_guard<std::mutex> g(show_lock);
		active = false;
		shown.clear();
	}
	ddraw_clear();
	if (!ok) {
		wr(R_SP, 4, esp);
		return false; /* the rest with the original player, from where this stopped */
	}
	wr(R_AX, 4, 1);
	wr(R_SP, 4, esp + 4); /* the RET (cdecl: the caller pops the arguments) */
	return true;
}

struct init {
	init() { hook_register(0x41c240, play_movie); }
} init_;

} // namespace

/* For the host: whether an HD movie is on, and its newest frame into pixels when count isn't the
 * frame's (count is set to it) */
bool movie_frame(std::vector<uint32_t> *pixels, int *w, int *h, uint64_t *count)
{
	std::lock_guard<std::mutex> g(show_lock);
	if (!active) return false;
	if (playing && playing->current(&shown, &shown_w, &shown_h)) shown_count++;
	if (!shown.empty() && *count != shown_count) {
		*pixels = shown;
		*w = shown_w, *h = shown_h;
		*count = shown_count;
	}
	return true;
}
