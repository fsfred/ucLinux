/* vi: set sw=4 ts=4: */
/* eraseall.c -- erase the whole of a MTD device
 *
 * Ported to busybox from mtd-utils.
 *
 * Copyright (C) 2000 Arcom Control System Ltd
 *
 * Renamed to flash_eraseall.c
 *
 * Licensed under GPLv2 or later, see file LICENSE in this tarball for details.
 */

#include "libbb.h"
#include <mtd/mtd-user.h>
#include <linux/jffs2.h>
#include <linux/version.h>

#define OPTION_J	(1 << 0)
#define OPTION_Q	(1 << 1)
#define IS_NAND		(1 << 2)
#define BBTEST		(1 << 3)

/* mtd/jffs2-user.h used to have this atrocity:
extern int target_endian;

#define t16(x) ({ __u16 __b = (x); (target_endian==__BYTE_ORDER)?__b:bswap_16(__b); })
#define t32(x) ({ __u32 __b = (x); (target_endian==__BYTE_ORDER)?__b:bswap_32(__b); })

#define cpu_to_je16(x) ((jint16_t){t16(x)})
#define cpu_to_je32(x) ((jint32_t){t32(x)})
#define cpu_to_jemode(x) ((jmode_t){t32(x)})

#define je16_to_cpu(x) (t16((x).v16))
#define je32_to_cpu(x) (t32((x).v32))
#define jemode_to_cpu(x) (t32((x).m))

but mtd/jffs2-user.h is gone now (at least 2.6.31.6 does not have it anymore)
*/

/* We always use native endianness */
#undef cpu_to_je16
#undef cpu_to_je32
#define cpu_to_je16(v) ((jint16_t){(v)})
#define cpu_to_je32(v) ((jint32_t){(v)})

static uint32_t crc32(uint32_t val, const void *ss, int len,
		uint32_t *crc32_table)
{
	const unsigned char *s = ss;
	while (--len >= 0)
		val = crc32_table[(val ^ *s++) & 0xff] ^ (val >> 8);
	return val;
}

static void show_progress(mtd_info_t *meminfo, erase_info_t *erase)
{
	printf("\rErasing %u Kibyte @ %x - %2u%% complete.",
		(unsigned)meminfo->erasesize / 1024,
		erase->start,
		(unsigned) ((unsigned long long) erase->start * 100 / meminfo->size)
	);
	fflush_all();
}

