#include <obs-module.h>
#include <graphics/image-file.h>
#include <util/threading.h>
#include <util/platform.h>
#include <util/dstr.h>
#include <sys/stat.h>

#define blog(log_level, format, ...)                    \
	blog(log_level, "[image_source: '%s'] " format, \
	     obs_source_get_name(context->source), ##__VA_ARGS__)

#define debug(format, ...) blog(LOG_DEBUG, format, ##__VA_ARGS__)
#define info(format, ...) blog(LOG_INFO, format, ##__VA_ARGS__)
#define warn(format, ...) blog(LOG_WARNING, format, ##__VA_ARGS__)

struct image_source {
	obs_source_t *source;

	char *file;
	bool persistent;
	bool is_slide;
	bool linear_alpha;
	time_t file_timestamp;
	float update_time_elapsed;
	uint64_t last_time;
	bool active;
	bool restart_gif;
	volatile bool file_decoded;
	volatile bool texture_loaded;

	/* optional limit for the decoded image (0 = none). the source keeps
	 * reporting the original size and draws the smaller texture stretched */
	uint32_t max_cx;
	uint32_t max_cy;
	uint32_t cx;
	uint32_t cy;

	gs_image_file4_t if4;
};

static time_t get_modified_timestamp(const char *filename)
{
	struct stat stats;
	if (os_stat(filename, &stats) != 0)
		return -1;
	return stats.st_mtime;
}

static const char *image_source_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("ImageInput");
}

static bool is_8bit_rgba(enum gs_color_format format)
{
	switch (format) {
	case GS_RGBA:
	case GS_BGRA:
	case GS_BGRX:
	case GS_RGBA_UNORM:
	case GS_BGRA_UNORM:
	case GS_BGRX_UNORM:
		return true;
	default:
		return false;
	}
}

/* area average (box filter) downscale of 8-bit 4 channel pixels. the data
 * is already premultiplied, so averaging the alpha channel along with the
 * colors is correct */
static uint8_t *downscale_rgba(const uint8_t *src, uint32_t cx, uint32_t cy,
			       uint32_t new_cx, uint32_t new_cy)
{
	uint8_t *dst = bmalloc((size_t)new_cx * new_cy * 4);
	uint32_t *x0 = bmalloc(sizeof(uint32_t) * (new_cx + 1));

	for (uint32_t x = 0; x <= new_cx; x++)
		x0[x] = (uint32_t)((uint64_t)x * cx / new_cx);

	for (uint32_t y = 0; y < new_cy; y++) {
		uint32_t y0 = (uint32_t)((uint64_t)y * cy / new_cy);
		uint32_t y1 = (uint32_t)((uint64_t)(y + 1) * cy / new_cy);
		if (y1 <= y0)
			y1 = y0 + 1;

		uint8_t *out = dst + (size_t)y * new_cx * 4;
		for (uint32_t x = 0; x < new_cx; x++) {
			uint32_t x1 = x0[x + 1] > x0[x] ? x0[x + 1] : x0[x] + 1;
			uint32_t sum[4] = {0};

			for (uint32_t sy = y0; sy < y1; sy++) {
				const uint8_t *in =
					src + ((size_t)sy * cx + x0[x]) * 4;
				for (uint32_t sx = x0[x]; sx < x1; sx++) {
					sum[0] += in[0];
					sum[1] += in[1];
					sum[2] += in[2];
					sum[3] += in[3];
					in += 4;
				}
			}

			uint32_t n = (y1 - y0) * (x1 - x0[x]);
			for (int c = 0; c < 4; c++)
				out[c] = (uint8_t)((sum[c] + n / 2) / n);
			out += 4;
		}
	}

	bfree(x0);
	return dst;
}

