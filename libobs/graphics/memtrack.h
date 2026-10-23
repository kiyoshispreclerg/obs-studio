/******************************************************************************
    Copyright (C) 2026 by Kiyoshi Spreclerg <kiyoshi_pip@protonmail.com>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
******************************************************************************/

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Optional GPU memory tracker.
 *
 * Every texture, stage surface and z-stencil buffer created through libobs
 * graphics is recorded with its estimated size, the source/filter that was
 * being created, updated, ticked or rendered on that thread at the time
 * (the "owner") and the code address that asked for it. A report grouped by
 * owner is written to the log, so it is possible to tell which source is
 * holding the GPU memory.
 *
 * Disabled by default. Enable with OBS_VRAM_PROFILER=1. The report is logged
 * whenever the tracked total changed, at most every OBS_VRAM_PROFILER_INTERVAL
 * seconds (default 10), and right away on SIGUSR1.
 *
 * Sizes are width * height * bpp (plus mip levels); driver padding and
 * memory not allocated through libobs (swap chains, driver internals,
 * shaders) are not counted. Imported X pixmaps/dma-bufs are reported apart,
 * as their memory belongs to another process.
 */

extern bool gs_memtrack_enabled;

enum gs_memtrack_kind {
	GS_MEMTRACK_TEXTURE,
	GS_MEMTRACK_RENDER_TARGET,
	GS_MEMTRACK_STAGESURF,
	GS_MEMTRACK_ZSTENCIL,
	GS_MEMTRACK_IMPORT,
};

void gs_memtrack_init(void);
void gs_memtrack_free(void);

struct gs_memtrack_owner {
	const char *name;
	const char *id;
	const char *parent; /* filters: the source the filter is on */
};

/* returns the previous owner, pass it back to gs_memtrack_pop_owner */
struct gs_memtrack_owner gs_memtrack_push_owner(const char *name,
						const char *id,
						const char *parent);
void gs_memtrack_pop_owner(struct gs_memtrack_owner prev);

void gs_memtrack_add(const void *obj, enum gs_memtrack_kind kind,
		     uint32_t width, uint32_t height, uint32_t depth,
		     int format, uint64_t bytes);
void gs_memtrack_remove(const void *obj);

/* graphics thread, once per frame */
void gs_memtrack_frame(uint64_t now_ns);
void gs_memtrack_dump(const char *reason);

#ifdef __cplusplus
}
#endif
