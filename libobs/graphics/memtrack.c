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

#if defined(__linux__)
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#define MEMTRACK_BACKTRACE
#include <dlfcn.h>
#include <execinfo.h>
#include <signal.h>
#include <dirent.h>
#include <stdio.h>
#endif

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../util/base.h"
#include "../util/bmem.h"
#include "../util/darray.h"
#include "../util/dstr.h"
#include "../util/threading.h"
#include "../util/uthash.h"
#include "graphics.h"
#include "memtrack.h"

#define MEMTRACK_FRAMES 6

struct memtrack_entry {
	const void *obj;
	enum gs_memtrack_kind kind;
	uint32_t width, height, depth;
	int format;
	uint64_t bytes;
	char *owner;
	void *frames[MEMTRACK_FRAMES];
	int num_frames;
	UT_hash_handle hh;
};

bool gs_memtrack_enabled = false;

static pthread_mutex_t memtrack_mutex;
static struct memtrack_entry *entries = NULL;
static uint64_t total_bytes = 0;
static uint64_t last_dump_bytes = 0;
static uint64_t last_dump_ns = 0;
static uint64_t interval_ns = 10000000000ULL;

static THREAD_LOCAL struct gs_memtrack_owner cur_owner = {0};

#ifdef MEMTRACK_BACKTRACE
static volatile sig_atomic_t dump_requested = 0;

static void sigusr1_handler(int sig)
{
	UNUSED_PARAMETER(sig);
	dump_requested = 1;
}
#endif

void gs_memtrack_init(void)
{
	const char *env = getenv("OBS_VRAM_PROFILER");
	if (!env || !*env || strcmp(env, "0") == 0)
		return;

	const char *interval_env = getenv("OBS_VRAM_PROFILER_INTERVAL");
	if (interval_env && *interval_env)
		interval_ns = (uint64_t)(atof(interval_env) * 1000000000.0);

	pthread_mutex_init(&memtrack_mutex, NULL);
	gs_memtrack_enabled = true;

#ifdef MEMTRACK_BACKTRACE
	struct sigaction sa = {0};
	sa.sa_handler = sigusr1_handler;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = SA_RESTART;
	sigaction(SIGUSR1, &sa, NULL);
#endif

	blog(LOG_INFO,
	     "VRAM profiler enabled (report every %.1f s when changed, "
	     "or on SIGUSR1)",
	     (double)interval_ns / 1000000000.0);
}

void gs_memtrack_free(void)
{
	if (!gs_memtrack_enabled)
		return;

	gs_memtrack_dump("shutdown (leftovers)");

	pthread_mutex_lock(&memtrack_mutex);
	struct memtrack_entry *e, *tmp;
	HASH_ITER (hh, entries, e, tmp) {
		HASH_DEL(entries, e);
		bfree(e->owner);
		bfree(e);
	}
	total_bytes = 0;
	pthread_mutex_unlock(&memtrack_mutex);
}

struct gs_memtrack_owner gs_memtrack_push_owner(const char *name,
						const char *id,
						const char *parent)
{
	struct gs_memtrack_owner prev = cur_owner;
	/* unnamed private sources (slideshow images...) keep the outer owner */
	if (!name)
		return prev;
	cur_owner.name = name;
	cur_owner.id = id;
	cur_owner.parent = parent;
	return prev;
}

void gs_memtrack_pop_owner(struct gs_memtrack_owner prev)
{
	cur_owner = prev;
}

static char *owner_string(void)
{
	struct dstr str = {0};

	if (!cur_owner.name) {
		dstr_copy(&str, "(libobs core, no source)");
	} else if (cur_owner.parent) {
		dstr_printf(&str, "'%s' [%s] on '%s'", cur_owner.name,
			    cur_owner.id ? cur_owner.id : "?",
			    cur_owner.parent);
	} else {
		dstr_printf(&str, "'%s' [%s]", cur_owner.name,
			    cur_owner.id ? cur_owner.id : "?");
	}
	return str.array;
}

