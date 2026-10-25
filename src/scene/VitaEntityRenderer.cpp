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

#include "scene/VitaEntityRenderer.h"

#if ARX_PLATFORM == ARX_PLATFORM_VITA

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <vector>

#include <glm/gtc/type_ptr.hpp>

#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>

#include "animation/AnimationRender.h"
#include "game/Entity.h"
#include "game/EntityManager.h"
#include "game/Player.h"
#include "graphics/GraphicsTypes.h"
#include "graphics/Renderer.h"
#include "graphics/data/Mesh.h"
#include "graphics/data/TextureContainer.h"
#include "graphics/opengl/GLVertexBuffer.h"
#include "graphics/opengl/OpenGLRenderer.h"
#include "graphics/opengl/OpenGLUtil.h"
#include "graphics/texture/Texture.h"
#include "graphics/texture/TextureStage.h"
#include "io/log/Logger.h"
#include "platform/vita/VitaBench.h"
#include "scene/Light.h"

extern long IN_BOOK_DRAW;

namespace vitaentity {

struct Range {
	MaterialId material;
	u32 firstIndex = 0;
	u32 count = 0;
	bool doubleSided = false;
};

struct Mesh {
	GLuint vbo = 0;
	GLuint ibo = 0;
	std::vector<Range> ranges;
	bool supported = false;
	// what it was built from, to notice changes
	bool skinned = false;
	bool flat = false;
	size_t faceCount = 0;
	size_t vertexCount = 0;
	const EERIE_FACE * faceData = nullptr;
	unsigned facesVersion = 0;
	size_t boneCount = 0;
	const char * unsupported = nullptr; // why the stock path draws it
};

namespace {

const size_t MaxLights = 4;  // UpdateLlights keeps the 4 nearest on Vita
const size_t MaxBones = 60;  // must match the vertex shader
const float GlobalLightFactor = 0.85f; // GLOBAL_LIGHT_FACTOR in Light.cpp

struct Vertex {
	Vec3f pos;       // bone space (skinned) or model space (rigid)
	Vec2f uv;
	Vec3f normal;
	ColorRGBA color; // for unlit faces
	u8 bone;
	u8 lit;
	u8 pad[2];
};
static_assert(sizeof(Vertex) == 40, "vertex layout");

const char * const VertexShader =
	"void main(\n"
	"	float3 aPosition,\n"
	"	float2 aTexcoord,\n"
	"	float3 aNormal,\n"
	"	float4 aColor,\n"
	"	float4 aBone,\n"
	"	uniform float4x4 uViewProj,\n"
	"	uniform float2 uFog,\n"
	"	uniform float4 uBones[180],\n"
	"	uniform float4 uLightPos[4],\n"
	"	uniform float4 uLightCol[4],\n"
	"	uniform float4 uAmbient,\n"
	"	uniform float4 uFactor,\n"
	"	uniform float4 uTerm,\n"
	"	float4 out vPosition : POSITION,\n"
	"	float4 out vColor : COLOR,\n"
	"	float2 out vTexcoord : TEXCOORD0,\n"
	"	float out vFog : TEXCOORD1)\n"
	"{\n"
	"	int b = int(aBone.x) * 3;\n"
	"	float4 r0 = uBones[b];\n"
	"	float4 r1 = uBones[b + 1];\n"
	"	float4 r2 = uBones[b + 2];\n"
	"	float4 p = float4(aPosition, 1.0);\n"
	"	float3 w = float3(dot(r0, p), dot(r1, p), dot(r2, p));\n"
	"	float3 n = normalize(float3(dot(r0.xyz, aNormal), dot(r1.xyz, aNormal), dot(r2.xyz, aNormal)));\n"
	"	float3 c = uAmbient.rgb;\n"
	"	for(int i = 0; i < 4; i++) {\n"
	"		float3 l = uLightPos[i].xyz - w;\n"
	"		float d2 = max(dot(l, l), 0.0001);\n"
	"		float inv = rsqrt(d2);\n"
	"		float ndl = max(dot(n, l) * inv, 0.0);\n"
	"		float att = saturate((uLightPos[i].w - d2 * inv) * uLightCol[i].w);\n"
	"		c += uLightCol[i].rgb * (ndl * att);\n"
	"	}\n"
	"	c = saturate(c * uFactor.rgb + uTerm.rgb);\n"
	"	vColor = float4(lerp(aColor.rgb, c, aBone.y), 1.0);\n"
	"	vPosition = mul(float4(w, 1.0), uViewProj);\n"
	"	vTexcoord = aTexcoord;\n"
	"	vFog = saturate((uFog.x - vPosition.w) * uFog.y);\n"
	"}\n";

const char * const FragmentShader =
	"float4 main(\n"
	"	float4 vColor : COLOR,\n"
	"	float2 vTexcoord : TEXCOORD0,\n"
	"	float vFog : TEXCOORD1,\n"
	"	uniform sampler2D uTexture : TEXUNIT0,\n"
	"	uniform float4 uFogColor,\n"
	"	uniform float uAlphaRef) : COLOR\n"
	"{\n"
	"	float4 t = tex2D(uTexture, vTexcoord);\n"
	"	if(t.a <= uAlphaRef) {\n"
	"		discard;\n"
	"	}\n"
	"	float3 rgb = t.rgb * vColor.rgb;\n"
	"	return float4(lerp(uFogColor.rgb, rgb, vFog), t.a);\n"
	"}\n";

struct Program {
	GLuint id = 0;
	GLint viewProj = -1;
	GLint fog = -1;
	GLint bones = -1;
	GLint lightPos = -1;
	GLint lightCol = -1;
	GLint ambient = -1;
	GLint factor = -1;
	GLint term = -1;
	GLint fogColor = -1;
	GLint alphaRef = -1;
	u64 frame = 0;
};

Program g_program;
bool g_shadersTried = false;
bool g_shadersOk = false;
bool g_shutdown = false; // GL is gone: leave the buffers alone
u64 g_frame = 1;

// Building a model's buffers takes ~1-3 ms: at most this much per frame, the rest of the
// models stay on the stock path a few frames longer (the first frame after a load sees dozens)
const s64 BuildBudgetUs = 3000;
s64 g_buildBudgetUs = BuildBudgetUs;

//! Per-frame counters, logged every 5 s
struct Stats {
	u32 skinned = 0;
	u32 rigid = 0;
	u32 draws = 0;
	u32 stock = 0;
	u32 builds = 0;
	u32 deferred = 0; // builds put off to a later frame
	u32 frames = 0;
	u64 nextLog = 0;
	const char * lastUnsupported = nullptr;
};
Stats g_stats;

bool disabledByToggle() {
	static const bool s_disabled = [] {
		SceIoStat st;
		return sceIoGetstat("ux0:data/arx/noentitygpu.txt", &st) >= 0;
	}();
	return s_disabled;
}

//! Which faces GL culls: 0 none, 1 back (default), 2 front (ux0:data/arx/entcull.txt)
int cullMode() {
	static const int s_mode = [] {
		int mode = 1;
		if(FILE * f = std::fopen("ux0:data/arx/entcull.txt", "r")) {
			if(std::fscanf(f, "%d", &mode) != 1 || mode < 0 || mode > 2) {
				mode = 1;
			}
			std::fclose(f);
		}
		return mode;
	}();
	return s_mode;
}

GLuint compile(GLenum type, const char * source, const char * name) {
	GLuint shader = glCreateShader(type);
	glShaderSource(shader, 1, &source, nullptr);
	glCompileShader(shader);
	GLint ok = GL_FALSE;
	glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
	if(!ok) {
		char log[512] = "";
		glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
		LogError << "[VitaEntity] " << name << " shader failed to compile: " << log;
		glDeleteShader(shader);
		return 0;
	}
	return shader;
}

bool initShaders() {

	if(g_shadersTried) {
		return g_shadersOk;
	}
	g_shadersTried = true;

	GLuint vs = compile(GL_VERTEX_SHADER, VertexShader, "vertex");
	GLuint fs = compile(GL_FRAGMENT_SHADER, FragmentShader, "fragment");
	if(!vs || !fs) {
		return false;
	}

	Program & p = g_program;
	p.id = glCreateProgram();
	glAttachShader(p.id, vs);
	glAttachShader(p.id, fs);
	glBindAttribLocation(p.id, 0, "aPosition");
	glBindAttribLocation(p.id, 1, "aTexcoord");
	glBindAttribLocation(p.id, 2, "aNormal");
	glBindAttribLocation(p.id, 3, "aColor");
	glBindAttribLocation(p.id, 4, "aBone");
	glLinkProgram(p.id);
	GLint ok = GL_FALSE;
	glGetProgramiv(p.id, GL_LINK_STATUS, &ok);
	if(!ok) {
		LogError << "[VitaEntity] shader program failed to link";
		return false;
	}
	p.viewProj = glGetUniformLocation(p.id, "uViewProj");
	p.fog = glGetUniformLocation(p.id, "uFog");
	p.bones = glGetUniformLocation(p.id, "uBones");
	p.lightPos = glGetUniformLocation(p.id, "uLightPos");
	p.lightCol = glGetUniformLocation(p.id, "uLightCol");
	p.ambient = glGetUniformLocation(p.id, "uAmbient");
	p.factor = glGetUniformLocation(p.id, "uFactor");
	p.term = glGetUniformLocation(p.id, "uTerm");
	p.fogColor = glGetUniformLocation(p.id, "uFogColor");
	p.alphaRef = glGetUniformLocation(p.id, "uAlphaRef");
	glUseProgram(0);

	// Objects released after the renderer (static ones, at exit) must not touch GL
	std::atexit([] { g_shutdown = true; });

	g_shadersOk = true;
	LogInfo << "[VitaEntity] shader ready, cull mode " << cullMode();
	return true;
}

bool active() {
	return !disabledByToggle() && initShaders();
}

// ---- building

struct VertexKey {
	u32 vertex;
	float u;
	float v;
	u32 lit;
	bool operator==(const VertexKey & o) const {
		return vertex == o.vertex && u == o.u && v == o.v && lit == o.lit;
	}
};

struct VertexKeyHash {
	size_t operator()(const VertexKey & k) const {
		u32 u;
		u32 v;
		std::memcpy(&u, &k.u, sizeof(u));
		std::memcpy(&v, &k.v, sizeof(v));
		size_t h = k.vertex * 0x9E3779B1u;
		h ^= u + 0x7F4A7C15u + (h << 6) + (h >> 2);
		h ^= v + 0x165667B1u + (h << 6) + (h >> 2);
		return h ^ k.lit;
	}
};

void build(Mesh & mesh, const EERIE_3DOBJ & eobj, bool skinned, bool flat) {

	mesh.skinned = skinned;
	mesh.flat = flat;
	mesh.faceCount = eobj.facelist.size();
	mesh.vertexCount = eobj.vertexlist.size();
	mesh.faceData = eobj.facelist.data();
	mesh.facesVersion = eobj.m_facesVersion;
	g_stats.builds++;

	std::vector<u8> boneOf(eobj.vertexlist.size(), 0);
	if(skinned) {
		const Skeleton * skeleton = eobj.m_skeleton.get();
		if(!skeleton || eobj.m_boneVertices.size() != skeleton->bones.size()) {
			mesh.unsupported = "no skeleton";
			return;
		}
		if(skeleton->bones.size() > MaxBones) {
			mesh.unsupported = "too many bones";
			return;
		}
		if(eobj.vertexlocal.size() != eobj.vertexlist.size()) {
			mesh.unsupported = "no bone space vertices";
			return;
		}
		for(VertexGroupId group : eobj.m_boneVertices.handles()) {
			for(VertexId vertex : eobj.m_boneVertices[group]) {
				boneOf[size_t(vertex)] = u8(size_t(group));
			}
		}
		mesh.boneCount = skeleton->bones.size();
	}

	struct FaceRef {
		const EERIE_FACE * face;
		size_t material;
		bool doubleSided;
	};
	std::vector<FaceRef> faces;
	faces.reserve(eobj.facelist.size());
	for(const EERIE_FACE & face : eobj.facelist) {
		if((face.facetype & POLY_HIDE) || !face.material) {
			continue;
		}
		if(face.facetype & (POLY_TRANS | POLY_WATER)) {
			mesh.unsupported = "transparent or water faces";
			return;
		}
		faces.push_back({ &face, size_t(face.material), (face.facetype & POLY_DOUBLESIDED) != 0 });
	}
	std::stable_sort(faces.begin(), faces.end(), [](const FaceRef & a, const FaceRef & b) {
		return a.material != b.material ? a.material < b.material : a.doubleSided < b.doubleSided;
	});

	std::vector<Vertex> vertices;
	std::vector<u16> indices;
	vertices.reserve(eobj.vertexlist.size() * 2);
	indices.reserve(faces.size() * 3);
	// Sized by the constructor: reserve() would pull in libm's ceill, which clashes with libstdc++'s
	std::unordered_map<VertexKey, u16, VertexKeyHash> lookup(eobj.vertexlist.size() * 2);

	for(const FaceRef & ref : faces) {

		const EERIE_FACE & face = *ref.face;
		if(mesh.ranges.empty() || size_t(mesh.ranges.back().material) != ref.material
		   || mesh.ranges.back().doubleSided != ref.doubleSided) {
			Range range;
			range.material = face.material;
			range.firstIndex = u32(indices.size());
			range.doubleSided = ref.doubleSided;
			mesh.ranges.push_back(range);
		}

		bool glow = (face.facetype & POLY_GLOW) != 0;
		for(size_t n = 0; n < 3; n++) {
			size_t vertex = size_t(face.vid[n]);
			VertexKey key = { u32(vertex), face.u[n], face.v[n], glow ? 0u : 1u };
			u16 index;
			auto it = flat ? lookup.end() : lookup.find(key);
			if(it != lookup.end()) {
				index = it->second;
			} else {
				if(vertices.size() >= 65535) {
					mesh.unsupported = "too many vertices";
					mesh.ranges.clear();
					return;
				}
				Vertex v;
				v.pos = skinned ? eobj.vertexlocal[face.vid[n]] : eobj.vertexlist[face.vid[n]].v;
				v.uv = Vec2f(face.u[n], face.v[n]);
				v.normal = flat ? face.norm : eobj.vertexlist[face.vid[n]].norm;
				v.color = Color::white.toRGBA();
				v.bone = boneOf[vertex];
				v.lit = glow ? 0 : 1;
				v.pad[0] = v.pad[1] = 0;
				index = u16(vertices.size());
				vertices.push_back(v);
				if(!flat) {
					lookup.emplace(key, index);
				}
			}
			indices.push_back(index);
		}
		mesh.ranges.back().count = u32(indices.size()) - mesh.ranges.back().firstIndex;
	}

	if(indices.empty()) {
		mesh.unsupported = "no visible faces";
		return;
	}

	glGenBuffers(1, &mesh.vbo);
	bindBuffer(mesh.vbo);
	glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(vertices.size() * sizeof(Vertex)), vertices.data(), GL_STATIC_DRAW);
	glGenBuffers(1, &mesh.ibo);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mesh.ibo);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER, GLsizeiptr(indices.size() * sizeof(u16)), indices.data(), GL_STATIC_DRAW);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

	mesh.supported = true;
}

