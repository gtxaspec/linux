// SPDX-License-Identifier: GPL-2.0
/*
 * Ingenic SoCs SPI Flash Controller Driver
 * Copyright (c) 2022 周琰杰 (Zhou Yanjie) <zhouyanjie@wanyeetech.com>
 */

#include <linux/bitfield.h>
#include <linux/bitops.h>
#include <linux/clk.h>
#include <linux/completion.h>
#include <linux/interrupt.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/spi/spi.h>
#include <linux/spi/spi-mem.h>

/* SFC register offsets */
#define SFC_REG_GLB						0x0000
#define	SFC_REG_DEV_CONF				0x0004
#define	SFC_REG_DEV_STA_EXP				0x0008
#define	SFC_REG_DEV_STA_RT				0x000c
#define	SFC_REG_DEV_STA_MSK				0x0010
#define	SFC_REG_TRAN_CONF(n)			(0x0014 + (n) * 4)
#define	SFC_REG_TRAN_CFG0(n)			(0x0014 + (n) * 4)
#define	SFC_REG_TRAN_LEN				0x002c
#define	SFC_REG_DEV_ADDR(n)				(0x0030 + (n) * 4)
#define	SFC_REG_DEV_ADDR_PLUS(n)		(0x0048 + (n) * 4)
#define	SFC_REG_MEM_ADDR				0x0060
#define	SFC_REG_TRIG					0x0064
#define	SFC_REG_SR						0x0068
#define	SFC_REG_SCR						0x006c
#define	SFC_REG_INTC					0x0070
#define	SFC_REG_FSM						0x0074
#define	SFC_REG_CGE						0x0078
#define SFC_REG_TRAN_CFG1(n)			(0x009c + (n) * 4)
#define	SFC_REG_DR						0x1000

/* bits within the GLB register */
#define GLB_TRAN_DIR_MASK				GENMASK(13, 13)
#define GLB_TRAN_DIR_WRITE				0x1
#define GLB_TRAN_DIR_READ				0x0
#define GLB_THRESHOLD_MASK				GENMASK(12, 7)
#define GLB_OP_MODE_MASK				GENMASK(6, 6)
#define GLB_OP_MODE_DMA					0x1
#define GLB_OP_MODE_SLAVE				0x0
#define GLB_PHASE_NUM_MASK				GENMASK(5, 3)
#define GLB_WP_EN						BIT(2)
#define GLB_BURST_MD_MASK				GENMASK(1, 0)
#define GLB_BURST_MD_INCR32				0x3
#define GLB_BURST_MD_INCR16				0x2
#define GLB_BURST_MD_INCR8				0x1
#define GLB_BURST_MD_INCR4				0x0

/* bits within the DEV_CONF register */
#define DEV_CONF_SMP_DELAY_MASK			GENMASK(20, 16)
#define	DEV_CONF_SMP_DELAY_180DEG		0x4
#define	DEV_CONF_SMP_DELAY_HALF_CYCLE	0x1
#define DEV_CONF_CMD_TYPE_MASK			GENMASK(15, 15)
#define DEV_CONF_CMD_TYPE_16BIT			0x1
#define DEV_CONF_CMD_TYPE_8BIT			0x0
#define DEV_CONF_STA_TYPE_MASK			GENMASK(14, 13)
#define DEV_CONF_THOLD_MASK				GENMASK(12, 11)
#define DEV_CONF_TSETUP_MASK			GENMASK(10, 9)
#define DEV_CONF_TSH_MASK				GENMASK(8, 5)
#define DEV_CONF_CPHA					BIT(4)
#define DEV_CONF_CPOL					BIT(3)
#define DEV_CONF_CE_DL					BIT(2)
#define DEV_CONF_HOLD_DL				BIT(1)
#define DEV_CONF_WP_DL					BIT(0)

