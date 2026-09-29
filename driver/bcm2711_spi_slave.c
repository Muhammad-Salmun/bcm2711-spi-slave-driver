// SPDX-License-Identifier: GPL-2.0-only
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
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

static void __iomem *bsc;
static struct device *bsc_dev;
static struct dma_chan *tx_dma;
static u32 *tx_dma_buf;
static dma_addr_t tx_dma_handle;
static size_t tx_dma_size;
static dma_addr_t bsc_dr_dma_addr;
static DEFINE_MUTEX(tx_dma_mutex);
static struct task_struct *monitor_task;
static struct dentry *debugfs_dir;
static u8 rx_ring[RX_RING_SIZE];
static unsigned int rx_head;
static unsigned int rx_tail;
static unsigned int rx_count;
static unsigned int tx_writers;
static unsigned long long rx_total;
static unsigned long long rx_overruns;
static unsigned long long rx_read_calls;
static unsigned long long rx_read_bytes;
static unsigned long long rx_empty_waits;
static unsigned long long rx_fifo_drain_calls;
static unsigned long long rx_fifo_empty_polls;
static unsigned long long rx_fifo_not_empty_observed;
static unsigned long long rx_fifo_bytes_drained;
static unsigned long long rx_fifo_bytes_discarded;
static unsigned long long rx_debug_prints;
static u32 rx_last_fr;
static u32 rx_last_dr;
static unsigned long long tx_write_calls;
static unsigned long long tx_write_requested;
static unsigned long long tx_write_completed;
static unsigned long long tx_write_short;
static unsigned long long tx_write_errors;
static u32 tx_fifo_last_fr;
static unsigned long long tx_clear_events;
static DEFINE_SPINLOCK(rx_lock);
static DEFINE_SPINLOCK(tx_lock);
static DECLARE_WAIT_QUEUE_HEAD(rx_wait);
static DECLARE_WAIT_QUEUE_HEAD(tx_wait);

static void tx_path_clear(void);

static void cm4_bsc_discard_rx_fifo(void)
{
	int discarded = 0;
	u32 fr;

	if (!bsc)
	return;

	fr = readl(bsc + BSC_FR);
	while (!(fr & FR_RXFE) && discarded < 256) {
	readl(bsc + BSC_DR);
	discarded++;
	fr = readl(bsc + BSC_FR);
	}

	if (discarded > 0) {
	unsigned long flags;

	spin_lock_irqsave(&rx_lock, flags);
	rx_fifo_bytes_discarded += discarded;
	rx_last_fr = fr;
	spin_unlock_irqrestore(&rx_lock, flags);

	pr_info("SPI Slave: discarded %d stale RX FIFO bytes\n", discarded);
	}
}

static void cm4_spi_reset_stats(void)
{
	unsigned long flags;

	spin_lock_irqsave(&rx_lock, flags);
	rx_total = 0;
	rx_overruns = 0;
	rx_read_calls = 0;
	rx_read_bytes = 0;
	rx_empty_waits = 0;
	rx_fifo_drain_calls = 0;
	rx_fifo_empty_polls = 0;
	rx_fifo_not_empty_observed = 0;
	rx_fifo_bytes_drained = 0;
	rx_fifo_bytes_discarded = 0;
	rx_debug_prints = 0;
	rx_last_fr = 0;
	rx_last_dr = 0;
	spin_unlock_irqrestore(&rx_lock, flags);

	spin_lock_irqsave(&tx_lock, flags);
	tx_write_calls = 0;
	tx_write_requested = 0;
	tx_write_completed = 0;
	tx_write_short = 0;
	tx_write_errors = 0;
	tx_fifo_last_fr = 0;
	tx_clear_events = 0;
	spin_unlock_irqrestore(&tx_lock, flags);
}

