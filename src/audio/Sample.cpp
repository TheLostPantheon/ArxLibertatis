/*
 * Copyright 2011-2022 Arx Libertatis Team (see the AUTHORS file)
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
/* Based on:
===========================================================================
ARX FATALIS GPL Source Code
Copyright (C) 1999-2010 Arkane Studios SA, a ZeniMax Media company.

This file is part of the Arx Fatalis GPL Source Code ('Arx Fatalis Source Code'). 

Arx Fatalis Source Code is free software: you can redistribute it and/or modify it under the terms of the GNU General Public 
License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version.

Arx Fatalis Source Code is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied 
warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with Arx Fatalis Source Code.  If not, see 
<http://www.gnu.org/licenses/>.

In addition, the Arx Fatalis Source Code is also subject to certain additional terms. You should have received a copy of these 
additional terms immediately following the terms and conditions of the GNU General Public License which accompanied the Arx 
Fatalis Source Code. If not, please request a copy in writing from Arkane Studios at the address below.

If you have questions concerning this license or the applicable additional terms, you may contact in writing Arkane Studios, c/o 
ZeniMax Media Inc., Suite 120, Rockville, Maryland 20850 USA.
===========================================================================
*/

#include "audio/Sample.h"

#include "platform/Platform.h"
#if ARX_PLATFORM == ARX_PLATFORM_VITA
#include <psp2/kernel/processmgr.h>
#endif

#include <string>
#include <unordered_map>
#include <utility>

#include "audio/AudioGlobal.h"
#include "audio/Stream.h"
#include "audio/AudioBackend.h"
#include "audio/AudioSource.h"

#include "platform/Platform.h"
#include "platform/vita/VitaBench.h"

#if ARX_PLATFORM == ARX_PLATFORM_VITA
#include "audio/codec/WAV.h"
#include "io/log/Logger.h"
#include "io/resource/PakReader.h"
#include "platform/vita/VitaInit.h"
#include "platform/vita/VitaPrefetch.h"
#endif

namespace audio {

Sample::Sample(res::path name)
	: m_name(std::move(name))
	, m_length(0)
	, m_format()
{}

Sample::~Sample() {

	// Delete sources referencing this sample.
	for(Backend::source_iterator p = backend->sourcesBegin(); p != backend->sourcesEnd();) {
		#if ARX_PLATFORM == ARX_PLATFORM_VITA
		if(*p && !platform::vita::isVtableValid(*p)) {
			const_cast<Source *&>(*p) = nullptr;
			++p;
			continue;
		}
		#endif
		if(*p && (*p)->getSample() == this) {
			p = backend->deleteSource(p);
		} else {
			++p;
		}
	}

}

aalError Sample::load() {
	
	VITA_LOAD_ZONE(LoadAudio);
	if(m_length != 0) {
		return AAL_ERROR_INIT;
	}
	
	#if ARX_PLATFORM == ARX_PLATFORM_VITA
	struct SlowLoadLog {
		const res::path & name;
		u64 start = sceKernelGetProcessTimeWide();
		~SlowLoadLog() {
			u64 us = sceKernelGetProcessTimeWide() - start;
			if(us > 5000) {
				LogInfo << "[SlowSample] header of " << name << ": " << float(us) / 1000.f << " ms";
			}
		}
	} slowLoadLog{ m_name };
	#endif
	
#if ARX_PLATFORM == ARX_PLATFORM_VITA
	// Opening a sample on the memory card costs ~6 ms: remember the formats already read
	static std::unordered_map<std::string, std::pair<PCMFormat, size_t>> s_headers;
	if(auto it = s_headers.find(m_name.string()); it != s_headers.end()) {
		m_format = it->second.first;
		m_length = it->second.second;
		return AAL_OK;
	}
	
	// Use file-backed handle for metadata-only read to avoid
	// pre-loading entire file just to get format and length.
	// While a level loads, read the whole file instead: the prefetcher notes it for the next
	// load of the level, whose workers then have it in memory.
	std::unique_ptr<PakFileHandle> file;
	if(platform::vita::prefetch::active()) {
		std::string data = g_resources->read(m_name);
		if(!data.empty()) {
			file = std::make_unique<MemoryPakFileHandle>(std::move(data));
		}
	} else {
		file = g_resources->open(m_name);
	}
	if(!file) {
		return AAL_ERROR_FILEIO;
	}
	file->seek(SeekSet, 0);
	std::unique_ptr<Stream> stream = std::make_unique<StreamWAV>();
	if(stream->setStream(std::move(file))) {
		return AAL_ERROR_FILEIO;
	}
#else
	std::unique_ptr<Stream> stream = createStream(m_name);
	if(!stream) {
		return AAL_ERROR_FILEIO;
	}
#endif
	
	stream->getFormat(m_format);
	m_length = stream->getLength();
	
	#if ARX_PLATFORM == ARX_PLATFORM_VITA
	s_headers.emplace(m_name.string(), std::make_pair(m_format, m_length));
	#endif
	
	return AAL_OK;
}

} // namespace audio
