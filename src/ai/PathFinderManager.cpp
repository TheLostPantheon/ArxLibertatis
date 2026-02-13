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
// Copyright (c) 1999-2001 ARKANE Studios SA. All rights reserved

#include "ai/PathFinderManager.h"

#include <cstring>
#include <cstdlib>
#include <algorithm>
#include <vector>

#include "ai/Anchors.h"
#include "ai/PathFinder.h"
#include "game/Entity.h"
#include "game/EntityManager.h"
#include "game/NPC.h"
#include "scene/Interactive.h"
#include "graphics/Math.h"
#include "platform/Platform.h"
#include "platform/profiler/Profiler.h"
#include "scene/Light.h"

#if ARX_PLATFORM != ARX_PLATFORM_VITA
#include <list>
#include <mutex>
#include "platform/Thread.h"
#endif

static constexpr float PATHFINDER_HEURISTIC_MIN = 0.2f;
static constexpr float PATHFINDER_HEURISTIC_MAX = PathFinder::HEURISTIC_MAX;
static constexpr float PATHFINDER_HEURISTIC_RANGE = PATHFINDER_HEURISTIC_MAX - PATHFINDER_HEURISTIC_MIN;
static constexpr float PATHFINDER_DISTANCE_MAX = 5000.0f;

//! Run a single pathfinder request and write results into the provided output params.
//! Returns true if a valid path was found.
static bool processPathfinderRequest(PathFinder & pathfinder, const PATHFINDER_REQUEST & request,
                                     long *& outList, long & outListnb) {

	outList = nullptr;
	outListnb = 0;

	if(request.behavior == BEHAVIOUR_NONE) {
		return false;
	}

	ARX_PROFILE_FUNC();

	pathfinder.setCylinder(request.cyl_radius, request.cyl_height);

	float distance;
	if(request.behavior & (BEHAVIOUR_MOVE_TO | BEHAVIOUR_GO_HOME)) {
		distance = fdist(g_anchors[request.from].pos, g_anchors[request.to].pos);
	} else if(request.behavior & (BEHAVIOUR_WANDER_AROUND | BEHAVIOUR_FLEE | BEHAVIOUR_HIDE)) {
		distance = request.behavior_param;
	} else if(request.behavior & BEHAVIOUR_LOOK_FOR) {
		distance = fdist(request.pos, request.target);
	} else {
		return false;
	}
	float heuristic = PATHFINDER_HEURISTIC_MAX;
	if(distance < PATHFINDER_DISTANCE_MAX) {
		heuristic = PATHFINDER_HEURISTIC_MIN + PATHFINDER_HEURISTIC_RANGE * (distance / PATHFINDER_DISTANCE_MAX);
	}
	pathfinder.setHeuristic(heuristic);

	bool stealth = request.behavior.hasAll(BEHAVIOUR_SNEAK | BEHAVIOUR_HIDE);

	PathFinder::Result result;
	if(request.behavior & (BEHAVIOUR_MOVE_TO | BEHAVIOUR_GO_HOME)) {
		pathfinder.move(request.from, request.to, result, stealth);
	} else if(request.behavior & BEHAVIOUR_WANDER_AROUND) {
		pathfinder.wanderAround(request.from, distance, result, stealth);
	} else if(request.behavior & (BEHAVIOUR_FLEE | BEHAVIOUR_HIDE)) {
		float safeDistance = distance + fdist(request.target, request.pos);
		pathfinder.flee(request.from, request.target, safeDistance, result, stealth);
	} else if(request.behavior & BEHAVIOUR_LOOK_FOR) {
		float radius = request.behavior_param;
		pathfinder.lookFor(request.from, request.target, radius, result, stealth);
	}

	outListnb = result.size();
	if(!result.empty()) {
		outList = new long[result.size()];
		std::copy(result.begin(), result.end(), outList);
	}

	return !result.empty();
}

#if ARX_PLATFORM == ARX_PLATFORM_VITA

// On Vita, pathfinding runs synchronously on the main thread.
// No thread, no mutex, no queue — just a static PathFinder instance.
static PathFinder * g_pathFinder = nullptr;

#else // !ARX_PLATFORM_VITA

struct PATHFINDER_RESULT {
	Entity * entity;
	long * list;     // nullptr if path not found
	long listnb;
};

class PathFinderThread final : public StoppableThread {

	std::mutex m_mutex;
	std::list<PATHFINDER_REQUEST> m_queue;
	std::vector<PATHFINDER_RESULT> m_results;
	volatile bool m_busy;

	void run() override;

public:

	PathFinderThread() : m_busy(false) { }

	void queueRequest(const PATHFINDER_REQUEST & request);

	size_t queueSize() {

		std::scoped_lock lock(m_mutex);

		return m_queue.size();
	}

	void clearQueue() {

		std::scoped_lock lock(m_mutex);

		m_queue.clear();

		// Free any pending results that were never propagated
		for(PATHFINDER_RESULT & r : m_results) {
			delete[] r.list;
		}
		m_results.clear();
	}

	//! Drain completed results (called from main thread)
	void propagateResults();

	bool isBusy() const {
		return m_busy;
	}

};

