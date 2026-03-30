/*
 * Copyright (c) 2026 Clovis Corde
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/delta/delta.h>
#include "heatshrink_decoder.h"

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(delta_backend_bsdiff, CONFIG_DELTA_UPDATE_LOG_LEVEL);

#define HEADER_BLOCK_SIZE         8
#define PATCH_HEADER_SIZE         16
#define CONTROL_DATA_SIZE         8
#define CONTROL_DATA_NB           3
#define BSPATCH_CHUNK_SIZE        256
#define DECOMPRESS_BUF_SIZE       2048
#define HEATSHRINK_ALLOC_INPUT_SZ 4096

/**
 * @brief Decoder state machine states.
 */
enum decoder_state {
	/** Read and sink more data */
	STATE_SINK,
	/** Poll the data sunk */
	STATE_POLL,
	/** No more input, handle the last data sunk */
	STATE_FINISH
};

/**
 * @brief BSpatch stream context
 */
struct bspatch_stream {
	heatshrink_decoder *hsd;
	size_t bytes_decompressed;
	size_t offset;
	uint8_t rd_buf[CONFIG_DELTA_INPUT_BUFFER_SIZE];
	size_t rd_buf_len;
	size_t rd_buf_pos;
	int (*decompress)(struct delta_ctx *ctx, uint8_t *output_buffer, size_t output_buffer_size,
			  heatshrink_decoder *hsd, size_t *bytes_written, enum decoder_state *state,
			  struct bspatch_stream *stream);
};

/**
 * @brief Reset the heatshrink decoder and rewind the patch offset
 *        by the number of unprocessed bytes still in the read-ahead buffer.
 *
 * When decompress_patch() reads CONFIG_DELTA_INPUT_BUFFER_SIZE bytes at a time,
 * the patch offset advances past the end of the current compressed stream.
 * This function corrects the offset before starting the next stream.
 */
static void stream_rewind_unused(struct delta_ctx *ctx, struct bspatch_stream *stream)
{
	if (stream->rd_buf_pos < stream->rd_buf_len) {
		size_t unused = stream->rd_buf_len - stream->rd_buf_pos;

		ctx->seek(&ctx->memory, ctx->memory.offset.source,
			  ctx->memory.offset.patch - unused, ctx->memory.offset.target);
	}
	stream->rd_buf_pos = 0;
	stream->rd_buf_len = 0;
	heatshrink_decoder_reset(stream->hsd);
}

/**
 * @brief Decompresses a patch using heatshrink decoder.
 *
 * @param ctx Pointer to delta API structure.
 * @param output_buffer Buffer to write decompressed data.
 * @param output_buffer_size Size of the output buffer.
 * @param hsd Heatshrink decoder.
 * @param bytes_written Number of bytes written after decompression.
 * @param state Current decoder state.
 * @param stream Pointer to bspatch stream with read-ahead buffer.
 * @return 0 on success, negative error code on failure.
 */
static int decompress_patch(struct delta_ctx *ctx, uint8_t *output_buffer,
			    size_t output_buffer_size, heatshrink_decoder *hsd,
			    size_t *bytes_written, enum decoder_state *state,
			    struct bspatch_stream *stream)
{
	size_t read_sz = 0;
	size_t write_sz = 0;
	size_t offset_buffer = 0;
	size_t bytes_to_decode = output_buffer_size;
	int ret = 0;

	/* Switch to patch reading mode */
	ctx->memory.reading_patch = true;

