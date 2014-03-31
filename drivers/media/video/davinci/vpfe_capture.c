/*
 * Copyright (C) 2008-2009 Texas Instruments Inc
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307 USA
 *
 * Driver name : VPFE Capture driver
 *    VPFE Capture driver allows applications to capture and stream video
 *    frames on DaVinci SoCs (DM6446, DM355 etc) from a YUV source such as
 *    TVP5146 or  Raw Bayer RGB image data from an image sensor
 *    such as Microns' MT9T001, MT9T031 etc.
 *
 *    These SoCs have, in common, a Video Processing Subsystem (VPSS) that
 *    consists of a Video Processing Front End (VPFE) for capturing
 *    video/raw image data and Video Processing Back End (VPBE) for displaying
 *    YUV data through an in-built analog encoder or Digital LCD port. This
 *    driver is for capture through VPFE. A typical EVM using these SoCs have
 *    following high level configuration.
 *
 *
 *    decoder(TVP5146/		YUV/
 * 	     MT9T001)   -->  Raw Bayer RGB ---> MUX -> VPFE (CCDC/ISIF)
 *    				data input              |      |
 *							V      |
 *						      SDRAM    |
 *							       V
 *							   Image Processor
 *							       |
 *							       V
 *							     SDRAM
 *    The data flow happens from a decoder connected to the VPFE over a
 *    YUV embedded (BT.656/BT.1120) or separate sync or raw bayer rgb interface
 *    and to the input of VPFE through an optional MUX (if more inputs are
 *    to be interfaced on the EVM). The input data is first passed through
 *    CCDC (CCD Controller, a.k.a Image Sensor Interface, ISIF). The CCDC
 *    does very little or no processing on YUV data and does pre-process Raw
 *    Bayer RGB data through modules such as Defect Pixel Correction (DFC)
 *    Color Space Conversion (CSC), data gain/offset etc. After this, data
 *    can be written to SDRAM or can be connected to the image processing
 *    block such as IPIPE (on DM355 only).
 *
 *    Features supported
 *  		- MMAP IO
 *		- Capture using TVP5146 over BT.656
 *		- support for interfacing decoders using sub device model
 *		- Work with DM355 or DM6446 CCDC to do Raw Bayer RGB/YUV
 *		  data capture to SDRAM.
 *    TODO list
 *		- Support multiple REQBUF after open
 *		- Support for de-allocating buffers through REQBUF
 *		- Support for Raw Bayer RGB capture
 *		- Support for chaining Image Processor
 *		- Support for static allocation of buffers
 *		- Support for USERPTR IO
 *		- Support for STREAMON before QBUF
 *		- Support for control ioctls
 */
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/platform_device.h>
#include <linux/interrupt.h>
#include <media/v4l2-common.h>
#include <linux/io.h>
#include <media/davinci/vpfe_capture.h>
#include "ccdc_hw_device.h"

static int debug;
static u32 numbuffers = 3;
static u32 bufsize = (720 * 576 * 2);

module_param(numbuffers, uint, S_IRUGO);
module_param(bufsize, uint, S_IRUGO);
module_param(debug, int, 0644);

MODULE_PARM_DESC(numbuffers, "buffer count (default:3)");
MODULE_PARM_DESC(bufsize, "buffer size in bytes (default:720 x 576 x 2)");
MODULE_PARM_DESC(debug, "Debug level 0-1");

MODULE_DESCRIPTION("VPFE Video for Linux Capture Driver");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Texas Instruments");

struct vpfe_standard {
	v4l2_std_id std_id;
	unsigned int width;
	unsigned int height;
	struct v4l2_fract pixelaspect;
	
	int frame_format;
};

struct ccdc_config {
	
	int vpfe_probed;
	
	char name[32];
};

static struct vpfe_config_params config_params = {
	.min_numbuffers = 3,
	.numbuffers = 3,
	.min_bufsize = 720 * 480 * 2,
	.device_bufsize = 720 * 576 * 2,
};

static struct ccdc_hw_device *ccdc_dev;
static DEFINE_MUTEX(ccdc_lock);
static struct ccdc_config *ccdc_cfg;

const struct vpfe_standard vpfe_standards[] = {
	{V4L2_STD_525_60, 720, 480, {11, 10}, 1},
	{V4L2_STD_625_50, 720, 576, {54, 59}, 1},
};

static const struct vpfe_pixel_format vpfe_pix_fmts[] = {
	{
		.fmtdesc = {
			.index = 0,
			.type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
			.description = "Bayer GrRBGb 8bit A-Law compr.",
			.pixelformat = V4L2_PIX_FMT_SBGGR8,
		},
		.bpp = 1,
	},
	{
		.fmtdesc = {
			.index = 1,
			.type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
			.description = "Bayer GrRBGb - 16bit",
			.pixelformat = V4L2_PIX_FMT_SBGGR16,
		},
		.bpp = 2,
	},
	{
		.fmtdesc = {
			.index = 2,
			.type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
			.description = "Bayer GrRBGb 8bit DPCM compr.",
			.pixelformat = V4L2_PIX_FMT_SGRBG10DPCM8,
		},
		.bpp = 1,
	},
	{
		.fmtdesc = {
			.index = 3,
			.type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
			.description = "YCbCr 4:2:2 Interleaved UYVY",
			.pixelformat = V4L2_PIX_FMT_UYVY,
		},
		.bpp = 2,
	},
	{
		.fmtdesc = {
			.index = 4,
			.type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
			.description = "YCbCr 4:2:2 Interleaved YUYV",
			.pixelformat = V4L2_PIX_FMT_YUYV,
		},
		.bpp = 2,
	},
	{
		.fmtdesc = {
			.index = 5,
			.type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
			.description = "Y/CbCr 4:2:0 - Semi planar",
			.pixelformat = V4L2_PIX_FMT_NV12,
		},
		.bpp = 1,
	},
};

static const struct vpfe_pixel_format *vpfe_lookup_pix_format(u32 pix_format)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(vpfe_pix_fmts); i++) {
		if (pix_format == vpfe_pix_fmts[i].fmtdesc.pixelformat)
			return &vpfe_pix_fmts[i];
	}
	return NULL;
}

int vpfe_register_ccdc_device(struct ccdc_hw_device *dev)
{
	int ret = 0;
	printk(KERN_NOTICE "vpfe_register_ccdc_device: %s\n", dev->name);

	BUG_ON(!dev->hw_ops.open);
	BUG_ON(!dev->hw_ops.enable);
	BUG_ON(!dev->hw_ops.set_hw_if_params);
	BUG_ON(!dev->hw_ops.configure);
	BUG_ON(!dev->hw_ops.set_buftype);
	BUG_ON(!dev->hw_ops.get_buftype);
	BUG_ON(!dev->hw_ops.enum_pix);
	BUG_ON(!dev->hw_ops.set_frame_format);
	BUG_ON(!dev->hw_ops.get_frame_format);
	BUG_ON(!dev->hw_ops.get_pixel_format);
	BUG_ON(!dev->hw_ops.set_pixel_format);
	BUG_ON(!dev->hw_ops.set_image_window);
	BUG_ON(!dev->hw_ops.get_image_window);
	BUG_ON(!dev->hw_ops.get_line_length);
	BUG_ON(!dev->hw_ops.getfid);

	mutex_lock(&ccdc_lock);
	if (NULL == ccdc_cfg) {
		printk(KERN_ERR "vpfe capture not initialized\n");
		ret = -EFAULT;
		goto unlock;
	}

	if (strcmp(dev->name, ccdc_cfg->name)) {
		
		ret = -EINVAL;
		goto unlock;
	}

	if (ccdc_dev) {
		printk(KERN_ERR "ccdc already registered\n");
		ret = -EINVAL;
		goto unlock;
	}

	ccdc_dev = dev;
unlock:
	mutex_unlock(&ccdc_lock);
	return ret;
}
EXPORT_SYMBOL(vpfe_register_ccdc_device);

void vpfe_unregister_ccdc_device(struct ccdc_hw_device *dev)
{
	if (NULL == dev) {
		printk(KERN_ERR "invalid ccdc device ptr\n");
		return;
	}

	printk(KERN_NOTICE "vpfe_unregister_ccdc_device, dev->name = %s\n",
		dev->name);

	if (strcmp(dev->name, ccdc_cfg->name)) {
		
		return;
	}

	mutex_lock(&ccdc_lock);
	ccdc_dev = NULL;
	mutex_unlock(&ccdc_lock);
	return;
}
EXPORT_SYMBOL(vpfe_unregister_ccdc_device);

static int vpfe_get_ccdc_image_format(struct vpfe_device *vpfe_dev,
				 struct v4l2_format *f)
{
	struct v4l2_rect image_win;
	enum ccdc_buftype buf_type;
	enum ccdc_frmfmt frm_fmt;

