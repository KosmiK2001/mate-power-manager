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
 *
 * Two rules this file must never break, both learned the hard way:
 *
 * 1. NEVER touch the wl_display GDK is driving.  A wl_display_roundtrip() on
 *    GDK's display corrupts its event queue and the next gdk_display_get_event()
 *    aborts the process (SIGABRT inside wl_display_dispatch_queue_pending, seen
 *    in both mate-panel and mate-power-manager).  We open our own connection.
 *
 * 2. The ramp length is whatever the compositor announces in the gamma_size
 *    event - not a constant.  Sending an fd whose length is not exactly
 *    3 * gamma_size makes the compositor raise the protocol error
 *    "invalid_gamma", and libwayland aborts the client.  Wait for gamma_size
 *    before the first set_gamma.
 *
 * The protocol has no wl_surface in it at all: set_gamma() is the whole
 * request.  Destroying the control (or dropping the connection) makes the
 * compositor restore the original table, so the connection is kept open for as
 * long as the dimming must last.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "config.h"

#ifdef HAVE_WAYLAND

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#include <glib.h>
#include <gdk/gdkwayland.h>

#include "wlr-gamma-control-client.h"

#include "gpm-gamma-wayland.h"

static struct wl_display *own_dpy = NULL;
static struct wl_registry *own_registry = NULL;
static struct wl_output *own_output = NULL;
static struct zwlr_gamma_control_manager_v1 *gamma_manager = NULL;
static struct zwlr_gamma_control_v1 *gamma_control = NULL;
static guint gamma_size = 0;
static gboolean gamma_failed = FALSE;

static void
gamma_teardown (void)
{
	if (gamma_control != NULL) {
		zwlr_gamma_control_v1_destroy (gamma_control);
		gamma_control = NULL;
	}
	if (own_output != NULL) {
		wl_output_destroy (own_output);
		own_output = NULL;
	}
	if (gamma_manager != NULL) {
		zwlr_gamma_control_manager_v1_destroy (gamma_manager);
		gamma_manager = NULL;
	}
	if (own_registry != NULL) {
		wl_registry_destroy (own_registry);
		own_registry = NULL;
	}
	if (own_dpy != NULL) {
		wl_display_flush (own_dpy);
		wl_display_disconnect (own_dpy);
		own_dpy = NULL;
	}
	gamma_size = 0;
	gamma_failed = FALSE;
}

static void
gamma_size_cb (void *data, struct zwlr_gamma_control_v1 *control, uint32_t size)
{
	gamma_size = size;
	g_debug ("Wayland gamma: compositor reports ramp size %u", size);
}

static void
gamma_failed_cb (void *data, struct zwlr_gamma_control_v1 *control)
{
	/* The output lost gamma control: unsupported, refused, or taken over.
	 * The next apply() rebuilds everything, which also picks up a new
	 * output after a hotplug. */
	g_debug ("Wayland gamma: control for this output is gone");
	gamma_failed = TRUE;
}

static const struct zwlr_gamma_control_v1_listener gamma_control_listener = {
	.gamma_size = gamma_size_cb,
	.failed = gamma_failed_cb,
};

static void
registry_global_cb (void *data, struct wl_registry *registry, uint32_t name,
		    const char *interface, uint32_t version)
{
	if (g_strcmp0 (interface, "zwlr_gamma_control_manager_v1") == 0) {
		gamma_manager = wl_registry_bind (registry, name,
						  &zwlr_gamma_control_manager_v1_interface,
						  MIN (version, 1));
	} else if (g_strcmp0 (interface, "wl_output") == 0 && own_output == NULL) {
		own_output = wl_registry_bind (registry, name,
					       &wl_output_interface,
					       MIN (version, 2));
	}
}

static void
registry_global_remove_cb (void *data, struct wl_registry *registry, uint32_t name)
{
	/* handled through the failed event of the gamma control object */
}

static const struct wl_registry_listener registry_listener = {
	.global = registry_global_cb,
	.global_remove = registry_global_remove_cb,
};

/* Fill @size entries of a linear ramp scaled by @level (0..100). */
static gboolean
gamma_build_lut (guint size, guint level, guint16 **out, gsize *out_len)
{
	guint16 *table;
	guint i;

	if (size == 0)
		return FALSE;

	*out_len = 3 * (gsize) size * sizeof (guint16);
	table = g_try_malloc0 (*out_len);
	if (table == NULL)
		return FALSE;

	for (i = 0; i < size; i++) {
		guint64 ramp = (guint64) i * 65535 / (size > 1 ? size - 1 : 1);
		guint16 value = (guint16) (ramp * level / 100);

		table[i] = value;
		table[i + size] = value;
		table[i + 2 * size] = value;
	}

	*out = table;
	return TRUE;
}

