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

#include "platform/vita/VitaPrefetch.h"

#if ARX_PLATFORM == ARX_PLATFORM_VITA

#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <pthread.h>

#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

#include "core/Config.h"
#include "graphics/image/Image.h"
#include "graphics/texture/Texture.h"
#include "io/Blast.h"
#include "io/log/Logger.h"
#include "io/resource/PakEntry.h"
#include "io/resource/PakReader.h"
#include "io/resource/ResourcePath.h"

namespace platform {
namespace vita {
namespace prefetch {

namespace {

const char * const CacheDir = "ux0:data/arx/cache";
const size_t Budget = 24 * 1024 * 1024; // decompressed bytes the workers may hold ahead of the main thread
// Two workers, on the cores of the lighting workers (idle while loading): decoding the JPEGs
// on top of reading and decompressing is more than one core keeps ahead of the main thread
const size_t WorkerCount = 2;

enum State : unsigned char {
	Pending, // the worker hasn't reached it
	Loading, // the worker is reading it
	Ready,   // read, waiting for the main thread
	Taken    // the main thread took it, or read it itself
};

struct Entry {
	const PakFile * file = nullptr;
	State state = Pending;
	std::string data;
	bool jpeg = false;  // decode it, as Image::load would
	bool ftl = false;   // models are imploded inside the pak too: ARX_FTL_Load takes them either way
	bool halve = false; // with the halving Texture::restore asks for
	bool halved = false;
	std::unique_ptr<Image> image; // replaces data once decoded
};

//! Memory an entry holds for the main thread.
size_t held(const Entry & entry) {
	return entry.data.size() + (entry.image ? entry.image->getSize() : 0);
}

struct Prefetcher {
	
	std::mutex mutex;
	std::condition_variable changed;
	
	bool active = false;     // a load is being recorded
	unsigned level = 0;
	
	std::vector<Entry> entries;
	std::unordered_map<const PakFile *, size_t> index;
	size_t buffered = 0;
	bool stop = false;
	bool threadRunning = false;
	size_t next = 0; // the next entry a worker takes
	pthread_t threads[WorkerCount];
	size_t threadCount = 0;
	SceUID workerIds[WorkerCount] = { -1, -1 };
	
	// what this load read, in order, for the next manifest
	std::vector<const PakFile *> recorded;
	std::unordered_set<const PakFile *> recordedSet;
	
