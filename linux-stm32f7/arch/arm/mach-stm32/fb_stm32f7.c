/*
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston,
 * MA 02111-1307 USA
 */

#include <linux/init.h>
#include <linux/platform_device.h>
#include <linux/irq.h>
#include <linux/dma-mapping.h>
#include <linux/delay.h>
#include <linux/gpio.h>

#include <mach/stm32.h>
#include <mach/fb.h>
#include <mach/platform.h>

static struct resource stm32f7_fb_resources[] = {
	{
		.start	= STM32F4_LTDC_BASE,
		.end	= STM32F4_LTDC_BASE + STM32F4_LTDC_LENGTH - 1,
		.flags	= IORESOURCE_MEM,
	},
	/*
	{
		.start	= STM32F4_LTDC_IRQ,
		.flags	= IORESOURCE_IRQ,
	},
	*/
};

static struct platform_device fb_pdev = {
	.name = "stm32f7-ltdc",
	.num_resources = ARRAY_SIZE(stm32f7_fb_resources),
	.resource = stm32f7_fb_resources,
};

void __init stm32f7_fb_init(void)
{
	if (platform_device_register(&fb_pdev))
		pr_info("failed to register %s device\n", "stm32f7-ltdc");
}
