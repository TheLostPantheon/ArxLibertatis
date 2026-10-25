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
 *
 * Based on:
 * 
 * blast.c
 * Copyright (C) 2003 Mark Adler
 * For conditions of distribution and use, see copyright notice in Blast.h
 * version 1.1, 16 Feb 2003
 *
 * blast.c decompresses data compressed by the PKWare Compression Library.
 * This function provides functionality similar to the explode() function of
 * the PKWare library, hence the name "blast".
 *
 * This decompressor is based on the excellent format description provided by
 * Ben Rudiak-Gould in comp.compression on August 13, 2001.  Interestingly, the
 * example Ben provided in the post is incorrect.  The distance 110001 should
 * instead be 111000.  When corrected, the example byte stream becomes:
 *
 *    00 04 82 24 25 8f 80 7f
 *
 * which decompresses to "AIAIAIAIAIAIA" (without the quotes).
 */

#include "io/Blast.h"

#include <cstring>
#include <cstdlib>
#include <exception>
#include <memory>

#include "io/log/Logger.h"
#include "platform/Platform.h"
#include "platform/vita/VitaBench.h"

#if ARX_PLATFORM == ARX_PLATFORM_VITA
#include <psp2/io/stat.h>
#endif

#define MAXBITS 13              /* maximum code length */
#define MAXWIN 4096             /* maximum window size */

namespace {

struct blast_truncated_error : public std::exception { };

} // anonymous namespace

/* input and output state */
struct state {
	
	/* input state */
	blast_in infun;             /* input function provided by user */
	void * inhow;               /* opaque information passed to infun() */
	const unsigned char * in;   /* next input location */
	unsigned left;              /* available input at in */
	int bitbuf;                 /* bit buffer */
	int bitcnt;                 /* number of bits in bit buffer */
	
	/* output state */
	blast_out outfun;           /* output function provided by user */
	void * outhow;              /* opaque information passed to outfun() */
	unsigned next;              /* index of next write location in out[] */
	int first;                  /* true to check distances (for first 4K) */
	unsigned char out[MAXWIN];  /* output buffer and sliding window */
	
};

/*
 * Return need bits from the input stream.  This always leaves less than
 * eight bits in the buffer.  bits() works properly for need == 0.
 *
 * Format notes:
 *
 * - Bits are stored in bytes from the least significant bit to the most
 *   significant bit.  Therefore bits are dropped from the bottom of the bit
 *   buffer, using shift right, and new bytes are appended to the top of the
 *   bit buffer, using shift left.
 */
static int bits(state * s, int need) {
	
	int val;            /* bit accumulator */
	
	/* load at least need bits into val */
	val = s->bitbuf;
	while(s->bitcnt < need) {
		if(s->left == 0) {
			s->left = s->infun(s->inhow, &(s->in));
			if(s->left == 0) {
				throw blast_truncated_error(); /* out of input */
			}
		}
		val |= int(*(s->in)++) << s->bitcnt; /* load eight bits */
		s->left--;
		s->bitcnt += 8;
	}
	
	/* drop need bits and update buffer, always zero to seven bits left */
	s->bitbuf = val >> need;
	s->bitcnt -= need;
	
	/* return need bits, zeroing the bits above that */
	return val & ((1 << need) - 1);
}

/*
 * Huffman code decoding tables.  count[1..MAXBITS] is the number of symbols of
 * each length, which for a canonical code are stepped through in order.
 * symbol[] are the symbol values in canonical order, where the number of
 * entries is the sum of the counts in count[].  The decoding process can be
 * seen in the function decode() below.
 */
struct huffman {
	short * count;       /* number of symbols of each length */
	short * symbol;      /* canonically ordered symbols */
};

/*
 * Decode a code from the stream s using huffman table h.  Return the symbol or
 * a negative value if there is an error.  If all of the lengths are zero, i.e.
 * an empty code, or if the code is incomplete and an invalid code is received,
 * then -9 is returned after reading MAXBITS bits.
 *
 * Format notes:
 *
 * - The codes as stored in the compressed data are bit-reversed relative to
 *   a simple integer ordering of codes of the same lengths.  Hence below the
 *   bits are pulled from the compressed data one at a time and used to
 *   build the code value reversed from what is in the stream in order to
 *   permit simple integer comparisons for decoding.
 *
 * - The first code for the shortest length is all ones.  Subsequent codes of
 *   the same length are simply integer decrements of the previous code.  When
 *   moving up a length, a one bit is appended to the code.  For a complete
 *   code, the last code of the longest length will be all zeros.  To support
 *   this ordering, the bits pulled during decoding are inverted to apply the
 *   more "natural" ordering starting with all zeros and incrementing.
 */
