/*
 * Qualcomm RPM (Resource Power Manager) Client for OnePlus 2 (MSM8994).
 *
 * Communicates with the RPM Cortex-M3 co-processor via Shared Memory (SMEM)
 * and SMD (Shared Memory Driver) edge 15 ("rpm_requests") using APCS IPC interrupts.
 *
 * Author: Veer Hooda / Antigravity pair programming
 */

#if defined(ARM64_BOARD_CONFIG_MSM8994)

#include <sys/param.h>
#include <sys/systm.h>
#include <kern/locks.h>
#include <kern/clock.h>
#include <pexpert/pexpert.h>
#include <libkern/libkern.h>
#include "op2_rpm.h"

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);

#define OP2_SMEM_PHYS_BASE        0x06a00000UL
#define OP2_SMEM_SIZE             0x00200000UL  /* 2 MB */
#define OP2_RPM_MSGRAM_PHYS_BASE  0xfc428000UL
#define OP2_RPM_MSGRAM_SIZE       0x00004000UL  /* 16 KB */
#define OP2_APCS_IPC_PHYS_BASE    0xf900d000UL
#define OP2_APCS_IPC_SIZE         0x00001000UL  /* 4 KB */
#define OP2_APCS_IPC_DOORBELL     0x008         /* offset +0x8: write bit 0 to kick RPM */

#define SMEM_CHANNEL_ALLOC_TBL    13U
#define SMEM_SMD_BASE_ID          14U
#define SMEM_SMD_FIFO_BASE_ID     338U

#define SMD_SS_CLOSED             0U
#define SMD_SS_OPENING            1U
#define SMD_SS_OPENED             2U
#define SMD_SS_FLUSHING           3U
#define SMD_SS_CLOSING            4U
#define SMD_SS_RESET              5U

#define SMD_HEADER_SIZE           20U           /* 5 dwords prepended to packet */
#define RPM_SERVICE_REQUEST       0x00716572U   /* "req\0" */
#define RPM_SERVICE_ERROR         0x00727265U   /* "err\0" */

struct smem_heap_info {
	uint32_t initialized;
	uint32_t free_offset;
	uint32_t heap_remaining;
	uint32_t reserved;
};

struct smem_heap_entry {
	uint32_t allocated;
	uint32_t offset;
	uint32_t size;
	uint32_t reserved; /* bits 31:2 aux smem base addr */
};

struct smem_proc_comm {
	uint32_t command;
	uint32_t status;
	uint32_t data1;
	uint32_t data2;
};

struct smem_shared {
	struct smem_proc_comm proc_comm[4];
	uint32_t version[32];
	struct smem_heap_info heap_info;
	struct smem_heap_entry heap_toc[512];
};

struct smd_alloc_elm {
	char name[20];
	uint32_t cid;
	uint32_t type;
	uint32_t ref_count;
};

struct smd_half_channel_word_access {
	uint32_t state;
	uint32_t fDSR;
	uint32_t fCTS;
	uint32_t fCD;
	uint32_t fRI;
	uint32_t fHEAD;
	uint32_t fTAIL;
	uint32_t fSTATE;
	uint32_t fBLOCKREADINTR;
	uint32_t tail;
	uint32_t head;
};

struct smd_shared_word_access {
	struct smd_half_channel_word_access ch0; /* APPS -> RPM */
	struct smd_half_channel_word_access ch1; /* RPM -> APPS */
};

struct op2_rpm_ack_msg {
	uint32_t req;
	uint32_t req_len;
	uint32_t rsc_id;
	uint32_t msg_len;
	uint32_t id_ack;
};

static struct {
	vm_offset_t smem_va;
	vm_offset_t aux_mem1_va;
	vm_offset_t apcs_ipc_va;
	volatile struct smd_half_channel_word_access *send_ch;  /* ch0 */
	volatile struct smd_half_channel_word_access *recv_ch;  /* ch1 */
	volatile uint32_t *send_fifo;
	volatile uint32_t *recv_fifo;
	uint32_t fifo_size;      /* size of one direction in bytes */
	uint32_t msg_id;
	bool initialized;
	bool ready;
} op2_rpm_dev;

