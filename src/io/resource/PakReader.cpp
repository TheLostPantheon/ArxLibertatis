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

#include "io/resource/PakReader.h"

#include <cstring>
#include <algorithm>
#include <iomanip>
#include <ios>
#include <utility>

#include <boost/algorithm/string/predicate.hpp>

#include "io/log/Logger.h"
#include "io/Blast.h"
#include "io/resource/PakEntry.h"
#include "io/fs/FilePath.h"
#include "io/fs/Filesystem.h"
#include "io/fs/FileStream.h"
#include "platform/Platform.h"

#include "util/String.h"

namespace {

const size_t PAK_READ_BUF_SIZE = 1024;

/*!
 * Thread-safe PAK archive wrapper.
 * Stores the file path so each file handle can open its own independent
 * ifstream, avoiding all shared state between threads. The main stream
 * is used only during initial FAT parsing at load time.
 */
struct PakArchive {

	fs::path filepath;
	fs::ifstream stream; // Used only during addArchive() for FAT parsing

	PakArchive(const PakArchive &) = delete;
	PakArchive & operator=(const PakArchive &) = delete;

	explicit PakArchive(const fs::path & path)
		: filepath(path)
		, stream(path, fs::fstream::in | fs::fstream::binary) { }

	//! Open a new independent ifstream to the same PAK file
	fs::ifstream openStream() const {
		return fs::ifstream(filepath, fs::fstream::in | fs::fstream::binary);
	}

};

#if ARX_PLATFORM == ARX_PLATFORM_VITA

/*!
 * Memory-backed PAK archive.
 * Loads the entire PAK file into a contiguous memory buffer at startup.
 * All reads are served via memcpy — no file I/O, no mutex, no heap fragmentation.
 */
struct MemoryPakArchive {

	std::vector<char> data;

	MemoryPakArchive(const MemoryPakArchive &) = delete;
	MemoryPakArchive & operator=(const MemoryPakArchive &) = delete;

	explicit MemoryPakArchive(const fs::path & path) {
		u64 fileSize = fs::file_size(path);
		if(fileSize == u64(-1)) {
			return;
		}
		data.resize(size_t(fileSize));
		fs::ifstream ifs(path, fs::fstream::in | fs::fstream::binary);
		if(!ifs.is_open()) {
			data.clear();
			return;
		}
		ifs.read(data.data(), std::streamsize(fileSize));
		if(ifs.fail()) {
			data.clear();
			return;
		}
		LogInfo << "Loaded PAK into memory: " << path << " (" << (fileSize / (1024 * 1024)) << " MB)";
	}

	bool isValid() const { return !data.empty(); }

};

/*! Uncompressed file backed by a MemoryPakArchive buffer. */
class MemoryUncompressedFile : public PakFile {

	const MemoryPakArchive & m_archive;
	size_t m_offset;
	size_t m_size;

public:

	explicit MemoryUncompressedFile(const MemoryPakArchive * archive, size_t offset, size_t size)
		: m_archive(*archive), m_offset(offset), m_size(size) { }

	std::string read() const override {
		if(m_offset + m_size > m_archive.data.size()) {
			LogError << "MemoryUncompressedFile::read: out of bounds";
			return { };
		}
		return std::string(m_archive.data.data() + m_offset, m_size);
	}

	std::unique_ptr<PakFileHandle> open() const override;

	friend class MemoryUncompressedFileHandle;

};

class MemoryUncompressedFileHandle : public PakFileHandle {

	const MemoryUncompressedFile & m_file;
	size_t m_offset;

public:

	explicit MemoryUncompressedFileHandle(const MemoryUncompressedFile * file)
		: m_file(*file), m_offset(0) { }

	size_t read(void * buf, size_t size) override {
		if(m_offset >= m_file.m_size) {
			return 0;
		}
		// Validate that file region is within archive bounds
		if(m_file.m_offset + m_file.m_size > m_file.m_archive.data.size()) {
			return 0;
		}
		size_t toRead = std::min(size, m_file.m_size - m_offset);
		std::memcpy(buf, m_file.m_archive.data.data() + m_file.m_offset + m_offset, toRead);
		m_offset += toRead;
		return toRead;
	}

