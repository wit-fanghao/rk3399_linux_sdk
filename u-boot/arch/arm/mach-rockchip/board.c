/*
 * (C) Copyright 2017 Rockchip Electronics Co., Ltd.
 *
 * SPDX-License-Identifier:     GPL-2.0+
 */
#include <common.h>
#include <amp.h>
#include <clk.h>
#include <bidram.h>
#include <dm.h>
#include <debug_uart.h>
#include <key.h>
#include <memblk.h>
#include <ram.h>
#include <syscon.h>
#include <sysmem.h>
#include <asm/io.h>
#include <asm/arch/vendor.h>
#include <misc.h>
#include <asm/gpio.h>
#include <dm/uclass-internal.h>
#include <asm/arch/clock.h>
#include <asm/arch/cpu.h>
#include <asm/arch/periph.h>
#include <asm/arch/boot_mode.h>
#include <asm/arch/hotkey.h>
#include <asm/arch/rk_atags.h>
#include <asm/arch/param.h>
#ifdef CONFIG_DM_CHARGE_DISPLAY
#include <power/charge_display.h>
#endif
#ifdef CONFIG_DM_DVFS
#include <dvfs.h>
#endif
#ifdef CONFIG_ROCKCHIP_IO_DOMAIN
#include <io-domain.h>
#endif
#ifdef CONFIG_DM_REGULATOR
#include <power/regulator.h>
#endif
#ifdef CONFIG_DRM_ROCKCHIP
#include <video_rockchip.h>
#endif
#ifdef CONFIG_ROCKCHIP_DEBUGGER
#include <rockchip_debugger.h>
#endif
#include <of_live.h>
#include <dm/root.h>
#include <console.h>

DECLARE_GLOBAL_DATA_PTR;
/* define serialno max length, the max length is 512 Bytes
 * The remaining bytes are used to ensure that the first 512 bytes
 * are valid when executing 'env_set("serial#", value)'.
 */
#define VENDOR_SN_MAX	513
#define CPUID_LEN       0x10
#define CPUID_OFF       0x7

static int rockchip_set_ethaddr(void)
{
#ifdef CONFIG_ROCKCHIP_VENDOR_PARTITION
	int ret;
	u8 ethaddr[ARP_HLEN];
	char buf[ARP_HLEN_ASCII + 1];

	ret = vendor_storage_read(VENDOR_LAN_MAC_ID, ethaddr, sizeof(ethaddr));
	if (ret > 0 && is_valid_ethaddr(ethaddr)) {
		sprintf(buf, "%pM", ethaddr);
		env_set("ethaddr", buf);
	}
#endif
	return 0;
}

static int rockchip_set_serialno(void)
{
	char serialno_str[VENDOR_SN_MAX];
	int ret = 0, i;
	u8 cpuid[CPUID_LEN] = {0};
	u8 low[CPUID_LEN / 2], high[CPUID_LEN / 2];
	u64 serialno;

	/* Read serial number from vendor storage part */
	memset(serialno_str, 0, VENDOR_SN_MAX);
#ifdef CONFIG_ROCKCHIP_VENDOR_PARTITION
	ret = vendor_storage_read(VENDOR_SN_ID, serialno_str, (VENDOR_SN_MAX-1));
	if (ret > 0) {
		env_set("serial#", serialno_str);
	} else {
#endif
#ifdef CONFIG_ROCKCHIP_EFUSE
		struct udevice *dev;

		/* retrieve the device */
		ret = uclass_get_device_by_driver(UCLASS_MISC,
						  DM_GET_DRIVER(rockchip_efuse), &dev);
		if (ret) {
			printf("%s: could not find efuse device\n", __func__);
			return ret;
		}
		/* read the cpu_id range from the efuses */
		ret = misc_read(dev, CPUID_OFF, &cpuid, sizeof(cpuid));
		if (ret) {
			printf("%s: reading cpuid from the efuses failed\n", __func__);
			return ret;
		}
#else
		/* generate random cpuid */
		for (i = 0; i < CPUID_LEN; i++) {
			cpuid[i] = (u8)(rand());
		}
#endif
		/* Generate the serial number based on CPU ID */
		for (i = 0; i < 8; i++) {
			low[i] = cpuid[1 + (i << 1)];
			high[i] = cpuid[i << 1];
		}
		serialno = crc32_no_comp(0, low, 8);
		serialno |= (u64)crc32_no_comp(serialno, high, 8) << 32;
		snprintf(serialno_str, sizeof(serialno_str), "%llx", serialno);

		env_set("serial#", serialno_str);
#ifdef CONFIG_ROCKCHIP_VENDOR_PARTITION
	}
#endif
	return ret;
}

