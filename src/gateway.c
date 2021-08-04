/*
    Copyright (C) 2020 Sam H Smith
    Contact: sam.henning.smith@protonmail.com

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

#define _POSIX_C_SOURCE 200112L
#include <getopt.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <limits.h>
#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/backend/session.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_matrix.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_xdg_output_v1.h>
#include <wlr/xwayland.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_screencopy_v1.h>
#include <wlr/types/wlr_relative_pointer_v1.h>
#include <wlr/types/wlr_pointer_constraints_v1.h>
#include <wlr/util/log.h>
#include <assert.h>
#include <xkbcommon/xkbcommon.h>

/* For brevity's sake, struct members are annotated where they are used. */
enum tinywl_cursor_mode {
	TINYWL_CURSOR_PASSTHROUGH,
	TINYWL_CURSOR_MOVE,
	TINYWL_CURSOR_RESIZE,
};

struct gateway_config {
    char* kbd_layout;
    char* kbd_variant;
    char* terminal;
    char* launcher;
    double mouse_sens;
    uint32_t window_gaps;
};

struct tinywl_server {
    struct gateway_config* config;
	struct wl_display *wl_display;
	struct wlr_backend *backend;
	struct wlr_renderer *renderer;
    struct wlr_compositor* compositor;

	struct wlr_xdg_shell *xdg_shell;
	struct wl_listener new_xdg_surface;

    struct wlr_xdg_decoration_manager_v1* decoration_manager;
    struct wl_listener new_toplevel_decoration;

    struct wlr_xwayland* xwayland;
    struct wl_listener new_xwayland_surface;

    struct wlr_layer_shell_v1* layer_shell;
    struct wl_listener new_layer_surface;
    struct wl_list layer_surfaces;

	struct wlr_cursor *cursor;
	struct wlr_xcursor_manager *cursor_mgr;
	struct wl_listener cursor_motion;
	struct wl_listener cursor_motion_absolute;
	struct wl_listener cursor_button;
	struct wl_listener cursor_axis;
	struct wl_listener cursor_frame;

	struct wlr_seat *seat;
	struct wl_listener new_input;
	struct wl_listener request_cursor;
	struct wl_listener request_set_selection;
	struct wl_list keyboards;
	enum tinywl_cursor_mode cursor_mode;
	struct tinywl_view *grabbed_view;
	double grab_x, grab_y;
	struct wlr_box grab_geobox;
	uint32_t resize_edges;

    struct wlr_xdg_output_manager_v1* xdg_output_manager;
	struct wlr_output_layout *output_layout;
	struct wl_list outputs;
    struct gateway_panel* focused_panel;
	struct wl_listener new_output;

    struct wlr_screencopy_manager_v1* screencopy;
    struct wlr_relative_pointer_manager_v1* relative_pointer;
    struct wlr_pointer_constraints_v1* pointer_constraints;

    float brightness;
    bool passthrough_enabled;
};

struct gateway_panel_stack {
    int32_t width, height, current_y, current_x, max_items, item_count;
    bool mapped;
};

struct gateway_panel {
    struct wl_list unmapped_views;
    struct wl_list views;
    struct wl_list redirect_views;
    struct tinywl_view* focused_view;

    struct gateway_panel_stack* stacks;
    int32_t stack_count;

    struct tinywl_output* main_output;
    struct wl_list outputs;
};

struct tinywl_output {
    struct wl_list link;
    struct wl_list plink; //panel list
    struct tinywl_server *server;
    struct wlr_output *wlr_output;
    struct wl_listener frame;
    struct gateway_panel* panel;
    int32_t* stacks;
    int32_t stack_count;
};

struct tinywl_view {
	struct wl_list link;
	struct tinywl_server *server;
	struct wlr_xdg_surface *xdg_surface;
    struct wlr_xwayland_surface* xwayland_surface;
	struct wl_listener map;
	struct wl_listener unmap;
	struct wl_listener destroy;
	struct wl_listener request_move;
	struct wl_listener request_resize;
    struct wl_listener request_fullscreen;
	int x, y;
    int width, height;
    bool is_fullscreen;
    struct gateway_panel* focused_by;
    int32_t stack_index;
};

struct gateway_layer_surface {
    struct wl_list link;
    struct tinywl_server* server;
    struct wlr_layer_surface_v1* surface;
    bool mapped;

    struct wl_listener map;
    struct wl_listener unmap;
    struct wl_listener destroy;
};

struct tinywl_keyboard {
	struct wl_list link;
	struct tinywl_server *server;
	struct wlr_input_device *device;

	struct wl_listener modifiers;
	struct wl_listener key;
};

static void panel_update(struct gateway_panel* panel, struct tinywl_output* output);

static void focus_view(struct tinywl_view *view, struct gateway_panel* panel, bool mouse_focus) {
	/* Note: this function only deals with keyboard focus. */
	if (view == NULL) {
		return;
	}
	struct tinywl_server *server = view->server;
	struct wlr_seat *seat = server->seat;
	struct wlr_surface *prev_surface = seat->keyboard_state.focused_surface;
    struct wlr_surface* surface;
    if(view->xwayland_surface != NULL)
    {
        surface = view->xwayland_surface->surface;
    } else if(view->xdg_surface != NULL)
    {
        surface = view->xdg_surface->surface;
    }
    if(!mouse_focus)
    {
        wlr_cursor_warp(view->server->cursor, NULL, view->x + (view->width / 2),
            view->y + (view->height / 2));
    }
	if (prev_surface == surface) {
		/* Don't re-focus an already focused surface. */// because X we can't do this. :(
		return; // Oh no it's a lot worse. We have to have this because X
	}
    if(prev_surface) {
		if(wlr_surface_is_xdg_surface(prev_surface))
        {
            struct wlr_xdg_surface *previous = wlr_xdg_surface_from_wlr_surface(
                    prev_surface);
            wlr_xdg_toplevel_set_activated(previous, false);
        } else if(wlr_surface_is_xwayland_surface(prev_surface))
        {
            struct wlr_xwayland_surface *previous = 
                wlr_xwayland_surface_from_wlr_surface(
                    prev_surface
                );
            wlr_xwayland_surface_activate(previous, false);
        }
	}
    struct tinywl_view* other_view;
    wl_list_for_each(other_view, &server->focused_panel->views, link) {
        if(other_view->xwayland_surface != NULL) {
            wlr_xwayland_surface_activate(other_view->xwayland_surface, false);
        }
    }
	struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(seat);
    if(panel->focused_view != NULL) {
        panel->focused_view->focused_by = NULL;
    }
    panel->focused_view = view;
    view->focused_by = panel;
	/* Activate the new surface */
	if(view->xdg_surface != NULL)
    {
        wlr_xdg_toplevel_set_activated(view->xdg_surface, true);
        wlr_seat_keyboard_notify_enter(seat, surface,
            keyboard->keycodes, keyboard->num_keycodes, &keyboard->modifiers);
    } else if(view->xwayland_surface != NULL) {
        wlr_xwayland_surface_activate(view->xwayland_surface, true);
        wlr_seat_keyboard_notify_enter(seat, surface,
            keyboard->keycodes, keyboard->num_keycodes, &keyboard->modifiers);
    }
}

static void center_mouse(struct tinywl_server* server)
{
    struct tinywl_output* output;
    wl_list_for_each(output, &server->focused_panel->outputs, plink) {
        panel_update(server->focused_panel, output);
    }
    focus_view(server->focused_panel->focused_view, server->focused_panel, false);
    wlr_cursor_set_surface(server->cursor, NULL, 0, 0);
}

static void move_to_front(struct tinywl_view *view)
{
    struct tinywl_server *server = view->server;
    /* Move the view to the front */
    wl_list_remove(&view->link);
    wl_list_insert(&server->focused_panel->views, &view->link);
}