static int decode(state * s, huffman * h) {
	
	int len;            /* current number of bits in code */
	int code;           /* len bits being decoded */
	int first;          /* first code of length len */
	int count;          /* number of codes of length len */
	int index;          /* index of first code of length len in symbol table */
	int bitbuf;         /* bits from stream */
	int left;           /* bits left in next or left to process */
	short * next;        /* next number of codes */
	
	bitbuf = s->bitbuf;
	left = s->bitcnt;
	code = first = index = 0;
	len = 1;
	next = h->count + 1;
	while(true) {
		while(left--) {
			code |= (bitbuf & 1) ^ 1; /* invert code */
			bitbuf >>= 1;
			count = *next++;
			if(code < first + count) { /* if length len, return symbol */
				s->bitbuf = bitbuf;
				s->bitcnt = (s->bitcnt - len) & 7;
				return h->symbol[index + (code - first)];
			}
			index += count; /* else update for next length */
			first += count;
			first <<= 1;
			code <<= 1;
			len++;
		}
		left = (MAXBITS + 1) - len;
		if(left == 0) {
			break;
		}
		if(s->left == 0) {
			s->left = s->infun(s->inhow, &(s->in));
			if(s->left == 0) {
				throw blast_truncated_error(); /* out of input */
			}
		}
		bitbuf = *(s->in)++;
		s->left--;
		if(left > 8) {
			left = 8;
		}
	}
	return -9; /* ran out of codes */
}

/*
 * Given a list of repeated code lengths rep[0..n-1], where each byte is a
 * count (high four bits + 1) and a code length (low four bits), generate the
 * list of code lengths.  This compaction reduces the size of the object code.
 * Then given the list of code lengths length[0..n-1] representing a canonical
 * Huffman code for n symbols, construct the tables required to decode those
 * codes.  Those tables are the number of codes of each length, and the symbols
 * sorted by length, retaining their original order within each length.  The
 * return value is zero for a complete code set, negative for an over-
 * subscribed code set, and positive for an incomplete code set.  The tables
 * can be used if the return value is zero or positive, but they cannot be used
 * if the return value is negative.  If the return value is zero, it is not
 * possible for decode() using that table to return an error--any stream of
 * enough bits will resolve to a symbol.  If the return value is positive, then
 * it is possible for decode() using that table to return an error for received
 * codes past the end of the incomplete lengths.
 */
static int construct(huffman * h, const unsigned char * rep, int n) {
	
	int symbol;         /* current symbol when stepping through length[] */
	int len;            /* current length when stepping through h->count[] */
	int left;           /* number of possible codes left of current length */
	short offs[MAXBITS + 1];      /* offsets in symbol table for each length */
	short length[256];  /* code lengths */
	
	/* convert compact repeat counts into symbol bit length list */
	symbol = 0;
	do {
		len = *rep++;
		left = (len >> 4) + 1;
		len &= 15;
		do {
			length[symbol++] = len;
		} while(--left);
	} while(--n);
	n = symbol;
	
	/* count number of codes of each length */
	for(len = 0; len <= MAXBITS; len++) {
		h->count[len] = 0;
	}
	for(symbol = 0; symbol < n; symbol++) {
		(h->count[length[symbol]])++; /* assumes lengths are within bounds */
	}
	if(h->count[0] == n) { /* no codes! */
		return 0; /* complete, but decode() will fail */
	}
	
	/* check for an over-subscribed or incomplete set of lengths */
	left = 1;                           /* one possible code of zero length */
	for(len = 1; len <= MAXBITS; len++) {
		left <<= 1;                     /* one more bit, double codes left */
		left -= h->count[len];          /* deduct count from possible codes */
		if(left < 0) {
			return left; /* over-subscribed--return negative */
		}
	}                                   /* left > 0 means incomplete */
	
	/* generate offsets into symbol table for each length for sorting */
	offs[1] = 0;
	for(len = 1; len < MAXBITS; len++) {
		offs[len + 1] = offs[len] + h->count[len];
	}
	
	/*
	 * put symbols in table sorted by length, by symbol order within each
	 * length
	 */
	for(symbol = 0; symbol < n; symbol++) {
		if(length[symbol] != 0) {
			h->symbol[offs[length[symbol]]++] = symbol;
		}
	}
	
	/* return zero for complete set, positive for incomplete set */
	return left;
}


