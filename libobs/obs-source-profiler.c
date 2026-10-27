/******************************************************************************
    Per-source CPU cost profiler

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#include <stdlib.h>
#include <string.h>

#include "obs-internal.h"
#include "obs-source-profiler.h"
#include "util/threading.h"
#include "util/platform.h"
#include "util/darray.h"

#define DEFAULT_INTERVAL_SEC 60
#define TOP_SOURCES 25
#define TOP_TYPES 15

#define LOG_TAG "[source-profiler] "

struct prof_acc {
	uint64_t tick_ns;
	uint64_t render_ns;
	uint64_t tick_max_ns;
	uint64_t render_max_ns;
};

struct source_prof_entry {
	enum obs_source_type type;
	char *id;
	char *name;
	char *parent;
	struct prof_acc win;
	struct prof_acc total;
};

struct prof_row {
	const struct source_prof_entry *entry;
	struct prof_acc acc;
	size_t instances;
};

static struct {
	pthread_mutex_t mutex;
	DARRAY(struct source_prof_entry *) entries;
	uint64_t win_frames;
	uint64_t total_frames;
	uint64_t win_start_ns;
	uint64_t start_ns;
	uint64_t interval_ns;
} prof;

static THREAD_LOCAL struct source_prof_frame *cur_frame = NULL;

bool obs_source_profiler_enabled = false;

static inline const char *safe_str(const char *str)
{
	return str ? str : "";
}

void source_profiler_init(void)
{
	const char *env = getenv("OBS_SOURCE_PROFILER");
	if (!env || !*env || strcmp(env, "0") == 0)
		return;

	long interval = DEFAULT_INTERVAL_SEC;
	const char *interval_env = getenv("OBS_SOURCE_PROFILER_INTERVAL");
	if (interval_env && *interval_env)
		interval = strtol(interval_env, NULL, 10);
	if (interval < 0)
		interval = 0;

	pthread_mutex_init(&prof.mutex, NULL);
	prof.interval_ns = (uint64_t)interval * 1000000000ULL;
	prof.start_ns = prof.win_start_ns = os_gettime_ns();
	obs_source_profiler_enabled = true;

	blog(LOG_INFO,
	     LOG_TAG "enabled (interval: %ld s). Times are CPU time on the "
		     "graphics thread; render excludes child sources/filters.",
	     interval);
}

static struct source_prof_entry *get_entry(obs_source_t *source)
{
	if (source->prof)
		return source->prof;

	/* a filter that is not attached yet has no parent to identify it */
	if (source->info.type == OBS_SOURCE_TYPE_FILTER &&
	    !source->filter_parent)
		return NULL;

	const char *id = safe_str(source->info.id);
	const char *name = safe_str(obs_source_get_name(source));
	const char *parent =
		source->filter_parent
			? safe_str(obs_source_get_name(source->filter_parent))
			: "";
	struct source_prof_entry *found = NULL;

	pthread_mutex_lock(&prof.mutex);

	for (size_t i = 0; i < prof.entries.num; i++) {
		struct source_prof_entry *e = prof.entries.array[i];
		if (e->type == source->info.type && strcmp(e->id, id) == 0 &&
		    strcmp(e->name, name) == 0 &&
		    strcmp(e->parent, parent) == 0) {
			found = e;
			break;
		}
	}

	if (!found) {
		found = bzalloc(sizeof(*found));
		found->type = source->info.type;
		found->id = bstrdup(id);
		found->name = bstrdup(name);
		found->parent = bstrdup(parent);
		da_push_back(prof.entries, &found);
	}

	pthread_mutex_unlock(&prof.mutex);

	source->prof = found;
	return found;
}

static inline void acc_tick(struct prof_acc *acc, uint64_t ns)
{
	acc->tick_ns += ns;
	if (ns > acc->tick_max_ns)
		acc->tick_max_ns = ns;
}

static inline void acc_render(struct prof_acc *acc, uint64_t ns)
{
	acc->render_ns += ns;
	if (ns > acc->render_max_ns)
		acc->render_max_ns = ns;
}

