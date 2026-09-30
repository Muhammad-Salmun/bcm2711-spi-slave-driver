// SPDX-License-Identifier: GPL-2.0-only
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/idr.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/debugfs.h>
#include <linux/dma-mapping.h>
#include <linux/dmaengine.h>
#include <linux/miscdevice.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/overflow.h>
#include <linux/platform_device.h>
#include <linux/poll.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/uaccess.h>
#include <linux/wait.h>

#define BSC_DR 0x00
#define BSC_RSR 0x04
#define BSC_SLV 0x08
#define BSC_CR 0x0C
#define BSC_FR 0x10
#define BSC_IFLS 0x14
#define BSC_IMSC 0x18
#define BSC_RIS 0x1C
#define BSC_MIS 0x20
#define BSC_ICR 0x24
#define BSC_DMACR 0x28
#define BSC_DEBUG_SPI 0x3C
#define BSC_CR_ENABLED 0x303
#define BSC_IFLS_TX_MASK GENMASK(2, 0)
#define BSC_IFLS_TX_SEVEN_EIGHTHS 4
#define FR_RXFE (1 << 1)
#define FR_TXFF (1 << 2)
#define FR_TXFE (1 << 4)
#define RX_RING_SIZE 65536

static unsigned int poll_interval_us = 20;
module_param(poll_interval_us, uint, 0644);
MODULE_PARM_DESC(poll_interval_us,
		 "BSC polling interval in microseconds (minimum 1)");

static unsigned int tx_pio_limit;
module_param(tx_pio_limit, uint, 0644);
MODULE_PARM_DESC(tx_pio_limit,
		 "maximum response bytes primed by CPU before TX DMA (default 0)");

static unsigned long dma_bus_address;
module_param(dma_bus_address, ulong, 0444);
MODULE_PARM_DESC(dma_bus_address,
		 "override BSC_DR DMA bus address (diagnostic only; zero uses DT)");

static DEFINE_IDA(bcm_spi_slave_ida);

struct bcm_spi_slave {
	struct device *dev;
	void __iomem *bsc;
	struct dma_chan *tx_dma;
	u32 *tx_dma_buf;
	dma_addr_t tx_dma_handle;
	size_t tx_dma_size;
	dma_addr_t bsc_dr_dma_addr;
	struct mutex tx_dma_mutex;
	struct task_struct *monitor_task;
	struct dentry *debugfs_dir;
	struct miscdevice miscdev;
	int id;
	atomic_t open_count;
	bool removing;
	u8 rx_ring[RX_RING_SIZE];
	unsigned int rx_head;
	unsigned int rx_tail;
	unsigned int rx_count;
	unsigned int tx_writers;
	unsigned long long rx_total;
	unsigned long long rx_overruns;
	unsigned long long rx_read_calls;
	unsigned long long rx_read_bytes;
	unsigned long long rx_empty_waits;
	unsigned long long rx_fifo_drain_calls;
	unsigned long long rx_fifo_empty_polls;
	unsigned long long rx_fifo_not_empty_observed;
	unsigned long long rx_fifo_bytes_drained;
	unsigned long long rx_fifo_bytes_discarded;
	unsigned long long rx_debug_prints;
	u32 rx_last_fr;
	u32 rx_last_dr;
	unsigned long long tx_write_calls;
	unsigned long long tx_write_requested;
	unsigned long long tx_write_completed;
	unsigned long long tx_write_short;
	unsigned long long tx_write_errors;
	u32 tx_fifo_last_fr;
	unsigned long long tx_clear_events;
	spinlock_t rx_lock;
	spinlock_t tx_lock;
	wait_queue_head_t rx_wait;
	wait_queue_head_t tx_wait;
	wait_queue_head_t remove_wait;
};

static void bcm_spi_free_id(void *data)
{
	struct bcm_spi_slave *spi = data;

	ida_free(&bcm_spi_slave_ida, spi->id);
}

static void tx_path_clear(struct bcm_spi_slave *spi);

static void cm4_bsc_discard_rx_fifo(struct bcm_spi_slave *spi)
{
	int discarded = 0;
	u32 fr;

	if (!spi->bsc)
	return;

	fr = readl(spi->bsc + BSC_FR);
	while (!(fr & FR_RXFE) && discarded < 256) {
	readl(spi->bsc + BSC_DR);
	discarded++;
	fr = readl(spi->bsc + BSC_FR);
	}

	if (discarded > 0) {
	unsigned long flags;

	spin_lock_irqsave(&spi->rx_lock, flags);
	spi->rx_fifo_bytes_discarded += discarded;
	spi->rx_last_fr = fr;
	spin_unlock_irqrestore(&spi->rx_lock, flags);

	pr_info("SPI Slave: discarded %d stale RX FIFO bytes\n", discarded);
	}
}

