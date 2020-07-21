// SPDX-License-Identifier: GPL-2.0-only
/*
 * gs101_tmu_v2.c - Samsung GS101 TMU (Thermal Management Unit)
 *
 *  Copyright (C) 2019 Samsung Electronics
 *  Hyeonseong Gil <hs.gil@samsung.com>
 *
 */

#include <linux/delay.h>
#include <linux/io.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>
#include <linux/cpufreq.h>
#include <linux/suspend.h>
#include <soc/google/exynos_pm_qos.h>
#include <linux/threads.h>
#include <linux/thermal.h>
#include <linux/slab.h>
#include <linux/debugfs.h>
#include <uapi/linux/sched/types.h>
#include <soc/google/tmu.h>
#include <soc/google/ect_parser.h>
#if IS_ENABLED(CONFIG_EXYNOS_MCINFO)
#include <soc/google/exynos-mcinfo.h>
#endif

#include "gs101_tmu.h"
#include "../thermal_core.h"
#if IS_ENABLED(CONFIG_EXYNOS_ACPM_THERMAL)
#include "exynos_acpm_tmu.h"
#endif
#include <soc/google/exynos-cpuhp.h>

#define EXYNOS_GPU_TMU_GRP_ID		(3)

struct kthread_worker *hotplug_worker;

static struct acpm_tmu_cap cap;
static unsigned int num_of_devices, suspended_count;

/* list of multiple instance for each thermal sensor */
static LIST_HEAD(dtm_dev_list);

static void gs101_report_trigger(struct gs101_tmu_data *p)
{
	struct thermal_zone_device *tz = p->tzd;

	if (!tz) {
		pr_err("No thermal zone device defined\n");
		return;
	}

	thermal_zone_device_update(tz, THERMAL_EVENT_UNSPECIFIED);
}

static int gs101_tmu_initialize(struct platform_device *pdev)
{
	struct gs101_tmu_data *data = platform_get_drvdata(pdev);
	struct thermal_zone_device *tz = data->tzd;
	enum thermal_trip_type type;
	int i, temp, ret = 0;
	unsigned char threshold[8] = {0, };
	unsigned char hysteresis[8] = {0, };
	unsigned char inten = 0;

	mutex_lock(&data->lock);

	for (i = (of_thermal_get_ntrips(tz) - 1); i >= 0; i--) {
		ret = tz->ops->get_trip_type(tz, i, &type);
		if (ret) {
			dev_err(&pdev->dev, "Failed to get trip type(%d)\n", i);
			goto out;
		}

		if (type == THERMAL_TRIP_PASSIVE)
			continue;

		ret = tz->ops->get_trip_temp(tz, i, &temp);
		if (ret) {
			dev_err(&pdev->dev, "Failed to get trip temp(%d)\n", i);
			goto out;
		}

		threshold[i] = (unsigned char)(temp / MCELSIUS);
		inten |= (1 << i);

		ret = tz->ops->get_trip_hyst(tz, i, &temp);
		if (ret) {
			dev_err(&pdev->dev, "Failed to get trip hyst(%d)\n", i);
			goto out;
		}

		hysteresis[i] = (unsigned char)(temp / MCELSIUS);
	}
	exynos_acpm_tmu_set_threshold(data->id, threshold);
	exynos_acpm_tmu_set_hysteresis(data->id, hysteresis);
	exynos_acpm_tmu_set_interrupt_enable(data->id, inten);

out:
	mutex_unlock(&data->lock);

	return ret;
}

static void gs101_tmu_control(struct platform_device *pdev, bool on)
{
	struct gs101_tmu_data *data = platform_get_drvdata(pdev);

	mutex_lock(&data->lock);
	exynos_acpm_tmu_tz_control(data->id, on);
	data->enabled = on;
	mutex_unlock(&data->lock);
}

#define MCINFO_LOG_THRESHOLD	(4)