	memset(f, 0, sizeof(*f));
	f->type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	ccdc_dev->hw_ops.get_image_window(&image_win);
	f->fmt.pix.width = image_win.width;
	f->fmt.pix.height = image_win.height;
	f->fmt.pix.bytesperline = ccdc_dev->hw_ops.get_line_length();
	f->fmt.pix.sizeimage = f->fmt.pix.bytesperline *
				f->fmt.pix.height;
	buf_type = ccdc_dev->hw_ops.get_buftype();
	f->fmt.pix.pixelformat = ccdc_dev->hw_ops.get_pixel_format();
	frm_fmt = ccdc_dev->hw_ops.get_frame_format();
	if (frm_fmt == CCDC_FRMFMT_PROGRESSIVE)
		f->fmt.pix.field = V4L2_FIELD_NONE;
	else if (frm_fmt == CCDC_FRMFMT_INTERLACED) {
		if (buf_type == CCDC_BUFTYPE_FLD_INTERLEAVED)
			f->fmt.pix.field = V4L2_FIELD_INTERLACED;
		else if (buf_type == CCDC_BUFTYPE_FLD_SEPARATED)
			f->fmt.pix.field = V4L2_FIELD_SEQ_TB;
		else {
			v4l2_err(&vpfe_dev->v4l2_dev, "Invalid buf_type\n");
			return -EINVAL;
		}
	} else {
		v4l2_err(&vpfe_dev->v4l2_dev, "Invalid frm_fmt\n");
		return -EINVAL;
	}
	return 0;
}

static int vpfe_config_ccdc_image_format(struct vpfe_device *vpfe_dev)
{
	enum ccdc_frmfmt frm_fmt = CCDC_FRMFMT_INTERLACED;
	int ret = 0;

	if (ccdc_dev->hw_ops.set_pixel_format(
			vpfe_dev->fmt.fmt.pix.pixelformat) < 0) {
		v4l2_err(&vpfe_dev->v4l2_dev,
			"couldn't set pix format in ccdc\n");
		return -EINVAL;
	}
	
	ccdc_dev->hw_ops.set_image_window(&vpfe_dev->crop);

	switch (vpfe_dev->fmt.fmt.pix.field) {
	case V4L2_FIELD_INTERLACED:
		
		ret = ccdc_dev->hw_ops.set_buftype(
				CCDC_BUFTYPE_FLD_INTERLEAVED);
		break;
	case V4L2_FIELD_NONE:
		frm_fmt = CCDC_FRMFMT_PROGRESSIVE;
		
		break;
	case V4L2_FIELD_SEQ_TB:
		ret = ccdc_dev->hw_ops.set_buftype(
				CCDC_BUFTYPE_FLD_SEPARATED);
		break;
	default:
		return -EINVAL;
	}

	
	if (!ret)
		ret = ccdc_dev->hw_ops.set_frame_format(frm_fmt);
	return ret;
}
static int vpfe_config_image_format(struct vpfe_device *vpfe_dev,
				    const v4l2_std_id *std_id)
{
	struct vpfe_subdev_info *sdinfo = vpfe_dev->current_subdev;
	struct v4l2_mbus_framefmt mbus_fmt;
	struct v4l2_pix_format *pix = &vpfe_dev->fmt.fmt.pix;
	int i, ret = 0;

	for (i = 0; i < ARRAY_SIZE(vpfe_standards); i++) {
		if (vpfe_standards[i].std_id & *std_id) {
			vpfe_dev->std_info.active_pixels =
					vpfe_standards[i].width;
			vpfe_dev->std_info.active_lines =
					vpfe_standards[i].height;
			vpfe_dev->std_info.frame_format =
					vpfe_standards[i].frame_format;
			vpfe_dev->std_index = i;
			break;
		}
	}

	if (i ==  ARRAY_SIZE(vpfe_standards)) {
		v4l2_err(&vpfe_dev->v4l2_dev, "standard not supported\n");
		return -EINVAL;
	}

	vpfe_dev->crop.top = 0;
	vpfe_dev->crop.left = 0;
	vpfe_dev->crop.width = vpfe_dev->std_info.active_pixels;
	vpfe_dev->crop.height = vpfe_dev->std_info.active_lines;
	pix->width = vpfe_dev->crop.width;
	pix->height = vpfe_dev->crop.height;

	
	if (vpfe_dev->std_info.frame_format) {
		pix->field = V4L2_FIELD_INTERLACED;
		
		pix->pixelformat = V4L2_PIX_FMT_UYVY;
		v4l2_fill_mbus_format(&mbus_fmt, pix,
				V4L2_MBUS_FMT_YUYV10_2X10);
	} else {
		pix->field = V4L2_FIELD_NONE;
		
		pix->pixelformat = V4L2_PIX_FMT_SBGGR8;
		v4l2_fill_mbus_format(&mbus_fmt, pix,
				V4L2_MBUS_FMT_SBGGR8_1X8);
	}

	
	ret = v4l2_device_call_until_err(&vpfe_dev->v4l2_dev,
			sdinfo->grp_id, video, g_mbus_fmt, &mbus_fmt);

	if (ret && ret != -ENOIOCTLCMD) {
		v4l2_err(&vpfe_dev->v4l2_dev,
			"error in getting g_mbus_fmt from sub device\n");
		return ret;
	}
	v4l2_fill_pix_format(pix, &mbus_fmt);
	pix->bytesperline = pix->width * 2;
	pix->sizeimage = pix->bytesperline * pix->height;

	
	ret = vpfe_config_ccdc_image_format(vpfe_dev);
	if (ret)
		return ret;

	
	if (!ret) {
		pix->bytesperline = ccdc_dev->hw_ops.get_line_length();
		pix->sizeimage = pix->bytesperline * pix->height;
	}
	return ret;
}

static int vpfe_initialize_device(struct vpfe_device *vpfe_dev)
{
	int ret = 0;

	
	vpfe_dev->current_input = 0;

	
	vpfe_dev->std_index = 0;

	
	ret = vpfe_config_image_format(vpfe_dev,
				&vpfe_standards[vpfe_dev->std_index].std_id);
	if (ret)
		return ret;

	
	mutex_lock(&ccdc_lock);
	if (NULL == ccdc_dev) {
		v4l2_err(&vpfe_dev->v4l2_dev, "ccdc device not registered\n");
		ret = -ENODEV;
		goto unlock;
	}

	if (!try_module_get(ccdc_dev->owner)) {
		v4l2_err(&vpfe_dev->v4l2_dev, "Couldn't lock ccdc module\n");
		ret = -ENODEV;
		goto unlock;
	}
	ret = ccdc_dev->hw_ops.open(vpfe_dev->pdev);
	if (!ret)
		vpfe_dev->initialized = 1;

	
	if (vpfe_dev->cfg->clr_intr)
		vpfe_dev->cfg->clr_intr(-1);

unlock:
	mutex_unlock(&ccdc_lock);
	return ret;
}

static int vpfe_open(struct file *file)
{
	struct vpfe_device *vpfe_dev = video_drvdata(file);
	struct vpfe_fh *fh;

	v4l2_dbg(1, debug, &vpfe_dev->v4l2_dev, "vpfe_open\n");

	if (!vpfe_dev->cfg->num_subdevs) {
		v4l2_err(&vpfe_dev->v4l2_dev, "No decoder registered\n");
		return -ENODEV;
	}

	
	fh = kmalloc(sizeof(struct vpfe_fh), GFP_KERNEL);
	if (NULL == fh) {
		v4l2_err(&vpfe_dev->v4l2_dev,
			"unable to allocate memory for file handle object\n");
		return -ENOMEM;
	}
	
	file->private_data = fh;
	fh->vpfe_dev = vpfe_dev;
	mutex_lock(&vpfe_dev->lock);
	
	if (!vpfe_dev->initialized) {
		if (vpfe_initialize_device(vpfe_dev)) {
			mutex_unlock(&vpfe_dev->lock);
			return -ENODEV;
		}
	}
	
	vpfe_dev->usrs++;
	
	fh->io_allowed = 0;
	
	fh->prio = V4L2_PRIORITY_UNSET;
	v4l2_prio_open(&vpfe_dev->prio, &fh->prio);
	mutex_unlock(&vpfe_dev->lock);
	return 0;
}

static void vpfe_schedule_next_buffer(struct vpfe_device *vpfe_dev)
{
	unsigned long addr;

	vpfe_dev->next_frm = list_entry(vpfe_dev->dma_queue.next,
					struct videobuf_buffer, queue);
	list_del(&vpfe_dev->next_frm->queue);
	vpfe_dev->next_frm->state = VIDEOBUF_ACTIVE;
	addr = videobuf_to_dma_contig(vpfe_dev->next_frm);

	ccdc_dev->hw_ops.setfbaddr(addr);
}

