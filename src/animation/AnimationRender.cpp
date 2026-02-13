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

#include "animation/AnimationRender.h"

#include <stddef.h>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <optional>
#include <vector>

#include "animation/Animation.h"

#include "core/Application.h"
#include "core/GameTime.h"
#include "core/Core.h"

#include "game/Damage.h"
#include "game/Equipment.h"
#include "game/EntityManager.h"
#include "game/NPC.h"
#include "game/Player.h"
#include "game/Spells.h"
#include "game/spell/Cheat.h"

#include "graphics/BaseGraphicsTypes.h"
#include "graphics/Draw.h"
#include "graphics/GlobalFog.h"
#include "graphics/GraphicsTypes.h"
#include "graphics/Math.h"
#include "graphics/Renderer.h"
#include "graphics/Vertex.h"
#include "graphics/data/Mesh.h"
#include "graphics/data/MeshManipulation.h"
#include "graphics/data/TextureContainer.h"
#include "graphics/texture/Texture.h"
#include "graphics/texture/TextureStage.h"
#include "graphics/particle/ParticleEffects.h"
#include "graphics/effects/Decal.h"
#include "graphics/effects/Halo.h"

#include "gui/Dragging.h"

#include "math/Angle.h"
#include "math/RandomVector.h"
#include "math/Vector.h"

#include "physics/Collisions.h"

#include "platform/Platform.h"
#include "platform/profiler/Profiler.h"
#if ARX_PLATFORM == ARX_PLATFORM_VITA
#include "platform/vita/VitaInit.h"
#endif

#include "scene/Light.h"
#include "scene/GameSound.h"
#include "scene/Scene.h"
#include "scene/Interactive.h"
#include "scene/Tiles.h"

// List of TO-TREAT vertex for MIPMESHING

// TODO: Convert to a RenderBatch & make TextureContainer constructor private
static TextureContainer TexSpecialColor("specialcolor_list", TextureContainer::NoInsert);

#if ARX_PLATFORM == ARX_PLATFORM_VITA
static std::vector<TextureContainer *> s_activeModelTextures;

// Interleaved vertex cache: packs clip position + color into one contiguous array
// so the face loop does 1 cache line load per vertex instead of 2 separate arrays.
struct VitaVertexRender {
	Vec4f clipPos;    // 16B
	ColorRGBA color;  //  4B
};                    // 20B — fits ~1.5 per 32B cache line

static std::vector<VitaVertexRender> s_vitaVertexCache;

static void vitaBuildVertexCache(const EERIE_3DOBJ * eobj) {
	size_t count = eobj->vertexClipPositions.size();
	s_vitaVertexCache.resize(count);
	const Vec4f * clipPos = eobj->vertexClipPositions.data();
	const ColorRGBA * colors = eobj->vertexColors.data();
	VitaVertexRender * dst = s_vitaVertexCache.data();
	for(size_t i = 0; i < count; i++) {
		dst[i].clipPos = clipPos[i];
		dst[i].color = colors[i];
	}
}
#endif

static TexturedVertex * PushVertexInTable(std::vector<TexturedVertex> & bucket) {
	bucket.resize(bucket.size() + 3);
	return &bucket[bucket.size() - 3];
}

#if ARX_PLATFORM == ARX_PLATFORM_VITA
static void vitaTrackActiveTexture(TextureContainer * tc) {
	if(tc->m_modelBatch[0].empty() && tc->m_modelBatch[1].empty()
	   && tc->m_modelBatch[2].empty() && tc->m_modelBatch[3].empty()
	   && tc->m_modelBatch[4].empty()) {
		s_activeModelTextures.push_back(tc);
	}
}
#endif

static void PopOneTriangleList(RenderState baseState, TextureContainer * material,
                               bool clear, bool manageTextureState = true) {

	std::vector<TexturedVertex> & bucket = material->m_modelBatch[BatchBucket_Opaque];

	if(bucket.empty()) {
		return;
	}

	GRenderer->SetTexture(0, material);
	baseState.setAlphaCutout(material->m_pTexture && material->m_pTexture->hasAlpha());

	UseRenderState state(baseState);

	// On Vita, callers hoist UseTextureState outside the per-material loop
	std::optional<UseTextureState> textureState;
	if(manageTextureState) {
		textureState.emplace(TextureStage::FilterLinear, TextureStage::WrapClamp);
	}

	if(material->userflags & POLY_LATE_MIP) {
		const float GLOBAL_NPC_MIPMAP_BIAS = -2.2f;
		GRenderer->GetTextureStage(0)->setMipMapLODBias(GLOBAL_NPC_MIPMAP_BIAS);
	}

	EERIEDRAWPRIM(Renderer::TriangleList, bucket.data(), bucket.size());

	if(clear) {
		bucket.clear();
	}

	if(material->userflags & POLY_LATE_MIP) {
		float biasResetVal = 0;
		GRenderer->GetTextureStage(0)->setMipMapLODBias(biasResetVal);
	}

}

static void PopOneTriangleListTransparency(TextureContainer * material,
                                           bool manageTextureState = true) {

	std::vector<TexturedVertex> * batch = material->m_modelBatch;

	if(batch[BatchBucket_Blended].empty()
	   && batch[BatchBucket_Additive].empty()
	   && batch[BatchBucket_Subtractive].empty()
	   && batch[BatchBucket_Multiplicative].empty()) {
		return;
	}

	RenderState baseState = render3D().depthWrite(false);

	std::optional<UseTextureState> textureState;
	if(manageTextureState) {
		textureState.emplace(TextureStage::FilterLinear, TextureStage::WrapClamp);
	}

	GRenderer->SetTexture(0, material);
	baseState.setAlphaCutout(material->m_pTexture && material->m_pTexture->hasAlpha());
	
	if(!batch[BatchBucket_Blended].empty()) {
		UseRenderState state(baseState.blend(BlendDstColor, BlendSrcColor));
		EERIEDRAWPRIM(Renderer::TriangleList, batch[BatchBucket_Blended].data(),
		                                      batch[BatchBucket_Blended].size());
		batch[BatchBucket_Blended].clear();
	}
	
	if(!batch[BatchBucket_Additive].empty()) {
		UseRenderState state(baseState.blend(BlendOne, BlendOne));
		EERIEDRAWPRIM(Renderer::TriangleList, batch[BatchBucket_Additive].data(),
		                                      batch[BatchBucket_Additive].size());
		batch[BatchBucket_Additive].clear();
	}
	
	if(!batch[BatchBucket_Subtractive].empty()) {
		UseRenderState state(baseState.blend(BlendZero, BlendInvSrcColor));
		EERIEDRAWPRIM(Renderer::TriangleList, batch[BatchBucket_Subtractive].data(),
		                                      batch[BatchBucket_Subtractive].size());
		batch[BatchBucket_Subtractive].clear();
	}
	
	if(!batch[BatchBucket_Multiplicative].empty()) {
		UseRenderState state(baseState.blend(BlendOne, BlendOne));
		EERIEDRAWPRIM(Renderer::TriangleList, batch[BatchBucket_Multiplicative].data(),
		                                      batch[BatchBucket_Multiplicative].size());
		batch[BatchBucket_Multiplicative].clear();
	}
	
}

void PopAllTriangleListOpaque(RenderState baseState, bool clear) {

	ARX_PROFILE_FUNC();

	#if ARX_PLATFORM == ARX_PLATFORM_VITA
	// Sort by pointer to group same-texture draws together, reducing bind thrashing
	std::sort(s_activeModelTextures.begin(), s_activeModelTextures.end());
	// Hoist texture state outside per-material loop — all entity draws use same filter/wrap
	UseTextureState textureState(TextureStage::FilterLinear, TextureStage::WrapClamp);
	for(TextureContainer * pTex : s_activeModelTextures) {
		PopOneTriangleList(baseState, pTex, clear, false);
	}
	#else
	// TODO sort texture list according to material properties to reduce state changes
	TextureContainer * pTex = GetTextureList();
	while(pTex) {
		PopOneTriangleList(baseState, pTex, clear);
		pTex = pTex->m_pNext;
	}
	#endif

}

void PopAllTriangleListTransparency() {

	ARX_PROFILE_FUNC();

	GRenderer->SetFogColor(Color());

	PopOneTriangleList(render3D().depthWrite(false).blend(BlendDstColor, BlendOne), &TexSpecialColor, true);

	#if ARX_PLATFORM == ARX_PLATFORM_VITA
	{
		UseTextureState textureState(TextureStage::FilterLinear, TextureStage::WrapClamp);
		for(TextureContainer * pTex : s_activeModelTextures) {
			PopOneTriangleListTransparency(pTex, false);
		}
	}
	s_activeModelTextures.clear();
	#else
	TextureContainer * pTex = GetTextureList();
	while(pTex) {
		PopOneTriangleListTransparency(pTex);
		pTex = pTex->m_pNext;
	}
	#endif

	GRenderer->SetFogColor(g_fogColor);

}


