// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2019 Fuzhou Rockchip Electronics Co., Ltd
 */

#include <common.h>
#include <malloc.h>
#include <spl_rkfw.h>

static const __aligned(16) struct s_fip_name_id fip_name_id[] = {
	{ BL30_IMAGE_NAME, UUID_SCP_FIRMWARE_BL30 },		/* optional */
	{ BL31_IMAGE_NAME, UUID_EL3_RUNTIME_FIRMWARE_BL31 },	/* mandatory */
	{ BL32_IMAGE_NAME, UUID_SECURE_PAYLOAD_BL32 },		/* optional */
};

static int file2comp_id(const char *file_name, u32 *comp_id)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(fip_name_id); i++) {
		if (!strcmp(file_name, fip_name_id[i].name)) {
			*comp_id = fip_name_id[i].id;
			return 0;
		}
	}

	return -ENOENT;
}

static int open_image(const char *image_name, tboot_entry *entry,
		      struct tag_tboot_header_2k *hdr)
{
	u32 i, component_num, sign_offset;
	component_data *pcompdata;
	boot_component *pcomp;
	int n_found = 0;
	u32 comp_id;
	int ret;

	ret = file2comp_id(image_name, &comp_id);
	if (ret) {
		printf("Can't find unknown image: %s\n", image_name);
		return ret;
	}

	component_num = (hdr->size >> 16) & 0xffff;
	sign_offset = (hdr->size & 0xffff) << 2;
	pcompdata = (component_data *)((char *)hdr + sizeof(tboot_header));
	pcomp = (boot_component *)((char *)hdr + sign_offset + SIGNATURE_SIZE);

	for (i = 0; i < component_num; i++) {
		if (comp_id == pcomp->component_id) {
			if (n_found < MAX_BL_CODE_NUM) {
				memcpy(&entry[n_found].component, pcomp,
				       sizeof(boot_component));
				memcpy(&entry[n_found].compdata, pcompdata,
				       sizeof(component_data));
				n_found++;
			} else {
				printf("Image num excess max: %d!\n",
				       MAX_BL_CODE_NUM);
				return -EINVAL;
			}
		} else {
			if (n_found > 0)
				break;
		}

		pcomp++;
		pcompdata++;
	}

	if (!n_found) {
		printf("No find %s\n", image_name);
		return -ENONET;
	}

	return n_found;
}

static int check_image(struct tag_tboot_header_2k *hdr)
{
	u32 hash_format[] = { 0, 160, 256, 256 };

	/* HASH format identifier */
	return (hash_format[hdr->flags & 0x3] == 0) ? -EINVAL : 0;
}

static int load_image(struct spl_load_info *info,
		      struct tag_tboot_header_2k *hdr,
		      u32 image_sector,
		      const char *image_name,
		      uintptr_t *entry_point)
{
	tboot_entry entry[MAX_BL_CODE_NUM];
	void *image_buf = NULL;
	ulong load_addr;
	u32 sect_off;
	u32 sect_cnt;
	int image_num;
	int i, ret;

	/* Parse components from image header */
	image_num = open_image(image_name, entry, hdr);
	if (image_num < 0)
		return image_num;

	/* Get all component */
	for (i = 0; i < image_num; i++) {
		load_addr = entry[i].compdata.load_addr;
		sect_cnt = entry[i].component.image_size;
		sect_off = entry[i].component.storage_addr;

		printf("%s[%d]: addr=0x%lx, size=0x%lx\n",
		       image_name, i, load_addr, (ulong)sect_cnt * 512);

		/*
		 * MMC/NAND controller DMA can't access sram region, so:
		 * data -> ddr buffer -> memcpy to sram region.
		 */
		if (load_addr >= SDRAM_MAX_SIZE) {
			image_buf = memalign(ARCH_DMA_MINALIGN, sect_cnt * 512);
			if (!image_buf) {
				printf("%s: malloc failed\n", __func__);
				return -ENOMEM;
			}
		} else {
			image_buf = (void *)load_addr;
		}

		ret = info->read(info, image_sector + sect_off,
				 sect_cnt, image_buf);
		if (ret != sect_cnt) {
			printf("Read '%s' failed at sector: %ld, ret=%d\n",
			       image_name, (ulong)image_sector + sect_off, ret);
			return -EIO;
		}

		/* Verify component */
		ret = check_image(hdr);
		if (ret) {
			printf("%s[%d]: verify image fail!\n", image_name, i);
			return ret;
		}

		/* Handle sram region */
		if ((ulong)image_buf != load_addr) {
			memcpy((void *)load_addr, image_buf, sect_cnt << 9);
			free(image_buf);
		}

		/* Fill entry_point by first component */
		if (i == 0)
			*entry_point = (uintptr_t)load_addr;
	}

	return ret;
}

