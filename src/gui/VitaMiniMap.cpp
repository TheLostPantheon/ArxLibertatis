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

#include "gui/VitaMiniMap.h"

#if ARX_PLATFORM == ARX_PLATFORM_VITA

#include <cstring>

#include <psp2/io/stat.h>

#include "graphics/Renderer.h"
#include "graphics/Vertex.h"
#include "graphics/data/Mesh.h"
#include "graphics/opengl/GLVertexBuffer.h"
#include "graphics/opengl/OpenGLRenderer.h"
#include "graphics/opengl/OpenGLUtil.h"
#include "io/log/Logger.h"
#include "platform/vita/VitaBench.h"

namespace vitaminimap {

namespace {

// The revealed levels with a border of unrevealed tiles: tiles outside the map are dark
const size_t RevealSize = MINIMAP_MAX_X + 2;
static_assert(MINIMAP_MAX_X == MINIMAP_MAX_Z, "square reveal texture");

const char * const VertexShader =
	"void main(\n"
	"	float2 aPosition,\n"
	"	uniform float2 uViewport,\n"
	"	float4 out vPosition : POSITION,\n"
	"	float2 out vPixel : TEXCOORD0)\n"
	"{\n"
	"	vPixel = aPosition;\n"
	"	vPosition = float4(-1.0 + (aPosition.x + 0.5) * uViewport.x, 1.0 - (aPosition.y + 0.5) * uViewport.y, 0.0, 1.0);\n"
	"}\n";

const char * const FragmentShader =
	"float4 main(\n"
	"	float2 vPixel : TEXCOORD0,\n"
	"	uniform sampler2D uMap : TEXUNIT0,\n"
	"	uniform sampler2D uReveal : TEXUNIT1,\n"
	"	uniform float4 uTiles,\n"
	"	uniform float4 uFade,\n"
	"	uniform float4 uParams,\n"
	"	uniform float uAlpha) : COLOR\n"
	"{\n"
	"	float2 t = (vPixel - uTiles.xy) * uTiles.zw;\n"
	"	float revealed = tex2D(uReveal, (t + 1.5) * uParams.w).r;\n"
	"	float fade = saturate((vPixel.x - uFade.x) * uParams.x) * saturate((uFade.z - vPixel.x) * uParams.x)\n"
	"	           * saturate((vPixel.y - uFade.y) * uParams.x) * saturate((uFade.w - vPixel.y) * uParams.x);\n"
	"	float4 c = tex2D(uMap, t * uParams.yz);\n"
	"	return float4(c.rgb * (revealed * fade * uAlpha), c.a);\n"
	"}\n";

struct Program {
	GLuint id = 0;
	GLint viewport = -1;
	GLint tiles = -1;
	GLint fade = -1;
	GLint params = -1;
	GLint alpha = -1;
};

Program g_program;
bool g_tried = false;
bool g_ok = false;

GLuint g_revealTexture = 0;
unsigned char g_uploaded[MINIMAP_MAX_X][MINIMAP_MAX_Z];
const void * g_uploadedFrom = nullptr;

bool disabledByToggle() {
	static const bool s_disabled = [] {
		SceIoStat st;
		return sceIoGetstat("ux0:data/arx/nominimapgpu.txt", &st) >= 0;
	}();
	return s_disabled;
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
		LogError << "[VitaMiniMap] " << name << " shader failed to compile: " << log;
		glDeleteShader(shader);
		return 0;
	}
	return shader;
}

bool init() {

	if(g_tried) {
		return g_ok;
	}
	g_tried = true;
	if(disabledByToggle()) {
		return false;
	}

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
	glLinkProgram(p.id);
	GLint ok = GL_FALSE;
	glGetProgramiv(p.id, GL_LINK_STATUS, &ok);
	if(!ok) {
		LogError << "[VitaMiniMap] shader program failed to link";
		return false;
	}
	p.viewport = glGetUniformLocation(p.id, "uViewport");
	p.tiles = glGetUniformLocation(p.id, "uTiles");
	p.fade = glGetUniformLocation(p.id, "uFade");
	p.params = glGetUniformLocation(p.id, "uParams");
	p.alpha = glGetUniformLocation(p.id, "uAlpha");
	// vitaGL takes sampler units from the uniforms, like GL, not from the TEXUNIT semantics
	glUseProgram(p.id);
	glUniform1i(glGetUniformLocation(p.id, "uMap"), 0);
	glUniform1i(glGetUniformLocation(p.id, "uReveal"), 1);
	glUseProgram(0);

	g_ok = true;
	return true;
}

//! Upload the revealed levels if they changed since the last upload (the player explores).
void updateReveal(const unsigned char (&revealed)[MINIMAP_MAX_X][MINIMAP_MAX_Z]) {

	bool fresh = (g_revealTexture == 0);
	if(fresh) {
		glGenTextures(1, &g_revealTexture);
	}
	glBindTexture(GL_TEXTURE_2D, g_revealTexture);
	if(!fresh && g_uploadedFrom == &revealed && std::memcmp(g_uploaded, revealed, sizeof(g_uploaded)) == 0) {
		return;
	}

	// Texel (1 + z, 1 + x): rows are z, like the minimap's y
	static unsigned char pixels[RevealSize * RevealSize * 4];
	std::memset(pixels, 0, sizeof(pixels));
	for(size_t x = 0; x < MINIMAP_MAX_X; x++) {
		for(size_t z = 0; z < MINIMAP_MAX_Z; z++) {
			unsigned char * texel = pixels + ((z + 1) * RevealSize + (x + 1)) * 4;
			texel[0] = texel[1] = texel[2] = texel[3] = revealed[x][z];
		}
	}
	if(fresh) {
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	}
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, GLsizei(RevealSize), GLsizei(RevealSize), 0, GL_RGBA,
	             GL_UNSIGNED_BYTE, pixels);
	std::memcpy(g_uploaded, revealed, sizeof(g_uploaded));
	g_uploadedFrom = &revealed;
}

} // anonymous namespace

