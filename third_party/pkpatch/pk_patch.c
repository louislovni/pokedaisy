/* ROM patches: IPS, UPS, BPS and xdelta (VCDIFF, RFC 3284, as xdelta3 writes it). A patch is applied to the
 * player's own clean ROM, giving the hack; UPS and BPS carry the source's CRC32, which is checked first. */
#include <errno.h>
#include "pk_patch.h"

#include <stdlib.h>
#include <string.h>

static uint32_t crc32_of(const uint8_t* data, size_t size) {
	static uint32_t table[256];
	static int ready;
	if (!ready) {
		for (uint32_t i = 0; i < 256; ++i) {
			uint32_t c = i;
			for (int k = 0; k < 8; ++k) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
			table[i] = c;
		}
		ready = 1;
	}
	uint32_t crc = 0xFFFFFFFFu;
	for (size_t i = 0; i < size; ++i) crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
	return crc ^ 0xFFFFFFFFu;
}

uint32_t pk_crc32(const uint8_t* data, size_t size) { return crc32_of(data, size); }

static uint32_t rd32le(const uint8_t* p) { return (uint32_t) p[0] | (uint32_t) p[1] << 8 | (uint32_t) p[2] << 16 | (uint32_t) p[3] << 24; }

int pk_patch_format(const uint8_t* patch, size_t size) {
	if (size >= 8 && !memcmp(patch, "PATCH", 5)) return PK_PATCH_IPS;
	if (size >= 16 && !memcmp(patch, "UPS1", 4)) return PK_PATCH_UPS;
	if (size >= 16 && !memcmp(patch, "BPS1", 4)) return PK_PATCH_BPS;
	if (size >= 5 && patch[0] == 0xD6 && patch[1] == 0xC3 && patch[2] == 0xC4) return PK_PATCH_XDELTA;
	return PK_PATCH_UNKNOWN;
}

/* ---- Output buffer ---- */

typedef struct {
	uint8_t* data;
	size_t size, capacity;
} Out;

static int out_reserve(Out* o, size_t size) {
	if (size <= o->capacity) return 1;
	size_t capacity = o->capacity ? o->capacity : 1 << 20;
	while (capacity < size) capacity *= 2;
	uint8_t* grown = realloc(o->data, capacity);
	if (!grown) return 0;
	memset(grown + o->capacity, 0, capacity - o->capacity);
	o->data = grown;
	o->capacity = capacity;
	return 1;
}

/* ---- IPS ---- */

static int apply_ips(const uint8_t* src, size_t srcSize, const uint8_t* p, size_t n, Out* o) {
	if (!out_reserve(o, srcSize)) return PK_PATCH_ERR_MEMORY;
	memcpy(o->data, src, srcSize);
	o->size = srcSize;
	size_t at = 5;
	for (;;) {
		if (at + 3 > n) return PK_PATCH_ERR_CORRUPT;
		if (!memcmp(p + at, "EOF", 3)) {
			at += 3;
			if (at + 3 <= n) o->size = (size_t) p[at] << 16 | (size_t) p[at + 1] << 8 | p[at + 2]; /* truncation */
			return PK_PATCH_OK;
		}
		if (at + 5 > n) return PK_PATCH_ERR_CORRUPT;
		size_t offset = (size_t) p[at] << 16 | (size_t) p[at + 1] << 8 | p[at + 2];
		size_t length = (size_t) p[at + 3] << 8 | p[at + 4];
		at += 5;
		if (length) {
			if (at + length > n) return PK_PATCH_ERR_CORRUPT;
			if (!out_reserve(o, offset + length)) return PK_PATCH_ERR_MEMORY;
			memcpy(o->data + offset, p + at, length);
			at += length;
			if (offset + length > o->size) o->size = offset + length;
		} else {
			if (at + 3 > n) return PK_PATCH_ERR_CORRUPT;
			size_t run = (size_t) p[at] << 8 | p[at + 1];
			uint8_t value = p[at + 2];
			at += 3;
			if (!out_reserve(o, offset + run)) return PK_PATCH_ERR_MEMORY;
			memset(o->data + offset, value, run);
			if (offset + run > o->size) o->size = offset + run;
		}
	}
}

/* ---- UPS and BPS: variable-length numbers ---- */

