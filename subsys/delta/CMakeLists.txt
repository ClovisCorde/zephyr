# SPDX-License-Identifier: Apache-2.0

# delta.c depends on the include directories exposed by the delta library.
# Hence, if it exists then the source files are added to that otherwise
# a library with same name is created.

zephyr_library_named(delta)

zephyr_library_sources_ifdef(CONFIG_DELTA_UPDATE delta.c)
zephyr_library_sources_ifdef(BSDIFF_ALGO backend_diff/delta_backend_bsdiff.c)

add_subdirectory(backend_diff)