#if defined(CONFIG_USB_FUNCTION_FASTBOOT)
int fb_set_reboot_flag(void)
{
	printf("Setting reboot to fastboot flag ...\n");
	/* Set boot mode to fastboot */
	writel(BOOT_FASTBOOT, CONFIG_ROCKCHIP_BOOT_MODE_REG);

	return 0;
}
#endif

__weak int rk_board_init(void)
{
	return 0;
}

__weak int rk_board_late_init(void)
{
	return 0;
}

__weak int soc_clk_dump(void)
{
	return 0;
}

__weak int set_armclk_rate(void)
{
	return 0;
}

int board_late_init(void)
{
	rockchip_set_ethaddr();
	rockchip_set_serialno();
#if (CONFIG_ROCKCHIP_BOOT_MODE_REG > 0)
	setup_boot_mode();
#endif

#ifdef CONFIG_DM_CHARGE_DISPLAY
	charge_display();
#endif

#ifdef CONFIG_DRM_ROCKCHIP
	rockchip_show_logo();
#endif

	soc_clk_dump();

	return rk_board_late_init();
}

#ifdef CONFIG_USING_KERNEL_DTB
#include <asm/arch/resource_img.h>

/* Here, only fixup cru phandle, pmucru is not included */
static int phandles_fixup(void *fdt)
{
	const char *props[] = { "clocks", "assigned-clocks" };
	struct udevice *dev;
	struct uclass *uc;
	const char *comp;
	u32 id, nclocks;
	u32 *clocks;
	int phandle, ncells;
	int off, offset;
	int ret, length;
	int i, j;
	int first_phandle = -1;

	phandle = -ENODATA;
	ncells = -ENODATA;

	/* fdt points to kernel dtb, getting cru phandle and "#clock-cells" */
	for (offset = fdt_next_node(fdt, 0, NULL);
	     offset >= 0;
	     offset = fdt_next_node(fdt, offset, NULL)) {
		comp = fdt_getprop(fdt, offset, "compatible", NULL);
		if (!comp)
			continue;

		/* Actually, this is not a good method to get cru node */
		off = strlen(comp) - strlen("-cru");
		if (off > 0 && !strncmp(comp + off, "-cru", 4)) {
			phandle = fdt_get_phandle(fdt, offset);
			ncells = fdtdec_get_int(fdt, offset,
						"#clock-cells", -ENODATA);
			break;
		}
	}

	if (phandle == -ENODATA || ncells == -ENODATA)
		return 0;

	debug("%s: target cru: clock-cells:%d, phandle:0x%x\n",
	      __func__, ncells, fdt32_to_cpu(phandle));

	/* Try to fixup all cru phandle from U-Boot dtb nodes */
	for (id = 0; id < UCLASS_COUNT; id++) {
		ret = uclass_get(id, &uc);
		if (ret)
			continue;

		if (list_empty(&uc->dev_head))
			continue;

		list_for_each_entry(dev, &uc->dev_head, uclass_node) {
			/* Only U-Boot node go further */
			if (!dev_read_bool(dev, "u-boot,dm-pre-reloc"))
				continue;

			for (i = 0; i < ARRAY_SIZE(props); i++) {
				if (!dev_read_prop(dev, props[i], &length))
					continue;

				clocks = malloc(length);
				if (!clocks)
					return -ENOMEM;

				/* Read "props[]" which contains cru phandle */
				nclocks = length / sizeof(u32);
				if (dev_read_u32_array(dev, props[i],
						       clocks, nclocks)) {
					free(clocks);
					continue;
				}

				/* Fixup with kernel cru phandle */
				for (j = 0; j < nclocks; j += (ncells + 1)) {
					/*
					 * Check: update pmucru phandle with cru
					 * phandle by mistake.
					 */
					if (first_phandle == -1)
						first_phandle = clocks[j];

					if (clocks[j] != first_phandle) {
						debug("WARN: %s: first cru phandle=%d, this=%d\n",
						      dev_read_name(dev),
						      first_phandle, clocks[j]);
						continue;
					}

					clocks[j] = phandle;
				}

				/*
				 * Override live dt nodes but not fdt nodes,
				 * because all U-Boot nodes has been imported
				 * to live dt nodes, should use "dev_xxx()".
				 */
				dev_write_u32_array(dev, props[i],
						    clocks, nclocks);
				free(clocks);
			}
		}
	}

	return 0;
}