static void vpfe_schedule_bottom_field(struct vpfe_device *vpfe_dev)
{
	unsigned long addr;

	addr = videobuf_to_dma_contig(vpfe_dev->cur_frm);
	addr += vpfe_dev->field_off;
	ccdc_dev->hw_ops.setfbaddr(addr);
}

static void vpfe_process_buffer_complete(struct vpfe_device *vpfe_dev)
{
	struct timeval timevalue;

	do_gettimeofday(&timevalue);
	vpfe_dev->cur_frm->ts = timevalue;
	vpfe_dev->cur_frm->state = VIDEOBUF_DONE;
	vpfe_dev->cur_frm->size = vpfe_dev->fmt.fmt.pix.sizeimage;
	wake_up_interruptible(&vpfe_dev->cur_frm->done);
	vpfe_dev->cur_frm = vpfe_dev->next_frm;
}

static irqreturn_t vpfe_isr(int irq, void *dev_id)
{
	struct vpfe_device *vpfe_dev = dev_id;
	enum v4l2_field field;
	int fid;

	v4l2_dbg(1, debug, &vpfe_dev->v4l2_dev, "\nStarting vpfe_isr...\n");
	field = vpfe_dev->fmt.fmt.pix.field;

	
	if (!vpfe_dev->started)
		goto clear_intr;

	
	if (NULL != ccdc_dev->hw_ops.reset)
		ccdc_dev->hw_ops.reset();

	if (field == V4L2_FIELD_NONE) {
		
		v4l2_dbg(1, debug, &vpfe_dev->v4l2_dev,
			"frame format is progressive...\n");
		if (vpfe_dev->cur_frm != vpfe_dev->next_frm)
			vpfe_process_buffer_complete(vpfe_dev);
		goto clear_intr;
	}

	
	fid = ccdc_dev->hw_ops.getfid();

	
	vpfe_dev->field_id ^= 1;
	v4l2_dbg(1, debug, &vpfe_dev->v4l2_dev, "field id = %x:%x.\n",
		fid, vpfe_dev->field_id);
	if (fid == vpfe_dev->field_id) {
		
		if (fid == 0) {
			if (vpfe_dev->cur_frm != vpfe_dev->next_frm)
				vpfe_process_buffer_complete(vpfe_dev);
			if (field == V4L2_FIELD_SEQ_TB) {
				vpfe_schedule_bottom_field(vpfe_dev);
			}
			goto clear_intr;
		}
		spin_lock(&vpfe_dev->dma_queue_lock);
		if (!list_empty(&vpfe_dev->dma_queue) &&
		    vpfe_dev->cur_frm == vpfe_dev->next_frm)
			vpfe_schedule_next_buffer(vpfe_dev);
		spin_unlock(&vpfe_dev->dma_queue_lock);
	} else if (fid == 0) {
		vpfe_dev->field_id = fid;
	}
clear_intr:
	if (vpfe_dev->cfg->clr_intr)
		vpfe_dev->cfg->clr_intr(irq);

	return IRQ_HANDLED;
}

static irqreturn_t vdint1_isr(int irq, void *dev_id)
{
	struct vpfe_device *vpfe_dev = dev_id;

	v4l2_dbg(1, debug, &vpfe_dev->v4l2_dev, "\nInside vdint1_isr...\n");

	
	if (!vpfe_dev->started) {
		if (vpfe_dev->cfg->clr_intr)
			vpfe_dev->cfg->clr_intr(irq);
		return IRQ_HANDLED;
	}

	spin_lock(&vpfe_dev->dma_queue_lock);
	if ((vpfe_dev->fmt.fmt.pix.field == V4L2_FIELD_NONE) &&
	    !list_empty(&vpfe_dev->dma_queue) &&
	    vpfe_dev->cur_frm == vpfe_dev->next_frm)
		vpfe_schedule_next_buffer(vpfe_dev);
	spin_unlock(&vpfe_dev->dma_queue_lock);

	if (vpfe_dev->cfg->clr_intr)
		vpfe_dev->cfg->clr_intr(irq);

	return IRQ_HANDLED;
}

static void vpfe_detach_irq(struct vpfe_device *vpfe_dev)
{
	enum ccdc_frmfmt frame_format;

	frame_format = ccdc_dev->hw_ops.get_frame_format();
	if (frame_format == CCDC_FRMFMT_PROGRESSIVE)
		free_irq(vpfe_dev->ccdc_irq1, vpfe_dev);
}

static int vpfe_attach_irq(struct vpfe_device *vpfe_dev)
{
	enum ccdc_frmfmt frame_format;

	frame_format = ccdc_dev->hw_ops.get_frame_format();
	if (frame_format == CCDC_FRMFMT_PROGRESSIVE) {
		return request_irq(vpfe_dev->ccdc_irq1, vdint1_isr,
				    IRQF_DISABLED, "vpfe_capture1",
				    vpfe_dev);
	}
	return 0;
}

static void vpfe_stop_ccdc_capture(struct vpfe_device *vpfe_dev)
{
	vpfe_dev->started = 0;
	ccdc_dev->hw_ops.enable(0);
	if (ccdc_dev->hw_ops.enable_out_to_sdram)
		ccdc_dev->hw_ops.enable_out_to_sdram(0);
}

static int vpfe_release(struct file *file)
{
	struct vpfe_device *vpfe_dev = video_drvdata(file);
	struct vpfe_fh *fh = file->private_data;
	struct vpfe_subdev_info *sdinfo;
	int ret;

	v4l2_dbg(1, debug, &vpfe_dev->v4l2_dev, "vpfe_release\n");

	
	mutex_lock(&vpfe_dev->lock);
	
	if (fh->io_allowed) {
		if (vpfe_dev->started) {
			sdinfo = vpfe_dev->current_subdev;
			ret = v4l2_device_call_until_err(&vpfe_dev->v4l2_dev,
							 sdinfo->grp_id,
							 video, s_stream, 0);
			if (ret && (ret != -ENOIOCTLCMD))
				v4l2_err(&vpfe_dev->v4l2_dev,
				"stream off failed in subdev\n");
			vpfe_stop_ccdc_capture(vpfe_dev);
			vpfe_detach_irq(vpfe_dev);
			videobuf_streamoff(&vpfe_dev->buffer_queue);
		}
		vpfe_dev->io_usrs = 0;
		vpfe_dev->numbuffers = config_params.numbuffers;
	}

	
	vpfe_dev->usrs--;
	
	v4l2_prio_close(&vpfe_dev->prio, fh->prio);
	
	if (!vpfe_dev->usrs) {
		vpfe_dev->initialized = 0;
		if (ccdc_dev->hw_ops.close)
			ccdc_dev->hw_ops.close(vpfe_dev->pdev);
		module_put(ccdc_dev->owner);
	}
	mutex_unlock(&vpfe_dev->lock);
	file->private_data = NULL;
	
	kfree(fh);
	return 0;
}

static int vpfe_mmap(struct file *file, struct vm_area_struct *vma)
{
	
	struct vpfe_device *vpfe_dev = video_drvdata(file);

	v4l2_dbg(1, debug, &vpfe_dev->v4l2_dev, "vpfe_mmap\n");

	return videobuf_mmap_mapper(&vpfe_dev->buffer_queue, vma);
}

static unsigned int vpfe_poll(struct file *file, poll_table *wait)
{
	struct vpfe_device *vpfe_dev = video_drvdata(file);

	v4l2_dbg(1, debug, &vpfe_dev->v4l2_dev, "vpfe_poll\n");

	if (vpfe_dev->started)
		return videobuf_poll_stream(file,
					    &vpfe_dev->buffer_queue, wait);
	return 0;
}

static const struct v4l2_file_operations vpfe_fops = {
	.owner = THIS_MODULE,
	.open = vpfe_open,
	.release = vpfe_release,
	.unlocked_ioctl = video_ioctl2,
	.mmap = vpfe_mmap,
	.poll = vpfe_poll
};

static const struct vpfe_pixel_format *
	vpfe_check_format(struct vpfe_device *vpfe_dev,
			  struct v4l2_pix_format *pixfmt)
{
	u32 min_height = 1, min_width = 32, max_width, max_height;
	const struct vpfe_pixel_format *vpfe_pix_fmt;
	u32 pix;
	int temp, found;

	vpfe_pix_fmt = vpfe_lookup_pix_format(pixfmt->pixelformat);
	if (NULL == vpfe_pix_fmt) {
		pixfmt->pixelformat = vpfe_dev->fmt.fmt.pix.pixelformat;
		vpfe_pix_fmt = vpfe_lookup_pix_format(pixfmt->pixelformat);
	}

	
	temp = 0;
	found = 0;
	while (ccdc_dev->hw_ops.enum_pix(&pix, temp) >= 0) {
		if (vpfe_pix_fmt->fmtdesc.pixelformat == pix) {
			found = 1;
			break;
		}
		temp++;
	}