static void keyboard_handle_modifiers(
		struct wl_listener *listener, void *data) {
	/* This event is raised when a modifier key, such as shift or alt, is
	 * pressed. We simply communicate this to the client. */
	struct tinywl_keyboard *keyboard =
		wl_container_of(listener, keyboard, modifiers);
	/*
	 * A seat can only have one keyboard, but this is a limitation of the
	 * Wayland protocol - not wlroots. We assign all connected keyboards to the
	 * same seat. You can swap out the underlying wlr_keyboard like this and
	 * wlr_seat handles this transparently.
	 */
	wlr_seat_set_keyboard(keyboard->server->seat, keyboard->device);
	/* Send modifiers to the client. */
	wlr_seat_keyboard_notify_modifiers(keyboard->server->seat,
		&keyboard->device->keyboard->modifiers);
}
static void list_swap(struct wl_list* a, struct wl_list* b) // swaps order in views a-b -> b-a
{
    struct wl_list* linknext = b->next;
    
    struct wl_list* linkprev = a->prev;

    linkprev->next = b;
    b->prev = linkprev;

    b->next = a;
    a->prev = b;

    a->next = linknext;
    linknext->prev = a;
}
static bool handle_keybinding(struct tinywl_server *server, uint32_t keycode, uint32_t modifiers) {
    if(server->passthrough_enabled && keycode != 88) {
        return false;
    }
    if(keycode == 88) {
        server->passthrough_enabled = !server->passthrough_enabled;
        return true;
    }

    if(keycode == 1)
    {
        wl_display_terminate(server->wl_display);
        assert(0);
    }
    else if(keycode == 36 && (modifiers & WLR_MODIFIER_SHIFT) == 0)
    {
        if (wl_list_length(&server->focused_panel->views) < 2) {
            return false;
        }
        struct tinywl_view* current_view = server->focused_panel->focused_view;
        struct wl_list* linknext = current_view->link.prev;
        if(linknext == &server->focused_panel->views) { linknext = linknext->prev; }
        struct tinywl_view* next_view = wl_container_of(
            linknext, next_view, link);

        focus_view(next_view, server->focused_panel, false);
        center_mouse(server);
    }
    else if(keycode == 37 && (modifiers & WLR_MODIFIER_SHIFT) == 0)
    {
        if (wl_list_length(&server->focused_panel->views) < 2) {
            return false;
        }
        struct tinywl_view* current_view = server->focused_panel->focused_view;
        struct wl_list* linknext = current_view->link.next;
        if(linknext == &server->focused_panel->views) { linknext = linknext->next; }
        struct tinywl_view* next_view = wl_container_of(
            linknext, next_view, link);

        focus_view(next_view, server->focused_panel, false);
        center_mouse(server);
    }
    else if(keycode == 38)
    {
        if (wl_list_length(&server->focused_panel->views) < 2) {
            return false;
        }

        struct tinywl_view* next_view = wl_container_of(
            server->focused_panel->views.prev, next_view, link);
 
        focus_view(next_view, server->focused_panel, false);
        center_mouse(server);
    }
    else if(keycode == 36 && (modifiers & WLR_MODIFIER_SHIFT) != 0)
    {
        if (wl_list_length(&server->focused_panel->views) < 2) {
            return false;
        }
        struct tinywl_view* current_view = server->focused_panel->focused_view;
        list_swap(current_view->link.prev, &current_view->link);
        center_mouse(server);
    }
    else if(keycode == 37 && (modifiers & WLR_MODIFIER_SHIFT) != 0)
    {
        if (wl_list_length(&server->focused_panel->views) < 2) {
            return false;
        }
        struct tinywl_view* current_view = server->focused_panel->focused_view;
        list_swap(&current_view->link, current_view->link.next);
        center_mouse(server);
    }
    else if(keycode == 49)
    {
        if(server->focused_panel->focused_view != NULL)
        {
            move_to_front(server->focused_panel->focused_view);
            center_mouse(server);
        }
    }
    else if(keycode == 21)
    {
        server->focused_panel->focused_view->is_fullscreen = 
                !server->focused_panel->focused_view->is_fullscreen;
    }
    else if(keycode == 28)
    {
        char cmd[512];
        strcpy(cmd, server->config->terminal);
        strcat(cmd, " &");
        system(cmd);
    }
    else if(keycode == 35)
    {
        char cmd[512];
        strcpy(cmd, server->config->launcher);
        strcat(cmd, " &");
        system(cmd);
    }
    else if(keycode == 53 && (modifiers && WLR_MODIFIER_SHIFT) == 1)
    {
        if(server->focused_panel->focused_view == NULL) { return false; }

        struct tinywl_view* current_view = server->focused_panel->focused_view;
        struct wl_list* linknext = current_view->link.next;
        if(linknext == &server->focused_panel->views) { linknext = linknext->next; }
        struct tinywl_view* next_view = wl_container_of(
            linknext, next_view, link);
        if(current_view->xdg_surface != NULL){ wlr_xdg_toplevel_send_close(current_view->xdg_surface); }
        if(current_view->xwayland_surface != NULL)
        { wlr_xwayland_surface_close(current_view->xwayland_surface); }

        if(next_view == current_view) { server->focused_panel->focused_view = NULL; }
        else { server->focused_panel->focused_view = next_view; }
        center_mouse(server);
    }
    else
    {
        return false;
    }
    return true;
}

static void keyboard_handle_key(
		struct wl_listener *listener, void *data) {
	/* This event is raised when a key is pressed or released. */
	struct tinywl_keyboard *keyboard =
		wl_container_of(listener, keyboard, key);
	struct tinywl_server *server = keyboard->server;
	struct wlr_event_keyboard_key *event = data;
	struct wlr_seat *seat = server->seat;

	/* Translate libinput keycode -> xkbcommon */
	uint32_t keycode = event->keycode + 8;
	/* Get a list of keysyms based on the keymap for this keyboard */
	const xkb_keysym_t *syms;
	int nsyms = xkb_state_key_get_syms(
			keyboard->device->keyboard->xkb_state, keycode, &syms);

	bool handled = false;
	uint32_t modifiers = wlr_keyboard_get_modifiers(keyboard->device->keyboard);

	if ((modifiers & WLR_MODIFIER_LOGO) && event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
        handled = handle_keybinding(server, event->keycode, modifiers);
	}
    struct wlr_session* session = wlr_backend_get_session(server->backend); //Virtual terminals
    if(session != NULL && (modifiers & WLR_MODIFIER_CTRL) && (modifiers & WLR_MODIFIER_ALT)){
        for(int i = 0; i < nsyms; i++) {
            for(int x = 1; x <= 12; x++){
                if(syms[i] == 269024768 + x) {
                    wlr_session_change_vt(session, x);
                }
            }
        }
    }

    if(event->state != WL_KEYBOARD_KEY_STATE_RELEASED) {
    for(int i = 0; i < nsyms; i++) {
        if(syms[i] == XKB_KEY_XF86MonBrightnessUp) {
            server->brightness += 0.05;
        }
        if(syms[i] == XKB_KEY_XF86MonBrightnessDown) {
            server->brightness -= 0.05;
        }
        if(syms[i] == XKB_KEY_XF86AudioRaiseVolume) {
            system("pamixer -i 10");
        }
        if(syms[i] == XKB_KEY_XF86AudioLowerVolume) {
            system("pamixer -d 10");
        }
        if(syms[i] == XKB_KEY_XF86AudioMute) {
            system("pamixer -t");
        }
    }}
    if(server->brightness > 1.0) { server->brightness = 1.0; }
else if(server->brightness< 0.0) { server->brightness = 0.0; }

	if (!handled) {
        struct wlr_keyboard *wkeyboard = wlr_seat_get_keyboard(seat);
        struct gateway_layer_surface* ls;
        wl_list_for_each(ls, &server->layer_surfaces, link) {
            if(!ls->mapped || !ls->surface->current.keyboard_interactive) { continue; }
            handled = true;

            wlr_seat_keyboard_notify_enter(seat, ls->surface->surface,
                wkeyboard->keycodes, wkeyboard->num_keycodes, &wkeyboard->modifiers);
            break;
        }
        if(!handled && server->focused_panel->focused_view != NULL)
        {
        if(server->focused_panel->focused_view->xdg_surface != NULL) {
            wlr_seat_keyboard_notify_enter(seat, server->focused_panel->focused_view->xdg_surface->surface,
                wkeyboard->keycodes, wkeyboard->num_keycodes, &wkeyboard->modifiers);
        }else if(server->focused_panel->focused_view->xwayland_surface != NULL) {
            wlr_seat_keyboard_notify_enter(seat, server->focused_panel->focused_view->xwayland_surface->surface,
                wkeyboard->keycodes, wkeyboard->num_keycodes, &wkeyboard->modifiers);
        }
        }
		/* Otherwise, we pass it along to the client. */
		wlr_seat_set_keyboard(seat, keyboard->device);
		wlr_seat_keyboard_notify_key(seat, event->time_msec,
			event->keycode, event->state);
	}
}

static void server_new_keyboard(struct tinywl_server *server,
		struct wlr_input_device *device) {
	struct tinywl_keyboard *keyboard =
		calloc(1, sizeof(struct tinywl_keyboard));
	keyboard->server = server;
	keyboard->device = device;

	/* We need to prepare an XKB keymap and assign it to the keyboard. This
	 * assumes the defaults (e.g. layout = "us"). */
	struct xkb_rule_names rules = { 0 };
    rules.layout = server->config->kbd_layout;
    rules.variant = server->config->kbd_variant;

	struct xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	struct xkb_keymap *keymap = xkb_map_new_from_names(context, &rules,
		XKB_KEYMAP_COMPILE_NO_FLAGS);

    assert(keymap != NULL);

	wlr_keyboard_set_keymap(device->keyboard, keymap);
	xkb_keymap_unref(keymap);
	xkb_context_unref(context);
	wlr_keyboard_set_repeat_info(device->keyboard, 25, 600);

	/* Here we set up listeners for keyboard events. */
	keyboard->modifiers.notify = keyboard_handle_modifiers;
	wl_signal_add(&device->keyboard->events.modifiers, &keyboard->modifiers);
	keyboard->key.notify = keyboard_handle_key;
	wl_signal_add(&device->keyboard->events.key, &keyboard->key);

	wlr_seat_set_keyboard(server->seat, device);

	/* And add the keyboard to our list of keyboards */
	wl_list_insert(&server->keyboards, &keyboard->link);
}

static void server_new_pointer(struct tinywl_server *server,
		struct wlr_input_device *device) {
	/* We don't do anything special with pointers. All of our pointer handling
	 * is proxied through wlr_cursor. On another compositor, you might take this
	 * opportunity to do libinput configuration on the device to set
	 * acceleration, etc. */
	wlr_cursor_attach_input_device(server->cursor, device);
}

