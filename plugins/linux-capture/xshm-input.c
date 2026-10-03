/*
Copyright (C) 2014 by Leonhard Oelke <leonhard@in-verted.de>

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
*/

#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include <sys/uio.h>
#include <xcb/randr.h>
#include <xcb/xcbext.h>
#include <xcb/shm.h>
#include <xcb/xfixes.h>
#include <xcb/xinerama.h>

#include <obs-module.h>
#include <glad/glad.h>
#include <util/dstr.h>
#include <util/platform.h>
#include "xcursor-xcb.h"
#include "xhelpers.h"

#define XSHM_DATA(voidptr) struct xshm_data *data = voidptr;

#define blog(level, msg, ...) blog(level, "xshm-input: " msg, ##__VA_ARGS__)

struct xshm_data {
	obs_source_t *source;

	xcb_connection_t *xcb;
	xcb_screen_t *xcb_screen;
	xcb_shm_t *xshm;
	xcb_xcursor_t *cursor;

	char *server;
	uint_fast32_t screen_id;
	int_fast32_t x_org;
	int_fast32_t y_org;
	int_fast32_t width;
	int_fast32_t height;

	gs_texture_t *texture;

	int_fast32_t cut_top;
	int_fast32_t cut_left;
	int_fast32_t cut_right;
	int_fast32_t cut_bot;

	int_fast32_t adj_x_org;
	int_fast32_t adj_y_org;
	int_fast32_t adj_width;
	int_fast32_t adj_height;

	/* GPU copy mode: the server copies the screen into a pixmap that is
	 * imported once as a texture, so no readback to system memory and no
	 * upload are needed every frame */
	int copy_mode;
	bool gpu_copy;
	xcb_pixmap_t pixmap;
	xcb_gcontext_t gc;

	bool show_cursor;
	bool use_xinerama;
	bool use_randr;
	bool advanced;
};

/**
 * Resize the texture
 *
 * This will automatically create the texture if it does not exist
 *
 * @note requires to be called within the obs graphics context
 */
static inline void xshm_resize_texture(struct xshm_data *data)
{
	if (data->texture)
		gs_texture_destroy(data->texture);
	data->texture = gs_texture_create(data->adj_width, data->adj_height,
					  GS_BGRA, 1, NULL, GS_DYNAMIC);
}

/**
 * Check if the xserver supports all the extensions we need
 */
static bool xshm_check_extensions(xcb_connection_t *xcb)
{
	bool ok = true;

	if (!xcb_get_extension_data(xcb, &xcb_shm_id)->present) {
		blog(LOG_ERROR, "Missing SHM extension !");
		ok = false;
	}

	if (!xcb_get_extension_data(xcb, &xcb_xinerama_id)->present)
		blog(LOG_INFO, "Missing Xinerama extension !");

	if (!xcb_get_extension_data(xcb, &xcb_randr_id)->present)
		blog(LOG_INFO, "Missing Randr extension !");

	return ok;
}

/**
 * Update the capture
 *
 * @return < 0 on error, 0 when size is unchanged, > 1 on size change
 */
static int_fast32_t xshm_update_geometry(struct xshm_data *data)
{
	int_fast32_t prev_width = data->adj_width;
	int_fast32_t prev_height = data->adj_height;

	if (data->use_randr) {
		if (randr_screen_geo(data->xcb, data->screen_id, &data->x_org,
				     &data->y_org, &data->width, &data->height,
				     &data->xcb_screen, NULL) < 0) {
			return -1;
		}
	} else if (data->use_xinerama) {
		if (xinerama_screen_geo(data->xcb, data->screen_id,
					&data->x_org, &data->y_org,
					&data->width, &data->height) < 0) {
			return -1;
		}
		data->xcb_screen = xcb_get_screen(data->xcb, 0);
	} else {
		data->x_org = 0;
		data->y_org = 0;
		if (x11_screen_geo(data->xcb, data->screen_id, &data->width,
				   &data->height) < 0) {
			return -1;
		}
		data->xcb_screen = xcb_get_screen(data->xcb, data->screen_id);
	}

	if (!data->width || !data->height) {
		blog(LOG_ERROR, "Failed to get geometry");
		return -1;
	}

	data->adj_y_org = data->y_org;
	data->adj_x_org = data->x_org;
	data->adj_height = data->height;
	data->adj_width = data->width;

	if (data->cut_top != 0) {
		if (data->y_org > 0)
			data->adj_y_org = data->y_org + data->cut_top;
		else
			data->adj_y_org = data->cut_top;
		data->adj_height = data->adj_height - data->cut_top;
	}
	if (data->cut_left != 0) {
		if (data->x_org > 0)
			data->adj_x_org = data->x_org + data->cut_left;
		else
			data->adj_x_org = data->cut_left;
		data->adj_width = data->adj_width - data->cut_left;
	}
	if (data->cut_right != 0)
		data->adj_width = data->adj_width - data->cut_right;
	if (data->cut_bot != 0)
		data->adj_height = data->adj_height - data->cut_bot;

	blog(LOG_INFO,
	     "Geometry %" PRIdFAST32 "x%" PRIdFAST32 " @ %" PRIdFAST32
	     ",%" PRIdFAST32,
	     data->width, data->height, data->x_org, data->y_org);

	if (prev_width == data->adj_width && prev_height == data->adj_height)
		return 0;

	return 1;
}