	if (!found) {
		
		pixfmt->pixelformat = vpfe_dev->fmt.fmt.pix.pixelformat;
		vpfe_pix_fmt = vpfe_lookup_pix_format(pixfmt->pixelformat);
	}

	
	if (pixfmt->field == V4L2_FIELD_ANY) {
		
		pixfmt->field = vpfe_dev->fmt.fmt.pix.field;
	}

	if (vpfe_dev->fmt.fmt.pix.field != pixfmt->field) {
		switch (pixfmt->field) {
		case V4L2_FIELD_INTERLACED:
		case V4L2_FIELD_SEQ_TB:
			
			if (!vpfe_dev->std_info.frame_format)
				pixfmt->field = V4L2_FIELD_NONE;
			break;
		case V4L2_FIELD_NONE:
			if (vpfe_dev->std_info.frame_format)
				pixfmt->field = V4L2_FIELD_INTERLACED;
			break;

		default:
			
			pixfmt->field = vpfe_dev->fmt.fmt.pix.field;
			break;
		}
	}

	
	if (pixfmt->field == V4L2_FIELD_INTERLACED ||
	    pixfmt->field == V4L2_FIELD_SEQ_TB)
		min_height = 2;

	max_width = vpfe_dev->std_info.active_pixels;
	max_height = vpfe_dev->std_info.active_lines;
	min_width /= vpfe_pix_fmt->bpp;

	v4l2_info(&vpfe_dev->v4l2_dev, "width = %d, height = %d, bpp = %d\n",
		  pixfmt->width, pixfmt->height, vpfe_pix_fmt->bpp);

	pixfmt->width = clamp((pixfmt->width), min_width, max_width);
	pixfmt->height = clamp((pixfmt->height), min_height, max_height);

	
	if (pixfmt->field == V4L2_FIELD_INTERLACED)
		pixfmt->height &= (~1);
	pixfmt->bytesperline = (((pixfmt->width * vpfe_pix_fmt->bpp) + 31)
				& ~31);
	if (pixfmt->pixelformat == V4L2_PIX_FMT_NV12)
		pixfmt->sizeimage =
			pixfmt->bytesperline * pixfmt->height +
			((pixfmt->bytesperline * pixfmt->height) >> 1);
	else
		pixfmt->sizeimage = pixfmt->bytesperline * pixfmt->height;

	v4l2_info(&vpfe_dev->v4l2_dev, "adjusted width = %d, height ="
		 " %d, bpp = %d, bytesperline = %d, sizeimage = %d\n",
		 pixfmt->width, pixfmt->height, vpfe_pix_fmt->bpp,
		 pixfmt->bytesperline, pixfmt->sizeimage);
	return vpfe_pix_fmt;
}

static int vpfe_querycap(struct file *file, void  *priv,
			       struct v4l2_capability *cap)
{
	struct vpfe_device *vpfe_dev = video_drvdata(file);

	v4l2_dbg(1, debug, &vpfe_dev->v4l2_dev, "vpfe_querycap\n");

	cap->version = VPFE_CAPTURE_VERSION_CODE;
	cap->capabilities = V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING;
	strlcpy(cap->driver, CAPTURE_DRV_NAME, sizeof(cap->driver));
	strlcpy(cap->bus_info, "VPFE", sizeof(cap->bus_info));
	strlcpy(cap->card, vpfe_dev->cfg->card_name, sizeof(cap->card));
	return 0;
}

static int vpfe_g_fmt_vid_cap(struct file *file, void *priv,
				struct v4l2_format *fmt)
{
	struct vpfe_device *vpfe_dev = video_drvdata(file);
	int ret = 0;

	v4l2_dbg(1, debug, &vpfe_dev->v4l2_dev, "vpfe_g_fmt_vid_cap\n");
	
	*fmt = vpfe_dev->fmt;
	return ret;
}

static int vpfe_enum_fmt_vid_cap(struct file *file, void  *priv,
				   struct v4l2_fmtdesc *fmt)
{
	struct vpfe_device *vpfe_dev = video_drvdata(file);
	const struct vpfe_pixel_format *pix_fmt;
	int temp_index;
	u32 pix;

	v4l2_dbg(1, debug, &vpfe_dev->v4l2_dev, "vpfe_enum_fmt_vid_cap\n");

	if (ccdc_dev->hw_ops.enum_pix(&pix, fmt->index) < 0)
		return -EINVAL;

	
	pix_fmt = vpfe_lookup_pix_format(pix);
	if (NULL != pix_fmt) {
		temp_index = fmt->index;
		*fmt = pix_fmt->fmtdesc;
		fmt->index = temp_index;
		return 0;
	}
	return -EINVAL;
}

static int vpfe_s_fmt_vid_cap(struct file *file, void *priv,
				struct v4l2_format *fmt)
{
	struct vpfe_device *vpfe_dev = video_drvdata(file);
	const struct vpfe_pixel_format *pix_fmts;
	int ret = 0;

	v4l2_dbg(1, debug, &vpfe_dev->v4l2_dev, "vpfe_s_fmt_vid_cap\n");

	
	if (vpfe_dev->started) {
		v4l2_err(&vpfe_dev->v4l2_dev, "Streaming is started\n");
		return -EBUSY;
	}

	
	pix_fmts = vpfe_check_format(vpfe_dev, &fmt->fmt.pix);

	if (NULL == pix_fmts)
		return -EINVAL;

	
	ret = mutex_lock_interruptible(&vpfe_dev->lock);
	if (ret)
		return ret;

	
	vpfe_detach_irq(vpfe_dev);
	vpfe_dev->fmt = *fmt;
	
	ret = vpfe_config_ccdc_image_format(vpfe_dev);
	mutex_unlock(&vpfe_dev->lock);
	return ret;
}

static int vpfe_try_fmt_vid_cap(struct file *file, void *priv,
				  struct v4l2_format *f)
{
	struct vpfe_device *vpfe_dev = video_drvdata(file);
	const struct vpfe_pixel_format *pix_fmts;

	v4l2_dbg(1, debug, &vpfe_dev->v4l2_dev, "vpfe_try_fmt_vid_cap\n");

	pix_fmts = vpfe_check_format(vpfe_dev, &f->fmt.pix);
	if (NULL == pix_fmts)
		return -EINVAL;
	return 0;
}

static int vpfe_get_subdev_input_index(struct vpfe_device *vpfe_dev,
					int *subdev_index,
					int *subdev_input_index,
					int app_input_index)
{
	struct vpfe_config *cfg = vpfe_dev->cfg;
	struct vpfe_subdev_info *sdinfo;
	int i, j = 0;

	for (i = 0; i < cfg->num_subdevs; i++) {
		sdinfo = &cfg->sub_devs[i];
		if (app_input_index < (j + sdinfo->num_inputs)) {
			*subdev_index = i;
			*subdev_input_index = app_input_index - j;
			return 0;
		}
		j += sdinfo->num_inputs;
	}
	return -EINVAL;
}

static int vpfe_get_app_input_index(struct vpfe_device *vpfe_dev,
				    int *app_input_index)
{
	struct vpfe_config *cfg = vpfe_dev->cfg;
	struct vpfe_subdev_info *sdinfo;
	int i, j = 0;

	for (i = 0; i < cfg->num_subdevs; i++) {
		sdinfo = &cfg->sub_devs[i];
		if (!strcmp(sdinfo->name, vpfe_dev->current_subdev->name)) {
			if (vpfe_dev->current_input >= sdinfo->num_inputs)
				return -1;
			*app_input_index = j + vpfe_dev->current_input;
			return 0;
		}
		j += sdinfo->num_inputs;
	}
	return -EINVAL;
}

static int vpfe_enum_input(struct file *file, void *priv,
				 struct v4l2_input *inp)
{
	struct vpfe_device *vpfe_dev = video_drvdata(file);
	struct vpfe_subdev_info *sdinfo;
	int subdev, index ;

	v4l2_dbg(1, debug, &vpfe_dev->v4l2_dev, "vpfe_enum_input\n");

	if (vpfe_get_subdev_input_index(vpfe_dev,
					&subdev,
					&index,
					inp->index) < 0) {
		v4l2_err(&vpfe_dev->v4l2_dev, "input information not found"
			 " for the subdev\n");
		return -EINVAL;
	}
	sdinfo = &vpfe_dev->cfg->sub_devs[subdev];
	memcpy(inp, &sdinfo->inputs[index], sizeof(struct v4l2_input));
	return 0;
}

static int vpfe_g_input(struct file *file, void *priv, unsigned int *index)
{
	struct vpfe_device *vpfe_dev = video_drvdata(file);

	v4l2_dbg(1, debug, &vpfe_dev->v4l2_dev, "vpfe_g_input\n");

	return vpfe_get_app_input_index(vpfe_dev, index);
}