static void cm4_spi_reset_stats(struct bcm_spi_slave *spi)
{
	unsigned long flags;

	spin_lock_irqsave(&spi->rx_lock, flags);
	spi->rx_total = 0;
	spi->rx_overruns = 0;
	spi->rx_read_calls = 0;
	spi->rx_read_bytes = 0;
	spi->rx_empty_waits = 0;
	spi->rx_fifo_drain_calls = 0;
	spi->rx_fifo_empty_polls = 0;
	spi->rx_fifo_not_empty_observed = 0;
	spi->rx_fifo_bytes_drained = 0;
	spi->rx_fifo_bytes_discarded = 0;
	spi->rx_debug_prints = 0;
	spi->rx_last_fr = 0;
	spi->rx_last_dr = 0;
	spin_unlock_irqrestore(&spi->rx_lock, flags);

	spin_lock_irqsave(&spi->tx_lock, flags);
	spi->tx_write_calls = 0;
	spi->tx_write_requested = 0;
	spi->tx_write_completed = 0;
	spi->tx_write_short = 0;
	spi->tx_write_errors = 0;
	spi->tx_fifo_last_fr = 0;
	spi->tx_clear_events = 0;
	spin_unlock_irqrestore(&spi->tx_lock, flags);
}

static int cm4_spi_stats_show(struct seq_file *m, void *v)
{
	struct bcm_spi_slave *spi = m->private;
	unsigned long flags;
	u32 fr;
	unsigned int rx_head_snapshot;
	unsigned int rx_tail_snapshot;
	unsigned int rx_count_snapshot;
	unsigned long long rx_total_snapshot;
	unsigned long long rx_overruns_snapshot;
	unsigned long long rx_read_calls_snapshot;
	unsigned long long rx_read_bytes_snapshot;
	unsigned long long rx_empty_waits_snapshot;
	unsigned long long rx_fifo_drain_calls_snapshot;
	unsigned long long rx_fifo_empty_polls_snapshot;
	unsigned long long rx_fifo_not_empty_observed_snapshot;
	unsigned long long rx_fifo_bytes_drained_snapshot;
	unsigned long long rx_fifo_bytes_discarded_snapshot;
	unsigned long long rx_debug_prints_snapshot;
	u32 rx_last_fr_snapshot;
	u32 rx_last_dr_snapshot;
	unsigned int tx_writers_snapshot;
	unsigned long long tx_write_calls_snapshot;
	unsigned long long tx_write_requested_snapshot;
	unsigned long long tx_write_completed_snapshot;
	unsigned long long tx_write_short_snapshot;
	unsigned long long tx_write_errors_snapshot;
	u32 tx_fifo_last_fr_snapshot;
	unsigned long long tx_clear_events_snapshot;
	size_t tx_dma_size_snapshot;

	(void)v;

	fr = readl(spi->bsc + BSC_FR);

	spin_lock_irqsave(&spi->rx_lock, flags);
	rx_head_snapshot = spi->rx_head;
	rx_tail_snapshot = spi->rx_tail;
	rx_count_snapshot = spi->rx_count;
	rx_total_snapshot = spi->rx_total;
	rx_overruns_snapshot = spi->rx_overruns;
	rx_read_calls_snapshot = spi->rx_read_calls;
	rx_read_bytes_snapshot = spi->rx_read_bytes;
	rx_empty_waits_snapshot = spi->rx_empty_waits;
	rx_fifo_drain_calls_snapshot = spi->rx_fifo_drain_calls;
	rx_fifo_empty_polls_snapshot = spi->rx_fifo_empty_polls;
	rx_fifo_not_empty_observed_snapshot = spi->rx_fifo_not_empty_observed;
	rx_fifo_bytes_drained_snapshot = spi->rx_fifo_bytes_drained;
	rx_fifo_bytes_discarded_snapshot = spi->rx_fifo_bytes_discarded;
	rx_debug_prints_snapshot = spi->rx_debug_prints;
	rx_last_fr_snapshot = spi->rx_last_fr;
	rx_last_dr_snapshot = spi->rx_last_dr;
	spin_unlock_irqrestore(&spi->rx_lock, flags);

	spin_lock_irqsave(&spi->tx_lock, flags);
	tx_writers_snapshot = spi->tx_writers;
	tx_write_calls_snapshot = spi->tx_write_calls;
	tx_write_requested_snapshot = spi->tx_write_requested;
	tx_write_completed_snapshot = spi->tx_write_completed;
	tx_write_short_snapshot = spi->tx_write_short;
	tx_write_errors_snapshot = spi->tx_write_errors;
	tx_fifo_last_fr_snapshot = spi->tx_fifo_last_fr;
	tx_clear_events_snapshot = spi->tx_clear_events;
	spin_unlock_irqrestore(&spi->tx_lock, flags);
	tx_dma_size_snapshot = READ_ONCE(spi->tx_dma_size);

	seq_printf(m, "fr=0x%08X\n", fr);
	seq_printf(m, "rx_ring_size=%u\n", RX_RING_SIZE);
	seq_printf(m, "rx_head=%u\n", rx_head_snapshot);
	seq_printf(m, "rx_tail=%u\n", rx_tail_snapshot);
	seq_printf(m, "rx_count=%u\n", rx_count_snapshot);
	seq_printf(m, "rx_total=%llu\n", rx_total_snapshot);
	seq_printf(m, "rx_overruns=%llu\n", rx_overruns_snapshot);
	seq_printf(m, "rx_read_calls=%llu\n", rx_read_calls_snapshot);
	seq_printf(m, "rx_read_bytes=%llu\n", rx_read_bytes_snapshot);
	seq_printf(m, "rx_empty_waits=%llu\n", rx_empty_waits_snapshot);
	seq_printf(m, "rx_fifo_drain_calls=%llu\n",
		   rx_fifo_drain_calls_snapshot);
	seq_printf(m, "rx_fifo_empty_polls=%llu\n",
		   rx_fifo_empty_polls_snapshot);
	seq_printf(m, "rx_fifo_not_empty_observed=%llu\n",
		   rx_fifo_not_empty_observed_snapshot);
	seq_printf(m, "rx_fifo_bytes_drained=%llu\n",
		   rx_fifo_bytes_drained_snapshot);
	seq_printf(m, "rx_fifo_bytes_discarded=%llu\n",
		   rx_fifo_bytes_discarded_snapshot);
	seq_printf(m, "rx_debug_prints=%llu\n", rx_debug_prints_snapshot);
	seq_printf(m, "rx_last_fr=0x%08X\n", rx_last_fr_snapshot);
	seq_printf(m, "rx_last_dr=0x%08X\n", rx_last_dr_snapshot);
	seq_printf(m, "tx_writers=%u\n", tx_writers_snapshot);
	seq_printf(m, "tx_write_calls=%llu\n", tx_write_calls_snapshot);
	seq_printf(m, "tx_write_requested=%llu\n", tx_write_requested_snapshot);
	seq_printf(m, "tx_write_completed=%llu\n", tx_write_completed_snapshot);
	seq_printf(m, "tx_write_short=%llu\n", tx_write_short_snapshot);
	seq_printf(m, "tx_write_errors=%llu\n", tx_write_errors_snapshot);
	seq_printf(m, "tx_fifo_last_fr=0x%08X\n", tx_fifo_last_fr_snapshot);
	seq_printf(m, "tx_clear_events=%llu\n", tx_clear_events_snapshot);
	seq_printf(m, "tx_dma_size=%zu\n", tx_dma_size_snapshot);

	return 0;
}

