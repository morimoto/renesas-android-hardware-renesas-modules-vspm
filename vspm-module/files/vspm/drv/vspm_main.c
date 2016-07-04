/*************************************************************************/ /*
 VSPM

 Copyright (C) 2015-2016 Renesas Electronics Corporation

 License        Dual MIT/GPLv2

 The contents of this file are subject to the MIT license as set out below.

 Permission is hereby granted, free of charge, to any person obtaining a copy
 of this software and associated documentation files (the "Software"), to deal
 in the Software without restriction, including without limitation the rights
 to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 copies of the Software, and to permit persons to whom the Software is
 furnished to do so, subject to the following conditions:

 The above copyright notice and this permission notice shall be included in
 all copies or substantial portions of the Software.

 Alternatively, the contents of this file may be used under the terms of
 the GNU General Public License Version 2 ("GPL") in which case the provisions
 of GPL are applicable instead of those above.

 If you wish to allow use of your version of this file only under the terms of
 GPL, and not to allow others to use your version of this file under the terms
 of the MIT license, indicate your decision by deleting the provisions above
 and replace them with the notice and other provisions required by GPL as set
 out in the file called "GPL-COPYING" included in this distribution. If you do
 not delete the provisions above, a recipient may use your version of this file
 under the terms of either the MIT license or GPL.

 This License is also included in this distribution in the file called
 "MIT-COPYING".

 EXCEPT AS OTHERWISE STATED IN A NEGOTIATED AGREEMENT: (A) THE SOFTWARE IS
 PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING
 BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR
 PURPOSE AND NONINFRINGEMENT; AND (B) IN NO EVENT SHALL THE AUTHORS OR
 COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.


 GPLv2:
 If you wish to use this file under the terms of GPL, following terms are
 effective.

 This program is free software; you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation; version 2 of the License.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with this program; if not, write to the Free Software
 Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
*/ /*************************************************************************/

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/slab.h>
#include <linux/io.h>
#include <linux/pm_runtime.h>

#include "vspm_public.h"
#include "vspm_ip_ctrl.h"
#include "vspm_main.h"
#include "vspm_log.h"

#include "vspm_lib_public.h"
#include "vsp_drv_public.h"

struct vspm_drvdata *p_vspm_drvdata;

/******************************************************************************
Function:		vspm_init_driver
Description:	Initialize VSP Manager.
Returns:		R_VSPM_OK/R_VSPM_NG
	return of vspm_init()
	return of vspm_lib_set_mode()
******************************************************************************/
long vspm_init_driver(void **handle, struct vspm_init_t *param)
{
	struct vspm_privdata *priv = 0;
	struct vspm_drvdata *pdrv = p_vspm_drvdata;
	int cnt;

	long ercd;

	down(&pdrv->init_sem);

	/* check parameter */
	if ((handle == NULL) || (param == NULL)) {
		ercd = R_VSPM_NG;
		goto err_exit1;
	}

	/* allocate the private data area of the vspm device file */
	priv = kzalloc(sizeof(*priv), GFP_KERNEL);
	if (!priv) {
		APRINT("could not allocate the private data area of the ");
		APRINT("vspm device file\n");
		ercd = R_VSPM_NG;
		goto err_exit1;
	}

	priv->pdrv = pdrv;

	if (atomic_add_return(1, &pdrv->counter) == 1) {
		/* first time open */
		ercd = vspm_init(pdrv);
		if (ercd) {
			atomic_dec(&pdrv->counter);
			goto err_exit2;
		}
	}

	/* set mode parameter */
	ercd = vspm_lib_set_mode(priv, param);
	if (ercd)
		goto err_exit3;

	*handle = (void *)priv;

	up(&pdrv->init_sem);
	return R_VSPM_OK;

err_exit3:
	cnt = atomic_sub_return(1, &pdrv->counter);
	if (cnt == 0)
		(void)vspm_quit(pdrv);

err_exit2:
	kfree(priv);

err_exit1:
	up(&pdrv->init_sem);
	return ercd;
}
EXPORT_SYMBOL(vspm_init_driver);


/******************************************************************************
Function:		vspm_quit_driver
Description:	Finalize VSP Manager.
Returns:		R_VSPM_OK/R_VSPM_NG
******************************************************************************/
long vspm_quit_driver(void *handle)
{
	struct vspm_privdata *priv = (struct vspm_privdata *)handle;
	struct vspm_drvdata *pdrv = p_vspm_drvdata;
	int cnt;

	long ercd;

	down(&pdrv->init_sem);

	/* check parameter */
	if (priv == NULL)
		goto err_exit1;

	if (priv->pdrv != pdrv)
		goto err_exit1;

	/* clear mode parameter */
	ercd = vspm_lib_set_mode(priv, NULL);
	if (ercd)
		goto err_exit1;

	cnt = atomic_sub_return(1, &pdrv->counter);
	if (cnt == 0) {
		ercd = vspm_quit(pdrv);
		if (ercd)
			goto err_exit2;
	} else if (cnt > 0) {
		ercd = vspm_cancel(priv);
		if (ercd)
			goto err_exit2;
	} else {
		APRINT("already closed\n");
		goto err_exit2;
	}

	up(&pdrv->init_sem);
	priv->pdrv = NULL;
	kfree(priv);

	return R_VSPM_OK;

err_exit2:
	atomic_inc(&pdrv->counter);

err_exit1:
	up(&pdrv->init_sem);
	return R_VSPM_NG;
}
EXPORT_SYMBOL(vspm_quit_driver);


