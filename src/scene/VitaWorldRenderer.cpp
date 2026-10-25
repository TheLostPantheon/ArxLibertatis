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

#include "scene/VitaWorldRenderer.h"

#if ARX_PLATFORM == ARX_PLATFORM_VITA

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <limits>
#include <optional>
#include <string>

#include <glm/gtc/type_ptr.hpp>

#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>

#include "core/GameTime.h"
#include "game/Camera.h"
#include "game/Player.h"
#include "graphics/GlobalFog.h"
#include "graphics/GraphicsTypes.h"
#include "graphics/Renderer.h"
#include "graphics/data/Mesh.h"
#include "graphics/data/TextureContainer.h"
#include "graphics/opengl/GLVertexBuffer.h"
#include "graphics/opengl/OpenGLRenderer.h"
#include "graphics/opengl/OpenGLUtil.h"
#include "graphics/texture/Texture.h"
#include "io/log/Logger.h"
#include "platform/vita/VitaBench.h"
#include "scene/Light.h"
#include "scene/Rooms.h"
#include "scene/Scene.h"
#include "scene/Tiles.h"

extern float WATEREFFECT;

namespace vitaworld {

namespace {

const size_t MaxLights = 12;  // lights in the lit vertex shader
const size_t SmallLights = 4; // and in its variant for batches near few lights

//! A material's polygons in a room are one batch unless they spread over more than
//! SplitTiles tiles; then they are cut into cells of NxN tiles so a batch stays local
//! enough for MaxLights (ux0:data/arx/worldcell.txt, default 16).
const int SplitTiles = 24;
int cellTiles() {
	static const int s_cell = [] {
		int cell = 16;
		if(FILE * f = std::fopen("ux0:data/arx/worldcell.txt", "r")) {
			if(std::fscanf(f, "%d", &cell) != 1 || cell < 1) {
				cell = 16;
			}
			std::fclose(f);
		}
		return cell;
	}();
	return s_cell;
}

//! Per-frame counters, logged every 5 s
struct Stats {
	u32 batches = 0;
	u32 litBatches = 0;
	u32 overflow = 0; // batches with more candidate lights than the shader takes
	u32 maxCandidates = 0;
	u32 merged = 0; // batches drawn with the one before
	u32 frames = 0;
	u64 nextLog = 0;
};
Stats g_stats;

struct Vertex {
	Vec3f pos;
	ColorRGBA color;
	Vec2f uv;
	Vec3f normal;
	float lit; // 0 for polygons the stock path doesn't light (transparent, glowing)
};
static_assert(sizeof(Vertex) == 40, "vertex layout");

struct Batch {
	TextureContainer * material = nullptr;
	BatchBucket bucket = BatchBucket_Opaque;
	u32 firstVertex = 0;
	u32 firstIndex = 0;
	u32 indexCount = 0;
	Vec3f center = Vec3f(0.f);
	float radius = 0.f;
	bool cutout = false;
	bool metal = false;
	bool lit = false;