int init_kernel_dtb(void)
{
	int ret = 0;
	ulong fdt_addr = 0;

	fdt_addr = env_get_ulong("fdt_addr_r", 16, 0);
	if (!fdt_addr) {
		printf("No Found FDT Load Address.\n");
		return -1;
	}

	ret = rockchip_read_dtb_file((void *)fdt_addr);
	if (ret < 0) {
		printf("%s dtb in resource read fail\n", __func__);
		return 0;
	}

	/*
	 * There is a phandle miss match between U-Boot and kernel dtb node,
	 * the typical is cru phandle, we fixup it in U-Boot live dt nodes.
	 */
	phandles_fixup((void *)fdt_addr);

	of_live_build((void *)fdt_addr, (struct device_node **)&gd->of_root);

	dm_scan_fdt((void *)fdt_addr, false);

	gd->fdt_blob = (void *)fdt_addr;

	/* Reserve 'reserved-memory' */
	ret = boot_fdt_add_sysmem_rsv_regions((void *)gd->fdt_blob);
	if (ret)
		return ret;

	return 0;
}
#endif

void board_env_fixup(void)
{
	struct memblock mem;
	ulong u_addr_r;
	phys_size_t end;
	char *addr_r;

#ifdef ENV_MEM_LAYOUT_SETTINGS1
	const char *env_addr0[] = {
		"scriptaddr", "pxefile_addr_r",
		"fdt_addr_r", "kernel_addr_r", "ramdisk_addr_r",
	};
	const char *env_addr1[] = {
		"scriptaddr1", "pxefile_addr1_r",
		"fdt_addr1_r", "kernel_addr1_r", "ramdisk_addr1_r",
	};
	int i;

	/* 128M is a typical ram size for most platform, so as default here */
	if (gd->ram_size <= SZ_128M) {
		/* Replace orignal xxx_addr_r */
		for (i = 0; i < ARRAY_SIZE(env_addr1); i++) {
			addr_r = env_get(env_addr1[i]);
			if (addr_r)
				env_set(env_addr0[i], addr_r);
		}
	}
#endif
	/* If bl32 is disabled, maybe kernel can be load to lower address. */
	if (!(gd->flags & GD_FLG_BL32_ENABLED)) {
		addr_r = env_get("kernel_addr_no_bl32_r");
		if (addr_r)
			env_set("kernel_addr_r", addr_r);
	/* If bl32 is enlarged, we move ramdisk addr right behind it */
	} else {
		mem = param_parse_optee_mem();
		end = mem.base + mem.size;
		u_addr_r = env_get_ulong("ramdisk_addr_r", 16, 0);
		if (u_addr_r >= mem.base && u_addr_r < end)
			env_set_hex("ramdisk_addr_r", end);
	}
}

static void early_download_init(void)
{
#if defined(CONFIG_PWRKEY_DNL_TRIGGER_NUM) && \
		(CONFIG_PWRKEY_DNL_TRIGGER_NUM > 0)
	if (pwrkey_download_init())
		printf("Pwrkey download init failed\n");
#endif

	if (!tstc())
		return;

	gd->console_evt = getc();
	if (gd->console_evt <= 0x1a) /* 'z' */
		printf("Hotkey: ctrl+%c\n", (gd->console_evt + 'a' - 1));

#if (CONFIG_ROCKCHIP_BOOT_MODE_REG > 0)
	/* ctrl+b */
	if (is_hotkey(HK_BROM_DNL)) {
		printf("Enter bootrom download...");
		flushc();
		writel(BOOT_BROM_DOWNLOAD, CONFIG_ROCKCHIP_BOOT_MODE_REG);
		do_reset(NULL, 0, 0, NULL);
		printf("failed!\n");
	}
#endif
}