static int read_vlq(const uint8_t* p, size_t n, size_t* at, uint64_t* value) {
	uint64_t data = 0, shift = 1;
	for (;;) {
		if (*at >= n || shift > (1ull << 56)) return 0;
		uint8_t x = p[(*at)++];
		data += (x & 0x7F) * shift;
		if (x & 0x80) break;
		shift <<= 7;
		data += shift;
	}
	*value = data;
	return 1;
}

static int apply_ups(const uint8_t* src, size_t srcSize, const uint8_t* p, size_t n, Out* o) {
	size_t at = 4;
	uint64_t inSize, outSize;
	if (!read_vlq(p, n - 12, &at, &inSize) || !read_vlq(p, n - 12, &at, &outSize)) return PK_PATCH_ERR_CORRUPT;
	if (crc32_of(p, n - 4) != rd32le(p + n - 4)) return PK_PATCH_ERR_CORRUPT;
	if (inSize != srcSize || crc32_of(src, srcSize) != rd32le(p + n - 12)) return PK_PATCH_ERR_WRONG_SOURCE;
	if (!out_reserve(o, (size_t) outSize)) return PK_PATCH_ERR_MEMORY;
	memcpy(o->data, src, srcSize < outSize ? srcSize : (size_t) outSize);
	o->size = (size_t) outSize;
	size_t pos = 0;
	while (at < n - 12) {
		uint64_t skip;
		if (!read_vlq(p, n - 12, &at, &skip)) return PK_PATCH_ERR_CORRUPT;
		pos += (size_t) skip;
		while (at < n - 12) {
			uint8_t x = p[at++];
			if (pos < o->size) o->data[pos] ^= x;
			++pos;
			if (!x) break;
		}
	}
	if (crc32_of(o->data, o->size) != rd32le(p + n - 8)) return PK_PATCH_ERR_RESULT;
	return PK_PATCH_OK;
}

static int apply_bps(const uint8_t* src, size_t srcSize, const uint8_t* p, size_t n, Out* o) {
	size_t at = 4, end = n - 12;
	uint64_t inSize, outSize, metaSize;
	if (!read_vlq(p, end, &at, &inSize) || !read_vlq(p, end, &at, &outSize) || !read_vlq(p, end, &at, &metaSize))
		return PK_PATCH_ERR_CORRUPT;
	at += (size_t) metaSize;
	if (at > end || crc32_of(p, n - 4) != rd32le(p + n - 4)) return PK_PATCH_ERR_CORRUPT;
	if (inSize != srcSize || crc32_of(src, srcSize) != rd32le(p + n - 12)) return PK_PATCH_ERR_WRONG_SOURCE;
	if (!out_reserve(o, (size_t) outSize)) return PK_PATCH_ERR_MEMORY;
	o->size = (size_t) outSize;
	size_t outPos = 0;
	int64_t srcRel = 0, outRel = 0;
	while (at < end) {
		uint64_t data;
		if (!read_vlq(p, end, &at, &data)) return PK_PATCH_ERR_CORRUPT;
		uint64_t command = data & 3, length = (data >> 2) + 1;
		if (outPos + length > o->size) return PK_PATCH_ERR_CORRUPT;
		if (command == 0) { /* source read */
			if (outPos + length > srcSize) return PK_PATCH_ERR_CORRUPT;
			memcpy(o->data + outPos, src + outPos, (size_t) length);
			outPos += (size_t) length;
		} else if (command == 1) { /* target read */
			if (at + length > end) return PK_PATCH_ERR_CORRUPT;
			memcpy(o->data + outPos, p + at, (size_t) length);
			at += (size_t) length;
			outPos += (size_t) length;
		} else {
			uint64_t offset;
			if (!read_vlq(p, end, &at, &offset)) return PK_PATCH_ERR_CORRUPT;
			int64_t delta = (int64_t) (offset >> 1) * (offset & 1 ? -1 : 1);
			if (command == 2) { /* source copy */
				srcRel += delta;
				if (srcRel < 0 || (uint64_t) srcRel + length > srcSize) return PK_PATCH_ERR_CORRUPT;
				memcpy(o->data + outPos, src + srcRel, (size_t) length);
				srcRel += (int64_t) length;
				outPos += (size_t) length;
			} else { /* target copy: may overlap, byte by byte */
				outRel += delta;
				if (outRel < 0 || (uint64_t) outRel >= outPos) return PK_PATCH_ERR_CORRUPT;
				for (uint64_t i = 0; i < length; ++i) o->data[outPos++] = o->data[outRel++];
			}
		}
	}
	if (outPos != o->size) return PK_PATCH_ERR_CORRUPT;
	if (crc32_of(o->data, o->size) != rd32le(p + n - 8)) return PK_PATCH_ERR_RESULT;
	return PK_PATCH_OK;
}

