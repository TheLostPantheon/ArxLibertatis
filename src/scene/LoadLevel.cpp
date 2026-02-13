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
// Code: Cyril Meynier
//
// Copyright (c) 1999-2000 ARKANE Studios SA. All rights reserved

#include "scene/LoadLevel.h"

#include <stddef.h>
#include <cstdio>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <utility>

#include <boost/algorithm/string/predicate.hpp>

#include "ai/PathFinderManager.h"
#include "ai/Paths.h"

#include "core/Application.h"
#include "core/GameTime.h"
#include "core/Config.h"
#include "core/Core.h"

#include "game/EntityManager.h"
#include "game/Levels.h"
#include "game/Player.h"

#include "gui/LoadLevelScreen.h"
#include "gui/MiniMap.h"
#include "gui/Interface.h"

#include "graphics/Math.h"
#include "graphics/data/FTL.h"
#include "graphics/data/TextureContainer.h"
#include "graphics/effects/Fade.h"
#include "graphics/effects/Fog.h"
#include "graphics/particle/ParticleEffects.h"

#include "platform/Platform.h"
#if ARX_PLATFORM == ARX_PLATFORM_VITA
#include "platform/vita/VitaInit.h"
#endif

#include "io/resource/ResourcePath.h"
#include "io/resource/PakReader.h"
#include "io/Blast.h"
#include "io/log/Logger.h"

#include "physics/CollisionShapes.h"

#include "audio/Audio.h"

#include "scene/Object.h"
#include "scene/GameSound.h"
#include "scene/Interactive.h"
#include "scene/LevelFormat.h"
#include "scene/Light.h"
#include "scene/Tiles.h"

#include "util/Range.h"
#include "util/String.h"


extern bool bGCroucheToggle;

Entity * LoadInter_Ex(const res::path & classPath, EntityInstance instance,
                      const Vec3f & pos, const Anglef & angle) {
	
	if(Entity * entity = entities.getById(EntityId(classPath, instance))) {
		return entity;
	}
	
	arx_assert(instance != 0);
	
	Entity * io = AddInteractive(classPath, instance, NO_MESH | NO_ON_LOAD);
	if(!io) {
		return nullptr;
	}
	
	RestoreInitialIOStatusOfIO(io);
	ARX_INTERACTIVE_HideGore(io);
	
	io->lastpos = io->initpos = io->pos = pos;
	io->move = Vec3f(0.f);
	io->initangle = io->angle = angle;
	
	if(PakDirectory * dir = g_resources->getDirectory(io->instancePath())) {
		loadScript(io->over_script, dir->getFile(io->className() + ".asl"));
	}
	
	if(SendIOScriptEvent(nullptr, io, SM_LOAD) == ACCEPT && io->obj == nullptr) {
		bool pbox = (io->ioflags & IO_ITEM) == IO_ITEM;
		io->obj = loadObject(io->classPath() + ".teo", pbox).release();
		if(io->ioflags & IO_NPC) {
			EERIE_COLLISION_Cylinder_Create(io);
		}
	}
	
	return io;
}

struct ToColorBGRA {
	template <typename T>
	ColorBGRA operator()(const T & bgra) const {
		return ColorBGRA(bgra);
	}
};

static std::vector<ColorBGRA> g_levelLighting;

static void loadLights(const char * dat, size_t & pos, size_t count, const Vec3f & trans = Vec3f(0.f)) {
	
	EERIE_LIGHT_GlobalInit();
	g_staticLights.reserve(count);
	
	for(size_t i = 0; i < count; i++) {
		
		const DANAE_LS_LIGHT * dlight = reinterpret_cast<const DANAE_LS_LIGHT *>(dat + pos);
		pos += sizeof(DANAE_LS_LIGHT);
		
		EERIE_LIGHT & light = g_staticLights.emplace_back();
		
		light.m_exists = true;
		light.m_isVisible = true;
		light.fallend = dlight->fallend;
		light.fallstart = dlight->fallstart;
		light.falldiffmul = 1.f / (light.fallend - light.fallstart);
		light.intensity = dlight->intensity;
		
		light.pos = dlight->pos.toVec3() + trans;
		
		light.rgb = dlight->rgb;
		
		light.extras = ExtrasType::load(dlight->extras);
		
		light.ex_flicker = dlight->ex_flicker;
		light.ex_radius = dlight->ex_radius;
		light.ex_frequency = dlight->ex_frequency;
		light.ex_size = dlight->ex_size;
		light.ex_speed = dlight->ex_speed;
		light.ex_flaresize = dlight->ex_flaresize;
		
		light.m_ignitionStatus = !(light.extras & EXTRAS_STARTEXTINGUISHED);
		
		if((light.extras & EXTRAS_SPAWNFIRE) && !(light.extras & EXTRAS_FLARE)) {
			light.extras |= EXTRAS_FLARE;
			if(light.extras & EXTRAS_FIREPLACE) {
				light.ex_flaresize = 95.f;
			} else {
				light.ex_flaresize = 80.f;
			}
		}
		
		light.m_ignitionLightHandle = { };
		light.sample = { };
		
	}
	
}