	Batch() noexcept = default;
};

struct AnimatedPoly {
	EERIEPOLY * ep;
	u32 firstVertex;
};

struct GpuRoom {
	GLuint vbo = 0;
	GLuint ibo = 0;
	std::vector<Batch> batches;
	std::vector<AnimatedPoly> animated;
	std::vector<Vec2s> tiles;
	Vec3f center = Vec3f(0.f);
	float radius = 0.f;
};

std::vector<GpuRoom> g_gpuRooms;
bool g_built = false;
unsigned g_generation = 0;

// ---- shaders

//! The lit vertex shader for a number of lights: batches near few lights use the small one
std::string litVertexShader(size_t lights) {
	std::string source =
		"void main(\n"
		"	float3 aPosition,\n"
		"	float4 aColor,\n"
		"	float2 aTexcoord,\n"
		"	float4 aNormal,\n"
		"	uniform float4x4 uMVP,\n"
		"	uniform float2 uFog,\n"
		"	uniform float4 uLightPos[LIGHTS],\n"
		"	uniform float4 uLightCol[LIGHTS],\n"
		"	float4 out vPosition : POSITION,\n"
		"	float4 out vColor : COLOR,\n"
		"	float2 out vTexcoord : TEXCOORD0,\n"
		"	float out vFog : TEXCOORD1)\n"
		"{\n"
		"	float3 c = aColor.rgb;\n"
		"	for(int i = 0; i < LIGHTS; i++) {\n"
		"		float3 l = uLightPos[i].xyz - aPosition;\n"
		"		float d2 = max(dot(l, l), 0.0001);\n"
		"		float inv = rsqrt(d2);\n"
		"		float ndl = max(dot(aNormal.xyz, l) * inv, 0.0);\n"
		"		float att = saturate((uLightPos[i].w - d2 * inv) * uLightCol[i].w);\n"
		"		c += uLightCol[i].rgb * (ndl * att * aNormal.w);\n"
		"	}\n"
		"	vColor = float4(saturate(c), aColor.a);\n"
		"	vPosition = mul(float4(aPosition, 1.0), uMVP);\n"
		"	vTexcoord = aTexcoord;\n"
		"	vFog = saturate((uFog.x - vPosition.w) * uFog.y);\n"
		"}\n";
	const std::string token = "LIGHTS";
	const std::string count = std::to_string(lights);
	for(size_t at = source.find(token); at != std::string::npos; at = source.find(token, at)) {
		source.replace(at, token.size(), count);
	}
	return source;
}

const char * const UnlitVertexShader =
	"void main(\n"
	"	float3 aPosition,\n"
	"	float4 aColor,\n"
	"	float2 aTexcoord,\n"
	"	uniform float4x4 uMVP,\n"
	"	uniform float2 uFog,\n"
	"	float4 out vPosition : POSITION,\n"
	"	float4 out vColor : COLOR,\n"
	"	float2 out vTexcoord : TEXCOORD0,\n"
	"	float out vFog : TEXCOORD1)\n"
	"{\n"
	"	vColor = aColor;\n"
	"	vPosition = mul(float4(aPosition, 1.0), uMVP);\n"
	"	vTexcoord = aTexcoord;\n"
	"	vFog = saturate((uFog.x - vPosition.w) * uFog.y);\n"
	"}\n";

const char * const OpaqueFragmentShader =
	"float4 main(\n"
	"	float4 vColor : COLOR,\n"
	"	float2 vTexcoord : TEXCOORD0,\n"
	"	float vFog : TEXCOORD1,\n"
	"	uniform sampler2D uTexture : TEXUNIT0,\n"
	"	uniform float4 uFogColor,\n"
	"	uniform float uColorScale) : COLOR\n"
	"{\n"
	"	float4 t = tex2D(uTexture, vTexcoord);\n"
	"	float3 rgb = t.rgb * vColor.rgb * uColorScale;\n"
	"	return float4(lerp(uFogColor.rgb, rgb, vFog), t.a);\n"
	"}\n";

const char * const CutoutFragmentShader =
	"float4 main(\n"
	"	float4 vColor : COLOR,\n"
	"	float2 vTexcoord : TEXCOORD0,\n"
	"	float vFog : TEXCOORD1,\n"
	"	uniform sampler2D uTexture : TEXUNIT0,\n"
	"	uniform float4 uFogColor,\n"
	"	uniform float uColorScale) : COLOR\n"
	"{\n"
	"	float4 t = tex2D(uTexture, vTexcoord);\n"
	"	if(t.a <= 0.5) {\n"
	"		discard;\n"
	"	}\n"
	"	float3 rgb = t.rgb * vColor.rgb * uColorScale;\n"
	"	return float4(lerp(uFogColor.rgb, rgb, vFog), t.a);\n"
	"}\n";

struct Program {
	GLuint id = 0;
	GLint mvp = -1;
	GLint fog = -1;
	GLint lightPos = -1;
	GLint lightCol = -1;
	GLint fogColor = -1;
	GLint colorScale = -1;
	size_t lights = 0; // in the vertex shader
	u64 frame = 0; // last frame the per-frame uniforms were set
};

enum ProgramId {
	LitOpaque,
	LitCutout,
	Lit4Opaque,
	Lit4Cutout,
	UnlitOpaque,
	UnlitCutout,
	ProgramCount
};

std::array<Program, ProgramCount> g_programs;
bool g_shadersTried = false;
bool g_shadersOk = false;
u64 g_frame = 0;

GLuint compile(GLenum type, const char * source, const char * name) {
	GLuint shader = glCreateShader(type);
	glShaderSource(shader, 1, &source, nullptr);
	glCompileShader(shader);
	GLint ok = GL_FALSE;
	glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
	if(!ok) {
		char log[512] = "";
		glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
		LogError << "[VitaWorld] " << name << " shader failed to compile: " << log;
		glDeleteShader(shader);
		return 0;
	}
	return shader;
}

bool link(Program & program, GLuint vs, GLuint fs, size_t lights) {
	const bool lit = (lights > 0);
	program.lights = lights;
	program.id = glCreateProgram();
	glAttachShader(program.id, vs);
	glAttachShader(program.id, fs);
	glBindAttribLocation(program.id, 0, "aPosition");
	glBindAttribLocation(program.id, 1, "aColor");
	glBindAttribLocation(program.id, 2, "aTexcoord");
	if(lit) {
		glBindAttribLocation(program.id, 3, "aNormal");
	}
	glLinkProgram(program.id);
	GLint ok = GL_FALSE;
	glGetProgramiv(program.id, GL_LINK_STATUS, &ok);
	if(!ok) {
		LogError << "[VitaWorld] shader program failed to link";
		return false;
	}
	program.mvp = glGetUniformLocation(program.id, "uMVP");
	program.fog = glGetUniformLocation(program.id, "uFog");
	program.fogColor = glGetUniformLocation(program.id, "uFogColor");
	program.colorScale = glGetUniformLocation(program.id, "uColorScale");
	if(lit) {
		program.lightPos = glGetUniformLocation(program.id, "uLightPos");
		program.lightCol = glGetUniformLocation(program.id, "uLightCol");
	}
	return true;
}

bool initShaders() {

	if(g_shadersTried) {
		return g_shadersOk;
	}
	g_shadersTried = true;

	std::string litSource = litVertexShader(MaxLights);
	std::string lit4Source = litVertexShader(SmallLights);
	GLuint litVs = compile(GL_VERTEX_SHADER, litSource.c_str(), "lit vertex");
	GLuint lit4Vs = compile(GL_VERTEX_SHADER, lit4Source.c_str(), "lit vertex (4 lights)");
	GLuint unlitVs = compile(GL_VERTEX_SHADER, UnlitVertexShader, "unlit vertex");
	GLuint opaqueFs = compile(GL_FRAGMENT_SHADER, OpaqueFragmentShader, "opaque fragment");
	GLuint cutoutFs = compile(GL_FRAGMENT_SHADER, CutoutFragmentShader, "cutout fragment");
	if(!litVs || !lit4Vs || !unlitVs || !opaqueFs || !cutoutFs) {
		return false;
	}

	g_shadersOk = link(g_programs[LitOpaque], litVs, opaqueFs, MaxLights)
	              && link(g_programs[LitCutout], litVs, cutoutFs, MaxLights)
	              && link(g_programs[Lit4Opaque], lit4Vs, opaqueFs, SmallLights)
	              && link(g_programs[Lit4Cutout], lit4Vs, cutoutFs, SmallLights)
	              && link(g_programs[UnlitOpaque], unlitVs, opaqueFs, 0)
	              && link(g_programs[UnlitCutout], unlitVs, cutoutFs, 0);
	glUseProgram(0);

	if(g_shadersOk) {
		LogInfo << "[VitaWorld] shaders ready";
	}
	return g_shadersOk;
}

bool disabledByToggle() {
	static const bool s_disabled = [] {
		SceIoStat st;
		return sceIoGetstat("ux0:data/arx/noworldgpu.txt", &st) >= 0;
	}();
	return s_disabled;
}

// ---- building

BatchBucket bucketOf(const EERIEPOLY & poly) {
	if(!(poly.type & POLY_TRANS)) {
		return BatchBucket_Opaque;
	}
	if(poly.transval >= 2.f) {
		return BatchBucket_Multiplicative;
	} else if(poly.transval >= 1.f) {
		return BatchBucket_Additive;
	} else if(poly.transval > 0.f) {
		return BatchBucket_Blended;
	}
	return BatchBucket_Subtractive;
}

struct PolyRef {
	EERIEPOLY * poly;
	TextureContainer * material;
	BatchBucket bucket;
	int cellX;
	int cellY;