static lck_grp_t *op2_rpm_grp = NULL;
static lck_mtx_t *op2_rpm_lock = NULL;

static void *
op2_smem_get_entry(uint32_t id, uint32_t *size_out)
{
	if (!op2_rpm_dev.smem_va || id >= 512) return NULL;
	struct smem_shared *shared = (struct smem_shared *)op2_rpm_dev.smem_va;
	struct smem_heap_entry *entry = &shared->heap_toc[id];
	if (!entry->allocated) return NULL;

	if (size_out) {
		*size_out = entry->size;
	}

	uint32_t phys_base = entry->reserved & 0xfffffffcU;
	if (phys_base == 0 || phys_base == OP2_SMEM_PHYS_BASE) {
		return (void *)(op2_rpm_dev.smem_va + entry->offset);
	} else if (phys_base == OP2_RPM_MSGRAM_PHYS_BASE && op2_rpm_dev.aux_mem1_va) {
		return (void *)(op2_rpm_dev.aux_mem1_va + entry->offset);
	} else {
		printf("OP2 RPM: unmapped aux SMEM region 0x%08x offset 0x%x\n",
		       phys_base, entry->offset);
		return NULL;
	}
}

static inline void
op2_rpm_kick(void)
{
	__asm__ volatile("dsb sy" ::: "memory");
	if (op2_rpm_dev.apcs_ipc_va) {
		*(volatile uint32_t *)(op2_rpm_dev.apcs_ipc_va + OP2_APCS_IPC_DOORBELL) = 1U;
	}
	__asm__ volatile("dsb sy" ::: "memory");
}

