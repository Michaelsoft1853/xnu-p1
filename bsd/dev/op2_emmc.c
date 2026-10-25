/*
 * OnePlus 2 (MSM8994) Internal eMMC 5.0 / SDHCI Persistent Root Device
 * Darwin/XNU Block Device Driver (read-only)
 */
#if defined(ARM64_BOARD_CONFIG_MSM8994)
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/buf.h>
#include <sys/conf.h>
#include <sys/disk.h>
#include <sys/errno.h>
#include <sys/proc.h>
#include <sys/uio.h>
#include <miscfs/devfs/devfs.h>
#include <kern/locks.h>
#include <kern/clock.h>
#include <vm/pmap.h>
#include <vm/vm_kern.h>
#include <pexpert/pexpert.h>
#include <pexpert/device_tree.h>
#include <libkern/libkern.h>

int op2_emmc_rootdev(dev_t *dev);
extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern uint64_t ml_get_timebase(void);

#define SDHC_REG_BASE    0xf9824900UL
#define SDHC_CORE_BASE   0xf9824000UL
#define TLMM_SDC1_CTL_REG 0xfd512044UL

/* Core MSM SDCC Registers */
#define CORE_POWER            0x00
#define CORE_SW_RST           (1 << 7)
#define CORE_MCI_VERSION      0x50
#define CORE_HC_MODE          0x78
#define HC_MODE_EN            (1 << 0)
#define FF_CLK_SW_RST_DIS     (1 << 13)
#define CORE_PWRCTL_STATUS    0xDC
#define CORE_PWRCTL_CLEAR     0xE4
#define CORE_PWRCTL_CTL       0xE8
#define CORE_PWRCTL_BUS_ON    (1 << 1)
#define CORE_PWRCTL_BUS_OFF   (1 << 0)
#define CORE_PWRCTL_IO_LOW    (1 << 2)
#define CORE_PWRCTL_IO_HIGH   (1 << 3)
#define CORE_PWRCTL_BUS_SUCCESS (1 << 0)
#define CORE_PWRCTL_IO_SUCCESS  (1 << 2)

#define CORE_DLL_CONFIG       0x100
#define CORE_DLL_PDN          (1 << 29)
#define CORE_DLL_RST          (1 << 30)

#define CORE_VENDOR_SPEC      0x10C
#define CORE_CLK_PWRSAVE      (1 << 1)
#define CORE_HC_MCLK_SEL_DFLT (2 << 8)
#define CORE_HC_MCLK_SEL_MASK (3 << 8)
#define CORE_IO_PAD_PWR_SWITCH_EN (1 << 15)
#define CORE_HC_SELECT_IN_EN  (1 << 18)
#define CORE_HC_SELECT_IN_MASK (7 << 19)

/* Standard SDHCI Registers */
#define SDHCI_BLOCK_SIZE      0x04
#define SDHCI_BLOCK_COUNT     0x06
#define SDHCI_ARGUMENT        0x08
#define SDHCI_TRANSFER_MODE   0x0C
#define SDHCI_COMMAND         0x0E
#define SDHCI_RESPONSE        0x10
#define SDHCI_BUFFER          0x20
#define SDHCI_PRESENT_STATE   0x24
#define SDHCI_HOST_CONTROL    0x28
#define  SDHCI_CTRL_4BITBUS   (1 << 1)
#define  SDHCI_CTRL_8BITBUS   (1 << 5)
#define SDHCI_POWER_CONTROL   0x29
#define SDHCI_CLOCK_CONTROL   0x2C
#define SDHCI_TIMEOUT_CONTROL 0x2E
#define SDHCI_SOFTWARE_RESET  0x2F
#define  SDHCI_RESET_CMD      0x02
#define  SDHCI_RESET_DATA     0x04
#define SDHCI_INT_STATUS      0x30
#define SDHCI_INT_ENABLE      0x34
#define SDHCI_SIGNAL_ENABLE   0x38
#define SDHCI_HOST_CONTROL2   0x3E
#define SDHCI_HOST_VERSION    0xFE

/* Present State bits */
#define SDHCI_CMD_INHIBIT     (1u << 0)
#define SDHCI_DATA_INHIBIT    (1u << 1)
#define SDHCI_BUFFER_READ_EN  (1u << 11)

/* Transfer Mode bits */
#define SDHCI_TRNS_BLK_CNT_EN (1u << 1)
#define SDHCI_TRNS_READ       (1u << 4)

