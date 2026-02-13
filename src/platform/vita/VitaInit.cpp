#include "platform/vita/VitaInit.h"

#if ARX_PLATFORM == ARX_PLATFORM_VITA

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <new>
#include <malloc.h>
#include <pthread.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/power.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include "io/log/Logger.h"

// These symbols MUST be at file scope with C linkage — the Vita loader and
// newlib crt0 resolve them as unmangled C symbols at process startup.
// Inside a C++ namespace they get name-mangled and the loader can't find them,
// silently falling back to defaults (wrong heap size, 256KB stack → overflow).
extern "C" {
unsigned int _newlib_heap_size_user = 220 * 1024 * 1024;
unsigned int sceUserMainThreadStackSize = 1 * 1024 * 1024;
}

namespace platform {
namespace vita {

static char s_emergencyBuf[256];
static size_t s_minHeapFree = SIZE_MAX;
static SceUID s_debugLogFd = -1;

// --- Missing Functions Required by the Engine ---

size_t getFreeUserMemory() {
    struct mallinfo mi = mallinfo();
    return (size_t)mi.fordblks;
}

void resetMemoryWatermarks() {
    s_minHeapFree = SIZE_MAX;
    LogInfo << "[VitaMem] Watermarks reset";
}

void logMemoryStatus(const char * label) {
    struct mallinfo mi = mallinfo();
    size_t heapFree = (size_t)mi.fordblks;

    if (heapFree < s_minHeapFree) {
        s_minHeapFree = heapFree;
    }

    // Optimization: Log to engine logger (stdout/file)
    // but only do the heavy string formatting once.
    LogInfo << "[VitaMem] " << label
            << " | used: " << (mi.uordblks / 1024) << "KB"
            << " | free: " << (heapFree / 1024) << "KB"
            << " | min: " << (s_minHeapFree / 1024) << "KB";
}

// --- Internal Logic ---

static void vitaNewHandler() {
    struct mallinfo mi = mallinfo();
    SceUID fd = sceIoOpen("ux0:data/arx/critical_oom.log",
                          SCE_O_WRONLY | SCE_O_APPEND | SCE_O_CREAT, 0777);
    if(fd >= 0) {
        int len = snprintf(s_emergencyBuf, sizeof(s_emergencyBuf),
                           "[OOM] Heap: %uKB used / %uKB free.\n",
                           (unsigned)(mi.uordblks / 1024),
                           (unsigned)(mi.fordblks / 1024));
        if(len > 0) sceIoWrite(fd, s_emergencyBuf, (size_t)len);
        sceIoClose(fd);
    }
    // Don't throw std::bad_alloc — nothing catches it, causing
    // std::terminate → abort → UDF instruction crash.
    // Exit cleanly so the crash dump and OOM log are useful.
    sceKernelExitProcess(1);
}

void debugLog(const char * msg) {
    // Basic low-level debug write
    if (s_debugLogFd < 0) {
        s_debugLogFd = sceIoOpen("ux0:data/arx/debug.log",
                                 SCE_O_WRONLY | SCE_O_APPEND | SCE_O_CREAT, 0777);
    }
    if (s_debugLogFd >= 0) {
        int len = snprintf(s_emergencyBuf, sizeof(s_emergencyBuf), "%s\n", msg);
        if(len > 0) sceIoWrite(s_debugLogFd, s_emergencyBuf, std::min((size_t)len, sizeof(s_emergencyBuf) - 1));
    }
}

void ensureDataDirectories() {
    sceIoMkdir("ux0:data/arx", 0777);
    sceIoMkdir("ux0:data/arx/config", 0777);
    sceIoMkdir("ux0:data/arx/saves", 0777);
}

void initClocks() {
    scePowerSetArmClockFrequency(444);
    scePowerSetGpuClockFrequency(222);
    scePowerSetBusClockFrequency(222);
    LogInfo << "Vita clocks: CPU=" << scePowerGetArmClockFrequency()
            << "MHz GPU=" << scePowerGetGpuClockFrequency()
            << "MHz Bus=" << scePowerGetBusClockFrequency() << "MHz";
}

void initialize() {
    ensureDataDirectories();

    sceIoRemove("ux0:data/arx/debug.log.prev");
    sceIoRename("ux0:data/arx/debug.log", "ux0:data/arx/debug.log.prev");

    std::set_new_handler(vitaNewHandler);
    debugLog("Vita Platform Initialized: 220MB Heap, 1MB Main Stack.");

    initLightingWorker();
}

// --- Parallel Worker Thread Pool ---
// Uses all 3 Vita userland cores (core 0 = main, cores 1-2 = workers).
// Core 3 is reserved by the system.

static const int NUM_WORKERS = 2;
static const int TOTAL_THREADS = NUM_WORKERS + 1; // workers + main

namespace {

struct WorkerPool {
	pthread_t threads[NUM_WORKERS];
	pthread_mutex_t mutex;
	pthread_cond_t workReady;
	pthread_cond_t allDone;
	VitaParallelFunc func;
	void * ctx;
	size_t totalCount;
	unsigned int generation;
	int workersActive;
	bool shutdown;
	bool initialized;
};

WorkerPool g_pool = {};

void * workerThread(void * arg) {
	int id = static_cast<int>(reinterpret_cast<intptr_t>(arg));

	// Pin each worker to its own core (worker 0 → core 1, worker 1 → core 2)
	sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(), 1 << (id + 1));