	bool operator<(const PolyRef & o) const {
		if(bucket != o.bucket) {
			return bucket < o.bucket; // opaque first
		}
		if(material != o.material) {
			return material < o.material;
		}
		if(cellX != o.cellX) {
			return cellX < o.cellX;
		}
		return cellY < o.cellY;
	}

	bool sameBatch(const PolyRef & o) const {
		return bucket == o.bucket && material == o.material && cellX == o.cellX && cellY == o.cellY;
	}
};

void finishBatch(Batch & batch, const std::vector<Vertex> & vertices, u32 endVertex) {
	Vec3f lo(std::numeric_limits<float>::max());
	Vec3f hi(-std::numeric_limits<float>::max());
	for(u32 i = batch.firstVertex; i < endVertex; i++) {
		lo = glm::min(lo, vertices[i].pos);
		hi = glm::max(hi, vertices[i].pos);
	}
	batch.center = (lo + hi) * 0.5f;
	batch.radius = glm::distance(lo, hi) * 0.5f;
}

void buildRoom(RoomHandle roomIndex, GpuRoom & out) {

	Room & room = g_rooms->rooms[roomIndex];

	std::vector<PolyRef> polys;
	polys.reserve(room.epdata.size());
	for(const EP_DATA & epd : room.epdata) {
		EERIEPOLY & poly = g_tiles->get(epd.tile).polygons()[epd.idx];
		out.tiles.push_back(epd.tile);
		if(!poly.tex || (poly.type & (POLY_IGNORE | POLY_NODRAW | POLY_HIDE))) {
			continue;
		}
		polys.push_back({ &poly, poly.tex, bucketOf(poly), epd.tile.x, epd.tile.y });
	}

	std::sort(out.tiles.begin(), out.tiles.end(), [](Vec2s a, Vec2s b) {
		return a.x != b.x ? a.x < b.x : a.y < b.y;
	});
	out.tiles.erase(std::unique(out.tiles.begin(), out.tiles.end()), out.tiles.end());

	if(polys.empty()) {
		return;
	}

	// cellX/cellY hold the tile for now: group by material, then cut the wide groups into cells
	std::stable_sort(polys.begin(), polys.end(), [](const PolyRef & a, const PolyRef & b) {
		return a.bucket != b.bucket ? a.bucket < b.bucket : a.material < b.material;
	});
	for(size_t begin = 0; begin < polys.size();) {
		size_t end = begin + 1;
		while(end < polys.size() && polys[end].bucket == polys[begin].bucket
		      && polys[end].material == polys[begin].material) {
			end++;
		}
		int minX = polys[begin].cellX, maxX = minX, minY = polys[begin].cellY, maxY = minY;
		for(size_t i = begin; i < end; i++) {
			minX = std::min(minX, polys[i].cellX);
			maxX = std::max(maxX, polys[i].cellX);
			minY = std::min(minY, polys[i].cellY);
			maxY = std::max(maxY, polys[i].cellY);
		}
		bool split = (maxX - minX >= SplitTiles || maxY - minY >= SplitTiles);
		for(size_t i = begin; i < end; i++) {
			polys[i].cellX = split ? polys[i].cellX / cellTiles() : 0;
			polys[i].cellY = split ? polys[i].cellY / cellTiles() : 0;
		}
		begin = end;
	}
	std::stable_sort(polys.begin(), polys.end());

	std::vector<Vertex> vertices;
	std::vector<u32> indices; // absolute: the vertex format is set once per room
	vertices.reserve(polys.size() * 4);
	indices.reserve(polys.size() * 6);

	Batch * batch = nullptr;
	const PolyRef * batchKey = nullptr;
	for(const PolyRef & ref : polys) {

		const EERIEPOLY & poly = *ref.poly;
		u32 nverts = (poly.type & POLY_QUAD) ? 4 : 3;

		if(!batch || !batchKey->sameBatch(ref)) {
			if(batch) {
				finishBatch(*batch, vertices, u32(vertices.size()));
			}
			out.batches.emplace_back();
			batch = &out.batches.back();
			batchKey = &ref;
			batch->material = ref.material;
			batch->bucket = ref.bucket;
			batch->firstVertex = u32(vertices.size());
			batch->firstIndex = u32(indices.size());
			batch->cutout = ref.material->m_pTexture && ref.material->m_pTexture->hasAlpha();
			batch->metal = (ref.material->userflags & POLY_METAL) != 0;
			batch->lit = (ref.bucket == BatchBucket_Opaque);
		}

		u32 base = u32(vertices.size());
		bool glow = (poly.type & POLY_GLOW) != 0;
		float lit = (!(poly.type & POLY_TRANS) && !glow) ? 1.f : 0.f;

		if(poly.type & (POLY_WATER | POLY_LAVA)) {
			out.animated.push_back({ ref.poly, u32(vertices.size()) });
		}

		for(u32 i = 0; i < nverts; i++) {
			Vertex v;
			v.pos = poly.v[i].p;
			v.color = glow ? Color::white.toRGBA() : poly.v[i].color;
			v.uv = poly.v[i].uv + ref.material->hd;
			v.normal = poly.nrml[i];
			v.lit = lit;
			vertices.push_back(v);
		}

		indices.push_back(base + 0);
		indices.push_back(base + 1);
		indices.push_back(base + 2);
		if(nverts == 4) {
			indices.push_back(base + 3);
			indices.push_back(base + 2);
			indices.push_back(base + 1);
		}
		batch->indexCount = u32(indices.size()) - batch->firstIndex;
	}
	if(batch) {
		finishBatch(*batch, vertices, u32(vertices.size()));
	}

	Vec3f lo(std::numeric_limits<float>::max());
	Vec3f hi(-std::numeric_limits<float>::max());
	for(const Batch & b : out.batches) {
		lo = glm::min(lo, b.center - Vec3f(b.radius));
		hi = glm::max(hi, b.center + Vec3f(b.radius));
	}
	out.center = (lo + hi) * 0.5f;
	out.radius = glm::distance(lo, hi) * 0.5f;

	glGenBuffers(1, &out.vbo);
	bindBuffer(out.vbo);
	glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(vertices.size() * sizeof(Vertex)), vertices.data(), GL_STATIC_DRAW);

