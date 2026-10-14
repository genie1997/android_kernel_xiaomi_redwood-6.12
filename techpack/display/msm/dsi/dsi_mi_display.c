// SPDX-License-Identifier: GPL-2.0-only

#include <linux/device.h>
#include <linux/sysfs.h>

#include "dsi_display.h"
#include "dsi_mi_display.h"

static ssize_t dynamic_fps_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct dsi_display *display = dev_get_drvdata(dev);
	u32 fps = 0;

	mutex_lock(&display->display_lock);
	if (display->panel && display->panel->cur_mode)
		fps = display->panel->cur_mode->timing.refresh_rate;
	mutex_unlock(&display->display_lock);

	/* "null" until the first mode set, as readers of this node expect */
	if (!fps)
		return sysfs_emit(buf, "null\n");

	return sysfs_emit(buf, "%u\n", fps);
}
static DEVICE_ATTR_RO(dynamic_fps);

static struct attribute *mi_display_attrs[] = {
	&dev_attr_dynamic_fps.attr,
	NULL
};
ATTRIBUTE_GROUPS(mi_display);

static const struct class mi_display_class = {
	.name = "mi_display",
};

int dsi_mi_display_register(void)
{
	return class_register(&mi_display_class);
}

void dsi_mi_display_unregister(void)
{
	if (class_is_registered(&mi_display_class))
		class_unregister(&mi_display_class);
}

void dsi_mi_display_init(struct dsi_display *display)
{
	int id = strcmp(display->display_type, "primary") ? 1 : 0;

	if (!class_is_registered(&mi_display_class))
		return;

	display->mi_dev = device_create_with_groups(&mi_display_class,
			&display->pdev->dev, 0, display, mi_display_groups,
			"disp-DSI-%d", id);
	if (IS_ERR(display->mi_dev)) {
		DSI_ERR("[%s] failed to create disp-DSI-%d, rc=%ld\n",
				display->name, id, PTR_ERR(display->mi_dev));
		display->mi_dev = NULL;
	}
}

void dsi_mi_display_deinit(struct dsi_display *display)
{
	if (!display->mi_dev)
		return;

	device_unregister(display->mi_dev);
	display->mi_dev = NULL;
}

void dsi_mi_display_fps_changed(struct dsi_display *display)
{
	if (display->mi_dev)
		sysfs_notify(&display->mi_dev->kobj, NULL, "dynamic_fps");
}
