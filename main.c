#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include <wordexp.h>
#include "background-image.h"
#include "cairo.h"
#include "comm.h"
#include "log.h"
#include "loop.h"
#include "password-buffer.h"
#include "pool-buffer.h"
#include "seat.h"
#include "swaylock.h"
#include "ext-session-lock-v1-client-protocol.h"

static uint32_t parse_color(const char *color) {
	if (color[0] == '#') {
		++color;
	}

	int len = strlen(color);
	if (len != 6 && len != 8) {
		swaylock_log(LOG_DEBUG, "Invalid color %s, defaulting to 0xFFFFFFFF",
				color);
		return 0xFFFFFFFF;
	}
	uint32_t res = (uint32_t)strtoul(color, NULL, 16);
	if (strlen(color) == 6) {
		res = (res << 8) | 0xFF;
	}
	return res;
}

int lenient_strcmp(char *a, char *b) {
	if (a == b) {
		return 0;
	} else if (!a) {
		return -1;
	} else if (!b) {
		return 1;
	} else {
		return strcmp(a, b);
	}
}

static void destroy_surface(struct swaylock_surface *surface) {
	wl_list_remove(&surface->link);
	if (surface->ext_session_lock_surface_v1 != NULL) {
		ext_session_lock_surface_v1_destroy(surface->ext_session_lock_surface_v1);
	}
	if (surface->subsurface) {
		wl_subsurface_destroy(surface->subsurface);
	}
	if (surface->child) {
		wl_surface_destroy(surface->child);
	}
	if (surface->surface != NULL) {
		wl_surface_destroy(surface->surface);
	}
	destroy_buffer(&surface->indicator_buffers[0]);
	destroy_buffer(&surface->indicator_buffers[1]);
	wl_output_release(surface->output);
	free(surface);
}

static const struct ext_session_lock_surface_v1_listener ext_session_lock_surface_v1_listener;

static cairo_surface_t *select_image(struct swaylock_state *state,
		struct swaylock_surface *surface);

static bool surface_is_opaque(struct swaylock_surface *surface) {
	if (surface->image) {
		return cairo_surface_get_content(surface->image) == CAIRO_CONTENT_COLOR;
	}
	return (surface->state->args.colors.background & 0xff) == 0xff;
}

static void create_surface(struct swaylock_surface *surface) {
	struct swaylock_state *state = surface->state;

	surface->image = select_image(state, surface);

	surface->surface = wl_compositor_create_surface(state->compositor);
	assert(surface->surface);

	surface->child = wl_compositor_create_surface(state->compositor);
	assert(surface->child);
	surface->subsurface = wl_subcompositor_get_subsurface(state->subcompositor, surface->child, surface->surface);
	assert(surface->subsurface);
	wl_subsurface_set_sync(surface->subsurface);

	surface->ext_session_lock_surface_v1 = ext_session_lock_v1_get_lock_surface(
		state->ext_session_lock_v1, surface->surface, surface->output);
	ext_session_lock_surface_v1_add_listener(surface->ext_session_lock_surface_v1,
		&ext_session_lock_surface_v1_listener, surface);

	if (surface_is_opaque(surface) &&
			surface->state->args.mode != BACKGROUND_MODE_CENTER &&
			surface->state->args.mode != BACKGROUND_MODE_FIT) {
		struct wl_region *region =
			wl_compositor_create_region(surface->state->compositor);
		wl_region_add(region, 0, 0, INT32_MAX, INT32_MAX);
		wl_surface_set_opaque_region(surface->surface, region);
		wl_region_destroy(region);
	}

	surface->created = true;
}

static void ext_session_lock_surface_v1_handle_configure(void *data,
		struct ext_session_lock_surface_v1 *lock_surface, uint32_t serial,
		uint32_t width, uint32_t height) {
	struct swaylock_surface *surface = data;
	surface->width = width;
	surface->height = height;
	ext_session_lock_surface_v1_ack_configure(lock_surface, serial);
	render_frame_background(surface);
	render_frame(surface);
}