bool
op2_rpm_init(void)
{
	if (op2_rpm_dev.ready) return true;
	if (op2_rpm_dev.initialized) return op2_rpm_dev.ready;

	op2_rpm_dev.initialized = true;
	printf("OP2 RPM: Initializing Qualcomm RPM client...\n");

	if (!op2_rpm_grp) {
		op2_rpm_grp = lck_grp_alloc_init("op2_rpm", LCK_GRP_ATTR_NULL);
		op2_rpm_lock = lck_mtx_alloc_init(op2_rpm_grp, LCK_ATTR_NULL);
	}

	/* Map physical regions */
	op2_rpm_dev.smem_va = ml_io_map(OP2_SMEM_PHYS_BASE, OP2_SMEM_SIZE);
	if (!op2_rpm_dev.smem_va) {
		printf("OP2 RPM: ERROR: failed to map SMEM at 0x%08lx\n", OP2_SMEM_PHYS_BASE);
		return false;
	}

	op2_rpm_dev.aux_mem1_va = ml_io_map(OP2_RPM_MSGRAM_PHYS_BASE, OP2_RPM_MSGRAM_SIZE);
	if (!op2_rpm_dev.aux_mem1_va) {
		printf("OP2 RPM: WARNING: failed to map RPM Message RAM at 0x%08lx\n",
		       OP2_RPM_MSGRAM_PHYS_BASE);
	}

	op2_rpm_dev.apcs_ipc_va = ml_io_map(OP2_APCS_IPC_PHYS_BASE, OP2_APCS_IPC_SIZE);
	if (!op2_rpm_dev.apcs_ipc_va) {
		printf("OP2 RPM: ERROR: failed to map APCS IPC at 0x%08lx\n", OP2_APCS_IPC_PHYS_BASE);
		return false;
	}

	/* Scan SMEM channel allocation table for "rpm_requests" */
	uint32_t tbl_sz = 0;
	struct smd_alloc_elm *tbl = (struct smd_alloc_elm *)op2_smem_get_entry(SMEM_CHANNEL_ALLOC_TBL, &tbl_sz);
	if (!tbl) {
		printf("OP2 RPM: ERROR: SMEM channel allocation table not found!\n");
		return false;
	}

	uint32_t n_entries = tbl_sz / sizeof(struct smd_alloc_elm);
	int rpm_cid = -1;
	for (uint32_t i = 0; i < n_entries; i++) {
		if (strncmp(tbl[i].name, "rpm_requests", 12) == 0 && (tbl[i].type & 0xff) == 15) {
			rpm_cid = (int)tbl[i].cid;
			printf("OP2 RPM: Found channel 'rpm_requests' (cid=%u, type=0x%08x)\n",
			       tbl[i].cid, tbl[i].type);
			break;
		}
	}

	if (rpm_cid < 0) {
		printf("OP2 RPM: ERROR: 'rpm_requests' channel not present in SMEM table\n");
		return false;
	}

	/* Resolve shared half-channels and FIFO buffer */
	uint32_t ch_sz = 0;
	struct smd_shared_word_access *shared_ch =
		(struct smd_shared_word_access *)op2_smem_get_entry(SMEM_SMD_BASE_ID + rpm_cid, &ch_sz);
	if (!shared_ch) {
		printf("OP2 RPM: ERROR: failed to find shared channel for cid=%d\n", rpm_cid);
		return false;
	}

	uint32_t fifo_sz = 0;
	void *fifo_buf = op2_smem_get_entry(SMEM_SMD_FIFO_BASE_ID + rpm_cid, &fifo_sz);
	if (!fifo_buf || fifo_sz < 64) {
		printf("OP2 RPM: ERROR: failed to find FIFO buffer for cid=%d\n", rpm_cid);
		return false;
	}

	op2_rpm_dev.send_ch = &shared_ch->ch0;
	op2_rpm_dev.recv_ch = &shared_ch->ch1;
	op2_rpm_dev.fifo_size = fifo_sz / 2;
	op2_rpm_dev.send_fifo = (volatile uint32_t *)fifo_buf;
	op2_rpm_dev.recv_fifo = (volatile uint32_t *)((uintptr_t)fifo_buf + op2_rpm_dev.fifo_size);
	op2_rpm_dev.msg_id = 2; /* 0=err, 1=unsent, valid IDs start at 2 */

	printf("OP2 RPM: Channel mapped: send_ch=%p recv_ch=%p fifo_half=%u bytes\n",
	       op2_rpm_dev.send_ch, op2_rpm_dev.recv_ch, op2_rpm_dev.fifo_size);
	printf("OP2 RPM: Channel state: APPS state=%u head=%u tail=%u; RPM state=%u head=%u tail=%u\n",
	       op2_rpm_dev.send_ch->state, op2_rpm_dev.send_ch->head, op2_rpm_dev.send_ch->tail,
	       op2_rpm_dev.recv_ch->state, op2_rpm_dev.recv_ch->head, op2_rpm_dev.recv_ch->tail);

	/* Check if channel requires SMD open handshake */
	if (op2_rpm_dev.send_ch->state != SMD_SS_OPENED ||
	    op2_rpm_dev.recv_ch->state != SMD_SS_OPENED) {
		printf("OP2 RPM: Performing SMD open handshake...\n");

		/* Reset pointers if starting fresh from CLOSED or CLOSING */
		op2_rpm_dev.send_ch->head = 0;
		op2_rpm_dev.send_ch->tail = 0;
		op2_rpm_dev.send_ch->fBLOCKREADINTR = 0;

		/* Step 1: Request OPENING */
		op2_rpm_dev.send_ch->fDSR = 0;
		op2_rpm_dev.send_ch->fCTS = 0;
		op2_rpm_dev.send_ch->fCD = 0;
		op2_rpm_dev.send_ch->state = SMD_SS_OPENING;
		__asm__ volatile("dsb sy" ::: "memory");
		op2_rpm_dev.send_ch->fSTATE = 1;
		op2_rpm_kick();

		/* Wait for RPM side to respond */
		for (int i = 0; i < 2000; i++) {
			if (op2_rpm_dev.recv_ch->state == SMD_SS_OPENED ||
			    op2_rpm_dev.recv_ch->state == SMD_SS_OPENING) {
				break;
			}
			delay(10);
		}

		/* Step 2: Set APPS side to OPENED */
		op2_rpm_dev.send_ch->fDSR = 1;
		op2_rpm_dev.send_ch->fCTS = 1;
		op2_rpm_dev.send_ch->fCD = 1;
		op2_rpm_dev.send_ch->state = SMD_SS_OPENED;
		__asm__ volatile("dsb sy" ::: "memory");
		op2_rpm_dev.send_ch->fSTATE = 1;
		op2_rpm_kick();

		/* Wait for RPM side to be fully OPENED */
		for (int i = 0; i < 2000; i++) {
			if (op2_rpm_dev.recv_ch->state == SMD_SS_OPENED) {
				break;
			}
			delay(10);
		}

		printf("OP2 RPM: Handshake result: APPS state=%u head=%u tail=%u; RPM state=%u head=%u tail=%u\n",
		       op2_rpm_dev.send_ch->state, op2_rpm_dev.send_ch->head, op2_rpm_dev.send_ch->tail,
		       op2_rpm_dev.recv_ch->state, op2_rpm_dev.recv_ch->head, op2_rpm_dev.recv_ch->tail);
	}

	/* Purge any stale unread packets left in FIFO by bootloader/TWRP */
	if (op2_rpm_dev.recv_ch->tail != op2_rpm_dev.recv_ch->head) {
		printf("OP2 RPM: Synchronizing RX FIFO (tail %u -> %u)\n",
		       op2_rpm_dev.recv_ch->tail, op2_rpm_dev.recv_ch->head);
		op2_rpm_dev.recv_ch->tail = op2_rpm_dev.recv_ch->head;
		__asm__ volatile("dsb sy" ::: "memory");
		op2_rpm_dev.send_ch->fTAIL = 1U;
		op2_rpm_kick();
	}

	op2_rpm_dev.ready = true;
	printf("OP2 RPM: Client ready for requests!\n");
	return true;
}