static int cm4_spi_stats_open(struct inode *inode, struct file *file)
{
	return single_open(file, cm4_spi_stats_show, inode->i_private);
}

static const struct file_operations cm4_spi_stats_fops = {
	.owner = THIS_MODULE,
	.open = cm4_spi_stats_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static ssize_t cm4_spi_reset_write(
	struct file *file,
	const char __user *buf,
	size_t count,
	loff_t *ppos)
{
	struct bcm_spi_slave *spi = file->private_data;

	(void)buf;
	(void)ppos;

	cm4_spi_reset_stats(spi);
	return count;
}

static const struct file_operations cm4_spi_reset_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = cm4_spi_reset_write,
	.llseek = noop_llseek,
};

static int cm4_spi_regs_show(struct seq_file *m, void *v)
{
	struct bcm_spi_slave *spi = m->private;

	(void)v;

	/* BSC_DR is intentionally omitted: reading it consumes RX FIFO data. */
	seq_printf(m, "RSR=0x%08X\n", readl(spi->bsc + BSC_RSR));
	seq_printf(m, "SLV=0x%08X\n", readl(spi->bsc + BSC_SLV));
	seq_printf(m, "CR=0x%08X\n", readl(spi->bsc + BSC_CR));
	seq_printf(m, "FR=0x%08X\n", readl(spi->bsc + BSC_FR));
	seq_printf(m, "IFLS=0x%08X\n", readl(spi->bsc + BSC_IFLS));
	seq_printf(m, "IMSC=0x%08X\n", readl(spi->bsc + BSC_IMSC));
	seq_printf(m, "RIS=0x%08X\n", readl(spi->bsc + BSC_RIS));
	seq_printf(m, "MIS=0x%08X\n", readl(spi->bsc + BSC_MIS));
	seq_printf(m, "ICR=0x%08X\n", readl(spi->bsc + BSC_ICR));
	seq_printf(m, "DMACR=0x%08X\n", readl(spi->bsc + BSC_DMACR));
	seq_printf(m, "DEBUG_SPI=0x%08X\n",
		   readl(spi->bsc + BSC_DEBUG_SPI));

	return 0;
}

static int cm4_spi_regs_open(struct inode *inode, struct file *file)
{
	return single_open(file, cm4_spi_regs_show, inode->i_private);
}

