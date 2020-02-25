/*
 * Cage: A Wayland kiosk.
 *
 * Copyright (C) 2018-2020 Jente Hidskes
 *
 * See the LICENSE file accompanying this file.
 */

#define _POSIX_C_SOURCE 200809L

#include "config.h"

#include <fcntl.h>
#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_export_dmabuf_v1.h>
#include <wlr/types/wlr_gamma_control_v1.h>
#include <wlr/types/wlr_idle.h>
#include <wlr/types/wlr_idle_inhibit_v1.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_screencopy_v1.h>
#include <wlr/types/wlr_server_decoration.h>
#if CAGE_HAS_XWAYLAND
#include <wlr/types/wlr_xcursor_manager.h>
#endif
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_xdg_output_v1.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>
#if CAGE_HAS_XWAYLAND
#include <wlr/xwayland.h>
#endif

#include "idle_inhibit_v1.h"
#include "output.h"
#include "seat.h"
#include "server.h"
#include "view.h"
#include "xdg_shell.h"
#if CAGE_HAS_XWAYLAND
#include "xwayland.h"
#endif
#include "present.h"

static int
sigchld_handler(int fd, uint32_t mask, void *data)
{
	struct cg_server *server = data;

	/* Close Cage's read pipe. */
	close(fd);

	if (mask & WL_EVENT_HANGUP) {
		wlr_log(WLR_DEBUG, "Child process closed normally");
	} else if (mask & WL_EVENT_ERROR) {
		wlr_log(WLR_DEBUG, "Connection closed by server");
	}

	/* Advance the command list by 1 (which will terminate
	 * the compositor if it goes past the end) */
	present_direction_keypress(server, 1);
	
	return 0;
}

static bool
set_cloexec(int fd)
{
	int flags = fcntl(fd, F_GETFD);

	if (flags == -1) {
		wlr_log(WLR_ERROR, "Unable to set the CLOEXEC flag: fnctl failed");
		return false;
	}

	flags = flags | FD_CLOEXEC;
	if (fcntl(fd, F_SETFD, flags) == -1) {
		wlr_log(WLR_ERROR, "Unable to set the CLOEXEC flag: fnctl failed");
		return false;
	}

	return true;
}

bool
spawn_client(struct cg_server *server, char *const argv[], pid_t *pid_out, struct wl_event_source **sigchld_source)
{
	int fd[2];
	if (pipe(fd) != 0) {
		wlr_log(WLR_ERROR, "Unable to create pipe");
		return false;
	}

	pid_t pid = fork();
	if (pid == 0) {
		sigset_t set;
		sigemptyset(&set);
		sigprocmask(SIG_SETMASK, &set, NULL);
		/* Close read, we only need write in the primary client process. */
		close(fd[0]);
		execvp(argv[0], argv);
		_exit(1);
	} else if (pid == -1) {
		wlr_log_errno(WLR_ERROR, "Unable to fork");
		return false;
	}

	/* Set this early so that if we fail, the client process will be cleaned up properly. */
	pid_t pid_prev = *pid_out;
	*pid_out = pid;

	if (!set_cloexec(fd[0]) || !set_cloexec(fd[1])) {
		return false;
	}

	/* Close write, we only need read in Cage. */
	close(fd[1]);

	struct wl_event_loop *event_loop = wl_display_get_event_loop(server->wl_display);
	uint32_t mask = WL_EVENT_HANGUP | WL_EVENT_ERROR;
	*sigchld_source = wl_event_loop_add_fd(event_loop, fd[0], mask, sigchld_handler, server);

	wlr_log(WLR_DEBUG, "Child process created with pid %d", pid);

	if (pid_prev != 0) {
		/* Terminate previous forked process */
		fprintf(stderr, "Previous PID = %d\n", pid_prev);
		kill(pid_prev, SIGHUP);
	}

	return true;
}

static bool
drop_permissions(void)
{
	if (getuid() != geteuid() || getgid() != getegid()) {
		if (setuid(getuid()) != 0 || setgid(getgid()) != 0) {
			wlr_log(WLR_ERROR, "Unable to drop root, refusing to start");
			return false;
		}
	}

	if (setuid(0) != -1) {
		wlr_log(WLR_ERROR,
			"Unable to drop root (we shouldn't be able to restore it after setuid), refusing to start");
		return false;
	}

	return true;
}