static int gs101_get_temp(void *p, int *temp)
{
	struct gs101_tmu_data *data = p;
#if IS_ENABLED(CONFIG_EXYNOS_MCINFO)
	unsigned int mcinfo_count;
	unsigned int mcinfo_result[4] = {0, 0, 0, 0};
	unsigned int mcinfo_logging = 0;
	unsigned int mcinfo_temp = 0;
	unsigned int i;
#endif
	int acpm_temp = 0, stat = 0;

	if (!data || !data->enabled)
		return -EINVAL;

	mutex_lock(&data->lock);

	exynos_acpm_tmu_set_read_temp(data->id, &acpm_temp, &stat);

	*temp = acpm_temp * MCELSIUS;

	if (data->limited_frequency) {
		if (!data->limited) {
			if (*temp >= data->limited_threshold) {
				exynos_pm_qos_update_request(&data->thermal_limit_request,
							     data->limited_frequency);
				data->limited = true;
			}
		} else {
			if (*temp < data->limited_threshold_release) {
				exynos_pm_qos_update_request(&data->thermal_limit_request,
							     INT_MAX);
				data->limited = false;
			}
		}
	}

	data->temperature = *temp / 1000;

	if (data->hotplug_enable)
		kthread_queue_work(hotplug_worker, &data->hotplug_work);

	mutex_unlock(&data->lock);

#if IS_ENABLED(CONFIG_EXYNOS_MCINFO)
	if (data->id == 0) {
		mcinfo_count = get_mcinfo_base_count();
		get_refresh_rate(mcinfo_result);

		for (i = 0; i < mcinfo_count; i++) {
			mcinfo_temp |= (mcinfo_result[i] & 0xf) << (8 * i);

			if (mcinfo_result[i] >= MCINFO_LOG_THRESHOLD)
				mcinfo_logging = 1;
		}

		if (mcinfo_logging == 1)
			dbg_snapshot_thermal(NULL, mcinfo_temp, "MCINFO", 0);
	}
#endif
	return 0;
}

static int gs101_get_trend(void *p, int trip, enum thermal_trend *trend)
{
	struct gs101_tmu_data *data = p;
	struct thermal_zone_device *tz = data->tzd;
	int trip_temp, ret = 0;

	if (!tz)
		return ret;

	ret = tz->ops->get_trip_temp(tz, trip, &trip_temp);
	if (ret < 0)
		return ret;

	if (tz->temperature >= trip_temp)
		*trend = THERMAL_TREND_RAISE_FULL;
	else
		*trend = THERMAL_TREND_DROP_FULL;

	return 0;
}

#if IS_ENABLED(CONFIG_THERMAL_EMULATION)
static int gs101_tmu_set_emulation(void *drv_data, int temp)
{
	struct gs101_tmu_data *data = drv_data;
	int ret = -EINVAL;
	unsigned char emul_temp;

	if (temp && temp < MCELSIUS)
		goto out;

	mutex_lock(&data->lock);
	emul_temp = (unsigned char)(temp / MCELSIUS);
	exynos_acpm_tmu_set_emul_temp(data->id, emul_temp);
	mutex_unlock(&data->lock);
	return 0;
out:
	return ret;
}
#else
static int gs101_tmu_set_emulation(void *drv_data, int temp)
{
	return -EINVAL;
}
#endif /* CONFIG_THERMAL_EMULATION */

static void gs101_tmu_work(struct kthread_work *work)
{
	struct gs101_tmu_data *data = container_of(work,
			struct gs101_tmu_data, irq_work);
	struct thermal_zone_device *tz = data->tzd;

	gs101_report_trigger(data);
	mutex_lock(&data->lock);

	exynos_acpm_tmu_clear_tz_irq(data->id);

	dev_dbg_ratelimited(&tz->device, "IRQ handled: tz:%s, temp:%d\n",
			    tz->type, tz->temperature);

	mutex_unlock(&data->lock);
	enable_irq(data->irq);
}

static irqreturn_t gs101_tmu_irq(int irq, void *id)
{
	struct gs101_tmu_data *data = id;

	disable_irq_nosync(irq);
	kthread_queue_work(&data->irq_worker, &data->irq_work);

	return IRQ_HANDLED;
}

static void gs101_throttle_cpu_hotplug(struct kthread_work *work)
{
	struct gs101_tmu_data *data = container_of(work,
						   struct gs101_tmu_data, hotplug_work);
	struct cpumask mask;

	mutex_lock(&data->lock);

	if (data->is_cpu_hotplugged_out) {
		if (data->temperature < data->hotplug_in_threshold) {
			/*
			 * If current temperature is lower than low threshold,
			 * call cluster1_cores_hotplug(false) for hotplugged out cpus.
			 */
			exynos_cpuhp_request("DTM", *cpu_possible_mask);
			data->is_cpu_hotplugged_out = false;
		}
	} else {
		if (data->temperature >= data->hotplug_out_threshold) {
			/*
			 * If current temperature is higher than high threshold,
			 * call cluster1_cores_hotplug(true) to hold temperature down.
			 */
			data->is_cpu_hotplugged_out = true;
			cpumask_and(&mask, cpu_possible_mask, &cpu_topology[0].core_sibling);
			exynos_cpuhp_request("DTM", mask);
		}
	}

	mutex_unlock(&data->lock);
}