/**
 * Returns the name of the plugin
 */
static const char *xshm_getname(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("X11SharedMemoryScreenInput");
}

/**
 * Stop the capture
 */
static void xshm_capture_stop(struct xshm_data *data)
{
	obs_enter_graphics();

	if (data->texture) {
		gs_texture_destroy(data->texture);
		data->texture = NULL;
	}
	if (data->cursor) {
		xcb_xcursor_destroy(data->cursor);
		data->cursor = NULL;
	}

	obs_leave_graphics();

	if (data->xcb) {
		if (data->gc)
			xcb_free_gc(data->xcb, data->gc);
		if (data->pixmap)
			xcb_free_pixmap(data->xcb, data->pixmap);
	}
	data->gc = 0;
	data->pixmap = 0;
	data->gpu_copy = false;

	if (data->xshm) {
		xshm_xcb_detach(data->xshm);
		data->xshm = NULL;
	}

	if (data->xcb) {
		xcb_disconnect(data->xcb);
		data->xcb = NULL;
	}

	if (data->server) {
		bfree(data->server);
		data->server = NULL;
	}
}

/* How the screen is copied: let the server tell us, or force a method */
enum xshm_copy_mode {
	XSHM_COPY_AUTO = 0,
	XSHM_COPY_GPU = 1,
	XSHM_COPY_SHM = 2,
};

/* Present extension: wire format of QueryCapabilities, sent by hand so the
 * plugin does not need to link against xcb-present */
#define PRESENT_QUERY_CAPABILITIES 4

/* XiS servers set this bit in the QueryCapabilities reply when a CopyArea from
 * the root window (IncludeInferiors) shows what is actually displayed,
 * including content page-flipped per CRTC. Without it the screen pixmap can be
 * stale and a CopyArea based capture freezes. */
#define PRESENT_CAP_XIS_ROOT_COPY 0x80000000u

static xcb_extension_t xshm_present_id = {"Present", 0};

struct present_caps_request {
	uint8_t major_opcode;
	uint8_t minor_opcode;
	uint16_t length;
	uint32_t target;
};

struct present_caps_reply {
	uint8_t response_type;
	uint8_t pad0;
	uint16_t sequence;
	uint32_t length;
	uint32_t capabilities;
	uint8_t pad1[20];
};

static inline int64_t i64_min(int64_t a, int64_t b)
{
	return a < b ? a : b;
}

static inline int64_t i64_max(int64_t a, int64_t b)
{
	return a > b ? a : b;
}