void PathFinderThread::queueRequest(const PATHFINDER_REQUEST & request) {
	
	arx_assert(request.entity && (request.entity->ioflags & IO_NPC));
	
	std::scoped_lock lock(m_mutex);
	
	// If this NPC is already requesting a Pathfinding then either
	// try to Override it or add it to queue if it is currently being
	// processed.
	for(PATHFINDER_REQUEST & oldRequest : m_queue) {
		if(oldRequest.entity == request.entity) {
			oldRequest = request;
			return;
		}
	}
	
	if(!m_queue.empty()
	   && (request.behavior & (BEHAVIOUR_MOVE_TO | BEHAVIOUR_FLEE | BEHAVIOUR_LOOK_FOR))) {
		// priority: insert as second element of queue
		m_queue.insert(++m_queue.begin(), request);
	} else {
		m_queue.push_back(request);
	}
	
}

void PathFinderThread::run() {

	PathFinder pathfinder(g_anchors.size(), g_anchors.data(), &g_staticLights);

	for(; !isStopRequested(); m_busy = false, sleep(10ms)) {

		std::scoped_lock lock(m_mutex);

		m_busy = true;

		if(m_queue.empty()) {
			continue;
		}

		PATHFINDER_REQUEST request = m_queue.front();
		m_queue.pop_front();

		PATHFINDER_RESULT pfResult;
		pfResult.entity = request.entity;
		processPathfinderRequest(pathfinder, request, pfResult.list, pfResult.listnb);
		m_results.push_back(pfResult);

	}

}

void PathFinderThread::propagateResults() {

	std::vector<PATHFINDER_RESULT> results;

	{
		std::scoped_lock lock(m_mutex);
		results.swap(m_results);
	}

	for(PATHFINDER_RESULT & r : results) {
		// Validate entity is still alive before writing to its data
		if(!ValidIOAddress(r.entity) || !(r.entity->ioflags & IO_NPC) || !r.entity->_npcdata) {
			delete[] r.list;
			continue;
		}

		// Safe to write — we are on the main thread, no race possible
		delete[] r.entity->_npcdata->pathfind.list;
		r.entity->_npcdata->pathfind.list = r.list;
		r.entity->_npcdata->pathfind.listnb = r.listnb;
	}

}

static PathFinderThread * g_pathFinderThread = nullptr;

#endif // !ARX_PLATFORM_VITA


#if ARX_PLATFORM == ARX_PLATFORM_VITA

// Vita: synchronous pathfinding — process request immediately on main thread

bool EERIE_PATHFINDER_Add_To_Queue(const PATHFINDER_REQUEST & request) {

	if(!g_pathFinder) {
		return false;
	}

	arx_assert(request.entity && (request.entity->ioflags & IO_NPC));

	long * list = nullptr;
	long listnb = 0;
	processPathfinderRequest(*g_pathFinder, request, list, listnb);

	// Safe to write directly — we're on the main thread, entity is alive (caller validated)
	delete[] request.entity->_npcdata->pathfind.list;
	request.entity->_npcdata->pathfind.list = list;
	request.entity->_npcdata->pathfind.listnb = listnb;

	return true;
}

long EERIE_PATHFINDER_Get_Queued_Number() {
	return 0;
}

bool EERIE_PATHFINDER_Is_Busy() {
	return false;
}

void EERIE_PATHFINDER_Clear() { }

void EERIE_PATHFINDER_Release() {
	delete g_pathFinder, g_pathFinder = nullptr;
}

void EERIE_PATHFINDER_Create() {

	if(g_pathFinder) {
		EERIE_PATHFINDER_Release();
	}

	g_pathFinder = new PathFinder(g_anchors.size(), g_anchors.data(), &g_staticLights);
}

void EERIE_PATHFINDER_Propagate_Results() { }

#else // !ARX_PLATFORM_VITA

// Desktop: threaded pathfinding with result propagation

bool EERIE_PATHFINDER_Add_To_Queue(const PATHFINDER_REQUEST & request) {

	if(!g_pathFinderThread) {
		return false;
	}

	g_pathFinderThread->queueRequest(request);

	return true;
}

long EERIE_PATHFINDER_Get_Queued_Number() {

	if(!g_pathFinderThread) {
		return 0;
	}

	return g_pathFinderThread->queueSize();
}

bool EERIE_PATHFINDER_Is_Busy() {

	if(!g_pathFinderThread) {
		return false;
	}

	return g_pathFinderThread->isBusy();
}

void EERIE_PATHFINDER_Clear() {

	if(!g_pathFinderThread) {
		return;
	}

	g_pathFinderThread->clearQueue();
}

void EERIE_PATHFINDER_Release() {

	if(!g_pathFinderThread) {
		return;
	}

	g_pathFinderThread->clearQueue();
	g_pathFinderThread->stop();

	delete g_pathFinderThread, g_pathFinderThread = nullptr;
}

void EERIE_PATHFINDER_Create() {

	if(g_pathFinderThread) {
		EERIE_PATHFINDER_Release();
	}

	g_pathFinderThread = new PathFinderThread();
	g_pathFinderThread->setThreadName("Pathfinder");
	g_pathFinderThread->start();
}

void EERIE_PATHFINDER_Propagate_Results() {

	if(!g_pathFinderThread) {
		return;
	}

	g_pathFinderThread->propagateResults();
}

#endif // ARX_PLATFORM_VITA