static void server_new_input(struct wl_listener *listener, void *data) {
	/* This event is raised by the backend when a new input device becomes
	 * available. */
	struct tinywl_server *server =
		wl_container_of(listener, server, new_input);
	struct wlr_input_device *device = data;
	switch (device->type) {
	case WLR_INPUT_DEVICE_KEYBOARD:
		server_new_keyboard(server, device);
		break;
	case WLR_INPUT_DEVICE_POINTER:
		server_new_pointer(server, device);
		break;
	default:
		break;
	}
	/* We need to let the wlr_seat know what our capabilities are, which is
	 * communiciated to the client. In TinyWL we always have a cursor, even if
	 * there are no pointer devices, so we always include that capability. */
	uint32_t caps = WL_SEAT_CAPABILITY_POINTER;
	if (!wl_list_empty(&server->keyboards)) {
		caps |= WL_SEAT_CAPABILITY_KEYBOARD;
	}
	wlr_seat_set_capabilities(server->seat, caps);
}

static void seat_request_cursor(struct wl_listener *listener, void *data) {
	struct tinywl_server *server = wl_container_of(
			listener, server, request_cursor);
	/* This event is rasied by the seat when a client provides a cursor image */
	struct wlr_seat_pointer_request_set_cursor_event *event = data;
	struct wlr_seat_client *focused_client =
		server->seat->pointer_state.focused_client;
	/* This can be sent by any client, so we check to make sure this one is
	 * actually has pointer focus first. */
	if (focused_client == event->seat_client) {
		/* Once we've vetted the client, we can tell the cursor to use the
		 * provided surface as the cursor image. It will set the hardware cursor
		 * on the output that it's currently on and continue to do so as the
		 * cursor moves between outputs. */
		wlr_cursor_set_surface(server->cursor, event->surface,
				event->hotspot_x, event->hotspot_y);
	}
}

static void seat_request_set_selection(struct wl_listener *listener, void *data) {
	/* This event is raised by the seat when a client wants to set the selection,
	 * usually when the user copies something. wlroots allows compositors to
	 * ignore such requests if they so choose, but in tinywl we always honor
	 */
	struct tinywl_server *server = wl_container_of(
			listener, server, request_set_selection);
	struct wlr_seat_request_set_selection_event *event = data;
	wlr_seat_set_selection(server->seat, event->source, event->serial);
}

static bool view_at(struct tinywl_view *view,
		double lx, double ly, struct wlr_surface **surface,
		double *sx, double *sy) {
	/*
	 * XDG toplevels may have nested surfaces, such as popup windows for context
	 * menus or tooltips. This function tests if any of those are underneath the
	 * coordinates lx and ly (in output Layout Coordinates). If so, it sets the
	 * surface pointer to that wlr_surface and the sx and sy coordinates to the
	 * coordinates relative to that surface's top-left corner.
	 */
	double view_sx = lx - view->x;
	double view_sy = ly - view->y;

    if(view->focused_by == NULL && view->is_fullscreen)
    {
        uint32_t gap = view->server->config->window_gaps; // lazy
        if(view_sx < gap || view_sy < gap || view_sx > view->width - gap || view_sy > view->height - gap)
        { return false; }
    }

	double _sx, _sy;
	struct wlr_surface *_surface = NULL;
    if(view->xdg_surface != NULL)
    {
        double _scale_x = 1.0, _scale_y=1.0;
        if(view->width != 0 && view->xdg_surface->toplevel->server_pending.width != 0)
        {
            _scale_x= ((double)view->xdg_surface->toplevel->server_pending.width) / ((double)view->width);
        }
        if(view->height != 0 && view->xdg_surface->toplevel->server_pending.height != 0)
        {
            _scale_y= ((double)view->xdg_surface->toplevel->server_pending.height) / ((double)view->height);
        }

        _surface = wlr_xdg_surface_surface_at(
                view->xdg_surface, view_sx * _scale_x, view_sy * _scale_y, &_sx, &_sy);
    } else if(view->xwayland_surface != NULL)
    {
        if(view_sx >= 0 && view_sx < view->width
            && view_sy >= 0 && view_sy < view->height)
        {
            double scale_x = 1.0; double scale_y = 1.0;
            _surface = view->xwayland_surface->surface;
            if(view->width != 0 && view->xwayland_surface->width != 0)
            {
                scale_x= ((double)view->xwayland_surface->width) / ((double)view->width);
            }
            if(view->height != 0 && view->xwayland_surface->height != 0)
            {
                scale_y= ((double)view->xwayland_surface->height) / ((double)view->height);
            }
            _sx = view_sx * scale_x;
            _sy = view_sy * scale_y;
        }
    }

	if (_surface != NULL) {
		*sx = _sx;
		*sy = _sy;
		*surface = _surface;
		return true;
	}

	return false;
}

static struct tinywl_view *desktop_view_at(
		struct tinywl_server *server, double lx, double ly,
		struct wlr_surface **surface, double *sx, double *sy)
{
	/* This iterates over all of our surfaces and attempts to find one under the
	 * cursor. This relies on server->views being ordered from top-to-bottom. */
	struct tinywl_view *view;
    wl_list_for_each(view, &server->focused_panel->views, link) {
        if(view->focused_by == NULL) { continue; }
        if (view_at(view, lx, ly, surface, sx, sy)) {
            return view;
        }
    }
    wl_list_for_each(view, &server->focused_panel->views, link) {
        if(!view->is_fullscreen || view->focused_by != NULL) { continue; }
        if (view_at(view, lx, ly, surface, sx, sy)) {
            return view;
        }
    }
    wl_list_for_each(view, &server->focused_panel->views, link) {
        if(view->is_fullscreen || view->focused_by != NULL) { continue; }
        if (view_at(view, lx, ly, surface, sx, sy)) {
            return view;
        }
    }
	return NULL;
}

static void process_cursor_move(struct tinywl_server *server, uint32_t time) {
	/* Move the grabbed view to the new position. */
	server->grabbed_view->x = server->cursor->x - server->grab_x;
	server->grabbed_view->y = server->cursor->y - server->grab_y;
}

static void process_cursor_resize(struct tinywl_server *server, uint32_t time) {
	/*
	 * Resizing the grabbed view can be a little bit complicated, because we
	 * could be resizing from any corner or edge. This not only resizes the view
	 * on one or two axes, but can also move the view if you resize from the top
	 * or left edges (or top-left corner).
	 *
	 * Note that I took some shortcuts here. In a more fleshed-out compositor,
	 * you'd wait for the client to prepare a buffer at the new size, then
	 * commit any movement that was prepared.
	 */
	struct tinywl_view *view = server->grabbed_view;
	double border_x = server->cursor->x - server->grab_x;
	double border_y = server->cursor->y - server->grab_y;
	int new_left = server->grab_geobox.x;
	int new_right = server->grab_geobox.x + server->grab_geobox.width;
	int new_top = server->grab_geobox.y;
	int new_bottom = server->grab_geobox.y + server->grab_geobox.height; 

	if (server->resize_edges & WLR_EDGE_TOP) {
		new_top = border_y;
		if (new_top >= new_bottom) {
			new_top = new_bottom - 1;
		}
	} else if (server->resize_edges & WLR_EDGE_BOTTOM) {
		new_bottom = border_y;
		if (new_bottom <= new_top) {
			new_bottom = new_top + 1;
		}
	}
	if (server->resize_edges & WLR_EDGE_LEFT) {
		new_left = border_x;
		if (new_left >= new_right) {
			new_left = new_right - 1;
		}
	} else if (server->resize_edges & WLR_EDGE_RIGHT) {
		new_right = border_x;
		if (new_right <= new_left) {
			new_right = new_left + 1;
		}
	}

	struct wlr_box geo_box;
	wlr_xdg_surface_get_geometry(view->xdg_surface, &geo_box);
	view->x = new_left - geo_box.x;
	view->y = new_top - geo_box.y;

	int new_width = new_right - new_left;
	int new_height = new_bottom - new_top;
	wlr_xdg_toplevel_set_size(view->xdg_surface, new_width, new_height);
}

static void process_cursor_motion(struct tinywl_server *server, uint32_t time) {
	/* If the mode is non-passthrough, delegate to those functions. */
//	if (server->cursor_mode == TINYWL_CURSOR_MOVE) {
//		process_cursor_move(server, time);
//		return;
//	} else if (server->cursor_mode == TINYWL_CURSOR_RESIZE) {
//		process_cursor_resize(server, time);
//		return;
//	}

	/* Otherwise, find the view under the pointer and send the event along. */
	double sx, sy;
	struct wlr_seat *seat = server->seat;
	struct wlr_surface *surface = NULL;
	struct tinywl_view *view = desktop_view_at(server,
			server->cursor->x, server->cursor->y, &surface, &sx, &sy);
	if (!view) {
		/* If there's no view under the cursor, set the cursor image to a
		 * default. This is what makes the cursor image appear when you move it
		 * around the screen, not over any views. */
		wlr_xcursor_manager_set_cursor_image(
				server->cursor_mgr, "left_ptr", server->cursor);
	} else
    {
        focus_view(view, view->server->focused_panel, true);
    }
	if (surface) {
		bool focus_changed = seat->pointer_state.focused_surface != surface;
		/*
		 * "Enter" the surface if necessary. This lets the client know that the
		 * cursor has entered one of its surfaces.
		 *
		 * Note that this gives the surface "pointer focus", which is distinct
		 * from keyboard focus. You get pointer focus by moving the pointer over
		 * a window.
		 */
		wlr_seat_pointer_notify_enter(seat, surface, sx, sy);
		if (!focus_changed) {
			/* The enter event contains coordinates, so we only need to notify
			 * on motion if the focus did not change. */
			wlr_seat_pointer_notify_motion(seat, time, sx, sy);
		}
	} else {
		/* Clear pointer focus so future button events and such are not sent to
		 * the last client to have the cursor over it. */
		wlr_seat_pointer_clear_focus(seat);
	}
}