static int rkfw_load_trust(struct spl_load_info *info, u32 image_sector,
			   uintptr_t *bl31_entry, uintptr_t *bl32_entry,
			   int *found_rkfw, u32 try_count)
{
	struct tag_tboot_header_2k hdr;
	u32 sect_addr = image_sector;
	int blkcnt = 4;	/* header sectors, 2KB */
	int i, ret = 0;

	/* Find valid image header */
	for (i = 0; i < try_count; i++) {
		sect_addr = image_sector + (i * RKFW_RETRY_SECTOR_SIZE);
		if (blkcnt != info->read(info, sect_addr, blkcnt, &hdr))
			continue;

		if (hdr.tag == TBOOT_HEAD_TAG) {
			/* Mark it */
			*found_rkfw = 1;

			/* bl31 is mandatory */
			ret = load_image(info, &hdr, sect_addr,
					 BL31_IMAGE_NAME, bl31_entry);
			if (ret)
				continue;

			/* bl32 is optional */
			ret = load_image(info, &hdr, sect_addr,
					 BL32_IMAGE_NAME, bl32_entry);
			if (ret) {
				if (ret == -ENONET) {
					*bl32_entry = -1;	/* Not exist */
					ret = 0;
				} else {
					continue;
				}
			}
			break;
		}
	}

	return ret;
}

static int rkfw_load_uboot(struct spl_load_info *info, u32 image_sector,
			   uintptr_t *bl33_entry, u32 try_count)
{
	struct tag_second_loader_hdr hdr;
	int i, ret, blkcnt = 4;	/* header sectors, 2KB */
	char *load_addr;
	u32 sect_addr;

	/* Detect valid image header */
	for (i = 0; i < try_count; i++) {
		sect_addr = image_sector + (i * RKFW_RETRY_SECTOR_SIZE);
		ret = info->read(info, sect_addr, blkcnt, &hdr);
		if (ret != blkcnt)
			continue;

		if (!memcmp(hdr.magic, LOADER_HARD_STR, 6)) {
			/* Load full binary image(right behind header) */
			sect_addr += blkcnt;
			load_addr = (char *)((size_t)hdr.loader_load_addr);
			blkcnt = DIV_ROUND_UP(hdr.loader_load_size, 512);

			printf("u-boot.bin: addr=0x%lx, size=0x%lx\n",
			       (ulong)load_addr, (ulong)blkcnt * 512);
			ret = info->read(info, sect_addr, blkcnt, load_addr);
			if (ret != blkcnt)
				continue;

			break;
		}
	}

	if (i == try_count) {
		printf("Can not find usable uboot\n");
		return -ENONET;
	}

	/* Fill entry point */
	*bl33_entry = (uintptr_t)hdr.loader_load_addr;

	return 0;
}

int spl_load_rkfw_image(struct spl_image_info *spl_image,
			struct spl_load_info *info,
			u32 trust_sector, u32 uboot_sector)
{
	int ret, try_count = RKFW_RETRY_SECTOR_TIMES;
	int found_rkfw = 0;

	ret = rkfw_load_trust(info, trust_sector,
			      &spl_image->entry_point,
			      &spl_image->entry_point_bl32,
			      &found_rkfw, try_count);
	if (ret) {
		printf("Load trust image failed! ret=%d\n", ret);
		goto out;
	}

	ret = rkfw_load_uboot(info, uboot_sector,
			      &spl_image->entry_point_bl33, try_count);
	if (ret) {
		printf("Load uboot image failed! ret=%d\n", ret);
		goto out;
	}

#if CONFIG_IS_ENABLED(LOAD_FIT)
	spl_image->fdt_addr = 0;
#endif
	spl_image->os = IH_OS_ARM_TRUSTED_FIRMWARE;

out:
	/* If not found rockchip firmware, try others outside */
	return found_rkfw ? ret : -EAGAIN;
}