static const struct of_device_id gs101_tmu_match[] = {
	{ .compatible = "samsung,gs101-tmu-v2", },
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(of, gs101_tmu_match);

static int gs101_tmu_irq_work_init(struct platform_device *pdev)
{
	struct gs101_tmu_data *data = platform_get_drvdata(pdev);
	struct cpumask mask;
	struct sched_param param = { .sched_priority = MAX_RT_PRIO / 4 - 1 };
	struct task_struct *thread;
	int ret = 0;

	kthread_init_worker(&data->irq_worker);
	thread = kthread_create(kthread_worker_fn, &data->irq_worker,
				"thermal_irq_%s", data->tmu_name);
	if (IS_ERR(thread)) {
		dev_err(&pdev->dev, "failed to create thermal thread: %ld\n",
			PTR_ERR(thread));
		return PTR_ERR(thread);
	}

	cpumask_and(&mask, cpu_possible_mask, &cpu_topology[0].core_sibling);
	set_cpus_allowed_ptr(thread, &mask);

	ret = sched_setscheduler_nocheck(thread, SCHED_FIFO, &param);
	if (ret) {
		kthread_stop(thread);
		dev_warn(&pdev->dev, "thermal failed to set SCHED_FIFO\n");
		return ret;
	}

	kthread_init_work(&data->irq_work, gs101_tmu_work);

	wake_up_process(thread);

	if (data->hotplug_enable) {
		exynos_cpuhp_register("DTM", *cpu_possible_mask);
		kthread_init_work(&data->hotplug_work, gs101_throttle_cpu_hotplug);

		if (!hotplug_worker) {
			hotplug_worker = kzalloc(sizeof(*hotplug_worker), GFP_KERNEL);
			if (!hotplug_worker)
				return -ENOMEM;

			kthread_init_worker(hotplug_worker);
			thread = kthread_create(kthread_worker_fn, hotplug_worker,
						"thermal_hotplug_kworker");
			kthread_bind(thread, 0);
			sched_setscheduler_nocheck(thread, SCHED_FIFO, &param);
			wake_up_process(thread);
		}
	}

	return ret;
}

static int gs101_map_dt_data(struct platform_device *pdev)
{
	struct gs101_tmu_data *data = platform_get_drvdata(pdev);
	struct resource res;
	const char *tmu_name;

	if (!data || !pdev->dev.of_node)
		return -ENODEV;

	data->np = pdev->dev.of_node;

	if (of_property_read_u32(pdev->dev.of_node, "id", &data->id)) {
		dev_err(&pdev->dev, "failed to get TMU ID\n");
		return -ENODEV;
	}

	data->irq = irq_of_parse_and_map(pdev->dev.of_node, 0);
	if (data->irq <= 0) {
		dev_err(&pdev->dev, "failed to get IRQ\n");
		return -ENODEV;
	}

	if (of_address_to_resource(pdev->dev.of_node, 0, &res)) {
		dev_err(&pdev->dev, "failed to get Resource 0\n");
		return -ENODEV;
	}

	data->base = devm_ioremap(&pdev->dev, res.start, resource_size(&res));
	if (!data->base) {
		dev_err(&pdev->dev, "Failed to ioremap memory\n");
		return -EADDRNOTAVAIL;
	}

	if (of_property_read_string(pdev->dev.of_node, "tmu_name", &tmu_name))
		dev_err(&pdev->dev, "failed to get tmu_name\n");
	else
		strncpy(data->tmu_name, tmu_name, THERMAL_NAME_LENGTH);

	data->hotplug_enable = of_property_read_bool(pdev->dev.of_node, "hotplug_enable");
	if (data->hotplug_enable) {
		dev_info(&pdev->dev, "thermal zone use hotplug function\n");
		of_property_read_u32(pdev->dev.of_node, "hotplug_in_threshold",
				     &data->hotplug_in_threshold);
		if (!data->hotplug_in_threshold)
			dev_err(&pdev->dev, "No input hotplug_in_threshold\n");

		of_property_read_u32(pdev->dev.of_node, "hotplug_out_threshold",
				     &data->hotplug_out_threshold);
		if (!data->hotplug_out_threshold)
			dev_err(&pdev->dev, "No input hotplug_out_threshold\n");
	}

	return 0;
}

static const struct thermal_zone_of_device_ops gs101_sensor_ops = {
	.get_temp = gs101_get_temp,
	.set_emul_temp = gs101_tmu_set_emulation,
	.get_trend = gs101_get_trend,
};

static ssize_t
hotplug_out_temp_show(struct device *dev, struct device_attribute *devattr,
		      char *buf)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct gs101_tmu_data *data = platform_get_drvdata(pdev);

	return snprintf(buf, PAGE_SIZE, "%d\n", data->hotplug_out_threshold);
}