static int vpfe_s_input(struct file *file, void *priv, unsigned int index)
{
	struct vpfe_device *vpfe_dev = video_drvdata(file);
	struct vpfe_subdev_info *sdinfo;
	int subdev_index, inp_index;
	struct vpfe_route *route;
	u32 input = 0, output = 0;
	int ret = -EINVAL;

	v4l2_dbg(1, debug, &vpfe_dev->v4l2_dev, "vpfe_s_input\n");

	ret = mutex_lock_interruptible(&vpfe_dev->lock);
	if (ret)
		return ret;

	if (vpfe_dev->started) {
		v4l2_err(&vpfe_dev->v4l2_dev, "Streaming is on\n");
		ret = -EBUSY;
		goto unlock_out;
	}

	if (vpfe_get_subdev_input_index(vpfe_dev,
					&subdev_index,
					&inp_index,
					index) < 0) {
		v4l2_err(&vpfe_dev->v4l2_dev, "invalid input index\n");
		goto unlock_out;
	}

	sdinfo = &vpfe_dev->cfg->sub_devs[subdev_index];
	route = &sdinfo->routes[inp_index];
	if (route && sdinfo->can_route) {
		input = route->input;
		output = route->output;
	}

	ret = v4l2_device_call_until_err(&vpfe_dev->v4l2_dev, sdinfo->grp_id,
					 video, s_routing, input, output, 0);

	if (ret) {
		v4l2_err(&vpfe_dev->v4l2_dev,
			"vpfe_doioctl:error in setting input in decoder\n");
		ret = -EINVAL;
		goto unlock_out;
	}
	vpfe_dev->current_subdev = sdinfo;
	vpfe_dev->current_input = index;
	vpfe_dev->std_index = 0;

	
	ret = ccdc_dev->hw_ops.set_hw_if_params(&sdinfo->ccdc_if_params);
	if (ret)
		goto unlock_out;

	
	ret = vpfe_config_image_format(vpfe_dev,
				&vpfe_standards[vpfe_dev->std_index].std_id);
unlock_out:
	mutex_unlock(&vpfe_dev->lock);
	return ret;
}

static int vpfe_querystd(struct file *file, void *priv, v4l2_std_id *std_id)
{
	struct vpfe_device *vpfe_dev = video_drvdata(file);
	struct vpfe_subdev_info *sdinfo;
	int ret = 0;

	v4l2_dbg(1, debug, &vpfe_dev->v4l2_dev, "vpfe_querystd\n");

	ret = mutex_lock_interruptible(&vpfe_dev->lock);
	sdinfo = vpfe_dev->current_subdev;
	if (ret)
		return ret;
	
	ret = v4l2_device_call_until_err(&vpfe_dev->v4l2_dev, sdinfo->grp_id,
					 video, querystd, std_id);
	mutex_unlock(&vpfe_dev->lock);
	return ret;
}

static int vpfe_s_std(struct file *file, void *priv, v4l2_std_id *std_id)
{
	struct vpfe_device *vpfe_dev = video_drvdata(file);
	struct vpfe_subdev_info *sdinfo;
	int ret = 0;

	v4l2_dbg(1, debug, &vpfe_dev->v4l2_dev, "vpfe_s_std\n");

	
	ret = mutex_lock_interruptible(&vpfe_dev->lock);
	if (ret)
		return ret;

	sdinfo = vpfe_dev->current_subdev;
	
	if (vpfe_dev->started) {
		v4l2_err(&vpfe_dev->v4l2_dev, "streaming is started\n");
		ret = -EBUSY;
		goto unlock_out;
	}

	ret = v4l2_device_call_until_err(&vpfe_dev->v4l2_dev, sdinfo->grp_id,
					 core, s_std, *std_id);
	if (ret < 0) {
		v4l2_err(&vpfe_dev->v4l2_dev, "Failed to set standard\n");
		goto unlock_out;
	}
	ret = vpfe_config_image_format(vpfe_dev, std_id);

unlock_out:
	mutex_unlock(&vpfe_dev->lock);
	return ret;
}

static int vpfe_g_std(struct file *file, void *priv, v4l2_std_id *std_id)
{
	struct vpfe_device *vpfe_dev = video_drvdata(file);

	v4l2_dbg(1, debug, &vpfe_dev->v4l2_dev, "vpfe_g_std\n");

	*std_id = vpfe_standards[vpfe_dev->std_index].std_id;
	return 0;
}
static int vpfe_videobuf_setup(struct videobuf_queue *vq,
				unsigned int *count,
				unsigned int *size)
{
	struct vpfe_fh *fh = vq->priv_data;
	struct vpfe_device *vpfe_dev = fh->vpfe_dev;

	v4l2_dbg(1, debug, &vpfe_dev->v4l2_dev, "vpfe_buffer_setup\n");
	*size = vpfe_dev->fmt.fmt.pix.sizeimage;
	if (vpfe_dev->memory == V4L2_MEMORY_MMAP &&
		vpfe_dev->fmt.fmt.pix.sizeimage > config_params.device_bufsize)
		*size = config_params.device_bufsize;

	if (*count < config_params.min_numbuffers)
		*count = config_params.min_numbuffers;
	v4l2_dbg(1, debug, &vpfe_dev->v4l2_dev,
		"count=%d, size=%d\n", *count, *size);
	return 0;
}

static int vpfe_videobuf_prepare(struct videobuf_queue *vq,
				struct videobuf_buffer *vb,
				enum v4l2_field field)
{
	struct vpfe_fh *fh = vq->priv_data;
	struct vpfe_device *vpfe_dev = fh->vpfe_dev;
	unsigned long addr;
	int ret;

	v4l2_dbg(1, debug, &vpfe_dev->v4l2_dev, "vpfe_buffer_prepare\n");

	
	if (VIDEOBUF_NEEDS_INIT == vb->state) {
		vb->width = vpfe_dev->fmt.fmt.pix.width;
		vb->height = vpfe_dev->fmt.fmt.pix.height;
		vb->size = vpfe_dev->fmt.fmt.pix.sizeimage;
		vb->field = field;

		ret = videobuf_iolock(vq, vb, NULL);
		if (ret < 0)
			return ret;

		addr = videobuf_to_dma_contig(vb);
		
		if (!ALIGN(addr, 32))
			return -EINVAL;

		vb->state = VIDEOBUF_PREPARED;
	}
	return 0;
}

static void vpfe_videobuf_queue(struct videobuf_queue *vq,
				struct videobuf_buffer *vb)
{
	
	struct vpfe_fh *fh = vq->priv_data;
	struct vpfe_device *vpfe_dev = fh->vpfe_dev;
	unsigned long flags;

	v4l2_dbg(1, debug, &vpfe_dev->v4l2_dev, "vpfe_buffer_queue\n");

	
	spin_lock_irqsave(&vpfe_dev->dma_queue_lock, flags);
	list_add_tail(&vb->queue, &vpfe_dev->dma_queue);
	spin_unlock_irqrestore(&vpfe_dev->dma_queue_lock, flags);

	
	vb->state = VIDEOBUF_QUEUED;
}

static void vpfe_videobuf_release(struct videobuf_queue *vq,
				  struct videobuf_buffer *vb)
{
	struct vpfe_fh *fh = vq->priv_data;
	struct vpfe_device *vpfe_dev = fh->vpfe_dev;
	unsigned long flags;

	v4l2_dbg(1, debug, &vpfe_dev->v4l2_dev, "vpfe_videobuf_release\n");

	spin_lock_irqsave(&vpfe_dev->dma_queue_lock, flags);
	INIT_LIST_HEAD(&vpfe_dev->dma_queue);
	spin_unlock_irqrestore(&vpfe_dev->dma_queue_lock, flags);
	videobuf_dma_contig_free(vq, vb);
	vb->state = VIDEOBUF_NEEDS_INIT;
}

static struct videobuf_queue_ops vpfe_videobuf_qops = {
	.buf_setup      = vpfe_videobuf_setup,
	.buf_prepare    = vpfe_videobuf_prepare,
	.buf_queue      = vpfe_videobuf_queue,
	.buf_release    = vpfe_videobuf_release,
};

static int vpfe_reqbufs(struct file *file, void *priv,
			struct v4l2_requestbuffers *req_buf)
{
	struct vpfe_device *vpfe_dev = video_drvdata(file);
	struct vpfe_fh *fh = file->private_data;
	int ret = 0;

	v4l2_dbg(1, debug, &vpfe_dev->v4l2_dev, "vpfe_reqbufs\n");

	if (V4L2_BUF_TYPE_VIDEO_CAPTURE != req_buf->type) {
		v4l2_err(&vpfe_dev->v4l2_dev, "Invalid buffer type\n");
		return -EINVAL;
	}

