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

#include "graphics/opengl/GLTexture.h"

#include "core/Config.h"
#include "graphics/Math.h"
#include "graphics/opengl/GLTextureStage.h"
#include "graphics/opengl/OpenGLRenderer.h"
#include "graphics/opengl/OpenGLUtil.h"
#include "io/fs/FilePath.h" // TODO remove
#include "io/log/Logger.h"
#include "platform/Platform.h"



GLTexture::GLTexture(OpenGLRenderer * _renderer)
	: renderer(_renderer)
	, tex(GL_NONE)
	, wrapMode(TextureStage::WrapRepeat)
	, minFilter(TextureStage::FilterLinear)
	, magFilter(TextureStage::FilterNearest)
	, isNPOT(false)
{ }

GLTexture::~GLTexture() {
	destroy();
}

bool GLTexture::create() {

	arx_assert_msg(tex == GL_NONE, "leaking OpenGL texture");

	glGenTextures(1, &tex);

	// Set our state to the default OpenGL state
	wrapMode = TextureStage::WrapRepeat;
	minFilter = TextureStage::FilterNearest;
	magFilter = TextureStage::FilterLinear;

	#if ARX_PLATFORM == ARX_PLATFORM_VITA
	// Disable mipmaps on Vita to save VRAM (~33% per texture)
	m_flags &= ~HasMipmaps;
	#endif

	Vec2i nextPowerOfTwo(GetNextPowerOf2(unsigned(getSize().x)), GetNextPowerOf2(unsigned(getSize().y)));
	m_storedSize = renderer->hasTextureNPOT() ? getSize() : nextPowerOfTwo;
	isNPOT = (getSize() != nextPowerOfTwo);

	return (tex != GL_NONE);
}

void GLTexture::upload() {
	
	arx_assert(tex != GL_NONE);

	// Selective texture downscaling on Vita
	if(config.video.textureDownscale
	   && m_image.getNumChannels() >= 3
	   && (m_image.getWidth() > 256 || m_image.getHeight() > 256)) {
		const res::path & fn = getFileName();
		bool isUI = !fn.empty() && (
			fn.string().find("interface") != std::string::npos
			|| fn.string().find("icon") != std::string::npos
			|| fn.string().find("rune") != std::string::npos
		);
		if(!isUI) {
			m_image.halfResize();
			m_size = Vec2i(s32(m_image.getWidth()), s32(m_image.getHeight()));
			m_format = m_image.getFormat();
			Vec2i nextPOT(GetNextPowerOf2(unsigned(m_size.x)), GetNextPowerOf2(unsigned(m_size.y)));
			m_storedSize = renderer->hasTextureNPOT() ? m_size : nextPOT;
			isNPOT = (m_size != nextPOT);
		}
	}

	glBindTexture(GL_TEXTURE_2D, tex);
	renderer->GetTextureStage(0)->current = this;

	// I8 to L8A8
	if(!renderer->hasIntensityTextures() && isIntensity()) {
		arx_assert(getFormat() == Image::Format_L8);
		size_t imgW = m_image.getWidth();
		size_t imgH = m_image.getHeight();
		Image converted;
		converted.create(imgW, imgH, Image::Format_L8A8);
		unsigned char * input = m_image.getData();
		unsigned char * end = input + imgW * imgH;
		unsigned char * output = converted.getData();
		for(; input != end; input++) {
			*output++ = *input;
			*output++ = *input;
		}
		m_image = converted;
		m_format = Image::Format_L8A8;
		m_size = Vec2i(int(imgW), int(imgH));
		m_flags &= ~Intensity;
	}
	
	#if ARX_PLATFORM == ARX_PLATFORM_VITA
	// vitaGL doesn't properly handle GL_ALPHA format — convert A8 to L8A8
	// with luminance=0xFF (white) so GL_MODULATE produces correct vertex color
	if(getFormat() == Image::Format_A8) {
		size_t imgW = m_image.getWidth();
		size_t imgH = m_image.getHeight();
		Image converted;
		converted.create(imgW, imgH, Image::Format_L8A8);
		unsigned char * input = m_image.getData();
		unsigned char * end = input + imgW * imgH;
		unsigned char * output = converted.getData();
		for(; input != end; input++) {
			*output++ = 0xFF; // L = white
			*output++ = *input; // A = original alpha
		}
		m_image = converted;
		m_format = Image::Format_L8A8;
		m_size = Vec2i(int(imgW), int(imgH));
	}
	#endif

	if(!renderer->hasBGRTextureTransfer()
	   && (getFormat() == Image::Format_B8G8R8 || getFormat() == Image::Format_B8G8R8A8)) {
		Image::Format rgbFormat = getFormat() == Image::Format_B8G8R8 ? Image::Format_R8G8B8 : Image::Format_R8G8B8A8;
		m_image.convertTo(rgbFormat);
		m_format = rgbFormat;
	}
	
	GLint internalUnsized, internalSized;
	GLenum format;
	if(isIntensity()) {
		internalUnsized = GL_INTENSITY, internalSized = GL_INTENSITY8, format = GL_RED;
	} else if(getFormat() == Image::Format_L8) {
		internalUnsized = GL_LUMINANCE, internalSized = GL_LUMINANCE8, format = GL_LUMINANCE;
	} else if(getFormat() == Image::Format_A8) {
		internalUnsized = GL_ALPHA, internalSized = GL_ALPHA8, format = GL_ALPHA;
	} else if(getFormat() == Image::Format_L8A8) {
		internalUnsized = GL_LUMINANCE_ALPHA, internalSized = GL_LUMINANCE8_ALPHA8, format = GL_LUMINANCE_ALPHA;
	} else if(getFormat() == Image::Format_R8G8B8) {
		internalUnsized = GL_RGB, internalSized = GL_RGB8, format = GL_RGB;
	} else if(getFormat() == Image::Format_B8G8R8) {
		internalUnsized = GL_RGB, internalSized = GL_RGB8, format = GL_BGR;
	} else if(getFormat() == Image::Format_R8G8B8A8) {
		internalUnsized = GL_RGBA, internalSized = GL_RGBA8, format = GL_RGBA;
	} else if(getFormat() == Image::Format_B8G8R8A8) {
		internalUnsized = GL_RGBA, internalSized = GL_RGBA8, format = GL_BGRA;
	} else {
		arx_assert_msg(false, "Unsupported image format: %ld", long(getFormat()));
		return;
	}
	GLint internal = renderer->hasSizedTextureFormats() ? internalSized : internalUnsized;
	
	if(getStoredSize() != getSize()) {
		m_flags &= ~HasMipmaps;
	}
	
	if(hasMipmaps()) {
		glTexParameteri(GL_TEXTURE_2D, GL_GENERATE_MIPMAP, GL_TRUE);
		if(renderer->getMaxAnisotropy() > 1.f) {
			glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT, renderer->getMaxAnisotropy());
		}
	} else {
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
	}
	
	// TODO handle GL_MAX_TEXTURE_SIZE
	
	if(getStoredSize() != getSize()) {
		Image extended;
		extended.create(size_t(getStoredSize().x), size_t(getStoredSize().y), m_image.getFormat());
		extended.extendClampToEdgeBorder(m_image);
		glTexImage2D(GL_TEXTURE_2D, 0, internal, getStoredSize().x, getStoredSize().y, 0, format,
		             GL_UNSIGNED_BYTE, extended.getData());
	} else {
		glTexImage2D(GL_TEXTURE_2D, 0, internal, getSize().x, getSize().y, 0, format,
		             GL_UNSIGNED_BYTE, m_image.getData());
	}

	#if ARX_PLATFORM == ARX_PLATFORM_VITA
	{
		size_t texBytes = size_t(getStoredSize().x) * size_t(getStoredSize().y) * m_image.getNumChannels();
		if(texBytes > 256 * 1024) {
			LogInfo << "[VitaTex] " << getFileName() << " uploaded: "
			        << (texBytes / 1024) << "KB " << getStoredSize().x << "x" << getStoredSize().y;
		}
	}
	#endif

}

