/*
 * Copyright (c) 2026 Clovis Corde
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#if defined(CONFIG_DELTA_UPDATE)
#include <zephyr/delta/delta.h>
#endif

#include "delta_apply.h"

LOG_MODULE_REGISTER(delta_sample, LOG_LEVEL_INF);

static struct delta_ctx ctx;

int main(void)
{
	int ret;

	LOG_INF("Delta sample");

	ret = delta_apply(&ctx);
	if (ret != 0) {
		LOG_ERR("Error while applying delta update: %d", ret);
	}

	return ret;
}