bool
op2_rpm_request(uint32_t set, uint32_t rsc_type, uint32_t rsc_id,
                const uint32_t *keys, const uint32_t *values, uint32_t nkvps)
{
	if (!op2_rpm_init()) return false;

	lck_mtx_lock(op2_rpm_lock);

	uint32_t cur_msg = op2_rpm_dev.msg_id++;
	if (op2_rpm_dev.msg_id < 2) op2_rpm_dev.msg_id = 2;

	/*
	 * Packet Format:
	 * 1. SMD Packet Header: 5 words = 20 bytes
	 *    [0]: payload length in bytes (28 + 12 * nkvps)
	 *    [1..4]: 0
	 * 2. RPM Message Header: 7 words = 28 bytes
	 *    [0]: service_type (0x00716572 "req\0")
	 *    [1]: request_len (20 + 12 * nkvps)
	 *    [2]: msg_id
	 *    [3]: set (0 = active, 1 = sleep)
	 *    [4]: resource_type (e.g. 0x62706d73 "smpb")
	 *    [5]: resource_id (e.g. 2)
	 *    [6]: data_len (12 * nkvps)
	 * 3. KVPs: 3 words per KVP = 12 * nkvps bytes
	 *    [0]: key
	 *    [1]: 4 (size)
	 *    [2]: value
	 */
	uint32_t rpm_hdr_sz = 28U;
	uint32_t kvp_sz = 12U * nkvps;
	uint32_t payload_len = rpm_hdr_sz + kvp_sz;
	uint32_t total_write = SMD_HEADER_SIZE + payload_len;
	uint32_t total_words = total_write / 4U;

	/* Assemble contiguous word buffer */
	uint32_t packet[32];
	if (total_words > 32) {
		lck_mtx_unlock(op2_rpm_lock);
		return false;
	}

	/* SMD Header */
	packet[0] = payload_len;
	packet[1] = 0;
	packet[2] = 0;
	packet[3] = 0;
	packet[4] = 0;

	/* RPM Request Header */
	packet[5] = RPM_SERVICE_REQUEST;
	packet[6] = 20U + kvp_sz;
	packet[7] = cur_msg;
	packet[8] = set;
	packet[9] = rsc_type;
	packet[10] = rsc_id;
	packet[11] = kvp_sz;

	/* KVPs */
	for (uint32_t i = 0; i < nkvps; i++) {
		packet[12 + i * 3 + 0] = keys[i];
		packet[12 + i * 3 + 1] = 4U;
		packet[12 + i * 3 + 2] = values[i];
	}

	/* Check available FIFO space */
	uint32_t head = op2_rpm_dev.send_ch->head;
	uint32_t tail = op2_rpm_dev.send_ch->tail;
	uint32_t fifo_half = op2_rpm_dev.fifo_size;
	uint32_t avail = (head < tail) ? (tail - head - 4U) : (fifo_half + tail - head - 4U);

	printf("OP2 RPM: Sending msg %u (set=%u, rsc=0x%08x, id=%u, %u KVPs): head=%u tail=%u avail=%u\n",
	       cur_msg, set, rsc_type, rsc_id, nkvps, head, tail, avail);

	if (avail < total_write) {
		printf("OP2 RPM: ERROR: send FIFO full (avail=%u needed=%u)\n", avail, total_write);
		lck_mtx_unlock(op2_rpm_lock);
		return false;
	}

	/* Copy packet into circular send FIFO (word-aligned) */
	uint32_t word_pos = head / 4U;
	uint32_t fifo_words = fifo_half / 4U;
	for (uint32_t i = 0; i < total_words; i++) {
		op2_rpm_dev.send_fifo[word_pos] = packet[i];
		word_pos = (word_pos + 1U) % fifo_words;
	}

	/* Update head and kick RPM */
	__asm__ volatile("dsb sy" ::: "memory");
	op2_rpm_dev.send_ch->head = word_pos * 4U;
	__asm__ volatile("dsb sy" ::: "memory");
	op2_rpm_dev.send_ch->fHEAD = 1U;
	op2_rpm_kick();

	/* Poll for ACK from RPM */
	bool acked = false;
	for (int iter = 0; iter < 10000; iter++) {
		uint32_t rx_head = op2_rpm_dev.recv_ch->head;
		uint32_t rx_tail = op2_rpm_dev.recv_ch->tail;

		if (rx_head != rx_tail) {
			/* Read SMD header */
			uint32_t rx_word_pos = rx_tail / 4U;
			uint32_t rx_payload_bytes = op2_rpm_dev.recv_fifo[rx_word_pos];

			printf("OP2 RPM: RX packet: rx_head=%u rx_tail=%u payload_bytes=%u (word_pos=%u)\n",
			       rx_head, rx_tail, rx_payload_bytes, rx_word_pos);

			/* Validate payload_bytes */
			if (rx_payload_bytes == 0 || rx_payload_bytes > 512) {
				printf("OP2 RPM: WARNING: invalid rx_payload_bytes=%u, resetting tail to head\n",
				       rx_payload_bytes);
				op2_rpm_dev.recv_ch->tail = rx_head;
				__asm__ volatile("dsb sy" ::: "memory");
				op2_rpm_dev.send_ch->fTAIL = 1U;
				op2_rpm_kick();
				break;
			}

			rx_word_pos = (rx_word_pos + (SMD_HEADER_SIZE / 4U)) % fifo_words;

			/* Read ACK message (at least 20 bytes = 5 words) */
			struct op2_rpm_ack_msg ack;
			ack.req = op2_rpm_dev.recv_fifo[rx_word_pos];
			ack.req_len = op2_rpm_dev.recv_fifo[(rx_word_pos + 1) % fifo_words];
			ack.rsc_id = op2_rpm_dev.recv_fifo[(rx_word_pos + 2) % fifo_words];
			ack.msg_len = op2_rpm_dev.recv_fifo[(rx_word_pos + 3) % fifo_words];
			ack.id_ack = op2_rpm_dev.recv_fifo[(rx_word_pos + 4) % fifo_words];

			printf("OP2 RPM: Received ACK: req=0x%08x req_len=%u rsc_id=%u msg_len=%u id_ack=%u (cur_msg=%u)\n",
			       ack.req, ack.req_len, ack.rsc_id, ack.msg_len, ack.id_ack, cur_msg);

			/* Advance tail past entire received packet */
			uint32_t total_rx = (SMD_HEADER_SIZE + rx_payload_bytes + 3U) & ~3U;
			op2_rpm_dev.recv_ch->tail = (rx_tail + total_rx) % fifo_half;
			__asm__ volatile("dsb sy" ::: "memory");
			op2_rpm_dev.send_ch->fTAIL = 1U;
			op2_rpm_kick();

			if (ack.id_ack == cur_msg) {
				acked = true;
				break;
			}
		}
		delay(10);
	}

	lck_mtx_unlock(op2_rpm_lock);

	if (!acked) {
		printf("OP2 RPM: ERROR: request timed out waiting for ACK (msg_id=%u, rsc=0x%08x id=%u)\n",
		       cur_msg, rsc_type, rsc_id);
		return false;
	}

	return true;
}