/* ---- xdelta (VCDIFF) ---- */

static int read_vint(const uint8_t* p, size_t n, size_t* at, uint64_t* value) {
	uint64_t v = 0;
	for (int i = 0; i < 10; ++i) {
		if (*at >= n) return 0;
		uint8_t x = p[(*at)++];
		v = v << 7 | (x & 0x7F);
		if (!(x & 0x80)) {
			*value = v;
			return 1;
		}
	}
	return 0;
}

typedef struct {
	uint8_t type1, size1, mode1, type2, size2, mode2;
} CodeEntry;

/* RFC 3284's default code table (5.6). */
static void default_code_table(CodeEntry t[256]) {
	enum { NOOP = 0, ADD = 1, RUN = 2, COPY = 3 };
	int i = 0;
	t[i++] = (CodeEntry){RUN, 0, 0, NOOP, 0, 0};
	for (int s = 0; s <= 17; ++s) t[i++] = (CodeEntry){ADD, (uint8_t) s, 0, NOOP, 0, 0};
	for (int m = 0; m < 9; ++m) {
		t[i++] = (CodeEntry){COPY, 0, (uint8_t) m, NOOP, 0, 0};
		for (int s = 4; s <= 18; ++s) t[i++] = (CodeEntry){COPY, (uint8_t) s, (uint8_t) m, NOOP, 0, 0};
	}
	for (int m = 0; m < 6; ++m)
		for (int a = 1; a <= 4; ++a)
			for (int s = 4; s <= 6; ++s) t[i++] = (CodeEntry){ADD, (uint8_t) a, 0, COPY, (uint8_t) s, (uint8_t) m};
	for (int m = 6; m < 9; ++m)
		for (int a = 1; a <= 4; ++a) t[i++] = (CodeEntry){ADD, (uint8_t) a, 0, COPY, 4, (uint8_t) m};
	for (int m = 0; m < 9; ++m) t[i++] = (CodeEntry){COPY, 4, (uint8_t) m, ADD, 1, 0};
}

static uint32_t adler32_of(const uint8_t* data, size_t size) {
	uint32_t a = 1, b = 0;
	for (size_t i = 0; i < size; ++i) {
		a = (a + data[i]) % 65521;
		b = (b + a) % 65521;
	}
	return b << 16 | a;
}