static void loadLighting(const char * dat, size_t & pos, size_t bufferSize,
                         bool compact, bool skip = false) {

	if(arx_unlikely(pos + sizeof(DANAE_LS_LIGHTINGHEADER) > bufferSize)) {
		LogError << "Truncated lighting header";
		return;
	}
	const DANAE_LS_LIGHTINGHEADER * dll = reinterpret_cast<const DANAE_LS_LIGHTINGHEADER *>(dat + pos);
	pos += sizeof(DANAE_LS_LIGHTINGHEADER);

	size_t count = dll->nb_values;

	if(!skip) {
		g_levelLighting.resize(count);
	}

	if(compact) {
		size_t dataBytes = sizeof(u32) * count;
		if(arx_unlikely(count != 0 && dataBytes / count != sizeof(u32))) {
			LogError << "Lighting data count overflow";
			return;
		}
		if(arx_unlikely(pos + dataBytes > bufferSize)) {
			LogError << "Truncated compact lighting data";
			return;
		}
		if(!skip) {
			const u32 * begin = reinterpret_cast<const u32 *>(dat + pos);
			std::transform(begin, begin + count, g_levelLighting.begin(), ToColorBGRA());
		}
		pos += dataBytes;
	} else {
		size_t dataBytes = sizeof(DANAE_LS_VLIGHTING) * count;
		if(arx_unlikely(count != 0 && dataBytes / count != sizeof(DANAE_LS_VLIGHTING))) {
			LogError << "Lighting data count overflow";
			return;
		}
		if(arx_unlikely(pos + dataBytes > bufferSize)) {
			LogError << "Truncated lighting data";
			return;
		}
		if(!skip) {
			const DANAE_LS_VLIGHTING * begin = reinterpret_cast<const DANAE_LS_VLIGHTING *>(dat + pos);
			std::transform(begin, begin + count, g_levelLighting.begin(), ToColorBGRA());
		}
		pos += dataBytes;
	}

}