	glGenBuffers(1, &out.ibo);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, out.ibo);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER, GLsizeiptr(indices.size() * sizeof(u32)), indices.data(), GL_STATIC_DRAW);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
}

// ---- per frame

struct LightSet {
	float pos[MaxLights * 4];
	float col[MaxLights * 4];
	size_t count;
};

void selectLights(const std::vector<const EERIE_LIGHT *> & candidates, const Batch & batch, LightSet & out) {

	struct Scored {
		const EERIE_LIGHT * light;
		float score;
	};
	Scored scored[g_dynamicLightsMax];
	size_t n = 0;
	for(const EERIE_LIGHT * light : candidates) {
		float reach = batch.radius + light->fallend;
		float d2 = arx::distance2(light->pos, batch.center);
		if(d2 < reach * reach) {
			scored[n++] = { light, std::sqrt(d2) - light->fallend };
		}
	}
	g_stats.maxCandidates = std::max(g_stats.maxCandidates, u32(n));
	if(n > MaxLights) {
		g_stats.overflow++;
		std::nth_element(scored, scored + MaxLights, scored + n,
		                 [](const Scored & a, const Scored & b) { return a.score < b.score; });
		n = MaxLights;
	}

	out.count = n;
	for(size_t i = 0; i < MaxLights; i++) {
		float * p = out.pos + i * 4;
		float * c = out.col + i * 4;
		if(i < n) {
			const EERIE_LIGHT & light = *scored[i].light;
			// Same as ApplyTileLights: rgb * intensity * GLOBAL_LIGHT_FACTOR (0.85) * 0.5
			float k = light.intensity * 0.425f;
			p[0] = light.pos.x;
			p[1] = light.pos.y;
			p[2] = light.pos.z;
			p[3] = light.fallend;
			c[0] = light.rgb.r * k;
			c[1] = light.rgb.g * k;
			c[2] = light.rgb.b * k;
			c[3] = light.falldiffmul;
		} else {
			p[0] = p[1] = p[2] = 0.f;
			p[3] = 0.f;
			c[0] = c[1] = c[2] = 0.f;
			c[3] = 0.f;
		}
	}
}