/* Pick the RandR CRTC the capture area overlaps most, or any active one */
static xcb_randr_crtc_t xshm_pick_crtc(struct xshm_data *data)
{
	xcb_randr_crtc_t best = XCB_NONE;
	int64_t best_overlap = -1;

	xcb_randr_get_screen_resources_current_reply_t *res =
		xcb_randr_get_screen_resources_current_reply(
			data->xcb,
			xcb_randr_get_screen_resources_current(
				data->xcb, data->xcb_screen->root),
			NULL);
	if (!res)
		return XCB_NONE;

	xcb_randr_crtc_t *crtcs = xcb_randr_get_screen_resources_current_crtcs(res);
	int n = xcb_randr_get_screen_resources_current_crtcs_length(res);

	for (int i = 0; i < n; i++) {
		xcb_randr_get_crtc_info_reply_t *info =
			xcb_randr_get_crtc_info_reply(
				data->xcb,
				xcb_randr_get_crtc_info(data->xcb, crtcs[i],
							res->config_timestamp),
				NULL);
		if (!info)
			continue;

		if (info->width && info->height) {
			int64_t ox = i64_min((int64_t)info->x + info->width,
					 (int64_t)data->adj_x_org + data->adj_width) -
				     i64_max((int64_t)info->x, (int64_t)data->adj_x_org);
			int64_t oy = i64_min((int64_t)info->y + info->height,
					 (int64_t)data->adj_y_org + data->adj_height) -
				     i64_max((int64_t)info->y, (int64_t)data->adj_y_org);
			int64_t overlap = (ox > 0 && oy > 0) ? ox * oy : 0;

			if (overlap > best_overlap) {
				best_overlap = overlap;
				best = crtcs[i];
			}
		}
		free(info);
	}

	free(res);
	return best;
}

/**
 * Does the server guarantee that a root CopyArea shows the displayed content?
 */
static bool xshm_server_has_root_copy(struct xshm_data *data)
{
	if (!data->xcb_screen ||
	    !xcb_get_extension_data(data->xcb, &xshm_present_id)->present)
		return false;

	xcb_randr_crtc_t crtc = xshm_pick_crtc(data);
	if (crtc == XCB_NONE)
		return false;

	struct present_caps_request req = {0};
	req.target = crtc;

	struct iovec parts[4];
	parts[2].iov_base = (char *)&req;
	parts[2].iov_len = sizeof(req);
	parts[3].iov_base = NULL;
	parts[3].iov_len = -parts[2].iov_len & 3;

	xcb_protocol_request_t proto = {2, &xshm_present_id,
					PRESENT_QUERY_CAPABILITIES, 1};
	xcb_generic_error_t *err = NULL;
	unsigned int seq = xcb_send_request(data->xcb, 0, parts + 2, &proto);
	struct present_caps_reply *reply =
		xcb_wait_for_reply(data->xcb, seq, &err);

	free(err);
	if (!reply)
		return false;

	bool supported = (reply->capabilities & PRESENT_CAP_XIS_ROOT_COPY) != 0;
	free(reply);
	return supported;
}

/**
 * Set up GPU copy mode
 *
 * @return true on success, false to fall back to SHM readback
 */
static bool xshm_gpu_copy_init(struct xshm_data *data)
{
	xcb_screen_t *screen = data->xcb_screen;
	xcb_void_cookie_t cookie;
	xcb_generic_error_t *err;

	if (!screen)
		return false;

	data->pixmap = xcb_generate_id(data->xcb);
	cookie = xcb_create_pixmap_checked(data->xcb, screen->root_depth,
					   data->pixmap, screen->root,
					   data->adj_width, data->adj_height);
	if ((err = xcb_request_check(data->xcb, cookie))) {
		free(err);
		data->pixmap = 0;
		return false;
	}

	/* without IncludeInferiors the copy would skip every top level window;
	 * without disabling graphics exposures every copy would queue a
	 * NoExpose event that nobody reads */
	uint32_t gc_values[] = {XCB_SUBWINDOW_MODE_INCLUDE_INFERIORS, 0};
	data->gc = xcb_generate_id(data->xcb);
	cookie = xcb_create_gc_checked(data->xcb, data->gc, data->pixmap,
				       XCB_GC_SUBWINDOW_MODE |
					       XCB_GC_GRAPHICS_EXPOSURES,
				       gc_values);
	if ((err = xcb_request_check(data->xcb, cookie))) {
		free(err);
		data->gc = 0;
		return false;
	}

	obs_enter_graphics();
	data->texture = gs_texture_create_from_pixmap(
		data->adj_width, data->adj_height, GS_BGRA_UNORM,
		GL_TEXTURE_2D, (void *)(uintptr_t)data->pixmap);
	obs_leave_graphics();

	if (!data->texture)
		return false;

	data->gpu_copy = true;
	return true;
}

/**
 * Start the capture
 */