void gs_memtrack_add(const void *obj, enum gs_memtrack_kind kind,
		     uint32_t width, uint32_t height, uint32_t depth,
		     int format, uint64_t bytes)
{
	if (!obj)
		return;

	struct memtrack_entry *e = bzalloc(sizeof(*e));
	e->obj = obj;
	e->kind = kind;
	e->width = width;
	e->height = height;
	e->depth = depth;
	e->format = format;
	e->bytes = bytes;
	e->owner = owner_string();

#ifdef MEMTRACK_BACKTRACE
	/* skip this function and the gs_*_create wrapper */
	void *frames[MEMTRACK_FRAMES + 2];
	int n = backtrace(frames, MEMTRACK_FRAMES + 2);
	for (int i = 2; i < n; i++)
		e->frames[e->num_frames++] = frames[i];
#endif

	pthread_mutex_lock(&memtrack_mutex);
	struct memtrack_entry *old;
	HASH_FIND_PTR(entries, &obj, old);
	if (old) {
		/* address reused without a destroy we saw: replace it */
		HASH_DEL(entries, old);
		if (old->kind != GS_MEMTRACK_IMPORT)
			total_bytes -= old->bytes;
		bfree(old->owner);
		bfree(old);
	}
	HASH_ADD_PTR(entries, obj, e);
	if (kind != GS_MEMTRACK_IMPORT)
		total_bytes += bytes;
	pthread_mutex_unlock(&memtrack_mutex);
}

void gs_memtrack_remove(const void *obj)
{
	if (!obj)
		return;

	pthread_mutex_lock(&memtrack_mutex);
	struct memtrack_entry *e;
	HASH_FIND_PTR(entries, &obj, e);
	if (e) {
		HASH_DEL(entries, e);
		if (e->kind != GS_MEMTRACK_IMPORT)
			total_bytes -= e->bytes;
		bfree(e->owner);
		bfree(e);
	}
	pthread_mutex_unlock(&memtrack_mutex);
}

void gs_memtrack_frame(uint64_t now_ns)
{
#ifdef MEMTRACK_BACKTRACE
	if (dump_requested) {
		dump_requested = 0;
		last_dump_ns = now_ns;
		gs_memtrack_dump("SIGUSR1");
		return;
	}
#endif
	if (!interval_ns || now_ns - last_dump_ns < interval_ns)
		return;

	last_dump_ns = now_ns;
	if (total_bytes != last_dump_bytes)
		gs_memtrack_dump("changed");
}

/* ------------------------------------------------------------------------- */

static const char *kind_name(enum gs_memtrack_kind kind)
{
	switch (kind) {
	case GS_MEMTRACK_TEXTURE:
		return "texture";
	case GS_MEMTRACK_RENDER_TARGET:
		return "render target";
	case GS_MEMTRACK_STAGESURF:
		return "stage surface";
	case GS_MEMTRACK_ZSTENCIL:
		return "zstencil";
	case GS_MEMTRACK_IMPORT:
		return "import";
	}
	return "?";
}

static const char *format_name(int format)
{
	switch ((enum gs_color_format)format) {
	case GS_A8:
		return "A8";
	case GS_R8:
		return "R8";
	case GS_RGBA:
		return "RGBA";
	case GS_BGRX:
		return "BGRX";
	case GS_BGRA:
		return "BGRA";
	case GS_R10G10B10A2:
		return "R10G10B10A2";
	case GS_RGBA16:
		return "RGBA16";
	case GS_R16:
		return "R16";
	case GS_RGBA16F:
		return "RGBA16F";
	case GS_RGBA32F:
		return "RGBA32F";
	case GS_RG16F:
		return "RG16F";
	case GS_RG32F:
		return "RG32F";
	case GS_R16F:
		return "R16F";
	case GS_R32F:
		return "R32F";
	case GS_DXT1:
		return "DXT1";
	case GS_DXT3:
		return "DXT3";
	case GS_DXT5:
		return "DXT5";
	case GS_R8G8:
		return "R8G8";
	case GS_RGBA_UNORM:
		return "RGBA_UNORM";
	case GS_BGRX_UNORM:
		return "BGRX_UNORM";
	case GS_BGRA_UNORM:
		return "BGRA_UNORM";
	case GS_RG16:
		return "RG16";
	case GS_UNKNOWN:
		break;
	}
	return "?";
}

