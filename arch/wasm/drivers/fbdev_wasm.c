// SPDX-License-Identifier: GPL-2.0-only
#include <linux/errno.h>
#include <linux/fb.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/string.h>
#include <linux/vmalloc.h>
#include <asm/smp.h>
#include <asm/wasm_imports.h>

#define WASMFB_WIDTH  640U
#define WASMFB_HEIGHT 480U
#define WASMFB_BPP    32U

struct wasmfb_register_req {
	void *fb_addr;
	u32 width;
	u32 height;
	u32 stride;
	u32 bpp;
};

struct wasmfb_flush_req {
	u32 x;
	u32 y;
	u32 width;
	u32 height;
};

static void wasmfb_register_on_main(void *arg)
{
	struct wasmfb_register_req *req = arg;

	wasm_fb_register(req->fb_addr, req->width, req->height, req->stride,
			 req->bpp);
}

static void wasmfb_flush_on_main(void *arg)
{
	struct wasmfb_flush_req *req = arg;

	wasm_fb_flush(req->x, req->y, req->width, req->height);
}

static void wasmfb_notify_flush(u32 x, u32 y, u32 width, u32 height)
{
	struct wasmfb_flush_req req = {
		.x = x,
		.y = y,
		.width = width,
		.height = height,
	};

	wasm_kernel_run_on_main(wasmfb_flush_on_main, &req);
}

static ssize_t wasmfb_write(struct fb_info *info, const char __user *buf,
			    size_t count, loff_t *ppos)
{
	ssize_t written = fb_sys_write(info, buf, count, ppos);

	if (written > 0)
		wasmfb_notify_flush(0, 0, info->var.xres, info->var.yres);
	return written;
}

static int wasmfb_check_var(struct fb_var_screeninfo *var,
			    struct fb_info *info)
{
	if (var->xres != info->var.xres || var->yres != info->var.yres ||
	    var->bits_per_pixel != WASMFB_BPP)
		return -EINVAL;

	var->xres_virtual = var->xres;
	var->yres_virtual = var->yres;
	var->xoffset = 0;
	var->yoffset = 0;
	var->red.offset = 16;
	var->red.length = 8;
	var->green.offset = 8;
	var->green.length = 8;
	var->blue.offset = 0;
	var->blue.length = 8;
	var->transp.offset = 24;
	var->transp.length = 8;
	return 0;
}

static int wasmfb_set_par(struct fb_info *info)
{
	info->fix.line_length = info->var.xres * (WASMFB_BPP / 8);
	return 0;
}

static int wasmfb_setcolreg(u_int regno, u_int red, u_int green, u_int blue,
			    u_int transp, struct fb_info *info)
{
	u32 *pal;

	if (regno >= 16)
		return -EINVAL;

	pal = info->pseudo_palette;
	pal[regno] = ((transp & 0xff00U) << 16) |
		     ((red & 0xff00U) << 8) |
		     (green & 0xff00U) |
		     ((blue & 0xff00U) >> 8);
	return 0;
}

static int wasmfb_pan_display(struct fb_var_screeninfo *var,
			      struct fb_info *info)
{
	if (var->xoffset != 0 || var->yoffset != 0)
		return -EINVAL;
	wasmfb_notify_flush(0, 0, info->var.xres, info->var.yres);
	return 0;
}

static const struct fb_ops wasmfb_ops = {
	.owner		= THIS_MODULE,
	.fb_read	= fb_sys_read,
	.fb_write	= wasmfb_write,
	.fb_check_var	= wasmfb_check_var,
	.fb_set_par	= wasmfb_set_par,
	.fb_setcolreg	= wasmfb_setcolreg,
	.fb_pan_display	= wasmfb_pan_display,
	__FB_DEFAULT_SYSMEM_OPS_DRAW,
};

static int wasmfb_probe(struct platform_device *pdev)
{
	u32 stride = WASMFB_WIDTH * (WASMFB_BPP / 8);
	size_t fb_size = PAGE_ALIGN((size_t)stride * WASMFB_HEIGHT);
	struct wasmfb_register_req reg_req;
	struct fb_info *info;
	void *videomemory;
	u32 *palette;
	int ret;

	videomemory = vzalloc(fb_size);
	if (!videomemory)
		return -ENOMEM;

	info = framebuffer_alloc(sizeof(u32) * 16, &pdev->dev);
	if (!info) {
		ret = -ENOMEM;
		goto err_vfree;
	}

	palette = info->par;
	info->pseudo_palette = palette;
	info->screen_buffer = videomemory;
	info->screen_size = fb_size;
	info->fbops = &wasmfb_ops;

	strscpy(info->fix.id, "Wasm FB", sizeof(info->fix.id));
	info->fix.smem_start = (unsigned long)videomemory;
	info->fix.smem_len = fb_size;
	info->fix.type = FB_TYPE_PACKED_PIXELS;
	info->fix.visual = FB_VISUAL_TRUECOLOR;
	info->fix.line_length = stride;
	info->fix.accel = FB_ACCEL_NONE;

	info->var.xres = WASMFB_WIDTH;
	info->var.yres = WASMFB_HEIGHT;
	info->var.xres_virtual = WASMFB_WIDTH;
	info->var.yres_virtual = WASMFB_HEIGHT;
	info->var.bits_per_pixel = WASMFB_BPP;
	info->var.red.offset = 16;
	info->var.red.length = 8;
	info->var.green.offset = 8;
	info->var.green.length = 8;
	info->var.blue.offset = 0;
	info->var.blue.length = 8;
	info->var.transp.offset = 24;
	info->var.transp.length = 8;
	info->var.activate = FB_ACTIVATE_NOW;

	reg_req.fb_addr = videomemory;
	reg_req.width = WASMFB_WIDTH;
	reg_req.height = WASMFB_HEIGHT;
	reg_req.stride = stride;
	reg_req.bpp = WASMFB_BPP;
	wasm_kernel_run_on_main(wasmfb_register_on_main, &reg_req);

	ret = register_framebuffer(info);
	if (ret < 0)
		goto err_fb_release;

	platform_set_drvdata(pdev, info);
	return 0;

err_fb_release:
	framebuffer_release(info);
err_vfree:
	vfree(videomemory);
	return ret;
}

static void wasmfb_remove(struct platform_device *pdev)
{
	struct fb_info *info = platform_get_drvdata(pdev);

	if (info) {
		void *videomemory = info->screen_buffer;

		unregister_framebuffer(info);
		framebuffer_release(info);
		vfree(videomemory);
	}
}

static struct platform_driver wasmfb_driver = {
	.probe	= wasmfb_probe,
	.remove	= wasmfb_remove,
	.driver	= {
		.name = "wasmfb",
	},
};

static struct platform_device *wasmfb_device;

static int __init wasmfb_init(void)
{
	int ret;

	ret = platform_driver_register(&wasmfb_driver);
	if (ret)
		return ret;

	wasmfb_device = platform_device_register_simple("wasmfb", -1, NULL, 0);
	if (IS_ERR(wasmfb_device)) {
		ret = PTR_ERR(wasmfb_device);
		platform_driver_unregister(&wasmfb_driver);
		return ret;
	}
	return 0;
}

static void __exit wasmfb_exit(void)
{
	platform_device_unregister(wasmfb_device);
	platform_driver_unregister(&wasmfb_driver);
}

module_init(wasmfb_init);
module_exit(wasmfb_exit);

MODULE_DESCRIPTION("WebAssembly linear framebuffer driver");
MODULE_LICENSE("GPL");