static void server_cursor_motion(struct wl_listener *listener, void *data) {
    /* This event is forwarded by the cursor when a pointer emits a _relative_
     * pointer motion event (i.e. a delta) */
    struct tinywl_server *server =
        wl_container_of(listener, server, cursor_motion);
    struct wlr_event_pointer_motion *event = data;
    /* The cursor doesn't move unless we tell it to. The cursor automatically
     * handles constraining the motion to the output layout, as well as any
     * special configuration applied for the specific input device which
     * generated the event. You can pass NULL for the device if you want to move
     * the cursor around without any input. */
    wlr_relative_pointer_manager_v1_send_relative_motion(
        server->relative_pointer,
        server->seat,
        ((uint64_t)event->time_msec) * 1000,
        event->delta_x * server->config->mouse_sens, event->delta_y * server->config->mouse_sens,
        event->unaccel_dx, event->unaccel_dy);
    wlr_cursor_move(server->cursor, event->device,
            event->delta_x * server->config->mouse_sens, event->delta_y * server->config->mouse_sens);
    struct wlr_surface* surface;
    if(server->focused_panel->focused_view != NULL) {
    if(server->focused_panel->focused_view->xwayland_surface != NULL)
    {
        surface = server->focused_panel->focused_view->xwayland_surface->surface;
    } else
    {
        surface = server->focused_panel->focused_view->xdg_surface->surface;
    }
    struct wlr_pointer_constraint_v1* constraint =
        wlr_pointer_constraints_v1_constraint_for_surface(
            server->pointer_constraints,
            surface,
            server->seat
        );
    if(constraint != NULL) {
    if(constraint->type == WLR_POINTER_CONSTRAINT_V1_LOCKED) {
        server->cursor->x = (double)server->focused_panel->focused_view->x + ((double)(server->focused_panel->focused_view->width) / 2.0);
        server->cursor->y = (double)server->focused_panel->focused_view->y + ((double)(server->focused_panel->focused_view->height) / 2.0);
    } else {
        if(server->cursor->x < server->focused_panel->focused_view->x) { server->cursor->x = server->focused_panel->focused_view->x; }
        else if(server->cursor->x > server->focused_panel->focused_view->x + server->focused_panel->focused_view->width)
        { server->cursor->x = server->focused_panel->focused_view->x + server->focused_panel->focused_view->width; }

        if(server->cursor->y < server->focused_panel->focused_view->y) { server->cursor->y = server->focused_panel->focused_view->y; }
        else if(server->cursor->y > server->focused_panel->focused_view->y + server->focused_panel->focused_view->height)
        { server->cursor->y = server->focused_panel->focused_view->y + server->focused_panel->focused_view->height; }
    }
    }
    }
    process_cursor_motion(server, event->time_msec);
}

static void server_cursor_motion_absolute(
		struct wl_listener *listener, void *data) {
	/* This event is forwarded by the cursor when a pointer emits an _absolute_
	 * motion event, from 0..1 on each axis. This happens, for example, when
	 * wlroots is running under a Wayland window rather than KMS+DRM, and you
	 * move the mouse over the window. You could enter the window from any edge,
	 * so we have to warp the mouse there. There is also some hardware which
	 * emits these events. */
	struct tinywl_server *server =
		wl_container_of(listener, server, cursor_motion_absolute);
	struct wlr_event_pointer_motion_absolute *event = data;
	wlr_cursor_warp_absolute(server->cursor, event->device, event->x, event->y);
	process_cursor_motion(server, event->time_msec);
}

static void server_cursor_button(struct wl_listener *listener, void *data) {
	/* This event is forwarded by the cursor when a pointer emits a button
	 * event. */
	struct tinywl_server *server =
		wl_container_of(listener, server, cursor_button);
	struct wlr_event_pointer_button *event = data;
	/* Notify the client with pointer focus that a button press has occurred */
	wlr_seat_pointer_notify_button(server->seat,
			event->time_msec, event->button, event->state);
	double sx, sy;
	struct wlr_surface *surface;
	struct tinywl_view *view = desktop_view_at(server,
			server->cursor->x, server->cursor->y, &surface, &sx, &sy);
	if (event->state == WLR_BUTTON_RELEASED) {
		/* If you released any buttons, we exit interactive move/resize mode. */
		server->cursor_mode = TINYWL_CURSOR_PASSTHROUGH;
	}
}

static void server_cursor_axis(struct wl_listener *listener, void *data) {
	/* This event is forwarded by the cursor when a pointer emits an axis event,
	 * for example when you move the scroll wheel. */
	struct tinywl_server *server =
		wl_container_of(listener, server, cursor_axis);
	struct wlr_event_pointer_axis *event = data;
	/* Notify the client with pointer focus of the axis event. */
	wlr_seat_pointer_notify_axis(server->seat,
			event->time_msec, event->orientation, event->delta,
			event->delta_discrete, event->source);
}

static void server_cursor_frame(struct wl_listener *listener, void *data) {
	/* This event is forwarded by the cursor when a pointer emits an frame
	 * event. Frame events are sent after regular pointer events to group
	 * multiple events together. For instance, two axis events may happen at the
	 * same time, in which case a frame event won't be sent in between. */
	struct tinywl_server *server =
		wl_container_of(listener, server, cursor_frame);
	/* Notify the client with pointer focus of the frame event. */
	wlr_seat_pointer_notify_frame(server->seat);
}

/* Used to move all of the data necessary to render a surface from the top-level
 * frame handler to the per-surface render function. */
struct render_data {
	struct wlr_output *output;
	struct wlr_renderer *renderer;
    struct tinywl_view *view;
    struct gateway_layer_surface* ls;
	struct timespec *when;
};

static void render_surface(struct wlr_surface *surface,
		int sx, int sy, void *data) {
	/* This function is called for every surface that needs to be rendered. */
	struct render_data *rdata = data;
	struct tinywl_view *view = rdata->view;
	struct wlr_output *output = rdata->output;

	/* We first obtain a wlr_texture, which is a GPU resource. wlroots
	 * automatically handles negotiating these with the client. The underlying
	 * resource could be an opaque handle passed from the client, or the client
	 * could have sent a pixel buffer which we copied to the GPU, or a few other
	 * means. You don't have to worry about this, wlroots takes care of it. */
	struct wlr_texture *texture = wlr_surface_get_texture(surface);
	if (texture == NULL) {
		return;
	}

	/* The view has a position in layout coordinates. If you have two displays,
	 * one next to the other, both 1080p, a view on the rightmost display might
	 * have layout coordinates of 2000,100. We need to translate that to
	 * output-local coordinates, or (2000 - 1920). */
	double ox = 0, oy = 0;
	wlr_output_layout_output_coords(
			view->server->output_layout, output, &ox, &oy);
	ox += view->x + sx, oy += view->y + sy;

	/* We also have to apply the scale factor for HiDPI outputs. This is only
	 * part of the puzzle, TinyWL does not fully support HiDPI. */
    struct wlr_box box = {
        .x = ox * output->scale,
        .y = oy * output->scale,
        .width = surface->current.width * output->scale,
        .height = surface->current.height * output->scale,
	};

    if((view->xdg_surface != NULL && view->xdg_surface->surface == surface) || view->xwayland_surface != NULL)
    {
        box.width = view->width * output->scale;
        box.height = view->height * output->scale;
    }

	/*
	 * Those familiar with OpenGL are also familiar with the role of matricies
	 * in graphics programming. We need to prepare a matrix to render the view
	 * with. wlr_matrix_project_box is a helper which takes a box with a desired
	 * x, y coordinates, width and height, and an output geometry, then
	 * prepares an orthographic projection and multiplies the necessary
	 * transforms to produce a model-view-projection matrix.
	 *
	 * Naturally you can do this any way you like, for example to make a 3D
	 * compositor.
	 */
	float matrix[9];
	enum wl_output_transform transform =
		wlr_output_transform_invert(surface->current.transform);
	wlr_matrix_project_box(matrix, &box, transform, 0,
		output->transform_matrix);

	/* This takes our matrix, the texture, and an alpha, and performs the actual
	 * rendering on the GPU. */
	wlr_render_texture_with_matrix(rdata->renderer, texture, matrix, 1);

	/* This lets the client know that we've displayed that frame and it can
	 * prepare another one now if it likes. */
	wlr_surface_send_frame_done(surface, rdata->when);
}

static void render_layer_surface(struct wlr_surface *surface,
        int sx, int sy, void *data) {
    /* This function is called for every surface that needs to be rendered. */
    struct render_data *rdata = data;
    struct gateway_layer_surface* view = rdata->ls;
    struct wlr_output *output = rdata->output;
 
    /* We first obtain a wlr_texture, which is a GPU resource. wlroots
     * automatically handles negotiating these with the client. The underlying
     * resource could be an opaque handle passed from the client, or the client
     * could have sent a pixel buffer which we copied to the GPU, or a few other
     * means. You don't have to worry about this, wlroots takes care of it. */
    struct wlr_texture *texture = wlr_surface_get_texture(surface);
    if (texture == NULL) {
        return;
    }

    double ox = 0.0;
    double oy = 0.0;
    struct wlr_box box = {
        .x = ox * output->scale,
        .y = oy * output->scale,
        .width = surface->current.width * output->scale,
        .height = surface->current.height * output->scale,
    };
 
    float matrix[9];
    enum wl_output_transform transform =
        wlr_output_transform_invert(surface->current.transform);
    wlr_matrix_project_box(matrix, &box, transform, 0,
        output->transform_matrix);
 
    /* This takes our matrix, the texture, and an alpha, and performs the actual
     * rendering on the GPU. */
    wlr_render_texture_with_matrix(rdata->renderer, texture, matrix, 1);

    /* This lets the client know that we've displayed that frame and it can
     * prepare another one now if it likes. */
    wlr_surface_send_frame_done(surface, rdata->when);
}