/* Command flags */
#define SDHCI_CMD_RESP_NONE       0x00
#define SDHCI_CMD_RESP_LONG       0x01
#define SDHCI_CMD_RESP_SHORT      0x02
#define SDHCI_CMD_RESP_SHORT_BUSY 0x03
#define SDHCI_CMD_CRC_CHECK       (1u << 3)
#define SDHCI_CMD_INDEX_CHECK     (1u << 4)
#define SDHCI_CMD_DATA_PRESENT    (1u << 5)

/* Interrupt Status bits */
#define SDHCI_INT_RESPONSE    (1u << 0)
#define SDHCI_INT_DATA_END    (1u << 1)
#define SDHCI_INT_DATA_AVAIL  (1u << 5)
#define SDHCI_INT_ERROR       (1u << 15)

#define EMMC_SECTOR_SIZE      512u

struct gpt_header {
	uint64_t signature;           /* "EFI PART" */
	uint32_t revision;
	uint32_t header_size;
	uint32_t header_crc32;
	uint32_t reserved;
	uint64_t my_lba;
	uint64_t alternate_lba;
	uint64_t first_usable_lba;
	uint64_t last_usable_lba;
	uint8_t  disk_guid[16];
	uint64_t partition_entry_lba;
	uint32_t num_partition_entries;
	uint32_t sizeof_partition_entry;
	uint32_t partition_entry_array_crc32;
} __attribute__((packed));

struct gpt_entry {
	uint8_t  type_guid[16];
	uint8_t  unique_guid[16];
	uint64_t starting_lba;
	uint64_t ending_lba;
	uint64_t attributes;
	uint16_t partition_name[36];
} __attribute__((packed));

struct op2_emmc_info {
	vm_offset_t regs;
	vm_offset_t core_regs;
	uint64_t first_block;
	uint64_t nblocks;
	uint32_t secsize;
	int bmajor;
	int cmajor;
	dev_t bdev;
	bool ready;
};

static struct op2_emmc_info op2_emmc;
static uint8_t op2_sector_buf[EMMC_SECTOR_SIZE] __attribute__((aligned(64)));

static lck_grp_t *op2_emmc_grp = NULL;
static lck_mtx_t *op2_emmc_lock = NULL;

static inline uint32_t sdhc_rd32(uint32_t off)
{
	return *(volatile uint32_t *)(op2_emmc.regs + off);
}

static inline void sdhc_wr32(uint32_t off, uint32_t val)
{
	*(volatile uint32_t *)(op2_emmc.regs + off) = val;
}

static inline uint16_t sdhc_rd16(uint32_t off)
{
	return *(volatile uint16_t *)(op2_emmc.regs + off);
}

static inline void sdhc_wr16(uint32_t off, uint16_t val)
{
	*(volatile uint16_t *)(op2_emmc.regs + off) = val;
}

static inline uint8_t sdhc_rd8(uint32_t off)
{
	return *(volatile uint8_t *)(op2_emmc.regs + off);
}

static inline void sdhc_wr8(uint32_t off, uint8_t val)
{
	*(volatile uint8_t *)(op2_emmc.regs + off) = val;
}

static inline uint32_t __unused sdhc_core_rd32(uint32_t off)
{
	return *(volatile uint32_t *)(op2_emmc.core_regs + off);
}

static inline void __unused sdhc_core_wr32(uint32_t off, uint32_t val)
{
	*(volatile uint32_t *)(op2_emmc.core_regs + off) = val;
}

static void sdhc_set_clock(uint32_t freq_hz)
{
	uint16_t clk = sdhc_rd16(SDHCI_CLOCK_CONTROL);
	sdhc_wr16(SDHCI_CLOCK_CONTROL, clk & ~0x0004);

	uint32_t base_clk = 200000000;
	uint32_t div = base_clk / (2 * freq_hz);
	if (div > 0x3FF) div = 0x3FF;
	if (div == 0) div = 1;

	uint16_t clk_val = ((div & 0xFF) << 8) | (((div >> 8) & 0x3) << 6);
	sdhc_wr16(SDHCI_CLOCK_CONTROL, clk_val | 0x0001);

	uint64_t start = ml_get_timebase();
	while (!(sdhc_rd16(SDHCI_CLOCK_CONTROL) & 0x0002)) {
		if (ml_get_timebase() - start > 50000ULL * 192ULL / 10ULL)
			break;
		__asm__ volatile("isb");
	}

	sdhc_wr16(SDHCI_CLOCK_CONTROL, clk_val | 0x0001 | 0x0004);
	delay(10000);
}

