# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
if(BOARD STREQUAL "unit_testing" OR BOARD STREQUAL "unit_testing/unit_testing")
  find_package(Zephyr COMPONENTS unittest REQUIRED HINTS $ENV{ZEPHYR_BASE})
  project(base)

  # This test suite depends on having this CONFIG_ value, so define it
  add_definitions( -DCONFIG_BUGxxxxx )
  target_sources(testbinary PRIVATE src/main.c)
else()
  find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
  project(base)

  if(CONFIG_CPP)
    message(STATUS "adding main.cpp")
    target_sources(app PRIVATE src/main.cpp)
  else()
    target_sources(app PRIVATE src/main.c)

    target_sources_ifdef(CONFIG_USERSPACE app PRIVATE src/main_userspace.c)
  endif()
endif()