static void xshm_capture_start(struct xshm_data *data)
{
	const char *server = (data->advanced && *data->server) ? data->server
							       : NULL;

	data->xcb = xcb_connect(server, NULL);
	if (!data->xcb || xcb_connection_has_error(data->xcb)) {
		blog(LOG_ERROR, "Unable to open X display !");
		goto fail;
	}

	if (!xshm_check_extensions(data->xcb))
		goto fail;

	data->use_randr = randr_is_active(data->xcb) ? true : false;
	data->use_xinerama = xinerama_is_active(data->xcb) ? true : false;

	if (xshm_update_geometry(data) < 0) {
		blog(LOG_ERROR, "failed to update geometry !");
		goto fail;
	}

	data->cursor = xcb_xcursor_init(data->xcb);
	xcb_xcursor_offset(data->cursor, data->adj_x_org, data->adj_y_org);

	bool want_gpu_copy = data->copy_mode == XSHM_COPY_GPU;
	if (data->copy_mode == XSHM_COPY_AUTO) {
		want_gpu_copy = xshm_server_has_root_copy(data);
		blog(LOG_INFO, "Server %s root CopyArea captures: %s",
		     want_gpu_copy ? "guarantees" : "does not guarantee",
		     want_gpu_copy ? "using GPU copy" : "using SHM readback");
	}

	if (want_gpu_copy) {
		if (xshm_gpu_copy_init(data)) {
			blog(LOG_INFO, "Using GPU copy (no readback)");
			return;
		}

		blog(LOG_WARNING,
		     "GPU copy unavailable, falling back to SHM readback");
		if (data->gc)
			xcb_free_gc(data->xcb, data->gc);
		if (data->pixmap)
			xcb_free_pixmap(data->xcb, data->pixmap);
		data->gc = 0;
		data->pixmap = 0;
	}

	data->xshm =
		xshm_xcb_attach(data->xcb, data->adj_width, data->adj_height);
	if (!data->xshm) {
		blog(LOG_ERROR, "failed to attach shm !");
		goto fail;
	}

	obs_enter_graphics();

	xshm_resize_texture(data);

	obs_leave_graphics();

	return;
fail:
	xshm_capture_stop(data);
}

/**
 * Update the capture with changed settings
 */
static void xshm_update(void *vptr, obs_data_t *settings)
{
	XSHM_DATA(vptr);

	xshm_capture_stop(data);

	data->screen_id = obs_data_get_int(settings, "screen");
	data->show_cursor = obs_data_get_bool(settings, "show_cursor");
	data->copy_mode = (int)obs_data_get_int(settings, "copy_mode");

	/* testing override: OBS_XSHM_GPU_COPY=1 forces GPU copy, =0 forces SHM */
	const char *force_gpu_copy = getenv("OBS_XSHM_GPU_COPY");
	if (force_gpu_copy && *force_gpu_copy)
		data->copy_mode = strcmp(force_gpu_copy, "0") != 0
					  ? XSHM_COPY_GPU
					  : XSHM_COPY_SHM;
	data->advanced = obs_data_get_bool(settings, "advanced");
	data->server = bstrdup(obs_data_get_string(settings, "server"));

	data->cut_top = obs_data_get_int(settings, "cut_top");
	data->cut_left = obs_data_get_int(settings, "cut_left");
	data->cut_right = obs_data_get_int(settings, "cut_right");
	data->cut_bot = obs_data_get_int(settings, "cut_bot");

	xshm_capture_start(data);
}

/**
 * Get the default settings for the capture
 */
static void xshm_defaults(obs_data_t *defaults)
{
	obs_data_set_default_int(defaults, "screen", 0);
	obs_data_set_default_bool(defaults, "show_cursor", true);
	obs_data_set_default_int(defaults, "copy_mode", XSHM_COPY_AUTO);
	obs_data_set_default_bool(defaults, "advanced", false);
	obs_data_set_default_int(defaults, "cut_top", 0);
	obs_data_set_default_int(defaults, "cut_left", 0);
	obs_data_set_default_int(defaults, "cut_right", 0);
	obs_data_set_default_int(defaults, "cut_bot", 0);
}

/**
 * Toggle visibility of advanced settings
 */