static int sdhc_send_cmd(uint16_t opcode, uint32_t arg, uint16_t flags, uint32_t *resp0)
{
	uint64_t start = ml_get_timebase();
	uint64_t ticks = 50000ULL * 192ULL / 10ULL;
	while (sdhc_rd32(SDHCI_PRESENT_STATE) & SDHCI_CMD_INHIBIT) {
		if (ml_get_timebase() - start >= ticks) {
			sdhc_wr8(SDHCI_SOFTWARE_RESET, SDHCI_RESET_CMD);
			delay(1000);
			return -1;
		}
		__asm__ volatile("isb");
	}

	if ((flags & SDHCI_CMD_DATA_PRESENT) || (flags & SDHCI_CMD_RESP_SHORT_BUSY)) {
		start = ml_get_timebase();
		while (sdhc_rd32(SDHCI_PRESENT_STATE) & SDHCI_DATA_INHIBIT) {
			if (ml_get_timebase() - start >= ticks) {
				sdhc_wr8(SDHCI_SOFTWARE_RESET, SDHCI_RESET_DATA);
				delay(1000);
				return -2;
			}
			__asm__ volatile("isb");
		}
	}

	sdhc_wr32(SDHCI_INT_STATUS, 0xFFFFFFFF);
	sdhc_wr32(SDHCI_ARGUMENT, arg);

	uint16_t cmd_val = ((opcode & 0x3F) << 8) | (flags & 0xFF);
	sdhc_wr16(SDHCI_COMMAND, cmd_val);

	start = ml_get_timebase();
	ticks = 100000ULL * 192ULL / 10ULL;
	uint32_t is = 0;
	while (ml_get_timebase() - start < ticks) {
		is = sdhc_rd32(SDHCI_INT_STATUS);
		if (is & (SDHCI_INT_RESPONSE | SDHCI_INT_ERROR))
			break;
		__asm__ volatile("isb");
	}

	if (is & SDHCI_INT_ERROR) {
		sdhc_wr32(SDHCI_INT_STATUS, is);
		sdhc_wr8(SDHCI_SOFTWARE_RESET, SDHCI_RESET_CMD);
		uint64_t t = ml_get_timebase();
		while ((sdhc_rd8(SDHCI_SOFTWARE_RESET) & SDHCI_RESET_CMD) &&
		       (ml_get_timebase() - t < 10000ULL * 192ULL / 10ULL)) {
			__asm__ volatile("isb");
		}
		return -3;
	}
	if (!(is & SDHCI_INT_RESPONSE)) {
		sdhc_wr8(SDHCI_SOFTWARE_RESET, SDHCI_RESET_CMD);
		uint64_t t = ml_get_timebase();
		while ((sdhc_rd8(SDHCI_SOFTWARE_RESET) & SDHCI_RESET_CMD) &&
		       (ml_get_timebase() - t < 10000ULL * 192ULL / 10ULL)) {
			__asm__ volatile("isb");
		}
		return -4;
	}

	if (resp0) {
		*resp0 = sdhc_rd32(SDHCI_RESPONSE);
	}

	sdhc_wr32(SDHCI_INT_STATUS, SDHCI_INT_RESPONSE);
	return 0;
}

