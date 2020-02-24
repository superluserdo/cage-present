#ifndef CG_SERVER_H
#define CG_SERVER_H

#include "config.h"

#include <wayland-server-core.h>
#include <wlr/types/wlr_idle.h>
#include <wlr/types/wlr_idle_inhibit_v1.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#if CAGE_HAS_XWAYLAND
#include <wlr/xwayland.h>
#endif

#include "output.h"
#include "seat.h"
#include "view.h"

struct cg_server {

	pid_t active_pid;

	struct wl_display *wl_display;
	struct wl_list views;

	struct cg_seat *seat;
	struct wlr_idle *idle;
	struct wlr_idle_inhibit_manager_v1 *idle_inhibit_v1;
	struct wl_listener new_idle_inhibitor_v1;
	struct wl_list inhibitors;

	struct wlr_output_layout *output_layout;
	struct wl_list outputs;
	struct wl_listener new_output;

	struct wl_listener xdg_toplevel_decoration;
	struct wl_listener new_xdg_shell_surface;
#if CAGE_HAS_XWAYLAND
	struct wl_listener new_xwayland_surface;
#endif

	bool xdg_decoration;
	enum wl_output_transform output_transform;
#ifdef DEBUG
	bool debug_damage_tracking;
#endif
};

#endif