static bool xshm_toggle_advanced(obs_properties_t *props, obs_property_t *p,
				 obs_data_t *settings)
{
	UNUSED_PARAMETER(p);
	const bool visible = obs_data_get_bool(settings, "advanced");
	obs_property_t *server = obs_properties_get(props, "server");

	obs_property_set_visible(server, visible);

	/* trigger server changed callback so the screen list is refreshed */
	obs_property_modified(server, settings);

	return true;
}

/**
 * The x server was changed
 */
static bool xshm_server_changed(obs_properties_t *props, obs_property_t *p,
				obs_data_t *settings)
{
	UNUSED_PARAMETER(p);

	bool advanced = obs_data_get_bool(settings, "advanced");
	int_fast32_t old_screen = obs_data_get_int(settings, "screen");
	const char *server = obs_data_get_string(settings, "server");
	obs_property_t *screens = obs_properties_get(props, "screen");

	/* we want a real NULL here in case there is no string here */
	server = (advanced && *server) ? server : NULL;

	obs_property_list_clear(screens);

	xcb_connection_t *xcb = xcb_connect(server, NULL);
	if (!xcb || xcb_connection_has_error(xcb)) {
		obs_property_set_enabled(screens, false);
		return true;
	}

	struct dstr screen_info;
	dstr_init(&screen_info);
	bool randr = randr_is_active(xcb);
	bool xinerama = xinerama_is_active(xcb);
	int_fast32_t count =
		randr ? randr_screen_count(xcb)
		      : (xinerama ? xinerama_screen_count(xcb)
				  : xcb_setup_roots_length(xcb_get_setup(xcb)));

	for (int_fast32_t i = 0; i < count; ++i) {
		char *name;
		char name_tmp[20];
		int_fast32_t x, y, w, h;
		x = y = w = h = 0;

		name = NULL;
		if (randr)
			randr_screen_geo(xcb, i, &x, &y, &w, &h, NULL, &name);
		else if (xinerama)
			xinerama_screen_geo(xcb, i, &x, &y, &w, &h);
		else
			x11_screen_geo(xcb, i, &w, &h);

		if (name == NULL) {
			int ret = snprintf(name_tmp, sizeof(name_tmp),
					   "%" PRIuFAST32, i);
			if (ret >= (int)sizeof(name_tmp))
				blog(LOG_DEBUG,
				     "linux-capture: A format truncation may have occurred."
				     " This can be ignored since it is quite improbable.");

			name = name_tmp;
		}

		dstr_printf(&screen_info,
			    "Screen %s (%" PRIuFAST32 "x%" PRIuFAST32
			    " @ %" PRIuFAST32 ",%" PRIuFAST32 ")",
			    name, w, h, x, y);

		if (name != name_tmp)
			free(name);

		if (h > 0 && w > 0)
			obs_property_list_add_int(screens, screen_info.array,
						  i);
	}

	/* handle missing screen */
	if (old_screen + 1 > count) {
		dstr_printf(&screen_info, "Screen %" PRIuFAST32 " (not found)",
			    old_screen);
		size_t index = obs_property_list_add_int(
			screens, screen_info.array, old_screen);
		obs_property_list_item_disable(screens, index, true);
	}

	dstr_free(&screen_info);

	xcb_disconnect(xcb);
	obs_property_set_enabled(screens, true);

	return true;
}

/**
 * Get the properties for the capture
 */