/*
 * Open our own connection and bind what we need.  Returns FALSE when the
 * session is not Wayland, the compositor has no gamma control, or the output
 * does not support gamma tables.
 */
static gboolean
gamma_ensure (void)
{
	if (gamma_control != NULL && !gamma_failed && gamma_size > 0)
		return TRUE;

	if (gamma_failed || gamma_size == 0)
		gamma_teardown ();

	if (own_dpy == NULL) {
		own_dpy = wl_display_connect (NULL);
		if (own_dpy == NULL) {
			g_debug ("Wayland gamma: cannot connect to the compositor");
			return FALSE;
		}

		own_registry = wl_display_get_registry (own_dpy);
		wl_registry_add_listener (own_registry, &registry_listener, NULL);

		if (wl_display_roundtrip (own_dpy) < 0) {
			g_debug ("Wayland gamma: registry roundtrip failed");
			gamma_teardown ();
			return FALSE;
		}
	}

	if (gamma_manager == NULL || own_output == NULL) {
		g_debug ("Wayland gamma: compositor exports no gamma control");
		gamma_teardown ();
		return FALSE;
	}

	if (gamma_control == NULL) {
		gamma_control = zwlr_gamma_control_manager_v1_get_gamma_control (gamma_manager,
										 own_output);
		if (gamma_control == NULL) {
			gamma_teardown ();
			return FALSE;
		}
		zwlr_gamma_control_v1_add_listener (gamma_control,
						    &gamma_control_listener, NULL);
	}

	/* wait for gamma_size (and a possible early "failed") */
	if (wl_display_roundtrip (own_dpy) < 0 || gamma_failed) {
		g_debug ("Wayland gamma: output rejected gamma control");
		gamma_teardown ();
		return FALSE;
	}

	if (gamma_size == 0) {
		g_debug ("Wayland gamma: compositor never announced a ramp size");
		gamma_teardown ();
		return FALSE;
	}

	return TRUE;
}

/**
 * gpm_gamma_wayland_available:
 *
 * TRUE when this session is Wayland and the compositor exports the gamma
 * control protocol (wlroots compositors such as wayfire/sway do) for the
 * current output.
 **/
gboolean
gpm_gamma_wayland_available (void)
{
	GdkDisplay *display;

	display = gdk_display_get_default ();
	if (display == NULL || !GDK_IS_WAYLAND_DISPLAY (display))
		return FALSE;

	return gamma_ensure ();
}

/**
 * gpm_gamma_wayland_apply:
 * @level: 0..100
 *
 * Push a linear brightness ramp to the output.  The table is a shared memory
 * buffer of 3 x gamma_size uint16 values (red, green, blue) - exactly what the
 * protocol expects.
 **/
gboolean
gpm_gamma_wayland_apply (guint level)
{
	guint16 *table = NULL;
	gsize len = 0;
	int fd;

	if (level > 100)
		level = 100;

	if (!gamma_ensure ())
		return FALSE;

	if (!gamma_build_lut (gamma_size, level, &table, &len))
		return FALSE;

	fd = memfd_create ("gpm-gamma", MFD_CLOEXEC);
	if (fd < 0) {
		g_debug ("Wayland gamma: memfd_create failed: %s", g_strerror (errno));
		g_free (table);
		return FALSE;
	}
	if (ftruncate (fd, len) != 0) {
		g_debug ("Wayland gamma: ftruncate failed: %s", g_strerror (errno));
		close (fd);
		g_free (table);
		return FALSE;
	}
	if (write (fd, table, len) != (ssize_t) len) {
		g_debug ("Wayland gamma: write failed: %s", g_strerror (errno));
		close (fd);
		g_free (table);
		return FALSE;
	}
	g_free (table);

	zwlr_gamma_control_v1_set_gamma (gamma_control, fd);
	close (fd);

	if (wl_display_flush (own_dpy) < 0) {
		g_debug ("Wayland gamma: flush failed");
		gamma_teardown ();
		return FALSE;
	}

	/* collect a possible "failed" event from the compositor */
	if (wl_display_roundtrip (own_dpy) < 0 || gamma_failed) {
		g_debug ("Wayland gamma: compositor refused the LUT");
		gamma_teardown ();
		return FALSE;
	}

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