	int seek(Whence whence, int offset) override {
		size_t base;
		switch(whence) {
			case SeekSet: base = 0; break;
			case SeekEnd: base = m_file.m_size; break;
			case SeekCur: base = m_offset; break;
			default: return -1;
		}
		if(offset < 0) {
			size_t back = size_t(-(offset + 1)) + 1u;
			if(back > base) {
				return -1;
			}
			m_offset = base - back;
		} else {
			m_offset = base + size_t(offset);
		}
		return int(m_offset);
	}

	size_t tell() override {
		return m_offset;
	}

};

std::unique_ptr<PakFileHandle> MemoryUncompressedFile::open() const {
	return std::make_unique<MemoryUncompressedFileHandle>(this);
}

/*! Compressed file backed by a MemoryPakArchive buffer. */
class MemoryCompressedFile : public PakFile {

	const MemoryPakArchive & m_archive;
	size_t m_offset;
	size_t m_storedSize;
	size_t m_uncompressedSize;

public:

	explicit MemoryCompressedFile(const MemoryPakArchive * archive, size_t offset,
	                              size_t size, size_t storedSize)
		: m_archive(*archive), m_offset(offset)
		, m_storedSize(storedSize), m_uncompressedSize(size) { }

	std::string read() const override {
		if(m_offset + m_storedSize > m_archive.data.size()) {
			LogError << "MemoryCompressedFile::read: out of bounds";
			return { };
		}
		if(m_uncompressedSize > 64 * 1024 * 1024) {
			LogError << "MemoryCompressedFile::read: decompressed size " << m_uncompressedSize << " exceeds 64MB cap";
			return { };
		}
		std::string_view compressed(m_archive.data.data() + m_offset, m_storedSize);
		return blast(compressed, m_uncompressedSize);
	}

	std::unique_ptr<PakFileHandle> open() const override;

	friend class MemoryCompressedFileHandle;

};

class MemoryCompressedFileHandle : public PakFileHandle {

	const MemoryCompressedFile & m_file;
	size_t m_offset;

public:

	explicit MemoryCompressedFileHandle(const MemoryCompressedFile * file)
		: m_file(*file), m_offset(0) { }

	size_t read(void * buf, size_t size) override;

	int seek(Whence whence, int offset) override {
		size_t base;
		switch(whence) {
			case SeekSet: base = 0; break;
			case SeekEnd: base = m_file.m_uncompressedSize; break;
			case SeekCur: base = m_offset; break;
			default: return -1;
		}
		if(offset < 0) {
			size_t back = size_t(-(offset + 1)) + 1u;
			if(back > base) {
				return -1;
			}
			m_offset = base - back;
		} else {
			m_offset = base + size_t(offset);
		}
		return int(m_offset);
	}

	size_t tell() override {
		return m_offset;
	}

};

std::unique_ptr<PakFileHandle> MemoryCompressedFile::open() const {
	return std::make_unique<MemoryCompressedFileHandle>(this);
}

#endif // ARX_PLATFORM == ARX_PLATFORM_VITA

PakReader::ReleaseType guessReleaseType(u32 first_bytes) {
	switch(first_bytes) {
		case 0x46515641:
			return PakReader::FullGame;
		case 0x4149534E:
			return PakReader::Demo;
		default:
			return PakReader::Unknown;
	}
}

void pakDecrypt(char * fat, size_t fat_size, PakReader::ReleaseType keyId) {
	
	static const char PAK_KEY_DEMO[] = "NSIARKPRQPHBTE50GRIH3AYXJP2AMF3FCEYAVQO5Q"
		"GA0JGIIH2AYXKVOA1VOGGU5GSQKKYEOIAQG1XRX0J4F5OEAEFI4DD3LL45VJTVOA1VOGGUKE50GRI";
	static const char PAK_KEY_FULL[] = "AVQF3FCKE50GRIAYXJP2AMEYO5QGA0JGIIH2NHBTV"
		"OA1VOGGU5H3GSSIARKPRQPQKKYEOIAQG1XRX0J4F5OEAEFI4DD3LL45VJTVOA1VOGGUKE50GRIAYX";
	
	const char * key;
	size_t keysize;
	if(keyId == PakReader::FullGame) {
		key = PAK_KEY_FULL, keysize = std::size(PAK_KEY_FULL) - 1;
	} else {
		key = PAK_KEY_DEMO, keysize = std::size(PAK_KEY_DEMO) - 1;
	}
	
	for(size_t i = 0, ki = 0; i < fat_size; i++, ki = (ki + 1) % keysize) {
		fat[i] ^= key[ki];
	}
	
}

/*! Uncompressed file in a .pak file archive. */
class UncompressedFile : public PakFile {