static void limit_image_size(struct image_source *context)
{
	gs_image_file_t *image = &context->if4.image3.image2.image;

	context->cx = image->cx;
	context->cy = image->cy;

	if (!image->loaded || image->is_animated_gif || !image->texture_data ||
	    !is_8bit_rgba(image->format))
		return;
	if ((!context->max_cx || image->cx <= context->max_cx) &&
	    (!context->max_cy || image->cy <= context->max_cy))
		return;

	double scale = 1.0;
	if (context->max_cx)
		scale = (double)context->max_cx / (double)image->cx;
	if (context->max_cy) {
		double sy = (double)context->max_cy / (double)image->cy;
		if (sy < scale)
			scale = sy;
	}

	uint32_t new_cx = (uint32_t)((double)image->cx * scale + 0.5);
	uint32_t new_cy = (uint32_t)((double)image->cy * scale + 0.5);
	if (!new_cx)
		new_cx = 1;
	if (!new_cy)
		new_cy = 1;

	uint8_t *data = downscale_rgba(image->texture_data, image->cx,
				       image->cy, new_cx, new_cy);
	bfree(image->texture_data);
	image->texture_data = data;
	image->cx = new_cx;
	image->cy = new_cy;
	context->if4.image3.image2.mem_usage = (uint64_t)new_cx * new_cy * 4;

	debug("decoded at %ux%u instead of %ux%u", new_cx, new_cy, context->cx,
	      context->cy);
}

void image_source_preload_image(void *data)
{
	struct image_source *context = data;
	if (os_atomic_load_bool(&context->file_decoded))
		return;

	context->file_timestamp = get_modified_timestamp(context->file);
	gs_image_file4_init(&context->if4, context->file,
			    context->linear_alpha
				    ? GS_IMAGE_ALPHA_PREMULTIPLY_SRGB
				    : GS_IMAGE_ALPHA_PREMULTIPLY);
	limit_image_size(context);
	os_atomic_set_bool(&context->file_decoded, true);
}

static void image_source_load_texture(void *data)
{
	struct image_source *context = data;
	if (os_atomic_load_bool(&context->texture_loaded))
		return;

	debug("loading texture '%s'", context->file);

	obs_enter_graphics();
	gs_image_file4_init_texture(&context->if4);
	obs_leave_graphics();

	if (!context->if4.image3.image2.image.loaded)
		warn("failed to load texture '%s'", context->file);
	context->update_time_elapsed = 0;
	os_atomic_set_bool(&context->texture_loaded, true);
}

static void image_source_unload(void *data)
{
	struct image_source *context = data;
	os_atomic_set_bool(&context->file_decoded, false);
	os_atomic_set_bool(&context->texture_loaded, false);

	obs_enter_graphics();
	gs_image_file4_free(&context->if4);
	obs_leave_graphics();
}

static void image_source_load(struct image_source *context)
{
	image_source_unload(context);

	if (context->file && *context->file) {
		image_source_preload_image(context);
		image_source_load_texture(context);
	}
}

static void image_source_update(void *data, obs_data_t *settings)
{
	struct image_source *context = data;
	const char *file = obs_data_get_string(settings, "file");
	const bool unload = obs_data_get_bool(settings, "unload");
	const bool linear_alpha = obs_data_get_bool(settings, "linear_alpha");
	const bool is_slide = obs_data_get_bool(settings, "is_slide");

	/* not in the properties: set by the slide show */
	context->max_cx = (uint32_t)obs_data_get_int(settings, "max_cx");
	context->max_cy = (uint32_t)obs_data_get_int(settings, "max_cy");

	if (context->file)
		bfree(context->file);
	context->file = bstrdup(file);
	context->persistent = !unload;
	context->linear_alpha = linear_alpha;
	context->is_slide = is_slide;

	if (is_slide)
		return;

	/* Load the image if the source is persistent or showing */
	if (context->persistent || obs_source_showing(context->source))
		image_source_load(data);
	else
		image_source_unload(data);
}

static void image_source_defaults(obs_data_t *settings)
{
	obs_data_set_default_bool(settings, "unload", false);
	obs_data_set_default_bool(settings, "linear_alpha", false);
}

static void image_source_show(void *data)
{
	struct image_source *context = data;

	if (!context->persistent && !context->is_slide)
		image_source_load(context);
}