//! The model's GPU copy, (re)built if the model changed since.
Mesh * meshFor(EERIE_3DOBJ & eobj, bool skinned, bool flat) {

	Mesh * mesh = eobj.m_gpuMesh.get();
	if(mesh && mesh->skinned == skinned && mesh->flat == flat && mesh->faceCount == eobj.facelist.size()
	   && mesh->vertexCount == eobj.vertexlist.size() && mesh->faceData == eobj.facelist.data()
	   && mesh->facesVersion == eobj.m_facesVersion
	   && (!skinned || (eobj.m_skeleton && mesh->boneCount == eobj.m_skeleton->bones.size()))) {
		return mesh;
	}

	if(g_buildBudgetUs <= 0) {
		g_stats.deferred++;
		return nullptr;
	}
	u64 start = sceKernelGetProcessTimeWide();
	eobj.m_gpuMesh.reset(new Mesh);
	mesh = eobj.m_gpuMesh.get();
	build(*mesh, eobj, skinned, flat);
	g_buildBudgetUs -= s64(sceKernelGetProcessTimeWide() - start);
	if(!mesh->supported) {
		g_stats.lastUnsupported = mesh->unsupported;
	}
	return mesh;
}

// ---- drawing

struct Uniforms {
	float bones[MaxBones * 12];
	size_t boneCount = 0;
	float lightPos[MaxLights * 4];
	float lightCol[MaxLights * 4];
	float ambient[4];
	float factor[4];
	float term[4];
};