static int sdhc_init_card(void)
{
	uint32_t status = 0;

	/* Verify if card is already in TRAN state (RCA = 1) */
	int ret = sdhc_send_cmd(13, 0x00010000,
	    SDHCI_CMD_INDEX_CHECK | SDHCI_CMD_CRC_CHECK | SDHCI_CMD_RESP_SHORT,
	    &status);
	if (ret == 0 && (((status >> 9) & 0xF) == 4)) {
		return 0;
	}

	sdhc_wr8(SDHCI_SOFTWARE_RESET, SDHCI_RESET_CMD | SDHCI_RESET_DATA);
	delay(1000);
	sdhc_wr16(SDHCI_HOST_CONTROL2, 0);
	sdhc_wr8(SDHCI_HOST_CONTROL, 0);
	sdhc_set_clock(400000);

	/* CMD0: Pre-Idle and Idle reset */
	sdhc_send_cmd(0, 0xF0F0F0F0, SDHCI_CMD_RESP_NONE, 0);
	delay(5000);
	sdhc_send_cmd(0, 0x00000000, SDHCI_CMD_RESP_NONE, 0);
	delay(10000);

	/* CMD1: SEND_OP_COND */
	uint32_t ocr = 0;
	ret = sdhc_send_cmd(1, 0x00000000, SDHCI_CMD_RESP_SHORT, &ocr);
	if (ret != 0) return -1;

	int ocr_ready = 0;
	for (int i = 0; i < 100; i++) {
		ret = sdhc_send_cmd(1, 0x40FF8080, SDHCI_CMD_RESP_SHORT, &ocr);
		if (ret == 0 && (ocr & 0x80000000)) {
			ocr_ready = 1;
			break;
		}
		delay(10000);
	}
	if (!ocr_ready) return -2;

	/* CMD2: ALL_SEND_CID */
	ret = sdhc_send_cmd(2, 0, SDHCI_CMD_RESP_LONG | SDHCI_CMD_CRC_CHECK, 0);
	if (ret != 0) return -3;

	/* CMD3: SET_RELATIVE_ADDR */
	ret = sdhc_send_cmd(3, 0x00010000,
	    SDHCI_CMD_INDEX_CHECK | SDHCI_CMD_CRC_CHECK | SDHCI_CMD_RESP_SHORT,
	    &status);
	if (ret != 0) return -4;

	/* CMD7: SELECT_CARD */
	ret = sdhc_send_cmd(7, 0x00010000,
	    SDHCI_CMD_INDEX_CHECK | SDHCI_CMD_CRC_CHECK | SDHCI_CMD_RESP_SHORT_BUSY,
	    &status);
	if (ret != 0) return -5;

	/* CMD6: SWITCH to 8-bit bus */
	sdhc_send_cmd(6, 0x03B70200,
	    SDHCI_CMD_INDEX_CHECK | SDHCI_CMD_CRC_CHECK | SDHCI_CMD_RESP_SHORT_BUSY,
	    &status);
	sdhc_wr8(SDHCI_HOST_CONTROL, SDHCI_CTRL_8BITBUS);
	delay(1000);

	/* Switch to 25 MHz */
	sdhc_set_clock(25000000);

	/* CMD16: SET_BLOCKLEN to 512 */
	sdhc_send_cmd(16, 512,
	    SDHCI_CMD_INDEX_CHECK | SDHCI_CMD_CRC_CHECK | SDHCI_CMD_RESP_SHORT,
	    &status);

	return 0;
}