static int cm4_spi_stats_show(struct seq_file *m, void *v)
{
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

	fr = readl(bsc + BSC_FR);

	spin_lock_irqsave(&rx_lock, flags);
	rx_head_snapshot = rx_head;
	rx_tail_snapshot = rx_tail;
	rx_count_snapshot = rx_count;
	rx_total_snapshot = rx_total;
	rx_overruns_snapshot = rx_overruns;
	rx_read_calls_snapshot = rx_read_calls;
	rx_read_bytes_snapshot = rx_read_bytes;
	rx_empty_waits_snapshot = rx_empty_waits;
	rx_fifo_drain_calls_snapshot = rx_fifo_drain_calls;
	rx_fifo_empty_polls_snapshot = rx_fifo_empty_polls;
	rx_fifo_not_empty_observed_snapshot = rx_fifo_not_empty_observed;
	rx_fifo_bytes_drained_snapshot = rx_fifo_bytes_drained;
	rx_fifo_bytes_discarded_snapshot = rx_fifo_bytes_discarded;
	rx_debug_prints_snapshot = rx_debug_prints;
	rx_last_fr_snapshot = rx_last_fr;
	rx_last_dr_snapshot = rx_last_dr;
	spin_unlock_irqrestore(&rx_lock, flags);

	spin_lock_irqsave(&tx_lock, flags);
	tx_writers_snapshot = tx_writers;
	tx_write_calls_snapshot = tx_write_calls;
	tx_write_requested_snapshot = tx_write_requested;
	tx_write_completed_snapshot = tx_write_completed;
	tx_write_short_snapshot = tx_write_short;
	tx_write_errors_snapshot = tx_write_errors;
	tx_fifo_last_fr_snapshot = tx_fifo_last_fr;
	tx_clear_events_snapshot = tx_clear_events;
	spin_unlock_irqrestore(&tx_lock, flags);
	tx_dma_size_snapshot = READ_ONCE(tx_dma_size);

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
	(void)file;
	(void)buf;
	(void)ppos;

	cm4_spi_reset_stats();
	return count;
}

static const struct file_operations cm4_spi_reset_fops = {
	.owner = THIS_MODULE,
	.write = cm4_spi_reset_write,
	.llseek = noop_llseek,
};