void prepare() {
	VITA_LOAD_ZONE(LoadShaders);
	init();
}

bool drawBackground(const unsigned char (&revealed)[MINIMAP_MAX_X][MINIMAP_MAX_Z], const Rect & boundaries,
                    const Rect & fadeBounds, Vec2f start, Vec2f tileSize, Vec2f tileUv,
                    float fadeBorder, float alpha) {

	if(!init() || fadeBorder <= 0.f) {
		return false;
	}

	OpenGLRenderer * renderer = static_cast<OpenGLRenderer *>(GRenderer);
	renderer->beforeDraw<TexturedVertex>(); // render state and the map texture on unit 0

	GLint previousUnit = GL_TEXTURE0;
	glGetIntegerv(GL_ACTIVE_TEXTURE, &previousUnit);
	glActiveTexture(GL_TEXTURE1);
	GLint previousTexture = 0;
	glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture);
	updateReveal(revealed);
	glActiveTexture(GL_TEXTURE0);

	const Rect & viewport = renderer->viewportRect();
	Program & p = g_program;
	glUseProgram(p.id);
	glUniform2f(p.viewport, 2.f / float(viewport.width()), 2.f / float(viewport.height()));
	glUniform4f(p.tiles, start.x, start.y, 1.f / tileSize.x, 1.f / tileSize.y);
	glUniform4f(p.fade, float(fadeBounds.left), float(fadeBounds.top), float(fadeBounds.right),
	            float(fadeBounds.bottom));
	glUniform4f(p.params, 1.f / fadeBorder, tileUv.x, tileUv.y, 1.f / float(RevealSize));
	glUniform1f(p.alpha, alpha);
	

	const float quad[8] = {
		float(boundaries.left), float(boundaries.top),
		float(boundaries.right), float(boundaries.top),
		float(boundaries.left), float(boundaries.bottom),
		float(boundaries.right), float(boundaries.bottom),
	};
	bindBuffer(0);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, quad);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	glDisableVertexAttribArray(0);
	glUseProgram(0);
	EERIEDrawCalls++;

	glActiveTexture(GL_TEXTURE1);
	glBindTexture(GL_TEXTURE_2D, GLuint(previousTexture));
	glActiveTexture(GLenum(previousUnit));
	return true;
}

} // namespace vitaminimap

#endif // ARX_PLATFORM == ARX_PLATFORM_VITA
