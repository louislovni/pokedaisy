/* ROM patches: IPS, UPS, BPS, xdelta (Capsule's pk_patch.c, Core/pkcore in louislovni/capsule; shared by the
 * Android app's PATCH A ROM and the PC host's drop-a-patch). */
#ifndef PK_PATCH_H
#define PK_PATCH_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* ---- ROM patches (pk_patch.c): IPS, UPS, BPS, xdelta ---- */
enum { PK_PATCH_UNKNOWN = 0, PK_PATCH_IPS, PK_PATCH_UPS, PK_PATCH_BPS, PK_PATCH_XDELTA };
enum {
	PK_PATCH_OK = 0,
	PK_PATCH_ERR_FORMAT = -1,       /* not a patch this knows */
	PK_PATCH_ERR_CORRUPT = -2,      /* the patch is damaged */
	PK_PATCH_ERR_WRONG_SOURCE = -3, /* the patch is for another ROM (UPS / BPS's CRC32, xdelta's sizes) */
	PK_PATCH_ERR_RESULT = -4,       /* the result's checksum doesn't match: usually the wrong ROM */
	PK_PATCH_ERR_UNSUPPORTED = -5,  /* a patch feature this can't read */
	PK_PATCH_ERR_MEMORY = -6,
};
int pk_patch_format(const uint8_t* patch, size_t size);
/* The patched ROM in *out (pk_patch_free it); PK_PATCH_OK or an error above. */
int pk_patch_apply(const uint8_t* source, size_t sourceSize, const uint8_t* patch, size_t patchSize,
                   uint8_t** out, size_t* outSize);
void pk_patch_free(uint8_t* data);
/* The CRC32 of the ROM a UPS or BPS patch is made for (1), or 0 when the patch doesn't say. */
int pk_patch_source_crc(const uint8_t* patch, size_t size, uint32_t* crc);
/* The size of the ROM a UPS or BPS patch is made for (1), or 0. */
int pk_patch_source_size(const uint8_t* patch, size_t size, uint64_t* sourceSize);
uint32_t pk_crc32(const uint8_t* data, size_t size);
#ifdef __cplusplus
}
#endif
#endif