/* The PKWare fixed codes, built once */
static short litcnt[MAXBITS + 1], litsym[256];        /* litcode memory */
static short lencnt[MAXBITS + 1], lensym[16];         /* lencode memory */
static short distcnt[MAXBITS + 1], distsym[64];       /* distcode memory */
static huffman litcode = {litcnt, litsym};   /* length code */
static huffman lencode = {lencnt, lensym};   /* length code */
static huffman distcode = {distcnt, distsym};/* distance code */
static const short base[16] = {     /* base for length codes */
	3, 2, 4, 5, 6, 7, 8, 9, 10, 12, 16, 24, 40, 72, 136, 264
};
static const char extra[16] = {     /* extra bits for length codes */
	0, 0, 0, 0, 0, 0, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8
};

static void initCodes() {
	static int virgin = 1;
	/* bit lengths of literal codes */
	static const unsigned char litlen[] = {
		11, 124, 8, 7, 28, 7, 188, 13, 76, 4, 10, 8, 12, 10, 12, 10, 8, 23, 8,
		9, 7, 6, 7, 8, 7, 6, 55, 8, 23, 24, 12, 11, 7, 9, 11, 12, 6, 7, 22, 5,
		7, 24, 6, 11, 9, 6, 7, 22, 7, 11, 38, 7, 9, 8, 25, 11, 8, 11, 9, 12,
		8, 12, 5, 38, 5, 38, 5, 11, 7, 5, 6, 21, 6, 10, 53, 8, 7, 24, 10, 27,
		44, 253, 253, 253, 252, 252, 252, 13, 12, 45, 12, 45, 12, 61, 12, 45,
		44, 173
	};
	/* bit lengths of length codes 0..15 */
	static const unsigned char lenlen[] = {2, 35, 36, 53, 38, 23};
	/* bit lengths of distance codes 0..63 */
	static const unsigned char distlen[] = {2, 20, 53, 230, 247, 151, 248};
	/* set up decoding tables (once--might not be thread-safe) */
	if(virgin) {
		construct(&litcode, litlen, sizeof(litlen));
		construct(&lencode, lenlen, sizeof(lenlen));
		construct(&distcode, distlen, sizeof(distlen));
		virgin = 0;
	}
}

/*
 * Decode PKWare Compression Library stream.
 *
 * Format notes:
 *
 * - First byte is 0 if literals are uncoded or 1 if they are coded.  Second
 *   byte is 4, 5, or 6 for the number of extra bits in the distance code.
 *   This is the base-2 logarithm of the dictionary size minus six.
 *
 * - Compressed data is a combination of literals and length/distance pairs
 *   terminated by an end code.  Literals are either Huffman coded or
 *   uncoded bytes.  A length/distance pair is a coded length followed by a
 *   coded distance to represent a string that occurs earlier in the
 *   uncompressed data that occurs again at the current location.
 *
 * - A bit preceding a literal or length/distance pair indicates which comes
 *   next, 0 for literals, 1 for length/distance.
 *
 * - If literals are uncoded, then the next eight bits are the literal, in the
 *   normal bit order in th stream, i.e. no bit-reversal is needed. Similarly,
 *   no bit reversal is needed for either the length extra bits or the distance
 *   extra bits.
 *
 * - Literal bytes are simply written to the output.  A length/distance pair is
 *   an instruction to copy previously uncompressed bytes to the output.  The
 *   copy is from distance bytes back in the output stream, copying for length
 *   bytes.
 *
 * - Distances pointing before the beginning of the output data are not
 *   permitted.
 *
 * - Overlapped copies, where the length is greater than the distance, are
 *   allowed and common.  For example, a distance of one and a length of 518
 *   simply copies the last byte 518 times.  A distance of four and a length of
 *   twelve copies the last four bytes three times.  A simple forward copy
 *   ignoring whether the length is greater than the distance or not implements
 *   this correctly.
 */