bool DanaeLoadLevel(AreaId area, bool loadEntities) {
	
	arx_assume(area);
	
	g_currentArea = area;
	
	const std::string levelId = std::to_string(u32(area));
	const res::path file = "graph/levels/level" + levelId + "/level" + levelId + ".dlf";
	
	LogInfo << "Loading level " << file;

	#if ARX_PLATFORM == ARX_PLATFORM_VITA
	size_t preLoadFreeMem = platform::vita::getFreeUserMemory();
	platform::vita::logMemoryStatus("Pre-level-load");
	#endif

	res::path lightingFileName = res::path(file).set_ext("llf");

	LogDebug("fic2 " << lightingFileName);
	LogDebug("fileDlf " << file);

	VITA_CHECKPOINT("DLF read start");
	std::string buffer = g_resources->read(file);
	if(buffer.empty()) {
		LogError << "Unable to find " << file;
		return false;
	}
	VITA_CHECKPOINT("DLF read done");

	g_requestLevelInit = true;

	PakFile * lightingFile = g_resources->getFile(lightingFileName);

	progressBarAdvance();
	VITA_CHECKPOINT("LoadLevelScreen start");
	LoadLevelScreen();
	VITA_CHECKPOINT("LoadLevelScreen done");

	DANAE_LS_HEADER dlh;
	memcpy(&dlh, buffer.data(), sizeof(DANAE_LS_HEADER));
	size_t pos = sizeof(DANAE_LS_HEADER);

	LogDebug("dlh.version " << dlh.version << " header size " << sizeof(DANAE_LS_HEADER));

	if(dlh.version > DLH_CURRENT_VERSION) {
		LogError << "Unexpected level file version: " << dlh.version << " for " << file;
		return false;
	}

	// using compression
	if(dlh.version >= 1.44f) {
		VITA_CHECKPOINT("blast start");
		buffer = blast(std::string_view(buffer).substr(pos));
		if(buffer.empty()) {
			LogError << "Could not decompress level file " << file;
			return false;
		}
		pos = 0;
		VITA_CHECKPOINT("blast done");
	}
	
	const char * dat = buffer.data();
	
	player.desiredangle = player.angle = dlh.angle_edit;
	
	if(strcmp(dlh.ident, "DANAE_FILE") != 0) {
		LogError << "Not a valid file " << file << ": \"" << util::loadString(dlh.ident) << '"';
		return false;
	}
	
	LogDebug("Loading Scene");
	
	Vec3f trans(0.f);
	
	// Loading Scene
	if(dlh.nb_scn > 0) {

		const DANAE_LS_SCENE * dls = reinterpret_cast<const DANAE_LS_SCENE *>(dat + pos);
		pos += sizeof(DANAE_LS_SCENE);

		res::path scene = res::path::load(util::loadString(dls->name));

		VITA_CHECKPOINT("FastSceneLoad start");

		if(FastSceneLoad(scene, trans)) {
			LogDebug("done loading scene");
		} else {
			LogError << "Fast loading scene failed";
		}

		VITA_CHECKPOINT("FastSceneLoad done");

		g_tiles->computeIntersectingPolygons();
		LastLoadedScene = scene;
	}
	
	player.pos = dlh.pos_edit.toVec3() + trans;
	
	float increment = 0;
	if(dlh.nb_inter > 0) {
		increment = 60.f / float(dlh.nb_inter);
	} else {
		progressBarAdvance(60);
		LoadLevelScreen();
	}
	
	VITA_CHECKPOINT("entities start");

	for(s32 i = 0 ; i < dlh.nb_inter ; i++) {

		progressBarAdvance(increment);
		LoadLevelScreen();
		
		const DANAE_LS_INTER * dli = reinterpret_cast<const DANAE_LS_INTER *>(dat + pos);
		pos += sizeof(DANAE_LS_INTER);
		
		if(loadEntities) {
			
			std::string pathstr = util::toLowercase(util::loadString(dli->name));
			
			size_t graphPos = pathstr.find("graph");
			if(graphPos != std::string::npos) {
				pathstr = pathstr.substr(graphPos);
			}
			
			res::path classPath = res::path::load(pathstr).remove_ext();
			LoadInter_Ex(classPath, dli->ident, dli->pos.toVec3() + trans, dli->angle);
		}
		
	}
	
	VITA_CHECKPOINT("entities done");

	if(dlh.lighting) {
		loadLighting(dat, pos, buffer.size(), dlh.version > 1.001f, lightingFile != nullptr);
	}
	
	progressBarAdvance();
	LoadLevelScreen();
	
	size_t nb_lights = (dlh.version < 1.003f) ? 0 : size_t(dlh.nb_lights);
	
	if(!lightingFile) {
		loadLights(dat, pos, nb_lights);
	} else {
		pos += sizeof(DANAE_LS_LIGHT) * nb_lights;
	}
	
	LogDebug("Loading FOGS");
	ARX_FOGS_Clear();
	
	for(s32 i = 0; i < dlh.nb_fogs; i++) {
		
		const DANAE_LS_FOG * dlf = reinterpret_cast<const DANAE_LS_FOG *>(dat + pos);
		pos += sizeof(DANAE_LS_FOG);
		
		FogHandle fogIndex = ARX_FOGS_GetFree();
		FOG_DEF * fd = &g_fogs[fogIndex];
		fd->rgb = dlf->rgb;
		fd->angle = dlf->angle;
		fd->pos = dlf->pos.toVec3() + trans;
		fd->blend = dlf->blend;
		fd->frequency = dlf->frequency;
		fd->rotatespeed = dlf->rotatespeed;
		fd->sizeDelta = dlf->scale;
		fd->size = dlf->size;
		fd->directional = (dlf->special & 1) != 0;
		fd->speed = dlf->speed;
		fd->duration = std::chrono::milliseconds(dlf->tolive);
		GameDuration max = std::chrono::duration<s32, std::micro>::max() / 4;
		if(arx_unlikely(fd->duration > max)) {
			LogWarning << "Excessive duration for fog #" << i;
			fd->duration = max;
		}
		Vec3f out = VRotateY(Vec3f(1.f, 0.f, 0.f), MAKEANGLE(fd->angle.getYaw()));
		fd->move = VRotateX(out, MAKEANGLE(fd->angle.getPitch()));
	}
	
	progressBarAdvance(2.f);
	LoadLevelScreen();
	
	// Skip nodes — use safe arithmetic to prevent integer overflow
	if(dlh.version >= 1.001f && dlh.nb_nodes > 0) {
		size_t linkCount = (dlh.nb_nodeslinks > 0) ? size_t(dlh.nb_nodeslinks) : 0;
		size_t nodeSize = 204 + linkCount * 64;
		size_t skipBytes = size_t(dlh.nb_nodes) * nodeSize;
		// Overflow check: if result / nb_nodes != nodeSize, multiplication overflowed
		if(arx_unlikely(dlh.nb_nodes != 0 && skipBytes / size_t(dlh.nb_nodes) != nodeSize)) {
			LogError << "Level file node data overflow";
			return false;
		}
		if(arx_unlikely(pos + skipBytes > buffer.size())) {
			LogError << "Level file truncated at nodes";
			return false;
		}
		pos += skipBytes;
	}
	
	LogDebug("Loading Paths");
	ARX_PATH_ReleaseAllPath();
	
	g_zones.clear();
	g_paths.clear();
	
	for(s32 i = 0; i < dlh.nb_paths; i++) {
		
		const DANAE_LS_PATH * dlp = reinterpret_cast<const DANAE_LS_PATH *>(dat + pos);
		pos += sizeof(DANAE_LS_PATH);
		
		Vec3f ppos = dlp->pos.toVec3() + trans;
		
		std::string name = util::toLowercase(util::loadString(dlp->name));
		
		s32 height = dlp->height;
		if(height == 0 && name == "level11_sewer1") {
			// TODO patch assets instead
			height = -1;
		}
		
		if(height != 0) {
			
			Zone & zone = g_zones.emplace_back(std::move(name), ppos);
			
			zone.flags = ZoneFlags::load(dlp->flags); // TODO save/load flags
			zone.height = height;
			if(zone.flags & PATH_AMBIANCE) {
				zone.ambiance = res::path::load(util::loadString(dlp->ambiance));
				zone.amb_max_vol = (dlp->amb_max_vol <= 1.f ? 100.f : dlp->amb_max_vol);
			}
			if(zone.flags & PATH_FARCLIP) {
				zone.farclip = dlp->farclip;
			}
			if(zone.flags & PATH_RGB) {
				zone.rgb = dlp->rgb;
			}
			
			zone.pathways.resize(dlp->nb_pathways);
			for(s32 j = 0; j < dlp->nb_pathways; j++) {
				const DANAE_LS_PATHWAYS * dlpw = reinterpret_cast<const DANAE_LS_PATHWAYS *>(dat + pos);
				pos += sizeof(DANAE_LS_PATHWAYS);
				zone.pathways[j] = dlpw->rpos.toVec3();
			}
			
		} else {
			
			Path & path = g_paths.emplace_back(std::move(name), ppos);
			
			path.pathways.resize(dlp->nb_pathways);
			for(s32 j = 0; j < dlp->nb_pathways; j++) {
				const DANAE_LS_PATHWAYS * dlpw = reinterpret_cast<const DANAE_LS_PATHWAYS *>(dat + pos);
				pos += sizeof(DANAE_LS_PATHWAYS);
				path.pathways[j].flag = PathwayType(dlpw->flag); // TODO save/load enum
				path.pathways[j].rpos = dlpw->rpos.toVec3();
				path.pathways[j]._time = std::chrono::milliseconds(dlpw->time);
			}
			if(!path.pathways.empty()) {
				path.pathways[0].rpos = Vec3f(0.f);
				path.pathways[0]._time = 0;
			}
			
		}
		
	}
	
	ARX_PATH_ComputeAllBoundingBoxes();
	progressBarAdvance(5.f);
	LoadLevelScreen();
	
	if(arx_unlikely(pos > buffer.size())) {
		LogWarning << "DLF read " << pos << " bytes past buffer of " << buffer.size();
	}

	// Now load a separate LLF lighting file
	
	pos = 0;
	buffer.clear();
	
	VITA_CHECKPOINT("LLF start");

	if(lightingFile) {

		LogDebug("Loading LLF Info");
		
		buffer = lightingFile->read();
		
		// using compression
		if(dlh.version >= 1.44f) {
			buffer = blast(buffer);
		}
		
	}
	
	if(buffer.empty()) {
		USE_PLAYERCOLLISIONS = true;
		LogInfo << "Done loading level";
		return true;
	}
	
	dat = buffer.data();
	
	const DANAE_LLF_HEADER * llh = reinterpret_cast<const DANAE_LLF_HEADER *>(dat + pos);
	pos += sizeof(DANAE_LLF_HEADER);
	
	progressBarAdvance(4.f);
	LoadLevelScreen();
	
	loadLights(dat, pos, size_t(llh->nb_lights), trans);
	
	progressBarAdvance(2.f);
	LoadLevelScreen();
	
	loadLighting(dat, pos, buffer.size(), dlh.version > 1.001f);
	
	if(arx_unlikely(pos > buffer.size())) {
		LogWarning << "LLF read " << pos << " bytes past buffer of " << buffer.size();
	}

	progressBarAdvance();
	LoadLevelScreen();

	USE_PLAYERCOLLISIONS = true;
	
	LogInfo << "Done loading level";

	#if ARX_PLATFORM == ARX_PLATFORM_VITA
	{
		size_t postLoadFreeMem = platform::vita::getFreeUserMemory();
		size_t cost = (preLoadFreeMem > postLoadFreeMem) ? (preLoadFreeMem - postLoadFreeMem) : 0;
		platform::vita::logMemoryStatus("Post-level-load");
		LogInfo << "[VitaMem] Level load cost: " << (cost / 1024) << "KB";
	}
	#endif

	return true;

}

