/*
 * Copyright (c) 2023 BrainCo Inc.
 * Copyright (c) 2025 Foss Analytical A/S
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#if DT_NODE_HAS_STATUS(DT_NODELABEL(sdram2), okay)
GROUP_START(SDRAM2)

	SECTION_PROLOGUE(_STM32_SDRAM2_SECTION_NAME, (NOLOAD),)
	{
		*(.lvgl_buf)
	} GROUP_LINK_IN(SDRAM2)

GROUP_END(SDRAM2)
#endif
