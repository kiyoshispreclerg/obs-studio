#include <obs-module.h>
#include <util/deque.h>
#include <util/util_uint64.h>

#include <stdio.h>
#include <string.h>

#define S_DELAY_MS "delay_ms"
#define S_BUFFER_SCALE "buffer_scale"
#define S_BUFFER_DIVISOR "buffer_divisor"

#define BUFFER_SCALE_ORIGINAL "original"
#define BUFFER_SCALE_OUTPUT "output"
#define BUFFER_SCALE_CUSTOM "custom"

#define T_DELAY_MS obs_module_text("DelayMs")
#define T_BUFFER_SCALE obs_module_text("GPUDelay.BufferScale")
#define T_BUFFER_SCALE_ORIGINAL obs_module_text("GPUDelay.BufferScale.Original")
#define T_BUFFER_SCALE_OUTPUT obs_module_text("GPUDelay.BufferScale.Output")
#define T_BUFFER_SCALE_CUSTOM obs_module_text("GPUDelay.BufferScale.Custom")
#define T_BUFFER_DIVISOR obs_module_text("GPUDelay.BufferResolution")

/* same downscale steps as the output resolution list in Settings > Video */
static const double divisors[] = {1.25, 4.0 / 3.0, 1.5,  1.75, 2.0,
				  2.25, 2.5,       2.75, 3.0};

enum buffer_scale {
	SCALE_ORIGINAL,
	SCALE_OUTPUT,
	SCALE_CUSTOM,
};

struct frame {
	gs_texrender_t *render;
	enum gs_color_space space;
	uint64_t ts;
};

struct gpu_delay_filter_data {
	obs_source_t *context;
	struct deque frames;
	uint64_t delay_ns;
	uint64_t interval_ns;
	enum buffer_scale buffer_scale;
	double buffer_divisor;
	uint32_t cx;
	uint32_t cy;
	/* size the delayed frames are stored at, drawn stretched to cx/cy */
	uint32_t buffer_cx;
	uint32_t buffer_cy;
	bool target_valid;
	bool processed_frame;
};

static const char *gpu_delay_filter_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("GPUDelayFilter");
}

static void free_textures(struct gpu_delay_filter_data *f)
{
	obs_enter_graphics();
	while (f->frames.size) {
		struct frame frame;
		deque_pop_front(&f->frames, &frame, sizeof(frame));
		gs_texrender_destroy(frame.render);
	}
	deque_free(&f->frames);
	obs_leave_graphics();
}

static size_t num_frames(struct deque *buf)
{
	return buf->size / sizeof(struct frame);
}

static void update_interval(struct gpu_delay_filter_data *f,
			    uint64_t new_interval_ns)
{
	if (!f->target_valid) {
		free_textures(f);
		return;
	}

	f->interval_ns = new_interval_ns;
	size_t num = (size_t)(f->delay_ns / new_interval_ns);

	if (num > num_frames(&f->frames)) {
		size_t prev_num = num_frames(&f->frames);

		obs_enter_graphics();

		deque_upsize(&f->frames, num * sizeof(struct frame));

		for (size_t i = prev_num; i < num; i++) {
			struct frame *frame =
				deque_data(&f->frames, i * sizeof(*frame));
			frame->render =
				gs_texrender_create(GS_RGBA, GS_ZS_NONE);
		}

		obs_leave_graphics();

	} else if (num < num_frames(&f->frames)) {
		obs_enter_graphics();

		while (num_frames(&f->frames) > num) {
			struct frame frame;
			deque_pop_front(&f->frames, &frame, sizeof(frame));
			gs_texrender_destroy(frame.render);
		}

		obs_leave_graphics();
	}
}

static inline void check_interval(struct gpu_delay_filter_data *f)
{
	struct obs_video_info ovi = {0};
	uint64_t interval_ns;

	obs_get_video_info(&ovi);

	interval_ns = util_mul_div64(ovi.fps_den, 1000000000ULL, ovi.fps_num);

	if (interval_ns != f->interval_ns)
		update_interval(f, interval_ns);
}

static inline void reset_textures(struct gpu_delay_filter_data *f)
{
	f->interval_ns = 0;
	free_textures(f);
	check_interval(f);
}