void DanaeClearLevel() {

	#if ARX_PLATFORM == ARX_PLATFORM_VITA
	size_t preClearFreeMem = platform::vita::getFreeUserMemory();
	platform::vita::logMemoryStatus("Pre-level-clear");
	#endif

	g_playerBook.forcePage(BOOKMODE_STATS);
	g_miniMap.reset();
	
	fadeReset();
	LAST_JUMP_ENDTIME = 0;
	ARX_GAME_Reset();

	// Ensure all async audio commands (mixer stop, source stop, etc.) are fully
	// processed before we clear entities and resources. On Vita, these commands are
	// queued in an SPSC ring buffer and processed by the audio thread — without this
	// flush, the audio thread could still be accessing sources/streams while we
	// delete entities and textures below.
	audio::flushCommands();
	VITA_CHECKPOINT("audio commands flushed");

	FlyingOverIO = nullptr;
	
	TREATZONE_Clear();
	
	EERIE_PATHFINDER_Release();
	
	arx_assert(g_tiles);
	EERIE_PORTAL_Release();
	AnchorData_ClearAll();
	g_tiles->clear();
	FreeRoomDistance();
	
	EERIE_LIGHT_GlobalInit();
	ARX_FOGS_Clear();
	
	culledStaticLightsReset();
	
	UnlinkAllLinkedObjects();
	
	entities.clear();
	
	TextureContainer::DeleteAll(TextureContainer::Level);
	g_miniMap.clearMarkerTexCont();
	
	bGCroucheToggle = false;
	
	resetDynLights();
	
	TREATZONE_Clear();
	
	g_currentArea = { };

	#if ARX_PLATFORM == ARX_PLATFORM_VITA
	g_levelLighting.shrink_to_fit();
	{
		size_t postClearFreeMem = platform::vita::getFreeUserMemory();
		size_t reclaimed = (postClearFreeMem > preClearFreeMem) ? (postClearFreeMem - preClearFreeMem) : 0;
		platform::vita::logMemoryStatus("Post-level-clear");
		LogInfo << "[VitaMem] Level clear reclaimed: " << (reclaimed / 1024) << "KB";
	}
	platform::vita::resetMemoryWatermarks();
	#endif

}

void RestoreLastLoadedLightning() {
	
	if(g_levelLighting.empty()) {
		return;
	}
	
	if(g_levelLighting.size() != g_tiles->countVertices()) {
		g_levelLighting.clear();
		return;
	}
	
	size_t i = 0;
	for(auto tile : g_tiles->tiles<util::GridYXIterator>()) {
		for(EERIEPOLY & ep : tile.polygons()) {
			size_t nbvert = (ep.type & POLY_QUAD) ? 4 : 3;
			for(size_t k = 0; k < nbvert; k++) {
				if(i >= g_levelLighting.size()) {
					g_levelLighting.clear();
					return;
				}
				ep.color[k] = ep.v[k].color = Color::fromBGRA(g_levelLighting[i++]).toRGB();
			}
		}
	}
	
}
