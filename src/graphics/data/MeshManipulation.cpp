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

#include "graphics/data/MeshManipulation.h"

#include <stddef.h>
#include <cstring>
#include <cstdlib>
#include <algorithm>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "game/Entity.h"

#include "graphics/BaseGraphicsTypes.h"
#include "graphics/GraphicsTypes.h"
#include "graphics/Vertex.h"
#include "graphics/data/Mesh.h"
#include "graphics/data/TextureContainer.h"

#include "io/resource/PakReader.h"
#include "io/log/Logger.h"

#include "math/Types.h"
#include "math/Vector.h"

#include "platform/Platform.h"

#include "scene/Object.h"

void EERIE_MESH_TWEAK_Skin(EERIE_3DOBJ * obj, const res::path & s1, const res::path & s2) {
	
	LogDebug("Tweak Skin " << s1 << " " << s2);
	
	if(obj == nullptr || s1.empty() || s2.empty()) {
		LogError << "Tweak Skin got NULL Pointer";
		return;
	}
	
	LogDebug("Tweak Skin " << s1 << " " << s2);
	
	res::path skintochange = "graph/obj3d/textures" / s1;
	
	res::path skinname = "graph/obj3d/textures" / s2;
	TextureContainer * tex = TextureContainer::Load(skinname);
	if(!tex) {
		return;
	}
	
	if(obj->originalMaterials.empty()) {
		obj->originalMaterials.reserve(obj->materials.size());
		for(TextureContainer * texture : obj->materials) {
			obj->originalMaterials.emplace_back(texture ? texture->m_texName : std::string_view());
		}
	}
	
	arx_assert(obj->originalMaterials.size() == obj->materials.size());
	
	bool found = false;
	
	for(MaterialId id : obj->materials.handles()) {
		if(obj->originalMaterials[id] == skintochange) {
			obj->materials[id] = tex;
			found = true;
		}
	}
	
	if(found) {
		return;
	}
	
	for(TextureContainer * & texture : obj->materials) {
		if(texture->m_texName == skintochange) {
			texture = tex;
		}
	}
	
}

bool IsInSelection(const EERIE_3DOBJ * obj, VertexId vert, VertexSelectionId tw) {
	
	if(!obj || !tw) {
		return false;
	}
	
	const EERIE_SELECTIONS & sel = obj->selections[tw];
	
	return std::find(sel.selected.begin(), sel.selected.end(), vert) != sel.selected.end();
}

MaterialId addMaterial(EERIE_3DOBJ & obj, TextureContainer * material) {
	
	if(!material) {
		return { };
	}
	
	for(MaterialId id : obj.materials.handles()) {
		if(obj.materials[id] == material) {
			return id;
		}
	}
	
	obj.materials.push_back(material);
	
	return obj.materials.last();
}

namespace {

//! Exact vertex position (Vec3f ==) as a hash key; -0 and +0 are the same position.
struct PositionKey {
	
	u32 x, y, z;
	
	bool operator==(const PositionKey & o) const noexcept {
		return x == o.x && y == o.y && z == o.z;
	}
	
	//! False for positions with a NaN, which never compare equal to anything.
	static bool make(Vec3f v, PositionKey & key) noexcept {
		if(v.x != v.x || v.y != v.y || v.z != v.z) {
			return false;
		}
		v += Vec3f(0.f); // -0 -> +0
		std::memcpy(&key.x, &v.x, sizeof(u32));
		std::memcpy(&key.y, &v.y, sizeof(u32));
		std::memcpy(&key.z, &v.z, sizeof(u32));
		return true;
	}
	
};

struct PositionKeyHash {
	size_t operator()(const PositionKey & k) const noexcept {
		size_t h = k.x;
		h = h * 0x9E3779B1u ^ k.y;
		h = h * 0x9E3779B1u ^ k.z;
		return h;
	}
};

struct FaceKey {
	
	PositionKey v[3];
	