static void frames_string(struct dstr *str, const struct memtrack_entry *e)
{
	dstr_free(str);
#ifdef MEMTRACK_BACKTRACE
	for (int i = 0; i < e->num_frames; i++) {
		Dl_info info;
		if (i)
			dstr_cat(str, " ");
		if (dladdr(e->frames[i], &info) && info.dli_fname) {
			const char *base = strrchr(info.dli_fname, '/');
			base = base ? base + 1 : info.dli_fname;
			/* return address points after the call */
			dstr_catf(str, "%s+0x%" PRIxPTR, base,
				  (uintptr_t)e->frames[i] -
					  (uintptr_t)info.dli_fbase - 1);
		} else {
			dstr_catf(str, "%p", e->frames[i]);
		}
	}
#else
	UNUSED_PARAMETER(e);
#endif
	if (dstr_is_empty(str))
		dstr_copy(str, "-");
}

/* VRAM/GTT of this process from the DRM fdinfo, one count per DRM client */
static void read_fdinfo(uint64_t *vram_kib, uint64_t *gtt_kib)
{
	*vram_kib = *gtt_kib = 0;
#ifdef MEMTRACK_BACKTRACE
	DIR *dir = opendir("/proc/self/fdinfo");
	if (!dir)
		return;

	DARRAY(uint64_t) seen;
	da_init(seen);

	struct dirent *ent;
	while ((ent = readdir(dir)) != NULL) {
		if (ent->d_name[0] == '.')
			continue;

		char path[64];
		snprintf(path, sizeof(path), "/proc/self/fdinfo/%s",
			 ent->d_name);
		FILE *f = fopen(path, "r");
		if (!f)
			continue;

		char line[256];
		uint64_t client = UINT64_MAX, vram = 0, gtt = 0;
		while (fgets(line, sizeof(line), f)) {
			unsigned long long v;
			if (sscanf(line, "drm-client-id: %llu", &v) == 1 ||
			    sscanf(line, "pasid: %llu", &v) == 1)
				client = v;
			else if (sscanf(line, "drm-memory-vram: %llu", &v) ==
					 1 ||
				 sscanf(line, "vram mem: %llu", &v) == 1)
				vram = v;
			else if (sscanf(line, "drm-memory-gtt: %llu", &v) ==
					 1 ||
				 sscanf(line, "gtt mem: %llu", &v) == 1)
				gtt = v;
		}
		fclose(f);

		if (client == UINT64_MAX)
			continue;
		bool dup = false;
		for (size_t i = 0; i < seen.num; i++)
			dup |= seen.array[i] == client;
		if (dup)
			continue;
		da_push_back(seen, &client);
		*vram_kib += vram;
		*gtt_kib += gtt;
	}
	closedir(dir);
	da_free(seen);
#endif
}

struct owner_sum {
	const char *owner;
	uint64_t bytes;
	uint64_t import_bytes;
	size_t count;
};

struct group {
	const struct memtrack_entry *first;
	char *frames;
	uint64_t bytes;
	size_t count;
};

static int cmp_owner(const void *a, const void *b)
{
	const struct owner_sum *x = a, *y = b;
	uint64_t xb = x->bytes ? x->bytes : x->import_bytes / 1024;
	uint64_t yb = y->bytes ? y->bytes : y->import_bytes / 1024;
	return xb < yb ? 1 : (xb > yb ? -1 : 0);
}

static int cmp_group(const void *a, const void *b)
{
	const struct group *x = a, *y = b;
	return x->bytes < y->bytes ? 1 : (x->bytes > y->bytes ? -1 : 0);
}

static inline double mib(uint64_t bytes)
{
	return (double)bytes / (1024.0 * 1024.0);
}