static void image_source_hide(void *data)
{
	struct image_source *context = data;

	if (!context->persistent && !context->is_slide)
		image_source_unload(context);
}

static void restart_gif(void *data)
{
	struct image_source *context = data;

	if (context->if4.image3.image2.image.is_animated_gif) {
		context->if4.image3.image2.image.cur_frame = 0;
		context->if4.image3.image2.image.cur_loop = 0;
		context->if4.image3.image2.image.cur_time = 0;

		obs_enter_graphics();
		gs_image_file4_update_texture(&context->if4);
		obs_leave_graphics();

		context->restart_gif = false;
	}
}

static void image_source_activate(void *data)
{
	struct image_source *context = data;
	context->restart_gif = true;
}

static void *image_source_create(obs_data_t *settings, obs_source_t *source)
{
	struct image_source *context = bzalloc(sizeof(struct image_source));
	context->source = source;

	image_source_update(context, settings);
	return context;
}

static void image_source_destroy(void *data)
{
	struct image_source *context = data;

	image_source_unload(context);

	if (context->file)
		bfree(context->file);
	bfree(context);
}

static uint32_t image_source_getwidth(void *data)
{
	struct image_source *context = data;
	return os_atomic_load_bool(&context->file_decoded)
		       ? context->cx
		       : context->if4.image3.image2.image.cx;
}

static uint32_t image_source_getheight(void *data)
{
	struct image_source *context = data;
	return os_atomic_load_bool(&context->file_decoded)
		       ? context->cy
		       : context->if4.image3.image2.image.cy;
}

static void image_source_render(void *data, gs_effect_t *effect)
{
	struct image_source *context = data;
	if (!os_atomic_load_bool(&context->texture_loaded))
		return;

	struct gs_image_file *const image = &context->if4.image3.image2.image;
	gs_texture_t *const texture = image->texture;
	if (!texture)
		return;

	const bool previous = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(true);

	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);

	gs_eparam_t *const param = gs_effect_get_param_by_name(effect, "image");
	gs_effect_set_texture_srgb(param, texture);

	gs_draw_sprite(texture, 0, context->cx, context->cy);

	gs_blend_state_pop();

	gs_enable_framebuffer_srgb(previous);
}

static void image_source_tick(void *data, float seconds)
{
	struct image_source *context = data;
	if (!os_atomic_load_bool(&context->texture_loaded)) {
		if (os_atomic_load_bool(&context->file_decoded))
			image_source_load_texture(context);
		else
			return;
	}

	uint64_t frame_time = obs_get_video_frame_time();

	context->update_time_elapsed += seconds;

	if (obs_source_showing(context->source)) {
		if (context->update_time_elapsed >= 1.0f) {
			time_t t = get_modified_timestamp(context->file);
			context->update_time_elapsed = 0.0f;

			if (context->file_timestamp != t) {
				image_source_load(context);
			}
		}
	}

	if (obs_source_showing(context->source)) {
		if (!context->active) {
			if (context->if4.image3.image2.image.is_animated_gif)
				context->last_time = frame_time;
			context->active = true;
		}

		if (context->restart_gif)
			restart_gif(context);

	} else {
		if (context->active) {
			restart_gif(context);
			context->active = false;
		}

		return;
	}

	if (context->last_time &&
	    context->if4.image3.image2.image.is_animated_gif) {
		uint64_t elapsed = frame_time - context->last_time;
		bool updated = gs_image_file4_tick(&context->if4, elapsed);

		if (updated) {
			obs_enter_graphics();
			gs_image_file4_update_texture(&context->if4);
			obs_leave_graphics();
		}
	}

	context->last_time = frame_time;
}

static const char *image_filter =
#ifdef _WIN32
	"All formats (*.bmp *.tga *.png *.jpeg *.jpg *.jxr *.gif *.psd *.webp);;"
#else
	"All formats (*.bmp *.tga *.png *.jpeg *.jpg *.gif *.psd *.webp);;"
