/*
 * ov5640.c - OV5640 sensor driver
 *
 * Based on ov5693.c
 *
 * Copyright (c) 2013-2017, NVIDIA CORPORATION.  All rights reserved.
 *
 * Copyright (c) 2018-2019, Antmicro Ltd. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */


#define DEBUG

#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/gpio.h>
#include <linux/module.h>

#include <linux/seq_file.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_gpio.h>

#include <media/camera_common.h>
#include <media/ov5640.h>

#include "../platform/tegra/camera/camera_gpio.h"

#include "ov5640_mode_tbls.h"

#define OV5640_DEFAULT_LINE_LENGTH	(0x09C4)
#define OV5640_DEFAULT_PIXEL_CLOCK	(160)
#define OV5640_DEFAULT_MODE	OV5640_MODE_1920x1080
#define OV5640_DEFAULT_WIDTH	1920
#define OV5640_DEFAULT_HEIGHT	1080
#define OV5640_DEFAULT_DATAFMT	MEDIA_BUS_FMT_UYVY8_2X8
#define OV5640_DEFAULT_CLK_FREQ	24000000

struct ov5640 {
	struct camera_common_power_rail	power;
	int				numctrls;
	struct v4l2_ctrl_handler	ctrl_handler;
	struct i2c_client		*i2c_client;
	struct v4l2_subdev		*subdev;
	struct media_pad		pad;

	int				reg_offset;

	s32				group_hold_prev;
	u32				frame_length;
	bool				group_hold_en;
	struct regmap			*regmap;
	struct camera_common_data	*s_data;
	struct camera_common_pdata	*pdata;
	struct v4l2_ctrl		*ctrls[];
};

static struct regmap_config ov5640_regmap_config = {
	.reg_bits = 16,
	.val_bits = 8,
};


static struct v4l2_ctrl_config ctrl_config_list[] = {
	{
		.id = V4L2_CID_HDR_EN,
		.name = "HDR enable",
		.type = V4L2_CTRL_TYPE_INTEGER_MENU,
		.min = 0,
		.max = ARRAY_SIZE(switch_ctrl_qmenu) - 1,
		.menu_skip_mask = 0,
		.def = 0,
		.qmenu_int = switch_ctrl_qmenu,
	},
};

static int test_mode;
module_param(test_mode, int, 0644);

static inline int ov5640_read_reg(struct camera_common_data *s_data,
				u16 addr, u8 *val)
{
	struct ov5640 *priv = (struct ov5640 *)s_data->priv;
	int err = 0;
	u32 reg_val = 0;

	err = regmap_read(priv->regmap, addr, &reg_val);
	*val = reg_val & 0xFF;

	return err;
}

static int ov5640_write_reg(struct camera_common_data *s_data, u16 addr, u8 val)
{
	int err = 0;
	struct ov5640 *priv = (struct ov5640 *)s_data->priv;

	err = regmap_write(priv->regmap, addr, val);
	if (err)
		pr_err("%s:i2c write failed, %x = %x\n",
			__func__, addr, val);

	return err;
}

static int ov5640_write_table(struct ov5640 *priv,
			      const ov5640_reg table[])
{
	int err = 0;
	err = regmap_util_write_table_8(priv->regmap,
				table_mode_init,
				NULL, 0,
				OV5640_TABLE_WAIT_MS,
				OV5640_TABLE_END);
	if (err)
		return err;

	err = regmap_util_write_table_8(priv->regmap,
					 table,
					 NULL, 0,
					 OV5640_TABLE_WAIT_MS,
					 OV5640_TABLE_END);
	if (err)
		return err;
	err = regmap_util_write_table_8(priv->regmap,
				table_common_end,
				NULL, 0,
				OV5640_TABLE_WAIT_MS,
				OV5640_TABLE_END);

	return err;
}