static const struct ext_session_lock_surface_v1_listener ext_session_lock_surface_v1_listener = {
	.configure = ext_session_lock_surface_v1_handle_configure,
};

static const struct wl_callback_listener surface_frame_listener;

static void surface_frame_handle_done(void *data, struct wl_callback *callback,
		uint32_t time) {
	struct swaylock_surface *surface = data;

	wl_callback_destroy(callback);
	surface->frame_pending = false;

	if (surface->dirty) {
		// Schedule a frame in case the surface is damaged again
		struct wl_callback *callback = wl_surface_frame(surface->surface);
		wl_callback_add_listener(callback, &surface_frame_listener, surface);
		surface->frame_pending = true;

		render_frame(surface);
		surface->dirty = false;
	}
}

static const struct wl_callback_listener surface_frame_listener = {
	.done = surface_frame_handle_done,
};

void damage_surface(struct swaylock_surface *surface) {
	if (surface->width == 0 || surface->height == 0) {
		// Not yet configured
		return;
	}

	surface->dirty = true;
	if (surface->frame_pending) {
		return;
	}

	struct wl_callback *callback = wl_surface_frame(surface->surface);
	wl_callback_add_listener(callback, &surface_frame_listener, surface);
	surface->frame_pending = true;
	wl_surface_commit(surface->surface);
}

void damage_state(struct swaylock_state *state) {
	struct swaylock_surface *surface;
	wl_list_for_each(surface, &state->surfaces, link) {
		damage_surface(surface);
	}
}

static void handle_wl_output_geometry(void *data, struct wl_output *wl_output,
		int32_t x, int32_t y, int32_t width_mm, int32_t height_mm,
		int32_t subpixel, const char *make, const char *model,
		int32_t transform) {
	struct swaylock_surface *surface = data;
	surface->subpixel = subpixel;
	if (surface->state->run_display) {
		damage_surface(surface);
	}
}

static void handle_wl_output_mode(void *data, struct wl_output *output,
		uint32_t flags, int32_t width, int32_t height, int32_t refresh) {
	// Who cares
}

static void handle_wl_output_done(void *data, struct wl_output *output) {
	struct swaylock_surface *surface = data;
	if (!surface->created && surface->state->run_display) {
		create_surface(surface);
	}
}

static void handle_wl_output_scale(void *data, struct wl_output *output,
		int32_t factor) {
	struct swaylock_surface *surface = data;
	surface->scale = factor;
	if (surface->state->run_display) {
		damage_surface(surface);
	}
}

static void handle_wl_output_name(void *data, struct wl_output *output,
		const char *name) {
	struct swaylock_surface *surface = data;
	surface->output_name = strdup(name);
}

static void handle_wl_output_description(void *data, struct wl_output *output,
		const char *description) {
	// Who cares
}

struct wl_output_listener _wl_output_listener = {
	.geometry = handle_wl_output_geometry,
	.mode = handle_wl_output_mode,
	.done = handle_wl_output_done,
	.scale = handle_wl_output_scale,
	.name = handle_wl_output_name,
	.description = handle_wl_output_description,
};

static void ext_session_lock_v1_handle_locked(void *data, struct ext_session_lock_v1 *lock) {
	struct swaylock_state *state = data;
	state->locked = true;
}

static void ext_session_lock_v1_handle_finished(void *data, struct ext_session_lock_v1 *lock) {
	swaylock_log(LOG_ERROR, "Failed to lock session -- is another lockscreen running?");
	exit(2);
}

static const struct ext_session_lock_v1_listener ext_session_lock_v1_listener = {
	.locked = ext_session_lock_v1_handle_locked,
	.finished = ext_session_lock_v1_handle_finished,
};