extern bool EXTERNALVIEW;

float Cedric_GetInvisibility(Entity * io) {
	
	if(!io) {
		return 0.f;
	}
	
	float invisibility = io->invisibility;
	if(invisibility > 1.f) {
		invisibility -= 1.f;
	}
	
	if(io != entities.player() && invisibility > 0.f && !EXTERNALVIEW) {
		Spell * spell = spells.getSpellOnTarget(io->index(), SPELL_INVISIBILITY);
		if(spell) {
			if(player.m_skillFull.intuition > spell->m_level * 10) {
				invisibility -= player.m_skillFull.intuition * (1.0f / 100) + spell->m_level * (1.0f / 10);
				invisibility = glm::clamp(invisibility, 0.1f, 1.f);
			}
		}
	}
	
	if(io == g_draggedEntity && g_dragStatus != EntityDragStatus_OnGround) {
		invisibility = glm::clamp(invisibility + 0.5f, 0.1f, 1.f);
	}
	
	return invisibility;
}

// TODO Move somewhere else
void Cedric_ApplyLightingFirstPartRefactor(Entity & io) {
	
	io.special_color = Color3f::white;

	float poisonpercent = 0.f;
	float trappercent = 0.f;
	float secretpercent = 0.f;

	if((io.ioflags & IO_NPC) && io._npcdata->poisonned > 0.f) {
		poisonpercent = io._npcdata->poisonned * 0.05f;
		if(poisonpercent > 1.f)
			poisonpercent = 1.f;
	}

	if((io.ioflags & IO_ITEM) && io.poisonous > 0.f && io.poisonous_count) {
		poisonpercent = io.poisonous * (1.0f / 20);
		if(poisonpercent > 1.f)
			poisonpercent = 1.f;
	}

	if((io.ioflags & IO_FIX) && io._fixdata->trapvalue > -1) {
		trappercent = player.TRAP_DETECT - io._fixdata->trapvalue;
		if(trappercent > 0.f) {
			trappercent = 0.6f + trappercent * 0.01f;
			trappercent = glm::clamp(trappercent, 0.6f, 1.f);
		}
	}

	if((io.ioflags & IO_FIX) && io.secretvalue > -1) {
		secretpercent = player.TRAP_SECRET - io.secretvalue;
		if(secretpercent > 0.f) {
			secretpercent = 0.6f + secretpercent * 0.01f;
			secretpercent = glm::clamp(secretpercent, 0.6f, 1.f);
		}
	}

	if(poisonpercent > 0.f) {
		io.special_color = Color3f::green;
	}

	if(trappercent > 0.f) {
		io.special_color = Color3f(trappercent, 1.f - trappercent, 1.f - trappercent);
	}

	if(secretpercent > 0.f) {
		io.special_color = Color3f(1.f - secretpercent, 1.f - secretpercent, secretpercent);
	}

	if(io.ioflags & IO_FREEZESCRIPT) {
		io.special_color = Color3f::blue;
	}

	if(io.sfx_flag & SFX_TYPE_YLSIDE_DEATH) {
		if(io.show == SHOW_FLAG_TELEPORTING) {
			io.sfx_time = io.sfx_time + g_gameTime.lastFrameDuration();
			if(io.sfx_time >= g_gameTime.now()) {
				io.sfx_time = g_gameTime.now();
			}
		} else {
			const GameDuration elapsed = g_gameTime.now() - io.sfx_time;

			if(elapsed > 0) {
				if(elapsed < 3s) { // 5 seconds to red
					float ratio = elapsed / 3s;
					io.special_color = Color3f(1.f, 1.f - ratio, 1.f - ratio);
					io.highlightColor += Color3f(std::max(ratio - 0.5f, 0.f), 0.f, 0.f) * 255;
					AddRandomSmoke(io, 1);
				} else if(elapsed < 6s) { // 5 seconds to White
					float ratio = elapsed / 3s;
					io.special_color = Color3f::red;
					io.highlightColor += Color3f(std::max(ratio - 0.5f, 0.f), 0.f, 0.f) * 255;
					AddRandomSmoke(io, 2);
				} else { // SFX finish
					io.sfx_time = 0;

					if(io.ioflags & IO_NPC) {
						MakePlayerAppearsFX(io);
						AddRandomSmoke(io, 50);
						Color3f rgb(io._npcdata->blood_color);
						Sphere sp = Sphere(io.pos, 200.f);
						
						long count = 6;
						while(count--) {
							Sphere splatSphere = Sphere(sp.origin, Random::getf(30.f, 60.f));
							PolyBoomAddSplat(splatSphere, rgb, 1);
							sp.origin.y -= Random::getf(0.f, 150.f);
							ARX_PARTICLES_Spawn_Splat(sp.origin, 200.f, io._npcdata->blood_color);
							sp.origin = io.pos + arx::randomVec3f() * Vec3f(200.f, 20.f, 200.f) - Vec3f(100.f, 10.f, 100.f);
							sp.radius = Random::getf(100.f, 200.f);
						}
						
						EERIE_LIGHT * light = dynLightCreate();
						if(light) {
							light->intensity = Random::getf(0.7f, 2.7f);
							light->fallend = 600.f;
							light->fallstart = 400.f;
							light->rgb = Color3f(1.0f, 0.8f, 0.f);
							light->pos = io.pos + Vec3f(0.f, -80.f, 0.f);
							light->duration = 600ms;
						}

						if(io.sfx_flag & SFX_TYPE_INCINERATE) {
							io.sfx_flag &= ~SFX_TYPE_INCINERATE;
							io.sfx_flag &= ~SFX_TYPE_YLSIDE_DEATH;
							Spell * spell = spells.getSpellOnTarget(io.index(), SPELL_INCINERATE);

							if(!spell)
								spell = spells.getSpellOnTarget(io.index(), SPELL_MASS_INCINERATE);

							if(spell) {
								spells.endSpell(spell);
								float damages = ARX_SPELLS_ApplyFireProtection(&io, 20 * spell->m_level);
								Entity * caster = entities.get(spell->m_caster);
								damageNpc(io, damages, caster, spell, DAMAGE_TYPE_FIRE | DAMAGE_TYPE_MAGICAL, caster ? &caster->pos : &io.pos);
								ARX_SOUND_PlaySFX(g_snd.SPELL_FIRE_HIT, &io.pos);
							}
						} else {
							io.sfx_flag &= ~SFX_TYPE_YLSIDE_DEATH;
							ARX_INTERACTIVE_DestroyIOdelayed(&io);
						}
					}
				}
			}
		}
	}
}

static void Cedric_PrepareHalo(EERIE_3DOBJ * eobj, Skeleton * obj) {
	
	Vec3f cam_vector = angleToVector(g_camera->angle);
	
	arx_assume(eobj->vertexWorldPositions.size() == eobj->vertexlist.size());
	arx_assume(eobj->m_boneVertices.size() == obj->bones.size());
	
	// Apply light on all vertices
	for(VertexGroupId group : obj->bones.handles()) {
		
		Vec3f t_vector = glm::inverse(obj->bones[group].anim.quat) * cam_vector;
		
		// Get light value for each vertex
		for(VertexId vertex : eobj->m_boneVertices[group]) {
			// Get cos angle between light and vertex norm
			eobj->vertexWorldPositions[vertex].norm.z = glm::dot(eobj->vertexlist[vertex].norm, t_vector);
		}
		
	}
	
}

static TexturedVertex * GetNewVertexList(std::vector<TexturedVertex> * batch,
                                         const EERIE_FACE & face, float invisibility,
                                         float & fTransp) {
	
	fTransp = 0.f;
	
	if((face.facetype & POLY_TRANS) || invisibility > 0.f) {
		if(invisibility > 0.f) {
			fTransp = 2.f - invisibility;
		} else {
			fTransp = face.transval;
		}
		if(fTransp >= 2.f) {
			fTransp *= (1.f / 2);
			fTransp += 0.5f;
			return PushVertexInTable(batch[BatchBucket_Multiplicative]);
		} else if(fTransp >= 1.f) {
			fTransp -= 1.f;
			return PushVertexInTable(batch[BatchBucket_Additive]);
		} else if(fTransp > 0.f) {
			fTransp = 1.f - fTransp;
			return PushVertexInTable(batch[BatchBucket_Blended]);
		} else {
			fTransp = 1.f - fTransp;
			return PushVertexInTable(batch[BatchBucket_Subtractive]);
		}
	} else {
		return PushVertexInTable(batch[BatchBucket_Opaque]);
	}
	
}

void drawQuadRTP(const RenderMaterial & mat, TexturedQuad quat) {
	
	worldToClipSpace(quat.v[0].p, quat.v[0]);
	worldToClipSpace(quat.v[1].p, quat.v[1]);
	worldToClipSpace(quat.v[2].p, quat.v[2]);
	worldToClipSpace(quat.v[3].p, quat.v[3]);
	
	g_renderBatcher.add(mat, quat);
}