static int ov5640_s_stream(struct v4l2_subdev *sd, int enable)
{
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	struct camera_common_data *s_data = to_camera_common_data(client);
	struct ov5640 *priv = (struct ov5640 *)s_data->priv;
	int err;
	u32 frame_time;
	u8 val;

	dev_dbg(&client->dev, "%s++\n", __func__);
	if (!enable) {
		frame_time = priv->frame_length *
			OV5640_DEFAULT_LINE_LENGTH / OV5640_DEFAULT_PIXEL_CLOCK;

		usleep_range(frame_time, frame_time + 1000);
		return 0;
	}
	dev_dbg(&client->dev, "s_data->mode %d----------------\n", s_data->mode);
	err = ov5640_write_table(priv, mode_table[s_data->mode]);
	if (err)
		goto exit;


	if (priv->pdata->v_flip) {
		ov5640_read_reg(priv->s_data, OV5640_TIMING_REG20, &val);
		ov5640_write_reg(priv->s_data, OV5640_TIMING_REG20,
				 val | VERTICAL_FLIP);
	}
	if (priv->pdata->h_mirror) {
		ov5640_read_reg(priv->s_data, OV5640_TIMING_REG21, &val);
		ov5640_write_reg(priv->s_data, OV5640_TIMING_REG21,
				 val | HORIZONTAL_MIRROR_MASK);
	} else {
		ov5640_read_reg(priv->s_data, OV5640_TIMING_REG21, &val);
		ov5640_write_reg(priv->s_data, OV5640_TIMING_REG21,
				 val & (~HORIZONTAL_MIRROR_MASK));
	}
	if (test_mode) {
	dev_dbg(&client->dev, "%s-- test_mode----------------\n", __func__);
		err = ov5640_write_table(priv,
			mode_table[OV5640_MODE_TEST_PATTERN]);
}
	dev_dbg(&client->dev, "%s--\n", __func__);
	return 0;
exit:
	dev_dbg(&client->dev, "%s: error setting stream\n", __func__);
	return err;
}

static int ov5640_g_input_status(struct v4l2_subdev *sd, u32 *status)
{
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	struct camera_common_data *s_data = to_camera_common_data(client);
	struct ov5640 *priv = (struct ov5640 *)s_data->priv;
	struct camera_common_power_rail *pw = &priv->power;

	*status = pw->state == SWITCH_ON;
	return 0;
}

static uint16_t cam_mbus_formats[] = {

	MEDIA_BUS_FMT_UYVY8_2X8,
};

static int cam_enum_mbus_code (struct v4l2_subdev *sd, struct v4l2_subdev_pad_config *cfg, struct v4l2_subdev_mbus_code_enum *code)
{
	if (code->index >= ARRAY_SIZE(cam_mbus_formats))
			return -EINVAL;
		code->code = cam_mbus_formats[code->index];
	return 0;
}


static struct v4l2_subdev_video_ops ov5640_subdev_video_ops = {
	.s_stream	= ov5640_s_stream,
	.g_mbus_config	= camera_common_g_mbus_config,
	.g_input_status = ov5640_g_input_status,
};

static struct v4l2_subdev_core_ops ov5640_subdev_core_ops = {
	.s_power	= camera_common_s_power,
};

static int ov5640_get_fmt(struct v4l2_subdev *sd,
		struct v4l2_subdev_pad_config *cfg,
		struct v4l2_subdev_format *format)
{
	return camera_common_g_fmt(sd, &format->format);
}

static int ov5640_set_fmt(struct v4l2_subdev *sd,
		struct v4l2_subdev_pad_config *cfg,
		struct v4l2_subdev_format *format)
{
	int ret;

	if (format->which == V4L2_SUBDEV_FORMAT_TRY)
		ret = camera_common_try_fmt(sd, &format->format);
	else
		ret = camera_common_s_fmt(sd, &format->format);

	return ret;
}

static struct v4l2_subdev_pad_ops ov5640_subdev_pad_ops = {
	.set_fmt = ov5640_set_fmt,
	.get_fmt = ov5640_get_fmt,
	.enum_mbus_code = cam_enum_mbus_code,
	.enum_frame_size	= camera_common_enum_framesizes,
	.enum_frame_interval	= camera_common_enum_frameintervals,
};

static struct v4l2_subdev_ops ov5640_subdev_ops = {
	.core	= &ov5640_subdev_core_ops,
	.video	= &ov5640_subdev_video_ops,
	.pad	= &ov5640_subdev_pad_ops,
};

static struct of_device_id ov5640_of_match[] = {
	{ .compatible = "nvidia,ov5640", },
	{ },
};

static struct camera_common_sensor_ops ov5640_common_ops = {
	.write_reg = ov5640_write_reg,
	.read_reg = ov5640_read_reg,
};