void setBone(Uniforms & u, size_t index, const glm::mat3 & m, const Vec3f & t) {
	float * row = u.bones + index * 12;
	for(int r = 0; r < 3; r++) {
		row[r * 4 + 0] = m[0][r];
		row[r * 4 + 1] = m[1][r];
		row[r * 4 + 2] = m[2][r];
		row[r * 4 + 3] = t[r];
	}
}

//! Lights and color modifiers, as ApplyLight uses them.
void setLighting(Uniforms & u, Entity * io, const Vec3f & lightPos, float diffuse) {

	ColorMod colorMod;
	colorMod.updateFromEntity(io);

	ShaderLight lights[llightsSize];
	size_t count = 0;
	UpdateLlights(lights, count, lightPos, false);
	count = std::min(count, MaxLights);

	for(size_t i = 0; i < MaxLights; i++) {
		float * p = u.lightPos + i * 4;
		float * c = u.lightCol + i * 4;
		if(i < count) {
			const ShaderLight & light = lights[i];
			float k = light.intensity * GlobalLightFactor * diffuse;
			p[0] = light.pos.x;
			p[1] = light.pos.y;
			p[2] = light.pos.z;
			p[3] = light.fallend;
			c[0] = light.rgb.r * k;
			c[1] = light.rgb.g * k;
			c[2] = light.rgb.b * k;
			c[3] = light.falldiffmul;
		} else {
			p[0] = p[1] = p[2] = p[3] = 0.f;
			c[0] = c[1] = c[2] = c[3] = 0.f;
		}
	}

	u.ambient[0] = colorMod.ambientColor.r * (1.f / 255);
	u.ambient[1] = colorMod.ambientColor.g * (1.f / 255);
	u.ambient[2] = colorMod.ambientColor.b * (1.f / 255);
	u.ambient[3] = 0.f;
	u.factor[0] = colorMod.factor.r;
	u.factor[1] = colorMod.factor.g;
	u.factor[2] = colorMod.factor.b;
	u.factor[3] = 1.f;
	u.term[0] = colorMod.term.r * (1.f / 255);
	u.term[1] = colorMod.term.g * (1.f / 255);
	u.term[2] = colorMod.term.b * (1.f / 255);
	u.term[3] = 0.f;
}