	const PakArchive & m_archive;
	size_t m_offset;
	size_t m_size;

public:

	explicit UncompressedFile(const PakArchive * archive, size_t offset, size_t size)
		: m_archive(*archive), m_offset(offset), m_size(size) { }

	std::string read() const override;

	std::unique_ptr<PakFileHandle> open() const override;

	friend class UncompressedFileHandle;

};

class UncompressedFileHandle : public PakFileHandle {

	const UncompressedFile & m_file;
	fs::ifstream m_stream; // Own stream — no shared state between threads
	size_t m_offset;

public:

	explicit UncompressedFileHandle(const UncompressedFile * file)
		: m_file(*file)
		, m_stream(file->m_archive.openStream())
		, m_offset(0) { }

	size_t read(void * buf, size_t size) override;

	int seek(Whence whence, int offset) override;

	size_t tell() override;

};

std::string UncompressedFile::read() const {

	// Open a temporary stream for this read — fully thread-safe
	#if ARX_PLATFORM == ARX_PLATFORM_VITA
	// Heap-allocate ifstream to reduce stack pressure during level loading
	std::unique_ptr<fs::ifstream> sp(new fs::ifstream(m_archive.filepath,
	                                 fs::fstream::in | fs::fstream::binary));
	fs::ifstream & stream = *sp;
	#else
	fs::ifstream stream = m_archive.openStream();
	#endif

	stream.seekg(m_offset);

	std::string buffer;

	buffer.resize(m_size);
	fs::read(stream, buffer.data(), m_size);
	if(stream.fail()) {
		LogError << "Error reading from PAK archive";
		buffer.clear();
	}

	return buffer;
}

std::unique_ptr<PakFileHandle> UncompressedFile::open() const {
	return std::make_unique<UncompressedFileHandle>(this);
}

size_t UncompressedFileHandle::read(void * buf, size_t size) {

	if(m_offset >= m_file.m_size) {
		return 0;
	}

	if(!m_stream.is_open()) {
		return 0;
	}

	m_stream.seekg(m_file.m_offset + m_offset);

	fs::read(m_stream, buf, std::min(size, m_file.m_size - m_offset));

	size_t nread = m_stream.gcount();
	m_offset += nread;

	m_stream.clear();

	return nread;
}

int UncompressedFileHandle::seek(Whence whence, int offset) {

	size_t base;
	switch(whence) {
		case SeekSet: base = 0; break;
		case SeekEnd: base = m_file.m_size; break;
		case SeekCur: base = m_offset; break;
		default: return -1;
	}

	if(offset < 0) {
		size_t back = size_t(-(offset + 1)) + 1u;
		if(back > base) {
			return -1;
		}
		m_offset = base - back;
	} else {
		m_offset = base + size_t(offset);
	}

	return int(m_offset);
}

size_t UncompressedFileHandle::tell() {
	return m_offset;
}

/*! Compressed file in a .pak file archive. */
class CompressedFile : public PakFile {

	const PakArchive & m_archive;
	size_t m_offset;
	size_t m_storedSize;
	size_t m_uncompressedSize;

public:

	explicit CompressedFile(const PakArchive * archive, size_t offset, size_t size, size_t storedSize)
		:  m_archive(*archive), m_offset(offset), m_storedSize(storedSize), m_uncompressedSize(size) { }

	std::string read() const override;

	std::unique_ptr<PakFileHandle> open() const override;

	friend class CompressedFileHandle;

};

class CompressedFileHandle : public PakFileHandle {

	const CompressedFile & m_file;
	fs::ifstream m_stream; // Own stream — no shared state between threads
	size_t m_offset;

public:

	explicit CompressedFileHandle(const CompressedFile * file)
		: m_file(*file)
		, m_stream(file->m_archive.openStream())
		, m_offset(0) { }

	size_t read(void * buf, size_t size) override;

	int seek(Whence whence, int offset) override;

	size_t tell() override;

};

struct BlastFileInBuffer {

	fs::ifstream & file;
	size_t remaining;

	unsigned char readbuf[PAK_READ_BUF_SIZE];

	BlastFileInBuffer(const BlastFileInBuffer &) = delete;
	BlastFileInBuffer & operator=(const BlastFileInBuffer &) = delete;

	explicit BlastFileInBuffer(fs::ifstream & f, size_t count)
		: file(f), remaining(count) { }

};

size_t blastInFile(void * Param, const unsigned char ** buf) {
	
	BlastFileInBuffer * p = static_cast<BlastFileInBuffer *>(Param);
	
	*buf = p->readbuf;
	
	size_t count = std::min(p->remaining, std::size(p->readbuf));
	p->remaining -= count;
	
	return fs::read(p->file, p->readbuf, count).gcount();
}

std::string CompressedFile::read() const {

	// Open a temporary stream for this read — fully thread-safe
	#if ARX_PLATFORM == ARX_PLATFORM_VITA
	// Heap-allocate ifstream to reduce stack pressure during level loading
	std::unique_ptr<fs::ifstream> sp(new fs::ifstream(m_archive.filepath,
	                                 fs::fstream::in | fs::fstream::binary));
	fs::ifstream & stream = *sp;
	#else
	fs::ifstream stream = m_archive.openStream();
	#endif

	stream.seekg(m_offset);

	std::string buffer;

	buffer.resize(m_storedSize);
	fs::read(stream, buffer.data(), m_storedSize);
	if(stream.fail()) {
		LogError << "Error reading from PAK archive";
		buffer.clear();
	}

	if(buffer.empty()) {
		return buffer;
	}

	if(m_uncompressedSize > 64 * 1024 * 1024) {
		LogError << "CompressedFile::read: decompressed size " << m_uncompressedSize << " exceeds 64MB cap";
		return { };
	}
	return blast(buffer, m_uncompressedSize);
}

std::unique_ptr<PakFileHandle> CompressedFile::open() const {
	return std::make_unique<CompressedFileHandle>(this);
}

struct BlastMemOutBufferOffset {
	char * buf;
	size_t currentOffset;
	size_t startOffset;
	size_t endOffset;
};

int blastOutMemOffset(void * Param, unsigned char * buf, size_t len) {
	
	BlastMemOutBufferOffset * p = static_cast<BlastMemOutBufferOffset *>(Param);
	
	arx_assert(p->currentOffset <= p->endOffset);
	
	if(p->currentOffset == p->endOffset) {
		return 1;
	}
	
	if(p->currentOffset < p->startOffset) {
		size_t toStart = p->startOffset - p->currentOffset;
		if(len <= toStart) {
			p->currentOffset += len;
			return 0;
		} else {
			p->currentOffset = p->startOffset;
			buf += toStart;
			len -= toStart;
		}
	}
	
	size_t toCopy = std::min(len, p->endOffset - p->currentOffset);
	
	arx_assert(toCopy != 0);
	
	memcpy(p->buf, buf, toCopy);
	
	p->currentOffset += toCopy;
	p->buf += toCopy;
	
	return 0;
}

#if ARX_PLATFORM == ARX_PLATFORM_VITA

size_t MemoryCompressedFileHandle::read(void * buf, size_t size) {

	if(m_offset >= m_file.m_uncompressedSize) {
		return 0;
	}

	if(size < m_file.m_uncompressedSize || m_offset != 0) {
		LogWarning << "Partially reading a memory-compressed file - inefficient: size=" << size
		           << " offset=" << m_offset << " total=" << m_file.m_uncompressedSize;
	}

	// Validate that compressed data region is within archive bounds
	if(m_file.m_offset + m_file.m_storedSize > m_file.m_archive.data.size()) {
		LogError << "MemoryCompressedFileHandle::read: compressed data out of bounds";
		return 0;
	}

	const char * compressed = m_file.m_archive.data.data() + m_file.m_offset;

	BlastMemInBuffer in(compressed, m_file.m_storedSize);
	BlastMemOutBufferOffset out;
	out.buf = reinterpret_cast<char *>(buf);
	out.currentOffset = 0;
	out.startOffset = m_offset;
	out.endOffset = std::min(m_offset + size, m_file.m_uncompressedSize);

	if(out.endOffset <= out.startOffset) {
		return 0;
	}

	int r = ::blast(blastInMem, &in, blastOutMemOffset, &out);
	if(r && (r != 1 || (size == m_file.m_uncompressedSize && m_offset == 0))) {
		LogError << "MemoryCompressedFileHandle::read: blast error " << r;
		return 0;
	}

	size = out.currentOffset - out.startOffset;
	m_offset += size;
	return size;
}

#endif // ARX_PLATFORM == ARX_PLATFORM_VITA

size_t CompressedFileHandle::read(void * buf, size_t size) {

	if(m_offset >= m_file.m_uncompressedSize) {
		return 0;
	}

	if(!m_stream.is_open()) {
		return 0;
	}

	if(size < m_file.m_uncompressedSize || m_offset != 0) {
		LogWarning << "Partially reading a compressed file - inefficient: size=" << size
		           << " offset=" << m_offset << " total=" << m_file.m_uncompressedSize;
	}

	m_stream.seekg(m_file.m_offset);

	BlastFileInBuffer in(m_stream, m_file.m_storedSize);
	BlastMemOutBufferOffset out;

	out.buf = reinterpret_cast<char *>(buf);
	out.currentOffset = 0;
	out.startOffset = m_offset;
	out.endOffset = std::min(m_offset + size, m_file.m_uncompressedSize);

	if(out.endOffset <= out.startOffset) {
		return 0;
	}

	// TODO this is really inefficient
	int r = ::blast(blastInFile, &in, blastOutMemOffset, &out);
	if(r && (r != 1 || (size == m_file.m_uncompressedSize && m_offset == 0))) {
		LogError << "PakReader::fRead: blast error " << r << " outSize=" << m_file.m_uncompressedSize;
		return 0;
	}

	size = out.currentOffset - out.startOffset;

	m_offset += size;

	m_stream.clear();

	return size;
}

int CompressedFileHandle::seek(Whence whence, int offset) {

	size_t base;
	switch(whence) {
		case SeekSet: base = 0; break;
		case SeekEnd: base = m_file.m_uncompressedSize; break;
		case SeekCur: base = m_offset; break;
		default: return -1;
	}

	if(offset < 0) {
		size_t back = size_t(-(offset + 1)) + 1u;
		if(back > base) {
			return -1;
		}
		m_offset = base - back;
	} else {
		m_offset = base + size_t(offset);
	}

	return int(m_offset);
}

size_t CompressedFileHandle::tell() {
	return m_offset;
}

/*! Plain file not in a .pak file archive. */
class PlainFile : public PakFile {
	