static int sdhc_read_single_block(uint32_t lba, uint8_t *buf)
{
	uint64_t start = ml_get_timebase();
	uint64_t ticks = 50000ULL * 192ULL / 10ULL;

	while (sdhc_rd32(SDHCI_PRESENT_STATE) & (SDHCI_CMD_INHIBIT | SDHCI_DATA_INHIBIT)) {
		if (ml_get_timebase() - start >= ticks) {
			sdhc_wr8(SDHCI_SOFTWARE_RESET, SDHCI_RESET_CMD | SDHCI_RESET_DATA);
			return -1;
		}
		__asm__ volatile("isb");
	}

	sdhc_wr32(SDHCI_INT_STATUS, 0xFFFFFFFF);
	sdhc_wr16(SDHCI_BLOCK_SIZE, 512);
	sdhc_wr16(SDHCI_BLOCK_COUNT, 1);
	sdhc_wr32(SDHCI_ARGUMENT, lba);
	sdhc_wr16(SDHCI_TRANSFER_MODE, SDHCI_TRNS_READ | SDHCI_TRNS_BLK_CNT_EN);

	uint16_t cmd_flags = SDHCI_CMD_DATA_PRESENT | SDHCI_CMD_INDEX_CHECK | SDHCI_CMD_CRC_CHECK | SDHCI_CMD_RESP_SHORT;
	sdhc_wr16(SDHCI_COMMAND, (17 << 8) | cmd_flags);

	start = ml_get_timebase();
	ticks = 100000ULL * 192ULL / 10ULL;
	uint32_t is = 0;
	while (ml_get_timebase() - start < ticks) {
		is = sdhc_rd32(SDHCI_INT_STATUS);
		if (is & (SDHCI_INT_RESPONSE | SDHCI_INT_ERROR))
			break;
		__asm__ volatile("isb");
	}
	if ((is & SDHCI_INT_ERROR) || !(is & SDHCI_INT_RESPONSE)) {
		sdhc_wr32(SDHCI_INT_STATUS, is);
		sdhc_wr8(SDHCI_SOFTWARE_RESET, SDHCI_RESET_CMD | SDHCI_RESET_DATA);
		return -2;
	}
	sdhc_wr32(SDHCI_INT_STATUS, SDHCI_INT_RESPONSE);

	start = ml_get_timebase();
	ticks = 200000ULL * 192ULL / 10ULL;
	int data_ready = 0;
	while (ml_get_timebase() - start < ticks) {
		is = sdhc_rd32(SDHCI_INT_STATUS);
		if (is & SDHCI_INT_ERROR)
			break;
		if ((is & SDHCI_INT_DATA_AVAIL) || (sdhc_rd32(SDHCI_PRESENT_STATE) & SDHCI_BUFFER_READ_EN)) {
			data_ready = 1;
			break;
		}
		__asm__ volatile("isb");
	}
	if (!data_ready || (is & SDHCI_INT_ERROR)) {
		sdhc_wr8(SDHCI_SOFTWARE_RESET, SDHCI_RESET_CMD | SDHCI_RESET_DATA);
		return -3;
	}
	sdhc_wr32(SDHCI_INT_STATUS, SDHCI_INT_DATA_AVAIL);

	uint32_t *p = (uint32_t *)(void *)buf;
	for (int i = 0; i < 128; i++) {
		p[i] = sdhc_rd32(SDHCI_BUFFER);
	}

	start = ml_get_timebase();
	ticks = 100000ULL * 192ULL / 10ULL;
	while (ml_get_timebase() - start < ticks) {
		is = sdhc_rd32(SDHCI_INT_STATUS);
		if (is & (SDHCI_INT_DATA_END | SDHCI_INT_ERROR))
			break;
		__asm__ volatile("isb");
	}
	if (is & SDHCI_INT_ERROR) {
		sdhc_wr32(SDHCI_INT_STATUS, is);
		sdhc_wr8(SDHCI_SOFTWARE_RESET, SDHCI_RESET_CMD | SDHCI_RESET_DATA);
		return -4;
	}
	sdhc_wr32(SDHCI_INT_STATUS, 0xFFFFFFFF);
	return 0;
}

static int op2_emmc_find_partition(const char *want)
{
	int ret = sdhc_read_single_block(1, op2_sector_buf);
	if (ret != 0) {
		printf("OP2 eMMC: failed to read primary GPT header (LBA 1)\n");
		return ENXIO;
	}

	struct gpt_header *hdr = (struct gpt_header *)(void *)op2_sector_buf;
	if (hdr->signature != 0x5452415020494645ULL) {
		printf("OP2 eMMC: invalid GPT signature 0x%llx\n", hdr->signature);
		return ENXIO;
	}

	uint64_t part_lba = hdr->partition_entry_lba;
	uint32_t num_parts = hdr->num_partition_entries;
	uint32_t part_sz = hdr->sizeof_partition_entry;
	if (part_sz != 128 || num_parts == 0) {
		printf("OP2 eMMC: unsupported partition entries count %u size %u\n", num_parts, part_sz);
		return ENXIO;
	}
	if (num_parts > 64) num_parts = 64;

	uint64_t cur_sec = 0xFFFFFFFFFFFFFFFFULL;
	for (uint32_t i = 0; i < num_parts; i++) {
		uint64_t lba = part_lba + (i * part_sz / 512);
		uint32_t off = (i * part_sz) % 512;
		if (lba != cur_sec) {
			if (sdhc_read_single_block((uint32_t)lba, op2_sector_buf) != 0) {
				return ENXIO;
			}
			cur_sec = lba;
		}

		struct gpt_entry *entry = (struct gpt_entry *)(void *)(op2_sector_buf + off);
		int non_zero = 0;
		for (int g = 0; g < 16; g++) {
			if (entry->type_guid[g]) { non_zero = 1; break; }
		}
		if (!non_zero) continue;

		char name[37];
		unsigned c;
		for (c = 0; c < 36; c++) {
			uint16_t ch = entry->partition_name[c];
			if (ch == 0 || ch > 0x7f) break;
			name[c] = (char)ch;
		}
		name[c] = '\0';

		if (strcmp(name, want) == 0) {
			uint64_t first = entry->starting_lba;
			uint64_t last = entry->ending_lba;
			if (last < first) return ENXIO;
			op2_emmc.first_block = first;
			op2_emmc.nblocks = last - first + 1;
			printf("OP2 eMMC: partition \"%s\" found: start LBA %llu, count %llu blocks (%llu MB)\n",
			       want, op2_emmc.first_block, op2_emmc.nblocks, (op2_emmc.nblocks * 512) / (1024 * 1024));
			return 0;
		}
	}

	printf("OP2 eMMC: no GPT partition named \"%s\"\n", want);
	return ENOENT;
}