static obs_properties_t *xshm_properties(void *vptr)
{
	XSHM_DATA(vptr);

	obs_properties_t *props = obs_properties_create();
	obs_property_t *prop;

	obs_properties_add_list(props, "screen", obs_module_text("Screen"),
				OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_properties_add_bool(props, "show_cursor",
				obs_module_text("CaptureCursor"));
	obs_property_t *copy_mode = obs_properties_add_list(
		props, "copy_mode", obs_module_text("XSHM.CopyMode"),
		OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(copy_mode, obs_module_text("XSHM.CopyMode.Auto"),
				  XSHM_COPY_AUTO);
	obs_property_list_add_int(copy_mode, obs_module_text("XSHM.CopyMode.Gpu"),
				  XSHM_COPY_GPU);
	obs_property_list_add_int(copy_mode, obs_module_text("XSHM.CopyMode.Shm"),
				  XSHM_COPY_SHM);
	obs_property_set_long_description(
		copy_mode, obs_module_text("XSHM.CopyMode.Description"));
	obs_property_t *advanced = obs_properties_add_bool(
		props, "advanced", obs_module_text("AdvancedSettings"));

	prop = obs_properties_add_int(
		props, "cut_top", obs_module_text("CropTop"), -4096, 4096, 1);
	obs_property_int_set_suffix(prop, " px");

	prop = obs_properties_add_int(
		props, "cut_left", obs_module_text("CropLeft"), -4096, 4096, 1);
	obs_property_int_set_suffix(prop, " px");

	prop = obs_properties_add_int(props, "cut_right",
				      obs_module_text("CropRight"), 0, 4096, 1);
	obs_property_int_set_suffix(prop, " px");

	prop = obs_properties_add_int(
		props, "cut_bot", obs_module_text("CropBottom"), 0, 4096, 1);
	obs_property_int_set_suffix(prop, " px");

	obs_property_t *server = obs_properties_add_text(
		props, "server", obs_module_text("XServer"), OBS_TEXT_DEFAULT);

	obs_property_set_modified_callback(advanced, xshm_toggle_advanced);
	obs_property_set_modified_callback(server, xshm_server_changed);

	/* trigger server callback to get screen count ... */
	obs_data_t *settings = obs_source_get_settings(data->source);
	obs_property_modified(server, settings);
	obs_data_release(settings);

	return props;
}

/**
 * Destroy the capture
 */
static void xshm_destroy(void *vptr)
{
	XSHM_DATA(vptr);

	if (!data)
		return;

	xshm_capture_stop(data);

	bfree(data);
}

/**
 * Create the capture
 */
static void *xshm_create(obs_data_t *settings, obs_source_t *source)
{
	struct xshm_data *data = bzalloc(sizeof(struct xshm_data));
	data->source = source;

	xshm_update(data, settings);

	return data;
}

/**
 * Prepare the capture data
 */
/* Optional timing breakdown of the tick (OBS_XSHM_PROFILE=1), logged every
 * 5 seconds. Only touched from the graphics thread. */
static struct {
	uint64_t send_ns, wait_ns, upload_ns, cursor_ns, max_wait_ns;
	uint64_t frames, last_log_ns;
	int enabled;
} xshm_prof = {.enabled = -1};

static void xshm_prof_log(uint64_t now)
{
	double n = (double)xshm_prof.frames;

	blog(LOG_INFO,
	     "[xshm-profile] %.0f frames: request %.3f ms, wait for X %.3f ms "
	     "(max %.2f), upload to GPU %.3f ms, cursor %.3f ms (per frame)",
	     n, xshm_prof.send_ns / 1e6 / n, xshm_prof.wait_ns / 1e6 / n,
	     xshm_prof.max_wait_ns / 1e6, xshm_prof.upload_ns / 1e6 / n,
	     xshm_prof.cursor_ns / 1e6 / n);

	xshm_prof.send_ns = xshm_prof.wait_ns = xshm_prof.upload_ns = 0;
	xshm_prof.cursor_ns = xshm_prof.max_wait_ns = 0;
	xshm_prof.frames = 0;
	xshm_prof.last_log_ns = now;
}

/* t0..t4: before request, after request, after reply, after upload, end */
static void xshm_prof_record(uint64_t t0, uint64_t t1, uint64_t t2,
			     uint64_t t3, uint64_t t4)
{
	xshm_prof.send_ns += t1 - t0;
	xshm_prof.wait_ns += t2 - t1;
	if (t2 - t1 > xshm_prof.max_wait_ns)
		xshm_prof.max_wait_ns = t2 - t1;
	xshm_prof.upload_ns += t3 - t2;
	xshm_prof.cursor_ns += t4 - t3;
	xshm_prof.frames++;

	if (!xshm_prof.last_log_ns)
		xshm_prof.last_log_ns = t4;
	else if (t4 - xshm_prof.last_log_ns > 5000000000ULL)
		xshm_prof_log(t4);
}

/* GPU copy mode: ask the server to blit the screen into our pixmap.
 * Asynchronous, there is no reply to wait for. The texture shares the
 * pixmap's buffer and implicit sync orders the blit against our reads. */
static void xshm_tick_gpu_copy(struct xshm_data *data, bool prof)
{
	uint64_t t0 = prof ? os_gettime_ns() : 0;

	xcb_copy_area(data->xcb, data->xcb_screen->root, data->pixmap,
		      data->gc, data->adj_x_org, data->adj_y_org, 0, 0,
		      data->adj_width, data->adj_height);
	xcb_flush(data->xcb);

	uint64_t t1 = prof ? os_gettime_ns() : 0;

	obs_enter_graphics();
	xcb_xcursor_update(data->xcb, data->cursor);
	obs_leave_graphics();

	if (prof)
		xshm_prof_record(t0, t1, t1, t1, os_gettime_ns());
}

static void xshm_video_tick(void *vptr, float seconds)
{
	UNUSED_PARAMETER(seconds);
	XSHM_DATA(vptr);

	if (!data->texture)
		return;
	if (!obs_source_showing(data->source))
		return;

	if (xshm_prof.enabled < 0) {
		const char *env = getenv("OBS_XSHM_PROFILE");
		xshm_prof.enabled = env && *env && strcmp(env, "0") != 0;
	}
	const bool prof = xshm_prof.enabled > 0;

	if (data->gpu_copy) {
		xshm_tick_gpu_copy(data, prof);
		return;
	}

	uint64_t t0 = 0, t1 = 0, t2 = 0, t3 = 0;
	xcb_shm_get_image_cookie_t img_c;
	xcb_shm_get_image_reply_t *img_r;

	if (prof)
		t0 = os_gettime_ns();

	img_c = xcb_shm_get_image_unchecked(data->xcb, data->xcb_screen->root,
					    data->adj_x_org, data->adj_y_org,
					    data->adj_width, data->adj_height,
					    ~0, XCB_IMAGE_FORMAT_Z_PIXMAP,
					    data->xshm->seg, 0);

	if (prof)
		t1 = os_gettime_ns();

	img_r = xcb_shm_get_image_reply(data->xcb, img_c, NULL);

	if (prof)
		t2 = os_gettime_ns();

	if (!img_r)
		goto exit;

	obs_enter_graphics();

	gs_texture_set_image(data->texture, (void *)data->xshm->data,
			     data->adj_width * 4, false);

	if (prof)
		t3 = os_gettime_ns();

	xcb_xcursor_update(data->xcb, data->cursor);

	obs_leave_graphics();

	if (prof)
		xshm_prof_record(t0, t1, t2, t3, os_gettime_ns());

exit:
	free(img_r);
}

/**
 * Render the capture data
 */
static void xshm_video_render(void *vptr, gs_effect_t *effect)
{
	XSHM_DATA(vptr);

	effect = obs_get_base_effect(OBS_EFFECT_OPAQUE);

	if (!data->texture)
		return;

	const bool linear_srgb = gs_get_linear_srgb();

	const bool previous = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(linear_srgb);

	gs_eparam_t *image = gs_effect_get_param_by_name(effect, "image");
	if (linear_srgb)
		gs_effect_set_texture_srgb(image, data->texture);
	else
		gs_effect_set_texture(image, data->texture);

	while (gs_effect_loop(effect, "Draw")) {
		gs_draw_sprite(data->texture, 0, 0, 0);
	}

	gs_enable_framebuffer_srgb(previous);

	if (data->show_cursor) {
		effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);

		while (gs_effect_loop(effect, "Draw")) {
			xcb_xcursor_render(data->cursor);
		}
	}
}

/**
 * Width of the captured data
 */
static uint32_t xshm_getwidth(void *vptr)
{
	XSHM_DATA(vptr);
	return data->adj_width;
}

/**
 * Height of the captured data
 */
static uint32_t xshm_getheight(void *vptr)
{
	XSHM_DATA(vptr);
	return data->adj_height;
}

struct obs_source_info xshm_input = {
	.id = "xshm_input",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW |
			OBS_SOURCE_DO_NOT_DUPLICATE | OBS_SOURCE_SRGB,
	.get_name = xshm_getname,
	.create = xshm_create,
	.destroy = xshm_destroy,
	.update = xshm_update,
	.get_defaults = xshm_defaults,
	.get_properties = xshm_properties,
	.video_tick = xshm_video_tick,
	.video_render = xshm_video_render,
	.get_width = xshm_getwidth,
	.get_height = xshm_getheight,
	.icon_type = OBS_ICON_TYPE_DESKTOP_CAPTURE,
};
