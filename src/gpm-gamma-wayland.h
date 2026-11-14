/*
 * Copyright (C) 2026 MATE developers
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef __GPM_GAMMA_WAYLAND_H
#define __GPM_GAMMA_WAYLAND_H

#include <glib.h>

G_BEGIN_DECLS

gboolean	 gpm_gamma_wayland_available	(void);
gboolean	 gpm_gamma_wayland_apply	(guint level);
void		 gpm_gamma_wayland_reset	(void);

G_END_DECLS

#endif /* __GPM_GAMMA_WAYLAND_H */