static int ov5640_ctrls_init(struct ov5640 *priv)
{
	struct i2c_client *client = priv->i2c_client;
	struct v4l2_ctrl *ctrl;
	int numctrls;
	int err;
	int i;

	dev_dbg(&client->dev, "%s++\n", __func__);

	numctrls = ARRAY_SIZE(ctrl_config_list);
	v4l2_ctrl_handler_init(&priv->ctrl_handler, numctrls);

	for (i = 0; i < numctrls; i++) {
		ctrl = v4l2_ctrl_new_custom(&priv->ctrl_handler,
			&ctrl_config_list[i], NULL);
		if (ctrl == NULL) {
			dev_err(&client->dev, "Failed to init %s ctrl\n",
				ctrl_config_list[i].name);
			continue;
		}

		if (ctrl_config_list[i].type == V4L2_CTRL_TYPE_STRING &&
			ctrl_config_list[i].flags & V4L2_CTRL_FLAG_READ_ONLY) {
			ctrl->p_new.p_char = devm_kzalloc(&client->dev,
				ctrl_config_list[i].max + 1, GFP_KERNEL);
			if (!ctrl->p_new.p_char)
				return -ENOMEM;
		}
		priv->ctrls[i] = ctrl;
	}

	priv->numctrls = numctrls;
	priv->subdev->ctrl_handler = &priv->ctrl_handler;
	if (priv->ctrl_handler.error) {
		dev_err(&client->dev, "Error %d adding controls\n",
			priv->ctrl_handler.error);
		err = priv->ctrl_handler.error;
		goto error;
	}

	err = v4l2_ctrl_handler_setup(&priv->ctrl_handler);
	if (err) {
		dev_err(&client->dev,
			"Error %d setting default controls\n", err);
		goto error;
	}
	return 0;

error:
	v4l2_ctrl_handler_free(&priv->ctrl_handler);
	return err;
}

MODULE_DEVICE_TABLE(of, ov5640_of_match);

static struct camera_common_pdata *ov5640_parse_dt(struct i2c_client *client)
{
	struct device_node *node = client->dev.of_node;
	struct camera_common_pdata *board_priv_pdata;
	const struct of_device_id *match;
	struct camera_common_pdata *ret = NULL;

	if (!node)
		return NULL;

	match = of_match_device(ov5640_of_match, &client->dev);
	if (!match) {
		dev_err(&client->dev, "Failed to find matching dt id\n");
		return NULL;
	}
	board_priv_pdata = devm_kzalloc(&client->dev,
			   sizeof(*board_priv_pdata), GFP_KERNEL);
	if (!board_priv_pdata)
		return NULL;

	board_priv_pdata->v_flip = of_property_read_bool(node, "vertical-flip");
	board_priv_pdata->h_mirror = of_property_read_bool(node,
							 "horizontal-mirror");
	return board_priv_pdata;
	return ret;
}

static int ov5640_open(struct v4l2_subdev *sd, struct v4l2_subdev_fh *fh)
{
	struct i2c_client *client = v4l2_get_subdevdata(sd);

	dev_dbg(&client->dev, "%s:\n", __func__);
	return 0;
}

static const struct v4l2_subdev_internal_ops ov5640_subdev_internal_ops = {
	.open = ov5640_open,
};

static const struct media_entity_operations ov5640_media_ops = {
	.link_validate = v4l2_subdev_link_validate,
};

