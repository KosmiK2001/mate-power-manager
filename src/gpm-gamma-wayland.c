/*
 * Copyright (C) 2026 MATE developers
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Gamma control on Wayland via zwlr_gamma_control_manager_v1 (wlroots).
 *
 * On X11 the same effect is produced with XRRSetCrtcGamma; on Wayland there is
 * no X ramp, but wlroots compositors export the gamma-control protocol, so the
 * LUT can be pushed to the output directly.  Like the X11 path this darkens the
 * picture rather than the backlight, so it is a fallback, never the goal.
 */

#include "config.h"

#ifdef HAVE_WAYLAND

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>

#include <glib.h>
#include <gdk/gdkwayland.h>

#include "wlr-gamma-control-client.h"

#include "gpm-gamma-wayland.h"

#define GAMMA_RAMP_SIZE 256

static struct wl_display *wl_dpy = NULL;
static struct wl_registry *wl_registry = NULL;
static struct wl_compositor *wl_compositor = NULL;
static struct wl_output *wl_output = NULL;
static struct wl_surface *wl_surface = NULL;
static struct zwlr_gamma_control_manager_v1 *gamma_manager = NULL;
static struct zwlr_gamma_control_v1 *gamma_control = NULL;

static gboolean
gamma_control_failed_cb (void *data, struct zwlr_gamma_control_v1 *control)
{
	g_debug ("Wayland gamma control failed for this output; giving up on it");
	if (gamma_control != NULL) {
		zwlr_gamma_control_v1_destroy (gamma_control);
		gamma_control = NULL;
	}
	return 0;
}

static const struct zwlr_gamma_control_v1_listener gamma_control_listener = {
	.failed = gamma_control_failed_cb,
};

static void
registry_global_cb (void *data, struct wl_registry *registry, uint32_t name,
		    const char *interface, uint32_t version)
{
	if (g_strcmp0 (interface, "zwlr_gamma_control_manager_v1") == 0) {
		gamma_manager = wl_registry_bind (registry, name,
						  &zwlr_gamma_control_manager_v1_interface, 1);
	} else if (g_strcmp0 (interface, "wl_compositor") == 0) {
		wl_compositor = wl_registry_bind (registry, name,
						  &wl_compositor_interface, 4);
	} else if (g_strcmp0 (interface, "wl_output") == 0 && wl_output == NULL) {
		wl_output = wl_registry_bind (registry, name, &wl_output_interface, 2);
	}
}

static void
registry_global_remove_cb (void *data, struct wl_registry *registry, uint32_t name)
{
	/* nothing to do: the bindings above stay valid for our lifetime */
}

static const struct wl_registry_listener registry_listener = {
	.global = registry_global_cb,
	.global_remove = registry_global_remove_cb,
};

/**
 * gpm_gamma_wayland_available:
 *
 * TRUE when this session is Wayland and the compositor exports the gamma
 * control protocol (wlroots compositors such as wayfire/sway do).
 **/
gboolean
gpm_gamma_wayland_available (void)
{
	GdkDisplay *display;

	display = gdk_display_get_default ();
	if (display == NULL || !GDK_IS_WAYLAND_DISPLAY (display))
		return FALSE;

	if (gamma_manager != NULL)
		return TRUE;

	if (wl_dpy == NULL) {
		wl_dpy = gdk_wayland_display_get_wl_display (display);
		if (wl_dpy == NULL)
			return FALSE;
		wl_registry = wl_display_get_registry (wl_dpy);
		wl_registry_add_listener (wl_registry, &registry_listener, NULL);
		/* one round trip is enough: the registry is filled synchronously */
		wl_display_roundtrip (wl_dpy);
	}

	if (gamma_manager == NULL || wl_output == NULL || wl_compositor == NULL)
		return FALSE;

	if (gamma_control == NULL) {
		gamma_control = zwlr_gamma_control_manager_v1_get_gamma_control (gamma_manager,
										 wl_output);
		if (gamma_control == NULL)
			return FALSE;
		zwlr_gamma_control_v1_add_listener (gamma_control,
						    &gamma_control_listener, NULL);
	}

	if (wl_surface == NULL)
		wl_surface = wl_compositor_create_surface (wl_compositor);

	return wl_surface != NULL;
}

/**
 * gpm_gamma_wayland_apply:
 * @level: 0..100
 *
 * Push a linear brightness ramp to the output.  The table is a shared memory
 * buffer of 3 x GAMMA_RAMP_SIZE uint16 values (red, green, blue), exactly what
 * the protocol expects; the compositor applies it when the surface is committed.
 **/
gboolean
gpm_gamma_wayland_apply (guint level)
{
	guint16 *table;
	gsize size;
	int fd;
	gdouble scale;
	guint i;

	if (!gpm_gamma_wayland_available ())
		return FALSE;

	if (level > 100)
		level = 100;

	scale = (gdouble) level / 100.0;

	size = 3 * GAMMA_RAMP_SIZE * sizeof (guint16);
	fd = memfd_create ("gpm-gamma", MFD_CLOEXEC);
	if (fd < 0) {
		g_debug ("Wayland gamma: memfd_create failed: %s", g_strerror (errno));
		return FALSE;
	}
	if (ftruncate (fd, size) != 0) {
		g_debug ("Wayland gamma: ftruncate failed: %s", g_strerror (errno));
		close (fd);
		return FALSE;
	}

	table = mmap (NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (table == MAP_FAILED) {
		g_debug ("Wayland gamma: mmap failed: %s", g_strerror (errno));
		close (fd);
		return FALSE;
	}

	for (i = 0; i < GAMMA_RAMP_SIZE; i++) {
		guint16 value = (guint16) (i * 257 * scale);

		table[i] = value;
		table[i + GAMMA_RAMP_SIZE] = value;
		table[i + 2 * GAMMA_RAMP_SIZE] = value;
	}
	munmap (table, size);

	zwlr_gamma_control_v1_set_gamma (gamma_control, fd);
	wl_surface_commit (wl_surface);
	wl_display_flush (wl_dpy);

	close (fd);
	return TRUE;
}

/**
 * gpm_gamma_wayland_reset:
 *
 * Restore a neutral ramp.  Must be called before switching to another backend
 * and on shutdown: leaving a dimmed LUT behind keeps the screen dark with no
 * way for the user to undo it.
 **/
void
gpm_gamma_wayland_reset (void)
{
	if (gamma_control == NULL)
		return;

	gpm_gamma_wayland_apply (100);
}

#endif /* HAVE_WAYLAND */