static void get_buffer_size(struct gpu_delay_filter_data *f, uint32_t cx,
			    uint32_t cy, uint32_t *buffer_cx,
			    uint32_t *buffer_cy)
{
	double scale = 1.0;

	if (f->buffer_scale == SCALE_OUTPUT) {
		struct obs_video_info ovi = {0};
		obs_get_video_info(&ovi);

		/* fit inside the output resolution, never upscale */
		double sx = (double)ovi.output_width / (double)cx;
		double sy = (double)ovi.output_height / (double)cy;
		scale = sx < sy ? sx : sy;
		if (scale > 1.0 || scale <= 0.0)
			scale = 1.0;
	} else if (f->buffer_scale == SCALE_CUSTOM && f->buffer_divisor > 1.0) {
		scale = 1.0 / f->buffer_divisor;
	}

	*buffer_cx = (uint32_t)((double)cx * scale + 0.5);
	*buffer_cy = (uint32_t)((double)cy * scale + 0.5);
	if (!*buffer_cx)
		*buffer_cx = 1;
	if (!*buffer_cy)
		*buffer_cy = 1;
}

static inline bool check_size(struct gpu_delay_filter_data *f)
{
	obs_source_t *target = obs_filter_get_target(f->context);
	uint32_t cx;
	uint32_t cy;
	uint32_t buffer_cx;
	uint32_t buffer_cy;

	f->target_valid = !!target;
	if (!f->target_valid)
		return true;

	cx = obs_source_get_base_width(target);
	cy = obs_source_get_base_height(target);

	f->target_valid = !!cx && !!cy;
	if (!f->target_valid)
		return true;

	get_buffer_size(f, cx, cy, &buffer_cx, &buffer_cy);

	if (cx != f->cx || cy != f->cy || buffer_cx != f->buffer_cx ||
	    buffer_cy != f->buffer_cy) {
		f->cx = cx;
		f->cy = cy;
		f->buffer_cx = buffer_cx;
		f->buffer_cy = buffer_cy;
		reset_textures(f);
		return true;
	}

	return false;
}

static void gpu_delay_filter_update(void *data, obs_data_t *s)
{
	struct gpu_delay_filter_data *f = data;

	f->delay_ns = (uint64_t)obs_data_get_int(s, S_DELAY_MS) * 1000000ULL;

	const char *scale = obs_data_get_string(s, S_BUFFER_SCALE);
	if (strcmp(scale, BUFFER_SCALE_OUTPUT) == 0)
		f->buffer_scale = SCALE_OUTPUT;
	else if (strcmp(scale, BUFFER_SCALE_CUSTOM) == 0)
		f->buffer_scale = SCALE_CUSTOM;
	else
		f->buffer_scale = SCALE_ORIGINAL;
	f->buffer_divisor = obs_data_get_double(s, S_BUFFER_DIVISOR);

	/* full reset */
	f->cx = 0;
	f->cy = 0;
	f->interval_ns = 0;
	free_textures(f);
}

static bool buffer_scale_modified(obs_properties_t *props, obs_property_t *p,
				  obs_data_t *settings)
{
	const char *scale = obs_data_get_string(settings, S_BUFFER_SCALE);
	obs_property_set_visible(obs_properties_get(props, S_BUFFER_DIVISOR),
				 strcmp(scale, BUFFER_SCALE_CUSTOM) == 0);

	UNUSED_PARAMETER(p);
	return true;
}

