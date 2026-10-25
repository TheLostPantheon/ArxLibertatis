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

#ifndef ARX_GUI_VITAMINIMAP_H
#define ARX_GUI_VITAMINIMAP_H

#include "platform/Platform.h"

#if ARX_PLATFORM == ARX_PLATFORM_VITA

#include "gui/MiniMap.h"
#include "math/Types.h"

class TextureContainer;

/*!
 * The in-game minimap background in one draw.
 *
 * The stock path builds a quad per map tile every frame (about 900 of them), with
 * the tile's revealed level and the border fade per corner. Here a fragment shader
 * computes the same per pixel: the map texture through the same tile to texture
 * mapping, the revealed levels as a small linearly filtered texture (which is what
 * interpolating them across the tiles' corners amounts to) and the fade.
 *
 * Disabled when ux0:data/arx/nominimapgpu.txt exists.
 */
namespace vitaminimap {

//! Compile the shader while the level loads.
void prepare();

/*!
 * Draw the background the way MiniMap::drawBackground does for the given parameters, with the
 * render state and the map texture already set. Returns false when the stock path has to.
 */
bool drawBackground(const unsigned char (&revealed)[MINIMAP_MAX_X][MINIMAP_MAX_Z], const Rect & boundaries,
                    const Rect & fadeBounds, Vec2f start, Vec2f tileSize, Vec2f tileUv,
                    float fadeBorder, float alpha);

} // namespace vitaminimap

#endif // ARX_PLATFORM == ARX_PLATFORM_VITA

#endif // ARX_GUI_VITAMINIMAP_H