int board_init(void)
{
	int ret;

	board_debug_uart_init();

#ifdef CONFIG_USING_KERNEL_DTB
	init_kernel_dtb();
#endif
	early_download_init();

	/*
	 * pmucru isn't referenced on some platforms, so pmucru driver can't
	 * probe that the "assigned-clocks" is unused.
	 */
	clks_probe();
#ifdef CONFIG_DM_REGULATOR
	ret = regulators_enable_boot_on(false);
	if (ret)
		debug("%s: Cannot enable boot on regulator\n", __func__);
#endif

#ifdef CONFIG_ROCKCHIP_IO_DOMAIN
	io_domain_init();
#endif

	set_armclk_rate();

#ifdef CONFIG_DM_DVFS
	dvfs_init(true);
#endif

	return rk_board_init();
}

int interrupt_debugger_init(void)
{
	int ret = 0;

#ifdef CONFIG_ROCKCHIP_DEBUGGER
	ret = rockchip_debugger_init();
#endif
	return ret;
}

#if defined(CONFIG_ROCKCHIP_RK1808) && !defined(CONFIG_COPROCESSOR_RK1808)
#define PINCTRL_EMMC_BUS8_PATH		"/pinctrl/emmc/emmc-bus8"
#define PINCTRL_EMMC_CMD_PATH		"/pinctrl/emmc/emmc-cmd"
#define PINCTRL_EMMC_CLK_PATH		"/pinctrl/emmc/emmc-clk"
#define PINCTRL_PCFG_PU_2MA_PATH	"/pinctrl/pcfg-pull-up-2ma"
#define MAX_ROCKCHIP_PINS_ENTRIES	12

static int rockchip_pinctrl_cfg_fdt_fixup(const char *path, u32 new_phandle)
{
	u32 cells[MAX_ROCKCHIP_PINS_ENTRIES * 4];
	const u32 *data;
	int i, count;
	int node;

	node = fdt_path_offset(gd->fdt_blob, path);
	if (node < 0) {
		debug("%s: can't find: %s\n", __func__, path);
		return node;
	}

	data = fdt_getprop(gd->fdt_blob, node, "rockchip,pins", &count);
	if (!data) {
		debug("%s: can't find prop \"rockchip,pins\"\n", __func__);
		return -ENODATA;
	}

	count /= sizeof(u32);
	if (count > MAX_ROCKCHIP_PINS_ENTRIES * 4) {
		debug("%s: %d is over max count\n", __func__, count);
		return -EINVAL;
	}

	for (i = 0; i < count; i++)
		cells[i] = data[i];

	for (i = 0; i < (count >> 2); i++)
		cells[4 * i + 3] = cpu_to_fdt32(new_phandle);

	fdt_setprop((void *)gd->fdt_blob, node, "rockchip,pins",
		    &cells, count * sizeof(u32));

	return 0;
}
#endif

int board_fdt_fixup(void *blob)
{
	int ret = 0;

	/*
	 * Common fixup for DRM
	 */
#ifdef CONFIG_DRM_ROCKCHIP
	rockchip_display_fixup(blob);
#endif

	/*
	 * Platform fixup:
	 *
	 * - RK3288: Recognize RK3288W by HDMI Revision ID is 0x1A;
	 * - RK1808: MMC strength 2mA;
	 */
#ifdef CONFIG_ROCKCHIP_RK3288
	if (soc_is_rk3288w()) {
		ret = fdt_setprop_string(blob, 0,
					 "compatible", "rockchip,rk3288w");
		if (ret)
			printf("fdt set compatible failed: %d\n", ret);
	}
#elif defined(CONFIG_ROCKCHIP_RK1808) && !defined(CONFIG_COPROCESSOR_RK1808)
	struct tag *t;
	u32 ph_pu_2ma;

	t = atags_get_tag(ATAG_SOC_INFO);
	if (!t)
		return 0;

	debug("soc=0x%x, flags=0x%x\n", t->u.soc.name, t->u.soc.flags);

	if (t->u.soc.flags != SOC_FLAGS_ET00)
		return 0;

	ph_pu_2ma = fdt_get_phandle(gd->fdt_blob,
		    fdt_path_offset(gd->fdt_blob, PINCTRL_PCFG_PU_2MA_PATH));
	if (!ph_pu_2ma) {
		debug("Can't find: %s\n", PINCTRL_PCFG_PU_2MA_PATH);
		return -EINVAL;
	}

	ret |= rockchip_pinctrl_cfg_fdt_fixup(PINCTRL_EMMC_BUS8_PATH, ph_pu_2ma);
	ret |= rockchip_pinctrl_cfg_fdt_fixup(PINCTRL_EMMC_CMD_PATH, ph_pu_2ma);
	ret |= rockchip_pinctrl_cfg_fdt_fixup(PINCTRL_EMMC_CLK_PATH, ph_pu_2ma);
#endif

	return ret;
}

