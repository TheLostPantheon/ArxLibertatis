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

#ifndef ARX_SCENE_VITAENTITYRENDERER_H
#define ARX_SCENE_VITAENTITYRENDERER_H

#include "platform/Platform.h"

#if ARX_PLATFORM == ARX_PLATFORM_VITA

#include "graphics/GraphicsTypes.h"
#include "math/Types.h"

struct TransformInfo;
class Entity;

/*!
 * Entity models on the GPU.
 *
 * The stock path lights every vertex of an entity on the CPU each frame, then
 * copies three vertices per visible face into per-texture lists that are drawn
 * from client memory. Here each model is built once into a static vertex and
 * index buffer, and a vertex shader skins it with the bone transforms the CPU
 * already computes (game code still needs the world positions) and lights it
 * with the same lights and falloff as ApplyLight.
 *
 * Entities the GPU path doesn't cover (transparent or water faces, invisibility,
 * infravision, the book, too many bones) keep the stock path. Disabled when
 * ux0:data/arx/noentitygpu.txt exists (for A/B runs) or when the shader doesn't
 * compile.
 */
namespace vitaentity {

//! Compile the shader while the level loads rather than at the first entity drawn.
void prepare();

//! Start of the entity pass (RenderInter): per-frame uniforms are set again.
void beginFrame();

/*!
 * The player renders between these two. Its draws are held back and issued at endPlayer(),
 * in first person (overlay) once without the depth test, so it shows through walls it
 * clips into, then once with it.
 */
void beginPlayer(bool overlay);
void endPlayer();

//! The entity will be drawn by drawSkinned() (so its CPU vertex lighting can be skipped).
bool handlesSkinned(Entity & io);

/*!
 * Draw a skinned entity whose skeleton is up to date, lit by the lights nearest to lightPos.
 * Returns false when the stock path has to draw it.
 */
bool drawSkinned(EERIE_3DOBJ * eobj, Entity * io, const Vec3f & lightPos, float invisibility);

/*!
 * Draw a rigid object (a static entity or an object linked to a bone) with transform t.
 * Returns false when the stock path has to draw it.
 */
bool drawRigid(EERIE_3DOBJ * eobj, const TransformInfo & t, Entity * io, float invisibility);

} // namespace vitaentity

#endif // ARX_PLATFORM == ARX_PLATFORM_VITA

#endif // ARX_SCENE_VITAENTITYRENDERER_H