static const struct file_operations cm4_spi_regs_fops = {
	.owner = THIS_MODULE,
	.open = cm4_spi_regs_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static void tx_dma_buffer_free(struct bcm_spi_slave *spi)
{
	if (!spi->tx_dma_buf)
		return;

	dma_free_coherent(spi->dev, spi->tx_dma_size, spi->tx_dma_buf,
			  spi->tx_dma_handle);
	spi->tx_dma_buf = NULL;
	spi->tx_dma_handle = 0;
	WRITE_ONCE(spi->tx_dma_size, 0);
}

static ssize_t cm4_spi_clear_tx_write(
	struct file *file,
	const char __user *buf,
	size_t count,
	loff_t *ppos)
{
	struct bcm_spi_slave *spi = file->private_data;

	(void)buf;
	(void)ppos;

	mutex_lock(&spi->tx_dma_mutex);
	dmaengine_terminate_sync(spi->tx_dma);
	tx_dma_buffer_free(spi);
	tx_path_clear(spi);
	mutex_unlock(&spi->tx_dma_mutex);
	wake_up_interruptible(&spi->tx_wait);
	return count;
}

static const struct file_operations cm4_spi_clear_tx_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = cm4_spi_clear_tx_write,
	.llseek = noop_llseek,
};

static void tx_path_clear(struct bcm_spi_slave *spi)
{
	unsigned long flags;

	spin_lock_irqsave(&spi->tx_lock, flags);
	spi->tx_clear_events++;

	if (spi->bsc) {
	writel(0x00000000, spi->bsc + BSC_CR);
	udelay(10);
	writel(0x00000000, spi->bsc + BSC_RSR);
	writel(BSC_CR_ENABLED, spi->bsc + BSC_CR);
	writel((readl(spi->bsc + BSC_IFLS) & ~BSC_IFLS_TX_MASK) |
	       BSC_IFLS_TX_SEVEN_EIGHTHS, spi->bsc + BSC_IFLS);
	writel(BIT(1), spi->bsc + BSC_DMACR);
	spi->tx_fifo_last_fr = readl(spi->bsc + BSC_FR);
	}

	spin_unlock_irqrestore(&spi->tx_lock, flags);
	pr_info("SPI Slave: TX path cleared\n");
}

static void rx_ring_push(struct bcm_spi_slave *spi, u8 rx)
{
	unsigned long flags;

	spin_lock_irqsave(&spi->rx_lock, flags);

	if (spi->rx_count == RX_RING_SIZE) {
	spi->rx_tail = (spi->rx_tail + 1) % RX_RING_SIZE;
	spi->rx_overruns++;
	} else {
	spi->rx_count++;
	}

	spi->rx_ring[spi->rx_head] = rx;
	spi->rx_head = (spi->rx_head + 1) % RX_RING_SIZE;
	spi->rx_total++;

	spin_unlock_irqrestore(&spi->rx_lock, flags);
	wake_up_interruptible(&spi->rx_wait);
}

static void cm4_bsc_drain_rx_fifo(struct bcm_spi_slave *spi)
{
	int drained = 0;
	u32 fr;

	spin_lock(&spi->rx_lock);
	spi->rx_fifo_drain_calls++;
	spin_unlock(&spi->rx_lock);

	fr = readl(spi->bsc + BSC_FR);
	if (fr & FR_RXFE) {
	spin_lock(&spi->rx_lock);
	spi->rx_fifo_empty_polls++;
	spi->rx_last_fr = fr;
	spin_unlock(&spi->rx_lock);
	return;
	}

	do {
	u32 dr = readl(spi->bsc + BSC_DR);
	u8 rx = dr & 0xFF;
	int do_debug_print = 0;

	spin_lock(&spi->rx_lock);
	spi->rx_fifo_not_empty_observed++;
	spi->rx_fifo_bytes_drained++;
	spi->rx_last_fr = fr;
	spi->rx_last_dr = dr;
	if (spi->rx_debug_prints < 16) {
		spi->rx_debug_prints++;
		do_debug_print = 1;
	}
	spin_unlock(&spi->rx_lock);

	if (do_debug_print) {
		pr_info("SPI Slave RX byte=0x%02X DR=0x%08X FR=0x%08X\n",
			rx,
			dr,
			fr);
	}

	rx_ring_push(spi, rx);
	drained++;

	if (drained >= 64)
		break;

	fr = readl(spi->bsc + BSC_FR);
	} while (!(fr & FR_RXFE));
}

static int rx_ring_pop(struct bcm_spi_slave *spi, u8 *rx)
{
	unsigned long flags;
	int ret = 0;

	spin_lock_irqsave(&spi->rx_lock, flags);

	if (spi->rx_count > 0) {
	*rx = spi->rx_ring[spi->rx_tail];
	spi->rx_tail = (spi->rx_tail + 1) % RX_RING_SIZE;
	spi->rx_count--;
	ret = 1;
	}

	spin_unlock_irqrestore(&spi->rx_lock, flags);

	return ret;
}