	*state = STATE_SINK;
	while (true) {
		switch (*state) {
		case STATE_SINK:
			/* Refill read-ahead buffer if exhausted */
			if (stream->rd_buf_pos >= stream->rd_buf_len) {
				ret = ctx->read(&ctx->memory, stream->rd_buf,
						CONFIG_DELTA_INPUT_BUFFER_SIZE);
				if (ret != 0) {
					LOG_ERR("Can't read %d bytes at offset: %lld, ret = %d",
						CONFIG_DELTA_INPUT_BUFFER_SIZE,
						ctx->memory.offset.patch, ret);
					return ret;
				}

				ret = ctx->seek(&ctx->memory, ctx->memory.offset.source,
						ctx->memory.offset.patch +
							CONFIG_DELTA_INPUT_BUFFER_SIZE,
						ctx->memory.offset.target);
				if (ret != 0) {
					LOG_ERR("Can't seek patch at offset: %lld, ret = %d",
						ctx->memory.offset.patch +
							CONFIG_DELTA_INPUT_BUFFER_SIZE,
						ret);
					return ret;
				}

				stream->rd_buf_len = CONFIG_DELTA_INPUT_BUFFER_SIZE;
				stream->rd_buf_pos = 0;
			}

			/*
			 * Sink one byte at a time from the read-ahead buffer.
			 * Sinking more would push bytes from the next compressed
			 * stream into heatshrink's internal state, which are then
			 * lost on decoder reset at stream boundaries.
			 */
			{
				HSD_sink_res sink_res = heatshrink_decoder_sink(
					hsd, stream->rd_buf + stream->rd_buf_pos, 1, &read_sz);

				if (sink_res != HSDR_SINK_OK) {
					LOG_ERR("Heatshrink sink error: %d", sink_res);
					return sink_res;
				}
				stream->rd_buf_pos += read_sz;
			}
			*state = STATE_POLL;
			break;
		case STATE_POLL: {
			HSD_poll_res poll_res = HSDR_POLL_MORE;

			while (poll_res != HSDR_POLL_EMPTY && bytes_to_decode > 0) {
				size_t to_decode =
					(output_buffer_size - offset_buffer) < BSPATCH_CHUNK_SIZE
						? (output_buffer_size - offset_buffer)
						: BSPATCH_CHUNK_SIZE;

				poll_res = heatshrink_decoder_poll(
					hsd, output_buffer + offset_buffer, to_decode, &write_sz);
				offset_buffer += write_sz;
				bytes_to_decode -= write_sz;
			}
			if (bytes_to_decode == 0) {
				*state = STATE_FINISH;
			}
			if (poll_res == HSDR_POLL_EMPTY) {
				*state = STATE_SINK;
			}
			break;
		}
		case STATE_FINISH: {
			HSD_finish_res finish_res = heatshrink_decoder_finish(hsd);

			if (offset_buffer > 0) {
				*bytes_written = offset_buffer;
				return 0;
			}
			return finish_res;
		}
		default:
			return -ENOTSUP;
		}
	}
}

/**
 * @brief Convert a byte buffer into a 64-bit integer.
 * @param buf Byte buffer.
 * @return 64-bit integer representation of the buffer.
 */
static int64_t offtin(uint8_t *buf)
{
	int64_t y;

	y = buf[7] & 0x7F;
	y = y * 256;
	y += buf[6];
	y = y * 256;
	y += buf[5];
	y = y * 256;
	y += buf[4];
	y = y * 256;
	y += buf[3];
	y = y * 256;
	y += buf[2];
	y = y * 256;
	y += buf[1];
	y = y * 256;
	y += buf[0];

	if (buf[7] & 0x80) {
		y = -y;
	}

	return y;
}

/**
 * @brief Apply the BSDiff patch.
 *
 * @param ctx Pointer to delta API structure.
 * @param target_size Size of the target file.
 * @param patch Patch data buffer.
 * @param offset_buffer Offset in the output buffer.
 * @param bytes_written Number of bytes written to the output buffer.
 * @param stream Pointer to the bspatch stream.
 * @param state Current decoder state.
 * @return 0 on success, negative error code on failure.
 */
static int bspatch(struct delta_ctx *ctx, int64_t target_size, uint8_t *patch,
		   size_t *offset_buffer, size_t *bytes_written, struct bspatch_stream *stream,
		   enum decoder_state *state)
{
	uint8_t buf[CONTROL_DATA_SIZE];
	size_t oldpos = 0;
	size_t newpos = 0;
	int64_t ctrl[CONTROL_DATA_NB];
	uint16_t bytes_to_read = BSPATCH_CHUNK_SIZE;
	uint8_t old[BSPATCH_CHUNK_SIZE];
	uint8_t new_data[BSPATCH_CHUNK_SIZE];
	int ret = 0;

