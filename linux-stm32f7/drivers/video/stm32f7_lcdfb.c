#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/platform_device.h>
#include <asm/uaccess.h>
#include <asm/setup.h>
#include <asm/system.h>
#include <linux/fb.h>
#include <linux/module.h>
#include <asm/pgtable.h>
#include <linux/clk.h>
#include <linux/string.h>

#define DRIVER_NAME "stm32f7-ltdc"

#define LTDC_SSCR	0x08
#define LTDC_BPCR	0x0c
#define LTDC_AWCR	0x10
#define LTDC_TWCR	0x14
#define LTDC_GCR	0x18
#define LTDC_SRCR	0x24 
#define LTDC_BCCR	0x2c 
#define LTDC_IER	0x34

static struct fb_fix_screeninfo fb_fix __initdata = {
	.id		= DRIVER_NAME,
	.smem_len	= 480 * 272 * 4,
	.type		= FB_TYPE_PACKED_PIXELS,
	.visual		= FB_VISUAL_TRUECOLOR,
	.line_length	= 480 * 4,
	.accel		= FB_ACCEL_NONE,
};

static struct fb_var_screeninfo fb_var __initdata = {
	.xres		= 480,
	.yres		= 272,
	.xres_virtual	= 480,
	.yres_virtual	= 272,
	.bits_per_pixel	= 32,
    	.red		= {16, 8, 0},
	.green		= {8, 8, 0},
	.blue		= {0, 8, 0},
	.activate	= FB_ACTIVATE_NOW,
	.height		= 272,
	.width		= 480,
	.vmode		= FB_VMODE_NONINTERLACED,
};

static u32 *fb_base;

static int fb_setcolreg(u32 regno, u32 red, u32 green,
			u32 blue, u32 transp, struct fb_info *info)
{
	((u32 *)info->pseudo_palette)[regno] = 0xff << 24 |
					red << 16 | green << 8 | blue;
	return 0;
}

static struct fb_ops fb_ops = {
	.owner		= THIS_MODULE,
	.fb_setcolreg	= fb_setcolreg,
	.fb_fillrect	= cfb_fillrect,
	.fb_copyarea	= cfb_copyarea,
	.fb_imageblit	= cfb_imageblit,
};

static void stm_clock_init(void)
{
	clk_enable(clk_get_sys(0, "sai_r_clk"));
	clk_enable(clk_get_sys("stm32f4-ltdc.0", 0));
	clk_enable(clk_get_sys(0, "gpioe"));
	clk_enable(clk_get_sys(0, "gpiog"));
	clk_enable(clk_get_sys(0, "gpioi"));
	clk_enable(clk_get_sys(0, "gpioj"));
	clk_enable(clk_get_sys(0, "gpiok"));
}

static void ltdc_init(base)
{
	stm_clock_init();

	writel(BIT(12), 0x40022018); /* Assert display enable LCD_DISP pin */
	writel(BIT(3), 0x40022818);
	writel(0x280009, base + LTDC_SSCR);
	writel(0x35000b, base + LTDC_BPCR);
	writel(0x215011b, base + LTDC_AWCR);
	writel(0x235011d, base + LTDC_TWCR);
	writel(0, base + LTDC_BCCR);
	writel(0x6, base + LTDC_IER);
	writel(0x2221, base + LTDC_GCR);
}

static void store_hex(u32 addr)
{
	u32 layer0[] = { 1, 0x2150036, 0x11b000c, 0, 0 ,0xff, 0, 0x607 ,0, 0,
		0, 0x7800783, 0x110, 0, 0, 0, 0 };
	u32 layer1[] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };

	layer0[10] = addr; /* framebuffer base address register */
	layer1[10] = addr; /* framebuffer base address register */

	memcpy((void *)0x40016884, layer0, sizeof(layer0));
	memcpy((void *)0x40016884 + 0x80, layer1, sizeof(layer1));
}

static int __init fb_probe(struct platform_device *dev)
{
	struct fb_info *info;
	int ret = -ENOMEM;
	u32 base = 0x40016800; // get from plat data TODO

	if (!(fb_base = kmalloc(480 * 272 * 4+ 256, GFP_KERNEL)))
		goto err_exit;
	fb_fix.smem_start = (u32) fb_base + 256 - ((u32)fb_base % 256);
	
	ltdc_init(base);
	store_hex(fb_fix.smem_start); /* layer config dumped memory */
	writel(1, (u32)(base + LTDC_SRCR)); /* force reload */

	if (!(info = framebuffer_alloc(sizeof(u32) * 16, &dev->dev)))
		goto err_free_mem;

	info->var = fb_var;
	info->fix = fb_fix;
	info->fbops = &fb_ops;
	info->flags = FBINFO_DEFAULT;
	info->pseudo_palette = info->par;
	info->par = NULL;
	info->screen_base = (char *) fb_fix.smem_start;

	if (fb_alloc_cmap(&info->cmap, 256, 0) < 0)
		goto err_free_fb;

	if (register_framebuffer(info) < 0) {
		ret = -EINVAL;
		goto err_free_cmap;
	}

        pr_info("%s: fb%d registered @ 0x%p\n", DRIVER_NAME, info->node,
			info->screen_base);

	return 0;

err_free_cmap:
	fb_dealloc_cmap(&info->cmap);
err_free_fb:
	framebuffer_release(info);
err_free_mem:
	kfree(fb_base);
err_exit:
	return ret;
}

static struct platform_driver fb_pdrv = {
	.probe	= fb_probe,
	.driver	= {
		.name	= DRIVER_NAME,
	},
};

int __init init_module0(void)
{
	if (platform_driver_register(&fb_pdrv)) {
		pr_info("failed to register %s driver\n", DRIVER_NAME);
		return -ENODEV;
	}
	
	return 0;
}

void __exit cleanup_module0(void)
{
	platform_driver_unregister(&fb_pdrv);
	kfree(fb_base);
	pr_info("clean");
}

module_init(init_module0);
module_exit(cleanup_module0);

MODULE_LICENSE("GPL v2");
