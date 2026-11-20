package com.pokedaisy.app

/**
 * PATCH A ROM's IPS, UPS and xdelta patches (BPS stays [BpsPatch]): third_party/pkpatch's pk_patch.c, xdelta3's decoder
 * included (with its LZMA / DJW compression, the CLI's default), through pk_patch_jni.c.
 */
object RomPatchNative {
    init {
        System.loadLibrary("pokedaisy")
    }

    const val UNKNOWN = 0
    const val IPS = 1
    const val UPS = 2
    const val BPS = 3
    const val XDELTA = 4

    const val ERR_FORMAT = -1
    const val ERR_CORRUPT = -2
    const val ERR_WRONG_SOURCE = -3
    const val ERR_RESULT = -4
    const val ERR_UNSUPPORTED = -5
    const val ERR_MEMORY = -6

    /** [format, source size or -1, source CRC32 or -1]. */
    @JvmStatic external fun info(patch: ByteArray): LongArray?

    /** [source] patched, or null with the reason (ERR_*) in [error]`[0]`. */
    @JvmStatic external fun apply(source: ByteArray, patch: ByteArray, error: IntArray): ByteArray?

    /** A patch's first bytes: IPS `PATCH`, UPS `UPS1`, BPS `BPS1` or xdelta's VCDIFF magic. No library needed. */
    fun looksLikePatch(head: ByteArray): Boolean {
        fun starts(vararg b: Int) = head.size >= b.size && b.indices.all { (head[it].toInt() and 0xFF) == b[it] }
        return starts('P'.code, 'A'.code, 'T'.code, 'C'.code, 'H'.code) || starts('U'.code, 'P'.code, 'S'.code, '1'.code) ||
            starts('B'.code, 'P'.code, 'S'.code, '1'.code) || starts(0xD6, 0xC3, 0xC4)
    }

    fun formatName(format: Int): String = when (format) {
        IPS -> "IPS"
        UPS -> "UPS"
        BPS -> "BPS"
        XDELTA -> "xdelta"
        else -> "?"
    }
}