	while (newpos < target_size) {
		/* Decompress the control data (ctrl[0] = diff, ctrl[1] = extra, ctrl[2] = offset in
		 * source firmware)
		 */
		if (*bytes_written == 0) {
			ret = stream->decompress(ctx, patch, CONTROL_DATA_SIZE * CONTROL_DATA_NB,
						 stream->hsd, bytes_written, state, stream);
			if (ret != 0) {
				LOG_ERR("Error decompressing patch for control data");
			}
			stream_rewind_unused(ctx, stream);
			*offset_buffer = 0;
		}

		/* Read control data */
		for (size_t i = 0; i < CONTROL_DATA_NB; i++) {
			memcpy(buf, patch + *offset_buffer, CONTROL_DATA_SIZE);
			*offset_buffer += CONTROL_DATA_SIZE;
			*bytes_written -= CONTROL_DATA_SIZE;
			ctrl[i] = offtin(buf);
		}

		LOG_DBG("ctrl[0] =%lld, ctrl[1] = %lld, ctrl[2] = %lld", ctrl[0], ctrl[1], ctrl[2]);

		/* Sanity check */
		if (ctrl[0] < 0 || ctrl[0] > INT_MAX || ctrl[1] < 0 || ctrl[1] > INT_MAX ||
		    newpos + ctrl[0] > target_size) {
			LOG_ERR("Sanity check failed on ctrl[0] or ctrl[1]");
			return -EOVERFLOW;
		}

		/* Process diff block (ctrl[0]) */
		while (ctrl[0] > 0) {
			if (*bytes_written == 0) {
				int64_t decoded = ctrl[0] > DECOMPRESS_BUF_SIZE
							  ? DECOMPRESS_BUF_SIZE
							  : ctrl[0];

				ret = stream->decompress(ctx, patch, decoded, stream->hsd,
							 bytes_written, state, stream);
				if (ret != 0) {
					LOG_ERR("Error decompressing patch for ctrl[0]");
				}
				if (*bytes_written < DECOMPRESS_BUF_SIZE) {
					stream_rewind_unused(ctx, stream);
				}
				*offset_buffer = 0;
			}

			bytes_to_read = ctrl[0] > BSPATCH_CHUNK_SIZE ? BSPATCH_CHUNK_SIZE : ctrl[0];
			if (bytes_to_read > *bytes_written) {
				bytes_to_read = *bytes_written;
			}

			/* Read diff string */
			memcpy(new_data, patch + *offset_buffer, bytes_to_read);
			*offset_buffer += bytes_to_read;
			*bytes_written -= bytes_to_read;

			/* Switch to source firmware reading mode */
			ctx->memory.reading_patch = false;
			ret = ctx->read(&ctx->memory, old, bytes_to_read);
			if (ret != 0) {
				LOG_ERR("Can not read %d bytes at offset: %" PRIu64 ", ret = %d",
					bytes_to_read, ctx->memory.offset.source, ret);
				return ret;
			}

			/* Add old data to diff string */
			for (size_t i = 0; i < bytes_to_read; i++) {
				new_data[i] += old[i];
			}

			/* Write to new slot */
			ret = ctx->write(&ctx->memory, new_data, bytes_to_read, false);
			if (ret != 0) {
				LOG_ERR("Flash write error: %d", ret);
				return ret;
			}

			/* Adjust pointers and counters */
			newpos += bytes_to_read;
			ctrl[0] -= bytes_to_read;

			/* Update offset to read old firmware */
			oldpos += bytes_to_read;
			ret = ctx->seek(&ctx->memory, oldpos, ctx->memory.offset.patch, newpos);
			if (ret != 0) {
				LOG_ERR("Can not seek to source: %zu, patch: %" PRIu64, oldpos,
					ctx->memory.offset.patch);
				return ret;
			}
		}

		/* Sanity check */
		if (newpos + ctrl[1] > target_size) {
			LOG_ERR("Sanity check failed: newpos + ctrl[1] > target_size");
			return -EOVERFLOW;
		}

		/* Process extra block (ctrl[1]) */
		while (ctrl[1] > 0) {
			if (*bytes_written == 0) {
				int64_t decoded = ctrl[1] > DECOMPRESS_BUF_SIZE
							  ? DECOMPRESS_BUF_SIZE
							  : ctrl[1];

				ret = stream->decompress(ctx, patch, decoded, stream->hsd,
							 bytes_written, state, stream);
				if (ret != 0) {
					LOG_ERR("Error decompressing patch for ctrl[1]");
				}
				if (*bytes_written < DECOMPRESS_BUF_SIZE) {
					stream_rewind_unused(ctx, stream);
				}
				*offset_buffer = 0;
			}
			bytes_to_read = ctrl[1] > BSPATCH_CHUNK_SIZE ? BSPATCH_CHUNK_SIZE : ctrl[1];
			if (bytes_to_read > *bytes_written) {
				bytes_to_read = *bytes_written;
			}

			/* Read extra string */
			memcpy(new_data, patch + *offset_buffer, bytes_to_read);
			*offset_buffer += bytes_to_read;
			*bytes_written -= bytes_to_read;

			/* Write extra string to new file */
			ret = ctx->write(&ctx->memory, new_data, bytes_to_read,
					 newpos + bytes_to_read == target_size);
			if (ret != 0) {
				LOG_ERR("Flash write error: %d", ret);
				return ret;
			}

			/* Adjust pointers and counters */
			newpos += bytes_to_read;
			ctrl[1] -= bytes_to_read;
		}

		/* Update offset to read old firmware (ctrl[2] is the seek offset) */
		oldpos += ctrl[2];
		ret = ctx->seek(&ctx->memory, oldpos, ctx->memory.offset.patch, newpos);
		if (ret != 0) {
			LOG_ERR("Can not seek to source: %zu, patch: %" PRIu64, oldpos,
				ctx->memory.offset.patch);
			return ret;
		}
	}