static bool output_contains_stack(struct tinywl_output* output, int32_t s)
{
    for(int i = 0; i < output->stack_count; i++)
    {
        if(output->stacks[i] == s)
        { return true; }
    }
    return false;
}

static void panel_update(struct gateway_panel* panel, struct tinywl_output* output)
{
    struct tinywl_view *view;
    struct tinywl_view *_view_tmp;
    wl_list_for_each_safe(view, _view_tmp, &panel->views, link)
    {
        view->stack_index = -1;
        if(view->xwayland_surface != NULL)
        {
            if(view->xwayland_surface->override_redirect)
            {
                view->x=view->xwayland_surface->x;
                view->y=view->xwayland_surface->y;
                view->width=view->xwayland_surface->width;
                view->height=view->xwayland_surface->height;
                wl_list_remove(&view->link);    
                wl_list_insert(&panel->redirect_views, &view->link);
                continue;
            }
        }
    }
    struct wlr_output_layout_output* output_layout = wlr_output_layout_get(
        output->server->output_layout, output->wlr_output
    );
    int32_t x= output_layout->x;
    int32_t last_stack = 0;

    for(int i = 0; i < panel->stack_count; i++)
    {
        if(!panel->stacks[i].mapped) { continue; }
        last_stack = i;
        panel->stacks[i].item_count = 0;
        if(!output_contains_stack(output, i)) { continue; }
        panel->stacks[i].current_y = output_layout->y;
        panel->stacks[i].current_x = x;
        panel->stacks[i].height = output->wlr_output->height;
        panel->stacks[i].width = output->wlr_output->width / output->stack_count;
        x += panel->stacks[i].width;
    }

    wl_list_for_each(view, &panel->views, link)
    {
        view->stack_index = last_stack;
        panel->stacks[last_stack].item_count++;
    }

wl_list_for_each(view, &panel->views, link)
    {
bool is_done = !(wl_list_length(&panel->views) > 0);
while(!is_done) {
    int32_t sid = view->stack_index;
    for(int i = view->stack_index - 1; i >= 0; i--)
    {
        if(!panel->stacks[i].mapped) { continue; }
        if(panel->stacks[i].item_count < panel->stacks[i].max_items &&
    ((panel->stacks[i].item_count + 2 <= panel->stacks[sid].item_count) || panel->stacks[i].item_count < 1))
        {
            sid = i;
            break;
        }
    }
    if(sid == view->stack_index) { is_done = true; break; }

    panel->stacks[view->stack_index].item_count--;
    panel->stacks[sid].item_count++;
    view->stack_index = sid;
}
}
    uint32_t gaps = output->server->config->window_gaps;
    wl_list_for_each(view, &panel->views, link)
    {
        if(!output_contains_stack(output, view->stack_index)) { continue; }
        view->width = panel->stacks[view->stack_index].width - 2*gaps;
        view->height = panel->stacks[view->stack_index].height / panel->stacks[view->stack_index].item_count;
        view->x = panel->stacks[view->stack_index].current_x + gaps;
        view->y = panel->stacks[view->stack_index].current_y + gaps;
        panel->stacks[view->stack_index].current_y += view->height;
        view->height -= 2*gaps;

        if(view->is_fullscreen)
        {
            view->x = output_layout->x;
            view->y = output_layout->y;
            view->width = output->wlr_output->width;
            view->height = output->wlr_output->height;
        }

        if(view->xwayland_surface != NULL)
        {
            int32_t w = view->width, h = view->height;
            struct wlr_xwayland_surface_size_hints size_hints = {0};
            if(view->xwayland_surface->size_hints != NULL)
            { size_hints = *view->xwayland_surface->size_hints; }
            if(size_hints.min_width > w) { w = size_hints.min_width; }
            if(size_hints.min_height > h) { h = size_hints.min_height; }
            if(size_hints.max_width > 0 && size_hints.max_width < w) { w = size_hints.max_width; }
            if(size_hints.max_height > 0 && size_hints.max_height < h) { h = size_hints.max_height; }

            wlr_xwayland_surface_configure(view->xwayland_surface, 0, 0,
                w, h);
        } else if(view->xdg_surface != NULL)
        {
            int32_t w = view->width, h = view->height;
            if(view->xdg_surface->toplevel->current.min_width > w) { w = view->xdg_surface->toplevel->current.min_width; }
            if(view->xdg_surface->toplevel->current.min_height > h) { h = view->xdg_surface->toplevel->current.min_height; }
            if(view->xdg_surface->toplevel->current.max_width > 0 &&
        view->xdg_surface->toplevel->current.max_width < w) { w = view->xdg_surface->toplevel->current.max_width; }
            if(view->xdg_surface->toplevel->current.max_height > 0 &&
        view->xdg_surface->toplevel->current.max_height < h) { h = view->xdg_surface->toplevel->current.max_height; }
            wlr_xdg_toplevel_set_size(view->xdg_surface, w, h);
        }
    }
}
static void panel_post_update(struct gateway_panel* panel)
{
    struct tinywl_view *view;
    struct tinywl_view *_view_tmp;
    wl_list_for_each_safe(view, _view_tmp, &panel->redirect_views, link)
    {
        wl_list_remove(&view->link);
        wl_list_insert(&panel->views, &view->link);
    }
}
static void output_frame(struct wl_listener *listener, void *data) {
	/* This function is called every time an output is ready to display a frame,
	 * generally at the output's refresh rate (e.g. 60Hz). */
	struct tinywl_output *output =
		wl_container_of(listener, output, frame);
	struct wlr_renderer *renderer = output->server->renderer;

    panel_update(output->panel, output);

	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);

	/* wlr_output_attach_render makes the OpenGL context current. */
	if (!wlr_output_attach_render(output->wlr_output, NULL)) {
		return;
	}
	/* The "effective" resolution can change if you rotate your outputs. */
	int width, height;
	wlr_output_effective_resolution(output->wlr_output, &width, &height);

	/* Begin the renderer (calls glViewport and some other GL sanity checks) */
	wlr_renderer_begin(renderer, width, height);

	float color[4] = {0.3, 0.3, 0.3, 1.0};
	wlr_renderer_clear(renderer, color);

    //Background wlr-layer-shell
    struct gateway_layer_surface* ls;
    wl_list_for_each_reverse(ls, &output->server->layer_surfaces, link) {
        if(!ls->mapped || ls->surface->output != output->wlr_output ||
            ls->surface->current.layer != 0) { continue; }
        struct render_data rdata = {
            .output = output->wlr_output,
            .ls = ls,
            .renderer = renderer,
            .when = &now,
        };
        wlr_layer_surface_v1_for_each_surface(ls->surface,
            render_layer_surface, &rdata);
    }

    wl_list_for_each_reverse(ls, &output->server->layer_surfaces, link) {
        if(!ls->mapped || ls->surface->output != output->wlr_output ||
            ls->surface->current.layer != 1) { continue; }
        struct render_data rdata = {
            .output = output->wlr_output,
            .ls = ls,
            .renderer = renderer,
            .when = &now,
        };
        wlr_layer_surface_v1_for_each_surface(ls->surface,
            render_layer_surface, &rdata);
    }

	/* Each subsequent window we render is rendered on top of the last. Because
	 * our view list is ordered front-to-back, we iterate over it backwards. */
	struct tinywl_view *view;
	wl_list_for_each_reverse(view, &output->panel->views, link) {
        if(!output_contains_stack(output, view->stack_index) || view->is_fullscreen
            || view->focused_by == output->panel) { continue; }
		struct render_data rdata = {
			.output = output->wlr_output,
			.view = view,
			.renderer = renderer,
			.when = &now,
		};
        if(view->xdg_surface != NULL)
        {
    		/* This calls our render_surface function for each surface among the
    		 * xdg_surface's toplevel and popups. */
    		wlr_xdg_surface_for_each_surface(view->xdg_surface,
    				render_surface, &rdata);
        } else if(view->xwayland_surface != NULL)
        {
            render_surface(view->xwayland_surface->surface,
                0, 0, &rdata);
        }
	}
    wl_list_for_each_reverse(view, &output->panel->views, link) {
        if(!output_contains_stack(output, view->stack_index) || !view->is_fullscreen
            || view->focused_by == output->panel) { continue; }
        struct render_data rdata = {
            .output = output->wlr_output,
            .view = view,
            .renderer = renderer,
            .when = &now,
        };
        if(view->xdg_surface != NULL)
        {
            /* This calls our render_surface function for each surface among the
             * xdg_surface's toplevel and popups. */
            wlr_xdg_surface_for_each_surface(view->xdg_surface,
                    render_surface, &rdata);
        } else if(view->xwayland_surface != NULL)
        {
            render_surface(view->xwayland_surface->surface,
                0, 0, &rdata);
        }
    }
    wl_list_for_each_reverse(view, &output->panel->views, link) {
        if(!output_contains_stack(output, view->stack_index) ||
            view->focused_by != output->panel) { continue; }
        struct render_data rdata = {
            .output = output->wlr_output,
            .view = view,
            .renderer = renderer,
            .when = &now,
        };
        if(view->xdg_surface != NULL)
        {
            /* This calls our render_surface function for each surface among the
             * xdg_surface's toplevel and popups. */
            wlr_xdg_surface_for_each_surface(view->xdg_surface,
                    render_surface, &rdata);
        } else if(view->xwayland_surface != NULL)
        {
            render_surface(view->xwayland_surface->surface,
                0, 0, &rdata);
        }
    }
    wl_list_for_each_reverse(view, &output->panel->redirect_views, link) {
        struct render_data rdata = {
            .output = output->wlr_output,
            .view = view,
            .renderer = renderer,
            .when = &now,
        };
        render_surface(view->xwayland_surface->surface,
            0, 0, &rdata);
    }

    wl_list_for_each_reverse(ls, &output->server->layer_surfaces, link) {
        if(!ls->mapped || ls->surface->output != output->wlr_output ||
            ls->surface->current.layer != 2) { continue; }
        struct render_data rdata = {
            .output = output->wlr_output,
            .ls = ls,
            .renderer = renderer,
            .when = &now,
        };
        wlr_layer_surface_v1_for_each_surface(ls->surface,
            render_layer_surface, &rdata);
    }

    wl_list_for_each_reverse(ls, &output->server->layer_surfaces, link) {
        if(!ls->mapped || ls->surface->output != output->wlr_output ||
            ls->surface->current.layer != 3) { continue; }
        struct render_data rdata = {
            .output = output->wlr_output,
            .ls = ls,
            .renderer = renderer,
            .when = &now,
        };
        wlr_layer_surface_v1_for_each_surface(ls->surface,
            render_layer_surface, &rdata);
    }

    float matrix[9] = {0};
    matrix[0] = 2.0;
    matrix[4] = 2.0;
    matrix[2] = -1.0;
    matrix[5] = -1.0;

    float colour[4] = {0.0, 0.0, 0.0, 1.0 - output->server->brightness};
    wlr_render_quad_with_matrix(renderer, colour, matrix);

	/* Hardware cursors are rendered by the GPU on a separate plane, and can be
	 * moved around without re-rendering what's beneath them - which is more
	 * efficient. However, not all hardware supports hardware cursors. For this
	 * reason, wlroots provides a software fallback, which we ask it to render
	 * here. wlr_cursor handles configuring hardware vs software cursors for you,
	 * and this function is a no-op when hardware cursors are in use. */
	wlr_output_render_software_cursors(output->wlr_output, NULL);

	/* Conclude rendering and swap the buffers, showing the final frame
	 * on-screen. */
	wlr_renderer_end(renderer);
	wlr_output_commit(output->wlr_output);

    panel_post_update(output->panel);
}

