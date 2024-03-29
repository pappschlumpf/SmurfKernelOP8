/*
 * These files include source code for the Licensed Software that is provided under the terms and
 * conditions of a Master Software License Agreement and its applicable schedules.
 */

#include <drm/drm_mipi_dsi.h>
#include <dsi_drm.h>
#include "sde_encoder_phys.h"
#include "sde_trace.h"

#include "dsi_iris5_api.h"
#include "dsi_iris5.h"
#include "dsi_iris5_lightup.h"
#include "dsi_irissoft_ioctl.h"

static int gcfg_index = 0;
static struct iris_cfg gcfg[IRIS_CFG_NUM] = {};

int iris_wait_vsync()
{
	struct iris_cfg *pcfg = &gcfg[gcfg_index];
	struct drm_encoder *drm_enc;

	if (pcfg->display->bridge == NULL)
		return -ENOLINK;
	drm_enc = pcfg->display->bridge->base.encoder;
	if (!drm_enc || !drm_enc->crtc)
		return -ENOLINK;
	if (sde_encoder_is_disabled(drm_enc))
		return -EIO;

	mutex_unlock(&pcfg->panel->panel_lock);
	sde_encoder_wait_for_event(drm_enc, MSM_ENC_VBLANK);
	mutex_lock(&pcfg->panel->panel_lock);

	return 0;
}

int iris_set_pending_panel_brightness(int32_t pending, int32_t delay, int32_t level)
{
	struct iris_cfg *pcfg = &gcfg[gcfg_index];

	pcfg->panel_pending = pending;
	pcfg->panel_delay = delay;
	pcfg->panel_level = level;

	return 0;
}

int iris5_sync_panel_brightness(int32_t step, void *phys_enc)
{
	struct sde_encoder_phys *phys_encoder = phys_enc;
	struct sde_connector *c_conn = NULL;
	struct dsi_display *display = NULL;
	struct iris_cfg *pcfg;
	int rc = 0;

	if (phys_encoder == NULL)
		return -EFAULT;
	if (phys_encoder->connector == NULL)
		return -EFAULT;

	c_conn = to_sde_connector(phys_encoder->connector);
	if (c_conn == NULL)
		return -EFAULT;

	if (c_conn->connector_type != DRM_MODE_CONNECTOR_DSI)
		return 0;

	display = c_conn->display;
	if (display == NULL)
		return -EFAULT;

	pcfg = &gcfg[gcfg_index];

	if (pcfg->panel_pending == step) {
		SDE_ATRACE_BEGIN("sync_panel_brightness");
		if (step <= 2) {
			rc = c_conn->ops.set_backlight(&c_conn->base,
					display, pcfg->panel_level);
			usleep_range(pcfg->panel_delay, pcfg->panel_delay);
		} else {
			usleep_range(pcfg->panel_delay, pcfg->panel_delay);
			rc = c_conn->ops.set_backlight(&c_conn->base,
					display, pcfg->panel_level);
		}
		if (c_conn->bl_device)
			c_conn->bl_device->props.brightness = pcfg->panel_level;
		pcfg->panel_pending = 0;
		SDE_ATRACE_END("sync_panel_brightness");
	}

	return rc;
}

int iris5_lightoff(
		struct dsi_panel *panel,
		struct dsi_panel_cmd_set *off_cmds)
{
	struct iris_cfg *pcfg;

	pcfg = &gcfg[gcfg_index];
	pcfg->panel_pending = 0;

	return 0;
}

int iris_configure(u32 display, u32 type, u32 value)
{
	if (type >= IRIS_CONFIG_TYPE_MAX)
		return -1;
	/* FIXME
	if (mfd->panel_power_state == MDSS_PANEL_POWER_OFF)
		return -1;
	*/

	switch (type) {
	case IRIS_WAIT_VSYNC:
		return iris_wait_vsync();
	default:
		break;
	}

	return 0;
}

int iris_configure_t(uint32_t display, u32 type, void __user *argp)
{
	int ret = -1;
	uint32_t value = 0;

	ret = copy_from_user(&value, argp, sizeof(uint32_t));
	if (ret) {
		DSI_ERR("can not copy from user");
		return -EPERM;
	}

	ret = iris_configure(display, type, value);
	return ret;
}

int iris_configure_ex(u32 display, u32 type, u32 count, u32 *values)
{
	if (type >= IRIS_CONFIG_TYPE_MAX)
		return -1;

	switch (type) {
	case IRIS_WAIT_VSYNC:
		if (count > 2)
			iris_set_pending_panel_brightness(values[0], values[1], values[2]);
		break;
	default:
		break;
	}

	return 0;
}

int iris_configure_ex_t(uint32_t display, uint32_t type,
								uint32_t count, void __user *values)
{
	int ret = -1;
	uint32_t *val = NULL;

	val = kmalloc(sizeof(uint32_t) * count, GFP_KERNEL);
	if (!val) {
		DSI_ERR("can not kmalloc space");
		return -ENOSPC;
	}
	ret = copy_from_user(val, values, sizeof(uint32_t) * count);
	if (ret) {
		DSI_ERR("can not copy from user");
		kfree(val);
		return -EPERM;
	}
	ret = iris_configure_ex(display, type, count, val);
	kfree(val);
	return ret;
}

void iris5_init(struct dsi_display *display, struct dsi_panel *panel)
{
	struct iris_cfg *pcfg = &gcfg[gcfg_index];

	pcfg->display = display;
	pcfg->panel = panel;
	pcfg->valid = 1;	/* empty */
}

int iris5_operate_tool(struct msm_iris_operate_value *argp)
{
	return -EINVAL;
}

int iris5_operate_conf(struct msm_iris_operate_value *argp)
{
	int ret = -1;
	uint32_t parent_type = 0;
	uint32_t child_type = 0;
	uint32_t display_type = 0;
	struct iris_cfg *pcfg = NULL;

	parent_type = argp->type & 0xff;
	child_type = (argp->type >> 8) & 0xff;
	display_type = (argp->type >> 16) & 0xff;
	pcfg = &gcfg[gcfg_index];
	if (pcfg == NULL || pcfg->valid < 1) {
		DSI_ERR("Target display does not exist!");
		return -EPERM;
	}

	switch (parent_type) {
	case IRIS_OPRT_CONFIGURE:
		mutex_lock(&pcfg->panel->panel_lock);
		ret = iris_configure_t(display_type, child_type, argp->values);
		mutex_unlock(&pcfg->panel->panel_lock);
		break;
	case IRIS_OPRT_CONFIGURE_NEW:
		mutex_lock(&pcfg->panel->panel_lock);
		ret = iris_configure_ex_t(display_type, child_type, argp->count, argp->values);
		mutex_unlock(&pcfg->panel->panel_lock);
		break;
	default:
		ret = 0;
		break;
	}

	return ret;
}
