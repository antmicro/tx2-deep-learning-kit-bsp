/*
 * Copyright (C) 2016 NVIDIA CORPORATION.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/clk.h>
#include <linux/host1x.h>
#include <linux/iommu.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/reset.h>

#ifdef CONFIG_DRM_TEGRA_DOWNSTREAM
#include <linux/clk/tegra.h>
#include <linux/platform/tegra/mc.h>
#else
#include <soc/tegra/pmc.h>
#endif

#include "tsec.h"
#ifdef CONFIG_ARCH_TEGRA_18x_SOC
#include "tsec_t186.h"
#endif

#define TSEC_OS_START_OFFSET 256
#define TSEC_AUTOSUSPEND_DELAY 500

static int tsec_boot(struct tsec *tsec);

static int tsec_runtime_resume(struct device *dev)
{
	struct tsec *tsec = dev_get_drvdata(dev);
	int err;

	err = clk_prepare_enable(tsec->clk);

	err = tsec_boot(tsec);
	if (err < 0)
		clk_disable_unprepare(tsec->clk);

	return err;
}

static int tsec_runtime_suspend(struct device *dev)
{
	struct tsec *tsec = dev_get_drvdata(dev);

	clk_disable_unprepare(tsec->clk);
	if (tsec->rst)
		reset_control_assert(tsec->rst);

	tsec->booted = false;

	return 0;
}

static int tsec_boot(struct tsec *tsec)
{
	int err = 0;
	struct tegra_drm_client *client = &tsec->client;

	if (tsec->booted)
		return 0;

	if (!tsec->falcon.firmware.valid) {
		err = falcon_read_firmware(&tsec->falcon,
					   tsec->config->ucode_name);
		if (err < 0)
			return err;
	}

	/* ensure that the engine is in sane state */
	if (tsec->rst) {
		reset_control_assert(tsec->rst);
		usleep_range(10, 100);
		reset_control_deassert(tsec->rst);
	}

	if (client->ops->load_regs)
		client->ops->load_regs(client);

	err = falcon_boot(&tsec->falcon);
	if (err < 0)
		return err;

	/*TODO: read kfuse */
	err = falcon_wait_idle(&tsec->falcon);
	if (err < 0) {
		dev_err(tsec->dev,
			"failed to set application ID and FCE base\n");
		return err;
	}

	tsec->booted = true;

	return 0;
}

static void *tsec_falcon_alloc(struct falcon *falcon, size_t size,
			       dma_addr_t *iova)
{
#ifdef CONFIG_DRM_TEGRA_DOWNSTREAM
	return dma_alloc_writecombine(falcon->dev, size, iova,
					   GFP_KERNEL | __GFP_NOWARN);
#else
	struct tegra_drm *tegra = falcon->data;

	return tegra_drm_alloc(tegra, size, iova);
#endif
}

static void tsec_falcon_free(struct falcon *falcon, size_t size,
			    dma_addr_t iova, void *va)
{
#ifdef CONFIG_DRM_TEGRA_DOWNSTREAM
	dma_free_writecombine(falcon->dev, size, va, iova);
#else
	struct tegra_drm *tegra = falcon->data;

	return tegra_drm_free(tegra, size, va, iova);
#endif
}

static const struct falcon_ops tsec_falcon_ops = {
	.alloc = tsec_falcon_alloc,
	.free = tsec_falcon_free
};

static int tsec_init(struct host1x_client *client)
{
	struct tegra_drm_client *drm = host1x_to_drm_client(client);
	struct drm_device *dev = dev_get_drvdata(client->parent);
	struct tegra_drm *tegra = dev->dev_private;
	struct tsec *tsec = to_tsec(drm);
	int err;

	if (tegra->domain) {
		err = iommu_attach_device(tegra->domain, tsec->dev);
		if (err < 0) {
			dev_err(tsec->dev, "failed to attach to domain: %d\n",
				err);
			return err;
		}

		tsec->domain = tegra->domain;
	}

	tsec->falcon.firmware.os_start_offset = TSEC_OS_START_OFFSET;
	tsec->falcon.dev = tsec->dev;
	tsec->falcon.regs = tsec->regs;
	tsec->falcon.data = tegra;
	tsec->falcon.ops = &tsec_falcon_ops;
	err = falcon_init(&tsec->falcon);
	if (err < 0)
		goto detach_device;

	tsec->channel = host1x_channel_request(client->dev);
	if (!tsec->channel) {
		err = -ENOMEM;
		goto exit_falcon;
	}

	client->syncpts[0] = host1x_syncpt_request(client->dev, 0);
	if (!client->syncpts[0]) {
		err = -ENOMEM;
		goto free_channel;
	}

	err = tegra_drm_register_client(tegra, drm);
	if (err < 0)
		goto free_syncpt;

	return 0;

free_syncpt:
	host1x_syncpt_free(client->syncpts[0]);
free_channel:
	host1x_channel_free(tsec->channel);
exit_falcon:
	falcon_exit(&tsec->falcon);
detach_device:
	if (tegra->domain)
		iommu_detach_device(tegra->domain, tsec->dev);

	return err;
}

static int tsec_exit(struct host1x_client *client)
{
	struct tegra_drm_client *drm = host1x_to_drm_client(client);
	struct drm_device *dev = dev_get_drvdata(client->parent);
	struct tegra_drm *tegra = dev->dev_private;
	struct tsec *tsec = to_tsec(drm);
	int err;

	err = tegra_drm_unregister_client(tegra, drm);
	if (err < 0)
		return err;

	host1x_syncpt_free(client->syncpts[0]);
	host1x_channel_free(tsec->channel);

	if (tsec->booted) {
		if (tsec->rst) {
			reset_control_assert(tsec->rst);
			usleep_range(10, 100);
			reset_control_deassert(tsec->rst);
		}
	}

	falcon_exit(&tsec->falcon);

	if (tsec->domain) {
		iommu_detach_device(tsec->domain, tsec->dev);
		tsec->domain = NULL;
	}

	return 0;
}