//! The player is queued while it renders: in first person all of it is drawn over the
//! scene first, then again with the depth test, like the stock path's two passes.
struct PlayerDraw {
	EERIE_3DOBJ * eobj;
	const Mesh * mesh;
	Uniforms uniforms;
};

struct PlayerPass {
	bool active = false;
	bool overlay = false;
	std::vector<PlayerDraw> queue;
};
PlayerPass g_player;

//! What both paths decline: effects only the stock path draws.
bool usable(Entity * io, float invisibility) {
	if(!io || IN_BOOK_DRAW || player.m_improve || invisibility > 0.f) {
		return false;
	}
	if(io == entities.player() && !g_player.active) {
		return false; // only between beginPlayer() and endPlayer()
	}
	if(io->sfx_flag & SFX_TYPE_YLSIDE_DEATH) {
		return false; // glows while it burns away
	}
	return active();
}

void logStats() {
	Stats & st = g_stats;
	st.frames++;
	u64 now = sceKernelGetProcessTimeWide();
	if(now < st.nextLog) {
		return;
	}
	if(st.nextLog != 0 && st.frames > 0) {
		float n = float(st.frames);
		LogInfo << "[VitaEntity] per frame: skinned " << float(st.skinned) / n << ", rigid "
		        << float(st.rigid) / n << ", draws " << float(st.draws) / n << ", stock "
		        << float(st.stock) / n << "; mesh builds " << st.builds << " (deferred " << st.deferred << ")"
		        << (st.lastUnsupported ? "; stock because: " : "")
		        << (st.lastUnsupported ? st.lastUnsupported : "");
	}
	st = Stats();
	st.nextLog = now + 5000000;
}