static BlastResult blastDecompress(state * s) {
	
	int lit;            /* true if literals are coded */
	int dict;           /* log2(dictionary size) - 6 */
	int symbol;         /* decoded symbol, extra bits for distance */
	int len;            /* length for copy */
	int dist;           /* distance for copy */
	int copy;           /* copy counter */
	unsigned char * from, *to;   /* copy pointers */
	initCodes();
	
	/* read header */
	lit = bits(s, 8);
	if(lit > 1) {
		return BLAST_INVALID_LITERAL_FLAG;
	}
	dict = bits(s, 8);
	if(dict < 4 || dict > 6) {
		return BLAST_INVALID_DIC_SIZE;
	}
	
	/* decode literals and length/distance pairs */
	do {
		if(bits(s, 1)) {
			/* get length */
			symbol = decode(s, &lencode);
			len = base[symbol] + bits(s, extra[symbol]);
			if(len == 519) {
				break; /* end code */
			}
			
			/* get distance */
			symbol = len == 2 ? 2 : dict;
			dist = decode(s, &distcode) << symbol;
			dist += bits(s, symbol);
			dist++;
			if(s->first && dist > int(s->next)) {
				return BLAST_INVALID_OFFSET;
			}
			
			/* copy length bytes from distance bytes back */
			do {
				to = s->out + s->next;
				from = to - dist;
				copy = MAXWIN;
				if(int(s->next) < dist) {
					from += copy;
					copy = dist;
				}
				copy -= s->next;
				if(copy > len) {
					copy = len;
				}
				len -= copy;
				s->next += copy;
				do {
					*to++ = *from++;
				} while(--copy);
				if(s->next == MAXWIN) {
					if(s->outfun(s->outhow, s->out, s->next)) {
						return BLAST_OUTPUT_ERROR;
					}
					s->next = 0;
					s->first = 0;
				}
			} while(len != 0);
			
		} else {
			/* get literal and write it */
			symbol = lit ? decode(s, &litcode) : bits(s, 8);
			s->out[s->next++] = symbol;
			if(s->next == MAXWIN) {
				if(s->outfun(s->outhow, s->out, s->next)) {
					return BLAST_OUTPUT_ERROR;
				}
				s->next = 0;
				s->first = 0;
			}
		}
	} while(true);
	
	return BLAST_SUCCESS;
}


/*
 * Table-driven version of blastDecompress(), with the same output and errors.
 *
 * decode() walks a code one bit at a time. Here each code has a table indexed
 * by the next MAXBITS input bits (in stream order) that gives the symbol and
 * its length, built by running decode()'s walk over every possible window, so
 * a symbol takes one lookup. Bits are kept in a 32-bit buffer.
 */
namespace {

struct FastCode {
	short symbol[1 << MAXBITS];
	unsigned char length[1 << MAXBITS]; /* 0: no valid code in this window */
};

FastCode g_fastLit;
FastCode g_fastLen;
FastCode g_fastDist;

void buildFastCode(const huffman & h, FastCode & out) {
	for(int window = 0; window < (1 << MAXBITS); window++) {
		/* the walk in decode(), on the bits of window */
		int code = 0, first = 0, index = 0;
		out.length[window] = 0;
		out.symbol[window] = -9;
		for(int len = 1; len <= MAXBITS; len++) {
			code |= ((window >> (len - 1)) & 1) ^ 1;
			int count = h.count[len];
			if(code < first + count) {
				out.symbol[window] = h.symbol[index + (code - first)];
				out.length[window] = static_cast<unsigned char>(len);
				break;
			}
			index += count;
			first += count;
			first <<= 1;
			code <<= 1;
		}
	}
}

void initFastCodes() {
	static bool done = false;
	if(!done) {
		initCodes();
		buildFastCode(litcode, g_fastLit);
		buildFastCode(lencode, g_fastLen);
		buildFastCode(distcode, g_fastDist);
		done = true;
	}
}

struct FastState {
	
	blast_in infun;
	void * inhow;
	const unsigned char * in;
	size_t left;
	u32 bitbuf;
	int bitcnt;
	bool eof;
	