static obs_properties_t *gpu_delay_filter_properties(void *data)
{
	struct gpu_delay_filter_data *f = data;
	obs_properties_t *props = obs_properties_create();

	obs_property_t *p = obs_properties_add_int(props, S_DELAY_MS,
						   T_DELAY_MS, 0, 500, 1);
	obs_property_int_set_suffix(p, " ms");

	p = obs_properties_add_list(props, S_BUFFER_SCALE, T_BUFFER_SCALE,
				    OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(p, T_BUFFER_SCALE_ORIGINAL,
				     BUFFER_SCALE_ORIGINAL);
	obs_property_list_add_string(p, T_BUFFER_SCALE_OUTPUT,
				     BUFFER_SCALE_OUTPUT);
	obs_property_list_add_string(p, T_BUFFER_SCALE_CUSTOM,
				     BUFFER_SCALE_CUSTOM);
	obs_property_set_long_description(
		p, obs_module_text("GPUDelay.BufferScale.Description"));
	obs_property_set_modified_callback(p, buffer_scale_modified);

	/* list resolutions proportional to the source when its size is known */
	obs_source_t *target = f ? obs_filter_get_target(f->context) : NULL;
	uint32_t cx = target ? obs_source_get_base_width(target) : 0;
	uint32_t cy = target ? obs_source_get_base_height(target) : 0;

	p = obs_properties_add_list(props, S_BUFFER_DIVISOR, T_BUFFER_DIVISOR,
				    OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_FLOAT);
	for (size_t i = 0; i < OBS_COUNTOF(divisors); i++) {
		char name[64];
		double percent = 100.0 / divisors[i];
		if (cx && cy)
			snprintf(name, sizeof(name), "%ux%u (%.1f%%)",
				 (uint32_t)((double)cx / divisors[i] + 0.5),
				 (uint32_t)((double)cy / divisors[i] + 0.5),
				 percent);
		else
			snprintf(name, sizeof(name), "%.1f%%", percent);
		obs_property_list_add_float(p, name, divisors[i]);
	}

	return props;
}

static void gpu_delay_filter_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, S_BUFFER_SCALE,
				    BUFFER_SCALE_ORIGINAL);
	obs_data_set_default_double(settings, S_BUFFER_DIVISOR, 1.5);
}

static void *gpu_delay_filter_create(obs_data_t *settings,
				     obs_source_t *context)
{
	struct gpu_delay_filter_data *f = bzalloc(sizeof(*f));
	f->context = context;

	obs_source_update(context, settings);
	return f;
}

static void gpu_delay_filter_destroy(void *data)
{
	struct gpu_delay_filter_data *f = data;

	free_textures(f);
	bfree(f);
}

/* the delayed frames are only filled while rendering, so once the source is
 * not shown anywhere they would sit in VRAM until the source shows up again,
 * and then play back stale frames. drop them; the delay fills up again
 * (showing nothing for the delay time) when the source is shown */
static void gpu_delay_filter_hide(void *data)
{
	struct gpu_delay_filter_data *f = data;

	f->interval_ns = 0;
	free_textures(f);
}

static void gpu_delay_filter_tick(void *data, float t)
{
	UNUSED_PARAMETER(t);

	struct gpu_delay_filter_data *f = data;

	f->processed_frame = false;

	if (check_size(f))
		return;
	check_interval(f);
}

static const char *
get_tech_name_and_multiplier(enum gs_color_space current_space,
			     enum gs_color_space source_space,
			     float *multiplier)
{
	const char *tech_name = "Draw";
	*multiplier = 1.f;

	switch (source_space) {
	case GS_CS_SRGB:
	case GS_CS_SRGB_16F:
		if (current_space == GS_CS_709_SCRGB) {
			tech_name = "DrawMultiply";
			*multiplier = obs_get_video_sdr_white_level() / 80.0f;
		}
		break;
	case GS_CS_709_EXTENDED:
		switch (current_space) {
		case GS_CS_SRGB:
		case GS_CS_SRGB_16F:
			tech_name = "DrawTonemap";
			break;
		case GS_CS_709_SCRGB:
			tech_name = "DrawMultiply";
			*multiplier = obs_get_video_sdr_white_level() / 80.0f;
			break;
		default:
			break;
		}
		break;
	case GS_CS_709_SCRGB:
		switch (current_space) {
		case GS_CS_SRGB:
		case GS_CS_SRGB_16F:
			tech_name = "DrawMultiplyTonemap";
			*multiplier = 80.0f / obs_get_video_sdr_white_level();
			break;
		case GS_CS_709_EXTENDED:
			tech_name = "DrawMultiply";
			*multiplier = 80.0f / obs_get_video_sdr_white_level();
			break;
		default:
			break;
		}
	}

	return tech_name;
}