	ret = mutex_lock_interruptible(&vpfe_dev->lock);
	if (ret)
		return ret;

	if (vpfe_dev->io_usrs != 0) {
		v4l2_err(&vpfe_dev->v4l2_dev, "Only one IO user allowed\n");
		ret = -EBUSY;
		goto unlock_out;
	}

	vpfe_dev->memory = req_buf->memory;
	videobuf_queue_dma_contig_init(&vpfe_dev->buffer_queue,
				&vpfe_videobuf_qops,
				vpfe_dev->pdev,
				&vpfe_dev->irqlock,
				req_buf->type,
				vpfe_dev->fmt.fmt.pix.field,
				sizeof(struct videobuf_buffer),
				fh, NULL);

	fh->io_allowed = 1;
	vpfe_dev->io_usrs = 1;
	INIT_LIST_HEAD(&vpfe_dev->dma_queue);
	ret = videobuf_reqbufs(&vpfe_dev->buffer_queue, req_buf);
unlock_out:
	mutex_unlock(&vpfe_dev->lock);
	return ret;
}

static int vpfe_querybuf(struct file *file, void *priv,
			 struct v4l2_buffer *buf)
{
	struct vpfe_device *vpfe_dev = video_drvdata(file);

	v4l2_dbg(1, debug, &vpfe_dev->v4l2_dev, "vpfe_querybuf\n");

	if (V4L2_BUF_TYPE_VIDEO_CAPTURE != buf->type) {
		v4l2_err(&vpfe_dev->v4l2_dev, "Invalid buf type\n");
		return  -EINVAL;
	}

	if (vpfe_dev->memory != V4L2_MEMORY_MMAP) {
		v4l2_err(&vpfe_dev->v4l2_dev, "Invalid memory\n");
		return -EINVAL;
	}
	
	return videobuf_querybuf(&vpfe_dev->buffer_queue, buf);
}

static int vpfe_qbuf(struct file *file, void *priv,
		     struct v4l2_buffer *p)
{
	struct vpfe_device *vpfe_dev = video_drvdata(file);
	struct vpfe_fh *fh = file->private_data;

	v4l2_dbg(1, debug, &vpfe_dev->v4l2_dev, "vpfe_qbuf\n");

	if (V4L2_BUF_TYPE_VIDEO_CAPTURE != p->type) {
		v4l2_err(&vpfe_dev->v4l2_dev, "Invalid buf type\n");
		return -EINVAL;
	}

	if (!fh->io_allowed) {
		v4l2_err(&vpfe_dev->v4l2_dev, "fh->io_allowed\n");
		return -EACCES;
	}
	return videobuf_qbuf(&vpfe_dev->buffer_queue, p);
}

static int vpfe_dqbuf(struct file *file, void *priv,
		      struct v4l2_buffer *buf)
{
	struct vpfe_device *vpfe_dev = video_drvdata(file);

	v4l2_dbg(1, debug, &vpfe_dev->v4l2_dev, "vpfe_dqbuf\n");

	if (V4L2_BUF_TYPE_VIDEO_CAPTURE != buf->type) {
		v4l2_err(&vpfe_dev->v4l2_dev, "Invalid buf type\n");
		return -EINVAL;
	}
	return videobuf_dqbuf(&vpfe_dev->buffer_queue,
				      buf, file->f_flags & O_NONBLOCK);
}

static int vpfe_queryctrl(struct file *file, void *priv,
		struct v4l2_queryctrl *qctrl)
{
	struct vpfe_device *vpfe_dev = video_drvdata(file);
	struct vpfe_subdev_info *sdinfo;

	sdinfo = vpfe_dev->current_subdev;

	return v4l2_device_call_until_err(&vpfe_dev->v4l2_dev, sdinfo->grp_id,
					 core, queryctrl, qctrl);

}

static int vpfe_g_ctrl(struct file *file, void *priv, struct v4l2_control *ctrl)
{
	struct vpfe_device *vpfe_dev = video_drvdata(file);
	struct vpfe_subdev_info *sdinfo;

	sdinfo = vpfe_dev->current_subdev;

	return v4l2_device_call_until_err(&vpfe_dev->v4l2_dev, sdinfo->grp_id,
					 core, g_ctrl, ctrl);
}

static int vpfe_s_ctrl(struct file *file, void *priv, struct v4l2_control *ctrl)
{
	struct vpfe_device *vpfe_dev = video_drvdata(file);
	struct vpfe_subdev_info *sdinfo;

	sdinfo = vpfe_dev->current_subdev;

	return v4l2_device_call_until_err(&vpfe_dev->v4l2_dev, sdinfo->grp_id,
					 core, s_ctrl, ctrl);
}

static void vpfe_calculate_offsets(struct vpfe_device *vpfe_dev)
{
	struct v4l2_rect image_win;

	v4l2_dbg(1, debug, &vpfe_dev->v4l2_dev, "vpfe_calculate_offsets\n");

	ccdc_dev->hw_ops.get_image_window(&image_win);
	vpfe_dev->field_off = image_win.height * image_win.width;
}

static void vpfe_start_ccdc_capture(struct vpfe_device *vpfe_dev)
{
	ccdc_dev->hw_ops.enable(1);
	if (ccdc_dev->hw_ops.enable_out_to_sdram)
		ccdc_dev->hw_ops.enable_out_to_sdram(1);
	vpfe_dev->started = 1;
}

static int vpfe_streamon(struct file *file, void *priv,
			 enum v4l2_buf_type buf_type)
{
	struct vpfe_device *vpfe_dev = video_drvdata(file);
	struct vpfe_fh *fh = file->private_data;
	struct vpfe_subdev_info *sdinfo;
	unsigned long addr;
	int ret = 0;

	v4l2_dbg(1, debug, &vpfe_dev->v4l2_dev, "vpfe_streamon\n");

	if (V4L2_BUF_TYPE_VIDEO_CAPTURE != buf_type) {
		v4l2_err(&vpfe_dev->v4l2_dev, "Invalid buf type\n");
		return -EINVAL;
	}

	
	if (!fh->io_allowed) {
		v4l2_err(&vpfe_dev->v4l2_dev, "fh->io_allowed\n");
		return -EACCES;
	}

	sdinfo = vpfe_dev->current_subdev;
	ret = v4l2_device_call_until_err(&vpfe_dev->v4l2_dev, sdinfo->grp_id,
					video, s_stream, 1);

	if (ret && (ret != -ENOIOCTLCMD)) {
		v4l2_err(&vpfe_dev->v4l2_dev, "stream on failed in subdev\n");
		return -EINVAL;
	}

	
	if (list_empty(&vpfe_dev->buffer_queue.stream)) {
		v4l2_err(&vpfe_dev->v4l2_dev, "buffer queue is empty\n");
		return -EIO;
	}

	
	ret = videobuf_streamon(&vpfe_dev->buffer_queue);
	if (ret)
		return ret;


	ret = mutex_lock_interruptible(&vpfe_dev->lock);
	if (ret)
		goto streamoff;
	
	vpfe_dev->next_frm = list_entry(vpfe_dev->dma_queue.next,
					struct videobuf_buffer, queue);
	vpfe_dev->cur_frm = vpfe_dev->next_frm;
	
	list_del(&vpfe_dev->cur_frm->queue);
	
	vpfe_dev->cur_frm->state = VIDEOBUF_ACTIVE;
	
	vpfe_dev->field_id = 0;
	addr = videobuf_to_dma_contig(vpfe_dev->cur_frm);

	
	vpfe_calculate_offsets(vpfe_dev);

	if (vpfe_attach_irq(vpfe_dev) < 0) {
		v4l2_err(&vpfe_dev->v4l2_dev,
			 "Error in attaching interrupt handle\n");
		ret = -EFAULT;
		goto unlock_out;
	}
	if (ccdc_dev->hw_ops.configure() < 0) {
		v4l2_err(&vpfe_dev->v4l2_dev,
			 "Error in configuring ccdc\n");
		ret = -EINVAL;
		goto unlock_out;
	}
	ccdc_dev->hw_ops.setfbaddr((unsigned long)(addr));
	vpfe_start_ccdc_capture(vpfe_dev);
	mutex_unlock(&vpfe_dev->lock);
	return ret;
unlock_out:
	mutex_unlock(&vpfe_dev->lock);
streamoff:
	ret = videobuf_streamoff(&vpfe_dev->buffer_queue);
	return ret;
}

static int vpfe_streamoff(struct file *file, void *priv,
			  enum v4l2_buf_type buf_type)
{
	struct vpfe_device *vpfe_dev = video_drvdata(file);
	struct vpfe_fh *fh = file->private_data;
	struct vpfe_subdev_info *sdinfo;
	int ret = 0;

