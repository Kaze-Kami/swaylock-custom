#include <math.h>
#include <stdlib.h>
#include <time.h>
#include <locale.h>
#include <wayland-client.h>
#include "cairo.h"
#include "background-image.h"
#include "swaylock.h"
#include "log.h"

#define M_PI 3.14159265358979323846
const float TYPE_INDICATOR_RANGE = M_PI / 3.0f;
const float TYPE_INDICATOR_BORDER_THICKNESS = M_PI / 128.0f;

static uint32_t get_font_size(struct swaylock_state *state, int arc_radius) {
	if (state->args.font_size > 0) {
		return state->args.font_size;
	} else {
		return arc_radius / 3.0f;
	}
}

void render_frame_background(struct swaylock_surface *surface) {
	struct swaylock_state *state = surface->state;

	int buffer_width = surface->width * surface->scale;
	int buffer_height = surface->height * surface->scale;
	if (buffer_width == 0 || buffer_height == 0) {
		return; // not yet configured
	}

	wl_surface_set_buffer_scale(surface->surface, surface->scale);

	if (buffer_width != surface->last_buffer_width ||
			buffer_height != surface->last_buffer_height) {
		struct pool_buffer buffer;
		if (!create_buffer(state->shm, &buffer, buffer_width, buffer_height, WL_SHM_FORMAT_ARGB8888)) {
			swaylock_log(LOG_ERROR, "Failed to create new buffer for frame background.");
			return;
		}

		cairo_t *cairo = buffer.cairo;
		cairo_set_antialias(cairo, CAIRO_ANTIALIAS_BEST);

		cairo_save(cairo);
		cairo_set_operator(cairo, CAIRO_OPERATOR_SOURCE);
		cairo_set_source_u32(cairo, state->args.colors.background);
		cairo_paint(cairo);
		if (surface->image && state->args.mode != BACKGROUND_MODE_SOLID_COLOR) {
			cairo_set_operator(cairo, CAIRO_OPERATOR_OVER);
			render_background_image(cairo, surface->image,
				state->args.mode, buffer_width, buffer_height);
		}
		cairo_restore(cairo);
		cairo_identity_matrix(cairo);

		wl_surface_attach(surface->surface, buffer.buffer, 0, 0);
		wl_surface_damage_buffer(surface->surface, 0, 0, INT32_MAX, INT32_MAX);
		wl_surface_commit(surface->surface);
		destroy_buffer(&buffer);

		surface->last_buffer_width = buffer_width;
		surface->last_buffer_height = buffer_height;
	} else {
		wl_surface_commit(surface->surface);
	}
}