	fs::path m_path;
	
public:
	
	explicit PlainFile(fs::path path) : m_path(std::move(path)) { }
	
	std::string read() const override;
	
	std::unique_ptr<PakFileHandle> open() const override;
	
};

class PlainFileHandle : public PakFileHandle {
	
	fs::ifstream ifs;
	
public:
	
	explicit  PlainFileHandle(const fs::path & path)
		: ifs(path, fs::fstream::in | fs::fstream::binary)
	{ }
	
	size_t read(void * buf, size_t size) override;
	
	int seek(Whence whence, int offset) override;
	
	size_t tell() override;
	
};

std::string PlainFile::read() const {
	return fs::read(m_path);
}

std::unique_ptr<PakFileHandle> PlainFile::open() const {
	return std::make_unique<PlainFileHandle>(m_path);
}

size_t PlainFileHandle::read(void * buf, size_t size) {
	return fs::read(ifs, buf, size).gcount();
}

std::ios_base::seekdir arxToStlSeekOrigin(Whence whence) {
	switch(whence) {
		case SeekSet: return std::ios_base::beg;
		case SeekCur: return std::ios_base::cur;
		case SeekEnd: return std::ios_base::end;
	}
	arx_unreachable();
}

int PlainFileHandle::seek(Whence whence, int offset) {
	return ifs.seekg(offset, arxToStlSeekOrigin(whence)).tellg();
}

size_t PlainFileHandle::tell() {
	return ifs.tellg();
}

} // anonymous namespace

