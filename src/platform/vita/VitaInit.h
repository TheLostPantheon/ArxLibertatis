/*
 * Copyright 2024 Arx Libertatis Team (see the AUTHORS file)
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

#ifndef ARX_PLATFORM_VITA_VITAINIT_H
#define ARX_PLATFORM_VITA_VITAINIT_H

#include <cstddef>
#include <cstdint>

#include "platform/Platform.h"

#if ARX_PLATFORM == ARX_PLATFORM_VITA

namespace platform {
namespace vita {

// Vita native display resolution.
constexpr int kDisplayWidth  = 960;
constexpr int kDisplayHeight = 544;

// Vita internal render resolution — lower than native for GPU performance.
// An FBO at this resolution is blitted to the native framebuffer each frame.
constexpr int kRenderWidth  = 720;
constexpr int kRenderHeight = 408;

void initialize();
void initClocks();
void ensureDataDirectories();
void debugLog(const char * msg);
void logMemoryStatus(const char * label);
size_t getFreeUserMemory();
void resetMemoryWatermarks();

//! Initialize the persistent lighting worker thread (call once at startup).
void initLightingWorker();

//! Fork/join parallel-for using main thread + 1 worker thread.
//! Splits [0..count) into two halves processed concurrently.
//! Falls back to serial if count <= 2 or worker not initialized.
typedef void (*VitaParallelFunc)(void * ctx, size_t idx);
void parallelFor(VitaParallelFunc func, void * ctx, size_t count);

// Vita module memory layout constants for vtable corruption detection.
// arx@1 LOAD segment 0 (RX) starts at 0x81000000, segment 1 (RW) at 0x81440000.
// .text starts at 0x81000040, .rodata at ~0x8133ef00, .data at 0x81440040.
constexpr uintptr_t kModuleBase       = 0x81000000u; // Start of RX segment
constexpr uintptr_t kModuleEnd        = 0x82000000u; // Conservative upper bound
constexpr uintptr_t kCodeSegmentStart = 0x81000000u; // Start of .init/.text (RX)
constexpr uintptr_t kCodeSegmentEnd   = 0x81440000u; // Start of RW segment (.data)

// Upper bound for valid user-space pointers. Vita user processes get up to ~512MB
// starting at kModuleBase. Anything above 0xC0000000 is definitely kernel/invalid.
constexpr uintptr_t kUserSpaceEnd     = 0xC0000000u;

//! Check if a pointer looks like a valid user-space address (for non-vtable objects like Ambiance).
inline bool isValidUserPointer(const void * ptr) {
	uintptr_t addr = reinterpret_cast<uintptr_t>(ptr);
	return addr >= kModuleBase && addr < kUserSpaceEnd;
}

//! Check if a vtable pointer looks valid (points into module range and first entry is in code).
//! Uses volatile reads so the compiler cannot optimize away the check via LTO.
inline bool isVtableValid(const void * obj) {
	if(!obj) {
		return false;
	}
	uintptr_t vtable = *static_cast<const volatile uintptr_t *>(
		reinterpret_cast<const void *>(obj));
	if(vtable < kModuleBase || vtable >= kModuleEnd) {
		return false;
	}
	uintptr_t fn = *reinterpret_cast<const volatile uintptr_t *>(vtable);
	// Strip ARM Thumb bit before range check
	return (fn & ~1u) >= kCodeSegmentStart && (fn & ~1u) < kCodeSegmentEnd;
}

} // namespace vita
} // namespace platform

#endif // ARX_PLATFORM == ARX_PLATFORM_VITA

// Crash-safe breadcrumb: writes directly to debug.log (open/write/close per call).
// Compiles to nothing on non-Vita.
#if ARX_PLATFORM == ARX_PLATFORM_VITA
#define VITA_CHECKPOINT(msg) platform::vita::debugLog(msg)
#else
#define VITA_CHECKPOINT(msg) ((void)0)
#endif

#endif // ARX_PLATFORM_VITA_VITAINIT_H