	bool operator==(const FaceKey & o) const noexcept {
		return v[0] == o.v[0] && v[1] == o.v[1] && v[2] == o.v[2];
	}
	
};

struct FaceKeyHash {
	size_t operator()(const FaceKey & k) const noexcept {
		PositionKeyHash h;
		return h(k.v[0]) * 31 + h(k.v[1]) * 17 + h(k.v[2]);
	}
};

/*!
 * Builds a mesh out of pieces of others, merging vertices with the same position.
 *
 * Lookups by position go through hash tables instead of scanning the vertex and
 * face lists, which made merging meshes quadratic (most of the time spent loading
 * a level with many dressed NPCs). The result is the same as with the scans: the
 * first vertex at a position wins, and a face over the same three positions is
 * only added once.
 */
class MeshBuilder {
	
	EERIE_3DOBJ & m_obj;
	std::unordered_map<PositionKey, VertexId, PositionKeyHash> m_vertices;
	std::unordered_set<FaceKey, FaceKeyHash> m_faces;
	
public:
	
	explicit MeshBuilder(EERIE_3DOBJ & obj) : m_obj(obj) { }
	
	VertexId find(Vec3f position) const {
		PositionKey key;
		if(!PositionKey::make(position, key)) {
			return { };
		}
		auto it = m_vertices.find(key);
		return it == m_vertices.end() ? VertexId() : it->second;
	}
	
	VertexId addVertex(const EERIE_VERTEX & vertex) {
		PositionKey key;
		bool hashable = PositionKey::make(vertex.v, key);
		if(hashable) {
			auto it = m_vertices.find(key);
			if(it != m_vertices.end()) {
				return it->second;
			}
		}
		m_obj.vertexlist.push_back(vertex);
		VertexId index = m_obj.vertexlist.last();
		if(hashable) {
			m_vertices.emplace(key, index);
		}
		return index;
	}
	
	void addFace(const EERIE_FACE & face, const EERIE_3DOBJ & source) {
		
		FaceKey key;
		bool hashable = true;
		for(size_t i = 0; i < 3; i++) {
			hashable = PositionKey::make(source.vertexlist[face.vid[i]].v, key.v[i]) && hashable;
		}
		if(hashable && !m_faces.insert(key).second) {
			return; // already there
		}
		
		EERIE_FACE & newface = m_obj.facelist.emplace_back(face);
		newface.vid[0] = addVertex(source.vertexlist[face.vid[0]]);
		newface.vid[1] = addVertex(source.vertexlist[face.vid[1]]);
		newface.vid[2] = addVertex(source.vertexlist[face.vid[2]]);
		newface.material = MaterialId(0);
		
		for(MaterialId material : m_obj.materials.handles()) {
			if(face.material && size_t(face.material) < source.materials.size()
			   && m_obj.materials[material] == source.materials[face.material]) {
				newface.material = material;
				break;
			}
		}
	}
	
	void addNamedVertex(std::string_view name, const EERIE_VERTEX & vertex) {
		
		VertexId newvert = addVertex(vertex);
		
		for(const EERIE_ACTIONLIST & action : m_obj.actionlist) {
			if(action.name == name) {
				return;
			}
		}
		
		EERIE_ACTIONLIST & action = m_obj.actionlist.emplace_back();
		action.name = name;
		action.idx = newvert;
	}
	
	//! Add the vertices of source at the given positions to a list, each once.
	template <typename Container, typename Positions>
	void addToList(Container & list, const Positions & positions) {
		std::vector<u8> present(m_obj.vertexlist.size(), 0);
		for(VertexId index : list) {
			if(size_t(index) < present.size()) {
				present[size_t(index)] = 1;
			}
		}
		for(const Vec3f & position : positions) {
			VertexId index = find(position);
			if(index && !present[size_t(index)]) {
				present[size_t(index)] = 1;
				list.push_back(index);
			}
		}
	}
	
};

//! Which vertices of obj are in a selection, for fast IsInSelection.
std::vector<u8> selectionMask(const EERIE_3DOBJ & obj, VertexSelectionId selection) {
	std::vector<u8> mask(obj.vertexlist.size(), 0);
	if(selection) {
		for(VertexId vertex : obj.selections[selection].selected) {
			if(size_t(vertex) < mask.size()) {
				mask[size_t(vertex)] = 1;
			}
		}
	}
	return mask;
}

bool inMask(const std::vector<u8> & mask, VertexId vertex) {
	return vertex && size_t(vertex) < mask.size() && mask[size_t(vertex)];
}

std::vector<Vec3f> selectionPositions(const EERIE_3DOBJ & obj, VertexSelectionId selection) {
	std::vector<Vec3f> positions;
	positions.reserve(obj.selections[selection].selected.size());
	for(VertexId vertex : obj.selections[selection].selected) {
		positions.push_back(obj.vertexlist[vertex].v);
	}
	return positions;
}

std::vector<Vec3f> groupPositions(const EERIE_3DOBJ & obj, VertexGroupId group) {
	std::vector<Vec3f> positions;
	positions.reserve(obj.grouplist[group].indexes.size());
	for(VertexId vertex : obj.grouplist[group].indexes) {
		positions.push_back(obj.vertexlist[vertex].v);
	}
	return positions;
}

} // anonymous namespace

