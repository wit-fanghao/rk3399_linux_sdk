/*
 * (C) Copyright 2019 Rockchip Electronics Co., Ltd
 *
 * SPDX-License-Identifier:	GPL-2.0+
 */

#include <common.h>
#include <blk.h>
#include <boot_rkimg.h>
#include <dm.h>
#include <errno.h>
#include <malloc.h>
#include <nand.h>
#include <part.h>
#include <dm/device-internal.h>

#define MTD_PART_NAND_HEAD		"mtdparts="
#define MTD_PART_INFO_MAX_SIZE		512
#define MTD_SINGLE_PART_INFO_MAX_SIZE	40

char *mtd_part_parse(void)
{
	char mtd_part_info_temp[MTD_SINGLE_PART_INFO_MAX_SIZE] = {0};
	u32 length, data_len = MTD_PART_INFO_MAX_SIZE;
	struct blk_desc *dev_desc;
	disk_partition_t info;
	char *mtd_part_info_p;
	char *mtd_part_info;
	int ret;
	int p;

	dev_desc = rockchip_get_bootdev();
	if (!dev_desc)
		return NULL;

	mtd_part_info = (char *)calloc(MTD_PART_INFO_MAX_SIZE, sizeof(char));
	if (!mtd_part_info) {
		printf("%s: Fail to malloc!", __func__);
		return NULL;
	}

	mtd_part_info_p = mtd_part_info;
	snprintf(mtd_part_info_p, data_len - 1, "%s%s:",
		 MTD_PART_NAND_HEAD,
		 dev_desc->product);
	data_len -= strlen(mtd_part_info_p);
	mtd_part_info_p = mtd_part_info_p + strlen(mtd_part_info_p);

	for (p = 1; p < MAX_SEARCH_PARTITIONS; p++) {
		ret = part_get_info(dev_desc, p, &info);
		if (ret)
			break;

		debug("name is %s, start addr is %x\n", info.name,
		      (int)(size_t)info.start);

		snprintf(mtd_part_info_p, data_len - 1, "0x%x@0x%x(%s)",
			 (int)(size_t)info.size << 9,
			 (int)(size_t)info.start << 9,
			 info.name);
		snprintf(mtd_part_info_temp, MTD_SINGLE_PART_INFO_MAX_SIZE - 1,
			 "0x%x@0x%x(%s)",
			 (int)(size_t)info.size << 9,
			 (int)(size_t)info.start << 9,
			 info.name);
		strcat(mtd_part_info, ",");
		if (part_get_info(dev_desc, p + 1, &info)) {
			snprintf(mtd_part_info_p, data_len - 1, "-@0x%x(%s)",
				 (int)(size_t)info.start << 9,
				 info.name);
			break;
		}
		length = strlen(mtd_part_info_temp);
		data_len -= length;
		mtd_part_info_p = mtd_part_info_p + length + 1;
		memset(mtd_part_info_temp, 0, MTD_SINGLE_PART_INFO_MAX_SIZE);
	}

	return mtd_part_info;
}

ulong mtd_dread(struct udevice *udev, lbaint_t start,
		lbaint_t blkcnt, void *dst)
{
	struct blk_desc *desc = dev_get_uclass_platdata(udev);

	if (!desc)
		return 0;

	if (blkcnt == 0)
		return 0;

	if (desc->devnum == BLK_MTD_NAND) {
		int ret = 0;
		size_t rwsize = blkcnt * 512;
		struct mtd_info *mtd = dev_get_priv(udev->parent);
		struct nand_chip *chip = mtd_to_nand(mtd);
		loff_t off = (loff_t)(start * 512);

		if (!mtd) {
			puts("\nno mtd available\n");
			return 0;
		}

		if (!chip) {
			puts("\nno chip available\n");
			return 0;
		}

		ret = nand_read_skip_bad(&chip->mtd, off, &rwsize,
					 NULL, chip->mtd.size,
					 (u_char *)(dst));
		if (ret)
			return 0;
		else
			return blkcnt;
	} else if (desc->devnum == BLK_MTD_SPI_NAND) {
		/* Not implemented */
		return 0;
	} else if (desc->devnum == BLK_MTD_SPI_NOR) {
		/* Not implemented */
		return 0;
	} else {
		return 0;
	}
}

ulong mtd_dwrite(struct udevice *udev, lbaint_t start,
		 lbaint_t blkcnt, const void *src)
{
	/* Not implemented */
	return 0;
}

ulong mtd_derase(struct udevice *udev, lbaint_t start,
		 lbaint_t blkcnt)
{
	/* Not implemented */
	return 0;
}

static int mtd_blk_probe(struct udevice *udev)
{
	struct blk_desc *desc = dev_get_uclass_platdata(udev);
	struct mtd_info *mtd = dev_get_priv(udev->parent);

	sprintf(desc->vendor, "0x%.4x", 0x2207);
	memcpy(desc->product, mtd->name, strlen(mtd->name));
	memcpy(desc->revision, "V1.00", sizeof("V1.00"));
	if (mtd->type == MTD_NANDFLASH) {
		/* Reserve 4 blocks for BBT(Bad Block Table) */
		desc->lba = (mtd->size >> 9) - (mtd->erasesize >> 9) * 4;
	} else {
		desc->lba = mtd->size >> 9;
	}

	return 0;
}

static const struct blk_ops mtd_blk_ops = {
	.read	= mtd_dread,
#ifndef CONFIG_SPL_BUILD
	.write	= mtd_dwrite,
	.erase	= mtd_derase,
#endif
};

U_BOOT_DRIVER(mtd_blk) = {
	.name		= "mtd_blk",
	.id		= UCLASS_BLK,
	.ops		= &mtd_blk_ops,
	.probe		= mtd_blk_probe,
};
