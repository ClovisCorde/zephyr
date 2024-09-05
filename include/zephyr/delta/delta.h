/** @file
 *  @brief Delta Firmware Update API
 */
/*
 * Copyright (c) 2025 Clovis Corde
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef ZEPHYR_INCLUDE_DELTA_H_
#define ZEPHYR_INCLUDE_DELTA_H_

#include <zephyr/kernel.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdbool.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/dfu/flash_img.h>
#include <zephyr/delta/delta_algorithm.h>
#if defined(CONFIG_BSDIFF_ALGO)
extern struct delta_backend_api_t delta_backend_bsdiff_api;
#endif

/*
 * Choose the slot partition to read flash memory
 */
typedef enum {
	SLOT_0 = FIXED_PARTITION_ID(slot0_partition),
	SLOT_1 = FIXED_PARTITION_ID(slot1_partition),
	PATCH_STORAGE = FIXED_PARTITION_ID(patch_partition),
} delta_image_slot_type_t;

/*
 * Structure to apply delta using flash memory
 */
typedef struct {
	const struct flash_area *flash_area;
	struct flash_img_context img_ctx;
} delta_flash_struct_t;

/*
 * Structure to apply delta using ram memory
 */
typedef struct {
	uint8_t patch[CONFIG_DELTA_TRANSFER_PATCH_SIZE];
} delta_ram_struct_t;

/*
 * Structure to handle offsets in the 3 partitions
 */
typedef struct {
	uint64_t source;
	uint64_t target;
	uint64_t patch;
	int64_t new_size;
} delta_offset_struct_t;

/*
 * Structure of the delta memory
 */
typedef struct {
	delta_offset_struct_t offset;
	delta_image_slot_type_t slot;

	delta_flash_struct_t flash;
	delta_ram_struct_t ram;

} delta_memory_struct_t;

/**
 * Read callback.
 *
 * @param[in] arg_p User data passed to delta_apply_patch_init().
 * @param[out] buf_p Buffer to read into.
 * @param[in] size Number of bytes to read.
 *
 * @return zero(0) or negative error code.
 */
typedef int (*delta_read_t)(delta_memory_struct_t *arg_p, uint8_t *buf_p, size_t size);

/**
 * Write callback.
 *
 * @param[in] arg_p User data passed to delta_apply_patch_init().
 * @param[in] buf_p Buffer to write.
 * @param[in] size Number of bytes to write.
 *
 * @return zero(0) or negative error code.
 */
typedef int (*delta_write_t)(delta_memory_struct_t *arg_p, uint8_t *buf_p, size_t size, bool flush);

/**
 * Memory erase callback.
 *
 * @param[in] arg_p User data passed to delta_apply_patch_init().
 * @param[in] addr Address to erase from.
 * @param[in] size Number of bytes to erase.
 *
 * @return zero(0) or negative error code.
 */
typedef int (*delta_mem_erase_t)(delta_memory_struct_t *arg_p, uint64_t offset, size_t size);

/**
 * Seek from current position callback.
 *
 * @param[in] arg_p User data passed to delta_apply_patch_init().
 * @param[in] offset Offset to seek to from current position.
 *
 * @return zero(0) or negative error code.
 */
typedef int (*delta_seek_t)(delta_memory_struct_t *arg_p, size_t source_offset, size_t patch_offset,
			    size_t target_offset);

/**
 * The delta api data structure.
 */
struct delta_api_t {
	delta_read_t read;
	delta_write_t write;
	delta_seek_t seek;
	delta_mem_erase_t erase;
	struct delta_backend_api_t backend;
	delta_memory_struct_t memory;
	uint8_t heatshrink_window_sz2;
	uint8_t heatshrink_lookahead_sz2;
};

/**
 * Initialize given apply patch object.
 *
 * @param[out] self_p Apply patch object to initialize.
 * @param[in] read Callback to read from-data.
 * @param[in] write Callback to seek from current position in from-data.
 * @param[in] seek Callback to seek from current position in from-data.
 * @param[in] erase Callback to erase.
 * @param[in] memory Argument passed to the callbacks.
 *
 * @return zero(0) or negative error code.
 */
int delta_apply_patch_init(struct delta_api_t *self_p, delta_read_t read, delta_write_t write,
			   delta_seek_t seek, delta_mem_erase_t erase);

#endif /* ZEPHYR_INCLUDE_DELTA_H_ */