static int op2_emmc_open(__unused dev_t dev, int flags, __unused int devtype, __unused struct proc *p)
{
	if (!op2_emmc.ready) {
		return ENXIO;
	}
	return (flags & FWRITE) ? EROFS : 0;
}

static int op2_emmc_close(__unused dev_t dev, __unused int flags, __unused int devtype, __unused struct proc *p)
{
	return 0;
}

static void op2_emmc_strategy(buf_t bp)
{
	int error = 0;
	caddr_t vaddr = NULL;
	uint64_t off = (uint64_t)buf_blkno(bp) * op2_emmc.secsize;
	uint32_t count = buf_count(bp);
	uint64_t part_bytes = op2_emmc.nblocks * op2_emmc.secsize;

	buf_setresid(bp, count);
	if (!(buf_flags(bp) & B_READ)) {
		error = EROFS;
		goto done;
	}
	if (off > part_bytes || count > part_bytes - off) {
		error = EINVAL;
		goto done;
	}
	if (count == 0) {
		goto done;
	}
	if (buf_map(bp, &vaddr)) {
		error = EIO;
		goto done;
	}

	if (op2_emmc_lock) lck_mtx_lock(op2_emmc_lock);
	for (uint32_t done_bytes = 0; done_bytes < count; done_bytes += EMMC_SECTOR_SIZE) {
		uint64_t lba = op2_emmc.first_block + ((off + done_bytes) / EMMC_SECTOR_SIZE);
		error = sdhc_read_single_block((uint32_t)lba, op2_sector_buf);
		if (error) {
			error = EIO;
			break;
		}
		uint32_t chunk = MIN(EMMC_SECTOR_SIZE, count - done_bytes);
		memcpy(vaddr + done_bytes, op2_sector_buf, chunk);
		buf_setresid(bp, count - (done_bytes + chunk));
	}
	if (op2_emmc_lock) lck_mtx_unlock(op2_emmc_lock);
	buf_unmap(bp);

done:
	if (error) {
		buf_seterror(bp, error);
	}
	buf_biodone(bp);
}

static int op2_emmc_ioctl(__unused dev_t dev, u_long cmd, caddr_t data, __unused int flag,
    __unused struct proc *p, int is_char)
{
	uint32_t *f = (uint32_t *)(void *)data;
	uint64_t *o = (uint64_t *)(void *)data;

	switch (cmd) {
	case DKIOCGETBLOCKSIZE:
		*f = op2_emmc.secsize;
		return 0;
	case DKIOCSETBLOCKSIZE:
		if (is_char) return ENODEV;
		if (*f != EMMC_SECTOR_SIZE) return EINVAL;
		return 0;
	case DKIOCGETPHYSICALBLOCKSIZE:
		*f = EMMC_SECTOR_SIZE;
		return 0;
	case DKIOCGETBLOCKCOUNT:
		*o = op2_emmc.nblocks;
		return 0;
	case DKIOCISWRITABLE:
		*f = 0;
		return 0;
	case DKIOCGETMAXBLOCKCOUNTREAD:
		*o = 2048; /* 1 MB */
		return 0;
	case DKIOCGETMAXBLOCKCOUNTWRITE:
		*o = 0;
		return 0;
	case DKIOCGETMAXSEGMENTCOUNTREAD:
	case DKIOCGETMAXSEGMENTCOUNTWRITE:
		*o = 1;
		return 0;
	default:
		return ENOTTY;
	}
}

static int op2_emmc_bioctl(dev_t dev, u_long cmd, caddr_t data, int flag, struct proc *p)
{
	return op2_emmc_ioctl(dev, cmd, data, flag, p, 0);
}

static int op2_emmc_cioctl(dev_t dev, u_long cmd, caddr_t data, int flag, struct proc *p)
{
	return op2_emmc_ioctl(dev, cmd, data, flag, p, 1);
}