static ssize_t cm4_spi_slave_read(
	struct file *file,
	char __user *buf,
	size_t count,
	loff_t *ppos)
{
	struct bcm_spi_slave *spi = file->private_data;
	unsigned long flags;
	size_t copied = 0;
	u8 rx;

	(void)ppos;

	if (count == 0)
	return 0;
	if (READ_ONCE(spi->removing))
	return -ENODEV;

	spin_lock_irqsave(&spi->rx_lock, flags);
	spi->rx_read_calls++;
	spin_unlock_irqrestore(&spi->rx_lock, flags);

	while (!rx_ring_pop(spi, &rx)) {
	if (file->f_flags & O_NONBLOCK)
		return -EAGAIN;

	spin_lock_irqsave(&spi->rx_lock, flags);
	spi->rx_empty_waits++;
	spin_unlock_irqrestore(&spi->rx_lock, flags);
	if (wait_event_interruptible(spi->rx_wait,
		READ_ONCE(spi->rx_count) > 0 || READ_ONCE(spi->removing)))
		return -ERESTARTSYS;
	if (READ_ONCE(spi->removing))
		return -ENODEV;
	}

	do {
	if (put_user(rx, buf + copied))
		return copied ? copied : -EFAULT;

	copied++;
	} while (copied < count && rx_ring_pop(spi, &rx));

	spin_lock_irqsave(&spi->rx_lock, flags);
	spi->rx_read_bytes += copied;
	spin_unlock_irqrestore(&spi->rx_lock, flags);
	return copied;
}

static ssize_t cm4_spi_slave_write(
	struct file *file,
	const char __user *buf,
	size_t count,
	loff_t *ppos)
{
	struct bcm_spi_slave *spi = file->private_data;
	struct dma_async_tx_descriptor *desc;
	dma_cookie_t cookie;
	unsigned long flags;
	u32 fr;
	u8 *bytes;
	u32 *new_dma_buf;
	dma_addr_t new_dma_handle;
	size_t new_dma_size;
	size_t i;
	size_t pio_bytes;
	size_t dma_bytes;
	int ret;

	(void)ppos;
	if (count == 0)
	return 0;
	if (READ_ONCE(spi->removing))
	return -ENODEV;
	if (check_mul_overflow(count, sizeof(*new_dma_buf), &new_dma_size))
	return -EOVERFLOW;

	spin_lock_irqsave(&spi->tx_lock, flags);
	spi->tx_write_calls++;
	spi->tx_write_requested += count;
	spin_unlock_irqrestore(&spi->tx_lock, flags);

	bytes = memdup_user(buf, count);
	if (IS_ERR(bytes)) {
	ret = PTR_ERR(bytes);
	goto record_error;
	}

	new_dma_buf = dma_alloc_coherent(spi->dev, new_dma_size,
					 &new_dma_handle, GFP_KERNEL);
	if (!new_dma_buf) {
		ret = -ENOMEM;
		kfree(bytes);
		goto record_error;
	}

	if (file->f_flags & O_NONBLOCK) {
		if (!mutex_trylock(&spi->tx_dma_mutex)) {
			ret = -EAGAIN;
			goto free_new_buffer;
		}
	} else {
		ret = mutex_lock_interruptible(&spi->tx_dma_mutex);
		if (ret)
			goto free_new_buffer;
	}
	if (READ_ONCE(spi->removing)) {
		ret = -ENODEV;
		mutex_unlock(&spi->tx_dma_mutex);
		wake_up_interruptible(&spi->tx_wait);
		goto free_new_buffer;
	}

	/* A new write replaces the previous published response. */
	dmaengine_terminate_sync(spi->tx_dma);
	tx_dma_buffer_free(spi);
	tx_path_clear(spi);

	/* Prime an optional prefix with CPU writes for diagnostics. */
	pio_bytes = 0;
	while (pio_bytes < count && pio_bytes < tx_pio_limit) {
		fr = readl(spi->bsc + BSC_FR);
		if (fr & FR_TXFF)
			break;
		writel(bytes[pio_bytes], spi->bsc + BSC_DR);
		pio_bytes++;
	}

	if (pio_bytes == count) {
		dma_free_coherent(spi->dev, new_dma_size,
				  new_dma_buf, new_dma_handle);
		kfree(bytes);
		goto write_success;
	}

	/* The DMA engine writes 32 bits; BSC_DR transmits the low byte. */
	dma_bytes = count - pio_bytes;
	for (i = 0; i < dma_bytes; i++)
		new_dma_buf[i] = bytes[pio_bytes + i];
	kfree(bytes);
	bytes = NULL;

	desc = dmaengine_prep_slave_single(spi->tx_dma, new_dma_handle,
					   dma_bytes * sizeof(*new_dma_buf),
					   DMA_MEM_TO_DEV,
					   DMA_CTRL_ACK);
	if (!desc) {
		ret = -EIO;
		goto unlock_free_buffer;
	}
	cookie = dmaengine_submit(desc);
	ret = dma_submit_error(cookie);
	if (ret)
		goto unlock_free_buffer;

	spi->tx_dma_buf = new_dma_buf;
	spi->tx_dma_handle = new_dma_handle;
	WRITE_ONCE(spi->tx_dma_size, new_dma_size);
	dma_async_issue_pending(spi->tx_dma);

write_success:
	mutex_unlock(&spi->tx_dma_mutex);
	wake_up_interruptible(&spi->tx_wait);
	spin_lock_irqsave(&spi->tx_lock, flags);
	spi->tx_write_completed += count;
	spin_unlock_irqrestore(&spi->tx_lock, flags);
	return count;

unlock_free_buffer:
	dma_free_coherent(spi->dev, new_dma_size,
			  new_dma_buf, new_dma_handle);
	mutex_unlock(&spi->tx_dma_mutex);
	wake_up_interruptible(&spi->tx_wait);
	goto record_error;

free_new_buffer:
	dma_free_coherent(spi->dev, new_dma_size,
			  new_dma_buf, new_dma_handle);
	kfree(bytes);
record_error:
	spin_lock_irqsave(&spi->tx_lock, flags);
	spi->tx_write_errors++;
	spi->tx_write_short++;
	spin_unlock_irqrestore(&spi->tx_lock, flags);
	return ret;
}

