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

#ifndef ARX_SCENE_VITAWORLDRENDERER_H
#define ARX_SCENE_VITAWORLDRENDERER_H

#include "platform/Platform.h"

#if ARX_PLATFORM == ARX_PLATFORM_VITA

#include <vector>

#include "math/Types.h"

struct EERIEPOLY;

/*!
 * World geometry on the GPU.
 *
 * The stock room renderer culls and lights every visible polygon on the CPU
 * each frame and rebuilds the index lists. Here each room is built once into
 * a static vertex and index buffer, split into batches by material, blend
 * bucket and an 8x8 tile cell. Per frame the CPU only picks the visible
 * batches and the dynamic lights for each, and a vertex shader adds the
 * dynamic lights to the baked static lighting with the same falloff as
 * ApplyTileLights.
 *
 * Infravision keeps the stock path. Disabled when ux0:data/arx/noworldgpu.txt
 * exists (for A/B runs) or when the shaders don't compile.
 */
namespace vitaworld {

//! Build the GPU data for the loaded level (end of ComputePortalVertexBuffer).
void build();

void release();

//! Changes whenever the level geometry is built or released: EERIEPOLY pointers from before are stale.
unsigned generation();

//! The GPU path can draw the world this frame.
bool active();

//! Visible rooms: activate their tiles, animate water and lava, collect the FX polygons.
void update(const Vec3f & camPos, std::vector<EERIEPOLY *> & water, std::vector<EERIEPOLY *> & lava);

void renderOpaque();

void renderTransparent();

} // namespace vitaworld

#endif // ARX_PLATFORM == ARX_PLATFORM_VITA

#endif // ARX_SCENE_VITAWORLDRENDERER_H