static int apply_xdelta_plain(const uint8_t* src, size_t srcSize, const uint8_t* p, size_t n, Out* o) {
	static CodeEntry table[256];
	static int ready;
	if (!ready) {
		default_code_table(table);
		ready = 1;
	}
	size_t at = 4;
	uint8_t hdr = p[at++];
	if (hdr & 0x01) return PK_PATCH_ERR_UNSUPPORTED; /* secondary compression (xdelta3 -S) */
	if (hdr & 0x02) return PK_PATCH_ERR_UNSUPPORTED; /* its own code table */
	if (hdr & 0x04) { /* application header (xdelta3: file names) */
		uint64_t len;
		if (!read_vint(p, n, &at, &len) || at + len > n) return PK_PATCH_ERR_CORRUPT;
		at += (size_t) len;
	}
	o->size = 0;
	while (at < n) {
		uint8_t win = p[at++];
		uint64_t segLen = 0, segPos = 0;
		if (win & 0x03) {
			if (!read_vint(p, n, &at, &segLen) || !read_vint(p, n, &at, &segPos)) return PK_PATCH_ERR_CORRUPT;
		}
		uint64_t encLen, targetLen, dataLen, instLen, addrLen;
		if (!read_vint(p, n, &at, &encLen) || !read_vint(p, n, &at, &targetLen)) return PK_PATCH_ERR_CORRUPT;
		uint8_t delta = p[at++];
		if (delta) return PK_PATCH_ERR_UNSUPPORTED; /* compressed sections */
		if (!read_vint(p, n, &at, &dataLen) || !read_vint(p, n, &at, &instLen) || !read_vint(p, n, &at, &addrLen))
			return PK_PATCH_ERR_CORRUPT;
		uint32_t checksum = 0;
		int hasChecksum = win & 0x04;
		if (hasChecksum) {
			if (at + 4 > n) return PK_PATCH_ERR_CORRUPT;
			checksum = (uint32_t) p[at] << 24 | (uint32_t) p[at + 1] << 16 | (uint32_t) p[at + 2] << 8 | p[at + 3];
			at += 4;
		}
		if (at + dataLen + instLen + addrLen > n) return PK_PATCH_ERR_CORRUPT;
		const uint8_t* data = p + at;
		const uint8_t* inst = data + dataLen;
		const uint8_t* addr = inst + instLen;
		at += (size_t) (dataLen + instLen + addrLen);

		/* The window's source segment: the source file (VCD_SOURCE) or the target so far (VCD_TARGET). */
		const uint8_t* seg = NULL;
		if (win & 0x01) {
			if (segPos + segLen > srcSize) return PK_PATCH_ERR_WRONG_SOURCE;
			seg = src + segPos;
		} else if (win & 0x02) {
			if (segPos + segLen > o->size) return PK_PATCH_ERR_CORRUPT;
		}
		size_t base = o->size;
		if (!out_reserve(o, base + (size_t) targetLen)) return PK_PATCH_ERR_MEMORY;
		if (win & 0x02) seg = o->data + segPos;
		uint8_t* t = o->data + base;
		size_t tPos = 0, dPos = 0, iPos = 0, aPos = 0;
		uint64_t near[4] = {0}, same[256 * 3] = {0};
		int nextNear = 0;
		while (iPos < instLen) {
			const CodeEntry* e = &table[inst[iPos++]];
			for (int half = 0; half < 2; ++half) {
				uint8_t type = half ? e->type2 : e->type1;
				uint64_t size = half ? e->size2 : e->size1;
				uint8_t mode = half ? e->mode2 : e->mode1;
				if (!type) continue;
				if (!size && !read_vint(inst, instLen, &iPos, &size)) return PK_PATCH_ERR_CORRUPT;
				if (tPos + size > targetLen) return PK_PATCH_ERR_CORRUPT;
				if (type == 1) { /* ADD */
					if (dPos + size > dataLen) return PK_PATCH_ERR_CORRUPT;
					memcpy(t + tPos, data + dPos, (size_t) size);
					dPos += (size_t) size;
				} else if (type == 2) { /* RUN */
					if (dPos >= dataLen) return PK_PATCH_ERR_CORRUPT;
					memset(t + tPos, data[dPos++], (size_t) size);
				} else { /* COPY: the address by its mode (5.3) */
					uint64_t here = segLen + tPos, a;
					if (mode == 0) {
						if (!read_vint(addr, addrLen, &aPos, &a)) return PK_PATCH_ERR_CORRUPT;
					} else if (mode == 1) {
						uint64_t v;
						if (!read_vint(addr, addrLen, &aPos, &v) || v > here) return PK_PATCH_ERR_CORRUPT;
						a = here - v;
					} else if (mode < 6) {
						uint64_t v;
						if (!read_vint(addr, addrLen, &aPos, &v)) return PK_PATCH_ERR_CORRUPT;
						a = near[mode - 2] + v;
					} else {
						if (aPos >= addrLen) return PK_PATCH_ERR_CORRUPT;
						a = same[(mode - 6) * 256 + addr[aPos++]];
					}
					near[nextNear] = a;
					nextNear = (nextNear + 1) % 4;
					same[a % (256 * 3)] = a;
					for (uint64_t k = 0; k < size; ++k) {
						uint64_t from = a + k;
						if (from < segLen) {
							if (!seg) return PK_PATCH_ERR_CORRUPT;
							t[tPos + k] = seg[from];
						} else {
							uint64_t inTarget = from - segLen;
							if (inTarget >= tPos + k) return PK_PATCH_ERR_CORRUPT;
							t[tPos + k] = t[inTarget];
						}
					}
				}
				tPos += (size_t) size;
			}
		}
		if (tPos != targetLen) return PK_PATCH_ERR_CORRUPT;
		if (hasChecksum && adler32_of(t, (size_t) targetLen) != checksum) return PK_PATCH_ERR_RESULT;
		o->size = base + (size_t) targetLen;
		(void) encLen;
	}
	return PK_PATCH_OK;
}

/* Patches made with compression (xdelta3's default -S: LZMA, or DJW) or their own code table go to xdelta3's own
 * decoder (third_party/xd3_capsule.c); the window headers stay readable, which gives the target's size. */
int capsule_xd3_decode(const uint8_t* patch, size_t patchSize, const uint8_t* source, size_t sourceSize,
	uint8_t* out, size_t outCapacity, size_t* outSize);

