/*
 * Copyright (c) 2017 Yifeng Zhao <yifeng.zhao@rock-chips.com>
 *
 * SPDX-License-Identifier:     GPL-2.0+
 */

#include <common.h>
#include <fdtdec.h>
#include <inttypes.h>
#include <nand.h>
#include <linux/kernel.h>
#include <linux/mtd/mtd.h>
#include <linux/mtd/nand.h>
#include <linux/mtd/partitions.h>
#include <linux/io.h>

DECLARE_GLOBAL_DATA_PTR;

#define NANDC_V6_BOOTROM_ECC	24
#define NANDC_V6_NUM_BANKS	4
#define NANDC_V6_DEF_TIMEOUT	20000
#define NANDC_V6_READ		0
#define NANDC_V6_WRITE		1

#define	NANDC_REG_V6_FMCTL	0x00
#define	NANDC_REG_V6_FMWAIT	0x04
#define	NANDC_REG_V6_FLCTL	0x08
#define	NANDC_REG_V6_BCHCTL	0x0c
#define	NANDC_REG_V6_DMA_CFG	0x10
#define	NANDC_REG_V6_DMA_BUF0	0x14
#define	NANDC_REG_V6_DMA_BUF1	0x18
#define	NANDC_REG_V6_DMA_ST	0x1C
#define	NANDC_REG_V6_BCHST	0x20
#define	NANDC_REG_V6_RANDMZ	0x150
#define	NANDC_REG_V6_VER	0x160
#define	NANDC_REG_V6_INTEN	0x16C
#define	NANDC_REG_V6_INTCLR	0x170
#define	NANDC_REG_V6_INTST	0x174
#define	NANDC_REG_V6_SPARE0	0x200
#define	NANDC_REG_V6_SPARE1	0x230
#define	NANDC_REG_V6_BANK0	0x800
#define	NANDC_REG_V6_SRAM0	0x1000
#define	NANDC_REG_V6_SRAM_SIZE	0x400

#define NANDC_REG_V6_DATA	0x00
#define NANDC_REG_V6_ADDR	0x04
#define NANDC_REG_V6_CMD	0x08

/* FMCTL */
#define NANDC_V6_FM_WP		BIT(8)
#define NANDC_V6_FM_CE_SEL_M	0xFF
#define NANDC_V6_FM_CE_SEL(x)	(1 << (x))
#define NANDC_V6_FM_FREADY	BIT(9)

/* FLCTL */
#define NANDC_V6_FL_RST		BIT(0)
#define NANDC_V6_FL_DIR_S	0x1
#define NANDC_V6_FL_XFER_START	BIT(2)
#define NANDC_V6_FL_XFER_EN	BIT(3)
#define NANDC_V6_FL_ST_BUF_S	0x4
#define NANDC_V6_FL_XFER_COUNT	BIT(5)
#define NANDC_V6_FL_ACORRECT	BIT(10)
#define NANDC_V6_FL_XFER_READY	BIT(20)

/* BCHCTL */
#define NAND_V6_BCH_REGION_S	0x5
#define NAND_V6_BCH_REGION_M	0x7

/* BCHST */
#define NANDC_V6_BCH0_ST_ERR	BIT(2)
#define NANDC_V6_BCH1_ST_ERR	BIT(15)
#define NANDC_V6_ECC_ERR_CNT0(x) ((((x & (0x1F << 3)) >> 3) \
				| ((x & (1 << 27)) >> 22)) & 0x3F)
#define NANDC_V6_ECC_ERR_CNT1(x) ((((x & (0x1F << 16)) >> 16) \
				| ((x & (1 << 29)) >> 24)) & 0x3F)

struct rk_nand {
	void __iomem *regs;
	u8 chipnr;
	u8 id[5];
	u8 *databuf;
};

struct rk_nand *g_rk_nand;

static void nandc_init(struct rk_nand *rknand)
{
	writel(0x1081, rknand->regs + NANDC_REG_V6_FMWAIT);
}

static void rockchip_nand_wait_dev_ready(void __iomem *regs)
{
	u32 reg;
	u32 timeout = NANDC_V6_DEF_TIMEOUT;

	while (timeout--) {
		udelay(1);
		reg = readl(regs + NANDC_REG_V6_FMCTL);

		if ((reg & NANDC_V6_FM_FREADY))
			break;
	}
}