static int op2_emmc_psize(__unused dev_t dev)
{
	return op2_emmc.ready ? (int)MIN(op2_emmc.nblocks, INT_MAX) : -1;
}

static int op2_emmc_read(dev_t dev, struct uio *uio, __unused int ioflag)
{
	return physio(op2_emmc_strategy, NULL, dev, B_READ, minphys, uio, (int)op2_emmc.secsize);
}

static int op2_emmc_write(__unused dev_t dev, __unused struct uio *uio, __unused int ioflag)
{
	return EROFS;
}

static struct bdevsw op2_emmc_bdevsw = {
	.d_open     = op2_emmc_open,
	.d_close    = op2_emmc_close,
	.d_strategy = op2_emmc_strategy,
	.d_ioctl    = op2_emmc_bioctl,
	.d_dump     = eno_dump,
	.d_psize    = op2_emmc_psize,
	.d_type     = D_DISK,
};

static struct cdevsw op2_emmc_cdevsw = {
	.d_open       = op2_emmc_open,
	.d_close      = op2_emmc_close,
	.d_read       = op2_emmc_read,
	.d_write      = op2_emmc_write,
	.d_ioctl      = op2_emmc_cioctl,
	.d_stop       = eno_stop,
	.d_reset      = eno_reset,
	.d_ttys       = NULL,
	.d_select     = eno_select,
	.d_mmap       = eno_mmap,
	.d_strategy   = eno_strat,
	.d_reserved_1 = eno_getc,
	.d_reserved_2 = eno_putc,
	.d_type       = D_DISK,
};

static int op2_emmc_init(void)
{
	char want[64] = "userdata";
	PE_parse_boot_argn("emmcroot", want, sizeof(want));

	op2_emmc.regs = ml_io_map(SDHC_REG_BASE, 0x200);
	op2_emmc.core_regs = ml_io_map(SDHC_CORE_BASE, 0x800);
	op2_emmc.secsize = EMMC_SECTOR_SIZE;

	if (!op2_emmc.regs || !op2_emmc.core_regs) {
		printf("OP2 eMMC: failed to map MMIO registers\n");
		return ENXIO;
	}

	if (!op2_emmc_grp) {
		op2_emmc_grp = lck_grp_alloc_init("op2_emmc", LCK_GRP_ATTR_NULL);
		if (op2_emmc_grp) {
			op2_emmc_lock = lck_mtx_alloc_init(op2_emmc_grp, LCK_ATTR_NULL);
		}
	}

	if (op2_emmc_lock) lck_mtx_lock(op2_emmc_lock);
	int error = sdhc_init_card();
	if (error == 0) {
		error = op2_emmc_find_partition(want);
	}
	if (op2_emmc_lock) lck_mtx_unlock(op2_emmc_lock);
	if (error) {
		return error;
	}

	op2_emmc.bmajor = bdevsw_add(-1, &op2_emmc_bdevsw);
	if (op2_emmc.bmajor < 0) {
		printf("OP2 eMMC: bdevsw_add failed\n");
		return ENXIO;
	}
	op2_emmc.cmajor = cdevsw_add_with_bdev(-1, &op2_emmc_cdevsw, op2_emmc.bmajor);
	if (op2_emmc.cmajor < 0) {
		printf("OP2 eMMC: cdevsw_add failed\n");
		bdevsw_remove(op2_emmc.bmajor, &op2_emmc_bdevsw);
		return ENXIO;
	}

	op2_emmc.bdev = makedev(op2_emmc.bmajor, 0);
	devfs_make_node(op2_emmc.bdev, DEVFS_BLOCK, UID_ROOT, GID_OPERATOR, 0640, "emmcroot0");
	devfs_make_node(makedev(op2_emmc.cmajor, 0), DEVFS_CHAR, UID_ROOT, GID_OPERATOR, 0640, "remmcroot0");

	op2_emmc.ready = true;
	printf("OP2 eMMC: registered /dev/emmcroot0 (bmajor=%d, cmajor=%d) for \"%s\" (%llu blocks)\n",
	       op2_emmc.bmajor, op2_emmc.cmajor, want, op2_emmc.nblocks);
	return 0;
}

int op2_emmc_rootdev(dev_t *dev)
{
	int error = 0;
	if (!op2_emmc.ready) {
		error = op2_emmc_init();
	}
	if (!error) {
		*dev = op2_emmc.bdev;
	}
	return error;
}

#endif /* ARM64_BOARD_CONFIG_MSM8994 */
