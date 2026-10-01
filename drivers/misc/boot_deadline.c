// SPDX-License-Identifier: GPL-2.0
#include <linux/init.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/moduleparam.h>
#include <linux/panic.h>
#include <linux/printk.h>
#include <linux/timer.h>

static unsigned int timeout = CONFIG_BOOT_DEADLINE_TIMEOUT;
module_param(timeout, uint, 0444);

static struct timer_list boot_deadline_timer;

static void boot_deadline_fire(struct timer_list *t)
{
	panic("boot deadline: %u s after late init", timeout);
}

static int __init boot_deadline_init(void)
{
	if (!timeout)
		return 0;

	timer_setup(&boot_deadline_timer, boot_deadline_fire, 0);
	mod_timer(&boot_deadline_timer, jiffies + timeout * HZ);
	pr_info("boot deadline: panic in %u s\n", timeout);
	return 0;
}
late_initcall(boot_deadline_init);