	return ret;
}

/**
 * @brief Apply a bsdiff patch to create new firmware.
 *
 * @param ctx Pointer to delta API structure.
 * @return 0 on success, negative error code on failure.
 */
static int bsdiff_patch(struct delta_ctx *ctx)
{
	int ret;
	struct bspatch_stream stream;
	uint8_t patch[DECOMPRESS_BUF_SIZE];
	enum decoder_state state = STATE_SINK;
	size_t bytes_written = 0;
	size_t offset_buffer = 0;

	/* Validate heatshrink parameters */
	if (ctx->heatshrink_window_sz2 < 4 || ctx->heatshrink_window_sz2 > 15) {
		LOG_ERR("Invalid heatshrink window_sz2: %d (must be 4..15)",
			ctx->heatshrink_window_sz2);
		return -EINVAL;
	}
	if (ctx->heatshrink_lookahead_sz2 < 3 || ctx->heatshrink_lookahead_sz2 > 14) {
		LOG_ERR("Invalid heatshrink lookahead_sz2: %d (must be 3..14)",
			ctx->heatshrink_lookahead_sz2);
		return -EINVAL;
	}

#if defined(CONFIG_HEATSHRINK_DYNAMIC_ALLOC)
	heatshrink_decoder *hsd =
		heatshrink_decoder_alloc(HEATSHRINK_ALLOC_INPUT_SZ, ctx->heatshrink_window_sz2,
					 ctx->heatshrink_lookahead_sz2);

	if (hsd == NULL) {
		LOG_ERR("Failed to allocate heatshrink decoder");
		return -ENOMEM;
	}

	heatshrink_decoder_reset(hsd);
	stream.hsd = hsd;
#else
	heatshrink_decoder hsd;

	heatshrink_decoder_reset(&hsd);
	stream.hsd = &hsd;
#endif

	stream.decompress = decompress_patch;
	stream.offset = 0;
	stream.rd_buf_len = 0;
	stream.rd_buf_pos = 0;

	LOG_DBG("Heatshrink config: window_sz2=%d, lookahead_sz2=%d", ctx->heatshrink_window_sz2,
		ctx->heatshrink_lookahead_sz2);

	/* Apply the bspatch algorithm */
	ret = bspatch(ctx, ctx->target_size, patch, &offset_buffer, &bytes_written, &stream,
		      &state);
	if (ret != 0) {
		LOG_ERR("bspatch failed: %d", ret);
	}

#if defined(CONFIG_HEATSHRINK_DYNAMIC_ALLOC)
	heatshrink_decoder_free(hsd);
#endif

	return ret;
}

struct delta_backend_api delta_backend_bsdiff_api = {
	.patch = bsdiff_patch,
};
