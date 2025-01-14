
// #define DEBUG
#include <linux/module.h>
#include <linux/delay.h>
#include <linux/kthread.h>
#include <linux/i2c.h>
#include <media/v4l2-async.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-event.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-subdev.h>
#include "otp_flash.h"

struct sensor {
	struct v4l2_subdev v4l2_subdev;
	struct media_pad pad;
	struct v4l2_mbus_framefmt fmt;
	struct i2c_client *i2c_client;
#ifdef __FAKE__
	void *otp_flash_instance;
#else
	struct otp_flash *otp_flash_instance;
#endif

	struct gpio_desc *reset_gpio;
	struct gpio_desc *host_power_gpio;
	struct gpio_desc *device_power_gpio;
	struct gpio_desc *standby_gpio;
	u8 selected_mode;
	u8 selected_sensor;
	char *sensor_name;

	/* V4L2 Controls */
	struct v4l2_ctrl_handler ctrl_handler;
	struct v4l2_ctrl *pixel_rate;
};

struct resolution {
	u16 width;
	u16 height;
};

static struct resolution ar0144_res_list[] = {
	{.width = 1280, .height = 800},
};

static struct resolution ar0234_res_list[] = {
	{.width = 1920, .height = 1200},
	{.width = 1920, .height = 1080},
	{.width = 1280, .height = 720},
};

static struct resolution ar0521_res_list[] = {
	{.width = 1280, .height = 720},
	{.width = 1920, .height = 1080},
	{.width = 2592, .height = 1944},
};

static struct resolution ar0821_res_list[] = {
	{.width = 1280, .height = 720},
	{.width = 1920, .height = 1080},
	{.width = 2560, .height = 1440},
	{.width = 3840, .height = 2160},
};

struct sensor_info {
	const char* sensor_name;
	const struct resolution *res_list;
	u32 res_list_size;
};

static struct sensor_info ap1302_sensor_table[] = {
	{
		.sensor_name = "TEVI-AR0144",
		.res_list = ar0144_res_list,
		.res_list_size = ARRAY_SIZE(ar0144_res_list)
	},
	{
		.sensor_name = "TEVI-AR0234",
		.res_list = ar0234_res_list,
		.res_list_size = ARRAY_SIZE(ar0234_res_list)
	},
	{
		.sensor_name = "TEVI-AR0521",
		.res_list = ar0521_res_list,
		.res_list_size = ARRAY_SIZE(ar0521_res_list)
	},
	{
		.sensor_name = "TEVI-AR0522",
		.res_list = ar0521_res_list,
		.res_list_size = ARRAY_SIZE(ar0521_res_list)
	},
	{
		.sensor_name = "TEVI-AR0821",
		.res_list = ar0821_res_list,
		.res_list_size = ARRAY_SIZE(ar0821_res_list)
	},
};

static int sensor_standby(struct i2c_client *client, int enable);

static int sensor_i2c_read(struct i2c_client *client, u16 reg, u8 *val, u8 size)
{
	struct i2c_msg msg[2];
	u8 buf[2];

	buf[0] = reg >> 8;
	buf[1] = reg & 0xff;

	msg[0].addr = client->addr;
	msg[0].flags = client->flags;
	msg[0].buf = buf;
	msg[0].len = sizeof(buf);

	msg[1].addr = client->addr;
	msg[1].flags = client->flags | I2C_M_RD;
	msg[1].buf = val;
	msg[1].len = size;

	return i2c_transfer(client->adapter, msg, 2);
}

static int sensor_i2c_read_16b(struct i2c_client *client, u16 reg, u16 *value)
{
	u8 v[2] = {0,0};
	int ret;

	ret = sensor_i2c_read(client, reg, v, 2);

	if (unlikely(ret < 0)) {
		dev_err(&client->dev, "i2c transfer error.\n");
		return ret;
	}

	*value = (v[0] << 8) | v[1];
	dev_dbg(&client->dev, "%s() read reg 0x%x, value 0x%x\n",
		 __func__, reg, *value);

	return 0;
}

static int sensor_i2c_write_16b(struct i2c_client *client, u16 reg, u16 val)
{
	struct i2c_msg msg;
	u8 buf[4];
	int retry_tmp = 0;

	buf[0] = reg >> 8;
	buf[1] = reg & 0xff;
	buf[2] = val >> 8;
	buf[3] = val & 0xff;

	msg.addr = client->addr;
	msg.flags = client->flags;
	msg.buf = buf;
	msg.len = sizeof(buf);


	while((i2c_transfer(client->adapter, &msg, 1)) < 0)
	{
		retry_tmp++;
		dev_err(&client->dev, "i2c transfer retry:%d.\n", retry_tmp);
		dev_dbg(&client->dev, "write 16b reg:%x val:%x.\n", reg, val);

		if (retry_tmp > 50)
		{
			dev_err(&client->dev, "i2c transfer error.\n");
			return -EIO;
		}
	}

	return 0;
}

static int sensor_i2c_write_bust(struct i2c_client *client, u8 *buf, size_t len)
{
	struct i2c_msg msg;
	int retry_tmp = 0;

	if (len == 0) {
		return 0;
	}

	msg.addr = client->addr;
	msg.flags = client->flags;
	msg.buf = buf;
	msg.len = len;

	while((i2c_transfer(client->adapter, &msg, 1)) < 0)
	{
		retry_tmp++;
		dev_err(&client->dev, "i2c transfer retry:%d.\n", retry_tmp);
		dev_dbg(&client->dev, "write bust buf:%x.\n", client->addr);

		if (retry_tmp > 50)
		{
			dev_err(&client->dev, "i2c transfer error.\n");
			return -EIO;
		}
	}

	return 0;
}