/* bits within the TRAN_CONF(n) register */
#define TRAN_CONF_TRAN_MODE_MASK		GENMASK(31, 29)
#define	TRAN_CONF_ADDR_WIDTH_MASK		GENMASK(28, 26)
#define TRAN_CONF_POLL_EN				BIT(25)
#define TRAN_CONF_CMD_EN				BIT(24)
#define TRAN_CONF_PHASE_FORMAT_MASK		GENMASK(23, 23)
#define TRAN_CONF_DMY_BITS_MASK			GENMASK(22, 17)
#define TRAN_CONF_DATA_EN				BIT(16)
#define TRAN_CONF_CMD_MASK				GENMASK(15, 0)

/* bits within the TRIG register */
#define TRIG_FLUSH						BIT(2)
#define TRIG_STOP						BIT(1)
#define TRIG_START						BIT(0)

/* bits within the SR register */
#define SR_FIFO_NUM_MASK				GENMASK(22, 16)
#define	SR_END							BIT(4)
#define SR_TRAN_REQ						BIT(3)
#define SR_RECE_REQ						BIT(2)
#define SR_OVER							BIT(1)
#define SR_UNDER						BIT(0)

/* bits within the SCR register */
#define	SCR_CLR_END						BIT(4)
#define SCR_CLR_TREQ					BIT(3)
#define SCR_CLR_RREQ					BIT(2)
#define SCR_CLR_OVER					BIT(1)
#define SCR_CLR_UNDER					BIT(0)

/* bits within the INTC register */
#define	INTC_MASK_END					BIT(4)
#define INTC_MASK_TREQ					BIT(3)
#define INTC_MASK_RREQ					BIT(2)
#define INTC_MASK_OVER					BIT(1)
#define INTC_MASK_UNDER					BIT(0)

/* bits within the TRAN_CFG1(n) register */
#define TRAN_CFG1_TRAN_MODE_MASK		GENMASK(7, 4)

#define TRAN_MODE_STANDARD				0
#define TRAN_MODE_DUAL_DATA				1
#define TRAN_MODE_DUAL_IO				2
#define TRAN_MODE_DUAL_FULL				3
#define TRAN_MODE_QUAD_DATA				5
#define TRAN_MODE_QUAD_IO				6
#define TRAN_MODE_QUAD_FULL				7
#define TRAN_MODE_OCTAL_DATA			9
#define TRAN_MODE_OCTAL_IO				10
#define TRAN_MODE_OCTAL_FULL			11

#define SFC_TRANSFER_TIMEOUT			1000

/* 64 32-bit words */
#define SFC_FIFO_BYTES					256
/* RREQ comes with this many words in the FIFO, the vendor drains that much per request */
#define SFC_THRESHOLD_WORDS				63

enum ingenic_sfc_version {
	ID_X1000,
	ID_X1600,
	ID_X2000,
};

struct ingenic_soc_info {
	enum ingenic_sfc_version version;

	unsigned int max_bus_width;

	const u32 tran_mode_mask;
};

struct ingenic_sfc {
	const struct ingenic_soc_info *soc_info;

	void __iomem *base;
	struct device *dev;
	struct clk *clk;
	int irq;

	struct completion completion;
};

static irqreturn_t ingenic_sfc_irq_handler(int irq, void *data)
{
	struct ingenic_sfc *sfc = data;

	writel(0x1f, sfc->base + SFC_REG_INTC);

	complete(&sfc->completion);

	return IRQ_HANDLED;
}

static int ingenic_sfc_adjust_op_size(struct spi_mem *mem, struct spi_mem_op *op)
{
	/* PIO through the FIFO */
	if (op->data.nbytes > SFC_FIFO_BYTES)
		op->data.nbytes = SFC_FIFO_BYTES;

	return 0;
}

