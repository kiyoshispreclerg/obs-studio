/******************************************************************************
    Per-source CPU cost profiler

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Measures, for every source and filter, the CPU time spent in its tick
 * callback and in its video render (self time, children excluded).
 *
 * Disabled by default. Enable with OBS_SOURCE_PROFILER=1. The report goes to
 * the log every OBS_SOURCE_PROFILER_INTERVAL seconds (default 60, 0 = only on
 * exit) and a cumulative report is written on shutdown.
 *
 * Note that render time is CPU time spent submitting GPU work, not GPU time.
 */

struct obs_source;

struct source_prof_frame {
	struct source_prof_frame *parent;
	uint64_t start_ns;
	uint64_t child_ns;
};

extern bool obs_source_profiler_enabled;

void source_profiler_init(void);
void source_profiler_dump_total(void);
void source_profiler_free(void);

/* graphics thread, once per frame, from tick_sources */
void source_profiler_frame(uint64_t now_ns);

void source_profiler_tick(struct obs_source *source, uint64_t elapsed_ns);
void source_profiler_render_begin(struct source_prof_frame *frame);
void source_profiler_render_end(struct obs_source *source,
				struct source_prof_frame *frame);

#ifdef __cplusplus
}
#endif
