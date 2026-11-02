// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 genie1997 <genie1997.dev@gmail.com>.
 */

/* Shallow CPU idle while the panel is on, deep idle once it is off or in low power */

#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_qos.h>
#include <linux/soc/qcom/panel_event_notifier.h>
#include <linux/workqueue.h>
#include <drm/drm_panel.h>

static struct pm_qos_request qos;
static struct device_node *panel_np;
static void *cookie;
static int retries = 10;
static bool panel_on = true;
static int latency_us;

static void screen_idle_apply(void)
{
	s32 v = panel_on && latency_us >= 0 ? latency_us : PM_QOS_DEFAULT_VALUE;

	if (cpu_latency_qos_request_active(&qos))
		cpu_latency_qos_update_request(&qos, v);
}

static int latency_us_set(const char *val, const struct kernel_param *kp)
{
	int ret = param_set_int(val, kp);

	if (!ret)
		screen_idle_apply();
	return ret;
}

static const struct kernel_param_ops latency_us_ops = {
	.set = latency_us_set,
	.get = param_get_int,
};
module_param_cb(latency_us, &latency_us_ops, &latency_us, 0644);
MODULE_PARM_DESC(latency_us, "CPU wakeup latency limit while the panel is on, -1 = no limit");

/* both the early and the late event of a change carry the same state */
static void screen_idle_event(enum panel_event_notifier_tag tag,
			      struct panel_event_notification *n, void *data)
{
	if (!n)
		return;

	switch (n->notif_type) {
	case DRM_PANEL_EVENT_UNBLANK:
		panel_on = true;
		break;
	case DRM_PANEL_EVENT_BLANK:
	case DRM_PANEL_EVENT_BLANK_LP:
		panel_on = false;
		break;
	default:
		return;
	}
	screen_idle_apply();
}

static void screen_idle_register(struct work_struct *work);
static DECLARE_DELAYED_WORK(register_dw, screen_idle_register);

/* the panel shows up when the display driver probes, which may be after this module */
static void screen_idle_register(struct work_struct *work)
{
	struct drm_panel *panel = ERR_PTR(-ENODEV);
	int i, count = of_count_phandle_with_args(panel_np, "qcom,display-panels", NULL);

	for (i = 0; i < count; i++) {
		struct device_node *node = of_parse_phandle(panel_np, "qcom,display-panels", i);

		panel = of_drm_find_panel(node);
		of_node_put(node);
		if (!IS_ERR(panel))
			break;
	}

	if (IS_ERR(panel)) {
		if (PTR_ERR(panel) == -EPROBE_DEFER && retries-- > 0) {
			schedule_delayed_work(&register_dw, 5 * HZ);
			return;
		}
		pr_err("screen_idle: no active panel: %ld\n", PTR_ERR(panel));
		goto release;
	}

	cookie = panel_event_notifier_register(PANEL_EVENT_NOTIFICATION_PRIMARY,
					       PANEL_EVENT_NOTIFIER_CLIENT_SCREEN_IDLE, panel,
					       screen_idle_event, NULL);
	if (!IS_ERR(cookie))
		return;

	pr_err("screen_idle: panel event notifier: %ld\n", PTR_ERR(cookie));
	cookie = NULL;
release:
	/* without panel events the request would outlive the screen */
	panel_on = false;
	screen_idle_apply();
}

static int screen_idle_probe(struct platform_device *pdev)
{
	if (of_count_phandle_with_args(pdev->dev.of_node, "qcom,display-panels", NULL) <= 0)
		return -ENODEV;

	panel_np = pdev->dev.of_node;
	cpu_latency_qos_add_request(&qos, PM_QOS_DEFAULT_VALUE);
	screen_idle_apply();
	schedule_delayed_work(&register_dw, 0);
	return 0;
}

static void screen_idle_remove(struct platform_device *pdev)
{
	cancel_delayed_work_sync(&register_dw);
	if (cookie)
		panel_event_notifier_unregister(cookie);
	cpu_latency_qos_remove_request(&qos);
}

static const struct of_device_id screen_idle_match[] = {
	{ .compatible = "xiaomi,screen-idle" },
	{ }
};
MODULE_DEVICE_TABLE(of, screen_idle_match);

static struct platform_driver screen_idle_driver = {
	.probe = screen_idle_probe,
	.remove = screen_idle_remove,
	.driver = {
		.name = "screen-idle",
		.of_match_table = screen_idle_match,
	},
};
module_platform_driver(screen_idle_driver);

MODULE_DESCRIPTION("Shallow CPU idle while the panel is on");
MODULE_LICENSE("GPL");