	v4l2_dbg(1, debug, &vpfe_dev->v4l2_dev, "vpfe_streamoff\n");

	if (V4L2_BUF_TYPE_VIDEO_CAPTURE != buf_type) {
		v4l2_err(&vpfe_dev->v4l2_dev, "Invalid buf type\n");
		return -EINVAL;
	}

	
	if (!fh->io_allowed) {
		v4l2_err(&vpfe_dev->v4l2_dev, "fh->io_allowed\n");
		return -EACCES;
	}

	
	if (!vpfe_dev->started) {
		v4l2_err(&vpfe_dev->v4l2_dev, "device started\n");
		return -EINVAL;
	}

	ret = mutex_lock_interruptible(&vpfe_dev->lock);
	if (ret)
		return ret;

	vpfe_stop_ccdc_capture(vpfe_dev);
	vpfe_detach_irq(vpfe_dev);

	sdinfo = vpfe_dev->current_subdev;
	ret = v4l2_device_call_until_err(&vpfe_dev->v4l2_dev, sdinfo->grp_id,
					video, s_stream, 0);

	if (ret && (ret != -ENOIOCTLCMD))
		v4l2_err(&vpfe_dev->v4l2_dev, "stream off failed in subdev\n");
	ret = videobuf_streamoff(&vpfe_dev->buffer_queue);
	mutex_unlock(&vpfe_dev->lock);
	return ret;
}

static int vpfe_cropcap(struct file *file, void *priv,
			      struct v4l2_cropcap *crop)
{
	struct vpfe_device *vpfe_dev = video_drvdata(file);

	v4l2_dbg(1, debug, &vpfe_dev->v4l2_dev, "vpfe_cropcap\n");

	if (vpfe_dev->std_index >= ARRAY_SIZE(vpfe_standards))
		return -EINVAL;

	memset(crop, 0, sizeof(struct v4l2_cropcap));
	crop->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	crop->bounds.width = crop->defrect.width =
		vpfe_standards[vpfe_dev->std_index].width;
	crop->bounds.height = crop->defrect.height =
		vpfe_standards[vpfe_dev->std_index].height;
	crop->pixelaspect = vpfe_standards[vpfe_dev->std_index].pixelaspect;
	return 0;
}

static int vpfe_g_crop(struct file *file, void *priv,
			     struct v4l2_crop *crop)
{
	struct vpfe_device *vpfe_dev = video_drvdata(file);

	v4l2_dbg(1, debug, &vpfe_dev->v4l2_dev, "vpfe_g_crop\n");

	crop->c = vpfe_dev->crop;
	return 0;
}

static int vpfe_s_crop(struct file *file, void *priv,
			     struct v4l2_crop *crop)
{
	struct vpfe_device *vpfe_dev = video_drvdata(file);
	int ret = 0;

	v4l2_dbg(1, debug, &vpfe_dev->v4l2_dev, "vpfe_s_crop\n");

	if (vpfe_dev->started) {
		
		v4l2_err(&vpfe_dev->v4l2_dev,
			"Cannot change crop when streaming is ON\n");
		return -EBUSY;
	}

	ret = mutex_lock_interruptible(&vpfe_dev->lock);
	if (ret)
		return ret;

	if (crop->c.top < 0 || crop->c.left < 0) {
		v4l2_err(&vpfe_dev->v4l2_dev,
			"doesn't support negative values for top & left\n");
		ret = -EINVAL;
		goto unlock_out;
	}

	
	crop->c.width = ((crop->c.width + 15) & ~0xf);

	
	if ((crop->c.left + crop->c.width >
		vpfe_dev->std_info.active_pixels) ||
	    (crop->c.top + crop->c.height >
		vpfe_dev->std_info.active_lines)) {
		v4l2_err(&vpfe_dev->v4l2_dev, "Error in S_CROP params\n");
		ret = -EINVAL;
		goto unlock_out;
	}
	ccdc_dev->hw_ops.set_image_window(&crop->c);
	vpfe_dev->fmt.fmt.pix.width = crop->c.width;
	vpfe_dev->fmt.fmt.pix.height = crop->c.height;
	vpfe_dev->fmt.fmt.pix.bytesperline =
		ccdc_dev->hw_ops.get_line_length();
	vpfe_dev->fmt.fmt.pix.sizeimage =
		vpfe_dev->fmt.fmt.pix.bytesperline *
		vpfe_dev->fmt.fmt.pix.height;
	vpfe_dev->crop = crop->c;
unlock_out:
	mutex_unlock(&vpfe_dev->lock);
	return ret;
}


static long vpfe_param_handler(struct file *file, void *priv,
		bool valid_prio, int cmd, void *param)
{
	struct vpfe_device *vpfe_dev = video_drvdata(file);
	int ret = 0;

	v4l2_dbg(2, debug, &vpfe_dev->v4l2_dev, "vpfe_param_handler\n");

	if (vpfe_dev->started) {
		
		v4l2_dbg(1, debug, &vpfe_dev->v4l2_dev,
			"device already started\n");
		return -EBUSY;
	}

	ret = mutex_lock_interruptible(&vpfe_dev->lock);
	if (ret)
		return ret;

	switch (cmd) {
	case VPFE_CMD_S_CCDC_RAW_PARAMS:
		v4l2_warn(&vpfe_dev->v4l2_dev,
			  "VPFE_CMD_S_CCDC_RAW_PARAMS: experimental ioctl\n");
		if (ccdc_dev->hw_ops.set_params) {
			ret = ccdc_dev->hw_ops.set_params(param);
			if (ret) {
				v4l2_dbg(1, debug, &vpfe_dev->v4l2_dev,
					"Error setting parameters in CCDC\n");
				goto unlock_out;
			}
			if (vpfe_get_ccdc_image_format(vpfe_dev,
						       &vpfe_dev->fmt) < 0) {
				v4l2_dbg(1, debug, &vpfe_dev->v4l2_dev,
					"Invalid image format at CCDC\n");
				goto unlock_out;
			}
		} else {
			ret = -EINVAL;
			v4l2_dbg(1, debug, &vpfe_dev->v4l2_dev,
				"VPFE_CMD_S_CCDC_RAW_PARAMS not supported\n");
		}
		break;
	default:
		ret = -EINVAL;
	}
unlock_out:
	mutex_unlock(&vpfe_dev->lock);
	return ret;
}


static const struct v4l2_ioctl_ops vpfe_ioctl_ops = {
	.vidioc_querycap	 = vpfe_querycap,
	.vidioc_g_fmt_vid_cap    = vpfe_g_fmt_vid_cap,
	.vidioc_enum_fmt_vid_cap = vpfe_enum_fmt_vid_cap,
	.vidioc_s_fmt_vid_cap    = vpfe_s_fmt_vid_cap,
	.vidioc_try_fmt_vid_cap  = vpfe_try_fmt_vid_cap,
	.vidioc_enum_input	 = vpfe_enum_input,
	.vidioc_g_input		 = vpfe_g_input,
	.vidioc_s_input		 = vpfe_s_input,
	.vidioc_querystd	 = vpfe_querystd,
	.vidioc_s_std		 = vpfe_s_std,
	.vidioc_g_std		 = vpfe_g_std,
	.vidioc_queryctrl	 = vpfe_queryctrl,
	.vidioc_g_ctrl		 = vpfe_g_ctrl,
	.vidioc_s_ctrl		 = vpfe_s_ctrl,
	.vidioc_reqbufs		 = vpfe_reqbufs,
	.vidioc_querybuf	 = vpfe_querybuf,
	.vidioc_qbuf		 = vpfe_qbuf,
	.vidioc_dqbuf		 = vpfe_dqbuf,
	.vidioc_streamon	 = vpfe_streamon,
	.vidioc_streamoff	 = vpfe_streamoff,
	.vidioc_cropcap		 = vpfe_cropcap,
	.vidioc_g_crop		 = vpfe_g_crop,
	.vidioc_s_crop		 = vpfe_s_crop,
	.vidioc_default		 = vpfe_param_handler,
};

static struct vpfe_device *vpfe_initialize(void)
{
	struct vpfe_device *vpfe_dev;

	
	if ((numbuffers > 0) &&
	    (numbuffers < config_params.min_numbuffers))
		numbuffers = config_params.min_numbuffers;

	if (bufsize < config_params.min_bufsize)
		bufsize = config_params.min_bufsize;

	config_params.numbuffers = numbuffers;

	if (numbuffers)
		config_params.device_bufsize = bufsize;

	
	vpfe_dev = kzalloc(sizeof(*vpfe_dev), GFP_KERNEL);

	return vpfe_dev;
}