static bool ingenic_sfc_supports_op(struct spi_mem *mem, const struct spi_mem_op *op)
{
	struct spi_device *spi = mem->spi;
	struct ingenic_sfc *sfc = spi_controller_get_devdata(spi->controller);

	/* The controller only supports Standard SPI mode, Duall mode and Quad mode. */
	if (op->cmd.buswidth > sfc->soc_info->max_bus_width ||
		op->addr.buswidth > sfc->soc_info->max_bus_width ||
		op->dummy.buswidth > sfc->soc_info->max_bus_width ||
		op->data.buswidth > sfc->soc_info->max_bus_width) {
		return false;
}

	/* Max 32 dummy clock cycles supported */
	if (op->dummy.nbytes && (op->dummy.nbytes * 8 / op->dummy.buswidth > 32)){
		return false;
	}

	/*
	 * Data goes through the FIFO in one go, any length up to its size;
	 * adjust_op_size() splits longer transfers.
	 */
	if (op->data.nbytes > SFC_FIFO_BYTES)
		return false;

	/* Max 6 bytes address width supported */
	if (op->addr.nbytes > 6) {
		return false;
	}

	return spi_mem_default_supports_op(mem, op);
}

static void ingenic_sfc_set_transfer_mode(struct ingenic_sfc *sfc, const struct spi_mem_op *op)
{
	int val;

	val = readl(sfc->base +
			(sfc->soc_info->version >= ID_X1600 ? SFC_REG_TRAN_CFG1(0) : SFC_REG_TRAN_CONF(0)));
	val &= ~sfc->soc_info->tran_mode_mask;
	if (op->cmd.buswidth == 8)
		val |= (TRAN_MODE_OCTAL_FULL << (ffs(sfc->soc_info->tran_mode_mask) - 1)) &
				sfc->soc_info->tran_mode_mask;
	else if (op->cmd.buswidth == 4)
		val |= (TRAN_MODE_QUAD_FULL << (ffs(sfc->soc_info->tran_mode_mask) - 1)) &
				sfc->soc_info->tran_mode_mask;
	else if (op->cmd.buswidth == 2)
		val |= (TRAN_MODE_DUAL_FULL << (ffs(sfc->soc_info->tran_mode_mask) - 1)) &
				sfc->soc_info->tran_mode_mask;
	else if (op->addr.buswidth == 8)
		val |= (TRAN_MODE_OCTAL_IO << (ffs(sfc->soc_info->tran_mode_mask) - 1)) &
				sfc->soc_info->tran_mode_mask;
	else if (op->addr.buswidth == 4)
		val |= (TRAN_MODE_QUAD_IO << (ffs(sfc->soc_info->tran_mode_mask) - 1)) &
				sfc->soc_info->tran_mode_mask;
	else if (op->addr.buswidth == 2)
		val |= (TRAN_MODE_DUAL_IO << (ffs(sfc->soc_info->tran_mode_mask) - 1)) &
				sfc->soc_info->tran_mode_mask;
	else if (op->data.buswidth == 8)
		val |= (TRAN_MODE_OCTAL_DATA << (ffs(sfc->soc_info->tran_mode_mask) - 1)) &
				sfc->soc_info->tran_mode_mask;
	else if (op->data.buswidth == 4)
		val |= (TRAN_MODE_QUAD_DATA << (ffs(sfc->soc_info->tran_mode_mask) - 1)) &
				sfc->soc_info->tran_mode_mask;
	else if (op->data.buswidth == 2)
		val |= (TRAN_MODE_DUAL_DATA << (ffs(sfc->soc_info->tran_mode_mask) - 1)) &
				sfc->soc_info->tran_mode_mask;
	else
		val |= (TRAN_MODE_STANDARD << (ffs(sfc->soc_info->tran_mode_mask) - 1)) &
				sfc->soc_info->tran_mode_mask;
	writel(val, sfc->base +
			(sfc->soc_info->version >= ID_X1600 ? SFC_REG_TRAN_CFG1(0) : SFC_REG_TRAN_CONF(0)));
}

/*
 * memcpy_{to,from}io doesn't gurantee 32b accesses - which we require for the
 * SFC_REG_DR register -so use __io{read,write}32_copy when possible. For
 * trailing bytes, copy them byte-by-byte from the SFC_REG_DR register, as we
 * can't clobber outside the source/dest buffer.
 *
 * For efficient data read/write, we try to put any start 32b unaligned data
 * into a separate transaction in ingenic_sfc_adjust_op_size().
 */