static ssize_t
hotplug_out_temp_store(struct device *dev, struct device_attribute *devattr,
		       const char *buf, size_t count)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct gs101_tmu_data *data = platform_get_drvdata(pdev);
	int hotplug_out = 0;

	mutex_lock(&data->lock);

	if (kstrtos32(buf, 10, &hotplug_out)) {
		mutex_unlock(&data->lock);
		return -EINVAL;
	}

	data->hotplug_out_threshold = hotplug_out;

	mutex_unlock(&data->lock);

	return count;
}

static ssize_t
hotplug_in_temp_show(struct device *dev, struct device_attribute *devattr,
		     char *buf)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct gs101_tmu_data *data = platform_get_drvdata(pdev);

	return snprintf(buf, PAGE_SIZE, "%d\n", data->hotplug_in_threshold);
}

static ssize_t
hotplug_in_temp_store(struct device *dev, struct device_attribute *devattr,
		      const char *buf, size_t count)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct gs101_tmu_data *data = platform_get_drvdata(pdev);
	int hotplug_in = 0;

	mutex_lock(&data->lock);

	if (kstrtos32(buf, 10, &hotplug_in)) {
		mutex_unlock(&data->lock);
		return -EINVAL;
	}

	data->hotplug_in_threshold = hotplug_in;

	mutex_unlock(&data->lock);

	return count;
}

static DEVICE_ATTR_RW(hotplug_out_temp);
static DEVICE_ATTR_RW(hotplug_in_temp);

static struct attribute *gs101_tmu_attrs[] = {
	&dev_attr_hotplug_out_temp.attr,
	&dev_attr_hotplug_in_temp.attr,
	NULL,
};

static const struct attribute_group gs101_tmu_attr_group = {
	.attrs = gs101_tmu_attrs,
};

#if IS_ENABLED(CONFIG_EXYNOS_ACPM_THERMAL)
static void exynos_acpm_tmu_test_cp_call(bool mode)
{
	struct gs101_tmu_data *devnode;

	if (mode) {
		list_for_each_entry(devnode, &dtm_dev_list, node) {
			disable_irq(devnode->irq);
		}
		exynos_acpm_tmu_set_cp_call();
	} else {
		exynos_acpm_tmu_set_resume();
		list_for_each_entry(devnode, &dtm_dev_list, node) {
			enable_irq(devnode->irq);
		}
	}
}

static int emul_call_get(void *data, unsigned long long *val)
{
	*val = exynos_acpm_tmu_is_test_mode();

	return 0;
}

static int emul_call_set(void *data, unsigned long long val)
{
	int status = exynos_acpm_tmu_is_test_mode();

	if ((val == 0 || val == 1) && val != status) {
		exynos_acpm_tmu_set_test_mode(val);
		exynos_acpm_tmu_test_cp_call(val);
	}

	return 0;
}
DEFINE_SIMPLE_ATTRIBUTE(emul_call_fops, emul_call_get, emul_call_set, "%llu\n");

static int log_print_set(void *data, unsigned long long val)
{
	if (val == 0 || val == 1)
		exynos_acpm_tmu_log(val);

	return 0;
}
DEFINE_SIMPLE_ATTRIBUTE(log_print_fops, NULL, log_print_set, "%llu\n");

