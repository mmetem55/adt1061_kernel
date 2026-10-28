/*
 *Copyright (C) 2018 Spreadtrum Communications Inc.
 *
 *This software is licensed under the terms of the GNU General Public
 *License version 2, as published by the Free Software Foundation, and
 *may be copied, distributed, and modified under those terms.
 *
 *This program is distributed in the hope that it will be useful,
 *but WITHOUT ANY WARRANTY; without even the implied warranty of
 *MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *GNU General Public License for more details.
 */


#include <drm/drm_atomic_helper.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_drv.h>
#include <drm/drm_file.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_ioctl.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_vblank.h>
#include <linux/component.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_graph.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>

#include "sprd_drm.h"
#include "sprd_drm_gsp.h"
#include "sprd_gem.h"
#include <uapi/drm/sprd_drm_gsp.h>

#define DRIVER_NAME	"sprd"
#define DRIVER_DESC	"Spreadtrum SoCs' DRM Driver"
#define DRIVER_DATE	"20180501"
#define DRIVER_MAJOR	1
#define DRIVER_MINOR	0

#define SPRD_FENCE_WAIT_TIMEOUT 3000 /* ms */

static bool cali_mode;

/*
 * 2026 GKI 6.12 port note: __setup() expands to nothing when the code is
 * built as a loadable module (MODULE is defined) - cmdline "androidboot.
 * mode=" parsing only ever applies to code built into vmlinux, not DDK
 * modules. That leaves boot_mode_check() genuinely unreferenced in this
 * build, which trips -Werror,-Wunused-function. __maybe_unused silences
 * that without touching the __setup() call itself, so the parsing is
 * still there (and still runs) the day this ever gets built in instead.
 */
static int __maybe_unused boot_mode_check(char *str)
{
	if (str != NULL && !strncmp(str, "cali", strlen("cali")))
		cali_mode = true;
	else
		cali_mode = false;
	return 0;
}
__setup("androidboot.mode=", boot_mode_check);

/**
 * sprd_atomic_wait_for_fences - wait for fences stashed in plane state
 * @dev: DRM device
 * @state: atomic state object with old state structures
 * @pre_swap: If true, do an interruptible wait, and @state is the new state.
 * Otherwise @state is the old state.
 *
 * For implicit sync, driver should fish the exclusive fence out from the
 * incoming fb's and stash it in the drm_plane_state.  This is called after
 * drm_atomic_helper_swap_state() so it uses the current plane state (and
 * just uses the atomic state to find the changed planes)
 *
 * Note that @pre_swap is needed since the point where we block for fences moves
 * around depending upon whether an atomic commit is blocking or
 * non-blocking. For non-blocking commit all waiting needs to happen after
 * drm_atomic_helper_swap_state() is called, but for blocking commits we want
 * to wait **before** we do anything that can't be easily rolled back. That is
 * before we call drm_atomic_helper_swap_state().
 *
 * Returns zero if success or < 0 if dma_fence_wait_timeout() fails.
 */
int sprd_atomic_wait_for_fences(struct drm_device *dev,
				      struct drm_atomic_state *state,
				      bool pre_swap)
{
	struct drm_plane *plane;
	struct drm_plane_state *new_plane_state;
	int i, ret;

	for_each_new_plane_in_state(state, plane, new_plane_state, i) {
		if (!new_plane_state->fence)
			continue;

		WARN_ON(!new_plane_state->fb);

		/*
		 * If waiting for fences pre-swap (ie: nonblock), userspace can
		 * still interrupt the operation. Instead of blocking until the
		 * timer expires, make the wait interruptible.
		 */
		ret = dma_fence_wait_timeout(new_plane_state->fence,
				pre_swap,
				msecs_to_jiffies(SPRD_FENCE_WAIT_TIMEOUT));
		if (ret == 0) {
			DRM_ERROR("wait fence timed out, index:%d,\n", i);
			return -EBUSY;
		} else if (ret < 0) {
			DRM_ERROR("wait fence failed, index:%d, ret:%d.\n",
				i, ret);
			return ret;
		}

		dma_fence_put(new_plane_state->fence);
		new_plane_state->fence = NULL;
	}

	return 0;
}

