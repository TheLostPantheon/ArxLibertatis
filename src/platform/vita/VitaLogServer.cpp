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

#include "platform/vita/VitaLogServer.h"

#if ARX_PLATFORM == ARX_PLATFORM_VITA

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <sstream>
#include <string>

#include <psp2/io/stat.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/net/net.h>
#include <psp2/sysmodule.h>

#include "io/log/LogBackend.h"
#include "io/log/Logger.h"
#include "platform/vita/VitaBench.h"

namespace platform {
namespace vita {

namespace {

const unsigned short Port = 1340;
const int NetMemSize = 128 * 1024;
const SceUInt PollUs = 100 * 1000;
const int HeartbeatPolls = 20; // ~2 s
const size_t MaxBuffered = 4 * 1024 * 1024;

//! Everything logged since startup; the oldest half goes when it gets too big.
std::mutex g_mutex;
std::string g_buffer;
size_t g_bufferBase = 0; // absolute offset of g_buffer[0]

class StreamBackend final : public logger::Backend {

public:

	void log(const logger::Source & file, int line, Logger::LogLevel level, std::string_view str) override {
		std::ostringstream oss;
		format(oss, file, line, level, str);
		std::string text = oss.str();
		std::scoped_lock lock(g_mutex);
		if(g_buffer.size() + text.size() > MaxBuffered) {
			size_t drop = g_buffer.size() / 2;
			g_buffer.erase(0, drop);
			g_bufferBase += drop;
		}
		g_buffer += text;
	}

};

bool sendAll(int s, const char * data, size_t len) {
	while(len > 0) {
		int n = sceNetSend(s, data, unsigned(len), 0);
		if(n <= 0) {
			return false;
		}
		data += n;
		len -= size_t(n);
	}
	return true;
}

bool sendHeartbeat(int s) {
	char line[160];
	int n = std::snprintf(line, sizeof(line), "#hb t=%llu main=%s main_age_ms=%llu\n",
	                      static_cast<unsigned long long>(perfNowUs() / 1000), perfMainPhase(),
	                      static_cast<unsigned long long>(perfMainAgeUs() / 1000));
	return n > 0 && sendAll(s, line, size_t(n));
}

void serveClient(int s) {

	size_t sent;
	{
		std::scoped_lock lock(g_mutex);
		sent = g_bufferBase;
	}

	std::string chunk;
	int polls = 0;
	for(;;) {
		{
			std::scoped_lock lock(g_mutex);
			if(sent < g_bufferBase) {
				sent = g_bufferBase; // dropped while we were behind
			}
			chunk.assign(g_buffer, sent - g_bufferBase, std::string::npos);
		}
		if(!chunk.empty()) {
			if(!sendAll(s, chunk.data(), chunk.size())) {
				return;
			}
			sent += chunk.size();
			continue;
		}
		if(++polls >= HeartbeatPolls) {
			polls = 0;
			if(!sendHeartbeat(s)) {
				return;
			}
		}
		sceKernelDelayThread(PollUs);
	}
}

int serverThread(SceSize /* args */, void * /* argp */) {

	int srv = sceNetSocket("arx_logserver", SCE_NET_AF_INET, SCE_NET_SOCK_STREAM, 0);
	if(srv < 0) {
		LogError << "[LogServer] socket failed 0x" << std::hex << unsigned(srv);
		return 0;
	}
	int on = 1;
	sceNetSetsockopt(srv, SCE_NET_SOL_SOCKET, SCE_NET_SO_REUSEADDR, &on, sizeof(on));
	SceNetSockaddrIn addr;
	std::memset(&addr, 0, sizeof(addr));
	addr.sin_len = sizeof(addr);
	addr.sin_family = SCE_NET_AF_INET;
	addr.sin_port = sceNetHtons(Port);
	addr.sin_addr.s_addr = SCE_NET_INADDR_ANY;
	int r = sceNetBind(srv, reinterpret_cast<SceNetSockaddr *>(&addr), sizeof(addr));
	if(r >= 0) {
		r = sceNetListen(srv, 1);
	}
	if(r < 0) {
		LogError << "[LogServer] bind/listen failed 0x" << std::hex << unsigned(r);
		sceNetSocketClose(srv);
		return 0;
	}
	LogInfo << "[LogServer] listening on port " << Port << " (logserver.txt)";

	for(;;) {
		int c = sceNetAccept(srv, nullptr, nullptr);
		if(c < 0) {
			sceKernelDelayThread(1000 * 1000);
			continue;
		}
		// A blocked send must not pin the thread when the host goes away
		int timeoutUs = 5 * 1000 * 1000;
		sceNetSetsockopt(c, SCE_NET_SOL_SOCKET, SCE_NET_SO_SNDTIMEO, &timeoutUs, sizeof(timeoutUs));
		serveClient(c);
		sceNetSocketClose(c);
	}

	return 0;
}

} // anonymous namespace

void startLogServerIfEnabled() {

	SceIoStat st;
	if(sceIoGetstat("ux0:data/arx/logserver.txt", &st) < 0) {
		return;
	}

	// Before anything else is logged, so the stream starts at the beginning
	Logger::add(new StreamBackend);

	int r = sceSysmoduleLoadModule(SCE_SYSMODULE_NET);
	if(r < 0) {
		LogError << "[LogServer] net module load failed 0x" << std::hex << unsigned(r);
		return;
	}
	static void * s_netMemory = std::malloc(NetMemSize);
	SceNetInitParam param;
	param.memory = s_netMemory;
	param.size = NetMemSize;
	param.flags = 0;
	r = sceNetInit(&param);
	if(r < 0 && unsigned(r) != unsigned(SCE_NET_ERROR_EBUSY)) {
		LogError << "[LogServer] sceNetInit failed 0x" << std::hex << unsigned(r);
		return;
	}

	// Lowest priority: it only runs in idle time and never competes with the frame
	SceUID thread = sceKernelCreateThread("arx_logserver", serverThread, 191, 16 * 1024, 0, 0, nullptr);
	if(thread >= 0) {
		sceKernelStartThread(thread, 0, nullptr);
	}
}

} // namespace vita
} // namespace platform

#endif // ARX_PLATFORM == ARX_PLATFORM_VITA