static ssize_t ipc_dump1_read(struct file *file, char __user *user_buf,
			      size_t count, loff_t *ppos)
{
	union {
		unsigned int dump[2];
		unsigned char val[8];
	} data;
	char buf[48];
	ssize_t ret;

	exynos_acpm_tmu_ipc_dump(0, data.dump);

	ret = snprintf(buf, sizeof(buf), "%3d %3d %3d %3d %3d %3d %3d\n",
		       data.val[1], data.val[2], data.val[3],
		       data.val[4], data.val[5], data.val[6], data.val[7]);
	if (ret < 0)
		return ret;

	return simple_read_from_buffer(user_buf, count, ppos, buf, ret);
}

static ssize_t ipc_dump2_read(struct file *file, char __user *user_buf,
			      size_t count, loff_t *ppos)
{
	union {
		unsigned int dump[2];
		unsigned char val[8];
	} data;
	char buf[48];
	ssize_t ret;

	exynos_acpm_tmu_ipc_dump(EXYNOS_GPU_TMU_GRP_ID, data.dump);

	ret = snprintf(buf, sizeof(buf), "%3d %3d %3d %3d %3d %3d %3d\n",
		       data.val[1], data.val[2], data.val[3],
		       data.val[4], data.val[5], data.val[6], data.val[7]);
	if (ret < 0)
		return ret;

	return simple_read_from_buffer(user_buf, count, ppos, buf, ret);
}

static const struct file_operations ipc_dump1_fops = {
	.open = simple_open,
	.read = ipc_dump1_read,
	.llseek = default_llseek,
};

static const struct file_operations ipc_dump2_fops = {
	.open = simple_open,
	.read = ipc_dump2_read,
	.llseek = default_llseek,
};
#endif

static struct dentry *debugfs_root;

static int gs101_thermal_create_debugfs(void)
{
	debugfs_root = debugfs_create_dir("gs101-thermal", NULL);
	if (!debugfs_root) {
		pr_err("Failed to create gs101 thermal debugfs\n");
		return 0;
	}

#if IS_ENABLED(CONFIG_EXYNOS_ACPM_THERMAL)
	debugfs_create_file("emul_call", 0644, debugfs_root, NULL, &emul_call_fops);
	debugfs_create_file("log_print", 0644, debugfs_root, NULL, &log_print_fops);
	debugfs_create_file("ipc_dump1", 0644, debugfs_root, NULL, &ipc_dump1_fops);
	debugfs_create_file("ipc_dump2", 0644, debugfs_root, NULL, &ipc_dump2_fops);
#endif
	return 0;
}

#define PARAM_NAME_LENGTH	25
#define FRAC_BITS 10	/* FRAC_BITS should be same with power_allocator */

#if IS_ENABLED(CONFIG_ECT)
static int gs101_tmu_ect_get_param(struct ect_pidtm_block *pidtm_block, char *name)
{
	int i;
	int param_value = -1;

	for (i = 0; i < pidtm_block->num_of_parameter; i++) {
		if (!strncasecmp(pidtm_block->param_name_list[i], name, PARAM_NAME_LENGTH)) {
			param_value = pidtm_block->param_value_list[i];
			break;
		}
	}

	return param_value;
}