static const struct host1x_client_ops tsec_client_ops = {
	.init = tsec_init,
	.exit = tsec_exit,
};

int tsec_open_channel(struct tegra_drm_client *client,
			    struct tegra_drm_context *context)
{
	struct tsec *tsec = to_tsec(client);

	context->channel = host1x_channel_get(tsec->channel);
	if (!context->channel)
		return -ENOMEM;

	return 0;
}

void tsec_close_channel(struct tegra_drm_context *context)
{
	host1x_channel_put(context->channel);
}

static const struct tegra_drm_client_ops tsec_ops = {
	.open_channel = tsec_open_channel,
	.close_channel = tsec_close_channel,
	.submit = tegra_drm_submit,
};

static const struct tsec_config tsec_t210_config = {
	.ucode_name = "tegra21x/nvhost_tsec.fw",
	.drm_client_ops = &tsec_ops,
	.class_id = HOST1X_CLASS_TSEC,
};

static const struct tsec_config tsecb_t210_config = {
	.ucode_name = "tegra21x/nvhost_tsec.fw",
	.drm_client_ops = &tsec_ops,
	.class_id = HOST1X_CLASS_TSECB,
};

static const struct of_device_id tsec_match[] = {
	{ .name = "tsec",
		.compatible = "nvidia,tegra210-tsec",
		.data = &tsec_t210_config },
	{ .name = "tsecb",
		.compatible = "nvidia,tegra210-tsec",
		.data = &tsecb_t210_config },
#ifdef CONFIG_ARCH_TEGRA_18x_SOC
	{ .name = "tsec",
		.compatible = "nvidia,tegra186-tsec",
		.data = &tsec_t186_config },
	{ .name = "tsecb",
		.compatible = "nvidia,tegra186-tsec",
		.data = &tsecb_t186_config },
#endif
	{ },
};

static int tsec_probe(struct platform_device *pdev)
{
	struct tsec_config *tsec_config = NULL;
	struct device *dev = &pdev->dev;
	struct host1x_syncpt **syncpts;
	struct resource *regs;
	const struct of_device_id *match;
	struct tsec *tsec;
	int err;

	match = of_match_device(tsec_match, dev);
	tsec_config = (struct tsec_config *)match->data;

	tsec = devm_kzalloc(dev, sizeof(*tsec), GFP_KERNEL);
	if (!tsec)
		return -ENOMEM;

	syncpts = devm_kzalloc(dev, sizeof(*syncpts), GFP_KERNEL);
	if (!syncpts)
		return -ENOMEM;

	regs = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!regs) {
		dev_err(dev, "failed to get registers\n");
		return -ENXIO;
	}

	tsec->regs = devm_ioremap_resource(dev, regs);
	if (IS_ERR(tsec->regs))
		return PTR_ERR(tsec->regs);

	tsec->clk = devm_clk_get(dev, NULL);
	if (IS_ERR(tsec->clk)) {
		dev_err(dev, "failed to get clock\n");
		return PTR_ERR(tsec->clk);
	}

	tsec->rst = devm_reset_control_get(dev, NULL);
	if (IS_ERR(tsec->rst)) {
		dev_err(dev, "cannot get reset\n");
		tsec->rst = NULL;
	}

	platform_set_drvdata(pdev, tsec);

	INIT_LIST_HEAD(&tsec->client.base.list);
	tsec->client.base.ops = &tsec_client_ops;
	tsec->client.base.dev = dev;
	tsec->client.base.class = tsec_config->class_id;
	tsec->client.base.syncpts = syncpts;
	tsec->client.base.num_syncpts = 1;
	tsec->dev = dev;
	tsec->config = tsec_config;

	INIT_LIST_HEAD(&tsec->client.list);
	tsec->client.ops = tsec->config->drm_client_ops;

	err = host1x_client_register(&tsec->client.base);
	if (err < 0) {
		dev_err(dev, "failed to register host1x client: %d\n", err);
		platform_set_drvdata(pdev, NULL);
		return err;
	}

	pm_runtime_set_autosuspend_delay(dev, TSEC_AUTOSUSPEND_DELAY);
	pm_runtime_use_autosuspend(dev);
	pm_runtime_enable(dev);
	if (!pm_runtime_enabled(dev)) {
		err = tsec_runtime_resume(dev);
		if (err < 0)
			goto unregister_client;
	}

	/* TODO: map carveout */
	dev_info(dev, "initialized");

	return 0;

unregister_client:
	host1x_client_unregister(&tsec->client.base);

	return err;
}

static int tsec_remove(struct platform_device *pdev)
{
	struct tsec *tsec = platform_get_drvdata(pdev);
	int err;

	err = host1x_client_unregister(&tsec->client.base);
	if (err < 0) {
		dev_err(&pdev->dev, "failed to unregister host1x client: %d\n",
			err);
		return err;
	}

	if (pm_runtime_enabled(&pdev->dev))
		pm_runtime_disable(&pdev->dev);
	else
		tsec_runtime_suspend(&pdev->dev);

	return 0;
}

static const struct dev_pm_ops tsec_pm_ops = {
	SET_RUNTIME_PM_OPS(tsec_runtime_suspend, tsec_runtime_resume, NULL)
};

struct platform_driver tegra_tsec_driver = {
	.driver = {
		.name = "tegra-tsec",
		.of_match_table = tsec_match,
		.pm = &tsec_pm_ops
	},
	.probe = tsec_probe,
	.remove = tsec_remove,
};