int flash_eraseall_main(int argc, char **argv) MAIN_EXTERNALLY_VISIBLE;
int flash_eraseall_main(int argc UNUSED_PARAM, char **argv)
{
	struct jffs2_unknown_node cleanmarker;
	mtd_info_t meminfo;
	int fd, clmpos, clmlen;
	erase_info_t erase;
	struct stat st;
	unsigned int flags;
	char *mtd_name;

	opt_complementary = "=1";
	flags = BBTEST | getopt32(argv, "jq");

	mtd_name = argv[optind];
	fd = xopen(mtd_name, O_RDWR);
	fstat(fd, &st);
	if (!S_ISCHR(st.st_mode))
		bb_error_msg_and_die("%s: not a char device", mtd_name);

	xioctl(fd, MEMGETINFO, &meminfo);
	erase.length = meminfo.erasesize;
	if (meminfo.type == MTD_NANDFLASH)
		flags |= IS_NAND;

	clmpos = 0;
	clmlen = 8;
	if (flags & OPTION_J) {
		uint32_t *crc32_table;

		crc32_table = crc32_filltable(NULL, 0);

		cleanmarker.magic = cpu_to_je16(JFFS2_MAGIC_BITMASK);
		cleanmarker.nodetype = cpu_to_je16(JFFS2_NODETYPE_CLEANMARKER);
		if (!(flags & IS_NAND))
			cleanmarker.totlen = cpu_to_je32(sizeof(struct jffs2_unknown_node));
		else {
			struct nand_oobinfo oobinfo;

#ifdef ECCGETLAYOUT
			if (ioctl(fd, MEMGETOOBSEL, &oobinfo) != 0) {
#if LINUX_VERSION_CODE <= KERNEL_VERSION(2,6,36)
				struct nand_ecclayout layout;
#else
				struct nand_ecclayout_user layout;
#endif
				xioctl(fd, ECCGETLAYOUT, &layout);

				if (!layout.oobfree[0].length)
					bb_error_msg_and_die("no empty space in oob");
				clmpos = layout.oobfree[0].offset;
				clmlen = layout.oobfree[0].length;
			} else {
#else /* ECCGETLAYOUT */
				xioctl(fd, MEMGETOOBSEL, &oobinfo);
#endif /* ECCGETLAYOUT */
				/* Check for autoplacement */
				if (oobinfo.useecc == MTD_NANDECC_AUTOPLACE) {
					/* Get the position of the free bytes */
					clmpos = oobinfo.oobfree[0][0];
					clmlen = oobinfo.oobfree[0][1];
					if (clmlen == 0) {
						bb_error_msg_and_die(
							"autoplacement selected and no empty space in oob");
					}
				} else {
					/* Legacy mode */
					switch (meminfo.oobsize) {
					case 8:
						clmpos = 6;
						clmlen = 2;
						break;
					case 16:
						clmpos = 8;
						/*clmlen = 8;*/
						break;
					case 64:
						clmpos = 16;
						/*clmlen = 8;*/
						break;
					}
				}
#ifdef ECCGETLAYOUT
			}
#endif /* ECCGETLAYOUT */
			if (clmlen > 8)
				clmlen = 8;
			cleanmarker.totlen = cpu_to_je32(8);
		}

		cleanmarker.hdr_crc = cpu_to_je32(crc32(0, &cleanmarker, sizeof(struct jffs2_unknown_node) - 4,
					crc32_table));
	}

	/* Don't want to destroy progress indicator by bb_error_msg's */
	applet_name = xasprintf("\n%s: %s", applet_name, mtd_name);

	for (erase.start = 0; erase.start < meminfo.size;
	     erase.start += meminfo.erasesize) {
		if (flags & BBTEST) {
			int ret;
			loff_t offset = erase.start;

			ret = ioctl(fd, MEMGETBADBLOCK, &offset);
			if (ret > 0) {
				if (!(flags & OPTION_Q))
					bb_info_msg("\nSkipping bad block at 0x%08x", erase.start);
				continue;
			}
			if (ret < 0) {
				/* Black block table is not available on certain flash
				 * types e.g. NOR
				 */
				if (errno == EOPNOTSUPP) {
					flags &= ~BBTEST;
					if (flags & IS_NAND)
						bb_error_msg_and_die("bad block check not available");
				} else {
					bb_perror_msg_and_die("MEMGETBADBLOCK error");
				}
			}
		}

		if (!(flags & OPTION_Q))
			show_progress(&meminfo, &erase);

		xioctl(fd, MEMERASE, &erase);

		/* format for JFFS2 ? */
		if (!(flags & OPTION_J))
			continue;

		/* write cleanmarker */
		if (flags & IS_NAND) {
			struct mtd_oob_buf oob;

			oob.ptr = (unsigned char *) &cleanmarker;
			oob.start = erase.start + clmpos;
			oob.length = clmlen;
			xioctl(fd, MEMWRITEOOB, &oob);
		} else {
			xlseek(fd, erase.start, SEEK_SET);
			/* if (lseek(fd, erase.start, SEEK_SET) < 0) {
				bb_perror_msg("MTD %s failure", "seek");
				continue;
			} */
			xwrite(fd, &cleanmarker, sizeof(cleanmarker));
			/* if (write(fd, &cleanmarker, sizeof(cleanmarker)) != sizeof(cleanmarker)) {
				bb_perror_msg("MTD %s failure", "write");
				continue;
			} */
		}
		if (!(flags & OPTION_Q))
			printf(" Cleanmarker written at %x.", erase.start);
	}
	if (!(flags & OPTION_Q)) {
		show_progress(&meminfo, &erase);
		bb_putchar('\n');
	}

	if (ENABLE_FEATURE_CLEAN_UP)
		close(fd);
	return EXIT_SUCCESS;
}
