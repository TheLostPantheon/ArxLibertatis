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

#ifndef ARX_PLATFORM_VITA_VITAPREFETCH_H
#define ARX_PLATFORM_VITA_VITAPREFETCH_H

#include "platform/Platform.h"

#if ARX_PLATFORM == ARX_PLATFORM_VITA

#include <string>

class Image;
class PakFile;
namespace res { class path; }

/*!
 * Level loads read and decompress their files on a worker thread, ahead of the main thread.
 *
 * Each level load records the files it reads, in order, into a manifest
 * (ux0:data/arx/cache/load-level<N>.txt). The next load of that level starts a
 * worker on an idle core that reads and decompresses those files in the same
 * order, up to a memory budget ahead of the main thread, which then takes the
 * data instead of waiting on the memory card and the decompressor. JPEG textures
 * are decoded there too. Files the worker hasn't reached yet, or that aren't in
 * the manifest, are read as before.
 *
 * Disabled when ux0:data/arx/noprefetch.txt exists.
 */
namespace platform {
namespace vita {
namespace prefetch {

//! A load of this level starts (the level is known): start prefetching its manifest.
void begin(unsigned level);

/*!
 * A PAK file is about to be read: give its prefetched content, if any (blocking
 * while the worker reads it), and note the read for the next manifest.
 * Returns false on the worker thread and outside prefetching loads.
 */
bool take(const PakFile * file, std::string & data);

/*!
 * A JPEG is about to be loaded as an image: give the worker's decoded copy, if it decoded
 * it with the same halving (blocking while it does), and note the read for the next manifest.
 */
bool takeImage(const res::path & path, bool halveLarge, Image & out, bool & halved);

//! A level load is prefetching (or recording what it reads).
bool active();

//! The load finished: stop the worker, save the manifest of what this load read.
void end();

} // namespace prefetch
} // namespace vita
} // namespace platform

#endif // ARX_PLATFORM == ARX_PLATFORM_VITA

#endif // ARX_PLATFORM_VITA_VITAPREFETCH_H