static void handle_global(void *data, struct wl_registry *registry, uint32_t name, const char *interface, uint32_t version) {
	struct swaylock_state *state = data;
	if (strcmp(interface, wl_compositor_interface.name) == 0) {
		state->compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 4);
	} else if (strcmp(interface, wl_subcompositor_interface.name) == 0) {
		state->subcompositor = wl_registry_bind(registry, name, &wl_subcompositor_interface, 1);
	} else if (strcmp(interface, wl_shm_interface.name) == 0) {
		state->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
	} else if (strcmp(interface, wl_seat_interface.name) == 0) {
		struct wl_seat *seat = wl_registry_bind(registry, name, &wl_seat_interface, 4);
		struct swaylock_seat *swaylock_seat = calloc(1, sizeof(struct swaylock_seat));
		swaylock_seat->state = state;
		wl_seat_add_listener(seat, &seat_listener, swaylock_seat);
	} else if (strcmp(interface, wl_output_interface.name) == 0) {
		struct swaylock_surface *surface = calloc(1, sizeof(struct swaylock_surface));
		surface->state = state;
		surface->output = wl_registry_bind(registry, name, &wl_output_interface, 4);
		surface->output_global_name = name;
		wl_output_add_listener(surface->output, &_wl_output_listener, surface);
		wl_list_insert(&state->surfaces, &surface->link);
	} else if (strcmp(interface, ext_session_lock_manager_v1_interface.name) == 0) {
		state->ext_session_lock_manager_v1 = wl_registry_bind(registry, name, &ext_session_lock_manager_v1_interface, 1);
	}
}

static void handle_global_remove(void *data, struct wl_registry *registry, uint32_t name) {
	struct swaylock_state *state = data;
	struct swaylock_surface *surface;
	wl_list_for_each(surface, &state->surfaces, link) {
		if (surface->output_global_name == name) {
			destroy_surface(surface);
			break;
		}
	}
}

static const struct wl_registry_listener registry_listener = {
	.global = handle_global,
	.global_remove = handle_global_remove,
};

static int sigusr_fds[2] = {-1, -1};

void do_sigusr(int sig) {
	(void)write(sigusr_fds[1], "1", 1);
}

static cairo_surface_t *select_image(struct swaylock_state *state, struct swaylock_surface *surface) {
	struct swaylock_image *image;
	cairo_surface_t *default_image = NULL;
	wl_list_for_each(image, &state->images, link) {
		if (lenient_strcmp(image->output_name, surface->output_name) == 0) {
			return image->cairo_surface;
		} else if (!image->output_name) {
			default_image = image->cairo_surface;
		}
	}
	return default_image;
}

static char *join_args(char **argv, int argc) {
	assert(argc > 0);
	int len = 0, i;
	for (i = 0; i < argc; ++i) {
		len += strlen(argv[i]) + 1;
	}
	char *res = malloc(len);
	len = 0;
	for (i = 0; i < argc; ++i) {
		strcpy(res + len, argv[i]);
		len += strlen(argv[i]);
		res[len++] = ' ';
	}
	res[len - 1] = '\0';
	return res;
}

