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

#ifndef ARX_PLATFORM_VITA_VITABENCH_H
#define ARX_PLATFORM_VITA_VITABENCH_H

#include "platform/Platform.h"

#if ARX_PLATFORM == ARX_PLATFORM_VITA

#include <cstdint>

#include "io/fs/FilePath.h"

namespace platform {
namespace vita {

/*!
 * Repeatable benchmark runs: record a route once by playing, replay it on
 * every build. A replay moves the player along the recorded positions and
 * view by time, so every run sees the same rooms from the same angles
 * (replaying raw inputs would drift with the frame rate).
 *
 * Toggle files in ux0:data/arx/:
 *  benchrec.txt  Record. From the first in-game frame after a save is loaded,
 *                sample the player's level, position and view at 10 Hz into
 *                bench/<route> ("route=<name>" in the file, default
 *                route.txt) and copy that save next to it as <route>.sav.
 *                Ends when the game quits or another save is loaded. The
 *                file is removed when recording starts (one recording).
 *  bench.txt     Replay. Load the route's save, hold at its first sample for
 *                "settle=<seconds>" (default 2), play the route with the
 *                player invulnerable, then log "[Bench] done ..." with frame
 *                time statistics and append them to bench/results.txt.
 *                "route=<name>" picks the route, "fps=<n>" overrides the
 *                frame limit for this session (0 = uncapped), "exit" quits
 *                when the route ends. Don't touch the controls.
 *
 * Every 5 s in game a "[Frame]" line logs the frame time split, bench or not.
 */

//! Frame phases timed on the main thread.
enum PerfPhase {
	PerfSleep,  //!< frame limiter sleep
	PerfUpdate, //!< ArxGame::updateLevel
	PerfRender, //!< ArxGame::renderLevel
	PerfAudio,  //!< audio::updateSync
	PerfSwap,   //!< FBO blit + vglSwapBuffers (GPU and vsync wait)
	PerfPhaseCount
};

//! Engine systems timed inside the phases (see VITA_ZONE). Zones must not nest.
enum PerfZone {
	// update
	ZoneControls,     //!< player input, editor controls
	ZoneEntityPrep,   //!< per-entity preprocessing (ignition, highlight, lighting prep)
	ZonePlayerMove,   //!< ARX_PLAYER_Manage_Movement
	ZonePlayerAnim,   //!< player animation and combat animations
	ZoneCamera,       //!< first person camera, cameras, frame checks, fog, listener
	ZoneScriptTimers, //!< ARX_SCRIPT_Timer_Check
	ZoneTreatZone,    //!< PrepareIOTreatZone
	ZoneNpcPhysics,   //!< ARX_PHYSICS_Apply: NPC movement, AI, physics boxes
	ZoneIOLighting,   //!< PrecalcIOLighting
	ZoneDynLights,    //!< background dynamic lights, dynamic light culling
	ZonePortals,      //!< portal traversal: visible rooms and frustums
	ZoneRoomCull,     //!< per-polygon culling and tile lighting of visible rooms
	ZoneThrown,       //!< thrown objects, dragged entity
	ZoneUpdateInter,  //!< UpdateInter: entity animation and skinning
	ZoneParticleMgr,  //!< particle systems update
	ZoneBgActions,    //!< TreatBackgroundActions
	ZoneMagic,        //!< rune drawing, precast, magic management, torch
	// render
	ZoneRoomsOpaque,  //!< opaque room batches
	ZoneShadows,      //!< blob shadows
	ZoneRenderInter,  //!< RenderInter: entity lighting and batching
	ZonePopOpaque,    //!< opaque entity batches and the player
	ZoneTransparent,  //!< decals, transparent entities and rooms, water, lava
	ZoneParticles,    //!< particle and spark render/update
	ZoneSpells,       //!< ARX_SPELLS_Update
	ZoneBatcher,      //!< render batcher, light flares
	ZoneHud,          //!< interface, text, minimap, cursor
	ZoneSpeech,       //!< ARX_SPEECH_Update
	// rest of the frame
	ZonePicking,      //!< hover picking and key/mouse handling
	ZoneScripts,      //!< script heartbeat and event stack
	ZoneGameMisc,     //!< damages, missiles, zones
	// nested in the zones above (counted twice)
	ZoneEntLightPre,  //!< renderinter: parallel entity vertex lighting
	ZoneEntAnimated,  //!< renderinter: animated entity face batching (and linked objects)
	ZoneEntStatic,    //!< renderinter: static entities (DrawEERIEInter)
	ZonePlayerRender, //!< popopaque: the player
	ZoneNpcFloor,     //!< npcphysics: floor polygon (lava) and underwater checks
	ZoneNpcBox,       //!< npcphysics: physics boxes
	ZoneNpcMove,      //!< npcphysics: ManageNPCMovement
	ZoneNpcCheck,     //!< npcphysics: CheckNPC and CheckNPCEx
	ZoneNpcGravity,   //!< npc.move: the gravity probe (CheckAnythingInCylinder)
	ZoneNpcCollide,   //!< npc.move: the collision move (ARX_COLLISION_Move_Cylinder)
	ZoneNpcAnchor,    //!< npc.move: nearest anchor searches
	ZoneNpcNextNode,  //!< npc.move: shortcuts along the path (ARX_NPC_GetNextAttainableNodeIncrement)
	ZoneNpcPathfind,  //!< npc.move: pathfinder requests
	ZoneNpcRoomDist,  //!< npc.check: room distance to the player (SP_GetRoomDist)
	ZoneNpcVisible,   //!< npc.check: line of sight to the player (IO_Visible)
	ZoneNpcDetect,    //!< npc.check: detect / undetect script events
	ZonePartSystems,  //!< particles: particle systems (g_particleManager)
	ZonePartLegacy,   //!< particles: ARX_PARTICLES_Update
	ZoneMiscDamages,  //!< gamemisc: damage spheres
	ZoneWorldCollect, //!< roomsopaque: visible batches, their lights, sorting
	ZoneHudMiniMap,   //!< hud: the minimap
	ZonePathZones,    //!< gamemisc: entities entering and leaving zones
	PerfZoneCount
};

//! Times an engine system for the zone breakdown.
class ZoneScope {
	