void roomLights(const GpuRoom & room, std::vector<const EERIE_LIGHT *> & out) {
	out.clear();
	for(size_t i = 0; i < g_culledDynamicLightsCount; i++) {
		const EERIE_LIGHT * light = g_culledDynamicLights[i];
		float reach = room.radius + light->fallend;
		if(arx::distance2(light->pos, room.center) < reach * reach) {
			out.push_back(light);
		}
	}
}

bool batchVisible(const Batch & batch, const RoomData::Frustums & frustums, const Vec3f & camPos, float fogCull) {
	float reach = fogCull + batch.radius;
	if(arx::distance2(batch.center, camPos) > reach * reach) {
		return false;
	}
	for(const EERIE_FRUSTRUM & frustum : frustums) {
		if(IsSphereInFrustrum(batch.center, frustum, batch.radius)) {
			return true;
		}
	}
	return false;
}

OpenGLRenderer * glRenderer() {
	return static_cast<OpenGLRenderer *>(GRenderer);
}

void useProgram(Program & program, OpenGLRenderer * renderer) {
	glUseProgram(program.id);
	if(program.frame != g_frame) {
		program.frame = g_frame;
		glm::mat4x4 mvp = renderer->projectionMatrix() * renderer->viewMatrix();
		glUniformMatrix4fv(program.mvp, 1, GL_FALSE, glm::value_ptr(mvp));
		float range = std::max(renderer->fogEnd() - renderer->fogStart(), 1.f);
		glUniform2f(program.fog, renderer->fogEnd(), 1.f / range);
	}
	// The fog color changes between the opaque and transparent passes
	Color4f fog(renderer->fogColor());
	glUniform4f(program.fogColor, fog.r, fog.g, fog.b, fog.a);
}

void setVertexFormat(u32 firstVertex) {
	const char * base = reinterpret_cast<const char *>(uintptr_t(firstVertex) * sizeof(Vertex));
	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), base + offsetof(Vertex, pos));
	glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(Vertex), base + offsetof(Vertex, color));
	glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), base + offsetof(Vertex, uv));
	glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, sizeof(Vertex), base + offsetof(Vertex, normal));
}