static int gs101_tmu_parse_ect(struct gs101_tmu_data *data)
{
	struct thermal_zone_device *tz = data->tzd;
	int ntrips = 0;

	if (!tz)
		return -EINVAL;

	if (strncasecmp(tz->tzp->governor_name, "power_allocator",
			THERMAL_NAME_LENGTH)) {
		/* if governor is not power_allocator */
		void *thermal_block;
		struct ect_ap_thermal_function *function;
		int i, temperature;
		int hotplug_threshold_temp = 0, hotplug_flag = 0;
		unsigned int freq;

		thermal_block = ect_get_block(BLOCK_AP_THERMAL);
		if (!thermal_block) {
			pr_err("Failed to get thermal block");
			return -EINVAL;
		}

		pr_info("%s %d thermal zone_name = %s\n", __func__, __LINE__, tz->type);

		function = ect_ap_thermal_get_function(thermal_block, tz->type);
		if (!function) {
			pr_err("Failed to get thermal block %s", tz->type);
			return -EINVAL;
		}

		ntrips = of_thermal_get_ntrips(tz);
		pr_info("Trip count parsed from ECT : %d, ntrips: %d, zone : %s",
			function->num_of_range, ntrips, tz->type);

		for (i = 0; i < function->num_of_range; ++i) {
			temperature = function->range_list[i].lower_bound_temperature;
			freq = function->range_list[i].max_frequency;

			tz->ops->set_trip_temp(tz, i, temperature  * MCELSIUS);

			pr_info("Parsed From ECT : [%d] Temperature : %d, frequency : %u\n",
				i, temperature, freq);

			if (function->range_list[i].flag != hotplug_flag) {
				if (function->range_list[i].flag != hotplug_flag) {
					hotplug_threshold_temp = temperature;
					hotplug_flag = function->range_list[i].flag;
					data->hotplug_out_threshold = temperature;

					if (i) {
						struct ect_ap_thermal_range range;
						unsigned int temperature;

						range = function->range_list[i - 1];
						temperature = range.lower_bound_temperature;
						data->hotplug_in_threshold = temperature;
					}

					pr_info("[ECT]hotplug_threshold : %d\n",
						hotplug_threshold_temp);
					pr_info("[ECT]hotplug_in_threshold : %d\n",
						data->hotplug_in_threshold);
					pr_info("[ECT]hotplug_out_threshold : %d\n",
						data->hotplug_out_threshold);
				}
			}

			if (hotplug_threshold_temp != 0)
				data->hotplug_enable = true;
			else
				data->hotplug_enable = false;
		}
	} else {
		void *block;
		struct ect_pidtm_block *pidtm_block;
		int i, temperature, value;
		int hotplug_out_threshold = 0, hotplug_in_threshold = 0, limited_frequency = 0;
		int limited_threshold = 0, limited_threshold_release = 0;

		block = ect_get_block(BLOCK_PIDTM);
		if (!block) {
			pr_err("Failed to get PIDTM block");
			return -EINVAL;
		}

		pr_info("%s %d thermal zone_name = %s\n", __func__, __LINE__, tz->type);

		pidtm_block = ect_pidtm_get_block(block, tz->type);
		if (!pidtm_block) {
			pr_err("Failed to get PIDTM block %s", tz->type);
			return -EINVAL;
		}

		ntrips = of_thermal_get_ntrips(tz);
		pr_info("Trip count parsed from ECT : %d, ntrips: %d, zone : %s",
			pidtm_block->num_of_temperature, ntrips, tz->type);

		for (i = 0; i < pidtm_block->num_of_temperature; ++i) {
			temperature = pidtm_block->temperature_list[i];

			tz->ops->set_trip_temp(tz, i, temperature  * MCELSIUS);

			pr_info("Parsed From ECT : [%d] Temperature : %d\n", i, temperature);
		}

		value = gs101_tmu_ect_get_param(pidtm_block, "k_po");
		if (value != -1) {
			pr_info("Parse from ECT k_po: %d\n", value);
			tz->tzp->k_po = value << FRAC_BITS;
		} else {
			pr_err("Fail to parse k_po parameter\n");
		}

		value = gs101_tmu_ect_get_param(pidtm_block, "k_pu");
		if (value != -1) {
			pr_info("Parse from ECT k_pu: %d\n", value);
			tz->tzp->k_pu = value << FRAC_BITS;
		} else {
			pr_err("Fail to parse k_pu parameter\n");
		}

		value = gs101_tmu_ect_get_param(pidtm_block, "k_i");
		if (value != -1) {
			pr_info("Parse from ECT k_i: %d\n", value);
			tz->tzp->k_i = value << FRAC_BITS;
		} else {
			pr_err("Fail to parse k_i parameter\n");
		}

		value = gs101_tmu_ect_get_param(pidtm_block, "i_max");
		if (value != -1) {
			pr_info("Parse from ECT i_max: %d\n", value);
			tz->tzp->integral_max = value;
		} else {
			pr_err("Fail to parse i_max parameter\n");
		}

		value = gs101_tmu_ect_get_param(pidtm_block, "integral_cutoff");
		if (value != -1) {
			pr_info("Parse from ECT integral_cutoff: %d\n", value);
			tz->tzp->integral_cutoff = value;
		} else {
			pr_err("Fail to parse integral_cutoff parameter\n");
		}

		value = gs101_tmu_ect_get_param(pidtm_block, "p_control_t");
		if (value != -1) {
			pr_info("Parse from ECT p_control_t: %d\n", value);
			tz->tzp->sustainable_power = value;
		} else {
			pr_err("Fail to parse p_control_t parameter\n");
		}

		value = gs101_tmu_ect_get_param(pidtm_block, "hotplug_out_threshold");
		if (value != -1) {
			pr_info("Parse from ECT hotplug_out_threshold: %d\n", value);
			hotplug_out_threshold = value;
		}

		value = gs101_tmu_ect_get_param(pidtm_block, "hotplug_in_threshold");
		if (value != -1) {
			pr_info("Parse from ECT hotplug_in_threshold: %d\n", value);
			hotplug_in_threshold = value;
		}

		value = gs101_tmu_ect_get_param(pidtm_block, "limited_frequency");
		if (value != -1) {
			pr_info("Parse from ECT limited_frequency: %d\n", value);
			limited_frequency = value;
		}

		value = gs101_tmu_ect_get_param(pidtm_block, "limited_threshold");
		if (value != -1) {
			pr_info("Parse from ECT limited_threshold: %d\n", value);
			limited_threshold = value * MCELSIUS;
			tz->ops->set_trip_temp(tz, 3, temperature  * MCELSIUS);
			data->limited_threshold = value;
		}

		value = gs101_tmu_ect_get_param(pidtm_block, "limited_threshold_release");
		if (value != -1) {
			pr_info("Parse from ECT limited_threshold_release: %d\n", value);
			limited_threshold_release = value * MCELSIUS;
			data->limited_threshold_release = value;
		}

		if (hotplug_out_threshold != 0 && hotplug_in_threshold != 0) {
			data->hotplug_out_threshold = hotplug_out_threshold;
			data->hotplug_in_threshold = hotplug_in_threshold;
			data->hotplug_enable = true;
		} else {
			data->hotplug_enable = false;
		}

		if (limited_frequency) {
			data->limited_frequency = limited_frequency;
			data->limited = false;
		}
	}
	return 0;
};
#endif