#endif
	"BMP Files (*.bmp);;"
	"Targa Files (*.tga);;"
	"PNG Files (*.png);;"
	"JPEG Files (*.jpeg *.jpg);;"
#ifdef _WIN32
	"JXR Files (*.jxr);;"
#endif
	"GIF Files (*.gif);;"
	"PSD Files (*.psd);;"
	"WebP Files (*.webp);;"
	"All Files (*.*)";

static obs_properties_t *image_source_properties(void *data)
{
	struct image_source *s = data;
	struct dstr path = {0};

	obs_properties_t *props = obs_properties_create();

	if (s && s->file && *s->file) {
		const char *slash;

		dstr_copy(&path, s->file);
		dstr_replace(&path, "\\", "/");
		slash = strrchr(path.array, '/');
		if (slash)
			dstr_resize(&path, slash - path.array + 1);
	}

	obs_properties_add_path(props, "file", obs_module_text("File"),
				OBS_PATH_FILE, image_filter, path.array);
	obs_properties_add_bool(props, "unload",
				obs_module_text("UnloadWhenNotShowing"));
	obs_properties_add_bool(props, "linear_alpha",
				obs_module_text("LinearAlpha"));
	dstr_free(&path);

	return props;
}

uint64_t image_source_get_memory_usage(void *data)
{
	struct image_source *s = data;
	return s->if4.image3.image2.mem_usage;
}

static void missing_file_callback(void *src, const char *new_path, void *data)
{
	struct image_source *s = src;

	obs_source_t *source = s->source;
	obs_data_t *settings = obs_source_get_settings(source);
	obs_data_set_string(settings, "file", new_path);
	obs_source_update(source, settings);
	obs_data_release(settings);

	UNUSED_PARAMETER(data);
}

static obs_missing_files_t *image_source_missingfiles(void *data)
{
	struct image_source *s = data;
	obs_missing_files_t *files = obs_missing_files_create();

	if (strcmp(s->file, "") != 0) {
		if (!os_file_exists(s->file)) {
			obs_missing_file_t *file = obs_missing_file_create(
				s->file, missing_file_callback,
				OBS_MISSING_FILE_SOURCE, s->source, NULL);

			obs_missing_files_add_file(files, file);
		}
	}

	return files;
}

static enum gs_color_space
image_source_get_color_space(void *data, size_t count,
			     const enum gs_color_space *preferred_spaces)
{
	UNUSED_PARAMETER(count);
	UNUSED_PARAMETER(preferred_spaces);

	struct image_source *const s = data;
	gs_image_file4_t *const if4 = &s->if4;
	return if4->image3.image2.image.texture ? if4->space : GS_CS_SRGB;
}

static struct obs_source_info image_source_info = {
	.id = "image_source",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_SRGB,
	.get_name = image_source_get_name,
	.create = image_source_create,
	.destroy = image_source_destroy,
	.update = image_source_update,
	.get_defaults = image_source_defaults,
	.show = image_source_show,
	.hide = image_source_hide,
	.get_width = image_source_getwidth,
	.get_height = image_source_getheight,
	.video_render = image_source_render,
	.video_tick = image_source_tick,
	.missing_files = image_source_missingfiles,
	.get_properties = image_source_properties,
	.icon_type = OBS_ICON_TYPE_IMAGE,
	.activate = image_source_activate,
	.video_get_color_space = image_source_get_color_space,
};

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("image-source", "en-US")
MODULE_EXPORT const char *obs_module_description(void)
{
	return "Image/color/slideshow sources";
}

extern struct obs_source_info slideshow_info;
extern struct obs_source_info slideshow_info_mk2;
extern struct obs_source_info color_source_info_v1;
extern struct obs_source_info color_source_info_v2;
extern struct obs_source_info color_source_info_v3;

bool obs_module_load(void)
{
	obs_register_source(&image_source_info);
	obs_register_source(&color_source_info_v1);
	obs_register_source(&color_source_info_v2);
	obs_register_source(&color_source_info_v3);
	obs_register_source(&slideshow_info);
	obs_register_source(&slideshow_info_mk2);
	return true;
}