//! What the previous batch left bound, so consecutive batches of one material skip the setup.
struct DrawCache {
	const TextureContainer * material = nullptr;
	const Program * program = nullptr;
	float colorScale = -1.f;

	void reset() {
		*this = DrawCache();
	}
};

DrawCache g_drawCache;

void beginDraws() {
	for(GLuint i = 0; i < 4; i++) {
		glEnableVertexAttribArray(i);
	}
	g_drawCache.reset();
}

void endDraws() {
	for(GLuint i = 0; i < 4; i++) {
		glDisableVertexAttribArray(i);
	}
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
	glUseProgram(0);
}


void drawBatch(const Batch & batch, Program & program, OpenGLRenderer * renderer, const LightSet * lights) {

	DrawCache & cache = g_drawCache;
	if(cache.material != batch.material) {
		GRenderer->SetTexture(0, batch.material);
		renderer->beforeDraw<SMY_VERTEX>();
		cache.material = batch.material;
	}

	if(cache.program != &program) {
		useProgram(program, renderer);
		cache.program = &program;
		cache.colorScale = -1.f;
	}
	float colorScale = batch.metal ? 2.f : 1.f;
	if(cache.colorScale != colorScale) {
		glUniform1f(program.colorScale, colorScale);
		cache.colorScale = colorScale;
	}
	if(lights) {
		GLsizei count = GLsizei(program.lights);
		glUniform4fv(program.lightPos, count, lights->pos);
		glUniform4fv(program.lightCol, count, lights->col);
	}

	glDrawElements(GL_TRIANGLES, GLsizei(batch.indexCount), GL_UNSIGNED_INT,
	               reinterpret_cast<const void *>(uintptr_t(batch.firstIndex) * sizeof(u32)));

	EERIEDrawnPolys += long(batch.indexCount);
	EERIEDrawCalls++;
}

void bindRoom(const GpuRoom & room) {
	bindBuffer(room.vbo);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, room.ibo);
	setVertexFormat(0);
}

} // anonymous namespace

void build() {

	release();
	g_generation++;

	if(!g_rooms || disabledByToggle()) {
		return;
	}
	bool shaders;
	{
		VITA_LOAD_ZONE(LoadShaders);
		shaders = initShaders();
	}
	if(!shaders) {
		LogWarning << "[VitaWorld] falling back to the CPU room renderer";
		return;
	}

	g_gpuRooms.resize(g_rooms->rooms.size());
	size_t batches = 0;
	size_t animated = 0;
	for(RoomHandle room : g_rooms->rooms.handles()) {
		buildRoom(room, g_gpuRooms[size_t(room)]);
		batches += g_gpuRooms[size_t(room)].batches.size();
		animated += g_gpuRooms[size_t(room)].animated.size();
	}
	g_built = true;

	LogInfo << "[VitaWorld] built " << g_gpuRooms.size() << " rooms, " << batches << " batches, "
	        << animated << " animated polygons, cells of " << cellTiles() << "x" << cellTiles() << " tiles";
}

void release() {
	for(GpuRoom & room : g_gpuRooms) {
		if(room.vbo) {
			unbindBuffer(room.vbo);
			glDeleteBuffers(1, &room.vbo);
		}
		if(room.ibo) {
			glDeleteBuffers(1, &room.ibo);
		}
	}
	g_gpuRooms.clear();
	g_built = false;
	g_generation++;
}

unsigned generation() {
	return g_generation;
}

bool active() {
	return g_built && g_rooms && g_gpuRooms.size() == g_rooms->rooms.size();
}