static void load_image(char *arg, struct swaylock_state *state) {
	// [[<output>]:]<path>
	struct swaylock_image *image = calloc(1, sizeof(struct swaylock_image));
	char *separator = strchr(arg, ':');
	if (separator) {
		*separator = '\0';
		image->output_name = separator == arg ? NULL : strdup(arg);
		image->path = strdup(separator + 1);
	} else {
		image->output_name = NULL;
		image->path = strdup(arg);
	}

	struct swaylock_image *iter_image, *temp;
	wl_list_for_each_safe(iter_image, temp, &state->images, link) {
		if (lenient_strcmp(iter_image->output_name, image->output_name) == 0) {
			if (image->output_name) {
				swaylock_log(LOG_DEBUG,
						"Replacing image defined for output %s with %s",
						image->output_name, image->path);
			} else {
				swaylock_log(LOG_DEBUG, "Replacing default image with %s",
						image->path);
			}
			wl_list_remove(&iter_image->link);
			free(iter_image->cairo_surface);
			free(iter_image->output_name);
			free(iter_image->path);
			free(iter_image);
			break;
		}
	}

	// The shell will not expand ~ to the value of $HOME when an output name is
	// given. Also, any image paths given in the config file need to have shell
	// expansions performed
	wordexp_t p;
	while (strstr(image->path, "  ")) {
		image->path = realloc(image->path, strlen(image->path) + 2);
		char *ptr = strstr(image->path, "  ") + 1;
		memmove(ptr + 1, ptr, strlen(ptr) + 1);
		*ptr = '\\';
	}
	if (wordexp(image->path, &p, 0) == 0) {
		free(image->path);
		image->path = join_args(p.we_wordv, p.we_wordc);
		wordfree(&p);
	}

	// Load the actual image
	image->cairo_surface = load_background_image(image->path);
	if (!image->cairo_surface) {
		free(image);
		return;
	}
	wl_list_insert(&state->images, &image->link);
	swaylock_log(LOG_DEBUG, "Loaded image %s for output %s", image->path,
			image->output_name ? image->output_name : "*");
}

static void set_default_colors(struct swaylock_colors *colors) {
	colors->background = 0x95A5A6FF;
	colors->text = 0x2C3E50FF;
	colors->highlight_bs = 0xE67E22FF;
	colors->highlight_key = 0x1ABC9CFF;
	colors->ring = 0x3498DBF;
	colors->highlight_clear = 0x27AE60FF;
	colors->highlight_ver = 0x7f8C8DFF;
	colors->highlight_wrong = 0xC0392BFF;
}

enum line_mode {
	LM_LINE,
	LM_INSIDE,
	LM_RING,
};