void drawTriangle(const RenderMaterial & mat, const TexturedVertexUntransformed * vertices) {
	
	TexturedVertex projected[3];
	worldToClipSpace(vertices[0].p, projected[0]);
	worldToClipSpace(vertices[1].p, projected[1]);
	worldToClipSpace(vertices[2].p, projected[2]);
	projected[0].color = vertices[0].color;
	projected[0].uv = vertices[0].uv;
	projected[1].color = vertices[1].color;
	projected[1].uv = vertices[1].uv;
	projected[2].color = vertices[2].color;
	projected[2].uv = vertices[2].uv;
	
	g_renderBatcher.add(mat, projected);
}

static bool Cedric_IO_Visible(const Vec3f & pos) {
	
	ARX_PROFILE_FUNC();
	
	return !g_tiles || g_tiles->isNearActiveTile(pos);
}

/* Object dynamic lighting */
static void Cedric_ApplyLighting(ShaderLight lights[], size_t lightsCount, EERIE_3DOBJ * eobj, Skeleton * obj, const ColorMod & colorMod) {
	
	ARX_PROFILE_FUNC();
	
	arx_assert(eobj->vertexColors.size() == eobj->vertexWorldPositions.size());
	arx_assert(eobj->vertexColors.size() == eobj->vertexlist.size());
	arx_assert(eobj->m_boneVertices.size() == obj->bones.size());
	
	/* Apply light on all vertices */
	for(VertexGroupId group : obj->bones.handles()) {
		#if ARX_PLATFORM == ARX_PLATFORM_VITA
		// Precompute rotation matrix per bone (avoids per-vertex quat*vec3 conversion)
		const glm::mat3 rotMat = glm::mat3_cast(obj->bones[group].anim.quat);
		for(VertexId vertex : eobj->m_boneVertices[group]) {
			const Vec3f & position = eobj->vertexWorldPositions[vertex].v;
			Vec3f normal = rotMat * eobj->vertexlist[vertex].norm;
			eobj->vertexColors[vertex] = ApplyLight(lights, lightsCount, position, normal, colorMod);
		}
		#else
		const glm::quat & quat = obj->bones[group].anim.quat;
		/* Get light value for each vertex */
		for(VertexId vertex : eobj->m_boneVertices[group]) {
			const Vec3f & position = eobj->vertexWorldPositions[vertex].v;
			Vec3f normal = quat * eobj->vertexlist[vertex].norm;
			eobj->vertexColors[vertex] = ApplyLight(lights, lightsCount, position, normal, colorMod);
		}
		#endif
	}
	
}

static EERIE_3D_BBOX UpdateBbox3d(EERIE_3DOBJ * eobj) {
	
	EERIE_3D_BBOX box3D;
	box3D.reset();
	
	for(const EERIE_VERTEX & vertex : eobj->vertexWorldPositions) {
		box3D.add(vertex.v);
	}
	
	return box3D;
}

EERIE_2D_BBOX UpdateBbox2d(const EERIE_3DOBJ & eobj) {
	
	EERIE_2D_BBOX box2D;
	box2D.reset();
	
	for(Vec4f vertex : eobj.vertexClipPositions) {
		
		if(vertex.w <= 0.f) {
			continue;
		}
		
		Vec3f p = Vec3f(vertex) / vertex.w;
		if(p.x >= -32000 && p.x <=  32000 && p.y >= -32000 && p.y <=  32000) {
			box2D.add(p);
		}
		
	}
	
	return box2D;
}

void DrawEERIEInter_ModelTransform(EERIE_3DOBJ * eobj, const TransformInfo & t) {
	
	arx_assume(eobj->vertexWorldPositions.size() == eobj->vertexlist.size());
	
	for(VertexId vertex : eobj->vertexlist.handles()) {
		eobj->vertexWorldPositions[vertex].v = t(eobj->vertexlist[vertex].v);
	}
	
}

void DrawEERIEInter_ViewProjectTransform(EERIE_3DOBJ * eobj) {
	
	arx_assert(eobj->vertexClipPositions.size() == eobj->vertexWorldPositions.size());
	
	for(VertexId vertex : eobj->vertexWorldPositions.handles()) {
		eobj->vertexClipPositions[vertex] = worldToClipSpace(eobj->vertexWorldPositions[vertex].v);
	}
	
}

static bool CullFace(const EERIE_3DOBJ * eobj, const EERIE_FACE & face) {
	
	if(!(face.facetype & POLY_DOUBLESIDED)) {
		Vec3f normV10 = eobj->vertexWorldPositions[face.vid[1]].v - eobj->vertexWorldPositions[face.vid[0]].v;
		Vec3f normV20 = eobj->vertexWorldPositions[face.vid[2]].v - eobj->vertexWorldPositions[face.vid[0]].v;
		Vec3f normFace;
		normFace.x = (normV10.y * normV20.z) - (normV10.z * normV20.y);
		normFace.y = (normV10.z * normV20.x) - (normV10.x * normV20.z);
		normFace.z = (normV10.x * normV20.y) - (normV10.y * normV20.x);
		Vec3f nrm = eobj->vertexWorldPositions[face.vid[0]].v - g_camera->m_pos;
		if(glm::dot(normFace, nrm) > 0.f) {
			return true;
		}
	}
	
	return false;
}

// TODO copy-paste halo
static void AddFixedObjectHalo(const EERIE_FACE & face, const TransformInfo & t,
                               const IO_HALO & halo, TexturedVertex * tvList,
                               const EERIE_3DOBJ * eobj) {
	
	float mdist = g_camera->cdepth;
	float ddist = mdist - fdist(t.pos, g_camera->m_pos);
	ddist = ddist / mdist;
	ddist = glm::pow(ddist, 6.f);
	ddist = glm::clamp(ddist, 0.25f, 0.9f);
	
	float tot = 0;
	float _ffr[3];
	
	for(long o = 0; o < 3; o++) {
		Vec3f temporary3D = t.rotation * eobj->vertexlist[face.vid[o]].norm;
		float power = glm::clamp(1.f - glm::abs(temporary3D.z * 0.5f), 0.f, 1.f);
		tot += power * 255.f;
		_ffr[o] = power * 255.f;
		tvList[o].color = (halo.color * power).toRGB();
	}
	
	if(tot > 150.f) {
		long first;
		long second;
		long third;

		if(_ffr[0] >= _ffr[1] && _ffr[1] >= _ffr[2]) {
			first = 0;
			second = 1;
			third = 2;
		} else if(_ffr[0] >= _ffr[2] && _ffr[2] >= _ffr[1]) {
			first = 0;
			second = 2;
			third = 1;
		} else if(_ffr[1] >= _ffr[0] && _ffr[0] >= _ffr[2]) {
			first = 1;
			second = 0;
			third = 2;
		} else if(_ffr[1] >= _ffr[2] && _ffr[2] >= _ffr[0]) {
			first = 1;
			second = 2;
			third = 0;
		} else if(_ffr[2] >= _ffr[0] && _ffr[0] >= _ffr[1]) {
			first = 2;
			second = 0;
			third = 1;
		} else {
			first = 2;
			second = 1;
			third = 0;
		}

		if(_ffr[first] > 70.f && _ffr[second] > 60.f) {
			float wave = timeWaveSin(g_gameTime.now(), 200ms * glm::pi<float>());
			float siz = ddist * (halo.radius * 1.5f * (wave * .1f + .7f)) * .6f;
			
			TexturedVertex vert[4];
			vert[0] = tvList[first];
			vert[1] = tvList[first];
			vert[2] = tvList[second];
			vert[3] = tvList[second];
			
			Vec3f a = tvList[first].p / tvList[first].w;
			Vec3f b = tvList[second].p / tvList[second].w;
			Vec3f c = tvList[third].p / tvList[third].w;
			
			Vec2f vect1 = Vec2f(a - c);
			vect1 /= ffsqrt(arx::length2(vect1));
			if(vect1.x < 0.f) {
				vect1 *= 1.2f;
			}
			vert[1].p.x += (vect1.x + Random::getf(0.1f, 0.2f)) * siz * vert[1].w;
			vert[1].p.y += (vect1.y + Random::getf(0.1f, 0.2f)) * siz * vert[1].w;
			
			Vec2f vect2 = Vec2f(b - c);
			vect2 /= ffsqrt(arx::length2(vect2));
			if(vect2.x < 0.f) {
				vect2 *= 1.2f;
			}
			vert[2].p.x += (vect2.x + Random::getf(0.1f, 0.2f)) * siz * vert[2].w;
			vert[2].p.y += (vect2.y + Random::getf(0.1f, 0.2f)) * siz * vert[2].w;
			
			vert[0].p.z += 0.0001f * vert[0].w;
			vert[3].p.z += 0.0001f * vert[3].w;
			
			vert[1].color = Color::black.toRGB();
			vert[2].color = ((halo.flags & HALO_NEGATIVE) ? Color() : Color::black).toRGBA();
			
			Halo_AddVertices(vert);
		}
	}
}