//! Draw the mesh with base as the render state (culling added for single-sided faces).
void submit(EERIE_3DOBJ & eobj, const Mesh & mesh, const Uniforms & u, RenderState base) {

	OpenGLRenderer * renderer = static_cast<OpenGLRenderer *>(GRenderer);
	Program & p = g_program;

	glUseProgram(p.id);
	if(p.frame != g_frame) {
		p.frame = g_frame;
		glm::mat4x4 viewProj = renderer->projectionMatrix() * renderer->viewMatrix();
		glUniformMatrix4fv(p.viewProj, 1, GL_FALSE, glm::value_ptr(viewProj));
		float range = std::max(renderer->fogEnd() - renderer->fogStart(), 1.f);
		glUniform2f(p.fog, renderer->fogEnd(), 1.f / range);
		Color4f fog(renderer->fogColor());
		glUniform4f(p.fogColor, fog.r, fog.g, fog.b, fog.a);
	}
	glUniform4fv(p.bones, GLsizei(std::max<size_t>(u.boneCount, 1) * 3), u.bones);
	glUniform4fv(p.lightPos, GLsizei(MaxLights), u.lightPos);
	glUniform4fv(p.lightCol, GLsizei(MaxLights), u.lightCol);
	glUniform4fv(p.ambient, 1, u.ambient);
	glUniform4fv(p.factor, 1, u.factor);
	glUniform4fv(p.term, 1, u.term);

	bindBuffer(mesh.vbo);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mesh.ibo);
	for(GLuint i = 0; i < 5; i++) {
		glEnableVertexAttribArray(i);
	}
	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<const void *>(offsetof(Vertex, pos)));
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<const void *>(offsetof(Vertex, uv)));
	glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<const void *>(offsetof(Vertex, normal)));
	glVertexAttribPointer(3, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(Vertex), reinterpret_cast<const void *>(offsetof(Vertex, color)));
	glVertexAttribPointer(4, 4, GL_UNSIGNED_BYTE, GL_FALSE, sizeof(Vertex), reinterpret_cast<const void *>(offsetof(Vertex, bone)));

	const RenderState saved = GRenderer->getRenderState();
	const RenderState culled = base.cull(cullMode() != 0);
	const RenderState doubleSided = base;
	if(cullMode() == 2) {
		glCullFace(GL_FRONT);
	}

	float alphaRef = -2.f;
	for(const Range & range : mesh.ranges) {

		TextureContainer * material = eobj.materials[range.material];
		if(!material) {
			continue;
		}

		GRenderer->setRenderState(range.doubleSided ? doubleSided : culled);
		GRenderer->SetTexture(0, material);
		renderer->beforeDraw<SMY_VERTEX>();

		float ref = (material->m_pTexture && material->m_pTexture->hasAlpha()) ? 0.5f : -1.f;
		if(ref != alphaRef) {
			glUniform1f(p.alphaRef, ref);
			alphaRef = ref;
		}

		glDrawElements(GL_TRIANGLES, GLsizei(range.count), GL_UNSIGNED_SHORT,
		               reinterpret_cast<const void *>(uintptr_t(range.firstIndex) * sizeof(u16)));

		EERIEDrawnPolys += long(range.count / 3);
		EERIEDrawCalls++;
		g_stats.draws++;
	}

	if(cullMode() == 2) {
		glCullFace(GL_BACK);
	}
	GRenderer->setRenderState(saved);

	for(GLuint i = 0; i < 5; i++) {
		glDisableVertexAttribArray(i);
	}
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
	glUseProgram(0);
}


