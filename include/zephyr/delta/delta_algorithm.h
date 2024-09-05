/*
 * Copyright (c) 2025 Clovis Corde
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef ZEPHYR_INCLUDE_DELTA_DELTA_ALGORITHM_H_
#define ZEPHYR_INCLUDE_DELTA_DELTA_ALGORITHM_H_

struct delta_api_t;

/**
 * @brief backend API to define the delta algorithm used for delta firmware update.
 */
struct delta_backend_api_t{
	int (*patch)(struct delta_api_t *self_p);
};

#endif /* ZEPHYR_INCLUDE_DELTA_DELTA_ALGORITHM_H_ */
