# Copyright 2021 Google LLC
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(ipc_service)

zephyr_include_directories(./common)

FILE(GLOB app_sources src/*.c)
target_sources(app PRIVATE ${app_sources})

zephyr_sources_ifdef(CONFIG_IPC_SERVICE_ICMSG_V1		interoperability/icmsg_v1.c)
zephyr_sources_ifdef(CONFIG_PBUF_V1				interoperability/pbuf_v1.c)
zephyr_sources_ifdef(CONFIG_IPC_SERVICE_BACKEND_ICMSG_V1	interoperability/ipc_icmsg_v1.c)
