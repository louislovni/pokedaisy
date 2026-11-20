#include "lzma.h"
#include "xz.h"

lzma_ret lzma_stream_decoder(lzma_stream* strm, uint64_t memlimit, uint32_t flags) {
	(void) memlimit;
	(void) flags;
	static int crcReady;
	if (!crcReady) {
		xz_crc32_init();
		crcReady = 1;
	}
	strm->internal = xz_dec_init(XZ_DYNALLOC, (uint32_t) -1);
	return strm->internal ? LZMA_OK : LZMA_MEM_ERROR;
}

lzma_ret lzma_code(lzma_stream* strm, lzma_action action) {
	(void) action;
	if (!strm->internal) return LZMA_PROG_ERROR;
	struct xz_buf b = {strm->next_in, 0, strm->avail_in, strm->next_out, 0, strm->avail_out};
	enum xz_ret r = xz_dec_run((struct xz_dec*) strm->internal, &b);
	strm->next_in += b.in_pos;
	strm->avail_in -= b.in_pos;
	strm->total_in += b.in_pos;
	strm->next_out += b.out_pos;
	strm->avail_out -= b.out_pos;
	strm->total_out += b.out_pos;
	switch (r) {
	case XZ_OK:
	case XZ_UNSUPPORTED_CHECK:
		/* No progress with room left: the section ended short (xdelta3 would ask again forever). */
		if (!b.in_pos && !b.out_pos && strm->avail_out) return LZMA_BUF_ERROR;
		return LZMA_OK;
	case XZ_STREAM_END: return LZMA_STREAM_END;
	case XZ_MEM_ERROR:
	case XZ_MEMLIMIT_ERROR: return LZMA_MEM_ERROR;
	default: return LZMA_DATA_ERROR;
	}
}

void lzma_end(lzma_stream* strm) {
	if (!strm) return; /* xdelta3 ends the streams it never started (liblzma allows it) */
	if (strm->internal) xz_dec_end((struct xz_dec*) strm->internal);
	strm->internal = 0;
}

int lzma_lzma_preset(lzma_options_lzma* options, uint32_t preset) {
	(void) options;
	(void) preset;
	return 1;
}

lzma_ret lzma_stream_encoder(lzma_stream* strm, const lzma_filter* filters, lzma_check check) {
	(void) strm;
	(void) filters;
	(void) check;
	return LZMA_PROG_ERROR;
}