static std::unique_ptr<EERIE_3DOBJ> CreateIntermediaryMesh(const EERIE_3DOBJ * obj1, const EERIE_3DOBJ * obj2,
                                                           TweakFlag tw) {
	
	VertexSelectionId sel_head1;
	VertexSelectionId sel_head2;
	VertexSelectionId sel_torso1;
	VertexSelectionId sel_torso2;
	VertexSelectionId sel_legs1;
	VertexSelectionId sel_legs2;
	
	// First we retreive selection groups indexes
	for(VertexSelectionId selection : obj1->selections.handles()) {
		if(obj1->selections[selection].name == "head") {
			sel_head1 = selection;
		} else if(obj1->selections[selection].name == "chest") {
			sel_torso1 = selection;
		} else if(obj1->selections[selection].name == "leggings") {
			sel_legs1 = selection;
		}
	}
	
	for(VertexSelectionId selection : obj2->selections.handles()) {
		if(obj2->selections[selection].name == "head") {
			sel_head2 = selection;
		} else if(obj2->selections[selection].name == "chest") {
			sel_torso2 = selection;
		} else if(obj2->selections[selection].name == "leggings") {
			sel_legs2 = selection;
		}
	}
	
	if(!sel_head1 || !sel_head2 || !sel_torso1 || !sel_torso2 || !sel_legs1 || !sel_legs2) {
		return nullptr;
	}
	
	VertexSelectionId tw1;
	VertexSelectionId tw2;
	VertexSelectionId iw1;
	VertexSelectionId jw1;
	
	if(tw == TWEAK_HEAD) {
		tw1 = sel_head1;
		tw2 = sel_head2;
		iw1 = sel_torso1;
		jw1 = sel_legs1;
	}
	
	if(tw == TWEAK_TORSO) {
		tw1 = sel_torso1;
		tw2 = sel_torso2;
		iw1 = sel_head1;
		jw1 = sel_legs1;
	}
	
	if(tw == TWEAK_LEGS) {
		tw1 = sel_legs1;
		tw2 = sel_legs2;
		iw1 = sel_torso1;
		jw1 = sel_head1;
	}
	
	if(!tw1 || !tw2) {
		return { };
	}
	
	if(!getNamedVertex(obj1, "head2chest") ||
	   !getNamedVertex(obj2, "head2chest") ||
	   !getNamedVertex(obj1, "chest2leggings") ||
	   !getNamedVertex(obj2, "chest2leggings")) {
		return { };
	}
	
	// Work will contain the Tweaked object
	std::unique_ptr<EERIE_3DOBJ> work = std::make_unique<EERIE_3DOBJ>();
	MeshBuilder builder(*work);
	
	const std::vector<u8> inIw1 = selectionMask(*obj1, iw1);
	const std::vector<u8> inJw1 = selectionMask(*obj1, jw1);
	const std::vector<u8> inTw2 = selectionMask(*obj2, tw2);
	auto keep1 = [&](VertexId vertex) { return inMask(inIw1, vertex) || inMask(inJw1, vertex); };
	
	// Linked objects are linked to this object.
	if(obj1->linked.size() > obj2->linked.size()) {
		work->linked = obj1->linked;
	} else {
		work->linked = obj2->linked;
	}
	
	// Is the origin of object in obj1 or obj2 ? Retreives it for work object
	if(IsInSelection(obj1, obj1->origin, tw1)) {
		work->origin = builder.addVertex(obj2->vertexlist[obj2->origin]);
	} else {
		work->origin = builder.addVertex(obj1->vertexlist[obj1->origin]);
	}
	
	// Recreate Action Points included in work object.for Obj1
	for(size_t i = 0; i < obj1->actionlist.size(); i++) {
		const EERIE_ACTIONLIST & action = obj1->actionlist[i];
		if(keep1(action.idx) ||
		   action.name == "head2chest" ||
		   action.name == "chest2leggings") {
			builder.addNamedVertex(action.name, obj1->vertexlist[action.idx]);
		}
	}
	
	// Do the same for Obj2
	for(size_t i = 0; i < obj2->actionlist.size(); i++) {
		const EERIE_ACTIONLIST & action = obj2->actionlist[i];
		if(inMask(inTw2, action.idx) ||
		   action.name == "head2chest" ||
		   action.name == "chest2leggings") {
			builder.addNamedVertex(action.name, obj2->vertexlist[action.idx]);
		}
	}
	
	// Recreate Vertex using Obj1 Vertexes
	for(VertexId vertex : obj1->vertexlist.handles()) {
		if(keep1(vertex)) {
			builder.addVertex(obj1->vertexlist[vertex]);
		}
	}
	
	// The same for Obj2
	for(VertexId vertex : obj2->vertexlist.handles()) {
		if(inMask(inTw2, vertex)) {
			builder.addVertex(obj2->vertexlist[vertex]);
		}
	}
	
	// Look in Faces for forgotten Vertexes... AND
	// Re-Create TextureContainers Infos
	// We look for texturecontainers included in the future tweaked object
	TextureContainer * tc = nullptr;
	for(const EERIE_FACE & face : obj1->facelist) {
		if(keep1(face.vid[0]) && keep1(face.vid[1]) && keep1(face.vid[2])) {
			if(face.material && tc != obj1->materials[face.material]) {
				tc = obj1->materials[face.material];
				addMaterial(*work, tc);
			}
			builder.addFace(face, *obj1);
		}
	}
	
	for(const EERIE_FACE & face : obj2->facelist) {
		if(inMask(inTw2, face.vid[0]) || inMask(inTw2, face.vid[1]) || inMask(inTw2, face.vid[2])) {
			if(face.material && tc != obj2->materials[face.material]) {
				tc = obj2->materials[face.material];
				addMaterial(*work, tc);
			}
			builder.addFace(face, *obj2);
		}
	}
	
	// Recreate Groups
	work->grouplist.resize(std::max(obj1->grouplist.size(), obj2->grouplist.size()));
	
	for(VertexGroupId group : obj1->grouplist.handles()) {
		work->grouplist[group].name = obj1->grouplist[group].name;
		if(VertexId vertex = builder.find(obj1->vertexlist[obj1->grouplist[group].origin].v)) {
			work->grouplist[group].m_blobShadowSize = obj1->grouplist[group].m_blobShadowSize;
			if(keep1(obj1->grouplist[group].origin)) {
				work->grouplist[group].origin = vertex;
			}
		}
	}
	
	for(VertexGroupId group : obj2->grouplist.handles()) {
		if(size_t(group) >= obj1->grouplist.size()) {
			work->grouplist[group].name = obj2->grouplist[group].name;
		}
		if(VertexId vertex = builder.find(obj2->vertexlist[obj2->grouplist[group].origin].v)) {
			work->grouplist[group].m_blobShadowSize = obj2->grouplist[group].m_blobShadowSize;
			if(inMask(inTw2, obj2->grouplist[group].origin)) {
				work->grouplist[group].origin = vertex;
			}
		}
	}
	
	// Recreate Selection Groups (only the 3 selections needed to reiterate mesh tweaking)
	work->selections.resize(3);
	work->selections[VertexSelectionId(0)].name = "head";
	work->selections[VertexSelectionId(1)].name = "chest";
	work->selections[VertexSelectionId(2)].name = "leggings";
	auto copySelection = [&](const EERIE_3DOBJ & source, VertexSelectionId from, VertexSelectionId to) {
		builder.addToList(work->selections[to].selected, selectionPositions(source, from));
	};
	if(tw == TWEAK_HEAD) {
		copySelection(*obj2, sel_head2, VertexSelectionId(0));
	} else {
		copySelection(*obj1, sel_head1, VertexSelectionId(0));
	}
	if(tw == TWEAK_TORSO) {
		copySelection(*obj2, sel_torso2, VertexSelectionId(1));
	} else {
		copySelection(*obj1, sel_torso1, VertexSelectionId(1));
	}
	if(tw == TWEAK_LEGS) {
		copySelection(*obj2, sel_legs2, VertexSelectionId(2));
	} else {
		copySelection(*obj1, sel_legs1, VertexSelectionId(2));
	}
	
	// Now recreates other selections...
	for(VertexSelectionId selection : obj1->selections.handles()) {
		
		if(EERIE_OBJECT_GetSelection(work.get(), obj1->selections[selection].name)) {
			continue;
		}
		
		work->selections.emplace_back().name = obj1->selections[selection].name;
		copySelection(*obj1, selection, work->selections.last());
		
		if(VertexSelectionId ii = EERIE_OBJECT_GetSelection(obj2, obj1->selections[selection].name)) {
			copySelection(*obj2, ii, work->selections.last());
		}
		
	}
	
	for(VertexSelectionId selection : obj2->selections.handles()) {
		
		if(EERIE_OBJECT_GetSelection(work.get(), obj2->selections[selection].name)) {
			continue;
		}
		
		work->selections.emplace_back().name = obj2->selections[selection].name;
		copySelection(*obj2, selection, work->selections.last());
		
	}
	
	// Recreate Animation-groups vertex
	for(VertexGroupId group : obj1->grouplist.handles()) {
		builder.addToList(work->grouplist[group].indexes, groupPositions(*obj1, group));
	}
	
	for(VertexGroupId group : obj2->grouplist.handles()) {
		builder.addToList(work->grouplist[group].indexes, groupPositions(*obj2, group));
	}
	
	work->vertexWorldPositions.resize(work->vertexlist.size());
	work->vertexClipPositions.resize(work->vertexlist.size());
	work->vertexColors.resize(work->vertexlist.size());
	
	return work;
}