/******************************************************************************
Function:		vspm_entry_job
Description:	Entry of job.
Returns:		R_VSPM_PARAERR
	return of vspm_lib_entry()
******************************************************************************/
long vspm_entry_job(
	void *handle,
	unsigned long *job_id,
	char job_priority,
	struct vspm_job_t *ip_param,
	void *user_data,
	PFN_VSPM_COMPLETE_CALLBACK cb_func)
{
	struct vspm_privdata *priv = (struct vspm_privdata *)handle;
	struct vspm_api_param_entry entry;
	long ercd;

	/* check parameter */
	if (priv == NULL)
		return R_VSPM_PARAERR;

	if (priv->pdrv != p_vspm_drvdata)
		return R_VSPM_PARAERR;

	/* set parameter */
	entry.priv				= priv;
	entry.p_job_id			= job_id;
	entry.job_priority		= job_priority;
	entry.p_ip_par			= ip_param;
	entry.pfn_complete_cb	= cb_func;
	entry.user_data			= user_data;

	/* execute entry */
	ercd = vspm_lib_entry(&entry);
	if (ercd)
		EPRINT("failed to vspm_lib_entry() %ld\n", ercd);

	return ercd;
}
EXPORT_SYMBOL(vspm_entry_job);


/******************************************************************************
Function:		vspm_cancel_job
Description:	Cancel of job.
Returns:		R_VSPM_PARAERR
	return of vspm_lib_queue_cancel()
******************************************************************************/
long vspm_cancel_job(void *handle, unsigned long job_id)
{
	struct vspm_privdata *priv = (struct vspm_privdata *)handle;
	long ercd;

	/* check parameter */
	if (priv == NULL)
		return R_VSPM_PARAERR;

	if (priv->pdrv != p_vspm_drvdata)
		return R_VSPM_PARAERR;

	/* execute cancel */
	ercd = vspm_lib_queue_cancel(priv, job_id);

	return ercd;
}
EXPORT_SYMBOL(vspm_cancel_job);


/******************************************************************************
Function:		vspm_get_status
Description:	Get status of processing.
Returns:		R_VSPM_OK
	return of vspm_lib_get_status()
******************************************************************************/
long vspm_get_status(void *handle, struct vspm_status_t *status)
{
	struct vspm_privdata *priv = (struct vspm_privdata *)handle;
	long ercd;

	/* check parameter */
	if (priv == NULL)
		return R_VSPM_PARAERR;

	if (priv->pdrv != p_vspm_drvdata)
		return R_VSPM_PARAERR;

	if (status == NULL)
		return R_VSPM_PARAERR;

	/* get status */
	ercd = vspm_lib_get_status(priv, status);

	return ercd;
}
EXPORT_SYMBOL(vspm_get_status);

static int vspm_vsp_probe(struct platform_device *pdev)
{
	struct vspm_drvdata *pdrv = p_vspm_drvdata;
	struct device_node *np = pdev->dev.of_node;

	int ch;

	/* get channel */
	of_property_read_u32(np, "renesas,#ch", &ch);
	if (ch < 0) {
		APRINT("Not find define of renesas,#ch.\n");
		return -1;
	}

	if (ch >= VSPM_VSP_IP_MAX) {
		APRINT("Invalid channel!! ch=%d\n", ch);
		return -1;
	}

	if (pdrv->vsp_pdev[ch] != NULL) {
		APRINT("Already registered channel!! ch=%d\n", ch);
		return -1;
	}

	/* set driver data */
	platform_set_drvdata(pdev, pdrv);
	pdrv->vsp_pdev[ch] = pdev;

	/* set runtime PM */
	pm_suspend_ignore_children(&pdev->dev, true);
	pm_runtime_enable(&pdev->dev);

	return 0;
}


static int vspm_vsp_remove(struct platform_device *pdev)
{
	struct vspm_drvdata *pdrv = p_vspm_drvdata;
	struct device_node *np = pdev->dev.of_node;

	int ch;

	/* unset runtime PM */
	pm_runtime_disable(&pdev->dev);

	/* get channel */
	of_property_read_u32(np, "renesas,#ch", &ch);

	/* unset driver data */
	platform_set_drvdata(pdev, NULL);
	pdrv->vsp_pdev[ch] = NULL;

	return 0;
}