extern float WATEREFFECT;

void DrawEERIEInter_Render(EERIE_3DOBJ * eobj, const TransformInfo & t, Entity * io, float invisibility) {

	ColorMod colorMod;
	colorMod.updateFromEntity(io, !io);

	Vec3f tv = t.pos;

	if(io && (io->ioflags & IO_ITEM))
		tv.y -= 60.f;
	else
		tv.y -= 90.f;
	
	bool useFaceNormal = io && (io->ioflags & IO_ANGULAR);
	
	ShaderLight lights[llightsSize];
	size_t lightsCount = 0;

	#if ARX_PLATFORM == ARX_PLATFORM_VITA
	float vitaDist2 = arx::distance2(tv, g_camera->m_pos);
	float vitaFogEnd2 = square(g_camera->cdepth * fZFogEnd);
	float vitaFogMid2 = square(g_camera->cdepth * 0.4f);
	bool vitaSkipLighting = vitaDist2 > vitaFogEnd2;
	if(!vitaSkipLighting) {
		UpdateLlights(lights, lightsCount, tv, false);
		if(vitaDist2 > vitaFogMid2 && lightsCount > 1) {
			lightsCount = 1;
		}
	}
	ColorRGBA vitaAmbientColor = colorMod.ambientColor.toRGB();
	#else
		UpdateLlights(lights, lightsCount, tv, false);
	#endif

	arx_assert(eobj->vertexColors.size() == eobj->vertexWorldPositions.size());

	#if ARX_PLATFORM == ARX_PLATFORM_VITA
	// Pre-reserve batch vectors to avoid per-face reallocation
	for(MaterialId mat : eobj->materials.handles()) {
		if(TextureContainer * pTex = eobj->materials[mat]) {
			size_t estimate = eobj->facelist.size() * 3;
			pTex->m_modelBatch[BatchBucket_Opaque].reserve(
				pTex->m_modelBatch[BatchBucket_Opaque].size() + estimate);
		}
	}
	#endif

	#if ARX_PLATFORM == ARX_PLATFORM_VITA
	const VitaVertexRender * vitaCacheB = s_vitaVertexCache.data();
	const EERIE_FACE * faceDataB = eobj->facelist.data();
	const size_t faceCountB = eobj->facelist.size();
	#endif

	for(size_t i = 0; i < eobj->facelist.size(); i++) {
		const EERIE_FACE & face = eobj->facelist[i];

		#if ARX_PLATFORM == ARX_PLATFORM_VITA
		if(i + 2 < faceCountB) {
			__builtin_prefetch(&faceDataB[i + 2], 0, 1);
			__builtin_prefetch(&vitaCacheB[faceDataB[i + 2].vid[0].handleData()], 0, 1);
		}
		#endif

		if(CullFace(eobj, face) || !face.material) {
			continue;
		}

		TextureContainer * pTex = eobj->materials[face.material];
		if(!pTex) {
			continue;
		}

		#if ARX_PLATFORM == ARX_PLATFORM_VITA
		vitaTrackActiveTexture(pTex);
		#endif

		float fTransp = 0.f;
		TexturedVertex * tvList = GetNewVertexList(pTex->m_modelBatch, face, invisibility, fTransp);

		Vec3f faceTransformedNorm;
		if(useFaceNormal) {
			faceTransformedNorm = t.rotation * face.norm;
		}

		for(size_t n = 0; n < 3; n++) {

			#if ARX_PLATFORM == ARX_PLATFORM_VITA
			if(vitaSkipLighting) {
				eobj->vertexColors[face.vid[n]] = vitaAmbientColor;
			} else {
				const Vec3f & position = eobj->vertexWorldPositions[face.vid[n]].v;
				Vec3f normal = useFaceNormal ? faceTransformedNorm : t.rotation * eobj->vertexlist[face.vid[n]].norm;
				float diffuse = useFaceNormal ? 0.5f : 1.f;
				eobj->vertexColors[face.vid[n]] = ApplyLight(lights, lightsCount, position, normal, colorMod, diffuse);
			}
			const VitaVertexRender & vr = s_vitaVertexCache[face.vid[n].handleData()];
			tvList[n].p = Vec3f(vr.clipPos);
			tvList[n].w = vr.clipPos.w;
			#else
			const Vec3f & position = eobj->vertexWorldPositions[face.vid[n]].v;
			Vec3f normal = useFaceNormal ? faceTransformedNorm : t.rotation * eobj->vertexlist[face.vid[n]].norm;
			float diffuse = useFaceNormal ? 0.5f : 1.f;
			eobj->vertexColors[face.vid[n]] = ApplyLight(lights, lightsCount, position, normal, colorMod, diffuse);
			tvList[n].p = Vec3f(eobj->vertexClipPositions[face.vid[n]]);
			tvList[n].w = eobj->vertexClipPositions[face.vid[n]].w;
			#endif
			tvList[n].uv.x = face.u[n];
			tvList[n].uv.y = face.v[n];
			
			// Treat WATER Polys (modify UVs)
			if(face.facetype & POLY_WATER) {
				tvList[n].uv += getWaterFxUvOffset(WATEREFFECT, eobj->vertexlist[face.vid[n]].v) * (0.3f * 0.05f);
			}
			
			if(face.facetype & POLY_GLOW) {
				// unaffected by light
				tvList[n].color = Color::white.toRGB();
			} else {
				// Normal Illuminations
				tvList[n].color = eobj->vertexColors[face.vid[n]];
			}
			
			// TODO copy-paste
			if(io && player.m_improve) {
				
				float lr = Color4f::fromRGBA(tvList[n].color).r;
				
				float dd = 1.f / tvList[n].w;

				dd = glm::clamp(dd, 0.f, 1.f);

				Vec3f & norm = eobj->vertexlist[face.vid[n]].norm;
				
				float fb = ((1.f - dd) * 6.f + (glm::abs(norm.x) + glm::abs(norm.y))) * 0.125f;
				float fr = ((0.6f - dd) * 6.f + (glm::abs(norm.z) + glm::abs(norm.y))) * 0.125f;
				
				if(fr < 0.f) {
					fr = 0.f;
				} else {
					fr = std::max(lr, fr);
				}
				
				tvList[n].color = Color::rgb(fr, 0.118f, fb).toRGB();
			}

			// Transparent poly: storing info to draw later
			if((face.facetype & POLY_TRANS) || invisibility > 0.f) {
				tvList[n].color = Color::gray(fTransp).toRGB();
			}
		}

		// HALO HANDLING START
		#if ARX_PLATFORM == ARX_PLATFORM_VITA
		if(false) { // Halos disabled on Vita
		#else
		if(io && (io->halo.flags & HALO_ACTIVE)) {
		#endif
			const IO_HALO & halo = io->halo;
			AddFixedObjectHalo(face, t, halo, tvList, eobj);
		}
	}
}

void DrawEERIEInter(EERIE_3DOBJ * eobj,
                    const TransformInfo & t,
                    Entity * io,
                    bool forceDraw,
                    float invisibility
) {
	ARX_PROFILE_FUNC();
	
	if(!eobj)
		return;

	// Avoids To treat an object that isn't Visible
	if(!forceDraw && io && io != entities.player() && !Cedric_IO_Visible(t.pos))
		return;

	DrawEERIEInter_ModelTransform(eobj, t);
	if(io) {
		io->bbox3D = UpdateBbox3d(eobj);
	}

	DrawEERIEInter_ViewProjectTransform(eobj);
	if(io) {
		io->bbox2D = UpdateBbox2d(*eobj);
	}

	if(!forceDraw && ARX_SCENE_PORTAL_ClipIO(io, t.pos))
		return;

	#if ARX_PLATFORM == ARX_PLATFORM_VITA
	vitaBuildVertexCache(eobj);
	#endif

	DrawEERIEInter_Render(eobj, t, io, invisibility);
}


struct HaloRenderInfo {
	
	HaloRenderInfo()
		: halo(nullptr)
	{ }
	
	explicit  HaloRenderInfo(IO_HALO * halo_, VertexSelectionId selection_ = { })
		: halo(halo_)
		, selection(selection_)
	{ }
	
	IO_HALO * halo;
	VertexSelectionId selection;
	
};

struct HaloInfo {
	static const u32 maxSize = 6;
	
	u32 size;
	HaloRenderInfo entries[maxSize];
	float MAX_ZEDE;
	float ddist;

	HaloInfo()
		: size(0)
		, MAX_ZEDE(0.f)
		, ddist(0.f)
	{}
	
	void push(const HaloRenderInfo & entry) {
		arx_assert(size < maxSize);
		entries[size] = entry;
		size++;
	}
};

static void pushSlotHalo(HaloInfo & haloInfo, EquipmentSlot slot, VertexSelectionId selection) {
	
	if(Entity * item = entities.get(player.equiped[slot])) {
		if(item->halo.flags & HALO_ACTIVE) {
			haloInfo.push(HaloRenderInfo(&item->halo, selection));
		}
	}
	
}

//-----------------------------------------------------------------------------
extern long IN_BOOK_DRAW;

static void PrepareAnimatedObjectHalo(HaloInfo & haloInfo, const Vec3f & pos,
                                      Skeleton * obj, EERIE_3DOBJ * eobj) {
	
	Vec3f ftrPos = pos;
	// TODO copy-pase
	float mdist = g_camera->cdepth;
	mdist *= 0.5f;
	
	float ddist = mdist - fdist(ftrPos, g_camera->m_pos);
	ddist = ddist / mdist;
	ddist = glm::pow(ddist, 6.f);
	ddist = glm::clamp(ddist, 0.25f, 0.9f);
	
	haloInfo.ddist = ddist;
	
	Cedric_PrepareHalo(eobj, obj);
	
	haloInfo.MAX_ZEDE = 0.f;
	for(Vec4f vertex : eobj->vertexClipPositions) {
		if(vertex.w > 0.f) {
			haloInfo.MAX_ZEDE = std::max(vertex.z / vertex.w, haloInfo.MAX_ZEDE);
		}
	}
	
}

// TODO copy-paste halo
static void AddAnimatedObjectHalo(const HaloInfo & haloInfo, const VertexId * paf,
                                  float invisibility, EERIE_3DOBJ * eobj, const Entity * io,
                                  TexturedVertex * tvList) {
	
	IO_HALO * curhalo = nullptr;
	
	for(size_t h = 0; h < haloInfo.size; h++) {
		const HaloRenderInfo & entry = haloInfo.entries[h];
		if(!entry.selection || IsInSelection(eobj, paf[0], entry.selection)) {
			curhalo = entry.halo;
			break;
		}
	}
	
	if(!curhalo) {
		return;
	}
	
	float tot = 0;
	float _ffr[3];
	ColorRGBA colors[3];
	
	for(size_t o = 0; o < 3; o++) {
		float tttz = glm::abs(eobj->vertexWorldPositions[paf[o]].norm.z) * 0.5f;
		float power =  glm::clamp((1.f - tttz) * (1.f - invisibility), 0.f, 1.f);
		tot += power * 255.f;
		_ffr[o] = power * 255.f;
		colors[o] = (curhalo->color * power).toRGB();
	}
	
	if(tot > 260) {
		long first;
		long second;
		long third;

		if(_ffr[0] >= _ffr[1] && _ffr[1] >= _ffr[2]) {
			first = 0;
			second = 1;
			third = 2;
		} else if(_ffr[0] >= _ffr[2] && _ffr[2] >= _ffr[1]) {
			first = 0;
			second = 2;
			third = 1;
		} else if(_ffr[1] >= _ffr[0] && _ffr[0] >= _ffr[2]) {
			first = 1;
			second = 0;
			third = 2;
		} else if(_ffr[1] >= _ffr[2] && _ffr[2] >= _ffr[0]) {
			first = 1;
			second = 2;
			third = 0;
		} else if(_ffr[2] >= _ffr[0] && _ffr[0] >= _ffr[1]) {
			first = 2;
			second = 0;
			third = 1;
		} else {
			first = 2;
			second = 1;
			third = 0;
		}

		if(_ffr[first] > 150.f && _ffr[second] > 110.f) {
			float wave = timeWaveSin(g_gameTime.now(), 200ms * glm::pi<float>());
			float siz = haloInfo.ddist * (curhalo->radius * (wave * .1f + 1.f)) * .6f;
			if(io == entities.player() && haloInfo.ddist > 0.8f && !EXTERNALVIEW) {
				siz *= 1.5f;
			}
			
			TexturedVertex vert[4];
			vert[0] = tvList[first];
			vert[1] = tvList[first];
			vert[2] = tvList[second];
			vert[3] = tvList[second];
			
			Vec3f a = tvList[first].p / tvList[first].w;
			Vec3f b = tvList[second].p / tvList[second].w;
			Vec3f c = tvList[third].p / tvList[third].w;
			
			float valll = 0.0006f + (glm::abs(a.z) + glm::abs(b.z) - 2 * glm::abs(c.z)) * 0.1f;
			valll = std::max(valll, 0.f);
			
			Vec2f vect1 = Vec2f(a - c);
			vect1 /= ffsqrt(arx::length2(vect1));
			if(vect1.x < 0.f) {
				vect1 *= 2.4f;
			} else {
				vect1 *= 2.f;
			}
			vert[1].p.x += (vect1.x + Random::getf(0.1f, 0.2f)) * (siz * vert[1].w);
			vert[1].p.y += (vect1.y + Random::getf(0.1f, 0.2f)) * (siz * vert[1].w);
			vert[1].p.z += valll * vert[1].w;
			vert[1].p.z = (vert[1].p.z + haloInfo.MAX_ZEDE * vert[1].w) * 0.5f;
			
			Vec2f vect2 = Vec2f(b - c);
			vect2 /= ffsqrt(arx::length2(vect2));
			if(vect2.x < 0.f) {
				vect2 *= 1.2f;
			}
			vert[2].p.x += (vect2.x + Random::getf(0.1f, 0.2f)) * (siz * vert[2].w);
			vert[2].p.y += (vect2.y + Random::getf(0.1f, 0.2f)) * (siz * vert[2].w);
			vert[2].p.z += valll * vert[2].w;
			vert[2].p.z = (vert[2].p.z + haloInfo.MAX_ZEDE * vert[2].w) * 0.5f;
			
			vert[0].p.z += 0.0001f * vert[0].w;
			vert[3].p.z += 0.0001f * vert[3].w;
			
			vert[0].color = colors[first];
			vert[1].color = Color::black.toRGB();
			vert[2].color = ((curhalo->flags & HALO_NEGATIVE) ? Color() : Color::black).toRGBA();
			vert[3].color = colors[second];
			
			Halo_AddVertices(vert);
		}
	}
}

static void Cedric_RenderObject(EERIE_3DOBJ * eobj, Skeleton * obj, Entity * io,
                                const Vec3f & pos, float invisibility) {
	
	ARX_PROFILE_FUNC();
	
	if(invisibility == 1.f) {
		return;
	}
	
	Entity * use_io = io;
	if(!io && IN_BOOK_DRAW && eobj == entities.player()->obj) {
		use_io = entities.player();
	}
	
	HaloInfo haloInfo;
	if(use_io) {
		#if ARX_PLATFORM == ARX_PLATFORM_VITA
		bool vitaSkipHalo = true; // Halos disabled on Vita — pure eye candy
		if(!vitaSkipHalo) {
		#endif
		if(use_io == entities.player()) {
			pushSlotHalo(haloInfo, EQUIP_SLOT_HELMET,   eobj->fastaccess.sel_head);
			pushSlotHalo(haloInfo, EQUIP_SLOT_ARMOR,    eobj->fastaccess.sel_chest);
			pushSlotHalo(haloInfo, EQUIP_SLOT_LEGGINGS, eobj->fastaccess.sel_leggings);
		}

		if(use_io->halo.flags & HALO_ACTIVE) {
			haloInfo.push(HaloRenderInfo(&use_io->halo));
		}

		if(haloInfo.size > 0) {
			PrepareAnimatedObjectHalo(haloInfo, pos, obj, eobj);
		}
		#if ARX_PLATFORM == ARX_PLATFORM_VITA
		}
		#endif
	}

	bool glow = false;
	ColorRGBA glowColor;
	if(io && (io->sfx_flag & SFX_TYPE_YLSIDE_DEATH) && io->show != SHOW_FLAG_TELEPORTING) {
		const GameDuration elapsed = g_gameTime.now() - io->sfx_time;
		if(elapsed >= 3s && elapsed < 6s) {
			float ratio = (elapsed - 3s) / 3s;
			glowColor = Color::gray(ratio).toRGB();
			glow = true;
		}
	}
	
	#if ARX_PLATFORM == ARX_PLATFORM_VITA
	// Pre-reserve batch vectors to avoid per-face reallocation
	for(MaterialId mat : eobj->materials.handles()) {
		if(TextureContainer * pTex = eobj->materials[mat]) {
			size_t estimate = eobj->facelist.size() * 3;
			pTex->m_modelBatch[BatchBucket_Opaque].reserve(
				pTex->m_modelBatch[BatchBucket_Opaque].size() + estimate);
		}
	}
	#endif

	#if ARX_PLATFORM == ARX_PLATFORM_VITA
	const VitaVertexRender * vitaCache = s_vitaVertexCache.data();
	const EERIE_FACE * faceData = eobj->facelist.data();
	const size_t faceCount = eobj->facelist.size();
	#endif

	for(size_t i = 0; i < eobj->facelist.size(); i++) {
		const EERIE_FACE & face = eobj->facelist[i];

		#if ARX_PLATFORM == ARX_PLATFORM_VITA
		// Prefetch next face's vertex data from interleaved cache
		if(i + 2 < faceCount) {
			__builtin_prefetch(&faceData[i + 2], 0, 1);
			__builtin_prefetch(&vitaCache[faceData[i + 2].vid[0].handleData()], 0, 1);
		}
		#endif

		if((face.facetype & POLY_HIDE) && !IN_BOOK_DRAW) {
			continue;
		}

		if(CullFace(eobj, face) || !face.material) {
			continue;
		}

		TextureContainer * pTex = eobj->materials[face.material];
		if(!pTex) {
			continue;
		}

		#if ARX_PLATFORM == ARX_PLATFORM_VITA
		vitaTrackActiveTexture(pTex);
		#endif

		float fTransp = 0.f;

		TexturedVertex * tvList = GetNewVertexList(pTex->m_modelBatch, face, invisibility, fTransp);

		for(size_t n = 0; n < 3; n++) {
			#if ARX_PLATFORM == ARX_PLATFORM_VITA
			const VitaVertexRender & vr = vitaCache[face.vid[n].handleData()];
			tvList[n].p = Vec3f(vr.clipPos);
			tvList[n].w = vr.clipPos.w;
			tvList[n].color = vr.color;
			#else
			tvList[n].p = Vec3f(eobj->vertexClipPositions[face.vid[n]]);
			tvList[n].w = eobj->vertexClipPositions[face.vid[n]].w;
			tvList[n].color = eobj->vertexColors[face.vid[n]];
			#endif
			tvList[n].uv = Vec2f(face.u[n], face.v[n]);
		}

		if((face.facetype & POLY_TRANS) || invisibility > 0.f) {
			tvList[0].color = tvList[1].color = tvList[2].color = Color::gray(fTransp).toRGB();
		}

		if(haloInfo.size) {
			AddAnimatedObjectHalo(haloInfo, face.vid, invisibility, eobj, io, tvList);
		}

		if(glow) {
			#if ARX_PLATFORM == ARX_PLATFORM_VITA
			vitaTrackActiveTexture(&TexSpecialColor);
			#endif
			TexturedVertex * tv2 = PushVertexInTable(TexSpecialColor.m_modelBatch[BatchBucket_Opaque]);
			std::copy(tvList, tvList + 3, tv2);
			tv2[0].color = tv2[1].color = tv2[2].color = glowColor;
		}

	}
}

#if ARX_PLATFORM == ARX_PLATFORM_VITA

static unsigned int s_lightPrecomputeFrame = 1;

struct VitaEntityLightWork {
	EERIE_3DOBJ * eobj;
	Skeleton * obj;
	Vec3f lightPos;
	ColorMod colorMod;
	float distToCamera2;
};

static const size_t VITA_MAX_LIGHT_BATCH = 256;
static VitaEntityLightWork s_lightWorkItems[VITA_MAX_LIGHT_BATCH];
static size_t s_lightWorkCount = 0;

static float s_fogEndDist2 = 0.f;
static float s_fogMidDist2 = 0.f;

static void vitaLightWorkFunc(void * /*ctx*/, size_t idx) {
	VitaEntityLightWork & work = s_lightWorkItems[idx];

	if(work.distToCamera2 > s_fogEndDist2) {
		ColorRGBA ambient = work.colorMod.ambientColor.toRGB();
		std::fill(work.eobj->vertexColors.begin(), work.eobj->vertexColors.end(), ambient);
	} else {
		ShaderLight lights[llightsSize];
		size_t lightsCount = 0;
		UpdateLlights(lights, lightsCount, work.lightPos, false);
		if(work.distToCamera2 > s_fogMidDist2 && lightsCount > 1) {
			lightsCount = 1;
		}
		Cedric_ApplyLighting(lights, lightsCount, work.eobj, work.obj, work.colorMod);
	}

	work.eobj->m_lightingFrame = s_lightPrecomputeFrame;
}

void vitaPrecomputeEntityLighting() {

	s_fogEndDist2 = square(g_camera->cdepth * fZFogEnd);
	s_fogMidDist2 = square(g_camera->cdepth * 0.4f);
	s_lightPrecomputeFrame++;
	s_lightWorkCount = 0;

	for(Entity & entity : entities.inScene()) {
		if(!(entity.gameFlags & GFLAG_ISINTREATZONE)
		   || (entity.ioflags & (IO_CAMERA | IO_MARKER))
		   || entity == *entities.player()
		   || !entity.animlayer[0].cur_anim
		   || !entity.obj || !entity.obj->m_skeleton) {
			continue;
		}

		if(s_lightWorkCount >= VITA_MAX_LIGHT_BATCH) {
			break;
		}

		Vec3f pos = entity.pos;
		if(entity.ioflags & IO_NPC) {
			pos.y = entity._npcdata->vvpos;
		}

		Vec3f lightPos = pos;
		if(entity.obj->fastaccess.head_group_origin) {
			lightPos.y = entity.obj->vertexWorldPositions[entity.obj->fastaccess.head_group_origin].v.y + 10;
		} else {
			lightPos.y -= 90.f;
		}

		VitaEntityLightWork & work = s_lightWorkItems[s_lightWorkCount++];
		work.eobj = entity.obj;
		work.obj = entity.obj->m_skeleton.get();
		work.lightPos = lightPos;
		work.colorMod.updateFromEntity(&entity);
		work.distToCamera2 = arx::distance2(lightPos, g_camera->m_pos);
	}

	platform::vita::parallelFor(vitaLightWorkFunc, nullptr, s_lightWorkCount);
}

#endif // ARX_PLATFORM == ARX_PLATFORM_VITA

static void Cedric_AnimateDrawEntityRender(EERIE_3DOBJ * eobj, const Vec3f & pos,
                                           Entity * io, float invisibility) {

	Skeleton * obj = eobj->m_skeleton.get();
	if(!obj) {
		return;
	}

	ColorMod colorMod;
	colorMod.updateFromEntity(io);

	/* Get nearest lights */
	Vec3f tv = pos;

	if(io && io->obj->fastaccess.head_group_origin) {
		tv.y = io->obj->vertexWorldPositions[io->obj->fastaccess.head_group_origin].v.y + 10;
	} else {
		tv.y -= 90.f;
	}

	ShaderLight lights[llightsSize];
	size_t lightsCount = 0;

	#if ARX_PLATFORM == ARX_PLATFORM_VITA
	if(eobj->m_lightingFrame == s_lightPrecomputeFrame) {
		// Lighting already pre-computed in parallel — skip
	} else {
		// Fallback for entities not in the pre-compute batch (linked objects, etc.)
		float distToCamera = arx::distance2(tv, g_camera->m_pos);
		float fogEnd2 = square(g_camera->cdepth * fZFogEnd);
		float fogMid2 = square(g_camera->cdepth * 0.4f);
		if(distToCamera > fogEnd2) {
			ColorRGBA vitaAmbient = colorMod.ambientColor.toRGB();
			std::fill(eobj->vertexColors.begin(), eobj->vertexColors.end(), vitaAmbient);
		} else {
			UpdateLlights(lights, lightsCount, tv, false);
			if(distToCamera > fogMid2 && lightsCount > 1) {
				lightsCount = 1;
			}
			Cedric_ApplyLighting(lights, lightsCount, eobj, obj, colorMod);
		}
	}
	#else
		UpdateLlights(lights, lightsCount, tv, false);
		Cedric_ApplyLighting(lights, lightsCount, eobj, obj, colorMod);
	#endif

	#if ARX_PLATFORM == ARX_PLATFORM_VITA
	vitaBuildVertexCache(eobj);
	#endif

	Cedric_RenderObject(eobj, obj, io, pos, invisibility);

	// Now we can render Linked Objects
	for(const EERIE_LINKED & link : eobj->linked) {

		if(!link.lgroup || !link.obj) {
			continue;
		}

		// specific check to avoid drawing player weapon on its back when in subjective view
		if(!EXTERNALVIEW && io == entities.player() &&
		   link.lidx == entities.player()->obj->fastaccess.weapon_attach) {
			continue;
		}

		TransformInfo t(eobj->vertexWorldPositions[link.lidx].v,
		                eobj->m_skeleton->bones[link.lgroup].anim.quat,
		                link.io ? link.io->scale : 1.f);
		t.pos = t(link.obj->vertexlist[link.obj->origin].v - link.obj->vertexlist[link.lidx2].v);

		DrawEERIEInter(link.obj, t, link.io, true, invisibility);

	}
	
}

static Vec3f CalcTranslation(AnimLayer & layer) {
	
	if(!layer.cur_anim) {
		return Vec3f(0.f);
	}
	
	const EERIE_ANIM * eanim = layer.currentAltAnim();
	if(!eanim) {
		return Vec3f(0.f);
	}
	
	// Avoiding impossible cases
	if(layer.currentFrame < 0) {
		layer.currentFrame = 0;
		layer.currentInterpolation = 0.f;
	} else if(layer.currentFrame >= long(eanim->frames.size()) - 1) {
		layer.currentFrame = long(eanim->frames.size()) - 2;
		layer.currentInterpolation = 1.f;
	}
	layer.currentInterpolation = glm::clamp(layer.currentInterpolation, 0.f, 1.f);
	
	// FIXME animation indices prevent invalid memory access, should be fixed properly
	if(layer.currentFrame < 0 || layer.currentFrame + 1 >= long(eanim->frames.size())) {
		return Vec3f(0.f);
	}
	
	// FRAME TRANSLATE : Gives the Virtual pos of Main Object
	if(eanim->frames[layer.currentFrame].f_translate && !(layer.flags & EA_STATICANIM)) {
		
		const EERIE_FRAME & sFrame = eanim->frames[layer.currentFrame];
		const EERIE_FRAME & eFrame = eanim->frames[layer.currentFrame + 1];
		// Linear interpolation of object translation (MOVE)
		return sFrame.translate + (eFrame.translate - sFrame.translate) * layer.currentInterpolation;
	}
	
	return Vec3f(0.f);
}

static void StoreEntityMovement(Entity * io, Vec3f & ftr, float scale) {

	if(!io)
		return;
	
	arx_assert(isallfinite(ftr));
	
	ftr *= scale;

	float temp;
	if(io == entities.player()) {
		temp = MAKEANGLE(180.f - player.angle.getYaw());
	} else {
		temp = MAKEANGLE(180.f - io->angle.getYaw());
	}

	Vec3f ftr2 = VRotateY(ftr, temp);

	// stores Translations for a later use
	io->move = ftr2;

	if(io->animlayer[0].cur_anim) {

		// Use calculated value to notify the Movement engine of the translation to do
		if(io->ioflags & IO_NPC) {
			ftr = Vec3f(0.f);
			io->move -= io->lastmove;
		} else if(io->gameFlags & GFLAG_ELEVATOR) {
			// Must recover translations for NON-NPC IO
			PushIO_ON_Top(*io, io->move.y - io->lastmove.y);
		}

		io->lastmove = ftr2;
	}
}

/*!
 * Animate skeleton
 */
static void Cedric_AnimateObject(Skeleton * obj, AnimLayer * animlayer) {
	
	unsigned char grps[256] = {};
	arx_assert(obj->bones.size() <= sizeof(grps));

	for(long count = MAX_ANIM_LAYERS - 1; count >= 0; count--) {
		
		AnimLayer & layer = animlayer[count];
		if(!layer.cur_anim) {
			continue;
		}
		
		const EERIE_ANIM * eanim = layer.currentAltAnim();
		if(!eanim) {
			continue;
		}
		
		if(layer.currentFrame < 0) {
			layer.currentFrame = 0;
			layer.currentInterpolation = 0.f;
		} else if(layer.currentFrame >= long(eanim->frames.size()) - 1) {
			layer.currentFrame = long(eanim->frames.size()) - 2;
			layer.currentInterpolation = 1.f;
		}
		layer.currentInterpolation = glm::clamp(layer.currentInterpolation, 0.f, 1.f);
		
		// FIXME animation indices are sometimes negative
		layer.currentFrame = glm::clamp(layer.currentFrame, 0l, long(eanim->frames.size()) - 1l);
		
		// Now go for groups rotation/translation/scaling, And transform Linked objects by the way
		size_t l = std::min(obj->bones.size(), eanim->nb_groups());
		
		for(size_t j = 0; j < l; j++) {
			
			if(grps[j]) {
				continue;
			}
			
			if(!eanim->voidgroups[j]) {
				grps[j] = 1;
			}
			
			if(eanim->frames.size() != 1) {
				const EERIE_GROUP & sGroup = eanim->groups[j + size_t(layer.currentFrame) * eanim->nb_groups()];
				const EERIE_GROUP & eGroup = eanim->groups[j + size_t(layer.currentFrame + 1) * eanim->nb_groups()];
				BoneTransform temp;
				#if ARX_PLATFORM == ARX_PLATFORM_VITA
				temp.quat = Quat_Nlerp(sGroup.quat, eGroup.quat, layer.currentInterpolation);
				#else
				temp.quat = Quat_Slerp(sGroup.quat, eGroup.quat, layer.currentInterpolation);
				#endif
				temp.trans = glm::mix(sGroup.translate, eGroup.translate, layer.currentInterpolation);
				temp.scale = glm::mix(sGroup.zoom, eGroup.zoom, layer.currentInterpolation);
				Bone & bone = obj->bones[VertexGroupId(j)];
				bone.init.quat = bone.init.quat * temp.quat;
				bone.init.trans = temp.trans + bone.transinit_global;
				bone.init.scale = temp.scale;
			}
			
		}
		
	}
	
}

static void Cedric_BlendAnimation(Skeleton & rig, AnimationBlendStatus * animBlend) {

	if(!animBlend->m_active) {
		return;
	}
	
	float timm = toMsf(g_gameTime.now() - animBlend->lastanimtime) + 0.0001f;
	if(timm >= 300.f) {
		animBlend->m_active = false;
		return;
	}
	
	timm *= 1.0f / 300;
	if(timm < 0.f || timm >= 1.f) {
		return;
	}
	
	for(Bone & bone : rig.bones) {
		#if ARX_PLATFORM == ARX_PLATFORM_VITA
		// NLERP: ~3-4x faster than SLERP (avoids acos + 3x sin), visually identical
		// for animation blending where angle changes are smooth over 300ms.
		{
			glm::quat from = bone.last.quat;
			glm::quat to = bone.init.quat;
			if(glm::dot(from, to) < 0.f) {
				to = -to;
			}
			float fBeta = 1.f - timm;
			bone.init.quat = glm::normalize(glm::quat(
				fBeta * from.w + timm * to.w,
				fBeta * from.x + timm * to.x,
				fBeta * from.y + timm * to.y,
				fBeta * from.z + timm * to.z
			));
		}
		#else
		bone.init.quat = Quat_Slerp(bone.last.quat, bone.init.quat, timm);
		#endif
		bone.init.trans = bone.last.trans + (bone.init.trans - bone.last.trans) * timm;
		bone.init.scale = bone.last.scale + (bone.init.scale - bone.last.scale) * timm;
	}
	
}

/*!
 * Apply transformations on all bones
 */
static void Cedric_ConcatenateTM(Skeleton & rig, const TransformInfo & t) {
	
	for(Bone & bone : rig.bones) {
		if(bone.father) { // Child bones
			
			const Bone & parent = rig.bones[bone.father];
			// Rotation
			bone.anim.quat = parent.anim.quat * bone.init.quat;
			
			// Translation
			bone.anim.trans = bone.init.trans * parent.anim.scale;
			bone.anim.trans = parent.anim.quat * bone.anim.trans;
			bone.anim.trans = parent.anim.trans + bone.anim.trans;
			
			// Scale
			bone.anim.scale = (bone.init.scale + Vec3f(1.f)) * parent.anim.scale;
			
		} else { // Root bone
			
			// Rotation
			bone.anim.quat = t.rotation * bone.init.quat;
			
			// Translation
			bone.anim.trans = t(bone.init.trans);
			
			// Compute Global Object Scale AND Global Animation Scale
			bone.anim.scale = (bone.init.scale + Vec3f(1.f)) * t.scale;
			
		}
	}
	
}

/*!
 * Transform object vertices
 */
static void Cedric_TransformVerts(EERIE_3DOBJ * eobj) {

	const Skeleton & rig = *eobj->m_skeleton;

	arx_assume(eobj->vertexWorldPositions.size() == eobj->vertexlocal.size());
	arx_assume(eobj->m_boneVertices.size() == rig.bones.size());

	// Transform & project all vertices
	for(VertexGroupId group : rig.bones.handles()) {
		const Bone & bone = rig.bones[group];

		#if ARX_PLATFORM == ARX_PLATFORM_VITA
		// Use 3x3 rotation+scale matrix directly (avoids 4x4 multiply overhead)
		glm::mat3 matrix = glm::mat3_cast(bone.anim.quat);
		matrix[0] *= bone.anim.scale.x;
		matrix[1] *= bone.anim.scale.y;
		matrix[2] *= bone.anim.scale.z;
		Vec3f vector = bone.anim.trans;

		for(VertexId vertex : eobj->m_boneVertices[group]) {
			eobj->vertexWorldPositions[vertex].v = matrix * eobj->vertexlocal[vertex] + vector;
		}
		#else
		glm::mat4x4 matrix = glm::mat4_cast(bone.anim.quat);

		// Apply Scale
		matrix[0][0] *= bone.anim.scale.x;
		matrix[0][1] *= bone.anim.scale.x;
		matrix[0][2] *= bone.anim.scale.x;

		matrix[1][0] *= bone.anim.scale.y;
		matrix[1][1] *= bone.anim.scale.y;
		matrix[1][2] *= bone.anim.scale.y;

		matrix[2][0] *= bone.anim.scale.z;
		matrix[2][1] *= bone.anim.scale.z;
		matrix[2][2] *= bone.anim.scale.z;

		Vec3f vector = bone.anim.trans;

		for(VertexId vertex : eobj->m_boneVertices[group]) {
			eobj->vertexWorldPositions[vertex].v = Vec3f(matrix * Vec4f(eobj->vertexlocal[vertex], 1.f)) + vector;
		}
		#endif

	}

}

static void animateSkeleton(EERIE_3DOBJ * eobj, AnimLayer * animlayer,
                             const Anglef & angle, const Vec3f & pos, float scale, Vec3f ftr,
                             Entity * io, Skeleton & skeleton, AnimationBlendStatus * animBlend) {
	
	glm::quat rotation;
	bool isNpc = io && (io->ioflags & IO_NPC);
	if(!isNpc) {
		// To correct invalid angle in Animated FIX/ITEMS
		rotation = toQuaternion(angle);
	} else {
		rotation = QuatFromAngles(angle);
	}
	
	EERIE_EXTRA_ROTATE * extraRotation = nullptr;
	
	if(io && (io->ioflags & IO_NPC) && io->_npcdata->ex_rotate) {
		extraRotation = io->_npcdata->ex_rotate;
	}
	
	// Initialize the rig
	for(Bone & bone : skeleton.bones) {
		bone.init.quat = quat_identity();
		bone.init.trans = bone.transinit_global;
	}
	
	// Apply Extra Rotations in Local Space
	if(extraRotation) {
		for(size_t k = 0; k < MAX_EXTRA_ROTATE; k++) {
			if(VertexGroupId group = extraRotation->group_number[k]) {
				skeleton.bones[group].init.quat = angleToQuatForExtraRotation(extraRotation->group_rotate[k]);
			}
		}
	}
	
	// Perform animation in Local space
	Cedric_AnimateObject(&skeleton, animlayer);
	
	if(BH_MODE && eobj->fastaccess.head_group) {
		Bone & bone = skeleton.bones[eobj->fastaccess.head_group];
		bone.init.scale += Vec3f(1.f);
	}
	
	// Check for Animation Blending in Local space
	if(io) {
		// Is There any Between-Animations Interpolation to make ?
		Cedric_BlendAnimation(skeleton, animBlend);
		for(Bone & bone : skeleton.bones) {
			bone.last = bone.init;
		}
	}
	
	// Build skeleton in Object Space
	TransformInfo t(pos, rotation, scale);
	t.pos = t(ftr);
	Cedric_ConcatenateTM(skeleton, t);
	
}

void animateSkeleton(Entity * entity, AnimLayer * animlayer, Skeleton & skeleton) {
	
	Vec3f ftr = Vec3f(0.f);
	
	AnimationBlendStatus animBlend = entity->animBlend;
	
	animateSkeleton(entity->obj, animlayer, entity->angle, entity->pos, entity->scale, ftr, entity, skeleton, &animBlend);
}

#if ARX_PLATFORM == ARX_PLATFORM_VITA

struct VitaTransformWork {
	EERIE_3DOBJ * eobj;
	AnimLayer * animlayer;
	Anglef angle;
	Vec3f pos;
	float scale;
	Vec3f ftr;
	Entity * io;
};

static std::vector<VitaTransformWork> s_transformBatch;
static bool s_deferTransforms = false;

static void vitaTransformFunc(void * /*ctx*/, size_t idx) {
	VitaTransformWork & w = s_transformBatch[idx];
	animateSkeleton(w.eobj, w.animlayer, w.angle, w.pos, w.scale, w.ftr,
	                w.io, *w.eobj->m_skeleton, w.io ? &w.io->animBlend : nullptr);
	Cedric_TransformVerts(w.eobj);
	if(w.io) {
		w.io->bbox3D = UpdateBbox3d(w.eobj);
	}
	DrawEERIEInter_ViewProjectTransform(w.eobj);
	if(w.io) {
		w.io->bbox2D = UpdateBbox2d(*w.eobj);
	}
}

void vitaBeginTransformBatch() {
	s_transformBatch.clear();
	if(s_transformBatch.capacity() == 0) {
		s_transformBatch.reserve(64);
	}
	s_deferTransforms = true;
}

void vitaFlushTransformBatch() {
	s_deferTransforms = false;
	platform::vita::parallelFor(vitaTransformFunc, nullptr, s_transformBatch.size());
}

#endif

void EERIEDrawAnimQuatUpdate(EERIE_3DOBJ * eobj,
                             AnimLayer * animlayer,
                             const Anglef & angle,
                             const Vec3f & pos,
                             AnimationDuration time,
                             Entity * io,
                             bool update_movement
) {

	ARX_PROFILE_FUNC();
	
	if(io) {
		float speedfactor = io->basespeed + io->speed_modif;

		if(speedfactor < 0)
			speedfactor = 0;

		AnimationDuration tim = time * speedfactor;

		if(tim <= 0)
			time = 0;
		else
			time = tim;
	}

	if(time > 0) {
		for(size_t count = 0; count < MAX_ANIM_LAYERS; count++) {
			AnimLayer & layer = animlayer[count];
			if(layer.cur_anim)
				PrepareAnim(layer, time, io);
		}
	}
	
	// Set scale and invisibility factors
	// Scaling Value for this object (Movements will also be scaled)
	float scale = (io) ? io->scale : 1.f;
	
	// Only layer 0 controls movement
	Vec3f ftr = CalcTranslation(animlayer[0]);
	
	if(update_movement) {
		StoreEntityMovement(io, ftr, scale);
	}
	
	if(io && io != entities.player() && !Cedric_IO_Visible(io->pos)) {
		return;
	}

	#if ARX_PLATFORM == ARX_PLATFORM_VITA
	// Frame-skip bone transforms for distant entities — reuse previous vertex positions.
	// Animation state and movement are still advanced above; only the expensive
	// skeleton+vertex transform is skipped on non-update frames.
	if(io && io != entities.player()) {
		float dist2 = arx::distance2(pos, g_camera->m_pos);
		static unsigned int s_animFrameCount = 0;
		s_animFrameCount++;
		unsigned int entityIdx = unsigned(io->index().handleData());
		float animFogEnd2 = square(g_camera->cdepth * fZFogEnd);
		float animFogMid2 = square(g_camera->cdepth * 0.4f);
		if(dist2 > animFogEnd2) {
			// Beyond fog end: update transforms every 4th frame
			if(((s_animFrameCount + entityIdx) & 3u) != 0) {
				DrawEERIEInter_ViewProjectTransform(eobj);
				if(io) {
					io->bbox2D = UpdateBbox2d(*eobj);
				}
				return;
			}
		} else if(dist2 > animFogMid2) {
			// Mid-fog: update transforms every other frame
			if((s_animFrameCount + entityIdx) & 1u) {
				DrawEERIEInter_ViewProjectTransform(eobj);
				if(io) {
					io->bbox2D = UpdateBbox2d(*eobj);
				}
				return;
			}
		}
	}
	#endif

	arx_assert(eobj->m_skeleton);

	#if ARX_PLATFORM == ARX_PLATFORM_VITA
	if(s_deferTransforms) {
		s_transformBatch.push_back({eobj, animlayer, angle, pos, scale, ftr, io});
		return;
	}
	#endif

	animateSkeleton(eobj, animlayer, angle, pos, scale, ftr, io, *eobj->m_skeleton, io ? &io->animBlend : nullptr);

	Cedric_TransformVerts(eobj);
	if(io) {
		io->bbox3D = UpdateBbox3d(eobj);
	}

	DrawEERIEInter_ViewProjectTransform(eobj);
	if(io) {
		io->bbox2D = UpdateBbox2d(*eobj);
	}
}

void EERIEDrawAnimQuatRender(EERIE_3DOBJ * eobj, const Vec3f & pos, Entity * io, float invisibility) {
	
	ARX_PROFILE_FUNC();
	
	if(io && io != entities.player() && !Cedric_IO_Visible(io->pos))
		return;

	// Skip portal clipping for book/menu rendering — uses its own camera,
	// and stale room data from a previous level would incorrectly clip the model.
	if(!IN_BOOK_DRAW) {
		bool isFightingNpc = io && (io->ioflags & IO_NPC) && (io->_npcdata->behavior & BEHAVIOUR_FIGHT)
		                     && closerThan(io->pos, player.pos, 240.f);
		if(!isFightingNpc && ARX_SCENE_PORTAL_ClipIO(io, pos)) {
			return;
		}
	}
	
	Cedric_AnimateDrawEntityRender(eobj, pos, io, invisibility);
}

void AnimatedEntityRender(Entity * entity, float invisibility) {
	
	DrawEERIEInter_ViewProjectTransform(entity->obj);
	entity->bbox2D = UpdateBbox2d(*entity->obj);
	
	EERIEDrawAnimQuatRender(entity->obj, entity->pos, entity, invisibility);
}