static int ops_power(struct v4l2_subdev *sub_dev, int on)
{
	//struct sensor *instance = container_of(sub_dev, struct sensor, v4l2_subdev);

	dev_dbg(sub_dev->dev, "%s() [%d]\n", __func__, on);
	return 0;
}

static int ops_init(struct v4l2_subdev *sub_dev, u32 val)
{
	//struct sensor *instance = container_of(sub_dev, struct sensor, v4l2_subdev);

	dev_dbg(sub_dev->dev, "%s() [%d]\n", __func__, val);
	return 0;
}

static int ops_load_fw(struct v4l2_subdev *sub_dev)
{
	//struct sensor *instance = container_of(sub_dev, struct sensor, v4l2_subdev);

	dev_dbg(sub_dev->dev, "%s()\n", __func__);
	return 0;
}

static int ops_reset(struct v4l2_subdev *sub_dev, u32 val)
{
	//struct sensor *instance = container_of(sub_dev, struct sensor, v4l2_subdev);

	dev_dbg(sub_dev->dev, "%s() [%d]\n", __func__, val);
	return 0;
}

static int ops_get_frame_interval(struct v4l2_subdev *sub_dev,
				  struct v4l2_subdev_frame_interval *fi)
{
	dev_dbg(sub_dev->dev, "%s()\n", __func__);

	if (fi->pad != 0)
		return -EINVAL;

	fi->interval.numerator = 1;
	fi->interval.denominator = 30;

	return 0;
}

static int ops_set_frame_interval(struct v4l2_subdev *sub_dev,
				  struct v4l2_subdev_frame_interval *fi)
{
	dev_dbg(sub_dev->dev, "%s()\n", __func__);

	if (fi->pad != 0)
		return -EINVAL;

	fi->interval.numerator = 1;
	fi->interval.denominator = 30;

	return 0;
}

static int ops_set_stream(struct v4l2_subdev *sub_dev, int enable)
{
	struct sensor *instance = container_of(sub_dev, struct sensor, v4l2_subdev);
	int ret = 0;

	dev_dbg(sub_dev->dev, "%s() enable [%x]\n", __func__, enable);

	if (instance->selected_mode >= ap1302_sensor_table[instance->selected_sensor].res_list_size)
		return -EINVAL;

	if (enable == 0) {
		sensor_i2c_write_16b(instance->i2c_client, 0x1184, 1); //ATOMIC
		//VIDEO_WIDTH
		sensor_i2c_write_16b(instance->i2c_client, 0x2000, 1920);
		//VIDEO_HEIGHT
		sensor_i2c_write_16b(instance->i2c_client, 0x2002, 1080);
		sensor_i2c_write_16b(instance->i2c_client, 0x1184, 0xb); //ATOMIC
		ret = sensor_standby(instance->i2c_client, 1);
	} else {
		ret = sensor_standby(instance->i2c_client, 0);
		if (ret == 0) {
			dev_dbg(sub_dev->dev, "%s() width=%d, height=%d\n", __func__, 
				ap1302_sensor_table[instance->selected_sensor].res_list[instance->selected_mode].width, 
				ap1302_sensor_table[instance->selected_sensor].res_list[instance->selected_mode].height);
			sensor_i2c_write_16b(instance->i2c_client, 0x1184, 1); //ATOMIC
			//VIDEO_WIDTH
			sensor_i2c_write_16b(instance->i2c_client, 0x2000,
					     ap1302_sensor_table[instance->selected_sensor].res_list[instance->selected_mode].width);
			//VIDEO_HEIGHT
			sensor_i2c_write_16b(instance->i2c_client, 0x2002,
					     ap1302_sensor_table[instance->selected_sensor].res_list[instance->selected_mode].height);
			sensor_i2c_write_16b(instance->i2c_client, 0x2020, 0x1e00); //VIDEO_MAX_FPS
			sensor_i2c_write_16b(instance->i2c_client, 0x1184, 0xb); //ATOMIC
		}
	}

	return ret;
}

static int ap1302_set_ctrl(struct v4l2_ctrl *ctrl) {

	return 0;
}


static const struct v4l2_ctrl_ops ap1302_ctrl_ops = {
	.s_ctrl = ap1302_set_ctrl,
};

static void ops_init_formats(struct v4l2_subdev_state *state)
{
	struct v4l2_mbus_framefmt *format;

	format = v4l2_state_get_stream_format(state, 0, 0);
	format->code = MEDIA_BUS_FMT_UYVY8_2X8;
	format->width = 1280;
	format->height = 800;
	format->field = V4L2_FIELD_NONE;
	format->colorspace = V4L2_COLORSPACE_SRGB;
}

static int _ap1302_set_routing(struct v4l2_subdev *sd,
                              struct v4l2_subdev_state *state)
{
	struct v4l2_subdev_route routes[] = {
		{
			.source_pad = 0,
			.source_stream = 0,
			.flags = V4L2_SUBDEV_ROUTE_FL_IMMUTABLE |
				V4L2_SUBDEV_ROUTE_FL_SOURCE |
				V4L2_SUBDEV_ROUTE_FL_ACTIVE,
		},
		{
			.source_pad = 0,
			.source_stream = 1,
			.flags = V4L2_SUBDEV_ROUTE_FL_IMMUTABLE |
				V4L2_SUBDEV_ROUTE_FL_SOURCE,
		}
	};