static void rockchip_nand_select_chip(void __iomem *regs, int chipnr)
{
	u32 reg;

	reg = readl(regs + NANDC_REG_V6_FMCTL);
	reg &= ~NANDC_V6_FM_CE_SEL_M;
	if (chipnr != -1)
		reg |= 1 << chipnr;
	writel(reg, regs + NANDC_REG_V6_FMCTL);
}

static void rockchip_nand_read_page(void __iomem *regs,
				    int page, int col)
{
	void __iomem *bank_base = regs + NANDC_REG_V6_BANK0;

	writeb(0x00, bank_base + NANDC_REG_V6_CMD);
	writeb(col, bank_base + NANDC_REG_V6_ADDR);
	writeb(col >> 8, bank_base + NANDC_REG_V6_ADDR);
	writeb(page, bank_base + NANDC_REG_V6_ADDR);
	writeb(page >> 8, bank_base + NANDC_REG_V6_ADDR);
	writeb(page >> 16, bank_base + NANDC_REG_V6_ADDR);
	writeb(0x30, bank_base + NANDC_REG_V6_CMD);
}

static void rockchip_nand_pio_xfer_start(struct rk_nand *rknand,
					 u8 dir,
					 u8 st_buf)
{
	u32 reg;

	reg = readl(rknand->regs + NANDC_REG_V6_BCHCTL);
	reg = (reg & (~(NAND_V6_BCH_REGION_M << NAND_V6_BCH_REGION_S)));
	writel(reg, rknand->regs + NANDC_REG_V6_BCHCTL);

	reg = (dir << NANDC_V6_FL_DIR_S) | (st_buf << NANDC_V6_FL_ST_BUF_S) |
		  NANDC_V6_FL_XFER_EN | NANDC_V6_FL_XFER_COUNT |
		  NANDC_V6_FL_ACORRECT;
	writel(reg, rknand->regs + NANDC_REG_V6_FLCTL);

	reg |= NANDC_V6_FL_XFER_START;
	writel(reg, rknand->regs + NANDC_REG_V6_FLCTL);
}

static int rockchip_nand_wait_pio_xfer_done(struct rk_nand *rknand)
{
	int timeout = NANDC_V6_DEF_TIMEOUT;
	int reg;

	while (timeout--) {
		reg = readl(rknand->regs + NANDC_REG_V6_FLCTL);

		if ((reg & NANDC_V6_FL_XFER_READY) != 0)
			break;

		udelay(1);
	}

	if (timeout == 0)
		return -1;

	return 0;
}

static int nandc_read_page(unsigned int page, uint8_t *buf)
{
	void __iomem *sram_base = g_rk_nand->regs + NANDC_REG_V6_SRAM0;
	unsigned int max_bitflips = 0;
	int ret, step, bch_st, ecc_step;

	ecc_step = CONFIG_SYS_NAND_PAGE_SIZE / 1024;
	rockchip_nand_select_chip(g_rk_nand->regs, 0);
	rockchip_nand_read_page(g_rk_nand->regs, page, 0);
	rockchip_nand_wait_dev_ready(g_rk_nand->regs);
	rockchip_nand_pio_xfer_start(g_rk_nand, NANDC_V6_READ, 0);

	for (step = 0; step < ecc_step; step++) {
		int data_off = step * 1024;
		u8 *data = buf + data_off;

		ret = rockchip_nand_wait_pio_xfer_done(g_rk_nand);
		if (ret)
			return ret;

		bch_st = readl(g_rk_nand->regs + NANDC_REG_V6_BCHST);

		if (bch_st & NANDC_V6_BCH0_ST_ERR) {
			max_bitflips = -1;
		} else {
			ret = NANDC_V6_ECC_ERR_CNT0(bch_st);
			max_bitflips = max_t(unsigned int, max_bitflips, ret);
		}

		if ((step + 1) < ecc_step)
			rockchip_nand_pio_xfer_start(g_rk_nand, NANDC_V6_READ,
						     (step + 1) & 0x1);

		memcpy_fromio(data, sram_base + NANDC_REG_V6_SRAM_SIZE *
			      (step & 1), 1024);
	}
	rockchip_nand_select_chip(g_rk_nand->regs, -1);

	return max_bitflips;
}