static void draw_frame(struct gpu_delay_filter_data *f)
{
	struct frame frame;
	deque_peek_front(&f->frames, &frame, sizeof(frame));

	const enum gs_color_space current_space = gs_get_color_space();
	float multiplier;
	const char *technique = get_tech_name_and_multiplier(
		current_space, frame.space, &multiplier);

	gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
	gs_texture_t *tex = gs_texrender_get_texture(frame.render);
	if (tex) {
		const bool previous = gs_framebuffer_srgb_enabled();
		gs_enable_framebuffer_srgb(true);

		gs_effect_set_texture_srgb(
			gs_effect_get_param_by_name(effect, "image"), tex);
		gs_effect_set_float(gs_effect_get_param_by_name(effect,
								"multiplier"),
				    multiplier);

		while (gs_effect_loop(effect, technique))
			gs_draw_sprite(tex, 0, f->cx, f->cy);

		gs_enable_framebuffer_srgb(previous);
	}
}

static void gpu_delay_filter_render(void *data, gs_effect_t *effect)
{
	struct gpu_delay_filter_data *f = data;
	obs_source_t *target = obs_filter_get_target(f->context);
	obs_source_t *parent = obs_filter_get_parent(f->context);

	if (!f->target_valid || !target || !parent || !f->frames.size) {
		obs_source_skip_video_filter(f->context);
		return;
	}

	if (f->processed_frame) {
		draw_frame(f);
		return;
	}

	struct frame frame;
	deque_pop_front(&f->frames, &frame, sizeof(frame));

	const enum gs_color_space preferred_spaces[] = {
		GS_CS_SRGB,
		GS_CS_SRGB_16F,
		GS_CS_709_EXTENDED,
	};
	const enum gs_color_space space = obs_source_get_color_space(
		target, OBS_COUNTOF(preferred_spaces), preferred_spaces);
	const enum gs_color_format format = gs_get_format_from_space(space);
	if (gs_texrender_get_format(frame.render) != format) {
		gs_texrender_destroy(frame.render);
		frame.render = gs_texrender_create(format, GS_ZS_NONE);
	}

	gs_texrender_reset(frame.render);

	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);

	/* the ortho below keeps source coordinates, so rendering to a smaller
	 * buffer just scales the source down */
	if (gs_texrender_begin_with_color_space(frame.render, f->buffer_cx,
						f->buffer_cy, space)) {
		uint32_t parent_flags = obs_source_get_output_flags(target);
		bool custom_draw = (parent_flags & OBS_SOURCE_CUSTOM_DRAW) != 0;
		bool async = (parent_flags & OBS_SOURCE_ASYNC) != 0;
		struct vec4 clear_color;

		vec4_zero(&clear_color);
		gs_clear(GS_CLEAR_COLOR, &clear_color, 0.0f, 0);
		gs_ortho(0.0f, (float)f->cx, 0.0f, (float)f->cy, -100.0f,
			 100.0f);

		if (target == parent && !custom_draw && !async)
			obs_source_default_render(target);
		else
			obs_source_video_render(target);

		gs_texrender_end(frame.render);

		frame.space = space;
	}

	gs_blend_state_pop();

	deque_push_back(&f->frames, &frame, sizeof(frame));
	draw_frame(f);
	f->processed_frame = true;

	UNUSED_PARAMETER(effect);
}

static enum gs_color_space
gpu_delay_filter_get_color_space(void *data, size_t count,
				 const enum gs_color_space *preferred_spaces)
{
	struct gpu_delay_filter_data *const f = data;
	obs_source_t *target = obs_filter_get_target(f->context);
	obs_source_t *parent = obs_filter_get_parent(f->context);

	if (!f->target_valid || !target || !parent || !f->frames.size) {
		return (count > 0) ? preferred_spaces[0] : GS_CS_SRGB;
	}

	struct frame frame;
	deque_peek_front(&f->frames, &frame, sizeof(frame));
	enum gs_color_space space = frame.space;
	for (size_t i = 0; i < count; ++i) {
		space = preferred_spaces[i];
		if (space == frame.space)
			break;
	}

	return space;
}

struct obs_source_info gpu_delay_filter = {
	.id = "gpu_delay",
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_VIDEO,
	.get_name = gpu_delay_filter_get_name,
	.create = gpu_delay_filter_create,
	.destroy = gpu_delay_filter_destroy,
	.update = gpu_delay_filter_update,
	.get_properties = gpu_delay_filter_properties,
	.get_defaults = gpu_delay_filter_defaults,
	.video_tick = gpu_delay_filter_tick,
	.hide = gpu_delay_filter_hide,
	.video_render = gpu_delay_filter_render,
	.video_get_color_space = gpu_delay_filter_get_color_space,
};