static __init int vpfe_probe(struct platform_device *pdev)
{
	struct vpfe_subdev_info *sdinfo;
	struct vpfe_config *vpfe_cfg;
	struct resource *res1;
	struct vpfe_device *vpfe_dev;
	struct i2c_adapter *i2c_adap;
	struct video_device *vfd;
	int ret = -ENOMEM, i, j;
	int num_subdevs = 0;

	
	vpfe_dev = vpfe_initialize();

	if (!vpfe_dev) {
		v4l2_err(pdev->dev.driver,
			"Failed to allocate memory for vpfe_dev\n");
		return ret;
	}

	vpfe_dev->pdev = &pdev->dev;

	if (NULL == pdev->dev.platform_data) {
		v4l2_err(pdev->dev.driver, "Unable to get vpfe config\n");
		ret = -ENODEV;
		goto probe_free_dev_mem;
	}

	vpfe_cfg = pdev->dev.platform_data;
	vpfe_dev->cfg = vpfe_cfg;
	if (NULL == vpfe_cfg->ccdc ||
	    NULL == vpfe_cfg->card_name ||
	    NULL == vpfe_cfg->sub_devs) {
		v4l2_err(pdev->dev.driver, "null ptr in vpfe_cfg\n");
		ret = -ENOENT;
		goto probe_free_dev_mem;
	}

	
	ccdc_cfg = kmalloc(sizeof(struct ccdc_config), GFP_KERNEL);
	if (NULL == ccdc_cfg) {
		v4l2_err(pdev->dev.driver,
			 "Memory allocation failed for ccdc_cfg\n");
		goto probe_free_lock;
	}

	mutex_lock(&ccdc_lock);

	strncpy(ccdc_cfg->name, vpfe_cfg->ccdc, 32);
	
	res1 = platform_get_resource(pdev, IORESOURCE_IRQ, 0);
	if (!res1) {
		v4l2_err(pdev->dev.driver,
			 "Unable to get interrupt for VINT0\n");
		ret = -ENODEV;
		goto probe_free_ccdc_cfg_mem;
	}
	vpfe_dev->ccdc_irq0 = res1->start;

	
	res1 = platform_get_resource(pdev, IORESOURCE_IRQ, 1);
	if (!res1) {
		v4l2_err(pdev->dev.driver,
			 "Unable to get interrupt for VINT1\n");
		ret = -ENODEV;
		goto probe_free_ccdc_cfg_mem;
	}
	vpfe_dev->ccdc_irq1 = res1->start;

	ret = request_irq(vpfe_dev->ccdc_irq0, vpfe_isr, IRQF_DISABLED,
			  "vpfe_capture0", vpfe_dev);

	if (0 != ret) {
		v4l2_err(pdev->dev.driver, "Unable to request interrupt\n");
		goto probe_free_ccdc_cfg_mem;
	}

	
	vfd = video_device_alloc();
	if (NULL == vfd) {
		ret = -ENOMEM;
		v4l2_err(pdev->dev.driver, "Unable to alloc video device\n");
		goto probe_out_release_irq;
	}

	
	vfd->release		= video_device_release;
	vfd->fops		= &vpfe_fops;
	vfd->ioctl_ops		= &vpfe_ioctl_ops;
	vfd->tvnorms		= 0;
	vfd->current_norm	= V4L2_STD_PAL;
	vfd->v4l2_dev 		= &vpfe_dev->v4l2_dev;
	snprintf(vfd->name, sizeof(vfd->name),
		 "%s_V%d.%d.%d",
		 CAPTURE_DRV_NAME,
		 (VPFE_CAPTURE_VERSION_CODE >> 16) & 0xff,
		 (VPFE_CAPTURE_VERSION_CODE >> 8) & 0xff,
		 (VPFE_CAPTURE_VERSION_CODE) & 0xff);
	
	vpfe_dev->video_dev	= vfd;

	ret = v4l2_device_register(&pdev->dev, &vpfe_dev->v4l2_dev);
	if (ret) {
		v4l2_err(pdev->dev.driver,
			"Unable to register v4l2 device.\n");
		goto probe_out_video_release;
	}
	v4l2_info(&vpfe_dev->v4l2_dev, "v4l2 device registered\n");
	spin_lock_init(&vpfe_dev->irqlock);
	spin_lock_init(&vpfe_dev->dma_queue_lock);
	mutex_init(&vpfe_dev->lock);

	
	vpfe_dev->numbuffers = config_params.numbuffers;

	
	v4l2_prio_init(&vpfe_dev->prio);
	
	v4l2_dbg(1, debug, &vpfe_dev->v4l2_dev,
		"trying to register vpfe device.\n");
	v4l2_dbg(1, debug, &vpfe_dev->v4l2_dev,
		"video_dev=%x\n", (int)&vpfe_dev->video_dev);
	vpfe_dev->fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	ret = video_register_device(vpfe_dev->video_dev,
				    VFL_TYPE_GRABBER, -1);

	if (ret) {
		v4l2_err(pdev->dev.driver,
			"Unable to register video device.\n");
		goto probe_out_v4l2_unregister;
	}

	v4l2_info(&vpfe_dev->v4l2_dev, "video device registered\n");
	
	platform_set_drvdata(pdev, vpfe_dev);
	
	video_set_drvdata(vpfe_dev->video_dev, vpfe_dev);
	i2c_adap = i2c_get_adapter(vpfe_cfg->i2c_adapter_id);
	num_subdevs = vpfe_cfg->num_subdevs;
	vpfe_dev->sd = kmalloc(sizeof(struct v4l2_subdev *) * num_subdevs,
				GFP_KERNEL);
	if (NULL == vpfe_dev->sd) {
		v4l2_err(&vpfe_dev->v4l2_dev,
			"unable to allocate memory for subdevice pointers\n");
		ret = -ENOMEM;
		goto probe_out_video_unregister;
	}

	for (i = 0; i < num_subdevs; i++) {
		struct v4l2_input *inps;

		sdinfo = &vpfe_cfg->sub_devs[i];

		
		vpfe_dev->sd[i] =
			v4l2_i2c_new_subdev_board(&vpfe_dev->v4l2_dev,
						  i2c_adap,
						  &sdinfo->board_info,
						  NULL);
		if (vpfe_dev->sd[i]) {
			v4l2_info(&vpfe_dev->v4l2_dev,
				  "v4l2 sub device %s registered\n",
				  sdinfo->name);
			vpfe_dev->sd[i]->grp_id = sdinfo->grp_id;
			
			for (j = 0; j < sdinfo->num_inputs; j++) {
				inps = &sdinfo->inputs[j];
				vfd->tvnorms |= inps->std;
			}
		} else {
			v4l2_info(&vpfe_dev->v4l2_dev,
				  "v4l2 sub device %s register fails\n",
				  sdinfo->name);
			goto probe_sd_out;
		}
	}

	
	vpfe_dev->current_subdev = &vpfe_cfg->sub_devs[0];

	
	mutex_unlock(&ccdc_lock);
	return 0;

probe_sd_out:
	kfree(vpfe_dev->sd);
probe_out_video_unregister:
	video_unregister_device(vpfe_dev->video_dev);
probe_out_v4l2_unregister:
	v4l2_device_unregister(&vpfe_dev->v4l2_dev);
probe_out_video_release:
	if (!video_is_registered(vpfe_dev->video_dev))
		video_device_release(vpfe_dev->video_dev);
probe_out_release_irq:
	free_irq(vpfe_dev->ccdc_irq0, vpfe_dev);
probe_free_ccdc_cfg_mem:
	kfree(ccdc_cfg);
probe_free_lock:
	mutex_unlock(&ccdc_lock);
probe_free_dev_mem:
	kfree(vpfe_dev);
	return ret;
}

static int __devexit vpfe_remove(struct platform_device *pdev)
{
	struct vpfe_device *vpfe_dev = platform_get_drvdata(pdev);

	v4l2_info(pdev->dev.driver, "vpfe_remove\n");

	free_irq(vpfe_dev->ccdc_irq0, vpfe_dev);
	kfree(vpfe_dev->sd);
	v4l2_device_unregister(&vpfe_dev->v4l2_dev);
	video_unregister_device(vpfe_dev->video_dev);
	kfree(vpfe_dev);
	kfree(ccdc_cfg);
	return 0;
}

static int vpfe_suspend(struct device *dev)
{
	return 0;
}

static int vpfe_resume(struct device *dev)
{
	return 0;
}

static const struct dev_pm_ops vpfe_dev_pm_ops = {
	.suspend = vpfe_suspend,
	.resume = vpfe_resume,
};

static struct platform_driver vpfe_driver = {
	.driver = {
		.name = CAPTURE_DRV_NAME,
		.owner = THIS_MODULE,
		.pm = &vpfe_dev_pm_ops,
	},
	.probe = vpfe_probe,
	.remove = __devexit_p(vpfe_remove),
};

module_platform_driver(vpfe_driver);