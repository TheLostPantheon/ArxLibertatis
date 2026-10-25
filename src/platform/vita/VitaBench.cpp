/*
 * Copyright 2026 Arx Libertatis Team (see the AUTHORS file)
 *
 * This file is part of Arx Libertatis.
 *
 * Arx Libertatis is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Arx Libertatis is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Arx Libertatis.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "platform/vita/VitaBench.h"

#if ARX_PLATFORM == ARX_PLATFORM_VITA

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <glm/common.hpp>

#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

#include "core/Application.h"
#include "core/Config.h"
#include "core/Core.h"
#include "core/Version.h"
#include "game/EntityManager.h"
#include "game/Player.h"
#include "graphics/data/Mesh.h"
#include "io/fs/Filesystem.h"
#include "io/fs/SystemPaths.h"
#include "io/log/Logger.h"
#include "platform/vita/VitaInit.h"

namespace platform {
namespace vita {

// ---- frame timing

namespace {

struct FrameRecord {
	uint32_t frameUs = 0;
	uint32_t phaseUs[PerfPhaseCount] = {};
	uint32_t zoneUs[PerfZoneCount] = {};
	uint32_t drawCalls = 0;
	bool load = false;
	bool inGame = false;
};

const char * const g_phaseNames[PerfPhaseCount] = { "sleep", "update", "render", "audio", "swap" };

const char * const g_zoneNames[PerfZoneCount] = {
	"controls", "entityprep", "playermove", "playeranim", "camera", "scripttimers", "treatzone",
	"npcphysics", "iolighting", "dynlights", "portals", "roomcull", "thrown", "updateinter",
	"particlemgr", "bgactions", "magic",
	"roomsopaque", "shadows", "renderinter", "popopaque", "transparent", "particles", "spells",
	"batcher", "hud", "speech",
	"picking", "scripts", "gamemisc",
	"ent.lightpre", "ent.animated", "ent.static", "ent.player",
	"npc.floor", "npc.box", "npc.move", "npc.check",
	"npc.gravity", "npc.collide", "npc.anchor", "npc.nextnode", "npc.pathfind", "npc.roomdist", "npc.visible", "npc.detect", "part.systems", "part.legacy", "misc.damages", "world.collect", "hud.minimap", "misc.zones",
};

//! "name=avg" for the costliest zones, by per-frame average over n frames.
std::string formatZones(const uint64_t (&sums)[PerfZoneCount], size_t n, size_t top) {
	size_t order[PerfZoneCount];
	for(size_t i = 0; i < PerfZoneCount; i++) {
		order[i] = i;
	}
	std::sort(order, order + PerfZoneCount, [&](size_t a, size_t b) { return sums[a] > sums[b]; });
	uint64_t total = 0;
	for(uint64_t v : sums) {
		total += v;
	}
	std::string out;
	char buf[48];
	std::snprintf(buf, sizeof(buf), "total=%.2f", double(total) / double(n) / 1000.0);
	out += buf;
	for(size_t i = 0; i < std::min(top, size_t(PerfZoneCount)); i++) {
		if(sums[order[i]] == 0) {
			break;
		}
		std::snprintf(buf, sizeof(buf), " %s=%.2f", g_zoneNames[order[i]],
		              double(sums[order[i]]) / double(n) / 1000.0);
		out += buf;
	}
	return out;
}

uint64_t g_frameStart = 0;
FrameRecord g_current;
FrameRecord g_last;
uint32_t g_frameIndex = 0;
const char * volatile g_mainPhase = "boot";
volatile uint64_t g_boundaryUs = 0;

uint32_t cpuUs(const FrameRecord & f) {
	uint32_t waits = f.phaseUs[PerfSleep] + f.phaseUs[PerfSwap];
	return f.frameUs > waits ? f.frameUs - waits : 0;
}

template <typename T>
T percentile(std::vector<T> & sorted, double p) {
	if(sorted.empty()) {
		return T(0);
	}
	return sorted[std::min(sorted.size() - 1, size_t(double(sorted.size()) * p))];
}

double average(const std::vector<uint32_t> & values) {
	if(values.empty()) {
		return 0.0;
	}
	double sum = 0.0;
	for(uint32_t v : values) {
		sum += v;
	}
	return sum / double(values.size());
}

//! "[Frame]" line every 5 s of in-game frames.
class FrameWindow {

	static constexpr uint64_t WindowUs = 5000000;

	std::vector<uint32_t> m_frames;
	std::vector<uint32_t> m_cpu;
	uint64_t m_phaseSum[PerfPhaseCount] = {};
	uint64_t m_zoneSum[PerfZoneCount] = {};
	uint64_t m_elapsed = 0;

public:

	FrameWindow() {
		m_frames.reserve(512);
		m_cpu.reserve(512);
	}

	void add(const FrameRecord & f) {

		if(!f.inGame || f.load) {
			return;
		}

		m_frames.push_back(f.frameUs);
		m_cpu.push_back(cpuUs(f));
		for(size_t i = 0; i < PerfPhaseCount; i++) {
			m_phaseSum[i] += f.phaseUs[i];
		}
		for(size_t i = 0; i < PerfZoneCount; i++) {
			m_zoneSum[i] += f.zoneUs[i];
		}
		m_elapsed += f.frameUs;

		if(m_elapsed >= WindowUs) {
			flush();
		}
	}

	void flush() {

		size_t n = m_frames.size();
		if(n == 0) {
			return;
		}

		double avg = average(m_frames);
		double cpu = average(m_cpu);
		unsigned over50 = unsigned(std::count_if(m_frames.begin(), m_frames.end(),
		                                         [](uint32_t us) { return us > 50000; }));
		std::sort(m_frames.begin(), m_frames.end());

		char buf[320];
		std::snprintf(buf, sizeof(buf),
		              "[Frame] fps=%.1f avg=%.1f p50=%.1f p90=%.1f max=%.1f cpu=%.1f upd=%.1f rend=%.1f "
		              "aud=%.1f swap=%.1f sleep=%.1f >50ms=%u",
		              1e6 * double(n) / double(m_elapsed), avg / 1000.0,
		              percentile(m_frames, 0.5) / 1000.0, percentile(m_frames, 0.9) / 1000.0,
		              m_frames.back() / 1000.0, cpu / 1000.0,
		              double(m_phaseSum[PerfUpdate]) / double(n) / 1000.0,
		              double(m_phaseSum[PerfRender]) / double(n) / 1000.0,
		              double(m_phaseSum[PerfAudio]) / double(n) / 1000.0,
		              double(m_phaseSum[PerfSwap]) / double(n) / 1000.0,
		              double(m_phaseSum[PerfSleep]) / double(n) / 1000.0, over50);
		LogInfo << buf;
		LogInfo << "[Zones] " << formatZones(m_zoneSum, n, 8);

		m_frames.clear();
		m_cpu.clear();
		std::fill(std::begin(m_phaseSum), std::end(m_phaseSum), 0);
		std::fill(std::begin(m_zoneSum), std::end(m_zoneSum), 0);
		m_elapsed = 0;
	}

};

FrameWindow g_window;

} // anonymous namespace

uint64_t perfNowUs() {
	return sceKernelGetProcessTimeWide();
}

//! A slow gameplay frame: where its time went (the zones are inclusive, nested ones count twice).
static void logHitch(const FrameRecord & frame) {
	static unsigned s_logged = 0;
	if(s_logged >= 300) {
		return;
	}
	s_logged++;
	size_t order[PerfZoneCount];
	for(size_t i = 0; i < PerfZoneCount; i++) {
		order[i] = i;
	}
	std::partial_sort(order, order + 5, order + PerfZoneCount, [&](size_t a, size_t b) {
		return frame.zoneUs[a] > frame.zoneUs[b];
	});
	char buf[512];
	int n = std::snprintf(buf, sizeof(buf), "[Hitch] %.1f ms: upd=%.1f rend=%.1f aud=%.1f swap=%.1f sleep=%.1f draws=%u |",
	                      double(frame.frameUs) / 1000.0, double(frame.phaseUs[PerfUpdate]) / 1000.0,
	                      double(frame.phaseUs[PerfRender]) / 1000.0, double(frame.phaseUs[PerfAudio]) / 1000.0,
	                      double(frame.phaseUs[PerfSwap]) / 1000.0, double(frame.phaseUs[PerfSleep]) / 1000.0,
	                      unsigned(frame.drawCalls));
	for(size_t i = 0; i < 5 && n > 0 && size_t(n) < sizeof(buf); i++) {
		n += std::snprintf(buf + n, sizeof(buf) - size_t(n), " %s=%.1f", g_zoneNames[order[i]],
		                   double(frame.zoneUs[order[i]]) / 1000.0);
	}
	LogInfo << buf;
}

void perfFrameBoundary() {

	uint64_t now = perfNowUs();

	if(g_frameStart != 0) {
		g_current.frameUs = uint32_t(std::min<uint64_t>(now - g_frameStart, UINT32_MAX));
		// render() resets the counter at its start, so it still holds this frame's count
		g_current.drawCalls = uint32_t(std::max(EERIEDrawCalls, 0l));
		if(g_current.inGame && !g_current.load && g_current.frameUs > 50000) {
			logHitch(g_current);
		}
		g_last = g_current;
		g_frameIndex++;
		g_window.add(g_last);
	}

	g_current = FrameRecord();
	g_frameStart = now;
	g_boundaryUs = now;
	g_mainPhase = "frame";
}

void perfMarkLoading() {
	g_current.load = true;
}

void perfAdd(PerfPhase phase, uint64_t us) {
	g_current.phaseUs[phase] += uint32_t(std::min<uint64_t>(us, UINT32_MAX));
}

PerfScope::PerfScope(PerfPhase phase)
	: m_phase(phase)
	, m_start(perfNowUs())
{
	g_mainPhase = g_phaseNames[phase];
}

PerfScope::~PerfScope() {
	perfAdd(m_phase, perfNowUs() - m_start);
	g_mainPhase = "frame";
}

ZoneScope::ZoneScope(PerfZone zone)
	: m_zone(zone)
	, m_start(perfNowUs())
{ }

ZoneScope::~ZoneScope() {
	g_current.zoneUs[m_zone] += uint32_t(std::min<uint64_t>(perfNowUs() - m_start, UINT32_MAX));
}

// ---- load breakdown

namespace {

const char * const g_loadZoneNames[LoadZoneCount] = {
	"other", "screen", "pak", "blast", "image", "jpeg", "texture", "audio", "clear", "saveblock", "savecopy", "dlf",
	"scene", "entities", "model", "physics", "anim", "script", "scriptinit", "tweak",
	"portalvb", "worldgpu", "shaders", "pathfinder", "levelinit",
};

bool g_loadActive = false;
SceUID g_loadThread = -1; // the profiler only follows the thread that started the load
const char * g_loadWhat = "";
uint64_t g_loadStart = 0;
uint64_t g_loadMark = 0;
uint64_t g_loadUs[LoadZoneCount] = {};
uint32_t g_loadCount[LoadZoneCount] = {};
LoadZone g_loadStack[64];
int g_loadDepth = 0;
uint64_t g_lastLoadUs = 0; // the last finished load, for the bench result
uint64_t g_loadBytes = 0;

void chargeLoad(uint64_t now) {
	LoadZone zone = g_loadDepth > 0 ? g_loadStack[g_loadDepth - 1] : LoadOther;
	g_loadUs[zone] += now - g_loadMark;
	g_loadMark = now;
}

} // anonymous namespace

LoadScope::LoadScope(LoadZone zone)
	: m_active(g_loadActive && g_loadDepth < int(std::size(g_loadStack))
	           && sceKernelGetThreadId() == g_loadThread)
{
	if(m_active) {
		chargeLoad(perfNowUs());
		g_loadStack[g_loadDepth++] = zone;
		g_loadCount[zone]++;
	}
}

LoadScope::~LoadScope() {
	if(m_active && g_loadActive && g_loadDepth > 0) {
		chargeLoad(perfNowUs());
		g_loadDepth--;
	}
}

void loadAddBytes(size_t bytes) {
	if(g_loadActive && sceKernelGetThreadId() == g_loadThread) {
		g_loadBytes += bytes;
	}
}

void loadBegin(const char * what) {
	if(g_loadActive) {
		return; // a save load runs a level load inside it: one breakdown for both
	}
	g_loadActive = true;
	g_loadThread = sceKernelGetThreadId();
	g_loadWhat = what;
	g_loadStart = g_loadMark = perfNowUs();
	std::fill(std::begin(g_loadUs), std::end(g_loadUs), 0);
	std::fill(std::begin(g_loadCount), std::end(g_loadCount), 0);
	g_loadDepth = 0;
	g_loadBytes = 0;
}

void loadEnd() {

	if(!g_loadActive) {
		return;
	}
	uint64_t now = perfNowUs();
	chargeLoad(now);
	g_loadActive = false;
	g_loadDepth = 0;
	g_lastLoadUs = now - g_loadStart;

	size_t order[LoadZoneCount];
	for(size_t i = 0; i < LoadZoneCount; i++) {
		order[i] = i;
	}
	std::sort(order, order + LoadZoneCount, [](size_t a, size_t b) { return g_loadUs[a] > g_loadUs[b]; });
	std::string parts;
	char buf[64];
	for(size_t i : order) {
		if(g_loadUs[i] < 1000) {
			break;
		}
		std::snprintf(buf, sizeof(buf), " %s=%.0f", g_loadZoneNames[i], double(g_loadUs[i]) / 1000.0);
		parts += buf;
		if(g_loadCount[i] > 1) {
			std::snprintf(buf, sizeof(buf), "(x%u)", unsigned(g_loadCount[i]));
			parts += buf;
		}
	}
	LogInfo << "[Load] " << g_loadWhat << " total=" << (g_lastLoadUs / 1000) << "ms read="
	        << (g_loadBytes / 1024) << "KB" << parts;
}

const char * perfMainPhase() {
	return g_mainPhase;
}

uint64_t perfMainAgeUs() {
	uint64_t boundary = g_boundaryUs;
	return boundary ? perfNowUs() - boundary : 0;
}

// ---- record and replay

namespace {

const char * const BenchDir = "ux0:data/arx/bench";
const char * const ResultsFile = "ux0:data/arx/bench/results.txt";
const char * const ReplayToggle = "ux0:data/arx/bench.txt";
const char * const RecordToggle = "ux0:data/arx/benchrec.txt";
const uint64_t SampleUs = 100000; // 10 Hz

enum Mode {
	ModeOff,
	ModeRecord,
	ModeReplay
};

struct Options {
	Mode mode = ModeOff;
	std::string route = "route.txt";
	std::string save;      // replay: "save=<path>" instead of the route's own save
	float settleSeconds = 2.f;
	int fps = -1;          // replay: "fps=<n>" frame limit for this session (0 = none)
	bool exit = false;
};

bool exists(const char * path) {
	SceIoStat st;
	return sceIoGetstat(path, &st) >= 0;
}

std::string benchPath(const std::string & name) {
	return std::string(BenchDir) + "/" + name;
}

void readOptions(const char * file, Options & options) {
	FILE * f = std::fopen(file, "r");
	if(!f) {
		return;
	}
	char line[256];
	while(std::fgets(line, sizeof(line), f)) {
		line[std::strcspn(line, "\r\n")] = '\0';
		if(std::strncmp(line, "route=", 6) == 0 && line[6] != '\0') {
			options.route = line + 6;
		} else if(std::strncmp(line, "save=", 5) == 0) {
			options.save = line + 5;
		} else if(std::strncmp(line, "settle=", 7) == 0) {
			options.settleSeconds = float(std::atof(line + 7));
		} else if(std::strncmp(line, "fps=", 4) == 0) {
			options.fps = std::atoi(line + 4);
		} else if(std::strcmp(line, "exit") == 0) {
			options.exit = true;
		}
	}
	std::fclose(f);
}

const Options & options() {
	static const Options s_options = [] {
		Options o;
		if(exists(ReplayToggle)) {
			o.mode = ModeReplay;
			readOptions(ReplayToggle, o);
		} else if(exists(RecordToggle)) {
			o.mode = ModeRecord;
			readOptions(RecordToggle, o);
		}
		return o;
	}();
	return s_options;
}

fs::path g_lastLoadedSave;
unsigned g_saveLoads = 0;

std::string routeSavePath(const std::string & route) {
	std::string path = benchPath(route);
	size_t dot = path.rfind('.');
	if(dot != std::string::npos && dot > path.rfind('/')) {
		path.resize(dot);
	}
	return path + ".sav";
}

struct Sample {
	float t;
	Vec3f pos;
	float pitch;
	float yaw;
	u32 level;
};

class Recorder {

	FILE * m_file = nullptr;
	bool m_failed = false;
	bool m_rotated = false;
	unsigned m_saveLoads = 0;
	uint64_t m_clock = 0;
	uint64_t m_next = 0;
	uint32_t m_seenFrame = 0;
	u32 m_level = 0;

	bool start() {

		sceIoMkdir(BenchDir, 0777);
		std::string route = benchPath(options().route);
		// Keep the previous recording once per session; a restart within the
		// session (another save loaded) just replaces its own attempt.
		if(!m_rotated) {
			std::string previous = route + ".prev";
			sceIoRemove(previous.c_str());
			sceIoRename(route.c_str(), previous.c_str());
			m_rotated = true;
		}
		m_file = std::fopen(route.c_str(), "w");
		if(!m_file) {
			LogError << "[Bench] cannot write " << route;
			m_failed = true;
			return false;
		}

		// The route replays from a copy of the save it started from, so later
		// saves in the same slot don't change the benchmark.
		std::string saveCopy = routeSavePath(options().route);
		bool copied = fs::copy_file(g_lastLoadedSave, saveCopy, true);
		if(!copied) {
			LogError << "[Bench] could not copy " << g_lastLoadedSave << " to " << saveCopy
			         << ": replays will need save=<path> in bench.txt";
		}

		std::fprintf(m_file, "# Arx Libertatis Vita bench route v1: t x y z pitch yaw; 'level <n>' on change\n");
		std::fprintf(m_file, "save %s\n", copied ? saveCopy.c_str() : "");
		m_saveLoads = g_saveLoads;
		m_clock = 0;
		m_next = 0;
		m_level = 0;
		m_seenFrame = g_frameIndex;

		// Record once: the next launch plays normally
		sceIoRemove(RecordToggle);

		LogInfo << "[Bench] recording " << route << " from save " << g_lastLoadedSave;
		return true;
	}

public:

	void tick() {

		// Wait for a save: at boot the game plays the intro in level 10 behind the main menu
		if(m_failed || g_saveLoads == 0) {
			return;
		}

		if(m_file && g_saveLoads != m_saveLoads) {
			std::fclose(m_file);
			m_file = nullptr;
			LogInfo << "[Bench] another save was loaded: recording again from it";
		}

		if(!m_file && !start()) {
			return;
		}

		// The route clock only runs in game and skips loading frames, so a
		// replay can pause it for its own loads at the same points.
		if(g_frameIndex != m_seenFrame) {
			m_seenFrame = g_frameIndex;
			if(!g_last.load && g_last.inGame) {
				m_clock += g_last.frameUs;
			}
		}
		if(m_clock < m_next) {
			return;
		}
		m_next = m_clock + SampleUs;

		u32 level = g_currentArea ? g_currentArea.handleData() : 0;
		if(level != m_level) {
			std::fprintf(m_file, "level %u\n", unsigned(level));
			m_level = level;
		}
		std::fprintf(m_file, "%.2f %.1f %.1f %.1f %.3f %.3f\n", double(m_clock) / 1e6,
		             double(player.pos.x), double(player.pos.y), double(player.pos.z),
		             double(player.angle.getPitch()), double(player.angle.getYaw()));
		// The route must survive a crash or a power-off
		std::fflush(m_file);
	}

};

float lerpDegrees(float a, float b, float f) {
	float d = b - a;
	while(d > 180.f) {
		d -= 360.f;
	}
	while(d < -180.f) {
		d += 360.f;
	}
	return a + d * f;
}

class Replayer {

	std::vector<Sample> m_route;
	std::string m_save;
	bool m_loaded = false;
	bool m_placed = false;
	bool m_started = false;
	bool m_done = false;
	bool m_levelRequested = false;
	uint64_t m_settleUntil = 0;
	uint64_t m_quitAt = 0;
	uint64_t m_clock = 0;
	uint32_t m_seenFrame = 0;
	size_t m_cursor = 0;
	int m_savedFpsLimit = 0;

	std::vector<uint32_t> m_frame;
	std::vector<uint32_t> m_cpu;
	std::vector<uint32_t> m_swap;
	uint64_t m_phaseSum[PerfPhaseCount] = {};
	uint64_t m_zoneSum[PerfZoneCount] = {};
	uint64_t m_drawCalls = 0;
	uint64_t m_saveLoadUs = 0;
	unsigned m_loads = 0;
	uint64_t m_loadUs = 0;

	void requestLevel(u32 level, float yaw) {
		if(m_levelRequested) {
			return;
		}
		m_levelRequested = true;
		LogInfo << "[Bench] changing to level " << level;
		g_teleportToArea = AreaId(level);
		TELEPORT_TO_POSITION.clear();
		TELEPORT_TO_ANGLE = long(yaw);
		CHANGE_LEVEL_ICON = ChangeLevelNow;
	}

	static void applyPose(const Vec3f & pos, float pitch, float yaw) {
		player.pos = pos;
		g_moveto = pos;
		player.angle = Anglef(pitch, yaw, 0.f);
		player.desiredangle = player.angle;
		player.physics.velocity = Vec3f(0.f);
		player.physics.cyl.origin = player.basePosition();
		player.jumpphase = NotJumping;
		if(Entity * io = entities.player()) {
			io->pos = player.basePosition();
		}
	}

	void finish() {

		m_done = true;

		if(options().fps >= 0) {
			config.video.fpsLimit = m_savedFpsLimit;
		}
		ARX_PLAYER_Invulnerability(0);

		size_t n = m_frame.size();
		if(n == 0) {
			LogError << "[Bench] done route=" << options().route << " frames=0";
			return;
		}

		double avg = average(m_frame);
		double cpuAvg = average(m_cpu);
		double swapAvg = average(m_swap);
		unsigned over50 = 0;
		unsigned over100 = 0;
		for(uint32_t us : m_frame) {
			over50 += (us > 50000);
			over100 += (us > 100000);
		}
		std::sort(m_frame.begin(), m_frame.end());
		std::sort(m_cpu.begin(), m_cpu.end());
		std::sort(m_swap.begin(), m_swap.end());
		auto ms = [](double us) { return us / 1000.0; };

		char buf[640];
		std::snprintf(buf, sizeof(buf),
		              "route=%s frames=%u avg=%.1fms median=%.1f p90=%.1f p99=%.1f max=%.0f "
		              "fps_median=%.1f hitches>50ms=%u >100ms=%u cpu avg=%.1f median=%.1f p90=%.1f "
		              "upd=%.1f rend=%.1f aud=%.1f swap avg=%.1f p90=%.1f sleep=%.1f "
		              "loads=%u load_ms=%.0f save_load_ms=%.0f draws=%.0f heap_free=%uMB fps_limit=%d fog=%.0f build=%s",
		              options().route.c_str(), unsigned(n), ms(avg),
		              ms(percentile(m_frame, 0.5)), ms(percentile(m_frame, 0.9)),
		              ms(percentile(m_frame, 0.99)), ms(m_frame.back()),
		              1e6 / std::max(1.0, double(percentile(m_frame, 0.5))), over50, over100,
		              ms(cpuAvg), ms(percentile(m_cpu, 0.5)), ms(percentile(m_cpu, 0.9)),
		              ms(double(m_phaseSum[PerfUpdate]) / double(n)),
		              ms(double(m_phaseSum[PerfRender]) / double(n)),
		              ms(double(m_phaseSum[PerfAudio]) / double(n)),
		              ms(swapAvg), ms(percentile(m_swap, 0.9)),
		              ms(double(m_phaseSum[PerfSleep]) / double(n)),
		              m_loads, ms(double(m_loadUs)), ms(double(m_saveLoadUs)), double(m_drawCalls) / double(n),
		              unsigned(getFreeUserMemory() / (1024 * 1024)),
		              config.video.fpsLimit, double(config.vita.fogDistance), arx_version.c_str());

		std::string zones = formatZones(m_zoneSum, n, PerfZoneCount);
		// Zones first: the host stops following the log at "[Bench] done"
		LogInfo << "[Bench] zones " << zones;
		LogInfo << "[Bench] done " << buf;
		if(FILE * f = std::fopen(ResultsFile, "a")) {
			std::fprintf(f, "%s\n", buf);
			std::fprintf(f, "zones %s\n", zones.c_str());
			std::fclose(f);
		}
		Logger::flush();

		if(options().exit) {
			// Give the log server time to send the result before the process goes
			m_quitAt = perfNowUs() + 1500000;
		}
	}

	void collectLastFrame() {
		if(g_frameIndex == m_seenFrame) {
			return;
		}
		m_seenFrame = g_frameIndex;
		if(g_last.load) {
			m_loads++;
			m_loadUs += g_last.frameUs;
			return;
		}
		m_clock += g_last.frameUs;
		m_frame.push_back(g_last.frameUs);
		m_cpu.push_back(cpuUs(g_last));
		m_swap.push_back(g_last.phaseUs[PerfSwap]);
		for(size_t i = 0; i < PerfPhaseCount; i++) {
			m_phaseSum[i] += g_last.phaseUs[i];
		}
		for(size_t i = 0; i < PerfZoneCount; i++) {
			m_zoneSum[i] += g_last.zoneUs[i];
		}
		m_drawCalls += g_last.drawCalls;
	}

public:

	bool load() {

		if(m_loaded) {
			return !m_route.empty();
		}
		m_loaded = true;

		std::string path = benchPath(options().route);
		FILE * f = std::fopen(path.c_str(), "r");
		if(!f) {
			LogError << "[Bench] route missing, nothing to replay: " << path;
			m_done = true;
			return false;
		}

		char line[320];
		u32 level = 0;
		while(std::fgets(line, sizeof(line), f)) {
			line[std::strcspn(line, "\r\n")] = '\0';
			if(line[0] == '#') {
				continue;
			}
			if(std::strncmp(line, "save ", 5) == 0) {
				m_save = line + 5;
				continue;
			}
			unsigned l;
			if(std::sscanf(line, "level %u", &l) == 1) {
				level = l;
				continue;
			}
			Sample s;
			if(std::sscanf(line, "%f %f %f %f %f %f", &s.t, &s.pos.x, &s.pos.y, &s.pos.z,
			               &s.pitch, &s.yaw) == 6) {
				s.level = level;
				m_route.push_back(s);
			}
		}
		std::fclose(f);

		if(!options().save.empty()) {
			m_save = options().save;
		}

		if(m_route.size() < 2) {
			LogError << "[Bench] route " << path << " has " << m_route.size() << " samples";
			m_route.clear();
			m_done = true;
			return false;
		}

		LogInfo << "[Bench] replaying " << path << ": " << m_route.size() << " samples, "
		        << m_route.back().t << " s, settle " << options().settleSeconds << " s";
		m_frame.reserve(size_t(m_route.back().t * 70.f) + 64);
		m_cpu.reserve(m_frame.capacity());
		m_swap.reserve(m_frame.capacity());
		return true;
	}

	const std::string & save() const {
		return m_save;
	}

	void tick() {

		if(m_done) {
			if(m_quitAt && perfNowUs() >= m_quitAt) {
				m_quitAt = 0;
				mainApp->quit();
			}
			return;
		}
		if(!load()) {
			return;
		}

		const Sample & first = m_route.front();
		u32 level = g_currentArea ? g_currentArea.handleData() : 0;

		if(!m_placed) {
			if(first.level && level != first.level) {
				requestLevel(first.level, first.yaw);
				return;
			}
			m_placed = true;
			m_levelRequested = false;
			m_saveLoadUs = g_lastLoadUs;
			ARX_PLAYER_Invulnerability(1);
			if(options().fps >= 0) {
				m_savedFpsLimit = config.video.fpsLimit;
				config.video.fpsLimit = options().fps;
			}
			m_settleUntil = perfNowUs() + uint64_t(options().settleSeconds * 1e6f);
			LogInfo << "[Bench] at the route start, settling";
		}

		if(!m_started) {
			applyPose(first.pos, first.pitch, first.yaw);
			if(perfNowUs() < m_settleUntil) {
				return;
			}
			m_started = true;
			m_seenFrame = g_frameIndex;
			LogInfo << "[Bench] route started";
			return;
		}

		collectLastFrame();

		float t = float(double(m_clock) / 1e6);
		if(t >= m_route.back().t) {
			finish();
			return;
		}

		while(m_cursor + 1 < m_route.size() && m_route[m_cursor + 1].t <= t) {
			m_cursor++;
		}
		const Sample & a = m_route[m_cursor];
		const Sample & b = m_route[std::min(m_cursor + 1, m_route.size() - 1)];

		// Doors to other levels: change level like the recording did, then
		// carry on from the same route time (loading frames don't count).
		if(a.level && a.level != level) {
			requestLevel(a.level, a.yaw);
			applyPose(player.pos, a.pitch, a.yaw);
			return;
		}
		m_levelRequested = false;

		float span = b.t - a.t;
		bool sameLevel = (a.level == b.level);
		float f = (span > 0.f && sameLevel) ? glm::clamp((t - a.t) / span, 0.f, 1.f) : 0.f;
		applyPose(a.pos + (b.pos - a.pos) * f, lerpDegrees(a.pitch, b.pitch, f), lerpDegrees(a.yaw, b.yaw, f));
	}

};

Recorder g_recorder;
Replayer g_replayer;

} // anonymous namespace

fs::path benchSaveToLoad() {

	if(options().mode != ModeReplay || !g_replayer.load()) {
		return fs::path();
	}

	fs::path save = g_replayer.save();
	if(save.empty() || !fs::exists(save)) {
		LogError << "[Bench] save not found: \"" << save << "\" (record by loading a save, or set save=<path>)";
		return fs::path();
	}

	LogInfo << "[Bench] loading save " << save;
	return save;
}

void benchNoteSaveLoaded(const fs::path & savefile) {

	perfMarkLoading();
	loadBegin("save");

	g_lastLoadedSave = savefile;
	if(fs::is_directory(g_lastLoadedSave)) {
		g_lastLoadedSave /= "gsave.sav";
	}
	g_saveLoads++;
}

void benchAfterPlayerMovement() {

	// Only runs in game: menus and cinematics skip ArxGame::updateLevel
	g_current.inGame = true;

	switch(options().mode) {
		case ModeOff: return;
		case ModeRecord: g_recorder.tick(); return;
		case ModeReplay: g_replayer.tick(); return;
	}
}

bool benchReplayActive() {
	return options().mode == ModeReplay;
}

} // namespace vita
} // namespace platform

#endif // ARX_PLATFORM == ARX_PLATFORM_VITA