static int cm4_spi_slave_open(struct inode *inode, struct file *file)
{
	struct miscdevice *miscdev = file->private_data;
	struct bcm_spi_slave *spi = container_of(miscdev,
						 struct bcm_spi_slave, miscdev);
	unsigned long flags;

	(void)inode;
	if (READ_ONCE(spi->removing))
		return -ENODEV;

	atomic_inc(&spi->open_count);
	smp_mb__after_atomic();
	if (READ_ONCE(spi->removing)) {
		if (atomic_dec_and_test(&spi->open_count))
			wake_up(&spi->remove_wait);
		return -ENODEV;
	}

	if (file->f_mode & FMODE_WRITE) {
	mutex_lock(&spi->tx_dma_mutex);
	if (READ_ONCE(spi->removing)) {
		mutex_unlock(&spi->tx_dma_mutex);
		if (atomic_dec_and_test(&spi->open_count))
			wake_up(&spi->remove_wait);
		return -ENODEV;
	}
	spin_lock_irqsave(&spi->tx_lock, flags);
	spi->tx_writers++;
	spin_unlock_irqrestore(&spi->tx_lock, flags);
	mutex_unlock(&spi->tx_dma_mutex);
	wake_up_interruptible(&spi->tx_wait);
	}

	file->private_data = spi;
	return 0;
}

static int cm4_spi_slave_release(struct inode *inode, struct file *file)
{
	struct bcm_spi_slave *spi = file->private_data;
	unsigned long flags;
	int clear_tx = 0;

	(void)inode;

	if (file->f_mode & FMODE_WRITE) {
	mutex_lock(&spi->tx_dma_mutex);
	spin_lock_irqsave(&spi->tx_lock, flags);
	if (spi->tx_writers > 0)
		spi->tx_writers--;
	clear_tx = spi->tx_writers == 0;
	spin_unlock_irqrestore(&spi->tx_lock, flags);

	if (clear_tx) {
	dmaengine_terminate_sync(spi->tx_dma);
	tx_dma_buffer_free(spi);
	tx_path_clear(spi);
	}
	mutex_unlock(&spi->tx_dma_mutex);
	wake_up_interruptible(&spi->tx_wait);
	}

	if (atomic_dec_and_test(&spi->open_count))
	wake_up(&spi->remove_wait);

	return 0;
}

static __poll_t cm4_spi_slave_poll(struct file *file, poll_table *wait)
{
	struct bcm_spi_slave *spi = file->private_data;
	__poll_t mask = 0;
	unsigned long flags;

	poll_wait(file, &spi->rx_wait, wait);
	poll_wait(file, &spi->tx_wait, wait);
	if (READ_ONCE(spi->removing))
	return POLLERR | POLLHUP;

	spin_lock_irqsave(&spi->rx_lock, flags);
	if (spi->rx_count > 0)
	mask |= POLLIN | POLLRDNORM;
	spin_unlock_irqrestore(&spi->rx_lock, flags);

	if ((file->f_mode & FMODE_WRITE) &&
	    mutex_trylock(&spi->tx_dma_mutex)) {
	mask |= POLLOUT | POLLWRNORM;
	mutex_unlock(&spi->tx_dma_mutex);
	}

	return mask;
}