void update(const Vec3f & camPos, std::vector<EERIEPOLY *> & water, std::vector<EERIEPOLY *> & lava) {

	g_frame++;

	float fogCull = g_camera->cdepth * fZFogEnd * 1.1f;
	float tileReach = fogCull + 150.f;
	float waterScroll = timeWaveSaw(g_gameTime.now(), 1s);
	float lavaScroll = timeWaveSaw(g_gameTime.now(), 12s);

	for(RoomHandle roomIndex : g_rooms->visibleRooms) {

		if(g_rooms->frustums[roomIndex].empty()) {
			continue;
		}
		GpuRoom & room = g_gpuRooms[size_t(roomIndex)];

		// Tiles near visible rooms are active: entity, particle and decal code relies on it
		for(Vec2s index : room.tiles) {
			auto tile = g_tiles->get(index);
			if(tile.active()) {
				continue;
			}
			Vec2f center = tile.center();
			float dx = center.x - camPos.x;
			float dz = center.y - camPos.z;
			if(dx * dx + dz * dz > tileReach * tileReach) {
				continue;
			}
			for(auto neighbour : g_tiles->tilesAround(tile, 1)) {
				if(!neighbour.active()) {
					neighbour.setActive();
					ComputeTileLights(neighbour);
				}
			}
		}

		if(room.animated.empty()) {
			continue;
		}

		// Water and lava scroll their texture: rewrite the UVs of the visible ones
		bindBuffer(room.vbo);
		Vertex * vertices = static_cast<Vertex *>(glMapBuffer(GL_ARRAY_BUFFER, GL_WRITE_ONLY));
		if(!vertices) {
			continue;
		}
		for(const AnimatedPoly & anim : room.animated) {
			EERIEPOLY * ep = anim.ep;
			float reach = fogCull + ep->v[0].w;
			if(arx::distance2(ep->center, camPos) > reach * reach) {
				continue;
			}
			if(ep->v[0].w < -distanceToPoint(efpPlaneNear, ep->center)) {
				continue;
			}
			Vec3f toPoly = ep->v[2].p - camPos;
			bool quad = (ep->type & POLY_QUAD) != 0;
			if(!(ep->type & POLY_DOUBLESIDED) && glm::dot(ep->norm, toPoly) > 0.f
			   && (!quad || glm::dot(ep->norm2, toPoly) > 0.f)) {
				continue;
			}
			bool isLava = (ep->type & POLY_LAVA) != 0;
			float scroll = isLava ? lavaScroll : waterScroll;
			size_t count = quad ? 4 : 3;
			for(size_t i = 0; i < count; i++) {
				Vec2f uv = ep->v[i].uv + getWaterFxUvOffset(WATEREFFECT, ep->v[i].p) * (0.35f * 0.05f);
				if(ep->type & POLY_FALL) {
					uv.y -= scroll;
				}
				vertices[anim.firstVertex + i].uv = uv;
			}
			(isLava ? lava : water).push_back(ep);
		}
		glUnmapBuffer(GL_ARRAY_BUFFER);
	}
}