	struct v4l2_subdev_krouting routing = {
		.num_routes = ARRAY_SIZE(routes),
		.routes = routes,
	};

	int ret;

	ret = v4l2_subdev_set_routing(sd, state, &routing);
	if (ret < 0)
		return ret;

	ops_init_formats(state);

	return 0;
}

static int ops_init_cfg(struct v4l2_subdev *sd,
                          struct v4l2_subdev_state *state)
{
	int ret;

	v4l2_subdev_lock_state(state);

	ret = _ap1302_set_routing(sd, state);

	v4l2_subdev_unlock_state(state);

	return ret;
}

static int ops_get_frame_desc(struct v4l2_subdev *sd, unsigned int pad,
                                struct v4l2_mbus_frame_desc *fd)
{
	struct v4l2_subdev_state *state;
	struct v4l2_mbus_framefmt *fmt;
	u32 bpp;
	int ret = 0;

	if (pad != 0)
		return -EINVAL;

	state = v4l2_subdev_lock_active_state(sd);

	fmt = v4l2_state_get_stream_format(state, 0, 0);
	if (!fmt) {
		ret = -EPIPE;
		goto out;
	}

	memset(fd, 0, sizeof(*fd));

	fd->type = V4L2_MBUS_FRAME_DESC_TYPE_CSI2;

	/* pixel stream */

	bpp = 8;

	fd->entry[fd->num_entries].stream = 0;

	fd->entry[fd->num_entries].flags = V4L2_MBUS_FRAME_DESC_FL_LEN_MAX;
	fd->entry[fd->num_entries].length = fmt->width * fmt->height * bpp / 8;
	fd->entry[fd->num_entries].pixelcode = fmt->code;
	fd->entry[fd->num_entries].bus.csi2.vc = 0;
	fd->entry[fd->num_entries].bus.csi2.dt = 0x1e; /* SRGB */

	fd->num_entries++;

out:
	v4l2_subdev_unlock_state(state);

	return ret;
}

static int ops_set_routing(struct v4l2_subdev *sd,
                             struct v4l2_subdev_state *state,
                             enum v4l2_subdev_format_whence which,
                             struct v4l2_subdev_krouting *routing)
{
	int ret;

	if (routing->num_routes == 0 || routing->num_routes > 1)
		return -EINVAL;

	v4l2_subdev_lock_state(state);

	ret = _ap1302_set_routing(sd, state);
	v4l2_subdev_lock_state(state);

	ret = _ap1302_set_routing(sd, state);

	v4l2_subdev_unlock_state(state);

	return ret;
}

static int ops_enum_mbus_code(struct v4l2_subdev *sub_dev,
			      struct v4l2_subdev_state *sd_state,
			      struct v4l2_subdev_mbus_code_enum *code)
{
	dev_dbg(sub_dev->dev, "%s()\n", __func__);

	if (code->pad || code->index > 0)
		return -EINVAL;

	code->code = MEDIA_BUS_FMT_UYVY8_2X8;

	return 0;
}

static int ops_get_fmt(struct v4l2_subdev *sub_dev,
		       struct v4l2_subdev_state *sd_state,
		       struct v4l2_subdev_format *format)
{
	struct v4l2_mbus_framefmt *fmt;
	struct v4l2_mbus_framefmt *mbus_fmt = &format->format;
	struct sensor *instance = container_of(sub_dev, struct sensor, v4l2_subdev);

	dev_dbg(sub_dev->dev, "%s()\n", __func__);

	if (format->pad != 0)
		return -EINVAL;

	if (format->which == V4L2_SUBDEV_FORMAT_TRY)
		fmt = v4l2_subdev_get_try_format(sub_dev,
						 sd_state,
						 format->pad);
	else
		fmt = &instance->fmt;

	memmove(mbus_fmt, fmt, sizeof(struct v4l2_mbus_framefmt));

	return 0;
}

static int ops_set_fmt(struct v4l2_subdev *sub_dev,
		       struct v4l2_subdev_state *sd_state,
		       struct v4l2_subdev_format *format)
{
	struct v4l2_mbus_framefmt *fmt;
	struct v4l2_mbus_framefmt *mbus_fmt = &format->format;
	struct sensor *instance = container_of(sub_dev, struct sensor, v4l2_subdev);
	int i;

	dev_dbg(sub_dev->dev, "%s()\n", __func__);

	if (format->pad != 0)
		return -EINVAL;

	for(i = 0 ; i < ap1302_sensor_table[instance->selected_sensor].res_list_size ; i++)
	{
		if (mbus_fmt->width == ap1302_sensor_table[instance->selected_sensor].res_list[i].width &&
				mbus_fmt->height == ap1302_sensor_table[instance->selected_sensor].res_list[i].height)
			break;
	}

	if (i >= ap1302_sensor_table[instance->selected_sensor].res_list_size)
	{
		return -EINVAL;
	}
	instance->selected_mode = i;
	dev_dbg(sub_dev->dev, "%s() selected mode index [%d]\n", __func__,
		instance->selected_mode);