PakReader::~PakReader() {
	clear();
}

bool PakReader::addArchive(const fs::path & pakfile, const PakFilter * filter) {

#if ARX_PLATFORM == ARX_PLATFORM_VITA
	// On Vita, load small PAKs entirely into memory to eliminate ifstream heap fragmentation.
	// Keep this low — SFX.pak (~43MB) must stay file-backed to avoid OOM.
	static const size_t VITA_MEMORY_PAK_THRESHOLD = 16 * 1024 * 1024; // 16 MB
	u64 pakFileSize = fs::file_size(pakfile);
	if(pakFileSize != u64(-1) && pakFileSize <= VITA_MEMORY_PAK_THRESHOLD) {
		return addArchiveMemory(pakfile, filter);
	}
#endif

	return addArchiveStreamed(pakfile, filter);
}

bool PakReader::addArchiveStreamed(const fs::path & pakfile, const PakFilter * filter) {

	PakArchive * archive = new PakArchive(pakfile);

	if(!archive->stream.is_open()) {
		delete archive;
		return false;
	}

	fs::ifstream & ifs = archive->stream;

	// Read fat location and size.
	u32 fat_offset;
	u32 fat_size;

	if(fs::read(ifs, fat_offset).fail()) {
		LogError << pakfile << ": error reading FAT offset";
		delete archive;
		return false;
	}
	if(ifs.seekg(fat_offset).fail()) {
		LogError << pakfile << ": error seeking to FAT offset " << fat_offset;
		delete archive;
		return false;
	}
	if(fs::read(ifs, fat_size).fail()) {
		LogError << pakfile << ": error reading FAT size at offset " << fat_offset;
		delete archive;
		return false;
	}

	// Read the whole FAT.
	std::vector<char> fat(fat_size);
	if(ifs.read(fat.data(), fat_size).fail()) {
		LogError << pakfile << ": error reading FAT at " << fat_offset
		         << " with size " << fat_size;
		delete archive;
		return false;
	}

	// Decrypt the FAT.
	ReleaseType key = guessReleaseType(*reinterpret_cast<const u32 *>(fat.data()));
	if(key != Unknown) {
		pakDecrypt(fat.data(), fat_size, key);
	} else {
		LogWarning << pakfile << ": unknown PAK key ID 0x" << std::hex << std::setfill('0')
		           << std::setw(8) << *reinterpret_cast<u32 *>(fat.data()) << ", assuming no key";
	}
	release |= key;

	util::md5::checksum checksum = util::md5::compute(fat.data(), fat_size);
	if(!empty()) {
		m_checksum = util::md5::checksum();
	} else {
		m_checksum = checksum;
	}

	const std::vector<std::string_view> * filters = nullptr;
	if(filter) {
		if(auto it = filter->find(checksum); it != filter->end()) {
			filters = &it->second;
		}
	}

	char * pos = fat.data();

	paks.push_back(archive);

	while(fat_size) {

		char * dirname = util::safeGetString(pos, fat_size);
		if(!dirname) {
			LogError << pakfile << ": error reading directory name from FAT, wrong key?";
			return false;
		}

		PakDirectory * dir = nullptr;
		res::path dirpath = res::path::load(dirname);
		bool filtered = false;
		if(filters) {
			for(std::string_view exclude : *filters) {
				if(boost::starts_with(dirpath.string(), exclude)
				   && (dirpath.string().length() == exclude.length() || dirpath.string()[exclude.length()] == '/')) {
					LogInfo << pakfile << ": ignoring " << dirpath;
					filtered = true;
					break;
				}
			}
		}
		if(!filtered) {
			dir = addDirectory(dirpath);
		}

		u32 nfiles;
		if(!util::safeGet(nfiles, pos, fat_size)) {
			LogError << pakfile << ": error reading file count from FAT, wrong key?";
			return false;
		}

		while(nfiles--) {

			char * filename =  util::safeGetString(pos, fat_size);
			if(!filename) {
				LogError << pakfile << ": error reading file name from FAT, wrong key?";
				return false;
			}

			u32 offset;
			u32 flags;
			u32 uncompressedSize;
			u32 size;
			if(!util::safeGet(offset, pos, fat_size) || !util::safeGet(flags, pos, fat_size)
			   || !util::safeGet(uncompressedSize, pos, fat_size)
				 || !util::safeGet(size, pos, fat_size)) {
				LogError << pakfile << ": error reading file attributes from FAT, wrong key?";
				return false;
			}

			if(!dir) {
				continue;
			}

			const u32 PAK_FILE_COMPRESSED = 1;
			std::unique_ptr<PakFile> file;
			if((flags & PAK_FILE_COMPRESSED) && size != 0) {
				file = std::make_unique<CompressedFile>(archive, offset, uncompressedSize, size);
			} else {
				file = std::make_unique<UncompressedFile>(archive, offset, size);
			}

			dir->addFile(util::toLowercase(filename), std::move(file));
		}

	}

	LogInfo << "Loaded PAK " << pakfile;
	return true;

}