	unsigned hits = 0;
	unsigned images = 0;
	unsigned waits = 0;
	unsigned misses = 0;
	u64 waitUs = 0;
	
};

Prefetcher g_prefetch;

bool disabled() {
	static const bool s_disabled = [] {
		SceIoStat st;
		return sceIoGetstat("ux0:data/arx/noprefetch.txt", &st) >= 0;
	}();
	return s_disabled;
}

std::string manifestPath(unsigned level) {
	return std::string(CacheDir) + "/load-level" + std::to_string(level) + ".txt";
}

void * workerMain(void * arg) {
	
	Prefetcher & p = g_prefetch;
	size_t worker = size_t(reinterpret_cast<uintptr_t>(arg));
	p.workerIds[worker] = sceKernelGetThreadId();
	sceKernelChangeThreadCpuAffinityMask(p.workerIds[worker], worker == 0 ? (1 << 2) : (1 << 1));
	
	std::unique_lock<std::mutex> lock(p.mutex);
	for(;;) {
		
		p.changed.wait(lock, [&] { return p.stop || p.next >= p.entries.size() || p.buffered < Budget; });
		if(p.stop || p.next >= p.entries.size()) {
			break;
		}
		Entry & entry = p.entries[p.next++];
		if(entry.state != Pending) {
			continue; // the main thread got there first
		}
		entry.state = Loading;
		const PakFile * file = entry.file;
		const bool jpeg = entry.jpeg;
		const bool halve = entry.halve;
		const bool ftl = entry.ftl;
		lock.unlock();
		
		std::string data = file->read();
		if(ftl && data.size() >= 3 && data.compare(0, 3, "FTL") != 0) {
			std::string model = blast(data);
			if(model.size() >= 3 && model.compare(0, 3, "FTL") == 0) {
				data = std::move(model);
			}
		}
		std::unique_ptr<Image> image;
		bool halved = false;
		if(jpeg && !data.empty()) {
			image = std::make_unique<Image>();
			if(decodeJpegImage(data, halve, *image, halved)) {
				data = std::string(); // the main thread takes the image instead
			} else {
				image.reset();
			}
		}
		
		lock.lock();
		entry.data = std::move(data);
		entry.image = std::move(image);
		entry.halved = halved;
		entry.state = Ready;
		p.buffered += held(entry);
		p.changed.notify_all();
	}
	
	return nullptr;
}

bool isWorker(SceUID thread) {
	const Prefetcher & p = g_prefetch;
	for(SceUID id : p.workerIds) {
		if(id == thread) {
			return true;
		}
	}
	return false;
}

void stopWorker() {
	Prefetcher & p = g_prefetch;
	if(!p.threadRunning) {
		return;
	}
	{
		std::scoped_lock lock(p.mutex);
		p.stop = true;
	}
	p.changed.notify_all();
	for(size_t i = 0; i < p.threadCount; i++) {
		pthread_join(p.threads[i], nullptr);
	}
	p.threadCount = 0;
	p.threadRunning = false;
	for(SceUID & id : p.workerIds) {
		id = -1;
	}
}

//! PakFile -> path for everything in the paks (built once, for writing manifests)
void addPaths(PakDirectory & dir, const std::string & prefix,
              std::unordered_map<const PakFile *, std::string> & out) {
	for(auto file : dir.files()) {
		const std::string & name = file;
		PakFile & entry = file;
		out.emplace(&entry, prefix + name);
	}
	for(auto sub : dir.dirs()) {
		const std::string & name = sub;
		PakDirectory & subdir = sub;
		addPaths(subdir, prefix + name + "/", out);
	}
}

const std::unordered_map<const PakFile *, std::string> & pakPaths() {
	static std::unordered_map<const PakFile *, std::string> s_paths;
	if(s_paths.empty() && g_resources) {
		addPaths(*g_resources, std::string(), s_paths);
	}
	return s_paths;
}

void writeManifest(unsigned level, const std::vector<const PakFile *> & files) {
	
	const auto & paths = pakPaths();
	sceIoMkdir(CacheDir, 0777);
	std::string path = manifestPath(level);
	FILE * f = std::fopen(path.c_str(), "w");
	if(!f) {
		LogWarning << "[Prefetch] cannot write " << path;
		return;
	}
	size_t written = 0;
	for(const PakFile * file : files) {
		auto it = paths.find(file);
		if(it != paths.end()) {
			std::fprintf(f, "%s\n", it->second.c_str());
			written++;
		}
	}
	std::fclose(f);
	LogInfo << "[Prefetch] saved the read order of level " << level << ": " << written << " files";
}

//! The main thread reads file: record it for the next manifest.
void note(const PakFile * file) {
	Prefetcher & p = g_prefetch;
	if(p.recordedSet.insert(file).second) {
		p.recorded.push_back(file);
	}
}

} // anonymous namespace

void begin(unsigned level) {
	
	Prefetcher & p = g_prefetch;
	if(disabled() || p.active || !g_resources) {
		return;
	}
	
	p.active = true;
	p.level = level;
	p.entries.clear();
	p.index.clear();
	p.recorded.clear();
	p.recordedSet.clear();
	p.buffered = 0;
	p.stop = false;
	p.hits = p.images = p.waits = p.misses = 0;
	p.waitUs = 0;
	
	FILE * f = std::fopen(manifestPath(level).c_str(), "r");
	if(!f) {
		LogInfo << "[Prefetch] no read order for level " << level << " yet: recording it";
		return;
	}
	char line[512];
	while(std::fgets(line, sizeof(line), f)) {
		line[std::strcspn(line, "\r\n")] = '\0';
		if(!line[0]) {
			continue;
		}
		res::path path = res::path::load(line);
		PakFile * file = g_resources->getFile(path);
		if(file && p.index.emplace(file, p.entries.size()).second) {
			Entry & entry = p.entries.emplace_back();
			entry.file = file;
			entry.jpeg = (path.ext() == ".jpg");
			entry.ftl = (path.ext() == ".ftl");
			entry.halve = entry.jpeg && config.video.textureDownscale && !isInterfaceTexturePath(path);
		}
	}
	std::fclose(f);
	
	if(p.entries.empty()) {
		return;
	}
	
	// The decoder builds its tables on first use: not from two threads at once
	blastInitTables();
	
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, 256 * 1024);
	p.next = 0;
	p.threadCount = 0;
	for(size_t i = 0; i < WorkerCount; i++) {
		if(pthread_create(&p.threads[p.threadCount], &attr, workerMain,
		                  reinterpret_cast<void *>(uintptr_t(i))) == 0) {
			p.threadCount++;
		}
	}
	p.threadRunning = (p.threadCount > 0);
	pthread_attr_destroy(&attr);
	