	mbus_fmt->width = ap1302_sensor_table[instance->selected_sensor].res_list[i].width;
	mbus_fmt->height = ap1302_sensor_table[instance->selected_sensor].res_list[i].height;
	mbus_fmt->code = MEDIA_BUS_FMT_UYVY8_2X8;
	mbus_fmt->colorspace = V4L2_COLORSPACE_SRGB;
	mbus_fmt->ycbcr_enc = V4L2_MAP_YCBCR_ENC_DEFAULT(mbus_fmt->colorspace);
	mbus_fmt->quantization = V4L2_QUANTIZATION_FULL_RANGE;
	mbus_fmt->xfer_func = V4L2_MAP_XFER_FUNC_DEFAULT(mbus_fmt->colorspace);
	memset(mbus_fmt->reserved, 0, sizeof(mbus_fmt->reserved));

	if (format->which == V4L2_SUBDEV_FORMAT_TRY)
		fmt = v4l2_subdev_get_try_format(sub_dev, sd_state, 0);
	else
		fmt = &instance->fmt;

	memmove(fmt, mbus_fmt, sizeof(struct v4l2_mbus_framefmt));

	return 0;
}

static int ops_enum_frame_size(struct v4l2_subdev *sub_dev,
			       struct v4l2_subdev_state *sd_state,
			       struct v4l2_subdev_frame_size_enum *fse)
{
	struct sensor *instance = container_of(sub_dev, struct sensor, v4l2_subdev);
	dev_dbg(sub_dev->dev, "%s() %x %x %x\n", __func__,
		fse->pad, fse->code, fse->index);

	if ((fse->pad != 0) ||
	    (fse->index >= ap1302_sensor_table[instance->selected_sensor].res_list_size))
		return -EINVAL;

	fse->min_width = fse->max_width = ap1302_sensor_table[instance->selected_sensor].res_list[fse->index].width;
	fse->min_height = fse->max_height = ap1302_sensor_table[instance->selected_sensor].res_list[fse->index].height;

	return 0;
}

static int ops_enum_frame_interval(struct v4l2_subdev *sub_dev,
				   struct v4l2_subdev_state *sd_state,
				   struct v4l2_subdev_frame_interval_enum *fie)
{
	dev_dbg(sub_dev->dev, "%s()\n", __func__);

	if ((fie->pad != 0) ||
	    (fie->index != 0))
		return -EINVAL;

	fie->interval.numerator = 1;
	fie->interval.denominator = 30;

	return 0;
}

static const struct v4l2_subdev_core_ops sensor_v4l2_subdev_core_ops = {
	.s_power = ops_power,
	.init = ops_init,
	.load_fw = ops_load_fw,
	.reset = ops_reset,
};
static const struct v4l2_subdev_video_ops sensor_v4l2_subdev_video_ops = {
	.g_frame_interval = ops_get_frame_interval,
	.s_frame_interval = ops_set_frame_interval,
	.s_stream = ops_set_stream,
};
static const struct v4l2_subdev_pad_ops sensor_v4l2_subdev_pad_ops = {
	.init_cfg = ops_init_cfg,
	.enum_mbus_code = ops_enum_mbus_code,
	.get_fmt = ops_get_fmt,
	.set_fmt = ops_set_fmt,
	.enum_frame_size = ops_enum_frame_size,
	.enum_frame_interval = ops_enum_frame_interval,
	.set_routing = ops_set_routing,
	.get_frame_desc	= ops_get_frame_desc,
};

static const struct v4l2_subdev_ops sensor_subdev_ops = {
	.core = &sensor_v4l2_subdev_core_ops,
	.video = &sensor_v4l2_subdev_video_ops,
	.pad = &sensor_v4l2_subdev_pad_ops,
};

static int check_sensor_chip_id(struct i2c_client *client, u16* chip_id)
{
	int timeout;

	for (timeout = 0 ; timeout < 100 ; timeout ++) {
		usleep_range(9000, 10000);
		sensor_i2c_read_16b(client, 0x60AC, chip_id);
		if ((*chip_id & 0x7) == 0)
			break;
	}
	if (timeout >= 100) {
		dev_err(&client->dev, "timeout: line[%d]v=%x\n", __LINE__, *chip_id);
		return -EINVAL;
	}
	sensor_i2c_write_16b(client, 0x60AA, 0x0002); // DMA_SIZE
	sensor_i2c_write_16b(client, 0x60A0, 0x0320); // DMA_SRC_0
	sensor_i2c_write_16b(client, 0x60A2, 0x3000); // DMA_SRC_1
	sensor_i2c_write_16b(client, 0x60A4, 0x0000); // DMA_DST_0
	sensor_i2c_write_16b(client, 0x60A6, 0x60A4); // DMA_DST_1
	sensor_i2c_write_16b(client, 0x60AC, 0x0032); // DMA_CTRL
	for (timeout = 0 ; timeout < 100 ; timeout ++) {
		usleep_range(9000, 10000);
		sensor_i2c_read_16b(client, 0x60AC, chip_id);
		if ((*chip_id & 0x7) == 0)
			break;
	}
	if (timeout >= 100) {
		dev_err(&client->dev, "timeout: line[%d]v=%x\n", __LINE__, *chip_id);
		return -EINVAL;
	}
	sensor_i2c_read_16b(client, 0x60A4, chip_id);

	return 0;
}

