/**
 * @file
 * @brief Implementation of the bsdiff algorithm using the delta API.
 * @author Clovis Corde
 * @date 2025
 */

#ifdef CONFIG_DELTA_UPDATE
#include <zephyr/delta/delta.h>
#endif

#ifdef CONFIG_HEATSHRINK
#include "heatshrink_decoder.h"
#endif

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(delta_backend_bsdiff, CONFIG_DELTA_UPDATE_LOG_LEVEL);

#define HEADER_BLOCK_SIZE  8
#define PATCH_HEADER_SIZE  16
#define CONTROL_DATA_SIZE  8
#define CONTROL_DATA_NB    3
#define INPUT_READ_SIZE    1
#define OUTPUT_READ_SIZE   256
#define OUTPUT_BUFFER_SIZE 2048

/**
 * @enum decoder_state_t
 * @brief Defines the state for the decoder.
 */
typedef enum {
	STATE_SINK,  /* read and sink more data */
	STATE_POLL,  /* poll the data sunk */
	STATE_FINISH /* no more input, handle the last data sunk*/
} decoder_state_t;

/**
 * @struct bspatch_stream
 * @brief Contains data required for the bspatch stream.
 */
struct bspatch_stream {
	heatshrink_decoder *hsd;
	size_t bytes_decompressed;
	size_t offset;
	int (*decompress)(struct bspatch_stream *stream, struct delta_api_t *self_p,
			  uint8_t *output_buffer, size_t output_buffer_size,
			  heatshrink_decoder *hsd, size_t *bytes_written, decoder_state_t *state);
};

/**
 * @brief Decompresses a patch using heatshrink decoder.
 * @param stream Pointer to the bspatch stream.
 * @param self_p Pointer to delta API structure.
 * @param output_buffer Buffer to write decompressed data.
 * @param output_buffer_size Size of the output buffer.
 * @param hsd Heatshrink decoder.
 * @param bytes_written Number of bytes written after decompression.
 * @param state Current decoder state.
 * @return Status of the decompression operation.
 */