static void sprd_commit_tail(struct drm_atomic_state *old_state)
{
	struct drm_device *dev = old_state->dev;
	const struct drm_mode_config_helper_funcs *funcs;

	funcs = dev->mode_config.helper_private;

	sprd_atomic_wait_for_fences(dev, old_state, false);

	drm_atomic_helper_wait_for_dependencies(old_state);

	if (funcs && funcs->atomic_commit_tail)
		funcs->atomic_commit_tail(old_state);
	else
		drm_atomic_helper_commit_tail(old_state);

	drm_atomic_helper_commit_cleanup_done(old_state);

	drm_atomic_state_put(old_state);
}

static void sprd_commit_work(struct work_struct *work)
{
	struct drm_atomic_state *state = container_of(work,
						      struct drm_atomic_state,
						      commit_work);
	sprd_commit_tail(state);
}

static int sprd_stall_checks(struct drm_crtc *crtc, bool nonblock)
{
	struct drm_crtc_commit *commit, *stall_commit = NULL;
	bool completed = true;
	int i;
	long ret = 0;

	spin_lock(&crtc->commit_lock);
	i = 0;
	list_for_each_entry(commit, &crtc->commit_list, commit_entry) {
		if (i == 0) {
			completed = try_wait_for_completion(&commit->flip_done);
			/* Userspace is not allowed to get ahead of the previous
			 * commit with nonblocking ones.
			 */
			if (!completed && nonblock)
				DRM_DEBUG("EBUSY\n");
		} else if (i == 1) {
			stall_commit = commit;
			drm_crtc_commit_get(stall_commit);
			break;
		}

		i++;
	}
	spin_unlock(&crtc->commit_lock);

	if (!stall_commit)
		return 0;

	/* We don't want to let commits get ahead of cleanup work too much,
	 * stalling on 2nd previous commit means triple-buffer won't ever stall.
	 */
	ret = wait_for_completion_interruptible_timeout(&stall_commit->cleanup_done,
							10*HZ);
	if (ret == 0)
		DRM_ERROR("[CRTC:%d:%s] cleanup_done timed out\n",
			  crtc->base.id, crtc->name);

	drm_crtc_commit_put(stall_commit);

	return ret < 0 ? ret : 0;
}

static void sprd_release_crtc_commit(struct completion *completion)
{
	struct drm_crtc_commit *commit = container_of(completion,
						      typeof(*commit),
						      flip_done);

	drm_crtc_commit_put(commit);
}


static int sprd_atomic_setup_commit(struct drm_atomic_state *state,
				   bool nonblock)
{
	struct drm_crtc *crtc;
	struct drm_crtc_state *old_crtc_state, *new_crtc_state;
	struct drm_crtc_commit *commit;
	int i, ret;

	for_each_oldnew_crtc_in_state(state, crtc, old_crtc_state, new_crtc_state, i) {
		commit = kzalloc(sizeof(*commit), GFP_KERNEL);
		if (!commit)
			return -ENOMEM;

		init_completion(&commit->flip_done);
		init_completion(&commit->hw_done);
		init_completion(&commit->cleanup_done);
		INIT_LIST_HEAD(&commit->commit_entry);
		kref_init(&commit->ref);
		commit->crtc = crtc;

		state->crtcs[i].commit = commit;

		ret = sprd_stall_checks(crtc, nonblock);
		if (ret)
			return ret;

		/* Drivers only send out events when at least either current or
		 * new CRTC state is active. Complete right away if everything
		 * stays off.
		 */
		if (!old_crtc_state->active && !new_crtc_state->active) {
			complete_all(&commit->flip_done);
			continue;
		}

		/* Legacy cursor updates are fully unsynced. */
		if (state->legacy_cursor_update) {
			complete_all(&commit->flip_done);
			continue;
		}

		if (!new_crtc_state->event) {
			commit->event = kzalloc(sizeof(*commit->event),
						GFP_KERNEL);
			if (!commit->event)
				return -ENOMEM;

			new_crtc_state->event = commit->event;
		}

		new_crtc_state->event->base.completion = &commit->flip_done;
		new_crtc_state->event->base.completion_release =
			sprd_release_crtc_commit;
		drm_crtc_commit_get(commit);
	}

	return 0;
}


