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

#ifndef ARX_PLATFORM_VITA_VITALOGSERVER_H
#define ARX_PLATFORM_VITA_VITALOGSERVER_H

#include "platform/Platform.h"

#if ARX_PLATFORM == ARX_PLATFORM_VITA

namespace platform {
namespace vita {

/*!
 * With ux0:data/arx/logserver.txt present: serve the log on TCP port 1340,
 * everything logged since startup and then new lines as they come, plus a
 * "#hb" heartbeat line every ~2 s naming the main thread's phase and how long
 * ago it last finished a frame. scripts/vita/vita_logtail.py is the client.
 */
void startLogServerIfEnabled();

} // namespace vita
} // namespace platform

#endif // ARX_PLATFORM == ARX_PLATFORM_VITA

#endif // ARX_PLATFORM_VITA_VITALOGSERVER_H