static void server_new_output(struct wl_listener *listener, void *data) {
	/* This event is rasied by the backend when a new output (aka a display or
	 * monitor) becomes available. */
	struct tinywl_server *server =
		wl_container_of(listener, server, new_output);
	struct wlr_output *wlr_output = data;

	/* Some backends don't have modes. DRM+KMS does, and we need to set a mode
	 * before we can use the output. The mode is a tuple of (width, height,
	 * refresh rate), and each monitor supports only a specific set of modes. We
	 * just pick the monitor's preferred mode, a more sophisticated compositor
	 * would let the user configure it. */
	if (!wl_list_empty(&wlr_output->modes)) {
		struct wlr_output_mode *mode = wlr_output_preferred_mode(wlr_output);
        struct wlr_output_mode *m;
        int32_t highest_refresh = mode->refresh;
        wl_list_for_each(m, &wlr_output->modes, link) {
            if(m->refresh > highest_refresh)
            { mode = m; }
        }
        wlr_log(WLR_ERROR, "Using mode %dx%d@%d mHz\n", mode->width, mode->height, mode->refresh);
		wlr_output_set_mode(wlr_output, mode);
		wlr_output_enable(wlr_output, true);
		if (!wlr_output_commit(wlr_output)) {
			return;
		}
	}

	/* Allocates and configures our state for this output */
	struct tinywl_output *output =
		calloc(1, sizeof(struct tinywl_output));
	output->wlr_output = wlr_output;
	output->server = server;
	/* Sets up a listener for the frame notify event. */
	output->frame.notify = output_frame;
	wl_signal_add(&wlr_output->events.frame, &output->frame);
	wl_list_insert(&server->outputs, &output->link);

    output->panel = server->focused_panel; //TODO add proper panel and output management.
    if(output->panel->main_output == NULL) { output->panel->main_output = output; }
    wl_list_insert(&output->panel->outputs, &output->plink);

    output->stacks = calloc(2, sizeof(int32_t));
    output->stack_count = 2;
static int32_t start = 0;
    output->stacks[0] = start;
    output->stacks[1] = start + 1;
    start += 2;

    //Map the stacks
    for(int i = 0; i < output->stack_count; i++){
        output->panel->stacks[output->stacks[i]].mapped = true;
    }
//    struct gateway_panel* panel = calloc(1, sizeof(struct gateway_panel));
//    output->panel = panel;
//    wl_list_init(&panel->views);

	/* Adds this to the output layout. The add_auto function arranges outputs
	 * from left-to-right in the order they appear. A more sophisticated
	 * compositor would let the user configure the arrangement of outputs in the
	 * layout.
	 *
	 * The output layout utility automatically adds a wl_output global to the
	 * display, which Wayland clients can see to find out information about the
	 * output (such as DPI, scale factor, manufacturer, etc).
	 */
	wlr_output_layout_add_auto(server->output_layout, wlr_output);
}

static void xdg_surface_request_fullscreen(struct wl_listener* listener, void* data)
{
    struct tinywl_view *view = wl_container_of(listener, view, destroy);
    struct wlr_xdg_toplevel_set_fullscreen_event* event = data;
    wlr_xdg_toplevel_set_fullscreen(event->surface, event->fullscreen);
}

static void xdg_surface_map(struct wl_listener *listener, void *data) {
	/* Called when the surface is mapped, or ready to display on-screen. */
	struct tinywl_view *view = wl_container_of(listener, view, map);
    wlr_xdg_toplevel_set_tiled(view->xdg_surface, UINT_MAX);
	wl_list_remove(&view->link);    
    wl_list_insert(view->server->focused_panel->views.prev, &view->link);
	if(wl_list_length(&view->server->focused_panel->views) <= 1) {
        focus_view(view, view->server->focused_panel, false);
        center_mouse(view->server);
    }

    if(view->xdg_surface->role == WLR_XDG_SURFACE_ROLE_TOPLEVEL) {
        view->request_fullscreen.notify = xdg_surface_request_fullscreen;
        wl_signal_add(&view->xdg_surface->toplevel->events.request_fullscreen,
                &view->request_fullscreen);
    }
}

static void xdg_surface_unmap(struct wl_listener *listener, void *data) {
	/* Called when the surface is unmapped, and should no longer be shown. */
	struct tinywl_view *view = wl_container_of(listener, view, unmap);
    if(view->focused_by != NULL) {
        if(view->link.next != &view->focused_by->views) {
            struct tinywl_view *new_view = wl_container_of(view->link.next, new_view, link);
            focus_view(new_view, view->focused_by, false);
            view->focused_by = NULL;
            wl_list_remove(&view->link);
            center_mouse(view->server);
        } else {
            if(wl_list_length(&view->focused_by->views) > 1) {
                struct tinywl_view *new_view = wl_container_of(view->link.prev, new_view, link);
                focus_view(new_view, view->focused_by, false);
                view->focused_by = NULL;
                wl_list_remove(&view->link);
                center_mouse(view->server);
            } else {
                view->focused_by->focused_view = NULL;
                view->focused_by = NULL;
                wl_list_remove(&view->link);
            }
        }
    } else {
        wl_list_remove(&view->link);
    }
    wl_list_insert(&view->server->focused_panel->unmapped_views, &view->link);

    if(view->xdg_surface->role == WLR_XDG_SURFACE_ROLE_TOPLEVEL)
    {
        wl_list_remove(&view->request_fullscreen.link);
    }
}

static void xdg_surface_destroy(struct wl_listener *listener, void *data) {
	/* Called when the surface is destroyed and should never be shown again. */
	struct tinywl_view *view = wl_container_of(listener, view, destroy);
	wl_list_remove(&view->link);
	free(view);
}

static void xwayland_surface_unmap(struct wl_listener *listener, void *data) {
    /* Called when the surface is unmapped, and should no longer be shown. */
    struct tinywl_view *view = wl_container_of(listener, view, unmap);
    if(view->focused_by != NULL) {
        if(view->link.next != &view->focused_by->views) {
            struct tinywl_view *new_view = wl_container_of(view->link.next, new_view, link);
            focus_view(new_view, view->focused_by, false);
            view->focused_by = NULL;
            wl_list_remove(&view->link);
            center_mouse(view->server);
        } else {
            if(wl_list_length(&view->focused_by->views) > 1) {
                struct tinywl_view *new_view = wl_container_of(view->link.prev, new_view, link);
                focus_view(new_view, view->focused_by, false);
                view->focused_by = NULL;
                wl_list_remove(&view->link);
                center_mouse(view->server);
            } else {
                view->focused_by->focused_view = NULL;
                view->focused_by = NULL;
                wl_list_remove(&view->link);
            }
        }
    } else {
        wl_list_remove(&view->link);
    }
    wl_list_insert(&view->server->focused_panel->unmapped_views, &view->link);
}
 