/**
 * sprd_atomic_commit - commit validated state object
 * @dev: DRM device
 * @state: the driver state object
 * @nonblock: whether nonblocking behavior is requested.
 *
 * This function commits a with drm_atomic_helper_check() pre-validated state
 * object. This can still fail when e.g. the framebuffer reservation fails. This
 * function implements nonblocking commits, using
 * drm_atomic_helper_setup_commit() and related functions.
 *
 * Committing the actual hardware state is done through the
 * &drm_mode_config_helper_funcs.atomic_commit_tail callback, or it's default
 * implementation drm_atomic_helper_commit_tail().
 *
 * RETURNS:
 * Zero for success or -errno.
 */
int sprd_atomic_commit(struct drm_device *dev,
			     struct drm_atomic_state *state,
			     bool nonblock)
{
	int ret;

	if (state->async_update) {
		ret = drm_atomic_helper_prepare_planes(dev, state);
		if (ret)
			return ret;

		drm_atomic_helper_async_commit(dev, state);
		drm_atomic_helper_cleanup_planes(dev, state);

		return 0;
	}

	ret = sprd_atomic_setup_commit(state, nonblock);
	if (ret)
		return ret;

	INIT_WORK(&state->commit_work, sprd_commit_work);

	ret = drm_atomic_helper_prepare_planes(dev, state);
	if (ret)
		return ret;

	if (!nonblock) {
		ret = sprd_atomic_wait_for_fences(dev, state, true);
		if (ret)
			goto err;
	}

	/*
	 * This is the point of no return - everything below never fails except
	 * when the hw goes bonghits. Which means we can commit the new state on
	 * the software side now.
	 */

	ret = drm_atomic_helper_swap_state(state, true);
	if (ret)
		goto err;

	/*
	 * Everything below can be run asynchronously without the need to grab
	 * any modeset locks at all under one condition: It must be guaranteed
	 * that the asynchronous work has either been cancelled (if the driver
	 * supports it, which at least requires that the framebuffers get
	 * cleaned up with drm_atomic_helper_cleanup_planes()) or completed
	 * before the new state gets committed on the software side with
	 * drm_atomic_helper_swap_state().
	 *
	 * This scheme allows new atomic state updates to be prepared and
	 * checked in parallel to the asynchronous completion of the previous
	 * update. Which is important since compositors need to figure out the
	 * composition of the next frame right after having submitted the
	 * current layout.
	 *
	 * NOTE: Commit work has multiple phases, first hardware commit, then
	 * cleanup. We want them to overlap, hence need system_unbound_wq to
	 * make sure work items don't artifically stall on each another.
	 */

	drm_atomic_state_get(state);
	if (nonblock)
		queue_work(system_unbound_wq, &state->commit_work);
	else
		sprd_commit_tail(state);

	return 0;

err:
	drm_atomic_helper_cleanup_planes(dev, state);
	return ret;
}

static void sprd_atomic_commit_tail(struct drm_atomic_state *old_state)
{
	struct drm_device *dev = old_state->dev;

	drm_atomic_helper_commit_modeset_disables(dev, old_state);

	drm_atomic_helper_commit_modeset_enables(dev, old_state);

	drm_atomic_helper_commit_planes(dev, old_state,
					DRM_PLANE_COMMIT_ACTIVE_ONLY);

	drm_atomic_helper_commit_hw_done(old_state);

	drm_atomic_helper_cleanup_planes(dev, old_state);
}

static const struct drm_mode_config_helper_funcs sprd_drm_mode_config_helper = {
	.atomic_commit_tail = sprd_atomic_commit_tail,
};

static const struct drm_mode_config_funcs sprd_drm_mode_config_funcs = {
	.fb_create = drm_gem_fb_create,
	.atomic_check = drm_atomic_helper_check,
	/*
	 * 6.12 port: the vendor 4.14 commit path (sprd_atomic_commit() +
	 * sprd_atomic_setup_commit()) stored the CRTC commit in the removed
	 * state->crtcs[].commit slot, so drm_atomic_helper_commit_cleanup_done()
	 * found no commit and warned on every commit. The stock helper does the
	 * same job (setup, prepare, swap, tail via our atomic_commit_tail).
	 */
	.atomic_commit = drm_atomic_helper_commit,
};