void gs_memtrack_dump(const char *reason)
{
	if (!gs_memtrack_enabled)
		return;

	DARRAY(struct owner_sum) owners;
	DARRAY(struct group) groups;
	struct dstr frames = {0};
	da_init(owners);
	da_init(groups);

	pthread_mutex_lock(&memtrack_mutex);

	uint64_t import_total = 0;
	size_t count = 0;
	struct memtrack_entry *e, *tmp;
	HASH_ITER (hh, entries, e, tmp) {
		count++;
		bool import = e->kind == GS_MEMTRACK_IMPORT;
		if (import)
			import_total += e->bytes;

		struct owner_sum *o = NULL;
		for (size_t i = 0; i < owners.num; i++) {
			if (strcmp(owners.array[i].owner, e->owner) == 0) {
				o = &owners.array[i];
				break;
			}
		}
		if (!o) {
			o = da_push_back_new(owners);
			o->owner = e->owner;
		}
		if (import)
			o->import_bytes += e->bytes;
		else
			o->bytes += e->bytes;
		o->count++;

		frames_string(&frames, e);
		struct group *g = NULL;
		for (size_t i = 0; i < groups.num; i++) {
			const struct memtrack_entry *f = groups.array[i].first;
			if (f->kind == e->kind && f->width == e->width &&
			    f->height == e->height && f->depth == e->depth &&
			    f->format == e->format &&
			    strcmp(f->owner, e->owner) == 0 &&
			    strcmp(groups.array[i].frames, frames.array) ==
				    0) {
				g = &groups.array[i];
				break;
			}
		}
		if (!g) {
			g = da_push_back_new(groups);
			g->first = e;
			g->frames = bstrdup(frames.array);
		}
		g->bytes += e->bytes;
		g->count++;
	}

	qsort(owners.array, owners.num, sizeof(*owners.array), cmp_owner);
	qsort(groups.array, groups.num, sizeof(*groups.array), cmp_group);

	uint64_t vram_kib, gtt_kib;
	read_fdinfo(&vram_kib, &gtt_kib);

	blog(LOG_INFO, "=== VRAM profiler (%s) ===", reason);
	blog(LOG_INFO,
	     "tracked: %.1f MiB in %zu objects (+%.1f MiB imported); "
	     "driver (fdinfo): VRAM %.1f MiB, GTT %.1f MiB",
	     mib(total_bytes), count, mib(import_total),
	     (double)vram_kib / 1024.0, (double)gtt_kib / 1024.0);
	blog(LOG_INFO, "--- by owner ---");
	for (size_t i = 0; i < owners.num; i++) {
		struct owner_sum *o = &owners.array[i];
		if (o->import_bytes)
			blog(LOG_INFO,
			     "%9.2f MiB %4zu obj  %s  (+%.2f MiB imported)",
			     mib(o->bytes), o->count, o->owner,
			     mib(o->import_bytes));
		else
			blog(LOG_INFO, "%9.2f MiB %4zu obj  %s", mib(o->bytes),
			     o->count, o->owner);
	}
	blog(LOG_INFO, "--- by allocation ---");
	for (size_t i = 0; i < groups.num; i++) {
		struct group *g = &groups.array[i];
		const struct memtrack_entry *f = g->first;
		char size[48];
		if (f->depth > 1)
			snprintf(size, sizeof(size), "%ux%ux%u", f->width,
				 f->height, f->depth);
		else
			snprintf(size, sizeof(size), "%ux%u", f->width,
				 f->height);
		blog(LOG_INFO, "%9.2f MiB %3zux %s %s %s | %s | at %s",
		     mib(g->bytes), g->count, kind_name(f->kind), size,
		     format_name(f->format), f->owner, g->frames);
	}
	blog(LOG_INFO, "=== end VRAM profiler ===");

	last_dump_bytes = total_bytes;
	pthread_mutex_unlock(&memtrack_mutex);

	for (size_t i = 0; i < groups.num; i++)
		bfree(groups.array[i].frames);
	da_free(groups);
	da_free(owners);
	dstr_free(&frames);
}