static void xwayland_surface_destroy(struct wl_listener *listener, void *data) {
    /* Called when the surface is destroyed and should never be shown again. */
    struct tinywl_view *view = wl_container_of(listener, view, destroy);
    wl_list_remove(&view->link);
    free(view);
}

static void xwayland_surface_map(struct wl_listener *listener, void *data) {
    /* Called when the surface is mapped, or ready to display on-screen. */
    struct tinywl_view *view = wl_container_of(listener, view, map);
    wl_list_remove(&view->link);    
    wl_list_insert(view->server->focused_panel->views.prev, &view->link);
    if(wl_list_length(&view->server->focused_panel->views) <= 1) {
        focus_view(view, view->server->focused_panel, false);
    }
}

static void layer_surface_map(struct wl_listener *listener, void *data) {
    /* Called when the surface is mapped, or ready to display on-screen. */
    struct gateway_layer_surface *view = wl_container_of(listener, view, map);

    view->mapped = true;
}
 
static void layer_surface_unmap(struct wl_listener *listener, void *data) {
    struct gateway_layer_surface *view = wl_container_of(listener, view, map);
    view->mapped = false;
}
 
static void layer_surface_destroy(struct wl_listener *listener, void *data) {
    struct gateway_layer_surface *view = wl_container_of(listener, view, destroy);
    wl_list_remove(&view->link);
    free(view);
}

static void begin_interactive(struct tinywl_view *view,
		enum tinywl_cursor_mode mode, uint32_t edges) {
	/* This function sets up an interactive move or resize operation, where the
	 * compositor stops propegating pointer events to clients and instead
	 * consumes them itself, to move or resize windows. */
	struct tinywl_server *server = view->server;
	struct wlr_surface *focused_surface =
		server->seat->pointer_state.focused_surface;
	if (view->xdg_surface->surface != focused_surface) {
		/* Deny move/resize requests from unfocused clients. */
		return;
	}
	server->grabbed_view = view;
	server->cursor_mode = mode;

	if (mode == TINYWL_CURSOR_MOVE) {
		server->grab_x = server->cursor->x - view->x;
		server->grab_y = server->cursor->y - view->y;
	} else {
		struct wlr_box geo_box;
		wlr_xdg_surface_get_geometry(view->xdg_surface, &geo_box);

		double border_x = (view->x + geo_box.x) + ((edges & WLR_EDGE_RIGHT) ? geo_box.width : 0);
		double border_y = (view->y + geo_box.y) + ((edges & WLR_EDGE_BOTTOM) ? geo_box.height : 0);
		server->grab_x = server->cursor->x - border_x;
		server->grab_y = server->cursor->y - border_y;

		server->grab_geobox = geo_box;
		server->grab_geobox.x += view->x;
		server->grab_geobox.y += view->y;

		server->resize_edges = edges;
	}
}

static void xdg_toplevel_request_move(
		struct wl_listener *listener, void *data) {
	/* This event is raised when a client would like to begin an interactive
	 * move, typically because the user clicked on their client-side
	 * decorations. Note that a more sophisticated compositor should check the
	 * provied serial against a list of button press serials sent to this
	 * client, to prevent the client from requesting this whenever they want. */
	struct tinywl_view *view = wl_container_of(listener, view, request_move);
	begin_interactive(view, TINYWL_CURSOR_MOVE, 0);
}

static void xdg_toplevel_request_resize(
		struct wl_listener *listener, void *data) {
	/* This event is raised when a client would like to begin an interactive
	 * resize, typically because the user clicked on their client-side
	 * decorations. Note that a more sophisticated compositor should check the
	 * provied serial against a list of button press serials sent to this
	 * client, to prevent the client from requesting this whenever they want. */
	struct wlr_xdg_toplevel_resize_event *event = data;
	struct tinywl_view *view = wl_container_of(listener, view, request_resize);
	begin_interactive(view, TINYWL_CURSOR_RESIZE, event->edges);
}

static void server_new_xdg_surface(struct wl_listener *listener, void *data) {
	/* This event is raised when wlr_xdg_shell receives a new xdg surface from a
	 * client, either a toplevel (application window) or popup. */
	struct tinywl_server *server =
		wl_container_of(listener, server, new_xdg_surface);
	struct wlr_xdg_surface *xdg_surface = data;
	if (xdg_surface->role != WLR_XDG_SURFACE_ROLE_TOPLEVEL) {
		return;
	}

	/* Allocate a tinywl_view for this surface */
	struct tinywl_view *view =
		calloc(1, sizeof(struct tinywl_view));
	view->server = server;
	view->xdg_surface = xdg_surface;

    /* Listen to the various events it can emit */
    view->map.notify = xdg_surface_map;
    wl_signal_add(&xdg_surface->events.map, &view->map);
    view->unmap.notify = xdg_surface_unmap;
    wl_signal_add(&xdg_surface->events.unmap, &view->unmap);
    view->destroy.notify = xdg_surface_destroy;
    wl_signal_add(&xdg_surface->events.destroy, &view->destroy);

    /* cotd */
    struct wlr_xdg_toplevel *toplevel = xdg_surface->toplevel;
    view->request_move.notify = xdg_toplevel_request_move;
    wl_signal_add(&toplevel->events.request_move, &view->request_move);
    view->request_resize.notify = xdg_toplevel_request_resize;
    wl_signal_add(&toplevel->events.request_resize, &view->request_resize);

    /* Add it to the list of views. */
    wl_list_insert(&server->focused_panel->unmapped_views, &view->link);
}

static void server_new_xwayland_surface(struct wl_listener *listener, void *data) {
    /* This event is raised when wlr_xdg_shell receives a new xdg surface from a
     * client, either a toplevel (application window) or popup. */
    struct tinywl_server *server =
        wl_container_of(listener, server, new_xwayland_surface);
    struct wlr_xwayland_surface* xwayland_surface = data;

    /* Allocate a tinywl_view for this surface */
    struct tinywl_view *view =
        calloc(1, sizeof(struct tinywl_view));
    view->server = server;
    view->xwayland_surface = xwayland_surface;
 
    /* Listen to the various events it can emit */
    view->map.notify = xwayland_surface_map;
    wl_signal_add(&xwayland_surface->events.map, &view->map);
    view->unmap.notify = xwayland_surface_unmap;
    wl_signal_add(&xwayland_surface->events.unmap, &view->unmap);
    view->destroy.notify = xwayland_surface_destroy;
    wl_signal_add(&xwayland_surface->events.destroy, &view->destroy);
 
    /* Add it to the list of views. */
    wl_list_insert(&server->focused_panel->unmapped_views, &view->link);
}

static void server_new_layer_surface(struct wl_listener *listener, void *data) {
    /* This event is raised when wlr_xdg_shell receives a new xdg surface from a
     * client, either a toplevel (application window) or popup. */
    struct tinywl_server *server =
        wl_container_of(listener, server, new_layer_surface);
    struct wlr_layer_surface_v1* layer_surface = data;

    /* Allocate a tinywl_view for this surface */
    struct gateway_layer_surface *view =
        calloc(1, sizeof(struct gateway_layer_surface));
    view->server = server;
    view->surface = layer_surface;

    view->map.notify = layer_surface_map;
    wl_signal_add(&layer_surface->events.map, &view->map);
    view->unmap.notify = layer_surface_unmap;
    wl_signal_add(&layer_surface->events.unmap, &view->unmap);
    view->destroy.notify = layer_surface_destroy;
    wl_signal_add(&layer_surface->events.destroy, &view->destroy);

    if(view->surface->output == NULL)
    {
        view->surface->output = server->focused_panel->main_output->wlr_output;
    }
    uint32_t w = view->surface->current.desired_width;
    uint32_t h = view->surface->current.desired_height;
    uint32_t anchor = view->surface->current.anchor;
    if(anchor & (1|2) == (1|2)) { h = view->surface->output->height; }
    if(anchor & (4|8) == (4|8)) { w = view->surface->output->width; }

    wlr_layer_surface_v1_configure(view->surface, w, h);
 
    wl_list_insert(&server->layer_surfaces, &view->link);
}

static void server_new_xdg_decoration(struct wl_listener* listener, void* data)
{
    struct wlr_xdg_toplevel_decoration_v1* decoration = data;
    wlr_xdg_toplevel_decoration_v1_set_mode(
        decoration,
        WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE
    );
}