void source_profiler_tick(struct obs_source *source, uint64_t elapsed_ns)
{
	struct source_prof_entry *e = get_entry(source);
	if (!e)
		return;

	acc_tick(&e->win, elapsed_ns);
	acc_tick(&e->total, elapsed_ns);
}

void source_profiler_render_begin(struct source_prof_frame *frame)
{
	frame->parent = cur_frame;
	frame->child_ns = 0;
	cur_frame = frame;
	frame->start_ns = os_gettime_ns();
}

void source_profiler_render_end(struct obs_source *source,
				struct source_prof_frame *frame)
{
	uint64_t elapsed = os_gettime_ns() - frame->start_ns;

	cur_frame = frame->parent;
	if (frame->parent)
		frame->parent->child_ns += elapsed;

	uint64_t self = elapsed > frame->child_ns ? elapsed - frame->child_ns
						  : 0;

	struct source_prof_entry *e = get_entry(source);
	if (!e)
		return;

	acc_render(&e->win, self);
	acc_render(&e->total, self);
}

static const char *type_name(enum obs_source_type type)
{
	switch (type) {
	case OBS_SOURCE_TYPE_INPUT:
		return "input";
	case OBS_SOURCE_TYPE_FILTER:
		return "filter";
	case OBS_SOURCE_TYPE_TRANSITION:
		return "transition";
	case OBS_SOURCE_TYPE_SCENE:
		return "scene";
	}
	return "?";
}

static int cmp_rows(const void *a, const void *b)
{
	const struct prof_row *ra = a;
	const struct prof_row *rb = b;
	uint64_t ca = ra->acc.tick_ns + ra->acc.render_ns;
	uint64_t cb = rb->acc.tick_ns + rb->acc.render_ns;

	return (ca < cb) - (ca > cb);
}

static inline double per_frame_ms(uint64_t ns, uint64_t frames)
{
	return frames ? (double)ns / 1000000.0 / (double)frames : 0.0;
}

static inline double to_ms(uint64_t ns)
{
	return (double)ns / 1000000.0;
}

static void log_row(const struct prof_row *row, uint64_t frames, bool by_type)
{
	const struct source_prof_entry *e = row->entry;
	const struct prof_acc *a = &row->acc;

	if (by_type) {
		blog(LOG_INFO,
		     LOG_TAG "%7.3f  (tick %6.3f + render %6.3f)  "
			     "max %6.2f/%6.2f  %-10s %-26s x%zu",
		     per_frame_ms(a->tick_ns + a->render_ns, frames),
		     per_frame_ms(a->tick_ns, frames),
		     per_frame_ms(a->render_ns, frames), to_ms(a->tick_max_ns),
		     to_ms(a->render_max_ns), type_name(e->type), e->id,
		     row->instances);
	} else if (e->parent[0]) {
		blog(LOG_INFO,
		     LOG_TAG "%7.3f  (tick %6.3f + render %6.3f)  "
			     "max %6.2f/%6.2f  %-10s %-26s \"%s\" > \"%s\"",
		     per_frame_ms(a->tick_ns + a->render_ns, frames),
		     per_frame_ms(a->tick_ns, frames),
		     per_frame_ms(a->render_ns, frames), to_ms(a->tick_max_ns),
		     to_ms(a->render_max_ns), type_name(e->type), e->id,
		     e->parent, e->name);
	} else {
		blog(LOG_INFO,
		     LOG_TAG "%7.3f  (tick %6.3f + render %6.3f)  "
			     "max %6.2f/%6.2f  %-10s %-26s \"%s\"",
		     per_frame_ms(a->tick_ns + a->render_ns, frames),
		     per_frame_ms(a->tick_ns, frames),
		     per_frame_ms(a->render_ns, frames), to_ms(a->tick_max_ns),
		     to_ms(a->render_max_ns), type_name(e->type), e->id,
		     e->name);
	}
}