static void ingenic_sfc_read_rxfifo(struct ingenic_sfc *sfc, u8 *to, unsigned int len)
{
	void __iomem *from;
	int i;

	from = sfc->base + SFC_REG_DR;

	for (i = 0; i < DIV_ROUND_UP(len, 4); i++) {
		u32 val = __raw_readl(from);
		int j;

		for (j = 0; j < 4 && (j + (i * 4) < len); to++, val >>= 8, j++)
			*to = (u8)val;
	}
}

static void ingenic_sfc_write_txfifo(struct ingenic_sfc *sfc, const u8 *from, unsigned int len)
{
	void __iomem *to;
	int i;

	to = sfc->base + SFC_REG_DR;

	for (i = 0; i < DIV_ROUND_UP(len, 4); i++) {
		u32 val = 0;
		int j;

		for (j = 0; j < 4 && (j + (i * 4) < len); from++, j++)
			val |= *from << j * 8;
		__raw_writel(val, to);
	}
}

/*
 * A read request means the FIFO holds the threshold's worth of words, not the
 * rest of the transfer: drain that much per request and the remainder once
 * the transfer has ended, as the vendor driver does.
 */
static int ingenic_sfc_pio_read(struct ingenic_sfc *sfc, u8 *to, unsigned int len)
{
	unsigned int words = DIV_ROUND_UP(len, 4);
	ktime_t timeout = ktime_add_ms(ktime_get(), SFC_TRANSFER_TIMEOUT);
	u32 sr;
	int ret;

	for (;;) {
		unsigned int n, bytes;

		sr = readl(sfc->base + SFC_REG_SR);
		if (sr & (SR_OVER | SR_UNDER)) {
			dev_err(sfc->dev, "SFC read error: SR=0x%08x\n", sr);
			return -EIO;
		}

		if (sr & SR_RECE_REQ) {
			n = min_t(unsigned int, words, SFC_THRESHOLD_WORDS);
			bytes = min(n * 4, len);
			ingenic_sfc_read_rxfifo(sfc, to, bytes);
			to += bytes;
			len -= bytes;
			words -= n;
			writel(SCR_CLR_RREQ, sfc->base + SFC_REG_SCR);
			continue;
		}

		/* the rest is all in the FIFO: below the threshold no request may come */
		if (sr & SR_END || FIELD_GET(SR_FIFO_NUM_MASK, sr) >= words) {
			ingenic_sfc_read_rxfifo(sfc, to, len);
			ret = readl_poll_timeout(sfc->base + SFC_REG_SR, sr, sr & SR_END,
						 10, SFC_TRANSFER_TIMEOUT * USEC_PER_MSEC);
			if (ret)
				dev_err(sfc->dev, "SFC read END timeout: SR=0x%08x\n", sr);
			return ret;
		}

		if (ktime_after(ktime_get(), timeout)) {
			dev_err(sfc->dev, "SFC read timeout: SR=0x%08x, %u words left\n",
				sr, words);
			return -ETIMEDOUT;
		}
		cpu_relax();
	}
}