	PerfZone m_zone;
	uint64_t m_start;
	
public:
	
	explicit ZoneScope(PerfZone zone);
	~ZoneScope();
	
	ZoneScope(const ZoneScope &) = delete;
	ZoneScope & operator=(const ZoneScope &) = delete;
	
};

//! Parts of a level or save load, timed exclusively (a nested part pauses its parent).
enum LoadZone {
	LoadOther,      //!< inside the load but in none of the parts below
	LoadScreen,     //!< drawing the loading screen and presenting it
	LoadPak,        //!< reading files from the paks
	LoadBlast,      //!< decompressing
	LoadImage,      //!< decoding images (other than JPEG)
	LoadJpeg,       //!< decoding JPEG images
	LoadTexture,    //!< preparing and uploading textures
	LoadAudio,      //!< loading sound samples
	LoadClear,      //!< DanaeClearLevel
	LoadSaveBlock,  //!< opening and reading the save block
	LoadSaveCopy,   //!< copying the save to current.sav
	LoadDlf,        //!< DanaeLoadLevel outside its parts
	LoadScene,      //!< FastSceneLoad (FTS geometry, rooms, anchors)
	LoadEntities,   //!< creating and restoring entities (outside the parts below)
	LoadModel,      //!< FTL model loading
	LoadPhysics,    //!< physics boxes for models
	LoadAnim,       //!< animation loading
	LoadScript,     //!< script loading
	LoadScriptInit, //!< running init script events
	LoadTweak,      //!< mesh tweaks
	LoadPortalVB,   //!< ComputePortalVertexBuffer
	LoadWorldGpu,   //!< vitaworld::build
	LoadShaders,    //!< compiling shaders
	LoadPathfinder, //!< pathfinder setup
	LoadLevelInit,  //!< levelInit
	LoadZoneCount
};

class LoadScope {
	
	bool m_active;
	
public:
	
	explicit LoadScope(LoadZone zone);
	~LoadScope();
	
	LoadScope(const LoadScope &) = delete;
	LoadScope & operator=(const LoadScope &) = delete;
	
};

//! Bytes read from the paks during the load.
void loadAddBytes(size_t bytes);

//! A save load or level change starts: reset the load breakdown.
void loadBegin(const char * what);

//! The load finished (end of levelInit): log the breakdown.
void loadEnd();

//! Top of ArxGame::doFrame, before the frame limiter: closes the previous frame.
void perfFrameBoundary();

//! The current frame loads a level or a save: keep it out of the frame statistics.
void perfMarkLoading();

void perfAdd(PerfPhase phase, uint64_t us);

uint64_t perfNowUs();

//! Times a phase and names it in the log server heartbeat.
class PerfScope {

	PerfPhase m_phase;
	uint64_t m_start;

public:

	explicit PerfScope(PerfPhase phase);
	~PerfScope();

	PerfScope(const PerfScope &) = delete;
	PerfScope & operator=(const PerfScope &) = delete;

};

//! Phase the main thread is in, for the heartbeat ("frame" between phases).
const char * perfMainPhase();

//! Microseconds since the main thread last closed a frame.
uint64_t perfMainAgeUs();

//! Replay: the save to load instead of showing the main menu, or an empty path.
fs::path benchSaveToLoad();

//! A save was loaded (ARX_LoadGame): the recorder starts from it.
void benchNoteSaveLoaded(const fs::path & savefile);

//! ArxGame::updateLevel, right after the player movement: record or move the player.
void benchAfterPlayerMovement();

//! A replay is running (bench.txt).
bool benchReplayActive();

} // namespace vita
} // namespace platform

#endif // ARX_PLATFORM == ARX_PLATFORM_VITA

#define VITA_ZONE_CONCAT_(a, b) a ## b
#define VITA_ZONE_CONCAT(a, b) VITA_ZONE_CONCAT_(a, b)

//! Time the rest of the enclosing scope as an engine system (Vita only).
#if ARX_PLATFORM == ARX_PLATFORM_VITA
#define VITA_ZONE(zone) \
	::platform::vita::ZoneScope VITA_ZONE_CONCAT(vitaZone, __LINE__)(::platform::vita::zone)
#define VITA_LOAD_ZONE(zone) \
	::platform::vita::LoadScope VITA_ZONE_CONCAT(vitaLoad, __LINE__)(::platform::vita::zone)
#else
#define VITA_ZONE(zone) ((void)0)
#define VITA_LOAD_ZONE(zone) ((void)0)
#endif

#endif // ARX_PLATFORM_VITA_VITABENCH_H
