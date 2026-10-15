// SPDX-License-Identifier: GPL-2.0-only
/*
 * Vendor services start and hold remote processors through /dev/subsys_<name> and read them in
 * /sys/bus/msm_subsys: give that interface to the remote processors listed in the device tree
 */

#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/idr.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/notifier.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/remoteproc.h>
#include <linux/remoteproc/qcom_rproc.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/sysfs.h>

enum msm_subsys_state {
	SUBSYS_OFFLINING,
	SUBSYS_OFFLINE,
	SUBSYS_ONLINE,
};

static const char * const subsys_states[] = {
	[SUBSYS_OFFLINING] = "OFFLINING",
	[SUBSYS_OFFLINE] = "OFFLINE",
	[SUBSYS_ONLINE] = "ONLINE",
};

struct msm_subsys {
	struct device dev;
	struct cdev cdev;
	struct rproc *rproc;
	const char *name;
	struct notifier_block ssr_nb;
	void *ssr_cookie;
	int state;
	int id;
};

struct msm_subsys_group {
	dev_t devt;
	int nr;
	int count;
	struct msm_subsys *subsys[] __counted_by(nr);
};

static DEFINE_IDA(subsys_ida);

#define to_msm_subsys(d) container_of(d, struct msm_subsys, dev)

static ssize_t name_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%s\n", to_msm_subsys(dev)->name);
}
static DEVICE_ATTR_RO(name);

static ssize_t state_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%s\n", subsys_states[READ_ONCE(to_msm_subsys(dev)->state)]);
}
static DEVICE_ATTR_RO(state);

static bool restart_level_is(const char *buf, size_t count, const char *level)
{
	size_t len = strlen(level);

	if (count && buf[count - 1] == '\n')
		count--;
	return count == len && !strncasecmp(buf, level, len);
}

/* SYSTEM: a crash of the processor restarts the phone, RELATED: only the processor restarts */
static ssize_t restart_level_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%s\n", to_msm_subsys(dev)->rproc->recovery_disabled ?
			  "SYSTEM" : "RELATED");
}

static ssize_t restart_level_store(struct device *dev, struct device_attribute *attr,
				   const char *buf, size_t count)
{
	struct rproc *rproc = to_msm_subsys(dev)->rproc;
	bool disabled;

	if (restart_level_is(buf, count, "SYSTEM"))
		disabled = true;
	else if (restart_level_is(buf, count, "RELATED"))
		disabled = false;
	else
		return -EINVAL;

	mutex_lock(&rproc->lock);
	rproc->recovery_disabled = disabled;
	mutex_unlock(&rproc->lock);

	return count;
}
static DEVICE_ATTR_RW(restart_level);

static struct attribute *msm_subsys_attrs[] = {
	&dev_attr_name.attr,
	&dev_attr_state.attr,
	&dev_attr_restart_level.attr,
	NULL,
};
ATTRIBUTE_GROUPS(msm_subsys);

static const struct bus_type msm_subsys_bus = {
	.name = "msm_subsys",
	.dev_groups = msm_subsys_groups,
};

/* ueventd names the node after this device: /dev/subsys_<name> */
static const struct class subsys_class = {
	.name = "subsys",
};

static int msm_subsys_open(struct inode *inode, struct file *file)
{
	struct msm_subsys *subsys = container_of(inode->i_cdev, struct msm_subsys, cdev);

	return rproc_boot(subsys->rproc);
}

static int msm_subsys_release(struct inode *inode, struct file *file)
{
	struct msm_subsys *subsys = container_of(inode->i_cdev, struct msm_subsys, cdev);
	int ret;

	ret = rproc_shutdown(subsys->rproc);
	if (ret)
		dev_err(&subsys->dev, "%s: dropping the vote failed: %d\n", subsys->name, ret);
	return ret;
}

static const struct file_operations msm_subsys_fops = {
	.owner = THIS_MODULE,
	.open = msm_subsys_open,
	.release = msm_subsys_release,
};

static void msm_subsys_set_state(struct msm_subsys *subsys, int state)
{
	if (READ_ONCE(subsys->state) == state)
		return;
	WRITE_ONCE(subsys->state, state);
	sysfs_notify(&subsys->dev.kobj, NULL, "state");
}

