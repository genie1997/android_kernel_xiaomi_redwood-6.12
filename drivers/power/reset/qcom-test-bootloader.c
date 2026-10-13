// SPDX-License-Identifier: GPL-2.0
#include <linux/device.h>
#include <linux/init.h>
#include <linux/notifier.h>
#include <linux/nvmem-consumer.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/printk.h>

/* The "bootloader" value of qcom-reboot-reason, read by the bootloader as fastboot mode */
#define RESTART_REASON_BOOTLOADER	0x02

static struct device_node *reason_np;

static void preset_bootloader_reason(void)
{
	u8 reason = RESTART_REASON_BOOTLOADER;
	struct nvmem_cell *cell;
	int ret;

	cell = of_nvmem_cell_get(reason_np, "restart_reason");
	if (IS_ERR(cell)) {
		pr_err("test bootloader stop: no restart reason cell: %ld\n", PTR_ERR(cell));
		return;
	}
	ret = nvmem_cell_write(cell, &reason, sizeof(reason));
	nvmem_cell_put(cell);
	if (ret < 0)
		pr_err("test bootloader stop: write failed: %d\n", ret);
	else
		pr_info("test bootloader stop: every reset now stops in the bootloader\n");
}

static int reason_dev_bound(struct notifier_block *nb, unsigned long action, void *data)
{
	struct device *dev = data;

	if (action == BUS_NOTIFY_BOUND_DRIVER && dev->of_node == reason_np)
		preset_bootloader_reason();
	return NOTIFY_DONE;
}

static struct notifier_block reason_nb = {
	.notifier_call = reason_dev_bound,
};

static int __init test_bootloader_stop_init(void)
{
	struct device *dev;
	int ret;

	reason_np = of_find_compatible_node(NULL, NULL, "qcom,reboot-reason");
	if (!reason_np) {
		pr_warn("test bootloader stop: no reboot reason node\n");
		return 0;
	}

	ret = bus_register_notifier(&platform_bus_type, &reason_nb);
	if (ret)
		return ret;

	/* A built-in reboot reason driver may have bound before this ran */
	dev = bus_find_device_by_of_node(&platform_bus_type, reason_np);
	if (dev) {
		if (device_is_bound(dev))
			preset_bootloader_reason();
		put_device(dev);
	}
	return 0;
}
late_initcall(test_bootloader_stop_init);