static void dump(bool total, uint64_t frames, uint64_t elapsed_ns)
{
	DARRAY(struct prof_row) rows;
	DARRAY(struct prof_row) types;
	uint64_t sum_tick = 0;
	uint64_t sum_render = 0;

	if (!frames)
		return;

	da_init(rows);
	da_init(types);

	pthread_mutex_lock(&prof.mutex);

	for (size_t i = 0; i < prof.entries.num; i++) {
		struct source_prof_entry *e = prof.entries.array[i];
		struct prof_acc *acc = total ? &e->total : &e->win;
		struct prof_row row = {.entry = e, .acc = *acc, .instances = 1};

		if (!acc->tick_ns && !acc->render_ns)
			continue;

		sum_tick += acc->tick_ns;
		sum_render += acc->render_ns;
		da_push_back(rows, &row);

		/* aggregate by plugin source id */
		size_t t;
		for (t = 0; t < types.num; t++) {
			struct prof_row *tr = &types.array[t];
			if (tr->entry->type == e->type &&
			    strcmp(tr->entry->id, e->id) == 0)
				break;
		}
		if (t == types.num) {
			da_push_back(types, &row);
		} else {
			struct prof_row *tr = &types.array[t];
			tr->acc.tick_ns += acc->tick_ns;
			tr->acc.render_ns += acc->render_ns;
			if (acc->tick_max_ns > tr->acc.tick_max_ns)
				tr->acc.tick_max_ns = acc->tick_max_ns;
			if (acc->render_max_ns > tr->acc.render_max_ns)
				tr->acc.render_max_ns = acc->render_max_ns;
			tr->instances++;
		}

		if (!total) {
			acc->tick_ns = acc->render_ns = 0;
			acc->tick_max_ns = acc->render_max_ns = 0;
		}
	}

	pthread_mutex_unlock(&prof.mutex);

	qsort(rows.array, rows.num, sizeof(*rows.array), cmp_rows);
	qsort(types.array, types.num, sizeof(*types.array), cmp_rows);

	blog(LOG_INFO,
	     LOG_TAG "==== %s: %.1f s, %llu frames. ms per frame, max in ms "
		     "(tick/render). Sum of listed: %.3f ms (tick %.3f + "
		     "render %.3f) ====",
	     total ? "TOTAL since start" : "last window",
	     to_ms(elapsed_ns) / 1000.0, (unsigned long long)frames,
	     per_frame_ms(sum_tick + sum_render, frames),
	     per_frame_ms(sum_tick, frames), per_frame_ms(sum_render, frames));

	blog(LOG_INFO, LOG_TAG "-- by source (top %d) --", TOP_SOURCES);
	for (size_t i = 0; i < rows.num && i < TOP_SOURCES; i++)
		log_row(&rows.array[i], frames, false);

	blog(LOG_INFO, LOG_TAG "-- by plugin/source type (top %d) --",
	     TOP_TYPES);
	for (size_t i = 0; i < types.num && i < TOP_TYPES; i++)
		log_row(&types.array[i], frames, true);

	da_free(rows);
	da_free(types);
}

void source_profiler_frame(uint64_t now_ns)
{
	prof.win_frames++;
	prof.total_frames++;

	if (!prof.interval_ns || now_ns - prof.win_start_ns < prof.interval_ns)
		return;

	dump(false, prof.win_frames, now_ns - prof.win_start_ns);
	prof.win_frames = 0;
	prof.win_start_ns = now_ns;
}

void source_profiler_dump_total(void)
{
	if (!obs_source_profiler_enabled)
		return;

	dump(true, prof.total_frames, os_gettime_ns() - prof.start_ns);
}

void source_profiler_free(void)
{
	if (!obs_source_profiler_enabled)
		return;

	obs_source_profiler_enabled = false;

	for (size_t i = 0; i < prof.entries.num; i++) {
		struct source_prof_entry *e = prof.entries.array[i];
		bfree(e->id);
		bfree(e->name);
		bfree(e->parent);
		bfree(e);
	}
	da_free(prof.entries);
	pthread_mutex_destroy(&prof.mutex);
}
