/*
 * Copyright (c) 2025 Clovis Corde
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <errno.h>
#include <zephyr/delta/delta.h>

int delta_apply_patch_init(struct delta_api_t *self_p, delta_read_t read, delta_write_t write,
			   delta_seek_t seek, delta_mem_erase_t erase)
{

	self_p->read = read;
	self_p->write = write;
	self_p->seek = seek;
	self_p->erase = erase;
#if defined(CONFIG_BSDIFF_ALGO)
	self_p->backend = delta_backend_bsdiff_api;
#else
	return -ENOSYS;
#endif

	return (0);
}