void draw(EERIE_3DOBJ & eobj, const Mesh & mesh, const Uniforms & u) {
	if(g_player.active) {
		g_player.queue.push_back({ &eobj, &mesh, u });
		return;
	}
	submit(eobj, mesh, u, render3D());
}

} // anonymous namespace

void MeshDeleter::operator()(Mesh * mesh) const noexcept {
	if(!mesh) {
		return;
	}
	if(!g_shutdown && GRenderer) {
		if(mesh->vbo) {
			unbindBuffer(mesh->vbo);
			glDeleteBuffers(1, &mesh->vbo);
		}
		if(mesh->ibo) {
			glDeleteBuffers(1, &mesh->ibo);
		}
	}
	delete mesh;
}

void prepare() {
	if(!disabledByToggle()) {
		VITA_LOAD_ZONE(LoadShaders);
		initShaders();
	}
}

void beginPlayer(bool overlay) {
	g_player.active = true;
	g_player.overlay = overlay;
	g_player.queue.clear();
}

void endPlayer() {
	g_player.active = false;
	if(g_player.queue.empty()) {
		return;
	}
	UseTextureState textureState(TextureStage::FilterLinear, TextureStage::WrapClamp);
	if(g_player.overlay) {
		for(PlayerDraw & d : g_player.queue) {
			submit(*d.eobj, *d.mesh, d.uniforms, render3D().depthTest(false));
		}
	}
	for(PlayerDraw & d : g_player.queue) {
		submit(*d.eobj, *d.mesh, d.uniforms, render3D());
	}
	g_player.queue.clear();
}