static void sprd_drm_mode_config_init(struct drm_device *drm)
{
	drm_mode_config_init(drm);

	drm->mode_config.min_width = 0;
	drm->mode_config.min_height = 0;
	drm->mode_config.max_width = 8192;
	drm->mode_config.max_height = 8192;
	/*
	 * 2026 GKI 6.12 port note: struct drm_mode_config.allow_fb_modifiers
	 * was removed - framebuffer modifiers are now always accepted
	 * whenever a driver sets up IN_FORMATS (addfb2.1), so there is
	 * nothing left for this flag to gate.
	 */

	drm->mode_config.funcs = &sprd_drm_mode_config_funcs;
	drm->mode_config.helper_private = &sprd_drm_mode_config_helper;
}

static const struct drm_ioctl_desc sprd_ioctls[] = {
	DRM_IOCTL_DEF_DRV(SPRD_GSP_GET_CAPABILITY,
			sprd_gsp_get_capability_ioctl, 0),
	DRM_IOCTL_DEF_DRV(SPRD_GSP_TRIGGER, sprd_gsp_trigger_ioctl, 0),
};

static const struct file_operations sprd_drm_fops = {
	.owner		= THIS_MODULE,
	/* drm_open_helper() in 6.12 refuses fops without this flag */
	.fop_flags	= FOP_UNSIGNED_OFFSET,
	.open		= drm_open,
	.release	= drm_release,
	.unlocked_ioctl	= drm_ioctl,
	.compat_ioctl	= drm_compat_ioctl,
	.poll		= drm_poll,
	.read		= drm_read,
	/*
	 * 2026 GKI 6.12 port note: no_llseek() was removed upstream (every
	 * caller that wanted "seeking makes no sense here" semantics now
	 * uses noop_llseek(), which behaves the same way: always returns 0).
	 */
	.llseek		= noop_llseek,
	.mmap		= sprd_gem_cma_mmap,
};

static struct drm_driver sprd_drm_drv = {
	/*
	 * 2026 GKI 6.12 port note: DRIVER_PRIME was removed - prime import/
	 * export is now wired up automatically whenever a driver sets the
	 * gem_prime_* callbacks (which this one does, below), no opt-in flag
	 * needed. DRIVER_HAVE_IRQ was removed along with the rest of the
	 * legacy IRQ midlayer (struct drm_device.irq_enabled below is part
	 * of that same removal) - IRQ handling is entirely this driver's
	 * own responsibility now, nothing left for the flag to gate.
	 */
	.driver_features	= DRIVER_GEM | DRIVER_MODESET | DRIVER_ATOMIC,
	.fops			= &sprd_drm_fops,

	/*
	 * 2026 GKI 6.12 port note: .gem_vm_ops and .gem_free_object_unlocked
	 * no longer exist on struct drm_driver - every GEM object now
	 * carries its own vm_ops/free callback via drm_gem_object.funcs
	 * (see sprd_gem_object_funcs in sprd_gem.c, wired up in
	 * sprd_gem_obj_create()).
	 */
	.dumb_create		= sprd_gem_cma_dumb_create,

	.prime_fd_to_handle	= drm_gem_prime_fd_to_handle,
	.gem_prime_import	= drm_gem_prime_import,
	.gem_prime_import_sg_table = sprd_gem_prime_import_sg_table,

	.ioctls			= sprd_ioctls,
	.num_ioctls		= ARRAY_SIZE(sprd_ioctls),

	.name			= DRIVER_NAME,
	.desc			= DRIVER_DESC,
	.date			= DRIVER_DATE,
	.major			= DRIVER_MAJOR,
	.minor			= DRIVER_MINOR,
};

