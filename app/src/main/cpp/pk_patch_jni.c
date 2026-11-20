/* PATCH A ROM's IPS / UPS / xdelta (and BPS, though the app reads those itself, BpsPatch.kt): third_party/pkpatch's
 * pk_patch.c for RomPatchNative.kt. */
#include <jni.h>
#include <stdint.h>
#include <stdlib.h>
#include "pk_patch.h"

/* [format, source size or -1, source CRC32 or -1] from the patch's own header. */
JNIEXPORT jlongArray JNICALL
Java_com_pokedaisy_app_RomPatchNative_info(JNIEnv* env, jclass cls, jbyteArray patch) {
	(void) cls;
	jsize n = (*env)->GetArrayLength(env, patch);
	jbyte* p = (*env)->GetPrimitiveArrayCritical(env, patch, NULL);
	if (!p) return NULL;
	jlong out[3] = {PK_PATCH_UNKNOWN, -1, -1};
	out[0] = pk_patch_format((const uint8_t*) p, (size_t) n);
	uint64_t size;
	uint32_t crc;
	if (pk_patch_source_size((const uint8_t*) p, (size_t) n, &size)) out[1] = (jlong) size;
	if (pk_patch_source_crc((const uint8_t*) p, (size_t) n, &crc)) out[2] = (jlong) crc;
	(*env)->ReleasePrimitiveArrayCritical(env, patch, p, JNI_ABORT);
	jlongArray result = (*env)->NewLongArray(env, 3);
	if (result) (*env)->SetLongArrayRegion(env, result, 0, 3, out);
	return result;
}

/* [source] patched, or null with the reason (pk_patch.h's PK_PATCH_ERR_*) in error[0]. */
JNIEXPORT jbyteArray JNICALL
Java_com_pokedaisy_app_RomPatchNative_apply(JNIEnv* env, jclass cls, jbyteArray source, jbyteArray patch, jintArray error) {
	(void) cls;
	jsize sn = (*env)->GetArrayLength(env, source), pn = (*env)->GetArrayLength(env, patch);
	uint8_t* out = NULL;
	size_t outSize = 0;
	jint code;
	jbyte* s = (*env)->GetPrimitiveArrayCritical(env, source, NULL);
	jbyte* p = s ? (*env)->GetPrimitiveArrayCritical(env, patch, NULL) : NULL;
	if (s && p) code = pk_patch_apply((const uint8_t*) s, (size_t) sn, (const uint8_t*) p, (size_t) pn, &out, &outSize);
	else code = PK_PATCH_ERR_MEMORY;
	if (p) (*env)->ReleasePrimitiveArrayCritical(env, patch, p, JNI_ABORT);
	if (s) (*env)->ReleasePrimitiveArrayCritical(env, source, s, JNI_ABORT);
	jbyteArray result = NULL;
	if (code == PK_PATCH_OK) {
		if (outSize > 0x7FFFFFF0u) code = PK_PATCH_ERR_MEMORY;
		else {
			result = (*env)->NewByteArray(env, (jsize) outSize);
			if (result) (*env)->SetByteArrayRegion(env, result, 0, (jsize) outSize, (const jbyte*) out);
			else {
				(*env)->ExceptionClear(env); /* an OutOfMemoryError: reported as MEMORY instead */
				code = PK_PATCH_ERR_MEMORY;
			}
		}
	}
	pk_patch_free(out);
	(*env)->SetIntArrayRegion(env, error, 0, 1, &code);
	return result;
}