static int parse_options(int argc, char **argv, struct swaylock_state *state, enum line_mode *line_mode, char **config_path) {
	enum long_option_codes {
		LO_CONFIG = 'C',
		LO_DEBUG = 'd',
		LO_HELP = 'h',
		LO_VERSION = 'v',
		LO_IMAGE = 'i',

		_UNUSED = 256,

		LO_IGNORE_EMPTY,
		LO_NO_INDICATOR,
		LO_IND_IDLE_VISIBLE,
		LO_IND_RADIUS,
		LO_IND_THICKNESS,
		LO_IND_X,
		LO_IND_Y,	
		
		LO_BACKGROUND_COLOR,
		LO_BACKGROUND_MODE,
		
		LO_FONT,
		LO_FONT_SIZE,
		
		LO_CLOCK,
		LO_TIMESTR,
		LO_DATESTR,

		LO_COLOR_TEXT,
		LO_COLOR_RING,
		LO_COLOR_HL_BS,
		LO_COLOR_HL_KEY,
		LO_COLOR_HL_CLEAR,
		LO_COLOR_HL_VER,
		LO_COLOR_HL_WRONG,
	};

	static struct option long_options[] = {
		// general
		{"config", required_argument, NULL, LO_CONFIG},
		{"debug", no_argument, NULL, LO_DEBUG},
		{"help", no_argument, NULL, LO_HELP},
		{"version", no_argument, NULL, LO_VERSION},
		{"image", required_argument, NULL, LO_IMAGE},
		// input & indicator	
		{"ignore-empty-password", no_argument, NULL, LO_IGNORE_EMPTY},
		{"no-indicator", no_argument, NULL, LO_NO_INDICATOR},
		{"indicator-idle-visible", no_argument, NULL, LO_IND_IDLE_VISIBLE},
		{"indicator-radius", required_argument, NULL, LO_IND_RADIUS},
		{"indicator-thickness", required_argument, NULL, LO_IND_THICKNESS},
		{"indicator-x-position", required_argument, NULL, LO_IND_X},
		{"indicator-y-position", required_argument, NULL, LO_IND_Y},
		// background
		{"color-background", required_argument, NULL, LO_BACKGROUND_COLOR},
		{"scaling", required_argument, NULL, LO_BACKGROUND_MODE},
		// font
		{"font", required_argument, NULL, LO_FONT},
		{"font-size", required_argument, NULL, LO_FONT_SIZE},
		// clock
		{"clock", no_argument, NULL, LO_CLOCK}, 
		{"timestr", required_argument, NULL, LO_TIMESTR},
		{"datestr", required_argument, NULL, LO_DATESTR},
		// colors
		{"color-text", required_argument, NULL, LO_COLOR_TEXT},
		{"color-ring", required_argument, NULL, LO_COLOR_RING},
		{"color-hl-bs", required_argument, NULL, LO_COLOR_HL_BS},
		{"color-hl-key", required_argument, NULL, LO_COLOR_HL_KEY},
		{"color-hl-clear", required_argument, NULL, LO_COLOR_HL_CLEAR},
		{"color-hl-ver", required_argument, NULL, LO_COLOR_HL_VER},
		{"color-hl-wrong", required_argument, NULL, LO_COLOR_HL_WRONG},
		{0, 0, 0, 0}
	};

	const char usage[] =
		"Usage: swaylock [options...]\n"
		"\n"
		"  -C, --config <config_file>       "
			"Path to the config file.\n"
		"  -d, --debug                      "
			"Enable debugging output.\n"
		"  -h, --help                       "
			"Show help message and quit.\n"
		"  -i, --image [[<output>]:]<path>  "
			"Display the given image, optionally only on the given output.\n"
		"  -v, --version                    "
			"Show the version number and quit.\n"
		"  --ignore-empty-password          "
			"When an empty password is provided, do not validate it.\n"
		" --no-indicator                    "
			"Don't show indicator at all.\n"
		"  --indicator-idle-visible         "
			"Sets the indicator to show even if idle.\n"
		"  --indicator-radius <radius>      "
			"Sets the indicator radius.\n"
		"  --indicator-thickness <thick>    "
			"Sets the indicator thickness.\n"
		"  --indicator-x-position <x>       "
			"Sets the horizontal position of the indicator.\n"
		"  --indicator-y-position <y>       "
			"Sets the vertical position of the indicator.\n"
		"  --scaling <mode>                 "
			"Image scaling mode: stretch, fill, fit, center, tile, solid_color.\n"
		"  --font <font>                    "
			"Sets the font of the text.\n"
		"  --font-size <size>               "
			"Sets a fixed font size for the indicator text.\n"
		" --clock                           "
			"Display a date and time inside indicator\n"
		"  --timestr <format>               "
			"The format string for the time. Defaults to '%T'.\n"
		"  --datestr <format>               "
			"The format string for the date. Defaults to '%a, %x'.\n"
		"  --color-text <color>             "
			"Sets the text color.\n"
		"  --color-ring <color>             "
			"Sets the color of ring segments.\n"
		"  --color-hl-bs <color>            "
			"Sets the color of backspace highlight segments.\n"
		"  --color-hl-key <color>           "
			"Sets the color of the key press highlight segments.\n"
		"  --color-hl-clear <color>         "
			"Sets the color of the clear password indicator.\n"
		"  --color-hl-ver <color>           "
			"Sets the color of the verifying password indicator.\n"
		"  --color-hl-wrong <color>         "
			"Sets the color of the wrong password indicator.\n"
		"\n"
		 "All <color> options are of the form <rrggbb[aa]>.\n";

	int c;
	optind = 1;
	while (1) {
		int opt_idx = 0;
		c = getopt_long(argc, argv, "C:dhvi:", long_options, &opt_idx);

		if (c == -1) {
			break;
		}
		switch (c) {
		case LO_CONFIG:
			if (config_path) {
				*config_path = strdup(optarg);
			}
			break;
		case LO_DEBUG:
			swaylock_log_init(LOG_DEBUG);
			break;
		case LO_VERSION:
			fprintf(stdout, "swaylock version " SWAYLOCK_VERSION "\n");
			exit(EXIT_SUCCESS);
			break;
		case LO_IMAGE:
			if (state) {
				load_image(optarg, state);
			}
			break;
		case LO_IGNORE_EMPTY:
			if (state) {
				state->args.ignore_empty = true;
			}
			break;
		case LO_NO_INDICATOR:
			if (state) {
				state->args.show_indicator = false;
			}
			break;
		case LO_IND_IDLE_VISIBLE:
			if (state) {
				state->args.indicator_idle_visible = true;
			}
			break;
		case LO_IND_RADIUS:
			if (state) {
				state->args.radius = strtol(optarg, NULL, 0);
			}
			break;
		case LO_IND_THICKNESS:
			if (state) {
				state->args.thickness = strtol(optarg, NULL, 0);
			}
			break;
		case LO_IND_X:
			if (state) {
				state->args.indicator_x_position = atoi(optarg);
			}
			break;
		case LO_IND_Y:
			if (state) {
				state->args.indicator_y_position = atoi(optarg);
			}
			break;
		case LO_BACKGROUND_MODE:
			if (state) {
				state->args.mode = parse_background_mode(optarg);
				if (state->args.mode == BACKGROUND_MODE_INVALID) {
					return 1;
				}
			}
			break;
		case LO_FONT:
			if (state) {
				free(state->args.font);
				state->args.font = strdup(optarg);
			}
			break;
		case LO_FONT_SIZE:
			if (state) {
				state->args.font_size = atoi(optarg);
			}
			break;
		case LO_CLOCK:
			if (state) {
				state->args.clock = true;
			}
			break;
		case LO_TIMESTR:
			if (state) {
				free(state->args.timestr);
				state->args.timestr = strdup(optarg);
			}
			break;
		case LO_DATESTR:
			if (state) {
				free(state->args.datestr);
				state->args.datestr = strdup(optarg);
			}
			break;
		case LO_COLOR_TEXT:
			if (state) {
				state->args.colors.text = parse_color(optarg);
			}
			break;
		case LO_COLOR_RING:
			if (state) {
				state->args.colors.ring = parse_color(optarg);
			}
			break;
		case LO_COLOR_HL_BS:
			if (state) {
				state->args.colors.highlight_bs = parse_color(optarg);
			}
			break;
		case LO_COLOR_HL_KEY:
			if (state) {
				state->args.colors.highlight_key = parse_color(optarg);
			}
			break;
		case LO_COLOR_HL_CLEAR:
			if (state) {
				state->args.colors.highlight_clear = parse_color(optarg);
			}
			break;
		case LO_COLOR_HL_VER:
			if (state) {
				state->args.colors.highlight_ver = parse_color(optarg);
			}
			break;
		case LO_COLOR_HL_WRONG:
			if (state) {
				state->args.colors.highlight_wrong = parse_color(optarg);
			}
			break;
		default:
			fprintf(stderr, "%s", usage);
			return 1;
		}
	}

	return 0;
}