static int cm4_spi_regs_show(struct seq_file *m, void *v)
{
	(void)v;

	/* BSC_DR is intentionally omitted: reading it consumes RX FIFO data. */
	seq_printf(m, "RSR=0x%08X\n", readl(bsc + BSC_RSR));
	seq_printf(m, "SLV=0x%08X\n", readl(bsc + BSC_SLV));
	seq_printf(m, "CR=0x%08X\n", readl(bsc + BSC_CR));
	seq_printf(m, "FR=0x%08X\n", readl(bsc + BSC_FR));
	seq_printf(m, "IFLS=0x%08X\n", readl(bsc + BSC_IFLS));
	seq_printf(m, "IMSC=0x%08X\n", readl(bsc + BSC_IMSC));
	seq_printf(m, "RIS=0x%08X\n", readl(bsc + BSC_RIS));
	seq_printf(m, "MIS=0x%08X\n", readl(bsc + BSC_MIS));
	seq_printf(m, "ICR=0x%08X\n", readl(bsc + BSC_ICR));
	seq_printf(m, "DMACR=0x%08X\n", readl(bsc + BSC_DMACR));
	seq_printf(m, "DEBUG_SPI=0x%08X\n", readl(bsc + BSC_DEBUG_SPI));

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

static void tx_dma_buffer_free(void)
{
	if (!tx_dma_buf)
		return;

	dma_free_coherent(bsc_dev, tx_dma_size, tx_dma_buf, tx_dma_handle);
	tx_dma_buf = NULL;
	tx_dma_handle = 0;
	WRITE_ONCE(tx_dma_size, 0);
}

static ssize_t cm4_spi_clear_tx_write(
	struct file *file,
	const char __user *buf,
	size_t count,
	loff_t *ppos)
{
	(void)file;
	(void)buf;
	(void)ppos;

	mutex_lock(&tx_dma_mutex);
	dmaengine_terminate_sync(tx_dma);
	tx_dma_buffer_free();
	tx_path_clear();
	mutex_unlock(&tx_dma_mutex);
	wake_up_interruptible(&tx_wait);
	return count;
}

static const struct file_operations cm4_spi_clear_tx_fops = {
	.owner = THIS_MODULE,
	.write = cm4_spi_clear_tx_write,
	.llseek = noop_llseek,
};

static void tx_path_clear(void)
{
	unsigned long flags;

	spin_lock_irqsave(&tx_lock, flags);
	tx_clear_events++;

	if (bsc) {
	writel(0x00000000, bsc + BSC_CR);
	udelay(10);
	writel(0x00000000, bsc + BSC_RSR);
	writel(BSC_CR_ENABLED, bsc + BSC_CR);
	writel((readl(bsc + BSC_IFLS) & ~BSC_IFLS_TX_MASK) |
	       BSC_IFLS_TX_SEVEN_EIGHTHS, bsc + BSC_IFLS);
	writel(BIT(1), bsc + BSC_DMACR);
	tx_fifo_last_fr = readl(bsc + BSC_FR);
	}

	spin_unlock_irqrestore(&tx_lock, flags);
	pr_info("SPI Slave: TX path cleared\n");
}

static void rx_ring_push(u8 rx)
{
	unsigned long flags;

	spin_lock_irqsave(&rx_lock, flags);

	if (rx_count == RX_RING_SIZE) {
	rx_tail = (rx_tail + 1) % RX_RING_SIZE;
	rx_overruns++;
	} else {
	rx_count++;
	}

	rx_ring[rx_head] = rx;
	rx_head = (rx_head + 1) % RX_RING_SIZE;
	rx_total++;

	spin_unlock_irqrestore(&rx_lock, flags);
	wake_up_interruptible(&rx_wait);
}

static void cm4_bsc_drain_rx_fifo(void)
{
	int drained = 0;
	u32 fr;

	spin_lock(&rx_lock);
	rx_fifo_drain_calls++;
	spin_unlock(&rx_lock);

	fr = readl(bsc + BSC_FR);
	if (fr & FR_RXFE) {
	spin_lock(&rx_lock);
	rx_fifo_empty_polls++;
	rx_last_fr = fr;
	spin_unlock(&rx_lock);
	return;
	}

	do {
	u32 dr = readl(bsc + BSC_DR);
	u8 rx = dr & 0xFF;
	int do_debug_print = 0;

	spin_lock(&rx_lock);
	rx_fifo_not_empty_observed++;
	rx_fifo_bytes_drained++;
	rx_last_fr = fr;
	rx_last_dr = dr;
	if (rx_debug_prints < 16) {
		rx_debug_prints++;
		do_debug_print = 1;
	}
	spin_unlock(&rx_lock);

	if (do_debug_print) {
		pr_info("SPI Slave RX byte=0x%02X DR=0x%08X FR=0x%08X\n",
			rx,
			dr,
			fr);
	}

	rx_ring_push(rx);
	drained++;

	if (drained >= 64)
		break;

	fr = readl(bsc + BSC_FR);
	} while (!(fr & FR_RXFE));
}

static int rx_ring_pop(u8 *rx)
{
	unsigned long flags;
	int ret = 0;

	spin_lock_irqsave(&rx_lock, flags);

	if (rx_count > 0) {
	*rx = rx_ring[rx_tail];
	rx_tail = (rx_tail + 1) % RX_RING_SIZE;
	rx_count--;
	ret = 1;
	}

	spin_unlock_irqrestore(&rx_lock, flags);

	return ret;
}

static ssize_t cm4_spi_slave_read(
	struct file *file,
	char __user *buf,
	size_t count,
	loff_t *ppos)
{
	unsigned long flags;
	size_t copied = 0;
	u8 rx;

	(void)ppos;

	if (count == 0)
	return 0;

	spin_lock_irqsave(&rx_lock, flags);
	rx_read_calls++;
	spin_unlock_irqrestore(&rx_lock, flags);

	while (!rx_ring_pop(&rx)) {
	if (file->f_flags & O_NONBLOCK)
		return -EAGAIN;

	spin_lock_irqsave(&rx_lock, flags);
	rx_empty_waits++;
	spin_unlock_irqrestore(&rx_lock, flags);
	if (wait_event_interruptible(rx_wait, READ_ONCE(rx_count) > 0))
		return -ERESTARTSYS;
	}

	do {
	if (put_user(rx, buf + copied))
		return copied ? copied : -EFAULT;

	copied++;
	} while (copied < count && rx_ring_pop(&rx));

	spin_lock_irqsave(&rx_lock, flags);
	rx_read_bytes += copied;
	spin_unlock_irqrestore(&rx_lock, flags);
	return copied;
}

static ssize_t cm4_spi_slave_write(
	struct file *file,
	const char __user *buf,
	size_t count,
	loff_t *ppos)
{
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

	(void)file;
	(void)ppos;
	if (count == 0)
	return 0;
	if (check_mul_overflow(count, sizeof(*new_dma_buf), &new_dma_size))
	return -EOVERFLOW;

	spin_lock_irqsave(&tx_lock, flags);
	tx_write_calls++;
	tx_write_requested += count;
	spin_unlock_irqrestore(&tx_lock, flags);

	bytes = memdup_user(buf, count);
	if (IS_ERR(bytes)) {
	ret = PTR_ERR(bytes);
	goto record_error;
	}

	new_dma_buf = dma_alloc_coherent(bsc_dev, new_dma_size,
					 &new_dma_handle, GFP_KERNEL);
	if (!new_dma_buf) {
		ret = -ENOMEM;
		kfree(bytes);
		goto record_error;
	}

	if (file->f_flags & O_NONBLOCK) {
		if (!mutex_trylock(&tx_dma_mutex)) {
			ret = -EAGAIN;
			goto free_new_buffer;
		}
	} else {
		ret = mutex_lock_interruptible(&tx_dma_mutex);
		if (ret)
			goto free_new_buffer;
	}

	/* A new write replaces the previous published response. */
	dmaengine_terminate_sync(tx_dma);
	tx_dma_buffer_free();
	tx_path_clear();

	/* Prime an optional prefix with CPU writes for diagnostics. */
	pio_bytes = 0;
	while (pio_bytes < count && pio_bytes < tx_pio_limit) {
		fr = readl(bsc + BSC_FR);
		if (fr & FR_TXFF)
			break;
		writel(bytes[pio_bytes], bsc + BSC_DR);
		pio_bytes++;
	}

	if (pio_bytes == count) {
		dma_free_coherent(bsc_dev, new_dma_size,
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

	desc = dmaengine_prep_slave_single(tx_dma, new_dma_handle,
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

	tx_dma_buf = new_dma_buf;
	tx_dma_handle = new_dma_handle;
	WRITE_ONCE(tx_dma_size, new_dma_size);
	dma_async_issue_pending(tx_dma);

write_success:
	mutex_unlock(&tx_dma_mutex);
	wake_up_interruptible(&tx_wait);
	spin_lock_irqsave(&tx_lock, flags);
	tx_write_completed += count;
	spin_unlock_irqrestore(&tx_lock, flags);
	return count;

unlock_free_buffer:
	dma_free_coherent(bsc_dev, new_dma_size,
			  new_dma_buf, new_dma_handle);
	mutex_unlock(&tx_dma_mutex);
	wake_up_interruptible(&tx_wait);
	goto record_error;

free_new_buffer:
	dma_free_coherent(bsc_dev, new_dma_size,
			  new_dma_buf, new_dma_handle);
	kfree(bytes);
record_error:
	spin_lock_irqsave(&tx_lock, flags);
	tx_write_errors++;
	tx_write_short++;
	spin_unlock_irqrestore(&tx_lock, flags);
	return ret;
}

static int cm4_spi_slave_open(struct inode *inode, struct file *file)
{
	unsigned long flags;

	(void)inode;

	if (file->f_mode & FMODE_WRITE) {
	spin_lock_irqsave(&tx_lock, flags);
	tx_writers++;
	spin_unlock_irqrestore(&tx_lock, flags);
	}

	return 0;
}

static int cm4_spi_slave_release(struct inode *inode, struct file *file)
{
	unsigned long flags;
	int clear_tx = 0;

	(void)inode;

	if (file->f_mode & FMODE_WRITE) {
	spin_lock_irqsave(&tx_lock, flags);
	if (tx_writers > 0)
		tx_writers--;
	clear_tx = tx_writers == 0;
	spin_unlock_irqrestore(&tx_lock, flags);

	if (clear_tx) {
	mutex_lock(&tx_dma_mutex);
	dmaengine_terminate_sync(tx_dma);
	tx_dma_buffer_free();
	tx_path_clear();
	mutex_unlock(&tx_dma_mutex);
	wake_up_interruptible(&tx_wait);
	}
	}

	return 0;
}

static __poll_t cm4_spi_slave_poll(struct file *file, poll_table *wait)
{
	__poll_t mask = 0;
	unsigned long flags;

	poll_wait(file, &rx_wait, wait);
	poll_wait(file, &tx_wait, wait);

	spin_lock_irqsave(&rx_lock, flags);
	if (rx_count > 0)
	mask |= POLLIN | POLLRDNORM;
	spin_unlock_irqrestore(&rx_lock, flags);

	if ((file->f_mode & FMODE_WRITE) && mutex_trylock(&tx_dma_mutex)) {
	mask |= POLLOUT | POLLWRNORM;
	mutex_unlock(&tx_dma_mutex);
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

static struct miscdevice cm4_spi_slave_miscdev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "cm4_spi_slave",
	.fops = &cm4_spi_slave_fops,
	.mode = 0660,
};

static int monitor_thread(void *arg)
{
	(void)arg;

	pr_info("SPI Slave RX monitor started\n");

	while (!kthread_should_stop()) {
	cm4_bsc_drain_rx_fifo();

	usleep_range(poll_interval_us, poll_interval_us + 10);
	}

	pr_info("SPI Slave RX monitor stopped\n");
	return 0;
}

static int bcm_spi_probe(struct platform_device *pdev)
{
	struct dma_slave_config dma_config = {};
	struct resource *res;
	int ret;

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
	bsc_dr_dma_addr = res->start + BSC_DR;
	if (dma_bus_address)
	bsc_dr_dma_addr = dma_bus_address;

	bsc = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(bsc))
	return dev_err_probe(&pdev->dev, PTR_ERR(bsc),
			     "failed to map BSC registers\n");

	tx_dma = dma_request_chan(&pdev->dev, "tx");
	if (IS_ERR(tx_dma)) {
	ret = dev_err_probe(&pdev->dev, PTR_ERR(tx_dma),
			    "failed to request TX DMA channel\n");
	tx_dma = NULL;
	bsc = NULL;
	return ret;
	}
	dev_info(&pdev->dev, "acquired TX DMA channel %s\n",
		 dma_chan_name(tx_dma));
	dev_info(&pdev->dev, "TX DMA destination=%pad\n", &bsc_dr_dma_addr);

	dma_config.direction = DMA_MEM_TO_DEV;
	dma_config.dst_addr = bsc_dr_dma_addr;
	dma_config.dst_addr_width = DMA_SLAVE_BUSWIDTH_4_BYTES;
	dma_config.dst_maxburst = 1;
	ret = dmaengine_slave_config(tx_dma, &dma_config);
	if (ret) {
	dev_err(&pdev->dev, "failed to configure TX DMA: %d\n", ret);
	goto err_release_dma;
	}

	bsc_dev = &pdev->dev;
	tx_dma_buf = NULL;
	tx_dma_handle = 0;
	tx_dma_size = 0;

	spin_lock_init(&rx_lock);
	spin_lock_init(&tx_lock);
	rx_head = 0;
	rx_tail = 0;
	rx_count = 0;
	tx_writers = 0;
	cm4_spi_reset_stats();

	writel(0x00000000, bsc + BSC_CR);
	udelay(100);
	writel(0x00000000, bsc + BSC_RSR);
	writel(BSC_CR_ENABLED, bsc + BSC_CR);
	writel((readl(bsc + BSC_IFLS) & ~BSC_IFLS_TX_MASK) |
	       BSC_IFLS_TX_SEVEN_EIGHTHS, bsc + BSC_IFLS);
	/* Enable transmit DMA requests; DREQ 8 paces writes to BSC_DR. */
	writel(BIT(1), bsc + BSC_DMACR);
	cm4_bsc_discard_rx_fifo();
	cm4_spi_reset_stats();

	ret = misc_register(&cm4_spi_slave_miscdev);
	if (ret) {
	dev_err(&pdev->dev,
		"failed to register /dev/cm4_spi_slave: %d\n", ret);
	goto err_release_dma;
	}

	pr_info("SPI Slave: registered /dev/cm4_spi_slave\n");

	debugfs_dir = debugfs_create_dir("cm4_spi_slave", NULL);
	if (IS_ERR_OR_NULL(debugfs_dir)) {
	pr_warn("SPI Slave: debugfs unavailable\n");
	debugfs_dir = NULL;
	} else {
	debugfs_create_file("stats", 0444, debugfs_dir, NULL,
				&cm4_spi_stats_fops);
	debugfs_create_file("regs", 0444, debugfs_dir, NULL,
				&cm4_spi_regs_fops);
	debugfs_create_file("reset_stats", 0200, debugfs_dir, NULL,
				&cm4_spi_reset_fops);
	debugfs_create_file("clear_tx", 0200, debugfs_dir, NULL,
				&cm4_spi_clear_tx_fops);
	pr_info("SPI Slave: debugfs at /sys/kernel/debug/cm4_spi_slave\n");
	}

	monitor_task = kthread_run(
			monitor_thread,
			NULL,
			"bsc_monitor");

	if (IS_ERR(monitor_task)) {
	ret = PTR_ERR(monitor_task);
	monitor_task = NULL;
	dev_err(&pdev->dev, "failed to start monitor thread: %d\n", ret);
	debugfs_remove_recursive(debugfs_dir);
	debugfs_dir = NULL;
	misc_deregister(&cm4_spi_slave_miscdev);
	goto err_release_dma;
	}

	platform_set_drvdata(pdev, tx_dma);
	return 0;

err_release_dma:
	tx_dma_buffer_free();
	dma_release_channel(tx_dma);
	tx_dma = NULL;
	bsc_dev = NULL;
	bsc = NULL;
	return ret;
}

static void bcm_spi_remove(struct platform_device *pdev)
{
	(void)pdev;

	if (monitor_task)
	kthread_stop(monitor_task);
	monitor_task = NULL;

	debugfs_remove_recursive(debugfs_dir);
	debugfs_dir = NULL;
	misc_deregister(&cm4_spi_slave_miscdev);

	if (bsc)
	writel(0x00000000, bsc + BSC_DMACR);
	if (bsc)
	writel(0x00000000, bsc + BSC_CR);

	if (tx_dma) {
	dmaengine_terminate_sync(tx_dma);
	tx_dma_buffer_free();
	dma_release_channel(tx_dma);
	tx_dma = NULL;
	}
	bsc_dev = NULL;
	bsc = NULL;

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