static int sprd_drm_bind(struct device *dev)
{
	struct drm_device *drm;
	struct sprd_drm *sprd;
	int err;

	DRM_INFO("%s()\n", __func__);

	drm = drm_dev_alloc(&sprd_drm_drv, dev);
	if (IS_ERR(drm))
		return PTR_ERR(drm);

	dev_set_drvdata(dev, drm);

	sprd = devm_kzalloc(drm->dev, sizeof(*sprd), GFP_KERNEL);
	if (!sprd) {
		err = -ENOMEM;
		goto err_free_drm;
	}
	drm->dev_private = sprd;

	sprd_drm_mode_config_init(drm);

	/* bind and init sub drivers */
	err = component_bind_all(drm->dev, drm);
	if (err) {
		DRM_ERROR("failed to bind all component.\n");
		goto err_dc_cleanup;
	}

	/* vblank init */
	err = drm_vblank_init(drm, drm->mode_config.num_crtc);
	if (err) {
		DRM_ERROR("failed to initialize vblank.\n");
		goto err_unbind_all;
	}
	/*
	 * 2026 GKI 6.12 port note: struct drm_device.irq_enabled was removed
	 * along with the legacy IRQ midlayer - drm_vblank_init() above no
	 * longer looks at it (vblank support just works once the driver
	 * itself has its IRQ wired up and calls drm_crtc_handle_vblank()),
	 * so this assignment has nothing left to do.
	 */

	/* reset all the states of crtc/plane/encoder/connector */
	drm_mode_config_reset(drm);

	/* init kms poll for handling hpd */
	drm_kms_helper_poll_init(drm);

	err = drm_dev_register(drm, 0);
	if (err < 0)
		goto err_kms_helper_poll_fini;

	return 0;

err_kms_helper_poll_fini:
	drm_kms_helper_poll_fini(drm);
err_unbind_all:
	component_unbind_all(drm->dev, drm);
err_dc_cleanup:
	drm_mode_config_cleanup(drm);
err_free_drm:
	/*
	 * 2026 GKI 6.12 port note: drm_dev_unref() was renamed to
	 * drm_dev_put() upstream.
	 */
	drm_dev_put(drm);
	return err;
}

static void sprd_drm_unbind(struct device *dev)
{
	struct drm_device *drm = dev_get_drvdata(dev);

	DRM_INFO("%s()\n", __func__);

	/*
	 * 2026 GKI 6.12 port note: drm_put_dev() (unregister + drop the
	 * driver's last reference in one call) was removed along with the
	 * rest of the legacy driver-load convenience helpers. The component
	 * unbind path now does the two steps it used to combine explicitly:
	 * unregister the device, then drop this function's reference to it.
	 */
	drm_dev_unregister(drm);
	drm_dev_put(drm);
}

static const struct component_master_ops drm_component_ops = {
	.bind = sprd_drm_bind,
	.unbind = sprd_drm_unbind,
};

static int compare_of(struct device *dev, void *data)
{
	struct device_node *np = data;

	DRM_DEBUG("compare %pOF\n", np);

	return dev->of_node == np;
}

static int sprd_drm_component_probe(struct device *dev,
			   const struct component_master_ops *m_ops)
{
	struct device_node *ep, *port, *remote;
	struct component_match *match = NULL;
	int i;

	if (!dev->of_node)
		return -EINVAL;

	/*
	 * Bind the crtc's ports first, so that drm_of_find_possible_crtcs()
	 * called from encoder's .bind callbacks works as expected
	 */
	for (i = 0; ; i++) {
		port = of_parse_phandle(dev->of_node, "ports", i);
		if (!port)
			break;

		if (!of_device_is_available(port->parent)) {
			of_node_put(port);
			continue;
		}

		component_match_add(dev, &match, compare_of, port->parent);
		of_node_put(port);
	}

	if (i == 0) {
		dev_err(dev, "missing 'ports' property\n");
		return -ENODEV;
	}

	if (!match) {
		dev_err(dev, "no available port\n");
		return -ENODEV;
	}

	/*
	 * For bound crtcs, bind the encoders attached to their remote endpoint
	 */
	for (i = 0; ; i++) {
		port = of_parse_phandle(dev->of_node, "ports", i);
		if (!port)
			break;

		if (!of_device_is_available(port->parent)) {
			of_node_put(port);
			continue;
		}

		for_each_child_of_node(port, ep) {
			remote = of_graph_get_remote_port_parent(ep);
			if (!remote || !of_device_is_available(remote)) {
				of_node_put(remote);
				continue;
			} else if (!of_device_is_available(remote->parent)) {
				dev_warn(dev, "parent device of %pOF is not available\n",
					 remote);
				of_node_put(remote);
				continue;
			}

			component_match_add(dev, &match, compare_of, remote);
			of_node_put(remote);
		}
		of_node_put(port);
	}

	if (IS_ENABLED(CONFIG_DRM_SPRD_GSP)) {
		for (i = 0; ; i++) {
			port = of_parse_phandle(dev->of_node, "gsp", i);
			if (!port)
				break;

			if (!of_device_is_available(port->parent)) {
				of_node_put(port);
				continue;
			}

			component_match_add(dev, &match, compare_of, port);
			of_node_put(port);
		}
	}

	return component_master_add_with_match(dev, m_ops, match);
}