static bool file_exists(const char *path) {
	return path && access(path, R_OK) != -1;
}

static char *get_config_path(void) {
	static const char *config_paths[] = {
		"$HOME/.swaylock/config",
		"$XDG_CONFIG_HOME/swaylock/config",
		SYSCONFDIR "/swaylock/config",
	};

	char *config_home = getenv("XDG_CONFIG_HOME");
	if (!config_home || config_home[0] == '\0') {
		config_paths[1] = "$HOME/.config/swaylock/config";
	}

	wordexp_t p;
	char *path;
	for (size_t i = 0; i < sizeof(config_paths) / sizeof(char *); ++i) {
		if (wordexp(config_paths[i], &p, 0) == 0) {
			path = strdup(p.we_wordv[0]);
			wordfree(&p);
			if (file_exists(path)) {
				return path;
			}
			free(path);
		}
	}

	return NULL;
}

static int load_config(char *path, struct swaylock_state *state,
		enum line_mode *line_mode) {
	FILE *config = fopen(path, "r");
	if (!config) {
		swaylock_log(LOG_ERROR, "Failed to read config. Running without it.");
		return 0;
	}
	char *line = NULL;
	size_t line_size = 0;
	ssize_t nread;
	int line_number = 0;
	int result = 0;
	while ((nread = getline(&line, &line_size, config)) != -1) {
		line_number++;

		if (line[nread - 1] == '\n') {
			line[--nread] = '\0';
		}

		if (!*line || line[0] == '#') {
			continue;
		}

		swaylock_log(LOG_DEBUG, "Config Line #%d: %s", line_number, line);
		char *flag = malloc(nread + 3);
		if (flag == NULL) {
			free(line);
			fclose(config);
			swaylock_log(LOG_ERROR, "Failed to allocate memory");
			return 0;
		}
		sprintf(flag, "--%s", line);
		char *argv[] = {"swaylock", flag};
		result = parse_options(2, argv, state, line_mode, NULL);
		free(flag);
		if (result != 0) {
			break;
		}
	}
	free(line);
	fclose(config);
	return 0;
}