static int decompress_patch(struct bspatch_stream *stream, struct delta_api_t *self_p,
			    uint8_t *output_buffer, size_t output_buffer_size,
			    heatshrink_decoder *hsd, size_t *bytes_written, decoder_state_t *state)
{
	uint8_t input_buffer[INPUT_READ_SIZE];
	size_t read_sz = 0;
	size_t write_sz = 0;
	size_t offset_buffer = 0;
	size_t bytes_to_decode = output_buffer_size;
	int ret = 0;

	/* Read in the partition where the patch is stored */
	self_p->memory.slot = PATCH_STORAGE;

	*state = STATE_SINK;
	while (true) {
		switch (*state) {
		case STATE_SINK:
			/* read INPUT_READ_SIZE bytes from the patch */
			ret = self_p->read(&self_p->memory, input_buffer, INPUT_READ_SIZE);
			if (ret != 0) {
				LOG_ERR("Can't read %d bytes in slot1 partition at offset : "
					"%lld, ret = %d\n",
					INPUT_READ_SIZE, self_p->memory.offset.patch, ret);
				return ret;
			}

			/* update the offset in the patch */
			ret = self_p->seek(&self_p->memory, self_p->memory.offset.source,
					   self_p->memory.offset.patch + INPUT_READ_SIZE,
					   self_p->memory.offset.target);
			if (ret != 0) {
				LOG_ERR("Can't seek at offset for source : %d and for patch : "
					"%lld, ret = %d\n",
					0, self_p->memory.offset.patch + INPUT_READ_SIZE, ret);
				return ret;
			}

			HSD_sink_res sink_res = heatshrink_decoder_sink(hsd, input_buffer,
									INPUT_READ_SIZE, &read_sz);
			if (sink_res != 0) {
				if (sink_res == 1) {
					LOG_ERR("Out of space in internal buffer");
					return sink_res;
				} else if (sink_res == -1) {
					LOG_ERR("NULL Argument");
					return sink_res;
				} else {
					LOG_ERR("Unknown error");
					return sink_res;
				}
			}
			*state = STATE_POLL;
			break;
		case STATE_POLL: {
			HSD_poll_res poll_res = HSDR_POLL_MORE;
			while (poll_res != HSDR_POLL_EMPTY && bytes_to_decode > 0) {
				size_t to_decode =
					output_buffer_size - offset_buffer < OUTPUT_READ_SIZE
						? output_buffer_size - offset_buffer
						: OUTPUT_READ_SIZE;

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
	return -ENOTSUP;
}

/**
 * @brief Convert a byte buffer into a 64-bit integer.
 * @param buf Byte buffer.
 * @return 64-bit integer representation of the buffer.
 */
static int64_t offtin(uint8_t *buf)
{
	int64_t y;

	/* Start with the last byte (buf[7]) and use a mask (0x7F) to keep only the lower 7 bits
	 * (ignoring the sign bit)*/
	y = buf[7] & 0x7F;
	/* Accumulate the bytes into "y". Each byte is shifted left by multiplying it by 256 before
	 * adding the next byte. */
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

	/* Check if the highest bit of buf[7] is set with the mask : 0x80.
	 *  If so, it indicates a negative number (in two's complement),
	 *  then 'y' is converted to its positive value.
	 */
	if (buf[7] & 0x80) {
		y = -y;
	}

	return y;
}

/**
 * @brief Apply the BSDiff patch.
 * @param delta_apply Pointer to delta API structure.
 * @param new_size Size of the target file.
 * @param patch Patch data.
 * @param offset_buffer Offset in the output buffer.
 * @param bytes_written Number of bytes written to the output buffer.
 * @param stream Pointer to the bspatch stream.
 * @param state Current decoder state.
 * @return Status of the patching operation.
 */
static int bspatch(struct delta_api_t *delta_apply, int64_t new_size, uint8_t *patch,
		   size_t *offset_buffer, size_t *bytes_written, struct bspatch_stream *stream,
		   decoder_state_t *state)
{
	uint8_t buf[CONTROL_DATA_SIZE];
	size_t oldpos = 0;
	size_t newpos = 0;
	int64_t ctrl[CONTROL_DATA_NB];
	uint16_t bytes_to_read = OUTPUT_BUFFER_SIZE;
	uint8_t old[OUTPUT_BUFFER_SIZE];
	uint8_t new[OUTPUT_BUFFER_SIZE];
	int ret;

	if (!delta_apply) {
		return -EFAULT;
	}

	while (newpos < new_size) {
		/* Decompress the control data (ctrl[0] = diff, ctrl[1] = extra, ctrl[2] = offset in
		 * source firmware) */
		if (*bytes_written == 0) {
			ret = stream->decompress(stream, delta_apply, patch,
						 CONTROL_DATA_SIZE * CONTROL_DATA_NB, stream->hsd,
						 bytes_written, state);
			if (ret != 0) {
				LOG_ERR("Error decompressing patch for control data");
			}
			heatshrink_decoder_reset(stream->hsd);
			*offset_buffer = 0;
		}
		/* Read control data */
		for (size_t i = 0; i < CONTROL_DATA_NB; i++) {
			memcpy(buf, patch + *offset_buffer, CONTROL_DATA_SIZE);
			*offset_buffer += CONTROL_DATA_SIZE;
			*bytes_written -= CONTROL_DATA_SIZE;
			ctrl[i] = offtin(buf);
		};
		LOG_DBG("ctrl[0] =%lld, ctrl[1] = %lld, ctrl[2] = %lld", ctrl[0], ctrl[1], ctrl[2]);

		/* Sanity-check */
		if (ctrl[0] < 0 || ctrl[0] > INT_MAX || ctrl[1] < 0 || ctrl[1] > INT_MAX ||
		    newpos + ctrl[0] > new_size) {
			LOG_ERR("Sanity check failed on ctrl[0] or ctrl[1]");
			return -EOVERFLOW;
		}

		while (ctrl[0] > 0) {
			/* Decompress more data for ctrl[0] */
			if (*bytes_written == 0) {
				int64_t decoded =
					ctrl[0] > OUTPUT_BUFFER_SIZE ? OUTPUT_BUFFER_SIZE : ctrl[0];
				ret = stream->decompress(stream, delta_apply, patch, decoded,
							 stream->hsd, bytes_written, state);
				if (ret != 0) {
					LOG_ERR("Error decompressing patch for ctrl[0]");
				}
				/* If we decompressed less bytes than the maximum possible that
				 * means we decompressed all the data for this ctrl[0] block */
				if (*bytes_written < OUTPUT_BUFFER_SIZE) {
					heatshrink_decoder_reset(stream->hsd);
				}
				*offset_buffer = 0;
			}
			bytes_to_read = ctrl[0] > OUTPUT_READ_SIZE ? OUTPUT_READ_SIZE : ctrl[0];
			if (bytes_to_read > *bytes_written) {
				bytes_to_read = *bytes_written;
			}

			/* read diff string */
			memcpy(new, patch + *offset_buffer, bytes_to_read);
			*offset_buffer += bytes_to_read;
			*bytes_written -= bytes_to_read;

			/* Read old data from source firmware (on slot 0) */
			delta_apply->memory.slot = SLOT_0;
			ret = delta_apply->read(&delta_apply->memory, old, bytes_to_read);
			if (ret != 0) {
				LOG_ERR("Can not read %d bytes in slot0 partition at offset : "
					"%lld, ret = %d\n",
					bytes_to_read, delta_apply->memory.offset.source, ret);
				return ret;
			}

			/* Add old data to diff string */
			for (size_t i = 0; i < bytes_to_read; i++) {
				new[i] += old[i];
			}

			/* Write on the new slot */
			ret = delta_apply->write(&delta_apply->memory, new, bytes_to_read, false);
			if (ret < 0) {
				LOG_ERR("Flash write error: %d\n", ret);
				return ret;
			}

			/* Adjust pointers and counters */
			newpos += bytes_to_read;
			ctrl[0] -= bytes_to_read;

			/* Update offset to read old firmware */
			oldpos += bytes_to_read;
			ret = delta_apply->seek(&delta_apply->memory, oldpos,
						delta_apply->memory.offset.patch, newpos);
			if (ret != 0) {
				LOG_ERR("Can not seek at offset for source : %d and for patch : "
					"%lld, ret = %d",
					oldpos, delta_apply->memory.offset.patch, ret);
				return ret;
			}
		}

		/* Sanity-check */
		if (newpos + ctrl[1] > new_size) {
			LOG_ERR("Sanity check failed on newpos + ctrl[1] > new_size");
			return -EOVERFLOW;
		}

		while (ctrl[1] > 0) {
			/* Decompress more data for ctrl[1] */
			if (*bytes_written == 0) {
				int64_t decoded =
					ctrl[1] > OUTPUT_BUFFER_SIZE ? OUTPUT_BUFFER_SIZE : ctrl[1];
				ret = stream->decompress(stream, delta_apply, patch, decoded,
							 stream->hsd, bytes_written, state);
				if (ret != 0) {
					LOG_ERR("Error decompressing patch for ctrl[1]");
				}
				/* If we decompressed less bytes than the maximum possible that
				 * means we decompressed all the data for this ctrl[1] block */
				if (*bytes_written < OUTPUT_BUFFER_SIZE) {
					heatshrink_decoder_reset(stream->hsd);
				}
				*offset_buffer = 0;
			}
			bytes_to_read = ctrl[1] > OUTPUT_READ_SIZE ? OUTPUT_READ_SIZE : ctrl[1];
			if (bytes_to_read > *bytes_written) {
				bytes_to_read = *bytes_written;
			}

			/* Read extra string */
			memcpy(new, patch + *offset_buffer, bytes_to_read);
			*offset_buffer += bytes_to_read;
			*bytes_written -= bytes_to_read;

			/* Write the extra string to the new file */
			ret = delta_apply->write(&delta_apply->memory, new, bytes_to_read,
						 newpos + bytes_to_read == new_size);
			if (ret < 0) {
				LOG_ERR("Flash write error: %d\n", ret);
				return ret;
			}

			/* Adjust pointers and counters */
			newpos += bytes_to_read;
			ctrl[1] -= bytes_to_read;
		}

		/* Update offset to read old firmware */
		oldpos += ctrl[2];
		ret = delta_apply->seek(&delta_apply->memory, oldpos,
					delta_apply->memory.offset.patch, newpos);
		if (ret != 0) {
			LOG_ERR("Can not seek at offset for source : %d and for patch : %lld, ret "
				"= %d\n",
				oldpos, delta_apply->memory.offset.patch, ret);
			return ret;
		}
	};
	return 0;
}

/**
 * @brief Function to apply a bsdiff patch.
 * @param self_p Pointer to delta API structure.
 * @return Status of the bsdiff patching operation.
 */
int bsdiff_patch(struct delta_api_t *self_p)
{
	if (!self_p) {
		return -EFAULT;
	}

	int ret;
	struct bspatch_stream stream;
	uint8_t patch[OUTPUT_BUFFER_SIZE];
	heatshrink_decoder hsd;
	decoder_state_t state = STATE_SINK;
	size_t bytes_written = 0;
	size_t offset_buffer = 0;

	/* Init heatshrink decompression */
	heatshrink_decoder_reset(&hsd);
	stream.hsd = &hsd;
	stream.decompress = decompress_patch;
	stream.offset = 0;

	/* Apply the bspatch algo*/
	ret = bspatch(self_p, self_p->memory.offset.new_size, patch, &offset_buffer, &bytes_written,
		      &stream, &state);
	if (ret != 0) {
		LOG_ERR("bspatch failed\n");
		return ret;
	}

	return ret;
}

struct delta_backend_api_t delta_backend_bsdiff_api = {
	.patch = bsdiff_patch,
};