static int msm_subsys_ssr_notify(struct notifier_block *nb, unsigned long event, void *data)
{
	struct msm_subsys *subsys = container_of(nb, struct msm_subsys, ssr_nb);
	struct qcom_ssr_notify_data *notify = data;

	switch (event) {
	case QCOM_SSR_AFTER_POWERUP:
		msm_subsys_set_state(subsys, SUBSYS_ONLINE);
		break;
	case QCOM_SSR_BEFORE_SHUTDOWN:
		/* A normal stop is OFFLINING first, a crash goes to OFFLINE once it is down */
		if (!notify->crashed)
			msm_subsys_set_state(subsys, SUBSYS_OFFLINING);
		break;
	case QCOM_SSR_AFTER_SHUTDOWN:
		msm_subsys_set_state(subsys, SUBSYS_OFFLINE);
		break;
	default:
		return NOTIFY_DONE;
	}
	return NOTIFY_OK;
}

static void msm_subsys_dev_release(struct device *dev)
{
	struct msm_subsys *subsys = to_msm_subsys(dev);

	rproc_put(subsys->rproc);
	ida_free(&subsys_ida, subsys->id);
	kfree_const(subsys->name);
	kfree(subsys);
}

static struct rproc *msm_subsys_get_rproc(struct device *dev, struct device_node *np)
{
	struct rproc *rproc;
	phandle rproc_ph;

	if (of_property_read_u32(np, "qcom,rproc-handle", &rproc_ph))
		return ERR_PTR(dev_err_probe(dev, -EINVAL, "%pOF: no qcom,rproc-handle\n", np));

	rproc = rproc_get_by_phandle(rproc_ph);
	if (!rproc)
		return ERR_PTR(dev_err_probe(dev, -EPROBE_DEFER, "%pOF: no processor yet\n", np));
	return rproc;
}

static struct msm_subsys *msm_subsys_add(struct device *dev, struct device_node *np, dev_t devt)
{
	struct msm_subsys *subsys;
	struct device *chardev;
	const char *ssr_name;
	struct rproc *rproc;
	int ret;

	if (of_property_read_string(np, "qcom,ssr-name", &ssr_name))
		return ERR_PTR(dev_err_probe(dev, -EINVAL, "%pOF: no qcom,ssr-name\n", np));

	rproc = msm_subsys_get_rproc(dev, np);
	if (IS_ERR(rproc))
		return ERR_CAST(rproc);

	subsys = kzalloc(sizeof(*subsys), GFP_KERNEL);
	if (!subsys) {
		ret = -ENOMEM;
		goto put_rproc;
	}

	subsys->name = kstrdup_const(np->name, GFP_KERNEL);
	if (!subsys->name) {
		ret = -ENOMEM;
		goto free_subsys;
	}

	ret = ida_alloc(&subsys_ida, GFP_KERNEL);
	if (ret < 0)
		goto free_name;
	subsys->id = ret;
	subsys->rproc = rproc;

	/* From here on the device release frees everything above */
	device_initialize(&subsys->dev);
	subsys->dev.bus = &msm_subsys_bus;
	subsys->dev.parent = rproc->dev.parent;
	subsys->dev.release = msm_subsys_dev_release;
	ret = dev_set_name(&subsys->dev, "subsys%d", subsys->id);
	if (ret)
		goto put_dev;

	subsys->ssr_nb.notifier_call = msm_subsys_ssr_notify;
	subsys->ssr_cookie = qcom_register_ssr_notifier(ssr_name, &subsys->ssr_nb);
	if (IS_ERR(subsys->ssr_cookie)) {
		ret = PTR_ERR(subsys->ssr_cookie);
		goto put_dev;
	}

	/* The notifier runs under the processor lock, so the state read here is never stale */
	mutex_lock(&rproc->lock);
	subsys->state = rproc->state == RPROC_RUNNING || rproc->state == RPROC_ATTACHED ?
			SUBSYS_ONLINE : SUBSYS_OFFLINE;
	mutex_unlock(&rproc->lock);

	ret = device_add(&subsys->dev);
	if (ret)
		goto unregister_ssr;

	/* An open file keeps the device, and with it the processor reference, until its release */
	cdev_init(&subsys->cdev, &msm_subsys_fops);
	cdev_set_parent(&subsys->cdev, &subsys->dev.kobj);
	ret = cdev_add(&subsys->cdev, devt, 1);
	if (ret)
		goto del_dev;