static struct swaylock_state state;

static void display_in(int fd, short mask, void *data) {
	if (wl_display_dispatch(state.display) == -1) {
		state.run_display = false;
	}
}

static void comm_in(int fd, short mask, void *data) {
	if (read_comm_reply()) {
		// Authentication succeeded
		state.run_display = false;
	} else {
		state.auth_state = AUTH_STATE_INVALID;
		schedule_auth_idle(&state);
		++state.failed_attempts;
		damage_state(&state);
	}
}

static void term_in(int fd, short mask, void *data) {
	state.run_display = false;
}

// Check for --debug 'early' we also apply the correct loglevel
// to the forked child, without having to first proces all of the
// configuration (including from file) before forking and (in the
// case of the shadow backend) dropping privileges
void log_init(int argc, char **argv) {
	static struct option long_options[] = {
		{"debug", no_argument, NULL, 'd'},
        {0, 0, 0, 0}
    };
    int c;
	optind = 1;
    while (1) {
		int opt_idx = 0;
		c = getopt_long(argc, argv, "-:d", long_options, &opt_idx);
		if (c == -1) {
			break;
		}
		switch (c) {
		case 'd':
			swaylock_log_init(LOG_DEBUG);
			return;
		}
	}
	swaylock_log_init(LOG_ERROR);
}