bool
op2_rpm_set_enable(uint32_t rsc_type, uint32_t rsc_id, bool enable)
{
	uint32_t key = OP2_RPM_KEY_SWEN;
	uint32_t val = enable ? 1U : 0U;
	return op2_rpm_request(OP2_RPM_SET_ACTIVE, rsc_type, rsc_id, &key, &val, 1U);
}

bool
op2_rpm_set_corner(uint32_t rsc_type, uint32_t rsc_id, uint32_t corner)
{
	uint32_t key = OP2_RPM_KEY_CORN;
	uint32_t val = corner;
	return op2_rpm_request(OP2_RPM_SET_ACTIVE, rsc_type, rsc_id, &key, &val, 1U);
}

bool
op2_rpm_set_voltage(uint32_t rsc_type, uint32_t rsc_id, uint32_t uv)
{
	uint32_t key = OP2_RPM_KEY_UV;
	uint32_t val = uv;
	return op2_rpm_request(OP2_RPM_SET_ACTIVE, rsc_type, rsc_id, &key, &val, 1U);
}

bool
op2_rpm_power_on_gfx(void)
{
	/*
	 * PMI8994 S2 + S3 = 2-phase VDD_GFX supply for Adreno 430 GPU.
	 * Resource: "smpb" (0x62706d73), Resource ID: 2.
	 *
	 * Send in Active Set first (set 0) to power the rail for execution:
	 * - "swen" = 1 (software enable)
	 * - "corn" = 2 (SVS SOC operating corner: ~0.900V - 1.000V)
	 */
	uint32_t keys[2] = { OP2_RPM_KEY_SWEN, OP2_RPM_KEY_CORN };
	uint32_t vals[2] = { 1U, OP2_RPM_CORNER_SVS_SOC };

	printf("OP2 RPM: Voting on PMI8994 S2 (VDD_GFX) in active set...\n");
	bool act_ok = op2_rpm_request(OP2_RPM_SET_ACTIVE, OP2_RPM_RSC_SMPB, 2U, keys, vals, 2U);

	printf("OP2 RPM: Voting on PMI8994 S2 (VDD_GFX) in sleep set...\n");
	bool slp_ok = op2_rpm_request(OP2_RPM_SET_SLEEP, OP2_RPM_RSC_SMPB, 2U, keys, vals, 2U);

	printf("OP2 RPM: VDD_GFX vote result: active=%s, sleep=%s\n",
	       act_ok ? "ACK" : "FAIL", slp_ok ? "ACK" : "FAIL");

	return act_ok;
}

#endif /* ARM64_BOARD_CONFIG_MSM8994 */