static int is_badblock(unsigned int page)
{
	int res = 0, i;
	u16 bad = 0xff;
	void __iomem *regs = g_rk_nand->regs;
	void __iomem *bank_base = regs + NANDC_REG_V6_BANK0;

	if (nandc_read_page(page, g_rk_nand->databuf) == -1) {
		rockchip_nand_select_chip(regs, 0);
		rockchip_nand_read_page(regs, page,
					CONFIG_SYS_NAND_PAGE_SIZE);
		rockchip_nand_wait_dev_ready(regs);
		for (i = 0; i < 8; i++) {
			bad = readb(bank_base);
			if (bad)
				break;
		}
		if (i >= 8)
			res = 1;
		rockchip_nand_select_chip(regs, 0);
	}
	if (res)
		printf("%s 0x%x %x %x\n", __func__, page, res, bad);
	return res;
}

static void read_flash_id(struct rk_nand *rknand, uint8_t *id)
{
	void __iomem *bank_base = rknand->regs + NANDC_REG_V6_BANK0;

	rockchip_nand_wait_dev_ready(g_rk_nand->regs);
	writeb(0x90, bank_base + NANDC_REG_V6_CMD);
	writeb(0x00, bank_base + NANDC_REG_V6_ADDR);
	udelay(1);
	id[0] = readb(bank_base);
	id[1] = readb(bank_base);
	id[2] = readb(bank_base);
	id[3] = readb(bank_base);
	id[4] = readb(bank_base);
	rockchip_nand_select_chip(rknand->regs, -1);
	printf("%s %x %x %x %x %x\n", __func__, id[0], id[1], id[2], id[3],
	       id[4]);
}

void board_nand_init(void)
{
	const void *blob = gd->fdt_blob;
	fdt_addr_t regs;
	int node;

	if (g_rk_nand)
		return;

	node = fdtdec_next_compatible(blob, 0, COMPAT_ROCKCHIP_NANDC);

	if (node < 0) {
		printf("Nand node not found\n");
		goto err;
	}

	if (!fdtdec_get_is_enabled(blob, node)) {
		debug("Nand disabled in device tree\n");
		goto err;
	}

	regs = fdt_get_base_address(blob, node);
	if (regs == FDT_ADDR_T_NONE) {
		debug("Nand address not found\n");
		goto err;
	}

	g_rk_nand = kzalloc(sizeof(*g_rk_nand), GFP_KERNEL);
	g_rk_nand->regs = (void *)regs;
	g_rk_nand->databuf = kzalloc(CONFIG_SYS_NAND_PAGE_SIZE, GFP_KERNEL);
	nandc_init(g_rk_nand);
	read_flash_id(g_rk_nand, g_rk_nand->id);
	if (g_rk_nand->id[0] != 0xFF && g_rk_nand->id[1] != 0xFF &&
	    g_rk_nand->id[0] != 0x00 && g_rk_nand->id[1] != 0x00)
		g_rk_nand->chipnr = 1;
	return;
err:
	kfree(g_rk_nand);
}

int nand_spl_load_image(u32 offs, u32 size, void *buf)
{
	int i;
	unsigned int page;
	unsigned int maxpages = CONFIG_SYS_NAND_SIZE /
				CONFIG_SYS_NAND_PAGE_SIZE;

	/* Convert to page number */
	page = offs / CONFIG_SYS_NAND_PAGE_SIZE;
	i = 0;

	size = roundup(size, CONFIG_SYS_NAND_PAGE_SIZE);
	while (i < size / CONFIG_SYS_NAND_PAGE_SIZE) {
		/*
		 * Check if we have crossed a block boundary, and if so
		 * check for bad block.
		 */
		if (!(page % CONFIG_SYS_NAND_PAGE_COUNT)) {
			/*
			 * Yes, new block. See if this block is good. If not,
			 * loop until we find a good block.
			 */
			while (is_badblock(page)) {
				page = page + CONFIG_SYS_NAND_PAGE_COUNT;
				/* Check i we've reached the end of flash. */
				if (page >= maxpages)
					return -EIO;
			}
		}

		if (nandc_read_page(page, buf) < 0)
			return -EIO;

		page++;
		i++;
		buf = buf + CONFIG_SYS_NAND_PAGE_SIZE;
	}
	return 0;
}

void nand_init(void)
{
	board_nand_init();
}

int rk_nand_init(void)
{
	board_nand_init();
	if (g_rk_nand && g_rk_nand->chipnr)
		return 0;
	else
		return -ENODEV;
}

void nand_deselect(void) {}