#if IS_ENABLED(CONFIG_MALI_DEBUG_KERNEL_SYSFS)
struct gs101_tmu_data *gpu_thermal_data;
#endif

static int gs101_tmu_probe(struct platform_device *pdev)
{
	struct gs101_tmu_data *data;
	int ret;

	data = devm_kzalloc(&pdev->dev, sizeof(struct gs101_tmu_data),
			    GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	platform_set_drvdata(pdev, data);
	mutex_init(&data->lock);

	ret = gs101_map_dt_data(pdev);
	if (ret)
		goto err_sensor;


	if (list_empty(&dtm_dev_list)) {
#if IS_ENABLED(CONFIG_EXYNOS_ACPM_THERMAL)
		exynos_acpm_tmu_init();
		exynos_acpm_tmu_set_init(&cap);
#endif
	}

	data->tzd = thermal_zone_of_sensor_register(&pdev->dev, 0, data,
						    &gs101_sensor_ops);
	if (IS_ERR(data->tzd)) {
		ret = PTR_ERR(data->tzd);
		dev_err(&pdev->dev, "Failed to register sensor: %d\n", ret);
		goto err_sensor;
	}

#if IS_ENABLED(CONFIG_ECT)
	if (!of_property_read_bool(pdev->dev.of_node, "ect_nouse"))
		gs101_tmu_parse_ect(data);

	if (data->limited_frequency) {
		exynos_pm_qos_add_request(&data->thermal_limit_request,
					  PM_QOS_CLUSTER2_FREQ_MAX,
					  PM_QOS_CLUSTER2_FREQ_MAX_DEFAULT_VALUE);
	}
#endif

	ret = gs101_tmu_initialize(pdev);
	if (ret) {
		dev_err(&pdev->dev, "Failed to initialize TMU\n");
		goto err_thermal;
	}

	ret = devm_request_irq(&pdev->dev, data->irq, gs101_tmu_irq,
			       IRQF_SHARED, dev_name(&pdev->dev), data);
	if (ret) {
		dev_err(&pdev->dev, "Failed to request irq: %d\n", data->irq);
		goto err_thermal;
	}

	ret = gs101_tmu_irq_work_init(pdev);
	if (ret) {
		dev_err(&pdev->dev, "cannot gs101 interrupt work initialize\n");
		goto err_thermal;
	}

	gs101_tmu_control(pdev, true);

	ret = sysfs_create_group(&pdev->dev.kobj, &gs101_tmu_attr_group);
	if (ret)
		dev_err(&pdev->dev, "cannot create gs101 tmu attr group");

	mutex_lock(&data->lock);
	list_add_tail(&data->node, &dtm_dev_list);
	num_of_devices++;
	mutex_unlock(&data->lock);


	if (!IS_ERR(data->tzd))
		data->tzd->ops->set_mode(data->tzd, THERMAL_DEVICE_ENABLED);

	if (list_is_singular(&dtm_dev_list))
		gs101_thermal_create_debugfs();

#if IS_ENABLED(CONFIG_MALI_DEBUG_KERNEL_SYSFS)
	if (data->id == EXYNOS_GPU_TMU_GRP_ID)
		gpu_thermal_data = data;
#endif

	return 0;

err_thermal:
	thermal_zone_of_sensor_unregister(&pdev->dev, data->tzd);
err_sensor:
	return ret;
}

static int gs101_tmu_remove(struct platform_device *pdev)
{
	struct gs101_tmu_data *data = platform_get_drvdata(pdev);
	struct thermal_zone_device *tzd = data->tzd;
	struct gs101_tmu_data *devnode;

	thermal_zone_of_sensor_unregister(&pdev->dev, tzd);
	gs101_tmu_control(pdev, false);

	mutex_lock(&data->lock);
	list_for_each_entry(devnode, &dtm_dev_list, node) {
		if (devnode->id == data->id) {
			list_del(&devnode->node);
			num_of_devices--;
			break;
		}
	}
	mutex_unlock(&data->lock);

	return 0;
}

#if IS_ENABLED(CONFIG_PM_SLEEP)
static int gs101_tmu_suspend(struct device *dev)
{
#if IS_ENABLED(CONFIG_EXYNOS_ACPM_THERMAL)
	struct platform_device *pdev = to_platform_device(dev);
	struct gs101_tmu_data *data = platform_get_drvdata(pdev);

	suspended_count++;
	disable_irq(data->irq);

	if (data->hotplug_enable)
		kthread_flush_work(&data->hotplug_work);
	kthread_flush_work(&data->irq_work);

	gs101_tmu_control(pdev, false);
	if (suspended_count == num_of_devices) {
		exynos_acpm_tmu_set_suspend(false);
		pr_info("%s: TMU suspend\n", __func__);
	}
#endif
	return 0;
}

static int gs101_tmu_resume(struct device *dev)
{
	struct platform_device *pdev = to_platform_device(dev);
#if IS_ENABLED(CONFIG_EXYNOS_ACPM_THERMAL)
	struct gs101_tmu_data *data = platform_get_drvdata(pdev);
	struct cpumask mask;
	int temp, stat;

	if (suspended_count == num_of_devices)
		exynos_acpm_tmu_set_resume();

	gs101_tmu_control(pdev, true);

	exynos_acpm_tmu_set_read_temp(data->id, &temp, &stat);

	pr_info("%s: thermal zone %d temp %d stat %d\n",
		__func__, data->tzd->id, temp, stat);

	enable_irq(data->irq);
	suspended_count--;

	cpumask_and(&mask, cpu_possible_mask, &cpu_topology[0].core_sibling);
	set_cpus_allowed_ptr(data->irq_worker.task, &mask);

	if (!suspended_count)
		pr_info("%s: TMU resume complete\n", __func__);
#endif

	return 0;
}

static SIMPLE_DEV_PM_OPS(gs101_tmu_pm,
			 gs101_tmu_suspend, gs101_tmu_resume);
#define EXYNOS_TMU_PM	(&gs101_tmu_pm)
#else
#define EXYNOS_TMU_PM	NULL
#endif

static struct platform_driver gs101_tmu_driver = {
	.driver = {
		.name   = "gs101-tmu",
		.pm     = EXYNOS_TMU_PM,
		.of_match_table = gs101_tmu_match,
		.suppress_bind_attrs = true,
	},
	.probe = gs101_tmu_probe,
	.remove	= gs101_tmu_remove,
};

module_platform_driver(gs101_tmu_driver);

MODULE_DESCRIPTION("GS101 TMU Driver");
MODULE_AUTHOR("Hyeonseong Gil <hs.gil@samsung.com>");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:gs101-tmu");