static int sprd_drm_probe(struct platform_device *pdev)
{
	int ret;

	if (cali_mode) {
		DRM_WARN("Calibration Mode! Don't register sprd drm driver");
		return 0;
	}

	ret = dma_set_mask_and_coherent(&pdev->dev, ~0);
	if (ret)
		DRM_ERROR("dma_set_mask_and_coherent failed (%d)\n", ret);

	return sprd_drm_component_probe(&pdev->dev, &drm_component_ops);
}

/*
 * 2026 GKI 6.12 port note: struct platform_driver.remove became
 * void-returning upstream (v6.11) - same change already applied to the
 * other platform drivers in this module (sprd_dpu.c, dummy_crtc.c,
 * dummy_connector.c, sprd_dsi.c).
 */
static void sprd_drm_remove(struct platform_device *pdev)
{
	component_master_del(&pdev->dev, &drm_component_ops);
}

static void sprd_drm_shutdown(struct platform_device *pdev)
{
	struct drm_device *drm = platform_get_drvdata(pdev);

	if (!drm) {
		DRM_WARN("drm device is not available, no shutdown\n");
		return;
	}

	drm_atomic_helper_shutdown(drm);
}

static int sprd_drm_pm_suspend(struct device *dev)
{
	struct drm_device *drm = dev_get_drvdata(dev);
	struct drm_atomic_state *state;
	struct sprd_drm *sprd;

	if (!drm) {
		DRM_WARN("drm device is not available, no suspend\n");
		return 0;
	}

	DRM_INFO("%s()\n", __func__);

	drm_kms_helper_poll_disable(drm);

	state = drm_atomic_helper_suspend(drm);
	if (IS_ERR(state)) {
		drm_kms_helper_poll_enable(drm);
		DRM_WARN("suspend fail\n");
		return PTR_ERR(state);
	}

	sprd = drm->dev_private;
	sprd->state = state;

	return 0;
}

static int sprd_drm_pm_resume(struct device *dev)
{
	struct drm_device *drm = dev_get_drvdata(dev);
	struct sprd_drm *sprd;

	if (!drm) {
		DRM_WARN("drm device is not available, no resume\n");
		return 0;
	}

	DRM_INFO("%s()\n", __func__);

	sprd = drm->dev_private;
	if (sprd->state) {
		drm_atomic_helper_resume(drm, sprd->state);
		drm_kms_helper_poll_enable(drm);
		sprd->state = NULL;
	}

	return 0;
}

static const struct dev_pm_ops sprd_drm_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(sprd_drm_pm_suspend, sprd_drm_pm_resume)
};

static const struct of_device_id drm_match_table[] = {
	{ .compatible = "sprd,display-subsystem",},
	{},
};
MODULE_DEVICE_TABLE(of, drm_match_table);

static struct platform_driver sprd_drm_driver = {
	.probe = sprd_drm_probe,
	.remove = sprd_drm_remove,
	.shutdown = sprd_drm_shutdown,
	.driver = {
		.name = "sprd-drm-drv",
		.of_match_table = drm_match_table,
		.pm = &sprd_drm_pm_ops,
	},
};