static const struct file_operations cm4_spi_slave_fops = {
	.owner = THIS_MODULE,
	.open = cm4_spi_slave_open,
	.release = cm4_spi_slave_release,
	.read = cm4_spi_slave_read,
	.write = cm4_spi_slave_write,
	.poll = cm4_spi_slave_poll,
	.llseek = noop_llseek,
};

static int monitor_thread(void *arg)
{
	struct bcm_spi_slave *spi = arg;

	pr_info("SPI Slave RX monitor started\n");

	while (!kthread_should_stop()) {
		cm4_bsc_drain_rx_fifo(spi);

	usleep_range(poll_interval_us, poll_interval_us + 10);
	}

	pr_info("SPI Slave RX monitor stopped\n");
	return 0;
}

static int bcm_spi_probe(struct platform_device *pdev)
{
	struct dma_slave_config dma_config = {};
	struct bcm_spi_slave *spi;
	struct resource *res;
	int ret;

	spi = devm_kzalloc(&pdev->dev, sizeof(*spi), GFP_KERNEL);
	if (!spi)
		return -ENOMEM;

	spi->dev = &pdev->dev;
	spi->id = ida_alloc(&bcm_spi_slave_ida, GFP_KERNEL);
	if (spi->id < 0)
		return spi->id;
	ret = devm_add_action_or_reset(&pdev->dev, bcm_spi_free_id, spi);
	if (ret)
		return ret;

	mutex_init(&spi->tx_dma_mutex);
	spin_lock_init(&spi->rx_lock);
	spin_lock_init(&spi->tx_lock);
	init_waitqueue_head(&spi->rx_wait);
	init_waitqueue_head(&spi->tx_wait);
	init_waitqueue_head(&spi->remove_wait);
	atomic_set(&spi->open_count, 0);
	platform_set_drvdata(pdev, spi);

	dev_info(&pdev->dev, "probing BCM2711 BSC SPI slave\n");
	dev_info(&pdev->dev,
		 "FR bits RXFE=0x%08X TXFF=0x%08X TXFE=0x%08X\n",
		FR_RXFE,
		FR_TXFF,
		FR_TXFE);

	if (poll_interval_us == 0)
	poll_interval_us = 1;

	/* Unlike the BCM2835 SPI-master block, the BCM2711 BSC slave accepts DMA
	 * writes through the translated CPU physical alias (0xfe...), verified by
	 * observing FR_TXFE clear after a DMA-only diagnostic transfer.
	 */
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
	return dev_err_probe(&pdev->dev, -EINVAL,
			     "missing BSC DMA register address\n");
	spi->bsc_dr_dma_addr = res->start + BSC_DR;
	if (dma_bus_address)
	spi->bsc_dr_dma_addr = dma_bus_address;

	spi->bsc = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(spi->bsc))
	return dev_err_probe(&pdev->dev, PTR_ERR(spi->bsc),
			     "failed to map BSC registers\n");

	spi->tx_dma = dma_request_chan(&pdev->dev, "tx");
	if (IS_ERR(spi->tx_dma)) {
	ret = dev_err_probe(&pdev->dev, PTR_ERR(spi->tx_dma),
			    "failed to request TX DMA channel\n");
	spi->tx_dma = NULL;
	return ret;
	}
	dev_info(&pdev->dev, "acquired TX DMA channel %s\n",
		 dma_chan_name(spi->tx_dma));
	dev_info(&pdev->dev, "TX DMA destination=%pad\n",
		 &spi->bsc_dr_dma_addr);

	dma_config.direction = DMA_MEM_TO_DEV;
	dma_config.dst_addr = spi->bsc_dr_dma_addr;
	dma_config.dst_addr_width = DMA_SLAVE_BUSWIDTH_4_BYTES;
	dma_config.dst_maxburst = 1;
	ret = dmaengine_slave_config(spi->tx_dma, &dma_config);
	if (ret) {
	dev_err(&pdev->dev, "failed to configure TX DMA: %d\n", ret);
	goto err_release_dma;
	}

	cm4_spi_reset_stats(spi);

	writel(0x00000000, spi->bsc + BSC_CR);
	udelay(100);
	writel(0x00000000, spi->bsc + BSC_RSR);
	writel(BSC_CR_ENABLED, spi->bsc + BSC_CR);
	writel((readl(spi->bsc + BSC_IFLS) & ~BSC_IFLS_TX_MASK) |
	       BSC_IFLS_TX_SEVEN_EIGHTHS, spi->bsc + BSC_IFLS);
	/* Enable transmit DMA requests; DREQ 8 paces writes to BSC_DR. */
	writel(BIT(1), spi->bsc + BSC_DMACR);
	cm4_bsc_discard_rx_fifo(spi);
	cm4_spi_reset_stats(spi);

	spi->miscdev.minor = MISC_DYNAMIC_MINOR;
	if (spi->id == 0)
		spi->miscdev.name = "cm4_spi_slave";
	else
		spi->miscdev.name = devm_kasprintf(&pdev->dev, GFP_KERNEL,
						   "cm4_spi_slave%d", spi->id);
	if (!spi->miscdev.name) {
		ret = -ENOMEM;
		goto err_release_dma;
	}
	spi->miscdev.fops = &cm4_spi_slave_fops;
	spi->miscdev.parent = &pdev->dev;
	spi->miscdev.mode = 0660;
	ret = misc_register(&spi->miscdev);
	if (ret) {
	dev_err(&pdev->dev,
		"failed to register /dev/cm4_spi_slave: %d\n", ret);
	goto err_release_dma;
	}

	dev_info(&pdev->dev, "registered /dev/%s\n", spi->miscdev.name);

	spi->debugfs_dir = debugfs_create_dir(spi->miscdev.name, NULL);
	if (IS_ERR_OR_NULL(spi->debugfs_dir)) {
	pr_warn("SPI Slave: debugfs unavailable\n");
	spi->debugfs_dir = NULL;
	} else {
	debugfs_create_file("stats", 0444, spi->debugfs_dir, spi,
				&cm4_spi_stats_fops);
	debugfs_create_file("regs", 0444, spi->debugfs_dir, spi,
				&cm4_spi_regs_fops);
	debugfs_create_file("reset_stats", 0200, spi->debugfs_dir, spi,
				&cm4_spi_reset_fops);
	debugfs_create_file("clear_tx", 0200, spi->debugfs_dir, spi,
				&cm4_spi_clear_tx_fops);
	dev_info(&pdev->dev, "debugfs at /sys/kernel/debug/%s\n",
		 spi->miscdev.name);
	}

	spi->monitor_task = kthread_run(
			monitor_thread,
			spi,
			"bsc_monitor");

	if (IS_ERR(spi->monitor_task)) {
	ret = PTR_ERR(spi->monitor_task);
	spi->monitor_task = NULL;
	dev_err(&pdev->dev, "failed to start monitor thread: %d\n", ret);
	debugfs_remove_recursive(spi->debugfs_dir);
	spi->debugfs_dir = NULL;
	WRITE_ONCE(spi->removing, true);
	misc_deregister(&spi->miscdev);
	wake_up_interruptible(&spi->rx_wait);
	wake_up_interruptible(&spi->tx_wait);
	wait_event(spi->remove_wait, atomic_read(&spi->open_count) == 0);
	goto err_release_dma;
	}

	return 0;

