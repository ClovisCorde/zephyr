/*
 * Copyright (c) 2026 Clovis Corde
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/dfu/mcuboot.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/delta/delta.h>

#define BSDIFF_MAGIC                  "BSDIFFHS"
#define BSDIFF_MAGIC_SIZE             8
#define TARGET_FIRMWARE_SIZE          8
#define HEATSHRINK_WINDOW_SZ2_SIZE    1
#define HEATSHRINK_LOOKAHEAD_SZ2_SIZE 1
#define BSDIFF_HEADER_SIZE                                                                         \
	(BSDIFF_MAGIC_SIZE + TARGET_FIRMWARE_SIZE + HEATSHRINK_WINDOW_SZ2_SIZE +                   \
	 HEATSHRINK_LOOKAHEAD_SZ2_SIZE)

LOG_MODULE_REGISTER(delta_apply, CONFIG_DELTA_UPDATE_LOG_LEVEL);

static int delta_mem_read(struct delta_memory *mem, uint8_t *buf, size_t size);
static int delta_mem_write(struct delta_memory *mem, uint8_t *buf, size_t size, bool flush);
static int delta_mem_erase(struct delta_memory *mem, uint64_t offset, size_t size);
static int delta_mem_seek(struct delta_memory *mem, size_t source_offset, size_t patch_offset,
			  size_t target_offset);

/**
 * @brief Check that a valid patch is present.
 *
 * Header of the patch:
 * - MAGIC: BSDIFFHS (8 bytes)
 * - Size of the target firmware (8 bytes, little-endian)
 * - Heatshrink window_sz2 configuration (1 byte)
 * - Heatshrink lookahead_sz2 configuration (1 byte)
 *
 * @param ctx Pointer to the delta context structure.
 * @return true if a valid patch is present, false otherwise.
 */
static bool valid_patch_present(struct delta_ctx *ctx)
{
	uint8_t header[BSDIFF_HEADER_SIZE];
	int ret;
	int64_t target_size = 0;

	/* Switch to patch reading mode */
	ctx->memory.reading_patch = true;

	ret = ctx->read(&ctx->memory, header, BSDIFF_HEADER_SIZE);
	if (ret != 0) {
		LOG_ERR("Failed to read patch (%d)", ret);
		return false;
	}

	/* Check magic */
	if (memcmp(header, BSDIFF_MAGIC, BSDIFF_MAGIC_SIZE) != 0) {
		LOG_INF("Wrong magic in the patch");
		return false;
	}

	/* Read target_size (little-endian 64-bit) from bytes 8-15 */
	for (int i = 0; i < TARGET_FIRMWARE_SIZE; i++) {
		target_size |= (int64_t)header[BSDIFF_MAGIC_SIZE + i] << (8 * i);
	}
	ctx->target_size = target_size;

	/* Read heatshrink configuration from bytes 16-17 */
	ctx->heatshrink_window_sz2 = header[BSDIFF_MAGIC_SIZE + TARGET_FIRMWARE_SIZE];
	ctx->heatshrink_lookahead_sz2 =
		header[BSDIFF_MAGIC_SIZE + TARGET_FIRMWARE_SIZE + HEATSHRINK_WINDOW_SZ2_SIZE];

	LOG_INF("Valid patch detected in patch_partition");
	LOG_INF("Target size: %lld, window_sz2: %d, lookahead_sz2: %d", ctx->target_size,
		ctx->heatshrink_window_sz2, ctx->heatshrink_lookahead_sz2);

	ctx->memory.offset.patch = BSDIFF_HEADER_SIZE;

	return true;
}

/**
 * @brief Initialize all the callbacks for the delta API.
 *
 * @param ctx Pointer to the delta context structure.
 * @return 0 on success, negative error code on failure.
 */
static int delta_apply_init(struct delta_ctx *ctx)
{
	int ret;

	if (ctx == NULL) {
		return -EINVAL;
	}

	/* The patch is stored in flash in this example */
	ctx->memory.patch_storage = DELTA_PATCH_STORAGE_FLASH;

	/* Init flash img to write the new firmware (on slot 1) */
	ret = flash_img_init(&ctx->memory.flash.img_ctx);
	if (ret != 0) {
		LOG_ERR("Can't initialise flash img, ret = %d", ret);
		return ret;
	}

	/* Init all the callbacks for the delta api */
	ret = delta_apply_patch_init(ctx, delta_mem_read, delta_mem_write, delta_mem_seek,
				     delta_mem_erase);
	if (ret != 0) {
		LOG_ERR("delta apply patch failed during initialization, ret: %d", ret);
		return ret;
	}

	/* Init the offsets for source, patch and target at 0 */
	ret = ctx->seek(&ctx->memory, 0, 0, 0);
	if (ret != 0) {
		LOG_ERR("delta api seek offset failed, ret = %d", ret);
		return ret;
	}

	return 0;
}