void EERIE_MESH_TWEAK_Do(Entity * io, TweakType tw, const res::path & path) {
	
	if(!io || !io->obj) {
		return;
	}
	
	res::path ftl_file = ("game" / path).set_ext("ftl");
	if((!g_resources->getFile(ftl_file)) && (!g_resources->getFile(path))) {
		return;
	}
	
	if(path.empty() && tw == TWEAK_REMOVE) {
		if(io->tweaky) {
			delete io->obj;
			io->obj = io->tweaky;
			EERIE_Object_Precompute_Fast_Access(io->obj);
			io->tweaky = nullptr;
		}
		return;
	}
	
	if(!(tw & (TWEAK_HEAD | TWEAK_TORSO | TWEAK_LEGS))) {
		return;
	}
	
	std::unique_ptr<EERIE_3DOBJ> tobj = loadObject(path);
	if(!tobj) {
		return;
	}
	
	std::unique_ptr<EERIE_3DOBJ> result;
	if(tw == (TWEAK_HEAD | TWEAK_TORSO | TWEAK_LEGS)) {
		
		result = std::move(tobj); // Replace the entire mesh
		
	} else {
		
		if(tw & TWEAK_HEAD) {
			result = CreateIntermediaryMesh(io->obj, tobj.get(), TWEAK_HEAD);
			if(!result) {
				return;
			}
		}
		if(tw & TWEAK_TORSO) {
			result = CreateIntermediaryMesh(result ? result.get() : io->obj, tobj.get(), TWEAK_TORSO);
			if(!result) {
				return;
			}
		}
		if(tw & TWEAK_LEGS) {
			result = CreateIntermediaryMesh(result ? result.get() : io->obj, tobj.get(), TWEAK_LEGS);
			if(!result) {
				return;
			}
		}
		
		EERIE_Object_Precompute_Fast_Access(result.get());
		
		EERIE_CreateCedricData(result.get());
		
		// TODO also do this for the other branch?
		io->animBlend.lastanimtime = 0;
		io->animBlend.m_active = false;
		
	}
	
	if(!io->tweaky) {
		io->tweaky = io->obj;
	} else if(io->tweaky != io->obj) {
		delete io->obj;
	}
	
	io->obj = result.release();
	
}
