/* The few liblzma calls xdelta3's LZMA secondary decoder makes (xdelta3-lzma.h), on XZ Embedded: Capsule only
 * decodes patches, so the encoder's calls are stubs that refuse. */
#ifndef CAPSULE_LZMA_SHIM_H
#define CAPSULE_LZMA_SHIM_H

#include <stddef.h>
#include <stdint.h>

typedef enum {
	LZMA_OK = 0, LZMA_STREAM_END = 1, LZMA_NO_CHECK = 2, LZMA_UNSUPPORTED_CHECK = 3, LZMA_GET_CHECK = 4,
	LZMA_MEM_ERROR = 5, LZMA_MEMLIMIT_ERROR = 6, LZMA_FORMAT_ERROR = 7, LZMA_OPTIONS_ERROR = 8,
	LZMA_DATA_ERROR = 9, LZMA_BUF_ERROR = 10, LZMA_PROG_ERROR = 11
} lzma_ret;
typedef enum { LZMA_RUN = 0, LZMA_SYNC_FLUSH = 1, LZMA_FULL_FLUSH = 2, LZMA_FINISH = 3 } lzma_action;
typedef enum { LZMA_CHECK_NONE = 0 } lzma_check;

#define LZMA_TELL_NO_CHECK 0x01u
#define LZMA_FILTER_LZMA2 0x21u
#define LZMA_VLI_UNKNOWN UINT64_MAX

typedef struct {
	const uint8_t* next_in;
	size_t avail_in;
	uint64_t total_in;
	uint8_t* next_out;
	size_t avail_out;
	uint64_t total_out;
	void* internal; /* the XZ Embedded decoder */
} lzma_stream;

typedef struct { uint32_t dict_size; } lzma_options_lzma;
typedef struct { uint64_t id; void* options; } lzma_filter;

lzma_ret lzma_stream_decoder(lzma_stream* strm, uint64_t memlimit, uint32_t flags);
lzma_ret lzma_code(lzma_stream* strm, lzma_action action);
void lzma_end(lzma_stream* strm);
int lzma_lzma_preset(lzma_options_lzma* options, uint32_t preset);
lzma_ret lzma_stream_encoder(lzma_stream* strm, const lzma_filter* filters, lzma_check check);

#endif