#if ARX_PLATFORM == ARX_PLATFORM_VITA

bool PakReader::addArchiveMemory(const fs::path & pakfile, const PakFilter * filter) {

	MemoryPakArchive * archive = new MemoryPakArchive(pakfile);

	if(!archive->isValid()) {
		delete archive;
		return false;
	}

	const char * data = archive->data.data();
	size_t dataSize = archive->data.size();

	if(dataSize < sizeof(u32)) {
		LogError << pakfile << ": file too small for FAT offset";
		delete archive;
		return false;
	}

	// Read fat location and size from memory buffer.
	u32 fat_offset;
	std::memcpy(&fat_offset, data, sizeof(u32));

	if(fat_offset + sizeof(u32) > dataSize) {
		LogError << pakfile << ": FAT offset " << fat_offset << " out of bounds";
		delete archive;
		return false;
	}

	u32 fat_size;
	std::memcpy(&fat_size, data + fat_offset, sizeof(u32));

	if(fat_offset + sizeof(u32) + fat_size > dataSize) {
		LogError << pakfile << ": FAT at " << fat_offset << " with size " << fat_size << " out of bounds";
		delete archive;
		return false;
	}

	// Copy FAT for decryption (need mutable copy).
	std::vector<char> fat(fat_size);
	std::memcpy(fat.data(), data + fat_offset + sizeof(u32), fat_size);

	// Decrypt the FAT.
	ReleaseType key = guessReleaseType(*reinterpret_cast<const u32 *>(fat.data()));
	if(key != Unknown) {
		pakDecrypt(fat.data(), fat_size, key);
	} else {
		LogWarning << pakfile << ": unknown PAK key ID 0x" << std::hex << std::setfill('0')
		           << std::setw(8) << *reinterpret_cast<u32 *>(fat.data()) << ", assuming no key";
	}
	release |= key;

	util::md5::checksum checksum = util::md5::compute(fat.data(), fat_size);
	if(!empty()) {
		m_checksum = util::md5::checksum();
	} else {
		m_checksum = checksum;
	}

	const std::vector<std::string_view> * filters = nullptr;
	if(filter) {
		if(auto it = filter->find(checksum); it != filter->end()) {
			filters = &it->second;
		}
	}

	char * pos = fat.data();

	memoryPaks.push_back(archive);

	while(fat_size) {

		char * dirname = util::safeGetString(pos, fat_size);
		if(!dirname) {
			LogError << pakfile << ": error reading directory name from FAT, wrong key?";
			return false;
		}

		PakDirectory * dir = nullptr;
		res::path dirpath = res::path::load(dirname);
		bool filtered = false;
		if(filters) {
			for(std::string_view exclude : *filters) {
				if(boost::starts_with(dirpath.string(), exclude)
				   && (dirpath.string().length() == exclude.length() || dirpath.string()[exclude.length()] == '/')) {
					LogInfo << pakfile << ": ignoring " << dirpath;
					filtered = true;
					break;
				}
			}
		}
		if(!filtered) {
			dir = addDirectory(dirpath);
		}

		u32 nfiles;
		if(!util::safeGet(nfiles, pos, fat_size)) {
			LogError << pakfile << ": error reading file count from FAT, wrong key?";
			return false;
		}

		while(nfiles--) {

			char * filename = util::safeGetString(pos, fat_size);
			if(!filename) {
				LogError << pakfile << ": error reading file name from FAT, wrong key?";
				return false;
			}

			u32 offset;
			u32 flags;
			u32 uncompressedSize;
			u32 size;
			if(!util::safeGet(offset, pos, fat_size) || !util::safeGet(flags, pos, fat_size)
			   || !util::safeGet(uncompressedSize, pos, fat_size)
			   || !util::safeGet(size, pos, fat_size)) {
				LogError << pakfile << ": error reading file attributes from FAT, wrong key?";
				return false;
			}

			if(!dir) {
				continue;
			}

			const u32 PAK_FILE_COMPRESSED = 1;
			std::unique_ptr<PakFile> file;
			if((flags & PAK_FILE_COMPRESSED) && size != 0) {
				file = std::make_unique<MemoryCompressedFile>(archive, offset, uncompressedSize, size);
			} else {
				file = std::make_unique<MemoryUncompressedFile>(archive, offset, size);
			}

			dir->addFile(util::toLowercase(filename), std::move(file));
		}

	}

	LogInfo << "Loaded PAK " << pakfile << " (memory-backed)";
	return true;

}