static void configure_font_drawing(cairo_t *cairo, struct swaylock_state *state,
		enum wl_output_subpixel subpixel, int arc_radius) {
	cairo_font_options_t *fo = cairo_font_options_create();
	cairo_font_options_set_hint_style(fo, CAIRO_HINT_STYLE_FULL);
	cairo_font_options_set_antialias(fo, CAIRO_ANTIALIAS_SUBPIXEL);
	cairo_font_options_set_subpixel_order(fo, to_cairo_subpixel_order(subpixel));

	cairo_set_font_options(cairo, fo);
	cairo_select_font_face(cairo, state->args.font,
		CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
	if (state->args.font_size > 0) {
		cairo_set_font_size(cairo, state->args.font_size);
	} else {
		cairo_set_font_size(cairo, arc_radius / 3.0f);
	}
	cairo_font_options_destroy(fo);
}

static void timetext(struct swaylock_surface *surface, char **tstr, char **dstr) {
	static char dbuf[256];
	static char tbuf[256];

	// Use user's locale for strftime calls
	char *prevloc = setlocale(LC_TIME, NULL);
	setlocale(LC_TIME, "");

	time_t t = time(NULL);
	struct tm *tm = localtime(&t);

	if (surface->state->args.timestr[0]) {
		strftime(tbuf, sizeof(tbuf), surface->state->args.timestr, tm);
		*tstr = tbuf;
	} else {
		*tstr = NULL;
	}

	if (surface->state->args.datestr[0]) {
		strftime(dbuf, sizeof(dbuf), surface->state->args.datestr, tm);
		*dstr = dbuf;
	} else {
		*dstr = NULL;
	}

	// Set it back, so we don't break stuff
	setlocale(LC_TIME, prevloc);
}

void render_frame(struct swaylock_surface *surface) {
	struct swaylock_state *state = surface->state;
	// First, compute the text that will be drawn, if any, since this
	// determines the size/positioning of the surface

	char *text_l1 = NULL;
	char *text_l2 = NULL;
	if (state->args.clock) {
		timetext(surface, &text_l1, &text_l2);
	}
	
	bool draw_indicator = state->args.show_indicator &&
		(state->auth_state != AUTH_STATE_IDLE ||
			state->input_state != INPUT_STATE_IDLE ||
			state->args.indicator_idle_visible);

	// Compute the size of the buffer needed
	int arc_radius = state->args.radius * surface->scale;
	int arc_thickness = state->args.thickness * surface->scale;
	int buffer_diameter = (arc_radius + arc_thickness) * 2;
	int buffer_width = buffer_diameter;
	int buffer_height = buffer_diameter;

	// Ensure buffer size is multiple of buffer scale - required by protocol
	buffer_height += surface->scale - (buffer_height % surface->scale);
	buffer_width += surface->scale - (buffer_width % surface->scale);

	int subsurf_xpos;
	int subsurf_ypos;

	// Center the indicator unless overridden by the user
	if (state->args.indicator_x_position >= 0) {
		subsurf_xpos = state->args.indicator_x_position -
			buffer_width / (2 * surface->scale) + 2 / surface->scale;
	} else {
		subsurf_xpos = surface->width / 2 -
			buffer_width / (2 * surface->scale) + 2 / surface->scale;
	}

	if (state->args.indicator_y_position >= 0) {
		subsurf_ypos = state->args.indicator_y_position -
			(state->args.radius + state->args.thickness);
	} else {
		subsurf_ypos = surface->height / 2 -
			(state->args.radius + state->args.thickness);
	}

	struct pool_buffer *buffer = get_next_buffer(state->shm,
			surface->indicator_buffers, buffer_width, buffer_height);
	if (buffer == NULL) {
		return;
	}

	// Render the buffer
	cairo_t *cairo = buffer->cairo;
	cairo_set_antialias(cairo, CAIRO_ANTIALIAS_BEST);

	cairo_identity_matrix(cairo);

	// Clear
	cairo_save(cairo);
	cairo_set_source_rgba(cairo, 0, 0, 0, 0);
	cairo_set_operator(cairo, CAIRO_OPERATOR_SOURCE);
	cairo_paint(cairo);
	cairo_restore(cairo);
	
	if (draw_indicator) {
		// Draw ring
		cairo_set_line_width(cairo, arc_thickness);
		cairo_arc(cairo, buffer_width / 2, buffer_diameter / 2, arc_radius, 0, 2 * M_PI);
		cairo_set_source_u32(cairo, state->args.colors.ring);
		cairo_stroke(cairo);

		// Draw message
		configure_font_drawing(cairo, state, surface->subpixel, arc_radius);
		cairo_set_source_u32(cairo, state->args.colors.text);
		
		cairo_text_extents_t extents_l1, extents_l2;
		cairo_font_extents_t fe_l1, fe_l2;
		double x_l1, y_l1, x_l2, y_l2;

		/* First line */
		if (text_l1 != NULL) {
			cairo_text_extents(cairo, text_l1, &extents_l1);
			cairo_font_extents(cairo, &fe_l1);
			x_l1 = (buffer_width / 2) -
				(extents_l1.width / 2 + extents_l1.x_bearing);
			y_l1 = (buffer_diameter / 2) +
				(fe_l1.height / 2 - fe_l1.descent) - arc_radius / 10.0f;

			cairo_move_to(cairo, x_l1, y_l1);
			cairo_show_text(cairo, text_l1);
			cairo_close_path(cairo);
			cairo_new_sub_path(cairo);
		}

		/* Second line */
		if (text_l2 != NULL) {
			cairo_set_font_size(cairo, arc_radius / 6.0f);
			cairo_text_extents(cairo, text_l2, &extents_l2);
			cairo_font_extents(cairo, &fe_l2);
			x_l2 = (buffer_width / 2) -
				(extents_l2.width / 2 + extents_l2.x_bearing);
			y_l2 = (buffer_diameter / 2) +
				(fe_l2.height / 2 - fe_l2.descent) + arc_radius / 3.5f;

			cairo_move_to(cairo, x_l2, y_l2);
			cairo_show_text(cairo, text_l2);
			cairo_close_path(cairo);
			cairo_new_sub_path(cairo);

			cairo_set_font_size(cairo, get_font_size(state, arc_radius));
		}

		// Typing indicator: Highlight random part on keypress
		if (state->input_state == INPUT_STATE_LETTER || state->input_state == INPUT_STATE_BACKSPACE) {
			double highlight_start = state->highlight_start * (M_PI / 1024.0);
			cairo_arc(cairo, buffer_width / 2, buffer_diameter / 2, arc_radius, highlight_start, highlight_start + TYPE_INDICATOR_RANGE);
			if (state->input_state == INPUT_STATE_LETTER) {
				cairo_set_source_u32(cairo, state->args.colors.highlight_key);
			} else {
				cairo_set_source_u32(cairo, state->args.colors.highlight_bs);
			}
			cairo_stroke(cairo);
		}

		// Draw inner + outer border of the circle
		if (state->input_state == INPUT_STATE_CLEAR) {
			cairo_set_source_u32(cairo, state->args.colors.highlight_clear);
		} else if (state->auth_state == AUTH_STATE_VALIDATING) {
			cairo_set_source_u32(cairo, state->args.colors.highlight_ver);
		} else if (state->auth_state == AUTH_STATE_INVALID) {
			cairo_set_source_u32(cairo, state->args.colors.highlight_wrong);
		} else {
			cairo_set_source_u32(cairo, state->args.colors.ring);
		}

		cairo_set_line_width(cairo, 2.0 * surface->scale);
		cairo_arc(cairo, buffer_width / 2, buffer_diameter / 2,
				arc_radius - arc_thickness / 2, 0, 2 * M_PI);
		cairo_stroke(cairo);
		cairo_arc(cairo, buffer_width / 2, buffer_diameter / 2,
				arc_radius + arc_thickness / 2, 0, 2 * M_PI);
		cairo_stroke(cairo);
	}
	else {
		swaylock_log(LOG_INFO, "Not drawing indicator...");
	}

	// Send Wayland requests
	wl_subsurface_set_position(surface->subsurface, subsurf_xpos, subsurf_ypos);

	wl_surface_set_buffer_scale(surface->child, surface->scale);
	wl_surface_attach(surface->child, buffer->buffer, 0, 0);
	wl_surface_damage_buffer(surface->child, 0, 0, INT32_MAX, INT32_MAX);
	wl_surface_commit(surface->child);

	wl_surface_commit(surface->surface);
}