static int set_standby_mode_rel419(struct i2c_client *client, int enable)
{
	u16 v = 0;
	int timeout;
	dev_dbg(&client->dev, "%s():enable=%d\n", __func__, enable);

	if (enable == 1) {
		sensor_i2c_write_16b(client, 0xf056, 0x0000);
		sensor_i2c_write_16b(client, 0x601a, 0x8140);
		for (timeout = 0 ; timeout < 500 ; timeout ++) {
			usleep_range(9000, 10000);
			sensor_i2c_read_16b(client, 0x601a, &v);
			if ((v & 0x200) == 0x200)
				break;
		}
		if (timeout < 500) {
			if(check_sensor_chip_id(client, &v) == 0) {
				if (v == 0x356) {
					dev_dbg(&client->dev, "sensor check: v=0x%x\nbypass standby and set fw stall gate frames.\n", v);
					return 0;
				}
			}
			// Reset ADV_GPIO in Advanced Registers
			sensor_i2c_write_16b(client, 0xF038, 0x002A);
			sensor_i2c_write_16b(client, 0xF03A, 0x0000);
			sensor_i2c_write_16b(client, 0xE002, 0x0490);
			sensor_i2c_write_16b(client, 0xFFFE, 1);
			msleep(100);
		} else {
			dev_err(&client->dev, "timeout: line[%d]\n", __LINE__);
			return -EINVAL;
		}
	} else {
		sensor_i2c_write_16b(client, 0xFFFE, 0);
		for (timeout = 0 ; timeout < 500 ; timeout ++) {
			usleep_range(9000, 10000);
			sensor_i2c_read_16b(client, 0, &v);
			if (v != 0)
				break;
		}
		if (timeout >= 500) {
		 dev_err(&client->dev, "timeout: line[%d]v=%x\n", __LINE__, v);
		 return -EINVAL;
		}
		for (timeout = 0 ; timeout < 500 ; timeout ++) {
			if(check_sensor_chip_id(client, &v) == 0) {
				if (v == 0x356) {
					dev_dbg(&client->dev, "sensor check: v=0x%x\nrecover status from fw stall gate frames.\n", v);
					sensor_i2c_write_16b(client, 0x601a, 0x8340);
					msleep(10);
					break;
				}
			}
		}

		for (timeout = 0 ; timeout < 100 ; timeout ++) {
			usleep_range(9000, 10000);
			sensor_i2c_read_16b(client, 0x601a, &v);
			if ((v & 0x200) == 0x200)
				break;
		}
		if ( (v & 0x200) != 0x200 ) {
			dev_dbg(&client->dev, "stop waking up: camera is working.\n");
			return 0;
		}

		sensor_i2c_write_16b(client, 0x601a, 0x241);
		usleep_range(1000, 2000);
		for (timeout = 0 ; timeout < 100 ; timeout ++) {
			usleep_range(9000, 10000);
			sensor_i2c_read_16b(client, 0x601a, &v);
			if ((v & 1) == 0)
				break;
		}
		if (timeout >= 100) {
			dev_err(&client->dev, "timeout: line[%d]v=%x\n", __LINE__, v);
			return -EINVAL;
		}

		for (timeout = 0 ; timeout < 100 ; timeout ++) {
			usleep_range(9000, 10000);
			sensor_i2c_read_16b(client, 0x601a, &v);
			if ((v & 0x8000) == 0x8000)
				break;
		}
		if (timeout >= 100) {
			dev_err(&client->dev, "timeout: line[%d]v=%x\n", __LINE__, v);
			return -EINVAL;
		}

		sensor_i2c_write_16b(client, 0x601a, 0x8250);
		for (timeout = 0 ; timeout < 100 ; timeout ++) {
			usleep_range(9000, 10000);
			sensor_i2c_read_16b(client, 0x601a, &v);
			if ((v & 0x8040) == 0x8040)
				break;
		}
		if (timeout >= 100) {
			dev_err(&client->dev, "timeout: line[%d]v=%x\n", __LINE__, v);
			return -EINVAL;
		}
		sensor_i2c_write_16b(client, 0xF056, 0x0000);

		dev_dbg(&client->dev, "sensor wake up\n");
	}

	return 0;
}

static int sensor_standby(struct i2c_client *client, int enable)
{
	u16 v = 0;
	int timeout;
	u16 checksum = 0;
	dev_dbg(&client->dev, "%s():enable=%d\n", __func__, enable);

	for (timeout = 0 ; timeout < 50 ; timeout ++) {
		usleep_range(9000, 10000);
		sensor_i2c_read_16b(client, 0x6134, &checksum);
		if (checksum == 0xFFFF)
			break;
	}
	if(checksum != 0xFFFF){
		return set_standby_mode_rel419(client, enable); // standby for rel419
	}

	if (enable == 1) {
		sensor_i2c_write_16b(client, 0x601a, 0x0180);
		for (timeout = 0 ; timeout < 500 ; timeout ++) {
			usleep_range(9000, 10000);
			sensor_i2c_read_16b(client, 0x601a, &v);
			if ((v & 0x200) == 0x200)
				break;
		}
		if (timeout < 500) {
			msleep(100);
		} else {
			dev_err(&client->dev, "timeout: line[%d]v=%x\n", __LINE__, v);
			return -EINVAL;
		}
	} else {
		sensor_i2c_write_16b(client, 0x601a, 0x0380);
		for (timeout = 0 ; timeout < 100 ; timeout ++) {
			usleep_range(9000, 10000);
			sensor_i2c_read_16b(client, 0x601a, &v);
			if ((v & 0x200) == 0)
				break;
		}
		if (timeout >= 100) {
			dev_err(&client->dev, "timeout: line[%d]v=%x\n", __LINE__, v);
			return -EINVAL;
		}
		dev_dbg(&client->dev, "sensor wake up\n");
	}

	return 0;
}