int main(int argc, char **argv) {
	log_init(argc, argv);
	initialize_pw_backend(argc, argv);
	srand(time(NULL));

	enum line_mode line_mode = LM_LINE;
	state.failed_attempts = 0;
	state.args = (struct swaylock_args){
		.ignore_empty = true,
		.show_indicator = true,
		.indicator_idle_visible = false,
		.radius = 50,
		.thickness = 10,
		.indicator_x_position = -1,
		.indicator_y_position = -1,
		.mode = BACKGROUND_MODE_FILL,
		.font = strdup("sans-serif"),
		.font_size = 0,
		.clock = true,
		.timestr = strdup("%T"),
		.datestr = strdup("%a, %x"),
	};
	wl_list_init(&state.images);
	set_default_colors(&state.args.colors);

	char *config_path = NULL;
	int result = parse_options(argc, argv, NULL, NULL, &config_path);
	if (result != 0) {
		free(config_path);
		return result;
	}
	if (!config_path) {
		config_path = get_config_path();
	}

	if (config_path) {
		swaylock_log(LOG_DEBUG, "Found config at %s", config_path);
		int config_status = load_config(config_path, &state, &line_mode);
		free(config_path);
		if (config_status != 0) {
			free(state.args.font);
			return config_status;
		}
	}

	if (argc > 1) {
		swaylock_log(LOG_DEBUG, "Parsing CLI Args");
		int result = parse_options(argc, argv, &state, &line_mode, NULL);
		if (result != 0) {
			free(state.args.font);
			return result;
		}
	}

	state.password.len = 0;
	state.password.buffer_len = 1024;
	state.password.buffer = password_buffer_create(state.password.buffer_len);
	if (!state.password.buffer) {
		return EXIT_FAILURE;
	}

	if (pipe(sigusr_fds) != 0) {
		swaylock_log(LOG_ERROR, "Failed to pipe");
		return EXIT_FAILURE;
	}
	if (fcntl(sigusr_fds[1], F_SETFL, O_NONBLOCK) == -1) {
		swaylock_log(LOG_ERROR, "Failed to make pipe end nonblocking");
		return EXIT_FAILURE;
	}

	wl_list_init(&state.surfaces);
	state.xkb.context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	state.display = wl_display_connect(NULL);
	if (!state.display) {
		free(state.args.font);
		swaylock_log(LOG_ERROR, "Unable to connect to the compositor. "
				"If your compositor is running, check or set the "
				"WAYLAND_DISPLAY environment variable.");
		return EXIT_FAILURE;
	}
	state.eventloop = loop_create();

	struct wl_registry *registry = wl_display_get_registry(state.display);
	wl_registry_add_listener(registry, &registry_listener, &state);
	if (wl_display_roundtrip(state.display) == -1) {
		swaylock_log(LOG_ERROR, "wl_display_roundtrip() failed");
		return EXIT_FAILURE;
	}

	if (!state.compositor) {
		swaylock_log(LOG_ERROR, "Missing wl_compositor");
		return 1;
	}

	if (!state.subcompositor) {
		swaylock_log(LOG_ERROR, "Missing wl_subcompositor");
		return 1;
	}

	if (!state.shm) {
		swaylock_log(LOG_ERROR, "Missing wl_shm");
		return 1;
	}

	if (!state.ext_session_lock_manager_v1) {
		swaylock_log(LOG_ERROR, "Missing ext-session-lock-v1");
		return 1;
	}

	state.ext_session_lock_v1 = ext_session_lock_manager_v1_lock(state.ext_session_lock_manager_v1);
	ext_session_lock_v1_add_listener(state.ext_session_lock_v1,
		&ext_session_lock_v1_listener, &state);

	if (wl_display_roundtrip(state.display) == -1) {
		free(state.args.font);
		return 1;
	}

	state.test_surface = cairo_image_surface_create(CAIRO_FORMAT_RGB24, 1, 1);
	state.test_cairo = cairo_create(state.test_surface);

	struct swaylock_surface *surface;
	wl_list_for_each(surface, &state.surfaces, link) {
		create_surface(surface);
	}

	while (!state.locked) {
		if (wl_display_dispatch(state.display) < 0) {
			swaylock_log(LOG_ERROR, "wl_display_dispatch() failed");
			return 2;
		}
	}

	loop_add_fd(state.eventloop, wl_display_get_fd(state.display), POLLIN, display_in, NULL);

	loop_add_fd(state.eventloop, get_comm_reply_fd(), POLLIN, comm_in, NULL);

	loop_add_fd(state.eventloop, sigusr_fds[0], POLLIN, term_in, NULL);

	struct sigaction sa;
	sa.sa_handler = do_sigusr;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = SA_RESTART;
	sigaction(SIGUSR1, &sa, NULL);

	state.run_display = true;
	while (state.run_display) {
		errno = 0;
		if (wl_display_flush(state.display) == -1 && errno != EAGAIN) {
			break;
		}
		loop_poll(state.eventloop);
	}

	ext_session_lock_v1_unlock_and_destroy(state.ext_session_lock_v1);
	wl_display_roundtrip(state.display);

	free(state.args.font);
	cairo_destroy(state.test_cairo);
	cairo_surface_destroy(state.test_surface);
	return 0;
}