void beginFrame() {
	g_frame++;
	g_buildBudgetUs = BuildBudgetUs;
	logStats();
}

bool handlesSkinned(Entity & io) {
	if(!usable(&io, 0.f) || !io.obj) {
		return false;
	}
	const Mesh * mesh = meshFor(*io.obj, true, false);
	return mesh && mesh->supported;
}

bool drawSkinned(EERIE_3DOBJ * eobj, Entity * io, const Vec3f & lightPos, float invisibility) {

	if(!eobj || !eobj->m_skeleton || !usable(io, invisibility)) {
		g_stats.stock++;
		return false;
	}
	Mesh * mesh = meshFor(*eobj, true, false);
	if(!mesh || !mesh->supported) {
		g_stats.stock++;
		return false;
	}

	Uniforms u;
	const Skeleton & skeleton = *eobj->m_skeleton;
	u.boneCount = skeleton.bones.size();
	size_t index = 0;
	for(const Bone & bone : skeleton.bones) {
		glm::mat3 m = glm::mat3_cast(bone.anim.quat);
		m[0] *= bone.anim.scale.x;
		m[1] *= bone.anim.scale.y;
		m[2] *= bone.anim.scale.z;
		setBone(u, index++, m, bone.anim.trans);
	}
	setLighting(u, io, lightPos, 1.f);

	draw(*eobj, *mesh, u);
	g_stats.skinned++;
	return true;
}

bool drawRigid(EERIE_3DOBJ * eobj, const TransformInfo & t, Entity * io, float invisibility) {

	// Linked objects (weapons, shields) have no entity of their own on some NPCs
	if(!eobj || (io && !usable(io, invisibility)) || (!io && (IN_BOOK_DRAW || player.m_improve
	                                                          || invisibility > 0.f || !active()))) {
		g_stats.stock++;
		return false;
	}
	bool flat = io && (io->ioflags & IO_ANGULAR);
	Mesh * mesh = meshFor(*eobj, false, flat);
	if(!mesh || !mesh->supported) {
		g_stats.stock++;
		return false;
	}

	Uniforms u;
	u.boneCount = 1;
	setBone(u, 0, glm::mat3_cast(t.rotation) * t.scale, t.pos);
	Vec3f lightPos = t.pos;
	lightPos.y -= (io && (io->ioflags & IO_ITEM)) ? 60.f : 90.f;
	setLighting(u, io, lightPos, flat ? 0.5f : 1.f);

	draw(*eobj, *mesh, u);
	g_stats.rigid++;
	return true;
}

} // namespace vitaentity

#endif // ARX_PLATFORM == ARX_PLATFORM_VITA