void GLTexture::destroy() {
	
	if(tex) {
		glDeleteTextures(1, &tex), tex = GL_NONE;
	}
	
	for(size_t i = 0; i < renderer->getTextureStageCount(); i++) {
		GLTextureStage * stage = renderer->GetTextureStage(i);
		if(stage->tex == this) {
			stage->tex = nullptr;
		}
		if(stage->current == this) {
			stage->current = nullptr;
		}
	}
	
}

static const GLint arxToGlWrapMode[] = {
	GL_REPEAT, // WrapRepeat,
	GL_MIRRORED_REPEAT, // WrapMirror
	GL_CLAMP_TO_EDGE // WrapClamp
};

static const GLint arxToGlFilter[][2] = {
	// no mipmap
	{
		GL_NEAREST, // FilterNearest
		GL_LINEAR   // FilterLinear
	},
	// mipmap
	{
		GL_NEAREST_MIPMAP_LINEAR, // FilterNearest TODO does GL_NEAREST_MIPMAP_NEAREST make more sense?
		GL_LINEAR_MIPMAP_LINEAR   // FilterLinear
	}
};

void GLTexture::apply(GLTextureStage * stage) {
	
	arx_assert(stage != nullptr);
	arx_assert(stage->tex == this);
	
	// TODO: Fix callers and change this into an assert/error/warning.
	TextureStage::WrapMode newWrapMode = (!isNPOT) ? stage->getWrapMode()
	                                               : TextureStage::WrapClamp;
	if(newWrapMode != wrapMode) {
		wrapMode = newWrapMode;
		GLint glwrap = arxToGlWrapMode[wrapMode];
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, glwrap);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, glwrap);
	}
	
	if(stage->getMinFilter() != minFilter) {
		minFilter = stage->getMinFilter();
		int mipFilter = hasMipmaps() ? 1 : 0;
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, arxToGlFilter[mipFilter][minFilter]);
	}
	
	if(stage->getMagFilter() != magFilter) {
		magFilter = stage->getMagFilter();
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, arxToGlFilter[0][magFilter]);
	}
	
}

void GLTexture::updateMaxAnisotropy() {
	
	if(hasMipmaps()) {
		glBindTexture(GL_TEXTURE_2D, tex);
		renderer->GetTextureStage(0)->current = this;
		glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT, renderer->getMaxAnisotropy());
	}
	
}