/*
 * GKI 6.12 port note: every one of these files (sprd_dphy.c, sprd_panel.c,
 * sprd_dpu.c, sprd_dsi.c, sprd_bl.c, dummy_connector.c, dummy_crtc.c,
 * gsp_dev.c, lt9611_i2c.c, the per-core-variant dpu/dsi/dphy-pll "register"
 * functions, and the per-SoC-variant global_{dphy,dpu,dsi,dispc}.c files)
 * used to be its own loadable kernel module, each with its own
 * module_init()/module_exit() (or a module_platform_driver()/
 * module_mipi_dsi_driver()/subsys_initcall()/postcore_initcall() macro
 * that expands to the same thing when MODULE is defined). Now that this
 * whole driver tree is built as a single combined sprd_drm.ko DDK module,
 * every one of those would define its own init_module/cleanup_module
 * symbol and the link would fail with "duplicate symbol: init_module"
 * (ld.lld). Each of those files now instead exposes a small, uniquely
 * named, non-static wrapper function (its own module_init/module_exit
 * logic unchanged underneath) that this single real module_init/
 * module_exit calls explicitly, in a fixed order:
 *
 *   1. the permanent "ops table" registrations (per-core-variant dpu/dsi
 *      core register, dphy pll/ppi register, per-SoC-variant global_*
 *      register) - these only append an entry to a static table consumed
 *      by version-string lookup later, have no matching teardown in the
 *      original code either, and must run before anything that might look
 *      such an entry up;
 *   2. the leaf platform_driver/mipi_dsi_driver/i2c_driver registrations
 *      (dphy, panel, dpu, dsi, backlight, the two dummy KMS objects, the
 *      gsp platform driver, the lt9611 i2c bridge) - registering a driver
 *      can synchronously probe an already-present matching device, so
 *      these must run after step 1;
 *   3. sprd_drm_driver itself last, since as the component-master driver
 *      its probe() is what actually binds all the above sub-drivers
 *      together.
 *
 * module_exit() tears down in the reverse order (sprd_drm_driver first,
 * then the leaf drivers in reverse registration order). The step-1 "ops
 * table" registrations are never unregistered, exactly as in the original
 * per-module code.
 */

/*
 * ADT1061/sharkl5pro-only port note: this board's stok.dts pins exactly
 * one variant for every one of these "ops table" families:
 *   - dpu:  sprd,ip = "dpu-r4p0"              -> dpu_r4p0.c only
 *   - dsi:  sprd,ip = "sprd,dsi-ctrl", "r3p1"  -> dsi_ctrl_r1p0.c (the
 *           matching dphy-ppi helper table dsi_ctrl_r1p0_ppi.c registers
 *           under the same "sprd,dsi-ctrl" key and is a required
 *           companion, not an alternative - see sprd_dphy.c's
 *           dphy_ppi_ops_attach() call)
 *   - dphy: sprd,ip = "sprd,megacores-sharkl5" -> megacores_sharkl5.c
 *   - soc:  sprd,soc = "sharkl5Pro" (on the dphy/dpu/dsi nodes alike)
 *           -> the sharkl5Pro/global_*.c glb-register trio only
 *
 * Every other DPU core (r2p0, r3p0, lite_r1p0, lite_r2p0, r5p0), the
 * Synopsys DWC MIPI DSI host core (+ its ppi helper table), the other
 * D-PHY PLLs (megacores_sharkle, intel_ip733mdsitop) and the other
 * per-SoC global_*.c glb-register trios (sharkle, pike2, sharkl3,
 * sharkl5, roc1) are for other Unisoc SoC boards and are never selected
 * on this one, so their files were dropped from this target's
 * BUILD.bazel srcs and their wrappers are no longer declared/called
 * here. lt9611_i2c.c (HDMI bridge, also unused on this board) was
 * already excluded before this cleanup.
 */
int sprd_dpu_r4p0_register(void);
int sprd_dsi_ctrl_r1p0_register(void);
int sprd_dsi_ctrl_r1p0_ppi_register(void);
int sprd_dphy_pll_megacores_sharkl5_register(void);
int sprd_display_class_register(void);
int sprd_sharkl5pro_dphy_glb_register(void);
int sprd_sharkl5pro_dpu_glb_register(void);
int sprd_sharkl5pro_dsi_glb_register(void);