static int ingenic_sfc_exec_op_pio(struct ingenic_sfc *sfc, const struct spi_mem_op *op)
{
	bool poll_rx = op->data.dir == SPI_MEM_DATA_IN && op->data.nbytes;
	int ret, val;

	val = readl(sfc->base + SFC_REG_GLB);
	u32p_replace_bits(&val,
			op->data.dir == SPI_MEM_DATA_IN ? GLB_TRAN_DIR_READ : GLB_TRAN_DIR_WRITE,
			GLB_TRAN_DIR_MASK);
	u32p_replace_bits(&val, GLB_OP_MODE_SLAVE, GLB_OP_MODE_MASK);
	writel(val, sfc->base + SFC_REG_GLB);

	val = TRAN_CONF_CMD_EN | op->cmd.opcode;

	if (op->addr.nbytes > 0) {
		val |= FIELD_PREP(TRAN_CONF_ADDR_WIDTH_MASK, op->addr.nbytes);

		writel(op->addr.val & 0xffffffff, sfc->base + SFC_REG_DEV_ADDR(0));
		writel(op->addr.val >> 32, sfc->base + SFC_REG_DEV_ADDR_PLUS(0));
	}

	if (op->dummy.nbytes > 0)
		val |= FIELD_PREP(TRAN_CONF_DMY_BITS_MASK, op->dummy.nbytes * 8 / op->dummy.buswidth);

	if (op->data.nbytes > 0)
		val |= TRAN_CONF_DATA_EN;

	writel(val, sfc->base + SFC_REG_TRAN_CONF(0));
	writel(op->data.nbytes, sfc->base + SFC_REG_TRAN_LEN);

	ingenic_sfc_set_transfer_mode(sfc, op);

	writel(0x1f, sfc->base + SFC_REG_SCR);
	/* a read is polled, its requests come many times in a transfer */
	if (poll_rx)
		writel(0x1f, sfc->base + SFC_REG_INTC);
	else
		writel(~(INTC_MASK_END | INTC_MASK_RREQ), sfc->base + SFC_REG_INTC);

	writel(0, sfc->base + SFC_REG_MEM_ADDR);

	writel(TRIG_FLUSH, sfc->base + SFC_REG_TRIG);
	writel(TRIG_START, sfc->base + SFC_REG_TRIG);

	if (poll_rx) {
		ret = ingenic_sfc_pio_read(sfc, op->data.buf.in, op->data.nbytes);
		writel(0x1f, sfc->base + SFC_REG_SCR);
		writel(TRIG_STOP, sfc->base + SFC_REG_TRIG);
		return ret;
	}

	if (op->data.dir == SPI_MEM_DATA_OUT)
		ingenic_sfc_write_txfifo(sfc, op->data.buf.out, op->data.nbytes);

	ret = wait_for_completion_timeout(&sfc->completion, msecs_to_jiffies(SFC_TRANSFER_TIMEOUT));
	if (!ret) {
		writel(0x1f, sfc->base + SFC_REG_INTC);
		writel(0x1f, sfc->base + SFC_REG_SCR);
		dev_err(sfc->dev, "SFC PIO timeout: SR=0x%08x GLB=0x%08x CONF=0x%08x\n",
			readl(sfc->base + SFC_REG_SR),
			readl(sfc->base + SFC_REG_GLB),
			readl(sfc->base + SFC_REG_TRAN_CONF(0)));
		return -ETIMEDOUT;
	}

	writel(INTC_MASK_END | INTC_MASK_RREQ, sfc->base + SFC_REG_SCR);
	writel(TRIG_STOP, sfc->base + SFC_REG_TRIG);

	return 0;
}

static int ingenic_sfc_exec_op(struct spi_mem *mem, const struct spi_mem_op *op)
{
	struct spi_device *spi = mem->spi;
	struct ingenic_sfc *sfc = spi_controller_get_devdata(spi->controller);

	reinit_completion(&sfc->completion);

	return ingenic_sfc_exec_op_pio(sfc, op);
}