static int sensor_power_on(struct sensor *instance)
{
	dev_dbg(&instance->i2c_client->dev, "%s()\n", __func__);
	gpiod_set_value_cansleep(instance->host_power_gpio, 1);
	gpiod_set_value_cansleep(instance->device_power_gpio, 1);
	usleep_range(500, 5000);
	gpiod_set_value_cansleep(instance->reset_gpio, 1);
	msleep(10);

	return 0;
}

static int sensor_power_off(struct sensor *instance)
{
	dev_dbg(&instance->i2c_client->dev, "%s()\n", __func__);
	gpiod_set_value_cansleep(instance->standby_gpio, 0);
	gpiod_set_value_cansleep(instance->reset_gpio, 0);
	usleep_range(50, 500);
	gpiod_set_value_cansleep(instance->device_power_gpio, 0);
	gpiod_set_value_cansleep(instance->host_power_gpio, 0);
	msleep(10);

	return 0;
}

static int sensor_try_on(struct sensor *instance)
{
	u16 v;
	dev_dbg(&instance->i2c_client->dev, "%s()\n", __func__);

	sensor_power_off(instance);

	sensor_power_on(instance);

	if (sensor_i2c_read_16b(instance->i2c_client, 0, &v) != 0) {
		dev_err(&instance->i2c_client->dev, "%s() try on failed\n",
			__func__);
		sensor_power_off(instance);
		return -EINVAL;
	}

	return 0;
}

static int sensor_load_bootdata(struct sensor *instance)
{
#ifdef __FAKE__
	struct device *dev = &instance->i2c_client->dev;
	int index = 0;
	size_t len = 0;
	size_t pll_len = 0;
	u16 otp_data;
	u16 *bootdata_temp_area;
	u16 checksum;
	int i;
	const int len_each_time = 1024;

	bootdata_temp_area = devm_kzalloc(dev,
					  len_each_time + 2,
					  GFP_KERNEL);
	if (bootdata_temp_area == NULL) {
		dev_err(dev, "allocate memory failed\n");
		return -EINVAL;
	}

	checksum = ap1302_otp_flash_get_checksum(instance->otp_flash_instance);

//load pll
	bootdata_temp_area[0] = cpu_to_be16(BOOT_DATA_START_REG);
	pll_len = ap1302_otp_flash_get_pll_section(instance->otp_flash_instance,
					    (u8 *)(&bootdata_temp_area[1]));
	dev_dbg(dev, "load pll data of length [%zu] into register [%x]\n",
		pll_len, BOOT_DATA_START_REG);
	sensor_i2c_write_bust(instance->i2c_client, (u8 *)bootdata_temp_area, pll_len + 2);
	sensor_i2c_write_16b(instance->i2c_client, 0x6002, 2);
	msleep(1);

	//load bootdata part1
	bootdata_temp_area[0] = cpu_to_be16(BOOT_DATA_START_REG + pll_len);
	len = ap1302_otp_flash_read(instance->otp_flash_instance,
			     (u8 *)(&bootdata_temp_area[1]),
			     pll_len, len_each_time - pll_len);
	dev_dbg(dev, "load data of length [%zu] into register [%zx]\n",
		len, BOOT_DATA_START_REG + pll_len);
	sensor_i2c_write_bust(instance->i2c_client, (u8 *)bootdata_temp_area, len + 2);
	i = index = pll_len + len;

	//load bootdata ronaming
	while(len != 0) {
		while(i < BOOT_DATA_WRITE_LEN) {
			bootdata_temp_area[0] =
				cpu_to_be16(BOOT_DATA_START_REG + i);
			len = ap1302_otp_flash_read(instance->otp_flash_instance,
					     (u8 *)(&bootdata_temp_area[1]),
					     index, len_each_time);
			if (len == 0) {
				dev_dbg(dev, "length get zero\n");
				break;
			}

			dev_dbg(dev,
				"load ronaming data of length [%zu] into register [%x]\n",
				len,
				BOOT_DATA_START_REG + i);
			sensor_i2c_write_bust(instance->i2c_client,
					 (u8 *)bootdata_temp_area,
					 len + 2);
			index += len;
			i += len_each_time;
		}

		i = 0;
	}
#else
	struct device *dev = &instance->i2c_client->dev;
	int index = 0;
	size_t len = BOOT_DATA_WRITE_LEN;
	u16 otp_data;
	u16 *bootdata_temp_area;
	u16 checksum;

	bootdata_temp_area = devm_kzalloc(dev,
					  BOOT_DATA_WRITE_LEN + 2,
					  GFP_KERNEL);
	if (bootdata_temp_area == NULL) {
		dev_err(dev, "allocate memory failed\n");
		return -EINVAL;
	}

	checksum = ap1302_otp_flash_get_checksum(instance->otp_flash_instance);

	while(!(len < BOOT_DATA_WRITE_LEN)) {
		bootdata_temp_area[0] = cpu_to_be16(BOOT_DATA_START_REG);
		len = ap1302_otp_flash_read(instance->otp_flash_instance,
					    (u8 *)(&bootdata_temp_area[1]),
					    index, BOOT_DATA_WRITE_LEN);
		dev_dbg(dev, "index: 0x%04x, len [%zu]\n", index, len);
		sensor_i2c_write_bust(instance->i2c_client,
				      (u8 *)bootdata_temp_area,
				      len + 2);
		index += len;
	}
#endif
	sensor_i2c_write_16b(instance->i2c_client, 0x6002, 0xffff);
	devm_kfree(dev, bootdata_temp_area);

	index = 0;
	otp_data = 0;
	while(otp_data != checksum && index < 20) {
		msleep(10);
		sensor_i2c_read_16b(instance->i2c_client, 0x6134, &otp_data);
		index ++;
	}
	if (unlikely(index == 20)) {
		if (likely(otp_data == 0))
			dev_err(dev, "failed try to read checksum\n");
		else
			dev_err(dev, "bootdata checksum missmatch\n");

		return -EINVAL;
	}

	return 0;
}