static int apply_xdelta(const uint8_t* src, size_t srcSize, const uint8_t* p, size_t n, Out* o) {
	size_t at = 4;
	uint8_t hdr = p[at++];
	int plain = !(hdr & 0x03);
	if (hdr & 0x01) { /* the secondary compressor: 1 DJW, 2 LZMA (FGK, 16, isn't built) */
		if (at >= n) return PK_PATCH_ERR_CORRUPT;
		if (p[at] != 1 && p[at] != 2) return PK_PATCH_ERR_UNSUPPORTED;
		at++;
	}
	if (hdr & 0x02) {
		uint64_t len;
		if (!read_vint(p, n, &at, &len) || at + len > n) return PK_PATCH_ERR_CORRUPT;
		at += (size_t) len;
	}
	if (hdr & 0x04) {
		uint64_t len;
		if (!read_vint(p, n, &at, &len) || at + len > n) return PK_PATCH_ERR_CORRUPT;
		at += (size_t) len;
	}
	uint64_t total = 0;
	int usesSource = 0;
	while (at < n) {
		uint8_t win = p[at++];
		uint64_t segLen = 0, segPos = 0, encLen, targetLen;
		if (win & 0x03) {
			if (!read_vint(p, n, &at, &segLen) || !read_vint(p, n, &at, &segPos)) return PK_PATCH_ERR_CORRUPT;
			if (win & 0x01) {
				usesSource = 1;
				if (segPos + segLen > srcSize) return PK_PATCH_ERR_WRONG_SOURCE;
			}
		}
		if (!read_vint(p, n, &at, &encLen)) return PK_PATCH_ERR_CORRUPT;
		size_t encStart = at;
		if (!read_vint(p, n, &at, &targetLen) || at >= n) return PK_PATCH_ERR_CORRUPT;
		if (p[at]) plain = 0;
		if (encStart + encLen > n) return PK_PATCH_ERR_CORRUPT;
		at = encStart + (size_t) encLen;
		total += targetLen;
		if (total > ((uint64_t) 1 << 31)) return PK_PATCH_ERR_CORRUPT;
	}
	if (plain) return apply_xdelta_plain(src, srcSize, p, n, o);
	if (!out_reserve(o, total ? (size_t) total : 1)) return PK_PATCH_ERR_MEMORY;
	size_t produced = 0;
	int ret = capsule_xd3_decode(p, n, src, srcSize, o->data, (size_t) total, &produced);
	if (ret == ENOMEM) return PK_PATCH_ERR_MEMORY;
	if (ret != 0 || produced != total) return usesSource ? PK_PATCH_ERR_WRONG_SOURCE : PK_PATCH_ERR_CORRUPT;
	o->size = produced;
	return PK_PATCH_OK;
}

int pk_patch_apply(const uint8_t* source, size_t sourceSize, const uint8_t* patch, size_t patchSize,
                   uint8_t** out, size_t* outSize) {
	*out = NULL;
	*outSize = 0;
	Out o = {0};
	int result;
	switch (pk_patch_format(patch, patchSize)) {
	case PK_PATCH_IPS: result = apply_ips(source, sourceSize, patch, patchSize, &o); break;
	case PK_PATCH_UPS: result = apply_ups(source, sourceSize, patch, patchSize, &o); break;
	case PK_PATCH_BPS: result = apply_bps(source, sourceSize, patch, patchSize, &o); break;
	case PK_PATCH_XDELTA: result = apply_xdelta(source, sourceSize, patch, patchSize, &o); break;
	default: return PK_PATCH_ERR_FORMAT;
	}
	if (result != PK_PATCH_OK) {
		free(o.data);
		return result;
	}
	*out = o.data;
	*outSize = o.size;
	return PK_PATCH_OK;
}

void pk_patch_free(uint8_t* data) { free(data); }

int pk_patch_source_size(const uint8_t* patch, size_t size, uint64_t* sourceSize) {
	int format = pk_patch_format(patch, size);
	size_t at = 4;
	return (format == PK_PATCH_UPS || format == PK_PATCH_BPS) && size >= 16 && read_vlq(patch, size - 12, &at, sourceSize);
}

int pk_patch_source_crc(const uint8_t* patch, size_t size, uint32_t* crc) {
	int format = pk_patch_format(patch, size);
	if ((format == PK_PATCH_UPS || format == PK_PATCH_BPS) && size >= 16) {
		*crc = rd32le(patch + size - 12);
		return 1;
	}
	return 0;
}