int main(int argc, char *argv[]) {
	wlr_log_init(WLR_DEBUG, NULL);
	char *startup_cmd = NULL;

	int c;
	while ((c = getopt(argc, argv, "s:h")) != -1) {
		switch (c) {
		case 's':
			startup_cmd = optarg;
			break;
		default:
			printf("Usage: %s [-s startup command]\n", argv[0]);
			return 0;
		}
	}
	if (optind < argc) {
		printf("Usage: %s [-s startup command]\n", argv[0]);
		return 0;
	}

                    // ENVIRONMENT SETUP
    setenv("QT_QPA_PLATFORMTHEME","qt5ct", 1);
    setenv("QT_QPA_PLATFORM", "wayland", 1);
    setenv("MOZ_ENABLE_WAYLAND", "1", 1);

    struct tinywl_server server; // GATEWAY CONFIGURATION
    server.config = calloc(1, sizeof(struct gateway_config));
    server.config->terminal = "foot";
    server.config->launcher = "fuzzel -b1f301fff -tffffffff -l20";
    server.config->mouse_sens = 0.5;
    server.config->kbd_layout = "us";
    server.config->kbd_variant = "dvorak";
    server.config->window_gaps = 8;


    server.brightness = 1.0;
    server.passthrough_enabled = false;

	/* The Wayland display is managed by libwayland. It handles accepting
	 * clients from the Unix socket, manging Wayland globals, and so on. */
	server.wl_display = wl_display_create();
	/* The backend is a wlroots feature which abstracts the underlying input and
	 * output hardware. The autocreate option will choose the most suitable
	 * backend based on the current environment, such as opening an X11 window
	 * if an X11 server is running. The NULL argument here optionally allows you
	 * to pass in a custom renderer if wlr_renderer doesn't meet your needs. The
	 * backend uses the renderer, for example, to fall back to software cursors
	 * if the backend does not support hardware cursors (some older GPUs
	 * don't). */
	server.backend = wlr_backend_autocreate(server.wl_display);

	/* If we don't provide a renderer, autocreate makes a GLES2 renderer for us.
	 * The renderer is responsible for defining the various pixel formats it
	 * supports for shared memory, this configures that for clients. */
	server.renderer = wlr_backend_get_renderer(server.backend);
	wlr_renderer_init_wl_display(server.renderer, server.wl_display);

	/* This creates some hands-off wlroots interfaces. The compositor is
	 * necessary for clients to allocate surfaces and the data device manager
	 * handles the clipboard. Each of these wlroots interfaces has room for you
	 * to dig your fingers in and play with their behavior if you want. Note that
	 * the clients cannot set the selection directly without compositor approval,
	 * see the handling of the request_set_selection event below.*/
	server.compositor = wlr_compositor_create(server.wl_display, server.renderer);
	wlr_data_device_manager_create(server.wl_display);

	/* Creates an output layout, which a wlroots utility for working with an
	 * arrangement of screens in a physical layout. */
	server.output_layout = wlr_output_layout_create();
    server.xdg_output_manager = wlr_xdg_output_manager_v1_create(server.wl_display, server.output_layout);

	/* Configure a listener to be notified when new outputs are available on the
	 * backend. */
	wl_list_init(&server.outputs);
	server.new_output.notify = server_new_output;
	wl_signal_add(&server.backend->events.new_output, &server.new_output);

    struct gateway_panel* panel = calloc(1, sizeof(struct gateway_panel));
    wl_list_init(&panel->unmapped_views);
    wl_list_init(&panel->views);
    wl_list_init(&panel->redirect_views);
    wl_list_init(&panel->outputs);

    panel->stacks = calloc(4, sizeof(struct gateway_panel));
    panel->stack_count = 4;
    panel->stacks[0].max_items = 1;
    panel->stacks[1].max_items = 1;
    panel->stacks[2].max_items = 2;
    panel->stacks[3].max_items = 2;
    server.focused_panel = panel;

	/* Set up our list of views and the xdg-shell. The xdg-shell is a Wayland
	 * protocol which is used for application windows. For more detail on
	 * shells, refer to my article:
	 *
	 * https://drewdevault.com/2018/07/29/Wayland-shells.html
	 */
	server.xdg_shell = wlr_xdg_shell_create(server.wl_display);
	server.new_xdg_surface.notify = server_new_xdg_surface;
	wl_signal_add(&server.xdg_shell->events.new_surface,
			&server.new_xdg_surface);

    server.decoration_manager = wlr_xdg_decoration_manager_v1_create(server.wl_display);
    server.new_toplevel_decoration.notify = server_new_xdg_decoration;
    wl_signal_add(&server.decoration_manager->events.new_toplevel_decoration,
            &server.new_toplevel_decoration);

    // XWayland
    server.xwayland = wlr_xwayland_create(server.wl_display, server.compositor, false);
    server.new_xwayland_surface.notify = server_new_xwayland_surface;
    wl_signal_add(&server.xwayland->events.new_surface,
            &server.new_xwayland_surface);

    // Layer Shell
    server.layer_shell = wlr_layer_shell_v1_create(server.wl_display);
    server.new_layer_surface.notify = server_new_layer_surface;
    wl_signal_add(&server.layer_shell->events.new_surface,
            &server.new_layer_surface);
    wl_list_init(&server.layer_surfaces);

    // Wlr Screencopy
    server.screencopy = wlr_screencopy_manager_v1_create(server.wl_display);

    // Relative and constrained pointer
    server.relative_pointer = wlr_relative_pointer_manager_v1_create(server.wl_display);
    server.pointer_constraints = wlr_pointer_constraints_v1_create(server.wl_display);

	/*
	 * Creates a cursor, which is a wlroots utility for tracking the cursor
	 * image shown on screen.
	 */
	server.cursor = wlr_cursor_create();
	wlr_cursor_attach_output_layout(server.cursor, server.output_layout);

	/* Creates an xcursor manager, another wlroots utility which loads up
	 * Xcursor themes to source cursor images from and makes sure that cursor
	 * images are available at all scale factors on the screen (necessary for
	 * HiDPI support). We add a cursor theme at scale factor 1 to begin with. */
	server.cursor_mgr = wlr_xcursor_manager_create(NULL, 24);
	wlr_xcursor_manager_load(server.cursor_mgr, 1);

	/*
	 * wlr_cursor *only* displays an image on screen. It does not move around
	 * when the pointer moves. However, we can attach input devices to it, and
	 * it will generate aggregate events for all of them. In these events, we
	 * can choose how we want to process them, forwarding them to clients and
	 * moving the cursor around. More detail on this process is described in my
	 * input handling blog post:
	 *
	 * https://drewdevault.com/2018/07/17/Input-handling-in-wlroots.html
	 *
	 * And more comments are sprinkled throughout the notify functions above.
	 */
	server.cursor_motion.notify = server_cursor_motion;
	wl_signal_add(&server.cursor->events.motion, &server.cursor_motion);
	server.cursor_motion_absolute.notify = server_cursor_motion_absolute;
	wl_signal_add(&server.cursor->events.motion_absolute,
			&server.cursor_motion_absolute);
	server.cursor_button.notify = server_cursor_button;
	wl_signal_add(&server.cursor->events.button, &server.cursor_button);
	server.cursor_axis.notify = server_cursor_axis;
	wl_signal_add(&server.cursor->events.axis, &server.cursor_axis);
	server.cursor_frame.notify = server_cursor_frame;
	wl_signal_add(&server.cursor->events.frame, &server.cursor_frame);

	/*
	 * Configures a seat, which is a single "seat" at which a user sits and
	 * operates the computer. This conceptually includes up to one keyboard,
	 * pointer, touch, and drawing tablet device. We also rig up a listener to
	 * let us know when new input devices are available on the backend.
	 */
	wl_list_init(&server.keyboards);
	server.new_input.notify = server_new_input;
	wl_signal_add(&server.backend->events.new_input, &server.new_input);
	server.seat = wlr_seat_create(server.wl_display, "seat0");
	server.request_cursor.notify = seat_request_cursor;
	wl_signal_add(&server.seat->events.request_set_cursor,
			&server.request_cursor);
	server.request_set_selection.notify = seat_request_set_selection;
	wl_signal_add(&server.seat->events.request_set_selection,
			&server.request_set_selection);

	/* Add a Unix socket to the Wayland display. */
	const char *socket = wl_display_add_socket_auto(server.wl_display);
	if (!socket) {
        wlr_xwayland_destroy(server.xwayland);
		wlr_backend_destroy(server.backend);
		return 1;
	}

	/* Start the backend. This will enumerate outputs and inputs, become the DRM
	 * master, etc */
	if (!wlr_backend_start(server.backend)) {
        wlr_xwayland_destroy(server.xwayland);
		wlr_backend_destroy(server.backend);
		wl_display_destroy(server.wl_display);
		return 1;
	}

	/* Set the WAYLAND_DISPLAY and DISPLAY environment variables to our sockets and run the
	 * startup command if requested. */
	setenv("WAYLAND_DISPLAY", socket, true);
    setenv("DISPLAY", server.xwayland->display_name, true);
	if (startup_cmd) {
		if (fork() == 0) {
			execl("/bin/sh", "/bin/sh", "-c", startup_cmd, (void *)NULL);
		}
    }

    char startup_file_path[512];
    strcpy(startup_file_path, getenv("HOME"));
    strcat(startup_file_path, "/.config/gateway/startup.sh");
    if( access(startup_file_path, X_OK ) == 0 ) {
        char* args[2];
        args[0] = "startup.sh";
        args[1] = NULL;
        if(fork() == 0)
        {
            execvp(startup_file_path, args);
        }
    }


	/* Run the Wayland event loop. This does not return until you exit the
	 * compositor. Starting the backend rigged up all of the necessary event
	 * loop configuration to listen to libinput events, DRM events, generate
	 * frame events at the refresh rate, and so on. */
	wlr_log(WLR_INFO, "Running Wayland compositor on WAYLAND_DISPLAY=%s",
			socket);
	wl_display_run(server.wl_display);

	/* Once wl_display_run returns, we shut down the server. */
    wlr_xwayland_destroy(server.xwayland);
	wl_display_destroy_clients(server.wl_display);
	wl_display_destroy(server.wl_display);
	return 0;
}