	blast_out outfun;
	void * outhow;
	unsigned next;
	int first;
	unsigned char out[MAXWIN];
	
	/* At least 25 bits in the buffer, unless the input ended */
	void refill() {
		while(bitcnt <= 24) {
			if(left == 0) {
				if(eof) {
					return;
				}
				left = infun(inhow, &in);
				if(left == 0) {
					eof = true;
					return;
				}
			}
			bitbuf |= u32(*in++) << bitcnt;
			left--;
			bitcnt += 8;
		}
	}
	
	int bits(int need) {
		if(bitcnt < need) {
			refill();
			if(bitcnt < need) {
				throw blast_truncated_error();
			}
		}
		int val = int(bitbuf & ((u32(1) << need) - 1));
		bitbuf >>= need;
		bitcnt -= need;
		return val;
	}
	
	int decode(const FastCode & code) {
		if(bitcnt < MAXBITS) {
			refill();
		}
		/* Missing bits past the end of the input read as zero: a code that fits
		 * in the bits we have doesn't depend on them, and one that doesn't is
		 * the truncated input decode() would have thrown for. */
		unsigned window = bitbuf & ((1u << MAXBITS) - 1);
		int len = code.length[window];
		if(len == 0) {
			len = MAXBITS; /* decode() gives up after MAXBITS bits */
		}
		if(len > bitcnt) {
			throw blast_truncated_error();
		}
		bitbuf >>= len;
		bitcnt -= len;
		return code.symbol[window];
	}
	
	bool flush() {
		if(outfun(outhow, out, next)) {
			return false;
		}
		next = 0;
		first = 0;
		return true;
	}
	
};

BlastResult blastDecompressFast(FastState & s) {
	
	int lit = s.bits(8);
	if(lit > 1) {
		return BLAST_INVALID_LITERAL_FLAG;
	}
	int dict = s.bits(8);
	if(dict < 4 || dict > 6) {
		return BLAST_INVALID_DIC_SIZE;
	}
	
	while(true) {
		if(s.bits(1)) {
			int symbol = s.decode(g_fastLen);
			if(symbol < 0) {
				return BLAST_INVALID_OFFSET; /* decode() returned -9: invalid code */
			}
			int len = base[symbol] + s.bits(extra[symbol]);
			if(len == 519) {
				break; /* end code */
			}
			int shift = len == 2 ? 2 : dict;
			int distSymbol = s.decode(g_fastDist);
			if(distSymbol < 0) {
				return BLAST_INVALID_OFFSET;
			}
			int dist = (distSymbol << shift) + s.bits(shift) + 1;
			if(s.first && dist > int(s.next)) {
				return BLAST_INVALID_OFFSET;
			}
			do {
				unsigned char * to = s.out + s.next;
				const unsigned char * from = to - dist;
				int copy = MAXWIN;
				if(int(s.next) < dist) {
					from += copy;
					copy = dist;
				}
				copy -= int(s.next);
				if(copy > len) {
					copy = len;
				}
				len -= copy;
				s.next += unsigned(copy);
				do {
					*to++ = *from++;
				} while(--copy);
				if(s.next == MAXWIN && !s.flush()) {
					return BLAST_OUTPUT_ERROR;
				}
			} while(len != 0);
		} else {
			int symbol = lit ? s.decode(g_fastLit) : s.bits(8);
			s.out[s.next++] = static_cast<unsigned char>(symbol);
			if(s.next == MAXWIN && !s.flush()) {
				return BLAST_OUTPUT_ERROR;
			}
		}
	}
	
	return BLAST_SUCCESS;
}

BlastResult blastFast(blast_in infun, void * inhow, blast_out outfun, void * outhow) {
	
	initFastCodes();
	
	std::unique_ptr<FastState> sp(new FastState);
	FastState & s = *sp;
	s.infun = infun;
	s.inhow = inhow;
	s.in = nullptr;
	s.left = 0;
	s.bitbuf = 0;
	s.bitcnt = 0;
	s.eof = false;
	s.outfun = outfun;
	s.outhow = outhow;
	s.next = 0;
	s.first = 1;
	
	BlastResult err;
	try {
		err = blastDecompressFast(s);
	} catch(const blast_truncated_error &) {
		err = BLAST_TRUNCATED_INPUT;
	}
	
	if(err != 1 && s.next && s.outfun(s.outhow, s.out, s.next) && err == 0) {
		err = BLAST_OUTPUT_ERROR;
	}
	
	return err;
}

} // anonymous namespace