	chardev = device_create(&subsys_class, rproc->dev.parent, devt, NULL, "subsys_%s",
				subsys->name);
	if (IS_ERR(chardev)) {
		ret = PTR_ERR(chardev);
		goto del_cdev;
	}

	return subsys;

del_cdev:
	cdev_del(&subsys->cdev);
del_dev:
	qcom_unregister_ssr_notifier(subsys->ssr_cookie, &subsys->ssr_nb);
	device_del(&subsys->dev);
	put_device(&subsys->dev);
	return ERR_PTR(ret);
unregister_ssr:
	qcom_unregister_ssr_notifier(subsys->ssr_cookie, &subsys->ssr_nb);
put_dev:
	put_device(&subsys->dev);
	return ERR_PTR(ret);

free_name:
	kfree_const(subsys->name);
free_subsys:
	kfree(subsys);
put_rproc:
	rproc_put(rproc);
	return ERR_PTR(ret);
}

static void msm_subsys_del(struct msm_subsys *subsys)
{
	device_destroy(&subsys_class, subsys->cdev.dev);
	cdev_del(&subsys->cdev);
	qcom_unregister_ssr_notifier(subsys->ssr_cookie, &subsys->ssr_nb);
	device_unregister(&subsys->dev);
}

static void msm_subsys_del_all(struct msm_subsys_group *group)
{
	while (group->count)
		msm_subsys_del(group->subsys[--group->count]);
	unregister_chrdev_region(group->devt, group->nr);
}

static int msm_subsys_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct msm_subsys_group *group;
	struct msm_subsys *subsys;
	struct rproc *rproc;
	int nr, ret;

	nr = of_get_available_child_count(dev->of_node);
	if (!nr)
		return dev_err_probe(dev, -ENODEV, "no remote processors listed\n");

	/* Wait for every listed processor first, so no entry appears and goes away again */
	for_each_available_child_of_node_scoped(dev->of_node, np) {
		rproc = msm_subsys_get_rproc(dev, np);
		if (IS_ERR(rproc))
			return PTR_ERR(rproc);
		rproc_put(rproc);
	}

	group = devm_kzalloc(dev, struct_size(group, subsys, nr), GFP_KERNEL);
	if (!group)
		return -ENOMEM;
	group->nr = nr;

	ret = alloc_chrdev_region(&group->devt, 0, nr, "subsys");
	if (ret)
		return ret;

	for_each_available_child_of_node_scoped(dev->of_node, np) {
		subsys = msm_subsys_add(dev, np, group->devt + group->count);
		if (IS_ERR(subsys)) {
			msm_subsys_del_all(group);
			return PTR_ERR(subsys);
		}
		group->subsys[group->count++] = subsys;
	}

	platform_set_drvdata(pdev, group);
	return 0;
}

static void msm_subsys_remove(struct platform_device *pdev)
{
	msm_subsys_del_all(platform_get_drvdata(pdev));
}

static const struct of_device_id msm_subsys_of_match[] = {
	{ .compatible = "qcom,msm-subsys" },
	{ }
};
MODULE_DEVICE_TABLE(of, msm_subsys_of_match);

static struct platform_driver msm_subsys_driver = {
	.probe = msm_subsys_probe,
	.remove = msm_subsys_remove,
	.driver = {
		.name = "qcom-msm-subsys",
		.of_match_table = msm_subsys_of_match,
	},
};

static int __init msm_subsys_init(void)
{
	int ret;

	ret = bus_register(&msm_subsys_bus);
	if (ret)
		return ret;

	ret = class_register(&subsys_class);
	if (ret)
		goto unregister_bus;

	ret = platform_driver_register(&msm_subsys_driver);
	if (ret)
		goto unregister_class;

	return 0;

unregister_class:
	class_unregister(&subsys_class);
unregister_bus:
	bus_unregister(&msm_subsys_bus);
	return ret;
}
module_init(msm_subsys_init);

static void __exit msm_subsys_exit(void)
{
	platform_driver_unregister(&msm_subsys_driver);
	class_unregister(&subsys_class);
	bus_unregister(&msm_subsys_bus);
}
module_exit(msm_subsys_exit);

MODULE_DESCRIPTION("msm_subsys interface for Qualcomm remote processors");
MODULE_LICENSE("GPL");