static int sensor_init_controls(struct sensor *instance, int pixel_rate)
{
	int ret;
	struct v4l2_ctrl_handler *ctrl_hdlr;


	ctrl_hdlr = &instance->ctrl_handler;
	ret = v4l2_ctrl_handler_init(ctrl_hdlr, 9);
	if (ret)
		return ret;

	instance->pixel_rate = v4l2_ctrl_new_std(ctrl_hdlr, &ap1302_ctrl_ops,
		V4L2_CID_PIXEL_RATE,
		pixel_rate,
		pixel_rate, 1,
		pixel_rate);

	instance->pixel_rate->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	instance->v4l2_subdev.ctrl_handler = ctrl_hdlr;

	//v4l2_ctrl_handler_free(ctrl_hdlr);
	return 0;
}


static int sensor_probe(struct i2c_client *client, const struct i2c_device_id *id)
{
	struct sensor *instance = NULL;
	struct device *dev = &client->dev;
	struct v4l2_mbus_framefmt *fmt;
	int data_lanes;
	int continuous_clock;
	int pixel_rate;
	int ret;
	int retry_f;
	int i;

	dev_info(&client->dev, "%s() device node: %s\n",
				__func__, client->dev.of_node->full_name);

	instance = devm_kzalloc(dev, sizeof(struct sensor), GFP_KERNEL);
	if (instance == NULL) {
		dev_err(dev, "allocate memory failed\n");
		return -EINVAL;
	}
	instance->i2c_client = client;

	instance->host_power_gpio = devm_gpiod_get_optional(dev, "host-power", GPIOD_OUT_LOW);
	if (IS_ERR(instance->host_power_gpio)) {
		ret = PTR_ERR(instance->host_power_gpio);
		if (ret != -EPROBE_DEFER)
			dev_err(dev, "Cannot get host-power GPIO (%d)", ret);
		return ret;
	}
	instance->device_power_gpio = devm_gpiod_get_optional(dev, "device-power", GPIOD_OUT_LOW);
	if (IS_ERR(instance->device_power_gpio)) {
		ret = PTR_ERR(instance->device_power_gpio);
		if (ret != -EPROBE_DEFER)
			dev_err(dev, "Cannot get device-power GPIO (%d)", ret);
		return ret;
	}
	instance->reset_gpio = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(instance->reset_gpio)) {
		ret = PTR_ERR(instance->reset_gpio);
		if (ret != -EPROBE_DEFER)
			dev_err(dev, "Cannot get reset GPIO (%d)", ret);
		return ret;
	}
	instance->standby_gpio = devm_gpiod_get_optional(dev, "standby", GPIOD_OUT_LOW);
	if (IS_ERR(instance->standby_gpio)) {
		ret = PTR_ERR(instance->standby_gpio);
		if (ret != -EPROBE_DEFER)
			dev_err(dev, "Cannot get standby GPIO (%d)", ret);
		return ret;
	}

	pixel_rate = 0;
	if (of_property_read_u32(dev->of_node, "pixel-rate", &pixel_rate) == 0) {
		if ((pixel_rate < 0)) {
			dev_err(dev, "value of 'pixel-rate' property is invaild\n");
			pixel_rate = 0;
		}
	}

	sensor_init_controls(instance, pixel_rate);

	data_lanes = 4;
	if (of_property_read_u32(dev->of_node, "data-lanes", &data_lanes) == 0) {
		if ((data_lanes < 1) || (data_lanes > 4)) {
			dev_err(dev, "value of 'data-lanes' property is invaild\n");
			data_lanes = 4;
		}
	}

	continuous_clock = 0;
	if (of_property_read_u32(dev->of_node, "continuous-clock",
				 &continuous_clock) == 0) {
		if (continuous_clock > 1) {
			dev_err(dev, "value of 'continuous-clock' property is invaild\n");
			continuous_clock = 0;
		}
	}
	dev_dbg(dev, "data-lanes [%d] ,continuous-clock [%d]\n",
		data_lanes, continuous_clock);

	retry_f = 0x01;
	/*
	bit 0: start bit
	bit 1: sensor_try_on fail
	bit 2: ap1302_otp_flash_init fail
	bit 3: sensor_load_bootdata fail
	bit 4-7: retry count
	*/
	while(retry_f) {
		retry_f &= ~0x01;

		if (sensor_try_on(instance) != 0) {
			retry_f |= 0x02 ;
		}

		instance->otp_flash_instance = ap1302_otp_flash_init(dev);
		if(IS_ERR(instance->otp_flash_instance)) {
			dev_err(dev, "otp flash init failed\n");
			// retry_f |= 0x04 ;
			return -EINVAL;
		}
#ifndef __FAKE__
		for(i = 0 ; i < ARRAY_SIZE(ap1302_sensor_table); i++)
		{
			if (strcmp((const char*)instance->otp_flash_instance->product_name, ap1302_sensor_table[i].sensor_name) == 0)
				break;
		}
		instance->selected_sensor = i;
		dev_dbg(dev, "selected_sensor:%d, sensor_name:%s\n", i, instance->otp_flash_instance->product_name);

#endif
		if(sensor_load_bootdata(instance) != 0) {
			dev_err(dev, "load bootdata failed\n");
			retry_f |= 0x08 ;
		}

		if ((retry_f & 0x0F) != 0x00) {
			if (((retry_f & 0x30) >> 4 ) < 3 ) {
				retry_f += 1 << 4;
				retry_f &= ~0x0F;
				dev_err(dev, "Probe retry:%d.\n", ((retry_f & 0x30) >> 4 ));
			}
			else {
				retry_f &= 0x00;
				dev_dbg(dev, "Probe retry failed\n");
				return  -EINVAL;
			}
		}
	}

	fmt = &instance->fmt;
	fmt->width = ap1302_sensor_table[instance->selected_sensor].res_list[0].width;
	fmt->height = ap1302_sensor_table[instance->selected_sensor].res_list[0].height;
	fmt->field = V4L2_FIELD_NONE;
	fmt->code = MEDIA_BUS_FMT_UYVY8_2X8;
	fmt->colorspace =  V4L2_COLORSPACE_SRGB;
	fmt->ycbcr_enc = V4L2_MAP_YCBCR_ENC_DEFAULT(fmt->colorspace);
	fmt->quantization = V4L2_QUANTIZATION_FULL_RANGE;
	fmt->xfer_func = V4L2_MAP_XFER_FUNC_DEFAULT(fmt->colorspace);
	memset(fmt->reserved, 0, sizeof(fmt->reserved));

	v4l2_i2c_subdev_init(&instance->v4l2_subdev,
			     instance->i2c_client, &sensor_subdev_ops);
	instance->v4l2_subdev.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE | V4L2_SUBDEV_FL_HAS_EVENTS | V4L2_SUBDEV_FL_MULTIPLEXED;
	instance->pad.flags = MEDIA_PAD_FL_SOURCE;
	instance->v4l2_subdev.entity.function = MEDIA_ENT_F_CAM_SENSOR;
	ret = media_entity_pads_init(&instance->v4l2_subdev.entity, 1, &instance->pad);

	ret = v4l2_subdev_init_finalize(&instance->v4l2_subdev);
	if (ret < 0)
		return -EINVAL;

	ret += v4l2_async_register_subdev(&instance->v4l2_subdev);
	if (ret != 0) {
		dev_err(&instance->i2c_client->dev, "v4l2 register failed\n");
		return -EINVAL;
	}

	//set something reference from DevX tool register log
	//cntx select 'Video'
	sensor_i2c_write_16b(instance->i2c_client, 0x1184, 1); //ATOMIC

	sensor_i2c_write_16b(instance->i2c_client, 0x1000, 0); //CTRL
	sensor_i2c_write_16b(instance->i2c_client, 0x1184, 0xb); //ATOMIC
	msleep(1);
	sensor_i2c_write_16b(instance->i2c_client, 0x1184, 1); //ATOMIC
	//Video output 1920x1080,YUV422
	sensor_i2c_write_16b(instance->i2c_client, 0x2000, ap1302_sensor_table[instance->selected_sensor].res_list[0].width); //VIDEO_WIDTH
	sensor_i2c_write_16b(instance->i2c_client, 0x2002, ap1302_sensor_table[instance->selected_sensor].res_list[0].height); //VIDEO_HEIGHT
	sensor_i2c_write_16b(instance->i2c_client, 0x2012, 0x50); //VIDEO_OUT_FMT
	//Video max fps 30
	sensor_i2c_write_16b(instance->i2c_client, 0x2020, 0x1e00); //VIDEO_MAX_FPS
	//continuous clock, data-lanes
	sensor_i2c_write_16b(instance->i2c_client, 0x2030,
			     0x10 | (continuous_clock << 5) | (data_lanes)); //VIDEO_HINF_CTRL
	//sensor_i2c_write_16b(instance->i2c_client, 0x4014, 0); //VIDEO_SENSOR_MODE
	sensor_i2c_write_16b(instance->i2c_client, 0x1184, 0xb); //ATOMIC

	//let ap1302 go to standby mode
	ret = sensor_standby(instance->i2c_client, 1);
	if (ret == 0)
		dev_info(&client->dev, "probe success\n");
	else
		dev_err(&client->dev, "probe failed\n");

	return ret;
}

static int sensor_remove(struct i2c_client *client)
{
	return 0;
}

static const struct i2c_device_id sensor_id[] = {
	{ "tevi-ap1302", 0 },
	{}
};
MODULE_DEVICE_TABLE(i2c, sensor_id);

static const struct of_device_id sensor_of[] = {
	{ .compatible = "tn,tevi-ap1302" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, sensor_of);

static struct i2c_driver sensor_i2c_driver = {
	.driver = {
		.of_match_table = of_match_ptr(sensor_of),
		.name  = "tevi-ap1302",
	},
	.probe = sensor_probe,
	.remove = sensor_remove,
	.id_table = sensor_id,
};

module_i2c_driver(sensor_i2c_driver);

MODULE_AUTHOR("TECHNEXION Inc.");
MODULE_DESCRIPTION("TechNexion driver for TEVI-AR Series");
MODULE_LICENSE("GPL");
MODULE_VERSION("1.0");
MODULE_ALIAS("Camera");