static int ov5640_probe(struct i2c_client *client,
			const struct i2c_device_id *id)
{
	struct camera_common_data *common_data;
	struct device_node *node = client->dev.of_node;
	struct ov5640 *priv;
	char debugfs_name[10];
	int err;
	u8 chip_id_hi, chip_id_lo;
	u16 chip_id;

	pr_info("[OV5640]: probing v4l2 sensor.\n");

	if (!IS_ENABLED(CONFIG_OF) || !node)
		return -EINVAL;

	common_data = devm_kzalloc(&client->dev,
			    sizeof(struct camera_common_data), GFP_KERNEL);
	if (!common_data)
		return -ENOMEM;

	priv = devm_kzalloc(&client->dev,
			    sizeof(struct ov5640) + sizeof(struct v4l2_ctrl *) *
			    ARRAY_SIZE(ctrl_config_list),
			    GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->regmap = devm_regmap_init_i2c(client, &ov5640_regmap_config);
	if (IS_ERR(priv->regmap)) {
		dev_err(&client->dev,
			"regmap init failed: %ld\n", PTR_ERR(priv->regmap));
		return -ENODEV;
	}

	priv->pdata = ov5640_parse_dt(client);
	if (PTR_ERR(priv->pdata) == -EPROBE_DEFER)
		return -EPROBE_DEFER;
	if (!priv->pdata) {
		dev_err(&client->dev, "unable to get platform data\n");
		return -EFAULT;
	}

	common_data->ops		= &ov5640_common_ops;
	common_data->ctrl_handler	= &priv->ctrl_handler;
	common_data->i2c_client		= client;
	common_data->frmfmt		= ov5640_frmfmt;
	common_data->colorfmt		= camera_common_find_datafmt(
					  OV5640_DEFAULT_DATAFMT);
	common_data->power		= &priv->power;
	common_data->ctrls		= priv->ctrls;
	common_data->priv		= (void *)priv;
	common_data->numctrls		= ARRAY_SIZE(ctrl_config_list);
	common_data->numfmts		= ARRAY_SIZE(ov5640_frmfmt);
	common_data->def_mode		= OV5640_DEFAULT_MODE;
	common_data->def_width		= OV5640_DEFAULT_WIDTH;
	common_data->def_height		= OV5640_DEFAULT_HEIGHT;
	common_data->fmt_width		= common_data->def_width;
	common_data->fmt_height		= common_data->def_height;
	common_data->def_clk_freq	= OV5640_DEFAULT_CLK_FREQ;

	priv->i2c_client = client;
	priv->s_data			= common_data;
	priv->subdev			= &common_data->subdev;
	priv->subdev->dev		= &client->dev;
	priv->s_data->dev		= &client->dev;



	err = ov5640_read_reg(priv->s_data, 0x300A, &chip_id_hi);
	if (err) {
		dev_err(&client->dev, "Failure to read Chip ID (high byte)\n");
		return err;
	}

	err = ov5640_read_reg(priv->s_data, 0x300B, &chip_id_lo);
	if (err) {
		dev_err(&client->dev, "Failure to read Chip ID (low byte)\n");
		return err;
	}

	chip_id = (chip_id_hi << 8) | chip_id_lo;
	if (chip_id != 0x5640) {
		dev_err(&client->dev, "Chip ID: %x not supported!\n", chip_id);
		err = -ENODEV;
		return err;
	}

	err = camera_common_parse_ports(client, common_data);
	if (err) {
		dev_err(&client->dev, "Failed to find port info\n");
		return err;
	}
	sprintf(debugfs_name, "ov5640_%c", common_data->csi_port + 'a');
	dev_dbg(&client->dev, "%s: name %s\n", __func__, debugfs_name);
	camera_common_create_debugfs(common_data, debugfs_name);

	v4l2_i2c_subdev_init(priv->subdev, client, &ov5640_subdev_ops);
	err = ov5640_ctrls_init(priv);
	if (err)
		return err;

	priv->subdev->internal_ops = &ov5640_subdev_internal_ops;
	priv->subdev->flags |= V4L2_SUBDEV_FL_HAS_DEVNODE |
			       V4L2_SUBDEV_FL_HAS_EVENTS;

#if defined(CONFIG_MEDIA_CONTROLLER)
	priv->pad.flags = MEDIA_PAD_FL_SOURCE;
	priv->subdev->entity.type = MEDIA_ENT_T_V4L2_SUBDEV_SENSOR;
	priv->subdev->entity.ops = &ov5640_media_ops;
	err = media_entity_init(&priv->subdev->entity, 1, &priv->pad, 0);
	if (err < 0) {
		dev_err(&client->dev, "unable to init media entity\n");
		return err;
	}
#endif

	err = v4l2_async_register_subdev(priv->subdev);
	if (err)
		return err;

	dev_dbg(&client->dev, "Detected OV5640 sensor\n");


	return 0;
}

static int
ov5640_remove(struct i2c_client *client)
{
	struct camera_common_data *s_data = to_camera_common_data(client);
	struct ov5640 *priv = (struct ov5640 *)s_data->priv;

	v4l2_async_unregister_subdev(priv->subdev);
#if defined(CONFIG_MEDIA_CONTROLLER)
	media_entity_cleanup(&priv->subdev->entity);
#endif

	v4l2_ctrl_handler_free(&priv->ctrl_handler);
	camera_common_remove_debugfs(s_data);

	return 0;
}

static const struct i2c_device_id ov5640_id[] = {
	{ "ov5640", 0 },
	{ }
};

MODULE_DEVICE_TABLE(i2c, ov5640_id);

static struct i2c_driver ov5640_i2c_driver = {
	.driver = {
		.name = "ov5640",
		.owner = THIS_MODULE,
		.of_match_table = of_match_ptr(ov5640_of_match),
	},
	.probe = ov5640_probe,
	.remove = ov5640_remove,
	.id_table = ov5640_id,
};

module_i2c_driver(ov5640_i2c_driver);

MODULE_DESCRIPTION("SoC Camera driver for Omnivision OV5640");
MODULE_AUTHOR("David Wang <davidw@nvidia.com>, Antmicro Ltd.");
MODULE_LICENSE("GPL v2");

