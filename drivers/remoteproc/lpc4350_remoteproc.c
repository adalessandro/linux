#include <linux/kernel.h>
#include <linux/clk.h>
#include <linux/reset.h>
#include <linux/module.h>
#include <linux/err.h>
#include <linux/remoteproc.h>
#include <linux/of_device.h>

#include "remoteproc_internal.h"

#define LPC_CREG			0x40043000
#define M0APPMEMMAP			0x404

#define FW_NAME		"m0core-fw.elf"

/* start M0 core */
static int lpc4350_rproc_start(struct rproc *rproc)
{
	struct device *dev = rproc->dev.parent;
	struct reset_control *rst;
	void *bootaddr;

	/* Get reset controller */
	rst = reset_control_get(dev, NULL);
	if (IS_ERR(rst)) {
		dev_err(dev, "reset_get error: %ld\n", PTR_ERR(rst));
		return PTR_ERR(rst);
	}

	/* Translate entry point to physical address */
	bootaddr = rproc_da_to_va(rproc, rproc->bootaddr, 0);

	writel(bootaddr, (void *) (LPC_CREG + M0APPMEMMAP));

	/* Deassert M0 reset */
	reset_control_deassert(rst);

	return 0;
}

/* stop/disable M0 core */
static int lpc4350_rproc_stop(struct rproc *rproc)
{
	struct device *dev = rproc->dev.parent;
	struct reset_control *rst;

	/* Get reset controller */
	rst = reset_control_get(dev, NULL);
	if (IS_ERR(rst)) {
		dev_err(dev, "reset_get error: %ld\n", PTR_ERR(rst));
		return PTR_ERR(rst);
	}

	/* Assert M0 reset */
	reset_control_assert(rst);

	return 0;
}

static struct rproc_ops lpc4350_rproc_ops = {
	.start			= lpc4350_rproc_start,
	.stop			= lpc4350_rproc_stop,
	.kick			= NULL,
};

static int lpc4350_rproc_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct rproc *rproc = NULL;
	struct clk *clk;
	struct reset_control *rst;
	int ret;

	if (!np) {
		dev_err(dev, "Non-DT platform device not supported\n");
		return -ENODEV;
	}

	/* Get reset controller */
	rst = reset_control_get(dev, NULL);
	if (IS_ERR(rst)) {
		dev_err(dev, "reset_get error: %ld\n", PTR_ERR(rst));
		return PTR_ERR(rst);
	}

	/* Ensure M0 is held on reset status */
	ret = reset_control_assert(rst);

	/* Get the clock */
	clk = devm_clk_get(dev, NULL);
	if (IS_ERR(clk)) {
		dev_err(dev, "clk_get error: %ld\n", PTR_ERR(clk));
		return PTR_ERR(clk);
	}

	/* Enable clock */
	ret = clk_prepare_enable(clk);
	if (ret) {
		dev_err(dev, "Unable to enable clock.\n");
		return ret;
	}

	/* Allocate rproc handler */
	rproc = rproc_alloc(dev, pdev->name, &lpc4350_rproc_ops, FW_NAME, 0);
	if (!rproc) {
		dev_err(dev, "Failed to allocate rproc\n");
		return -ENOMEM;
	}

	ret = rproc_add(rproc);
	if (ret) {
		dev_err(dev, "rproc_add failed: %d\n", ret);
		goto free_rproc;
	}


	/* Boot core */
	wait_for_completion(&rproc->firmware_loading_complete);
	if (list_empty(&rproc->rvdevs)) {
		dev_info(dev, "booting the M0 core manually\n");
		ret = rproc_boot(rproc);
		if (ret) {
			dev_err(dev, "rproc_boot failed\n");
			goto del_rproc;
		}
	}

	return 0;

del_rproc:
	rproc_del(rproc);
free_rproc:
	rproc_put(rproc);
	return ret;
}

static int lpc4350_rproc_remove(struct platform_device *pdev)
{
	return 0;
}

static const struct of_device_id lpc4350_rproc_match[] = {
	{ .compatible = "nxp,lpc4350-rproc" },
	{ }
};
MODULE_DEVICE_TABLE(of, lpc4350_rproc_match);

static struct platform_driver lpc4350_rproc_driver = {
	.probe = lpc4350_rproc_probe,
	.remove = lpc4350_rproc_remove,
	.driver = {
		.name = "lpc4350-rproc",
		.of_match_table	= of_match_ptr(lpc4350_rproc_match),
	},
};

static int __init lpc4350_init(void)
{
	int ret;

	ret = platform_driver_register(&lpc4350_rproc_driver);
	if (ret) {
		pr_err("platform driver register failed for \
		       lpc4350_rproc_driver, %d\n", ret);
		return ret;
	}

	return 0;
}
module_init(lpc4350_init);

static void __exit lpc4350_exit(void)
{
	platform_driver_unregister(&lpc4350_rproc_driver);
}
module_exit(lpc4350_exit);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("LPC4350 Remote Processor control driver");