	LogInfo << "[Prefetch] level " << level << ": prefetching " << p.entries.size() << " files";
}

bool take(const PakFile * file, std::string & data) {
	
	Prefetcher & p = g_prefetch;
	if(!p.active) {
		return false;
	}
	SceUID self = sceKernelGetThreadId();
	if(isWorker(self)) {
		return false;
	}
	
	note(file);
	
	if(!p.threadRunning) {
		return false;
	}
	
	std::unique_lock<std::mutex> lock(p.mutex);
	auto it = p.index.find(file);
	if(it == p.index.end()) {
		p.misses++;
		return false;
	}
	Entry & entry = p.entries[it->second];
	if(entry.state == Loading) {
		u64 start = sceKernelGetProcessTimeWide();
		p.changed.wait(lock, [&] { return entry.state == Ready; });
		p.waits++;
		p.waitUs += sceKernelGetProcessTimeWide() - start;
	}
	if(entry.state == Ready && entry.image) {
		return false; // decoded for takeImage(): the raw file is read again
	}
	switch(entry.state) {
		case Taken: {
			return false;
		}
		case Pending: {
			// The worker is behind: read it here
			entry.state = Taken;
			p.misses++;
			p.changed.notify_all();
			return false;
		}
		case Loading: {
			u64 start = sceKernelGetProcessTimeWide();
			p.changed.wait(lock, [&] { return entry.state == Ready; });
			p.waits++;
			p.waitUs += sceKernelGetProcessTimeWide() - start;
			break;
		}
		case Ready: {
			break;
		}
	}
	
	entry.state = Taken;
	p.buffered -= held(entry);
	data = std::move(entry.data);
	entry.data = std::string();
	p.hits++;
	p.changed.notify_all();
	return true;
}

bool takeImage(const res::path & path, bool halveLarge, Image & out, bool & halved) {
	
	Prefetcher & p = g_prefetch;
	if(!p.active || !g_resources || isWorker(sceKernelGetThreadId())) {
		return false;
	}
	const PakFile * file = g_resources->getFile(path);
	if(!file) {
		return false;
	}
	
	note(file);
	
	if(!p.threadRunning) {
		return false;
	}
	
	std::unique_lock<std::mutex> lock(p.mutex);
	auto it = p.index.find(file);
	if(it == p.index.end()) {
		return false; // take() counts the miss
	}
	Entry & entry = p.entries[it->second];
	if(entry.state == Loading) {
		u64 start = sceKernelGetProcessTimeWide();
		p.changed.wait(lock, [&] { return entry.state == Ready; });
		p.waits++;
		p.waitUs += sceKernelGetProcessTimeWide() - start;
	}
	if(entry.state != Ready || !entry.image) {
		return false; // not reached yet (take() reads it), or not decoded (take() gives the file)
	}
	
	p.buffered -= held(entry);
	entry.state = Taken;
	p.changed.notify_all();
	if(entry.halve != halveLarge) {
		entry.image.reset(); // decoded at another size than asked: read it again
		return false;
	}
	out.swap(*entry.image);
	entry.image.reset();
	halved = entry.halved;
	p.hits++;
	p.images++;
	return true;
}

bool active() {
	return g_prefetch.active;
}

void end() {
	
	Prefetcher & p = g_prefetch;
	if(!p.active) {
		return;
	}
	
	stopWorker();
	
	size_t unused = 0;
	for(const Entry & entry : p.entries) {
		unused += (entry.state == Ready) ? 1 : 0;
	}
	if(!p.entries.empty()) {
		LogInfo << "[Prefetch] level " << p.level << ": " << p.hits << " prefetched (" << p.images
		        << " decoded images, " << p.waits
		        << " waited " << (p.waitUs / 1000) << " ms), " << p.misses << " read directly, "
		        << unused << " unused";
	}
	
	if(!p.recorded.empty()) {
		writeManifest(p.level, p.recorded);
	}
	
	p.entries.clear();
	p.entries.shrink_to_fit();
	p.index.clear();
	p.recorded.clear();
	p.recordedSet.clear();
	p.buffered = 0;
	p.active = false;
}

} // namespace prefetch
} // namespace vita
} // namespace platform

#endif // ARX_PLATFORM == ARX_PLATFORM_VITA