void renderOpaque() {

	OpenGLRenderer * renderer = glRenderer();
	const Vec3f camPos = g_camera->m_pos;
	float fogCull = g_camera->cdepth * fZFogEnd * 1.1f;

	// Collect the visible batches with their lights, then draw them sorted by program, material
	// and room: consecutive draws then share the texture, the buffers and often the lights.
	struct DrawItem {
		const Batch * batch;
		const GpuRoom * room;
		ProgramId program;
		u32 lights; // in lightSets, or NoLights
	};
	static constexpr u32 NoLights = u32(-1);
	static std::vector<DrawItem> items;
	static std::vector<LightSet> lightSets;
	static std::vector<const EERIE_LIGHT *> candidates;
	items.clear();
	lightSets.clear();
	candidates.reserve(g_dynamicLightsMax);
	std::optional<platform::vita::ZoneScope> collectZone(std::in_place, platform::vita::ZoneWorldCollect);

	for(RoomHandle roomIndex : g_rooms->visibleRooms) {

		const RoomData::Frustums & frustums = g_rooms->frustums[roomIndex];
		const GpuRoom & room = g_gpuRooms[size_t(roomIndex)];
		if(frustums.empty() || !room.vbo) {
			continue;
		}

		roomLights(room, candidates);

		for(const Batch & batch : room.batches) {
			if(batch.bucket != BatchBucket_Opaque) {
				break; // opaque batches come first
			}
			if(!batchVisible(batch, frustums, camPos, fogCull)) {
				continue;
			}
			u32 lights = NoLights;
			if(batch.lit) {
				LightSet & set = lightSets.emplace_back();
				selectLights(candidates, batch, set);
				if(set.count > 0) {
					lights = u32(lightSets.size() - 1);
				} else {
					lightSets.pop_back();
				}
			}
			bool lit = (lights != NoLights);
			g_stats.batches++;
			g_stats.litBatches += lit ? 1 : 0;
			bool few = lit && lightSets[lights].count <= SmallLights;
			ProgramId program = few ? (batch.cutout ? Lit4Cutout : Lit4Opaque)
			                  : lit ? (batch.cutout ? LitCutout : LitOpaque)
			                        : (batch.cutout ? UnlitCutout : UnlitOpaque);
			items.push_back({ &batch, &room, program, lights });
		}
	}

	std::sort(items.begin(), items.end(), [](const DrawItem & a, const DrawItem & b) {
		if(a.program != b.program) {
			return a.program < b.program;
		}
		if(a.batch->material != b.batch->material) {
			return a.batch->material < b.batch->material;
		}
		if(a.room != b.room) {
			return a.room < b.room;
		}
		return a.batch->firstIndex < b.batch->firstIndex;
	});
	collectZone.reset();

	// The shaders do the alpha cutout, so one render state covers every opaque batch
	UseRenderState use(render3D());
	beginDraws();

	auto sameLights = [](const LightSet * a, const LightSet * b) {
		return a == b || (a && b && std::memcmp(a->pos, b->pos, sizeof(a->pos)) == 0
		                  && std::memcmp(a->col, b->col, sizeof(a->col)) == 0);
	};
	auto lightsOf = [&](const DrawItem & item) -> const LightSet * {
		return item.lights == NoLights ? nullptr : &lightSets[item.lights];
	};
	
	const GpuRoom * boundRoom = nullptr;
	const LightSet * uploaded[ProgramCount] = { };
	for(size_t i = 0; i < items.size();) {
		
		const DrawItem & item = items[i];
		const LightSet * lights = lightsOf(item);
		
		// Batches of one material next to each other in the room's index buffer, with the same
		// lights, are one draw
		Batch merged = *item.batch;
		size_t next = i + 1;
		while(next < items.size()) {
			const DrawItem & other = items[next];
			if(other.program != item.program || other.room != item.room
			   || other.batch->material != merged.material || other.batch->metal != merged.metal
			   || other.batch->firstIndex != merged.firstIndex + merged.indexCount
			   || !sameLights(lightsOf(other), lights)) {
				break;
			}
			merged.indexCount += other.batch->indexCount;
			next++;
		}
		g_stats.merged += u32(next - i - 1);
		i = next;
		
		if(item.room != boundRoom) {
			bindRoom(*item.room);
			boundRoom = item.room;
		}
		if(lights) {
			if(sameLights(uploaded[item.program], lights)) {
				lights = nullptr; // the program has these already
			} else {
				uploaded[item.program] = lights;
			}
		}
		drawBatch(merged, g_programs[item.program], renderer, lights);
	}

	endDraws();

	Stats & st = g_stats;
	st.frames++;
	u64 now = sceKernelGetProcessTimeWide();
	if(now >= st.nextLog) {
		if(st.nextLog != 0 && st.frames > 0) {
			LogInfo << "[VitaWorld] opaque batches/frame=" << st.batches / st.frames
			        << " lit=" << st.litBatches / st.frames << " light overflow/frame=" << st.overflow / st.frames
			        << " max lights near a batch=" << st.maxCandidates << " merged/frame=" << st.merged / st.frames;
		}
		st = Stats();
		st.nextLog = now + 5000000;
	}
}

void renderTransparent() {

	static constexpr std::array<BatchBucket, 4> order = {
		BatchBucket_Blended,
		BatchBucket_Multiplicative,
		BatchBucket_Additive,
		BatchBucket_Subtractive
	};

	OpenGLRenderer * renderer = glRenderer();
	const Vec3f camPos = g_camera->m_pos;
	float fogCull = g_camera->cdepth * fZFogEnd * 1.1f;

	beginDraws();

	for(BatchBucket bucket : order) {

		RenderState blendState = render3D().depthWrite(false).depthOffset(2);
		switch(bucket) {
			case BatchBucket_Blended:
				blendState.setBlend(BlendSrcColor, BlendDstColor);
				break;
			case BatchBucket_Multiplicative:
			case BatchBucket_Additive:
				blendState.setBlend(BlendOne, BlendOne);
				break;
			case BatchBucket_Subtractive:
				blendState.setDepthOffset(8);
				blendState.setBlend(BlendZero, BlendInvSrcColor);
				break;
			case BatchBucket_Opaque:
				arx_unreachable();
		}

		UseRenderState use(blendState);
		g_drawCache.material = nullptr; // the blend state changed: flush it with the next texture

		for(RoomHandle roomIndex : g_rooms->visibleRooms) {

			const RoomData::Frustums & frustums = g_rooms->frustums[roomIndex];
			const GpuRoom & room = g_gpuRooms[size_t(roomIndex)];
			if(frustums.empty() || !room.vbo) {
				continue;
			}

			bool bound = false;
			for(const Batch & batch : room.batches) {
				if(batch.bucket != bucket || !batchVisible(batch, frustums, camPos, fogCull)) {
					continue;
				}
				if(!bound) {
					bindRoom(room);
					bound = true;
				}
				drawBatch(batch, g_programs[batch.cutout ? UnlitCutout : UnlitOpaque], renderer, nullptr);
			}
		}
	}

	endDraws();
}

} // namespace vitaworld

#endif // ARX_PLATFORM == ARX_PLATFORM_VITA
