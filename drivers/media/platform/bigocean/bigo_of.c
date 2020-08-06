// SPDX-License-Identifier: GPL-2.0-only
/*
 * Parses BigOcean device tree node
 *
 * Copyright 2020 Google LLC.
 *
 * Author: Vinay Kalia <vinaykalia@google.com>
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/slab.h>
#include <soc/google/bts.h>

#include "bigo_of.h"

static int bigo_of_get_resource(struct bigo_core *core)
{
	struct platform_device *pdev = to_platform_device(core->dev);
	struct resource *res;
	int rc = 0;

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "bo");
	if (IS_ERR_OR_NULL(res)) {
		rc = PTR_ERR(res);
		pr_err("Failed to find bo register base: %d\n", rc);
		goto err;
	}
	core->base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR_OR_NULL(core->base)) {
		rc = PTR_ERR(core->base);
		if (rc == 0)
			rc = -EIO;
		pr_err("Failed to map bo register base: %d\n", rc);
		core->base = NULL;
		goto err;
	}
	core->regs_size = res->end - res->start + 1;

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "ssmt_bo_pid");
	if (IS_ERR_OR_NULL(res)) {
		rc = PTR_ERR(res);
		pr_err("Failed to find ssmt_bo register base: %d\n", rc);
		goto err;
	}
	core->slc.ssmt_pid_base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR_OR_NULL(core->slc.ssmt_pid_base)) {
		pr_warn("Failed to map ssmt_bo register base: %d\n",
			PTR_ERR(core->slc.ssmt_pid_base));
		core->slc.ssmt_pid_base = NULL;
	}

	core->irq = platform_get_irq(pdev, 0);

	if (core->irq < 0) {
		rc = core->irq;
		pr_err("platform_get_irq failed: %d\n", rc);
		goto err;
	}

err:
	return rc;
}

static void bigo_of_remove_opp_table(struct bigo_core *core)
{
	struct bigo_opp *opp, *tmp;

	list_for_each_entry_safe(opp, tmp, &core->pm.opps, list) {
		list_del(&opp->list);
		kfree(opp);
	}
}

static int bigo_of_parse_opp_table(struct bigo_core *core)
{
	int rc = 0;
	struct device_node *np;
	struct bigo_opp *opp;

	struct device_node *opp_np =
		of_parse_phandle(core->dev->of_node, "bigo-opp-table", 0);
	if (!opp_np) {
		return -ENOENT;
		goto err_add_table;
	}
	for_each_available_child_of_node(opp_np, np) {
		opp = kmalloc(sizeof(*opp), GFP_KERNEL);
		if (!opp) {
			rc = -ENOMEM;
			goto err_entry;
		}
		rc = of_property_read_u32(np, "load-pps", &opp->load_pps);
		if (rc < 0) {
			kfree(opp);
			goto err_entry;
		}
		core->pm.max_load = opp->load_pps;
		rc = of_property_read_u32(np, "freq-khz", &opp->freq_khz);
		if (rc < 0) {
			kfree(opp);
			goto err_entry;
		}
		if (!core->pm.min_freq)
			core->pm.min_freq = opp->freq_khz;

		list_add_tail(&opp->list, &core->pm.opps);
	}
	return rc;
err_entry:
	bigo_of_remove_opp_table(core);
err_add_table:
	return rc;
}

int bigo_of_dt_parse(struct bigo_core *core)
{
	int rc = 0;

	rc = bigo_of_get_resource(core);
	if (rc < 0) {
		pr_err("failed to get respource: %d\n", rc);
		goto err_get_res;
	}

	rc = bigo_of_parse_opp_table(core);
	if (rc < 0) {
		pr_err("failed to parse bigocean OPP table\n");
		goto err_parse_table;
	}

	core->pm.bwindex = bts_get_bwindex("bo");
	if (core->pm.bwindex < 0) {
		rc = core->pm.bwindex;
		goto err_bwindex;
	}

	return rc;
err_bwindex:
	bigo_of_remove_opp_table(core);
err_parse_table:
err_get_res:
	return rc;
}

void bigo_of_dt_release(struct bigo_core *core)
{
	if (!core)
		return;
	bigo_of_remove_opp_table(core);
}