/* step 2: leaf driver registrations (torn down in module_exit) */
int sprd_dphy_driver_register(void);
void sprd_dphy_driver_unregister(void);
int sprd_panel_driver_register(void);
void sprd_panel_driver_unregister(void);
int sprd_dpu_driver_register(void);
void sprd_dpu_driver_unregister(void);
int sprd_dsi_driver_register(void);
void sprd_dsi_driver_unregister(void);
int sprd_backlight_driver_register(void);
void sprd_backlight_driver_unregister(void);
int sprd_dummy_connector_driver_register(void);
void sprd_dummy_connector_driver_unregister(void);
int sprd_dummy_crtc_driver_register(void);
void sprd_dummy_crtc_driver_unregister(void);
int sprd_gsp_dev_register(void);
void sprd_gsp_dev_unregister(void);

static int __init sprd_drm_combined_init(void)
{
	int ret;

	/* step 1: ops-table registrations - never unregistered */
	sprd_dpu_r4p0_register();
	sprd_dsi_ctrl_r1p0_register();
	sprd_dsi_ctrl_r1p0_ppi_register();
	sprd_dphy_pll_megacores_sharkl5_register();
	sprd_display_class_register();
	sprd_sharkl5pro_dphy_glb_register();
	sprd_sharkl5pro_dpu_glb_register();
	sprd_sharkl5pro_dsi_glb_register();

	/* step 2: leaf drivers */
	ret = sprd_dphy_driver_register();
	if (ret)
		goto err_dphy;
	ret = sprd_panel_driver_register();
	if (ret)
		goto err_panel;
	ret = sprd_dpu_driver_register();
	if (ret)
		goto err_dpu;
	ret = sprd_dsi_driver_register();
	if (ret)
		goto err_dsi;
	ret = sprd_backlight_driver_register();
	if (ret)
		goto err_bl;
	ret = sprd_dummy_connector_driver_register();
	if (ret)
		goto err_dummy_connector;
	ret = sprd_dummy_crtc_driver_register();
	if (ret)
		goto err_dummy_crtc;
	ret = sprd_gsp_dev_register();
	if (ret)
		goto err_gsp;

	/* step 3: the component-master driver itself, last */
	ret = platform_driver_register(&sprd_drm_driver);
	if (ret)
		goto err_drm;

	return 0;

err_drm:
	sprd_gsp_dev_unregister();
err_gsp:
	sprd_dummy_crtc_driver_unregister();
err_dummy_crtc:
	sprd_dummy_connector_driver_unregister();
err_dummy_connector:
	sprd_backlight_driver_unregister();
err_bl:
	sprd_dsi_driver_unregister();
err_dsi:
	sprd_dpu_driver_unregister();
err_dpu:
	sprd_panel_driver_unregister();
err_panel:
	sprd_dphy_driver_unregister();
err_dphy:
	return ret;
}
module_init(sprd_drm_combined_init);

static void __exit sprd_drm_combined_exit(void)
{
	platform_driver_unregister(&sprd_drm_driver);
	sprd_gsp_dev_unregister();
	sprd_dummy_crtc_driver_unregister();
	sprd_dummy_connector_driver_unregister();
	sprd_backlight_driver_unregister();
	sprd_dsi_driver_unregister();
	sprd_dpu_driver_unregister();
	sprd_panel_driver_unregister();
	sprd_dphy_driver_unregister();
}
module_exit(sprd_drm_combined_exit);

MODULE_AUTHOR("Leon He <leon.he@unisoc.com>");
MODULE_DESCRIPTION("SPRD DRM KMS Master Driver");
MODULE_LICENSE("GPL v2");
/*
 * GKI 6.12 port: dma_buf_get/put/attach/detach/map_attachment/
 * unmap_attachment (used in sprd_gem.c for the DMA-BUF import path) are
 * exported under the DMA_BUF symbol namespace. Any module calling them
 * must declare that it imports that namespace, or modpost refuses to
 * link ("uses symbol ... from namespace DMA_BUF, but does not import
 * it."). One declaration covers the whole module.
 */
MODULE_IMPORT_NS(DMA_BUF);