static int ingenic_sfc_poll_status(struct spi_mem *mem, const struct spi_mem_op *op,
			u16 mask, u16 match, unsigned long initial_delay_us,
			unsigned long polling_delay_us, unsigned long timeout_ms)
{
	struct spi_device *spi = mem->spi;
	struct ingenic_sfc *sfc = spi_controller_get_devdata(spi->controller);
	int ret, val;

	val = readl(sfc->base + SFC_REG_GLB);
	u32p_replace_bits(&val, GLB_TRAN_DIR_READ, GLB_TRAN_DIR_MASK);
	u32p_replace_bits(&val, GLB_OP_MODE_SLAVE, GLB_OP_MODE_MASK);
	writel(val, sfc->base + SFC_REG_GLB);

	writel(match, sfc->base + SFC_REG_DEV_STA_EXP);
	writel(mask, sfc->base + SFC_REG_DEV_STA_MSK);

	val = TRAN_CONF_POLL_EN | TRAN_CONF_CMD_EN | op->cmd.opcode;

	if (op->addr.nbytes > 0) {
		val |= FIELD_PREP(TRAN_CONF_ADDR_WIDTH_MASK, op->addr.nbytes);

		writel(op->addr.val & 0xffffffff, sfc->base + SFC_REG_DEV_ADDR(0));
		writel(op->addr.val >> 32, sfc->base + SFC_REG_DEV_ADDR_PLUS(0));
	}

	if (op->dummy.nbytes > 0)
		val |= FIELD_PREP(TRAN_CONF_DMY_BITS_MASK, op->dummy.nbytes * 8 / op->dummy.buswidth);

	if (op->data.nbytes > 0)
		val |= TRAN_CONF_DATA_EN;

	writel(val, sfc->base + SFC_REG_TRAN_CONF(0));
	writel(op->data.nbytes, sfc->base + SFC_REG_TRAN_LEN);

	ingenic_sfc_set_transfer_mode(sfc, op);

	writel(0x1f, sfc->base + SFC_REG_SCR);
	reinit_completion(&sfc->completion);
	writel(~INTC_MASK_END, sfc->base + SFC_REG_INTC);

	writel(0, sfc->base + SFC_REG_MEM_ADDR);

	writel(TRIG_START, sfc->base + SFC_REG_TRIG);

	ret = wait_for_completion_timeout(&sfc->completion, msecs_to_jiffies(SFC_TRANSFER_TIMEOUT));
	if (!ret) {
		writel(0x1f, sfc->base + SFC_REG_INTC);
		writel(0x1f, sfc->base + SFC_REG_SCR);
		dev_err(sfc->dev, "line:%d Timeout for ACK from SFC device\n", __LINE__);
		return -ETIMEDOUT;
	}

	writel(SCR_CLR_END, sfc->base + SFC_REG_SCR);
	writel(TRIG_STOP, sfc->base + SFC_REG_TRIG);

	return 0;
}

static const struct spi_controller_mem_ops ingenic_sfc_mem_ops = {
	.adjust_op_size = ingenic_sfc_adjust_op_size,
	.supports_op = ingenic_sfc_supports_op,
	.exec_op = ingenic_sfc_exec_op,
	.poll_status = ingenic_sfc_poll_status,
};

static int ingenic_sfc_setup(struct spi_device *spi)
{
	struct ingenic_sfc *sfc = spi_controller_get_devdata(spi->controller);
	unsigned long rate;
	int val;

	if (!spi->max_speed_hz)
		return -EINVAL;

	/* Skip clk_set_rate — use U-Boot's SFC clock config.
	 * The Ingenic CGU SSICDR CE/BUSY bit handling can break
	 * the working clock configuration set by the bootloader.
	 */

	writel(TRIG_STOP, sfc->base + SFC_REG_TRIG);

	/* X1000 need set to 0, but X2000 can be set to 1 */
	writel(0, sfc->base + SFC_REG_CGE);

	val = readl(sfc->base + SFC_REG_GLB);
	u32p_replace_bits(&val, SFC_THRESHOLD_WORDS, GLB_THRESHOLD_MASK);
	u32p_replace_bits(&val, GLB_BURST_MD_INCR8, GLB_BURST_MD_MASK);
	writel(val, sfc->base + SFC_REG_GLB);

	val = readl(sfc->base + SFC_REG_DEV_CONF);

	/* cpha bit:0 , cpol bit:0 */
	val &= ~(DEV_CONF_CPHA | DEV_CONF_CPOL);
	val |= spi->mode & SPI_CPHA ? DEV_CONF_CPHA : 0;
	val |= spi->mode & SPI_CPOL ? DEV_CONF_CPOL : 0;

	/* ce_dl bit:1, hold bit:1, wp bit:1 */
	val |= (DEV_CONF_CE_DL | DEV_CONF_HOLD_DL | DEV_CONF_WP_DL);

	writel(val, sfc->base + SFC_REG_DEV_CONF);

	val = readl(sfc->base + SFC_REG_GLB);
	u32p_replace_bits(&val, GLB_OP_MODE_SLAVE, GLB_OP_MODE_MASK);
	writel(val, sfc->base + SFC_REG_GLB);

	rate = clk_get_rate(sfc->clk);
	val = readl(sfc->base + SFC_REG_DEV_CONF);
	if (sfc->soc_info->version >= ID_X1600 && rate >= 200000000)
		u32p_replace_bits(&val, DEV_CONF_SMP_DELAY_180DEG, DEV_CONF_SMP_DELAY_MASK);
	else if (sfc->soc_info->version == ID_X1000 && rate >= 100000000)
		u32p_replace_bits(&val, DEV_CONF_SMP_DELAY_HALF_CYCLE, DEV_CONF_SMP_DELAY_MASK);
	writel(val, sfc->base + SFC_REG_DEV_CONF);

	return 0;
}

