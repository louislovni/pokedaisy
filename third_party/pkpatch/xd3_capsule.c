/* xdelta3's decoder, built for Capsule: decode only, with its secondary compressors (DJW, FGK, LZMA through
 * ../xz-shim). pk_patch.c hands it the .xdelta patches it can't read itself. */
#define XD3_ENCODER 0
#define XD3_MAIN 0
#define NOT_MAIN 0
#define REGRESSION_TEST 0
#define XD3_DEBUG 0
#define SECONDARY_DJW 1
#define SECONDARY_FGK 0 /* experimental in xdelta3, and its decoder needs the encoder built */
#define SECONDARY_LZMA 1
#define XD3_USE_LARGEFILE64 1
#include <stdint.h>
#if UINTPTR_MAX == 0xffffffffffffffffu
#define SIZEOF_SIZE_T 8
#else
#define SIZEOF_SIZE_T 4
#endif
#define SIZEOF_UNSIGNED_LONG_LONG 8
#include "xdelta3/xdelta3.c"

int capsule_xd3_decode(const uint8_t* patch, size_t patchSize, const uint8_t* source, size_t sourceSize,
	uint8_t* out, size_t outCapacity, size_t* outSize) {
	usize_t produced = 0;
	int ret = xd3_decode_memory(patch, (usize_t) patchSize, source, (usize_t) sourceSize, out, &produced,
		(usize_t) outCapacity, 0);
	*outSize = produced;
	return ret;
}