static BlastResult blastReference(blast_in infun, void * inhow, blast_out outfun, void * outhow) {

	#if ARX_PLATFORM == ARX_PLATFORM_VITA
	// Heap-allocate to avoid ~4KB stack usage — Vita main thread stack is small
	// and blast() is called from deep level-loading call chains
	std::unique_ptr<state> sp(new state);
	state & s = *sp;
	#else
	state s;
	#endif

	// initialize input state
	s.infun = infun;
	s.inhow = inhow;
	s.left = 0;
	s.bitbuf = 0;
	s.bitcnt = 0;

	// initialize output state
	s.outfun = outfun;
	s.outhow = outhow;
	s.next = 0;
	s.first = 1;

	BlastResult err;
	try {
		err = blastDecompress(&s);
	} catch(const blast_truncated_error &) {
		err = BLAST_TRUNCATED_INPUT;
	}

	// write any leftover output and update the error code if needed
	if(err != 1 && s.next && s.outfun(s.outhow, s.out, s.next) && err == 0) {
		err = BLAST_OUTPUT_ERROR;
	}

	return err;
}

void blastInitTables() {
	initCodes();
	#if ARX_PLATFORM == ARX_PLATFORM_VITA
	initFastCodes();
	#endif
}

BlastResult blast(blast_in infun, void * inhow, blast_out outfun, void * outhow) {
	
	VITA_LOAD_ZONE(LoadBlast);
	
	#if ARX_PLATFORM == ARX_PLATFORM_VITA
	return blastFast(infun, inhow, outfun, outhow);
	#else
	return blastReference(infun, inhow, outfun, outhow);
	#endif
}

// Additional functions.

size_t blastInMem(void * param, const unsigned char ** buf) {
	
	BlastMemInBuffer * p = static_cast<BlastMemInBuffer *>(param);
	
	*buf = reinterpret_cast<const unsigned char *>(p->buf);
	
	size_t size = p->size;
	
	p->buf += size;
	p->size = 0;

	return size;
}

int blastOutString(void * param, unsigned char * buf, size_t len) {
	
	BlastMemOutString * p = static_cast<BlastMemOutString *>(param);
	
	p->buffer.append(reinterpret_cast<const char *>(buf), len);
	
	return 0;
}

std::string blast(std::string_view from, size_t toSizeHint) {
	
	std::string uncompressed;
	uncompressed.reserve(toSizeHint == size_t(-1) ? from.size() : toSizeHint);
	
	BlastMemInBuffer in(from.data(), from.size());
	BlastMemOutString out(uncompressed);
	
	BlastResult error = blast(blastInMem, &in, blastOutString, &out);
	if(error) {
		LogError << "blast error " << int(error) << " for " << from.size();
		uncompressed.clear();
	}
	
	#if ARX_PLATFORM == ARX_PLATFORM_VITA
	// ux0:data/arx/blastcheck.txt: decompress everything with the reference decoder too
	static const bool s_check = [] {
		SceIoStat st;
		return sceIoGetstat("ux0:data/arx/blastcheck.txt", &st) >= 0;
	}();
	if(s_check) {
		static unsigned s_checked = 0;
		static unsigned s_mismatches = 0;
		std::string reference;
		BlastMemInBuffer refIn(from.data(), from.size());
		BlastMemOutString refOut(reference);
		BlastResult refError = blastReference(blastInMem, &refIn, blastOutString, &refOut);
		if(refError) {
			reference.clear();
		}
		s_checked++;
		if(refError != error || reference != uncompressed) {
			s_mismatches++;
			LogError << "[BlastCheck] mismatch: " << from.size() << " bytes, errors " << int(error) << "/"
			         << int(refError) << ", sizes " << uncompressed.size() << "/" << reference.size();
		}
		if(s_checked % 200 == 0) {
			LogInfo << "[BlastCheck] " << s_checked << " checked, " << s_mismatches << " mismatches";
		}
	}
	#endif
	
	return uncompressed;
}
