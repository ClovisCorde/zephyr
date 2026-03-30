/*
 * Copyright (c) 2026 Clovis Corde
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <errno.h>
#include <zephyr/delta/delta.h>

int delta_apply_patch_init(struct delta_ctx *ctx, delta_read_t read, delta_write_t write,
			   delta_seek_t seek, delta_mem_erase_t erase)
{
	if (ctx == NULL) {
		return -EINVAL;
	}

	ctx->read = read;
	ctx->write = write;
	ctx->seek = seek;
	ctx->erase = erase;

#if defined(CONFIG_BSDIFF_HS_ALGO)
	ctx->backend = delta_backend_bsdiff_api;
#else
	return -ENOSYS;
#endif

	return 0;
}