static int vspm_fdp_probe(struct platform_device *pdev)
{
	struct vspm_drvdata *pdrv = p_vspm_drvdata;
	struct device_node *np = pdev->dev.of_node;

	int ch;

	/* get channel */
	of_property_read_u32(np, "renesas,#ch", &ch);
	if (ch < 0) {
		APRINT("Not find define of renesas,#ch.\n");
		return -1;
	}

	if (ch >= VSPM_FDP_IP_MAX) {
		APRINT("Invalid channel!! ch=%d\n", ch);
		return -1;
	}

	if (pdrv->fdp_pdev[ch] != NULL) {
		APRINT("Already registered channel!! ch=%d\n", ch);
		return -1;
	}

	/* set driver data */
	platform_set_drvdata(pdev, pdrv);
	pdrv->fdp_pdev[ch] = pdev;

	/* set runtime PM */
	pm_suspend_ignore_children(&pdev->dev, true);
	pm_runtime_enable(&pdev->dev);

	return 0;
}


static int vspm_fdp_remove(struct platform_device *pdev)
{
	struct vspm_drvdata *pdrv = p_vspm_drvdata;
	struct device_node *np = pdev->dev.of_node;

	int ch;

	/* unset runtime PM */
	pm_runtime_disable(&pdev->dev);

	/* get channel */
	of_property_read_u32(np, "renesas,#ch", &ch);

	/* unset driver data */
	platform_set_drvdata(pdev, NULL);
	pdrv->fdp_pdev[ch] = NULL;

	return 0;
}


static int vspm_runtime_nop(struct device *dev)
{
	/* Runtime PM callback shared between ->runtime_suspend()
	 * and ->runtime_resume(). Simply returns success.
	 *
	 * This driver re-initializes all registers after
	 * pm_runtime_get_sync() anyway so there is no need
	 * to save and restore registers here.
	 */
	return 0;
}


static const struct dev_pm_ops vspm_vsp_pm_ops = {
	.runtime_suspend = vspm_runtime_nop,
	.runtime_resume = vspm_runtime_nop,
	.suspend = vspm_runtime_nop,
	.resume = vspm_runtime_nop,
};


static const struct dev_pm_ops vspm_fdp_pm_ops = {
	.runtime_suspend = vspm_runtime_nop,
	.runtime_resume = vspm_runtime_nop,
	.suspend = vspm_runtime_nop,
	.resume = vspm_runtime_nop,
};

static const struct of_device_id vspm_vsp_of_match[] = {
	{ .compatible = "renesas,vspm" },
	{ },
};

static const struct of_device_id vspm_fdp_of_match[] = {
	{ .compatible = "renesas,fdpm" },
	{ },
};

static struct platform_driver vspm_vsp_driver = {
	.driver = {
		.name	= DEVNAME "-vsp",
		.owner	= THIS_MODULE,
		.pm		= &vspm_vsp_pm_ops,
		.of_match_table = vspm_vsp_of_match,
	},
	.probe		= vspm_vsp_probe,
	.remove		= vspm_vsp_remove,
};


static struct platform_driver vspm_fdp_driver = {
	.driver = {
		.name	= DEVNAME "-fdp",
		.owner	= THIS_MODULE,
		.pm		= &vspm_fdp_pm_ops,
		.of_match_table = vspm_fdp_of_match,
	},
	.probe		= vspm_fdp_probe,
	.remove		= vspm_fdp_remove,
};

static int vspm_platform_driver_register(void)
{
	int ercd;

	ercd = platform_driver_register(&vspm_vsp_driver);
	if (ercd) {
		APRINT("could not register a driver for ");
		APRINT("platform-level devices %d\n", ercd);
		return ercd;
	}

	ercd = platform_driver_register(&vspm_fdp_driver);
	if (ercd) {
		APRINT("could not register a driver for ");
		APRINT("platform-level devices %d\n", ercd);
		/* forced quit */
		platform_driver_unregister(&vspm_vsp_driver);
		return ercd;
	}

	return 0;
}


static void vspm_platform_driver_unregister(void)
{
	platform_driver_unregister(&vspm_vsp_driver);
	platform_driver_unregister(&vspm_fdp_driver);
}


static int __init vspm_module_init(void)
{
	struct vspm_drvdata *pdrv = 0;
	int ercd = 0;

	/* allocate vspm driver data area */
	pdrv = kzalloc(sizeof(*pdrv), GFP_KERNEL);
	if (!pdrv) {
		APRINT("could not allocate vspm driver data area\n");
		return -ENOMEM;
	}
	p_vspm_drvdata = pdrv;

	/* register a driver for platform-level devices */
	ercd = vspm_platform_driver_register();
	if (ercd) {
		kfree(pdrv);
		p_vspm_drvdata = NULL;
		return ercd;
	}

	/* initialize open counter */
	atomic_set(&pdrv->counter, 0);

	/* initialize semaphore */
	sema_init(&pdrv->init_sem, 1);/* unlock */
	return 0;
}


static void __exit vspm_module_exit(void)
{
	struct vspm_drvdata *pdrv = p_vspm_drvdata;

	vspm_platform_driver_unregister();

	kfree(pdrv);
	p_vspm_drvdata = NULL;
}

module_init(vspm_module_init);
module_exit(vspm_module_exit);

MODULE_AUTHOR("Renesas Electronics Corporation");
MODULE_LICENSE("Dual MIT/GPL");