err_release_dma:
	if (spi->bsc)
	writel(0x00000000, spi->bsc + BSC_DMACR);
	if (spi->bsc)
	writel(0x00000000, spi->bsc + BSC_CR);
	if (spi->tx_dma)
	dmaengine_terminate_sync(spi->tx_dma);
	tx_dma_buffer_free(spi);
	if (spi->tx_dma)
		dma_release_channel(spi->tx_dma);
	spi->tx_dma = NULL;
	return ret;
}

static void bcm_spi_remove(struct platform_device *pdev)
{
	struct bcm_spi_slave *spi = platform_get_drvdata(pdev);

	WRITE_ONCE(spi->removing, true);
	debugfs_remove_recursive(spi->debugfs_dir);
	spi->debugfs_dir = NULL;
	misc_deregister(&spi->miscdev);
	wake_up_interruptible(&spi->rx_wait);
	wake_up_interruptible(&spi->tx_wait);
	wait_event(spi->remove_wait, atomic_read(&spi->open_count) == 0);

	if (spi->monitor_task)
	kthread_stop(spi->monitor_task);
	spi->monitor_task = NULL;

	mutex_lock(&spi->tx_dma_mutex);
	if (spi->tx_dma) {
	dmaengine_terminate_sync(spi->tx_dma);
	tx_dma_buffer_free(spi);
	if (spi->bsc)
		writel(0x00000000, spi->bsc + BSC_DMACR);
	if (spi->bsc)
		writel(0x00000000, spi->bsc + BSC_CR);
	dma_release_channel(spi->tx_dma);
	spi->tx_dma = NULL;
	}
	mutex_unlock(&spi->tx_dma_mutex);

	pr_info("SPI Slave: exit\n");
}

static const struct of_device_id bcm_spi_of_match[] = {
	{ .compatible = "eccb,bcm2711-bsc-spi-slave" },
	{}
};
MODULE_DEVICE_TABLE(of, bcm_spi_of_match);

static struct platform_driver bcm_spi_driver = {
	.probe = bcm_spi_probe,
	.remove = bcm_spi_remove,
	.driver = {
		.name = "bcm2711-bsc-spi-slave",
		.of_match_table = bcm_spi_of_match,
	},
};
module_platform_driver(bcm_spi_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Muhammad");
MODULE_DESCRIPTION("Experimental BCM2711 BSC SPI slave character driver");
MODULE_VERSION("0.1.0");