static int
handle_signal(int signal, void *data)
{
	struct cg_server *server = data;
	struct wl_display *display = server->wl_display;

	switch (signal) {
	case SIGINT:
		/* Fallthrough */
	case SIGTERM:
		wlr_log(WLR_DEBUG, "Interrupt signal caught, exiting...");
		wl_display_terminate(display);
		wlr_idle_notify_activity(server->idle, server->seat->seat);
		return 0;
	default:
		return 0;
	}
}

static void
usage(FILE *file, const char *cage)
{
	fprintf(file,
		"Usage: %s [OPTIONS] [--] APPLICATION\n"
		"\n"
		" -d            Do draw client side decorations, when possible (default=false)\n"
		" -r            Rotate the output 90 degrees clockwise, specify up to three times\n"
#ifdef DEBUG
		" -D            Turn on damage tracking debugging\n"
#endif
		" -h            Display this help message\n"
		" -v            Show the version number and exit\n"
		" -m <key>      Specify a modifier key (default=alt)\n"
		"    values:    alt, ctrl, shift, mod2, mod3, mod5, logo\n"
		"\n"
		" Use -- when you want to pass arguments to APPLICATION\n",
		cage);
}

static bool
parse_args(struct cg_server *server, int argc, char *argv[])
{
	int c;
#ifdef DEBUG
	while ((c = getopt(argc, argv, "drDhvm:")) != -1) {
#else
	while ((c = getopt(argc, argv, "drhvm:")) != -1) {
#endif
		switch (c) {
		case 'd':
			server->xdg_decoration = true;
			break;
		case 'r':
			server->output_transform++;
			if (server->output_transform > WL_OUTPUT_TRANSFORM_270) {
				server->output_transform = WL_OUTPUT_TRANSFORM_NORMAL;
			}
			break;
#ifdef DEBUG
		case 'D':
			server->debug_damage_tracking = true;
			break;
#endif
		case 'h':
			usage(stdout, argv[0]);
			return false;
		case 'm':
			if(configure_modifier_key(optarg)) {
				fprintf(stderr, "Incorrect modifier key specified (no such specifier: %s\n).",
						optarg);
				return false;
			}
			break;
		case 'v':
			fprintf(stdout, "Cage version " CAGE_VERSION "\n");
			exit(0);
		default:
			usage(stderr, argv[0]);
			return false;
		}
	}

	if (optind >= argc) {
		usage(stderr, argv[0]);
		return false;
	}

	return true;
}

int
main(int argc, char *argv[])
{
	struct cg_server server = {0};
	struct wl_event_loop *event_loop = NULL;
	struct wl_event_source *sigint_source = NULL;
	struct wl_event_source *sigterm_source = NULL;
	struct wlr_backend *backend = NULL;
	struct wlr_renderer *renderer = NULL;
	struct wlr_compositor *compositor = NULL;
	struct wlr_data_device_manager *data_device_manager = NULL;
	struct wlr_server_decoration_manager *server_decoration_manager = NULL;
	struct wlr_xdg_decoration_manager_v1 *xdg_decoration_manager = NULL;
	struct wlr_export_dmabuf_manager_v1 *export_dmabuf_manager = NULL;
	struct wlr_screencopy_manager_v1 *screencopy_manager = NULL;
	struct wlr_xdg_output_manager_v1 *output_manager = NULL;
	struct wlr_gamma_control_manager_v1 *gamma_control_manager = NULL;
	struct wlr_xdg_shell *xdg_shell = NULL;
#if CAGE_HAS_XWAYLAND
	struct wlr_xwayland *xwayland = NULL;
	struct wlr_xcursor_manager *xcursor_manager = NULL;
#endif
	int ret = 0;

	if (!parse_args(&server, argc, argv)) {
		return 1;
	}
	const char *filename = argv[optind];
	FILE *cmds_file = fopen(filename, "r");
	if (!cmds_file) {
		fprintf(stderr, "Couldn't open file %s for reading\n", filename);
		exit(1);
	}
	present_read_cmds(cmds_file);
	fclose(cmds_file);

#ifdef DEBUG
	wlr_log_init(WLR_DEBUG, NULL);
#else
	wlr_log_init(WLR_ERROR, NULL);
#endif

	/* Wayland requires XDG_RUNTIME_DIR to be set. */
	if (!getenv("XDG_RUNTIME_DIR")) {
		wlr_log(WLR_ERROR, "XDG_RUNTIME_DIR is not set in the environment");
		return 1;
	}

	server.wl_display = wl_display_create();
	if (!server.wl_display) {
		wlr_log(WLR_ERROR, "Cannot allocate a Wayland display");
		return 1;
	}

	event_loop = wl_display_get_event_loop(server.wl_display);
	sigint_source = wl_event_loop_add_signal(event_loop, SIGINT, handle_signal, &server);
	sigterm_source = wl_event_loop_add_signal(event_loop, SIGTERM, handle_signal, &server);
	backend = wlr_backend_autocreate(server.wl_display, NULL);
	if (!backend) {
		wlr_log(WLR_ERROR, "Unable to create the wlroots backend");
		ret = 1;
		goto end;
	}

	if (!drop_permissions()) {
		ret = 1;
		goto end;
	}

	renderer = wlr_backend_get_renderer(backend);
	wlr_renderer_init_wl_display(renderer, server.wl_display);

	wl_list_init(&server.views);
	wl_list_init(&server.outputs);

	server.output_layout = wlr_output_layout_create();
	if (!server.output_layout) {
		wlr_log(WLR_ERROR, "Unable to create output layout");
		ret = 1;
		goto end;
	}

	compositor = wlr_compositor_create(server.wl_display, renderer);
	if (!compositor) {
		wlr_log(WLR_ERROR, "Unable to create the wlroots compositor");
		ret = 1;
		goto end;
	}

	data_device_manager = wlr_data_device_manager_create(server.wl_display);
	if (!data_device_manager) {
		wlr_log(WLR_ERROR, "Unable to create the data device manager");
		ret = 1;
		goto end;
	}

	/* Configure a listener to be notified when new outputs are
	 * available on the backend. We use this only to detect the
	 * first output and ignore subsequent outputs. */
	server.new_output.notify = handle_new_output;
	wl_signal_add(&backend->events.new_output, &server.new_output);

	server.seat = seat_create(&server, backend);
	if (!server.seat) {
		wlr_log(WLR_ERROR, "Unable to create the seat");
		ret = 1;
		goto end;
	}

	server.idle = wlr_idle_create(server.wl_display);
	if (!server.idle) {
		wlr_log(WLR_ERROR, "Unable to create the idle tracker");
		ret = 1;
		goto end;
	}

	server.idle_inhibit_v1 = wlr_idle_inhibit_v1_create(server.wl_display);
	if (!server.idle_inhibit_v1) {
		wlr_log(WLR_ERROR, "Cannot create the idle inhibitor");
		ret = 1;
		goto end;
	}
	server.new_idle_inhibitor_v1.notify = handle_idle_inhibitor_v1_new;
	wl_signal_add(&server.idle_inhibit_v1->events.new_inhibitor, &server.new_idle_inhibitor_v1);
	wl_list_init(&server.inhibitors);

	xdg_shell = wlr_xdg_shell_create(server.wl_display);
	if (!xdg_shell) {
		wlr_log(WLR_ERROR, "Unable to create the XDG shell interface");
		ret = 1;
		goto end;
	}
	server.new_xdg_shell_surface.notify = handle_xdg_shell_surface_new;
	wl_signal_add(&xdg_shell->events.new_surface, &server.new_xdg_shell_surface);

	xdg_decoration_manager = wlr_xdg_decoration_manager_v1_create(server.wl_display);
	if (!xdg_decoration_manager) {
		wlr_log(WLR_ERROR, "Unable to create the XDG decoration manager");
		ret = 1;
		goto end;
	}
	wl_signal_add(&xdg_decoration_manager->events.new_toplevel_decoration, &server.xdg_toplevel_decoration);
	server.xdg_toplevel_decoration.notify = handle_xdg_toplevel_decoration;

	server_decoration_manager = wlr_server_decoration_manager_create(server.wl_display);
	if (!server_decoration_manager) {
		wlr_log(WLR_ERROR, "Unable to create the server decoration manager");
		ret = 1;
		goto end;
	}
	wlr_server_decoration_manager_set_default_mode(
		server_decoration_manager, server.xdg_decoration ? WLR_SERVER_DECORATION_MANAGER_MODE_CLIENT
								 : WLR_SERVER_DECORATION_MANAGER_MODE_SERVER);

	export_dmabuf_manager = wlr_export_dmabuf_manager_v1_create(server.wl_display);
	if (!export_dmabuf_manager) {
		wlr_log(WLR_ERROR, "Unable to create the export DMABUF manager");
		ret = 1;
		goto end;
	}

	screencopy_manager = wlr_screencopy_manager_v1_create(server.wl_display);
	if (!screencopy_manager) {
		wlr_log(WLR_ERROR, "Unable to create the screencopy manager");
		ret = 1;
		goto end;
	}

	output_manager = wlr_xdg_output_manager_v1_create(server.wl_display, server.output_layout);
	if (!output_manager) {
		wlr_log(WLR_ERROR, "Unable to create the output manager");
		ret = 1;
		goto end;
	}

	gamma_control_manager = wlr_gamma_control_manager_v1_create(server.wl_display);
	if (!gamma_control_manager) {
		wlr_log(WLR_ERROR, "Unable to create the gamma control manager");
		ret = 1;
		goto end;
	}

#if CAGE_HAS_XWAYLAND
	xwayland = wlr_xwayland_create(server.wl_display, compositor, true);
	if (!xwayland) {
		wlr_log(WLR_ERROR, "Cannot create XWayland server");
		ret = 1;
		goto end;
	}
	server.new_xwayland_surface.notify = handle_xwayland_surface_new;
	wl_signal_add(&xwayland->events.new_surface, &server.new_xwayland_surface);

	xcursor_manager = wlr_xcursor_manager_create(DEFAULT_XCURSOR, XCURSOR_SIZE);
	if (!xcursor_manager) {
		wlr_log(WLR_ERROR, "Cannot create XWayland XCursor manager");
		ret = 1;
		goto end;
	}

	if (setenv("DISPLAY", xwayland->display_name, true) < 0) {
		wlr_log_errno(WLR_ERROR, "Unable to set DISPLAY for XWayland. Clients may not be able to connect");
	} else {
		wlr_log(WLR_DEBUG, "XWayland is running on display %s", xwayland->display_name);
	}

	if (wlr_xcursor_manager_load(xcursor_manager, 1)) {
		wlr_log(WLR_ERROR, "Cannot load XWayland XCursor theme");
	}
	struct wlr_xcursor *xcursor = wlr_xcursor_manager_get_xcursor(xcursor_manager, DEFAULT_XCURSOR, 1);
	if (xcursor) {
		struct wlr_xcursor_image *image = xcursor->images[0];
		wlr_xwayland_set_cursor(xwayland, image->buffer, image->width * 4, image->width, image->height,
					image->hotspot_x, image->hotspot_y);
	}
#endif

	const char *socket = wl_display_add_socket_auto(server.wl_display);
	if (!socket) {
		wlr_log_errno(WLR_ERROR, "Unable to open Wayland socket");
		ret = 1;
		goto end;
	}

	if (!wlr_backend_start(backend)) {
		wlr_log(WLR_ERROR, "Unable to start the wlroots backend");
		ret = 1;
		goto end;
	}

	if (setenv("WAYLAND_DISPLAY", socket, true) < 0) {
		wlr_log_errno(WLR_ERROR, "Unable to set WAYLAND_DISPLAY. Clients may not be able to connect");
	} else {
		wlr_log(WLR_DEBUG, "Cage " CAGE_VERSION " is running on Wayland display %s", socket);
	}

#if CAGE_HAS_XWAYLAND
	wlr_xwayland_set_seat(xwayland, server.seat->seat);
#endif

	present_direction_keypress(&server, -1);

	/* Place the cursor in the center of the output layout. */
	struct wlr_box *layout_box = wlr_output_layout_get_box(server.output_layout, NULL);
	wlr_cursor_warp(server.seat->cursor, NULL, layout_box->width / 2, layout_box->height / 2);

	wl_display_run(server.wl_display);
	wlr_log(WLR_DEBUG, "Server ended");

#if CAGE_HAS_XWAYLAND
	wlr_xwayland_destroy(xwayland);
	wlr_xcursor_manager_destroy(xcursor_manager);
#endif
	wl_display_destroy_clients(server.wl_display);

end:

	wl_event_source_remove(sigint_source);
	wl_event_source_remove(sigterm_source);
	seat_destroy(server.seat);
	/* This function is not null-safe, but we only ever get here
	   with a proper wl_display. */
	wl_display_destroy(server.wl_display);
	wlr_output_layout_destroy(server.output_layout);
	return ret;
}