#ifdef CONFIG_ARM64_BOOT_AARCH32
/*
 * Fixup MMU region attr for OP-TEE on ARMv8 CPU:
 *
 * What ever U-Boot is 64-bit or 32-bit mode, the OP-TEE is always 64-bit mode.
 *
 * Command for OP-TEE:
 *	64-bit mode: dcache is always enabled;
 *	32-bit mode: dcache is always disabled(Due to some unknown issue);
 *
 * Command for U-Boot:
 *	64-bit mode: MMU table is static defined in rkxxx.c file, all memory
 *		     regions are mapped. That's good to match OP-TEE MMU policy.
 *
 *	32-bit mode: MMU table is setup according to gd->bd->bi_dram[..] where
 *		     the OP-TEE region has been reserved, so it can not be
 *		     mapped(i.e. dcache is disabled). That's also good to match
 *		     OP-TEE MMU policy.
 *
 * For the data coherence when communication between U-Boot and OP-TEE, U-Boot
 * should follow OP-TEE MMU policy.
 *
 * Here is the special:
 *	When CONFIG_ARM64_BOOT_AARCH32 is enabled, U-Boot is 32-bit mode while
 *	OP-TEE is still 64-bit mode. U-Boot would not map MMU table for OP-TEE
 *	region(but OP-TEE requires it cacheable) so we fixup here.
 */
int board_initr_caches_fixup(void)
{
	struct memblock mem;

	mem = param_parse_optee_mem();
	if (mem.size)
		mmu_set_region_dcache_behaviour(mem.base, mem.size,
						DCACHE_WRITEBACK);
	return 0;
}
#endif

void board_quiesce_devices(void)
{
	hotkey_run(HK_CMDLINE);
	hotkey_run(HK_CLI);

#ifdef CONFIG_ROCKCHIP_PRELOADER_ATAGS
	/* Destroy atags makes next warm boot safer */
	atags_destroy();
#endif

#if defined(CONFIG_CONSOLE_RECORD)
	/* Print record console data */
	console_record_print_purge();
#endif
}

void enable_caches(void)
{
	icache_enable();
	dcache_enable();
}

#ifdef CONFIG_LMB
/*
 * Using last bi_dram[...] to initialize "bootm_low" and "bootm_mapsize".
 * This makes lmb_alloc_base() always alloc from tail of sdram.
 * If we don't assign it, bi_dram[0] is used by default and it may cause
 * lmb_alloc_base() fail when bi_dram[0] range is small.
 */
void board_lmb_reserve(struct lmb *lmb)
{
	u64 start, size;
	char bootm_low[32];
	char bootm_mapsize[32];
	int i;

	for (i = 0; i < CONFIG_NR_DRAM_BANKS; i++) {
		if (!gd->bd->bi_dram[i].size)
			break;
	}

	start = gd->bd->bi_dram[i - 1].start;
	size = gd->bd->bi_dram[i - 1].size;

	/*
	 * 32-bit kernel: ramdisk/fdt shouldn't be loaded to highmem area(768MB+),
	 * otherwise "Unable to handle kernel paging request at virtual address ...".
	 *
	 * So that we hope limit highest address at 768M, but there comes the the
	 * problem: ramdisk is a compressed image and it expands after descompress,
	 * so it accesses 768MB+ and brings the above "Unable to handle kernel ...".
	 *
	 * We make a appointment that the highest memory address is 512MB, it
	 * makes lmb alloc safer.
	 */
#ifndef CONFIG_ARM64
	if (start >= ((u64)CONFIG_SYS_SDRAM_BASE + SZ_512M)) {
		start = gd->bd->bi_dram[i - 2].start;
		size = gd->bd->bi_dram[i - 2].size;
	}

	if ((start + size) > ((u64)CONFIG_SYS_SDRAM_BASE + SZ_512M))
		size = (u64)CONFIG_SYS_SDRAM_BASE + SZ_512M - start;
#endif
	sprintf(bootm_low, "0x%llx", start);
	sprintf(bootm_mapsize, "0x%llx", size);
	env_set("bootm_low", bootm_low);
	env_set("bootm_mapsize", bootm_mapsize);
}
#endif