#endif // ARX_PLATFORM == ARX_PLATFORM_VITA

void PakReader::clear() {

	release = 0;

	m_files.clear();
	m_dirs.clear();

	for(void * p : paks) {
		delete static_cast<PakArchive *>(p);
	}
	paks.clear();

	#if ARX_PLATFORM == ARX_PLATFORM_VITA
	for(void * p : memoryPaks) {
		delete static_cast<MemoryPakArchive *>(p);
	}
	memoryPaks.clear();
	#endif
}

std::string PakReader::read(const res::path & name) {
	
	PakFile * f = getFile(name);
	if(!f) {
		return std::string();
	}
	
	return f->read();
}

std::unique_ptr<PakFileHandle> PakReader::open(const res::path & name) {
	
	PakFile * f = getFile(name);
	if(!f) {
		return { };
	}
	
	return f->open();
}

bool PakReader::addFiles(const fs::path & path, const res::path & mount) {
	
	m_checksum = util::md5::checksum();
	
	fs::FileType type = fs::get_type(path);
	
	if(type == fs::Directory) {
		
		bool ret = addFiles(addDirectory(mount), path);
		
		if(ret) {
			release |= External;
			LogInfo << "Added dir " << path;
		}
		
		return ret;
		
	} else if(type == fs::RegularFile && !mount.empty()) {
		
		PakDirectory * dir = addDirectory(mount.parent());
		
		return addFile(dir, path, std::string(mount.filename()));
		
	}
	
	return false;
}

void PakReader::removeFile(const res::path & file) {
	
	PakDirectory * dir = getDirectory(file.parent());
	if(dir) {
		dir->removeFile(file.filename());
	}
}

bool PakReader::removeDirectory(const res::path & name) {
	
	PakDirectory * pdir = getDirectory(name.parent());
	if(pdir) {
		return pdir->removeDirectory(name.filename());
	} else {
		return true;
	}
}

bool PakReader::addFile(PakDirectory * dir, fs::path path, std::string name) {
	
	if(name.empty()) {
		return false;
	}
	
	dir->addFile(std::move(name), std::make_unique<PlainFile>(std::move(path)));
	return true;
}

bool PakReader::addFiles(PakDirectory * dir, const fs::path & path) {
	
	bool ret = true;
	
	for(fs::directory_iterator it(path); !it.end(); ++it) {
		
		std::string name = it.name();
		
		if(name.empty() || name[0] == '.') {
			// Ignore
			continue;
		}
		
		fs::path entry = path / name;
		
		util::makeLowercase(name);
		
		fs::FileType type = it.type();
		
		if(type == fs::Directory) {
			ret &= addFiles(dir->addDirectory(std::move(name)), entry);
		} else if(type == fs::RegularFile) {
			ret &= addFile(dir, std::move(entry), std::move(name));
		}
		
	}
	
	return ret;
}

PakReader * g_resources;