/**
 * @brief Apply the patch using the delta algorithm defined in backend and
 *        write the magic to define the new firmware as a bootable image.
 *
 * @param ctx Pointer to the delta context structure.
 * @return 0 on success, negative error code on failure.
 */
static int delta_apply_algo(struct delta_ctx *ctx)
{
	int ret;

	if (ctx == NULL) {
		return -EINVAL;
	}

	/* Apply the patch using a delta algorithm */
	ret = ctx->backend.patch(ctx);
	if (ret != 0) {
		LOG_ERR("apply patch failed");
		return ret;
	}

	ret = boot_request_upgrade(BOOT_UPGRADE_PERMANENT);
	if (ret != 0) {
		LOG_ERR("Boot request error: %d", ret);
		return ret;
	}

	return 0;
}

int delta_apply(struct delta_ctx *ctx)
{
	int ret;

	if (ctx == NULL) {
		return -EINVAL;
	}

	ret = delta_apply_init(ctx);
	if (ret != 0) {
		LOG_ERR("The delta API initialization failed, ret = %d", ret);
		return ret;
	}

	if (!valid_patch_present(ctx)) {
		LOG_INF("No valid patch in patch_partition");
		return -ECANCELED;
	}

	ret = delta_apply_algo(ctx);
	if (ret != 0) {
		LOG_ERR("The delta application algorithm failed, ret = %d", ret);
		return ret;
	}

	LOG_INF("The new FW was successfully written, now rebooting...");

	/* Flush the logs before reboot */
	LOG_PANIC();

	sys_reboot(SYS_REBOOT_COLD);

	return ret;
}

static int delta_mem_read(struct delta_memory *mem, uint8_t *buf, size_t size)
{
	int ret = 0;

	if (!mem->reading_patch) {
		ret = flash_area_open(PARTITION_ID(slot0_partition), &mem->flash.flash_area);
		if (ret != 0) {
			LOG_ERR("Can not open the flash area for slot 0");
			return -ENODEV;
		}

		ret = flash_area_read(mem->flash.flash_area, mem->offset.source, buf, size);
		if (ret != 0) {
			LOG_ERR("Can not read %zu bytes at offset: %" PRIu64, size,
				mem->offset.source);
			flash_area_close(mem->flash.flash_area);
			return -EINVAL;
		}
		flash_area_close(mem->flash.flash_area);
	} else if (mem->patch_storage == DELTA_PATCH_STORAGE_FLASH) {
		ret = flash_area_open(PARTITION_ID(patch_partition), &mem->flash.flash_area);
		if (ret != 0) {
			LOG_ERR("Can not open the patch partition");
			return -ENODEV;
		}

		if (mem->offset.patch + size > mem->flash.flash_area->fa_size) {
			LOG_ERR("Read operation exceeds patch partition size");
			flash_area_close(mem->flash.flash_area);
			return -EINVAL;
		}

		ret = flash_area_read(mem->flash.flash_area, mem->offset.patch, buf, size);
		if (ret != 0) {
			LOG_ERR("Can not read %zu bytes at offset: %" PRIu64, size,
				mem->offset.patch);
			flash_area_close(mem->flash.flash_area);
			return -EINVAL;
		}
		flash_area_close(mem->flash.flash_area);
	} else {
		if (mem->offset.patch + size > mem->ram.patch_size) {
			LOG_ERR("Read operation exceeds RAM patch buffer size");
			return -EINVAL;
		}

		memcpy(buf, mem->ram.patch + mem->offset.patch, size);
	}

	return ret;
}

static int delta_mem_write(struct delta_memory *mem, uint8_t *buf, size_t size, bool flush)
{
	int ret;

	ret = flash_img_buffered_write(&mem->flash.img_ctx, buf, size, flush);
	if (ret != 0) {
		LOG_ERR("Flash write error");
		return -EIO;
	}

	return ret;
}

static int delta_mem_erase(struct delta_memory *mem, uint64_t offset, size_t size)
{
	int ret;

	ret = flash_area_open(mem->slot, &mem->flash.flash_area);
	if (ret != 0) {
		LOG_ERR("Can not open the flash area for %d", mem->slot);
		return -ENODEV;
	}

	ret = flash_area_erase(mem->flash.flash_area, offset, size);
	if (ret != 0) {
		LOG_ERR("Can not erase the flash area, ret = %d", ret);
		flash_area_close(mem->flash.flash_area);
		return -EIO;
	}
	flash_area_close(mem->flash.flash_area);

	return ret;
}

static int delta_mem_seek(struct delta_memory *mem, size_t source_offset, size_t patch_offset,
			  size_t target_offset)
{
	mem->offset.source = source_offset;
	mem->offset.patch = patch_offset;
	mem->offset.target = target_offset;

	return 0;
}