#ifdef CONFIG_BIDRAM
int board_bidram_reserve(struct bidram *bidram)
{
	struct memblock mem;
	int ret;

	/* ATF */
	mem = param_parse_atf_mem();
	ret = bidram_reserve(MEMBLK_ID_ATF, mem.base, mem.size);
	if (ret)
		return ret;

	/* PSTORE/ATAGS/SHM */
	mem = param_parse_common_resv_mem();
	ret = bidram_reserve(MEMBLK_ID_SHM, mem.base, mem.size);
	if (ret)
		return ret;

	/* OP-TEE */
	mem = param_parse_optee_mem();
	ret = bidram_reserve(MEMBLK_ID_OPTEE, mem.base, mem.size);
	if (ret)
		return ret;

	return 0;
}

parse_fn_t board_bidram_parse_fn(void)
{
	return param_parse_ddr_mem;
}
#endif

#ifdef CONFIG_ROCKCHIP_AMP
void cpu_secondary_init_r(void)
{
	amp_cpus_on();
}
#endif

#if defined(CONFIG_ROCKCHIP_PRELOADER_SERIAL) && \
    defined(CONFIG_ROCKCHIP_PRELOADER_ATAGS)
int board_init_f_init_serial(void)
{
	struct tag *t = atags_get_tag(ATAG_SERIAL);

	if (t) {
		gd->serial.using_pre_serial = t->u.serial.enable;
		gd->serial.addr = t->u.serial.addr;
		gd->serial.baudrate = t->u.serial.baudrate;
		gd->serial.id = t->u.serial.id;

		debug("%s: enable=%d, addr=0x%lx, baudrate=%d, id=%d\n",
		      __func__, gd->serial.using_pre_serial,
		      gd->serial.addr, gd->serial.baudrate,
		      gd->serial.id);
	}

	return 0;
}
#endif

#if defined(CONFIG_USB_GADGET) && defined(CONFIG_USB_GADGET_DWC2_OTG)
#include <fdt_support.h>
#include <usb.h>
#include <usb/dwc2_udc.h>

static struct dwc2_plat_otg_data otg_data = {
	.rx_fifo_sz	= 512,
	.np_tx_fifo_sz	= 16,
	.tx_fifo_sz	= 128,
};

int board_usb_init(int index, enum usb_init_type init)
{
	int node;
	fdt_addr_t addr;
	const fdt32_t *reg;
	const void *blob = gd->fdt_blob;

	/* find the usb_otg node */
	node = fdt_node_offset_by_compatible(blob, -1, "snps,dwc2");

retry:
	if (node > 0) {
		reg = fdt_getprop(blob, node, "reg", NULL);
		if (!reg)
			return -EINVAL;

		addr = fdt_translate_address(blob, node, reg);
		if (addr == OF_BAD_ADDR) {
			pr_err("Not found usb_otg address\n");
			return -EINVAL;
		}

#if defined(CONFIG_ROCKCHIP_RK3288)
		if (addr != 0xff580000) {
			node = fdt_node_offset_by_compatible(blob, node,
							     "snps,dwc2");
			goto retry;
		}
#endif
	} else {
		/*
		 * With kernel dtb support, rk3288 dwc2 otg node
		 * use the rockchip legacy dwc2 driver "dwc_otg_310"
		 * with the compatible "rockchip,rk3288_usb20_otg",
		 * and rk3368 also use the "dwc_otg_310" driver with
		 * the compatible "rockchip,rk3368-usb".
		 */
#if defined(CONFIG_ROCKCHIP_RK3288)
		node = fdt_node_offset_by_compatible(blob, -1,
				"rockchip,rk3288_usb20_otg");
#elif defined(CONFIG_ROCKCHIP_RK3368)
		node = fdt_node_offset_by_compatible(blob, -1,
				"rockchip,rk3368-usb");
#endif
		if (node > 0) {
			goto retry;
		} else {
			pr_err("Not found usb_otg device\n");
			return -ENODEV;
		}
	}

	otg_data.regs_otg = (uintptr_t)addr;

	return dwc2_udc_probe(&otg_data);
}

int board_usb_cleanup(int index, enum usb_init_type init)
{
	return 0;
}
#endif