	unsigned int myGeneration = 0;

	pthread_mutex_lock(&g_pool.mutex);
	while(!g_pool.shutdown) {
		while(g_pool.generation == myGeneration && !g_pool.shutdown) {
			// Use timedwait to recover from potentially missed broadcasts
			struct timespec ts;
			clock_gettime(CLOCK_REALTIME, &ts);
			ts.tv_nsec += 100000000; // 100ms
			if(ts.tv_nsec >= 1000000000) {
				ts.tv_sec++;
				ts.tv_nsec -= 1000000000;
			}
			pthread_cond_timedwait(&g_pool.workReady, &g_pool.mutex, &ts);
		}
		if(g_pool.shutdown) {
			break;
		}
		myGeneration = g_pool.generation;

		// Compute this worker's chunk
		size_t chunkSize = (g_pool.totalCount + TOTAL_THREADS - 1) / TOTAL_THREADS;
		size_t start = static_cast<size_t>(id + 1) * chunkSize;
		size_t end = std::min(start + chunkSize, g_pool.totalCount);
		VitaParallelFunc func = g_pool.func;
		void * ctx = g_pool.ctx;

		pthread_mutex_unlock(&g_pool.mutex);

		for(size_t i = start; i < end; i++) {
			func(ctx, i);
		}

		pthread_mutex_lock(&g_pool.mutex);
		g_pool.workersActive--;
		if(g_pool.workersActive == 0) {
			pthread_cond_signal(&g_pool.allDone);
		}
	}
	pthread_mutex_unlock(&g_pool.mutex);
	return nullptr;
}

} // anonymous namespace

void initLightingWorker() {
	if(g_pool.initialized) {
		return;
	}

	pthread_mutex_init(&g_pool.mutex, nullptr);
	pthread_cond_init(&g_pool.workReady, nullptr);
	pthread_cond_init(&g_pool.allDone, nullptr);
	g_pool.shutdown = false;
	g_pool.generation = 0;
	g_pool.workersActive = 0;

	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, 128 * 1024);

	for(int i = 0; i < NUM_WORKERS; i++) {
		pthread_create(&g_pool.threads[i], &attr, workerThread,
		               reinterpret_cast<void *>(static_cast<intptr_t>(i)));
	}

	pthread_attr_destroy(&attr);
	g_pool.initialized = true;
	LogInfo << "[Vita] Worker pool created: " << NUM_WORKERS
	        << " workers (128KB stack each, cores 1-" << NUM_WORKERS << ")";
}

void parallelFor(VitaParallelFunc func, void * ctx, size_t count) {
	if(!g_pool.initialized || count < static_cast<size_t>(TOTAL_THREADS)) {
		for(size_t i = 0; i < count; i++) {
			func(ctx, i);
		}
		return;
	}

	size_t chunkSize = (count + TOTAL_THREADS - 1) / TOTAL_THREADS;

	// Wake all workers
	pthread_mutex_lock(&g_pool.mutex);
	g_pool.func = func;
	g_pool.ctx = ctx;
	g_pool.totalCount = count;
	g_pool.workersActive = NUM_WORKERS;
	g_pool.generation++;
	pthread_cond_broadcast(&g_pool.workReady);
	pthread_mutex_unlock(&g_pool.mutex);

	// Main thread processes chunk 0
	size_t end = std::min(chunkSize, count);
	for(size_t i = 0; i < end; i++) {
		func(ctx, i);
	}

	// Wait for all workers to finish (with timeout + re-broadcast to recover from missed signals)
	pthread_mutex_lock(&g_pool.mutex);
	while(g_pool.workersActive > 0) {
		struct timespec ts;
		clock_gettime(CLOCK_REALTIME, &ts);
		ts.tv_nsec += 50000000; // 50ms timeout
		if(ts.tv_nsec >= 1000000000) {
			ts.tv_sec++;
			ts.tv_nsec -= 1000000000;
		}
		int ret = pthread_cond_timedwait(&g_pool.allDone, &g_pool.mutex, &ts);
		if(ret == ETIMEDOUT && g_pool.workersActive > 0) {
			// Worker may have missed the broadcast — re-signal
			pthread_cond_broadcast(&g_pool.workReady);
		}
	}
	pthread_mutex_unlock(&g_pool.mutex);
}

} // namespace vita
} // namespace platform

#endif