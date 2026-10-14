/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef _DSI_MI_DISPLAY_H_
#define _DSI_MI_DISPLAY_H_

struct dsi_display;

#if IS_ENABLED(CONFIG_DSI_MI_DISPLAY)
int dsi_mi_display_register(void);
void dsi_mi_display_unregister(void);
void dsi_mi_display_init(struct dsi_display *display);
void dsi_mi_display_deinit(struct dsi_display *display);
void dsi_mi_display_fps_changed(struct dsi_display *display);
#else
static inline int dsi_mi_display_register(void)
{
	return 0;
}

static inline void dsi_mi_display_unregister(void)
{
}

static inline void dsi_mi_display_init(struct dsi_display *display)
{
}

static inline void dsi_mi_display_deinit(struct dsi_display *display)
{
}

static inline void dsi_mi_display_fps_changed(struct dsi_display *display)
{
}
#endif

#endif /* _DSI_MI_DISPLAY_H_ */