static int ingenic_sfc_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct ingenic_sfc *sfc;
	struct spi_controller *ctlr;
	int ret;

	ctlr = devm_spi_alloc_host(dev, sizeof(*sfc));
	if (!ctlr)
		return -ENOMEM;

	sfc = spi_controller_get_devdata(ctlr);
	sfc->dev = dev;
	init_completion(&sfc->completion);

	sfc->soc_info = device_get_match_data(dev);
	if (!sfc->soc_info)
		return dev_err_probe(dev, -ENODEV, "no match data\n");

	sfc->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(sfc->base))
		return PTR_ERR(sfc->base);

	sfc->clk = devm_clk_get_enabled(dev, "sfc");
	if (IS_ERR(sfc->clk))
		return dev_err_probe(dev, PTR_ERR(sfc->clk), "failed to enable the clock\n");

	sfc->irq = platform_get_irq(pdev, 0);
	if (sfc->irq < 0)
		return sfc->irq;

	platform_set_drvdata(pdev, sfc);

	/* The boot loader may leave transfer interrupts pending. */
	writel(0x1f, sfc->base + SFC_REG_SCR);
	writel(0x1f, sfc->base + SFC_REG_INTC);

	ret = devm_request_irq(dev, sfc->irq, ingenic_sfc_irq_handler, 0,
			       dev_name(dev), sfc);
	if (ret)
		return dev_err_probe(dev, ret, "failed to request IRQ %d\n", sfc->irq);

	ctlr->bus_num = -1;
	ctlr->num_chipselect = 1;
	ctlr->mem_ops = &ingenic_sfc_mem_ops;
	ctlr->dev.of_node = dev->of_node;
	ctlr->setup = ingenic_sfc_setup;
	ctlr->mode_bits = SPI_CPHA | SPI_CPOL |
			SPI_RX_DUAL | SPI_RX_QUAD | SPI_TX_DUAL | SPI_TX_QUAD;
	if (sfc->soc_info->max_bus_width >= 8)
		ctlr->mode_bits |= SPI_RX_OCTAL | SPI_TX_OCTAL;

	return devm_spi_register_controller(dev, ctlr);
}

static const struct ingenic_soc_info x1000_soc_info = {
	.version = ID_X1000,

	.max_bus_width = 4,

	.tran_mode_mask = TRAN_CONF_TRAN_MODE_MASK,
};

/* The transfer mode field of the X1600 and later, four data lines */
static const struct ingenic_soc_info t32_soc_info = {
	.version = ID_X2000,

	.max_bus_width = 4,

	.tran_mode_mask = TRAN_CFG1_TRAN_MODE_MASK,
};

static const struct of_device_id ingenic_sfc_of_matches[] = {
	{ .compatible = "ingenic,x1000-sfc", .data = &x1000_soc_info },
	{ .compatible = "ingenic,t32-sfc", .data = &t32_soc_info },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, ingenic_sfc_of_matches);

static struct platform_driver ingenic_sfc_driver = {
	.driver = {
		.name = "ingenic-sfc",
		.of_match_table = ingenic_sfc_of_matches,
	},
	.probe = ingenic_sfc_probe,
};
module_platform_driver(ingenic_sfc_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("周琰杰 (Zhou Yanjie) <zhouyanjie@wanyeetech.com>");
MODULE_DESCRIPTION("Ingenic SoCs SPI Flash Controller Driver");
