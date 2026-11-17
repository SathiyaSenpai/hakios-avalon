/*
 * Bluetooth of OnePlus Nord 4 (avalon, SM7675 Cliffs) and POCO X5 Pro (redwood, SM7325):
 * WCN6750 Bluetooth over QUPv3 GENI HSUART. Nothing here is touched unless boot-arg bt=N asks.
 *
 * Hardware layout on SM7675 Cliffs (dtbo_1.dts bt_wcn6750, vb_1.dts qupv3_se10_4uart / hsuart0):
 *   TLMM (0x0f100000):
 *     qcom,wl-reset-gpio    GPIO 35   (redwood: 84)
 *     qcom,bt-reset-gpio    GPIO 156  (redwood: 85; bt_en_sleep)
 *     qcom,bt-sw-ctrl-gpio  GPIO 80   (redwood: 86; mpm_wake_set_gpios)
 *     hsuart0 CTS/RTS/TX/RX GPIO 40, 41, 42, 43 (func 1: qup1_se2_l0..l3)
 *   Supplies (dtbo_1.dts bt_wcn6750 -> SPMI PMIC Arbiter v7 + RPMh cmd-db @ 0x81c60000):
 *     L7B  ("ldob7", slave 1 0xc700)  bt-vdd-io                  1800 mV
 *     S1B  ("smpb1", slave 1 0x9b00)  bt-vdd-rfa1                1900 mV
 *     S2B  ("smpb2", slave 1 0x9e00)  bt-vdd-rfa2                1352 mV
 *     S3B  ("smpb3", slave 1 0xa100)  bt-vdd-aon/dig/rfacmn/0p8   968 mV
 *     S3E  ("smpe3", slave 4 0xa100)  bt-vdd-ipa-2p2             2200 mV
 *   Clocks (gcc-cliffs.ko) & GENI UART (vb_1.dts hsuart0 @ 0x00888000, qupv3_1 SE2):
 *     GCC_VOTE_WRAP1 (0x00152010): bits 0,1,2,3 (wrap1 core/2x/m_ahb/s_ahb), bit 6 (wrap1_s2)
 *     GCC_SE10_CBCR  (0x0011e274), GCC_SE10_RCGR (0x0011e280): 7.3728 MHz (300 MHz * 384 / 15625)
 *     GENI_SER_M_CLK_CFG / S_CLK_CFG: div 4 * oversampling 16 = 115200 baud (8N1)
 *
 * Boot-arg levels:
 *   bt=1   Read-only survey: TLMM pins (plus UART pins), SPMI BT rails, cmd-db addresses, GCC clocks
 *   bt=2   bt=1 + enable GCC SE10 clocks (7.3728 MHz), pinmux GPIOs 40..43, probe GENI UART ID/status
 *   bt=3   bt=2 + RPMh VRM votes on L7B/S1B/S2B/S3B/S3E, GPIO 156 reset pulse, GENI UART 115200 8N1
 *          init, H4 HCI_Reset + QCA patch version query + optional fw/msbtfw*.mbn / fw/msnv*.bin load
 *   bt_again=<s>   Seconds until second pin look in bt=1 mode (0: none; default 30)
 */
#include <stdint.h>
#include <stdbool.h>
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/startup.h>
#include <kern/thread.h>
#include <kern/thread_call.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/systm.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern vm_offset_t ml_io_map_wcomb(vm_offset_t phys_addr, vm_size_t size);
extern void loader_log_putc(char c);
extern void hakios_spmi_lock(void);
extern void hakios_spmi_unlock(void);
extern lck_mtx_t hakios_rsc_lock;
extern int hakios_soc_id(void) __attribute__((weak));
extern boolean_t PE_parse_boot_arg_str(const char *arg_string, char *arg_ptr, int strlen);
extern const uint8_t *hakios_image_file(const char *dir, const char *name, uint64_t *size, int *disk_out) __attribute__((weak));

static int say(const char *format, ...) __printflike(1, 2);

static int
say(const char *format, ...)
{
	char line[256];
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	for (const char *c = line; *c != 0; c++) {
		loader_log_putc(*c);
	}
	return printf("%s", line);
}

static inline uint32_t
rd(vm_offset_t address)
{
	return *(volatile uint32_t *)address;
}

static inline void
wr(vm_offset_t address, uint32_t value)
{
	*(volatile uint32_t *)address = value;
	__asm__ volatile ("dsb sy" ::: "memory");
}

static void
sleep_us(uint32_t us)
{
	delay_for_interval(us, NSEC_PER_USEC);
}

static void
sleep_ms(uint32_t ms)
{
	delay_for_interval(ms, NSEC_PER_MSEC);
}

static bool
bt_is_cliffs_soc(void)
{
	uint32_t soc_smem = 0;
	char soc_arg[32] = { 0 };

	if (PE_parse_boot_argn("soc_smem", &soc_smem, sizeof(soc_smem)) && soc_smem == 0x81d00000) {
		return true;
	}
	if (PE_parse_boot_arg_str("soc", soc_arg, sizeof(soc_arg)) && strcmp(soc_arg, "cliffs") == 0) {
		return true;
	}
	if (hakios_soc_id != NULL && (hakios_soc_id() == 632 || hakios_soc_id() == 618)) {
		return true;
	}
	return false;
}

/* ---- TLMM GPIOs -------------------------------------------------------------------------------- */

#define TLMM_PHYS               0x0f100000UL
#define TLMM_SIZE               0x100000UL
#define GPIO_CFG(n)             (tlmm + 0x1000UL * (n))         /* [1:0] pull, [5:2] function, [8:6] drive, [9] output */
#define GPIO_IO(n)              (tlmm + 0x1000UL * (n) + 4)     /* bit 0 in, bit 1 out */
#define CFG_PULL_NONE           0u
#define CFG_PULL_DOWN           1u
#define CFG_PULL_UP             3u
#define CFG_FUNC(f)             (((uint32_t)(f) & 0xfu) << 2)
#define CFG_DRV_2MA             (0u << 6)
#define CFG_OUTPUT              (1u << 9)

static vm_offset_t tlmm;
static uint32_t bt_level, again_seconds = 30;
static bool is_cliffs;
static thread_call_t again_call;

static const struct bt_pin {
	unsigned pin;
	const char *what;
} redwood_pins[] = {
	{ 84, "qcom,wl-reset-gpio: the Wi-Fi half's reset" },
	{ 85, "qcom,bt-reset-gpio: the Bluetooth half's reset" },
	{ 86, "qcom,bt-sw-ctrl-gpio: the antenna switch" },
}, cliffs_pins[] = {
	{ 35,  "qcom,wl-reset-gpio: the Wi-Fi half's reset" },
	{ 156, "qcom,bt-reset-gpio: the Bluetooth half's reset (bt_en)" },
	{ 80,  "qcom,bt-sw-ctrl-gpio: the antenna switch" },
	{ 40,  "hsuart0 CTS (qup1_se2_l0)" },
	{ 41,  "hsuart0 RTS (qup1_se2_l1)" },
	{ 42,  "hsuart0 TX  (qup1_se2_l2)" },
	{ 43,  "hsuart0 RX  (qup1_se2_l3)" },
};

static void
look(const char *when)
{
	const struct bt_pin *pins = is_cliffs ? cliffs_pins : redwood_pins;
	unsigned count = (unsigned)(is_cliffs ? (sizeof(cliffs_pins) / sizeof(cliffs_pins[0]))
	                                      : (sizeof(redwood_pins) / sizeof(redwood_pins[0])));

	for (unsigned i = 0; i < count; i++) {
		uint32_t cfg = rd(GPIO_CFG(pins[i].pin)), io = rd(GPIO_IO(pins[i].pin));

		say("bt: %s: gpio%u (%s): function %u, pull %u, %u mA, %s, reads %u\n",
		    when, pins[i].pin, pins[i].what, (cfg >> 2) & 15, cfg & 3,
		    (((cfg >> 6) & 7) + 1) * 2,
		    (cfg & CFG_OUTPUT) ? (io & 2 ? "drives high" : "drives low") : "input", io & 1);
	}
}

static void
look_again(__unused thread_call_param_t p0, __unused thread_call_param_t p1)
{
	look("again");
}

/* ---- SPMI PMIC Arbiter v7 & RPMh VRM (SM7675 Cliffs) ------------------------------------------- */

#define ARB_CORE_PHYS           0x0c400000UL
#define ARB_CORE_SIZE           0x3000UL
#define ARB_CNFG_PHYS           0x0c42d000UL
#define ARB_CNFG_SIZE           0x4000UL
#define ARB_OBSERVER_PHYS       0x0c440000UL
#define ARB_OBSERVER_SIZE       0x8000UL
#define ARB_SYSTEM              0
#define ARB_VERSION             0x000
#define ARB_FEATURES            0x004
#define OBSERVER_COMMAND        0x00
#define OBSERVER_STATUS         0x08
#define OBSERVER_DATA0          0x18
#define OBSERVER_DATA1          0x1c
#define STATUS_DONE             0x1
#define STATUS_FAILURE          0x2
#define STATUS_DENIED           0x4
#define STATUS_DROPPED          0x8
#define COMMAND_READ            (1u << 27)
#define SPMI_TIMEOUT_US         2000

#define CMD_DB_PHYS             0x81c60000UL
#define CMD_DB_SIZE             0x20000UL
#define RSC_PHYS                0x17a20000UL
#define RSC_SIZE                0x10000UL
#define RSC_TCS_BASE            (rsc + 0xd00)
#define RSC_IRQ(reg)            (RSC_TCS_BASE + (reg))
#define TCS(reg)                (RSC_TCS_BASE + tcs_slot_stride * rpmh_tcs_id + (reg))
#define TCS_IRQ_CLEAR           0x08
#define TCS_CONTROL_ENABLE      (1u << 16)
#define TCS_CONTROL_TRIGGER     (1u << 24)
#define MSGID_WRITE_4_BYTES     0x10008
#define MSGID_RESPONSE          0x100
#define VRM_VOLTAGE             0
#define VRM_ENABLE              4
#define VRM_MODE                8
#define VRM_MODE_NPM            7

struct cmd_db_slave {
	uint16_t id, header_offset, data_offset, count, version, reserved[3];
};
struct cmd_db_header {
	uint32_t version;
	uint8_t magic[4];
	struct cmd_db_slave slave[8];
	uint32_t checksum, reserved;
	uint8_t data[];
};
struct cmd_db_entry {
	char id[8];
	uint32_t priority[2];
	uint32_t address;
	uint16_t length, offset;
};

static const struct bt_rail {
	const char *name;
	const char *rpmh_name;
	uint16_t ppid;          /* slave << 8 | periph */
	uint32_t target_mv;
	uint32_t mode;
} cliffs_bt_rails[] = {
	{ "bt-vdd-io (L7B)",    "ldob7", 0x1c7, 1800, 7 },
	{ "bt-vdd-rfa1 (S1B)",  "smpb1", 0x19b, 1900, 7 },
	{ "bt-vdd-rfa2 (S2B)",  "smpb2", 0x19e, 1352, 7 },
	{ "bt-vdd-cx/dig (S3B)","smpb3", 0x1a1, 968,  7 },
	{ "bt-vdd-ipa (S3E)",   "smpe3", 0x4a1, 2200, 7 },
};

static vm_offset_t arb_core, arb_cnfg, arb_observer, cmd_db, rsc;
static uint32_t rpmh_tcs_id = 3, tcs_slot_stride = 0x2a0;
static uint32_t tcs_reg_status = 0x28, tcs_reg_control = 0x24, tcs_reg_wait = 0x20, tcs_reg_enable = 0x2c;
static uint32_t tcs_cmd_stride = 24, tcs_cmd_msgid_base = 0x34, tcs_cmd_addr_base = 0x38, tcs_cmd_data_base = 0x3c, tcs_cmd_status_base = 0x40;

static int
spmi_find_channel(uint16_t ppid)
{
	uint32_t channels;
	int found = -1;

	if (!arb_core) {
		return -1;
	}
	channels = rd(arb_core + ARB_FEATURES) & 0x7ff;
	if (channels == 0 || channels > 1024) {
		channels = 1024;
	}
	for (uint32_t n = 0; n < channels; n++) {
		uint32_t map = rd(arb_core + 0x2000 + 4 * n);
		if (map == 0 || ((map >> 8) & 0xfff) != ppid) {
			continue;
		}
		if (found < 0) {
			found = (int)n;
		}
		if (arb_cnfg && (rd(arb_cnfg + 4 * n) & 7) == ARB_SYSTEM) {
			return (int)n;
		}
	}
	return found;
}

static const char *
spmi_read(uint32_t channel, uint8_t reg, uint8_t *bytes, uint32_t count)
{
	vm_offset_t ch;
	uint64_t start, now, interval;
	uint32_t status, data[2];

	if (!arb_observer) {
		return "arbiter unmapped";
	}
	ch = arb_observer + 0x8000UL * ARB_SYSTEM + 0x20UL * channel;
	nanoseconds_to_absolutetime(SPMI_TIMEOUT_US * (uint64_t)NSEC_PER_USEC, &interval);
	start = mach_absolute_time();
	hakios_spmi_lock();
	wr(ch + OBSERVER_COMMAND, COMMAND_READ | ((uint32_t)reg << 4) | (count - 1));
	do {
		status = rd(ch + OBSERVER_STATUS);
		now = mach_absolute_time();
	} while (!(status & STATUS_DONE) && now - start < interval);
	data[0] = rd(ch + OBSERVER_DATA0);
	data[1] = count > 4 ? rd(ch + OBSERVER_DATA1) : 0;
	hakios_spmi_unlock();

	if (!(status & STATUS_DONE)) return "timeout";
	if (status & STATUS_DENIED)  return "denied";
	if (status & STATUS_FAILURE) return "failed";
	if (status & STATUS_DROPPED) return "dropped";
	for (uint32_t i = 0; i < count; i++) {
		bytes[i] = (uint8_t)(data[i / 4] >> (8 * (i % 4)));
	}
	return NULL;
}

static bool
cmd_db_id_matches(const char *entry_id, const char *name)
{
	size_t len = strlen(name);
	if (strncmp(entry_id, name, len) != 0) {
		return false;
	}
	for (size_t i = len; i < 8; i++) {
		if (entry_id[i] != 0 && entry_id[i] != ' ') {
			return false;
		}
	}
	return true;
}

static uint32_t
rpmh_address(const char *name)
{
	const struct cmd_db_header *db = (const struct cmd_db_header *)cmd_db;

	if (!db || db->magic[0] != 0xdb || db->magic[1] != 0x30 || db->magic[2] != 0x03 || db->magic[3] != 0x0c) {
		return 0;
	}
	for (unsigned s = 0; s < 8; s++) {
		const struct cmd_db_slave *slave = &db->slave[s];
		const struct cmd_db_entry *entry = (const struct cmd_db_entry *)(db->data + slave->header_offset);
		if (slave->id == 0 || slave->header_offset >= CMD_DB_SIZE - 0x1000 || slave->count > 512) {
			continue;
		}
		for (unsigned i = 0; i < slave->count; i++) {
			if (cmd_db_id_matches(entry[i].id, name)) {
				return entry[i].address;
			}
		}
	}
	return 0;
}

static void
dump_rails(const char *phase)
{
	if (!arb_core) {
		arb_core = ml_io_map(ARB_CORE_PHYS, ARB_CORE_SIZE);
		arb_cnfg = ml_io_map(ARB_CNFG_PHYS, ARB_CNFG_SIZE);
		arb_observer = ml_io_map(ARB_OBSERVER_PHYS, ARB_OBSERVER_SIZE);
	}
	if (!cmd_db) {
		cmd_db = ml_io_map_wcomb(CMD_DB_PHYS, CMD_DB_SIZE);
	}
	if (!arb_core || !arb_observer) {
		say("bt: %s: SPMI arbiter unavailable\n", phase);
		return;
	}
	uint32_t ver = rd(arb_core + ARB_VERSION);
	if (ver < 0x70000000u) {
		say("bt: %s: unexpected PMIC arbiter version %#x\n", phase, ver);
		return;
	}
	for (size_t i = 0; i < sizeof(cliffs_bt_rails) / sizeof(cliffs_bt_rails[0]); i++) {
		const struct bt_rail *r = &cliffs_bt_rails[i];
		uint32_t vrm = rpmh_address(r->rpmh_name);
		int ch = spmi_find_channel(r->ppid);
		uint8_t st[2] = { 0 }, ctrl[8] = { 0 };
		if (ch < 0) {
			say("bt: %s: %s (ppid %#x, vrm %#x): no SPMI channel\n", phase, r->name, r->ppid, vrm);
			continue;
		}
		const char *e1 = spmi_read((uint32_t)ch, 0x08, st, 2);
		const char *e2 = e1 ? e1 : spmi_read((uint32_t)ch, 0x40, ctrl, 8);
		if (e2 != NULL) {
			say("bt: %s: %s (ppid %#x ch %d vrm %#x): %s\n", phase, r->name, r->ppid, ch, vrm, e2);
			continue;
		}
		uint32_t req_mv = (uint32_t)ctrl[0] | ((uint32_t)ctrl[1] << 8);
		uint32_t cur_mv = (uint32_t)ctrl[2] | ((uint32_t)ctrl[3] << 8);
		say("bt: %s: %s (ch %d vrm %#x): %s (%s), req=%u mV, cur=%u mV, mode=%#x\n",
		    phase, r->name, ch, vrm,
		    (ctrl[6] & 0x80) ? "ENABLED" : "OFF",
		    (st[0] & 0x80) ? "in-reg" : "not-in-reg",
		    req_mv, cur_mv, ctrl[5]);
	}
}

static void
rpmh_init_offsets(void)
{
	uint32_t rsc_id = rd(rsc + 0);
	uint32_t major = (rsc_id >> 16) & 0xff;
	if (major >= 3) {
		rpmh_tcs_id = 3;
		tcs_slot_stride = 0x2a0;
		tcs_reg_status = 0x28;
		tcs_reg_control = 0x24;
		tcs_reg_wait = 0x20;
		tcs_reg_enable = 0x2c;
		tcs_cmd_stride = 24;
		tcs_cmd_msgid_base = 0x34;
		tcs_cmd_addr_base = 0x38;
		tcs_cmd_data_base = 0x3c;
		tcs_cmd_status_base = 0x40;
	} else {
		rpmh_tcs_id = 0;
		tcs_slot_stride = 0x2a0;
		tcs_reg_status = 0x18;
		tcs_reg_control = 0x14;
		tcs_reg_wait = 0x10;
		tcs_reg_enable = 0x1c;
		tcs_cmd_stride = 20;
		tcs_cmd_msgid_base = 0x30;
		tcs_cmd_addr_base = 0x34;
		tcs_cmd_data_base = 0x38;
		tcs_cmd_status_base = 0x3c;
	}
}

static bool
rpmh_set_vrm(const char *name, uint32_t mv, uint32_t mode)
{
	uint32_t base = rpmh_address(name);
	if (!rsc || base == 0) {
		return false;
	}
	lck_mtx_lock(&hakios_rsc_lock);
	for (unsigned i = 0; i < 1000 && !(rd(TCS(tcs_reg_status)) & 1); i++) {
		sleep_us(10);
	}
	if (!(rd(TCS(tcs_reg_status)) & 1)) {
		lck_mtx_unlock(&hakios_rsc_lock);
		return false;
	}
	const struct {
		uint32_t off, val;
	} cmds[3] = {
		{ VRM_VOLTAGE, mv },
		{ VRM_MODE,    mode },
		{ VRM_ENABLE,  1 },
	};
	for (unsigned i = 0; i < 3; i++) {
		wr(TCS(tcs_cmd_msgid_base + tcs_cmd_stride * i), MSGID_WRITE_4_BYTES | MSGID_RESPONSE);
		wr(TCS(tcs_cmd_addr_base + tcs_cmd_stride * i), base + cmds[i].off);
		wr(TCS(tcs_cmd_data_base + tcs_cmd_stride * i), cmds[i].val);
	}
	wr(TCS(tcs_reg_wait), 7);
	wr(TCS(tcs_reg_enable), 7);
	wr(RSC_IRQ(TCS_IRQ_CLEAR), 1u << rpmh_tcs_id);
	uint32_t ctrl = rd(TCS(tcs_reg_control));
	wr(TCS(tcs_reg_control), (ctrl & ~TCS_CONTROL_TRIGGER) | TCS_CONTROL_ENABLE);
	wr(TCS(tcs_reg_control), ctrl | TCS_CONTROL_ENABLE | TCS_CONTROL_TRIGGER);
	bool ok = false;
	for (unsigned i = 0; i < 2000; i++) {
		if (rd(TCS(tcs_reg_status)) & 1) {
			ok = true;
			break;
		}
		sleep_us(10);
	}
	wr(RSC_IRQ(TCS_IRQ_CLEAR), 1u << rpmh_tcs_id);
	lck_mtx_unlock(&hakios_rsc_lock);
	return ok;
}

/* ---- GCC & QUPv3 SE10 (hsuart0 @ 0x00888000) --------------------------------------------------- */

#define GCC_BASE_PHYS           0x00100000UL
#define GCC_MAP_SIZE            0x60000UL
#define GCC_SE10_CBCR           0x1e274UL       /* gcc_qupv3_wrap1_s2_clk */
#define GCC_SE10_RCGR           0x1e280UL       /* gcc_qupv3_wrap1_s2_clk_src */
#define GCC_VOTE_WRAP1          0x52010UL       /* bits 0..3: wrap1 core/s_ahb/m_ahb/core_2x, bit 6: s2 */
#define VOTE_WRAP1_SE2          ((1u << 0) | (1u << 1) | (1u << 2) | (1u << 3) | (1u << 6))

#define HSUART0_PHYS            0x00888000UL
#define HSUART0_SIZE            0x4000UL
#define GENI_FORCE_DEFAULT      0x020
#define GENI_OUTPUT_CTRL        0x024
#define GENI_CGC_CTRL           0x028
#define GENI_STATUS             0x040
#define GENI_SER_M_CLK_CFG      0x048
#define GENI_SER_S_CLK_CFG      0x04c
#define GENI_IF_DISABLE_RO      0x064
#define GENI_FW_REVISION_RO     0x068
#define GENI_CLK_SEL            0x07c
#define GENI_BYTE_GRAN          0x254
#define GENI_DMA_MODE_EN        0x258
#define UART_TX_TRANS_CFG       0x25c
#define GENI_TX_PACKING_CFG0    0x260
#define GENI_TX_PACKING_CFG1    0x264
#define UART_TX_WORD_LEN        0x268
#define UART_TX_STOP_BIT_LEN    0x26c
#define UART_TX_TRANS_LEN       0x270
#define UART_RX_TRANS_CFG       0x280
#define GENI_RX_PACKING_CFG0    0x284
#define GENI_RX_PACKING_CFG1    0x288
#define UART_RX_WORD_LEN        0x28c
#define UART_RX_STALE_CNT       0x294
#define UART_TX_PARITY_CFG      0x2a4
#define UART_RX_PARITY_CFG      0x2a8
#define UART_MANUAL_RFR         0x2ac
#define GENI_M_CMD0             0x600
#define GENI_M_CMD_CTRL         0x604
#define GENI_M_IRQ_STATUS       0x610
#define GENI_M_IRQ_EN           0x614
#define GENI_M_IRQ_CLEAR        0x618
#define GENI_S_CMD0             0x630
#define GENI_S_CMD_CTRL         0x634
#define GENI_S_IRQ_STATUS       0x640
#define GENI_S_IRQ_EN           0x644
#define GENI_S_IRQ_CLEAR        0x648
#define GENI_TX_FIFO            0x700
#define GENI_RX_FIFO            0x780
#define GENI_TX_FIFO_STATUS     0x800
#define GENI_RX_FIFO_STATUS     0x804
#define SE_IRQ_EN               0xe1c
#define SE_HW_PARAM_0           0xe24
#define SE_HW_PARAM_1           0xe28
#define SE_DMA_GENERAL_CFG      0xe30

static vm_offset_t gcc, uart;

static bool
uart_clocks_enable(void)
{
	if (!gcc) {
		gcc = ml_io_map(GCC_BASE_PHYS, GCC_MAP_SIZE);
	}
	if (!gcc) {
		say("bt: failed to map GCC\n");
		return false;
	}
	/* Set gcc_qupv3_wrap1_s2_clk_src to 7.3728 MHz (300 MHz GPLL0_OUT_EVEN src=1 * 384 / 15625) */
	wr(gcc + GCC_SE10_RCGR + 4, (2u << 12) | (1u << 8));
	wr(gcc + GCC_SE10_RCGR + 8, 384u);
	wr(gcc + GCC_SE10_RCGR + 12, 0xc476u);
	wr(gcc + GCC_SE10_RCGR + 16, 0xc2f6u);
	wr(gcc + GCC_SE10_RCGR, rd(gcc + GCC_SE10_RCGR) | 1u);
	for (unsigned i = 0; i < 1000 && (rd(gcc + GCC_SE10_RCGR) & 1u); i++) {
		sleep_us(10);
	}
	wr(gcc + GCC_VOTE_WRAP1, rd(gcc + GCC_VOTE_WRAP1) | VOTE_WRAP1_SE2);
	for (unsigned i = 0; i < 1000 && (rd(gcc + GCC_SE10_CBCR) & (1u << 31)); i++) {
		sleep_us(10);
	}
	say("bt: GCC wrap1 vote=%#x, se10_cbcr=%#x, se10_rcgr=%#x/%#x\n",
	    rd(gcc + GCC_VOTE_WRAP1), rd(gcc + GCC_SE10_CBCR),
	    rd(gcc + GCC_SE10_RCGR), rd(gcc + GCC_SE10_RCGR + 4));
	return !(rd(gcc + GCC_SE10_CBCR) & (1u << 31));
}

static void
uart_pinmux_active(void)
{
	wr(GPIO_CFG(40), CFG_FUNC(1) | CFG_PULL_NONE | CFG_DRV_2MA);  /* CTS */
	wr(GPIO_CFG(41), CFG_FUNC(1) | CFG_PULL_DOWN | CFG_DRV_2MA);  /* RTS */
	wr(GPIO_CFG(42), CFG_FUNC(1) | CFG_PULL_UP   | CFG_DRV_2MA);  /* TX */
	wr(GPIO_CFG(43), CFG_FUNC(1) | CFG_PULL_DOWN | CFG_DRV_2MA);  /* RX */
}

static bool
uart_init_115200(void)
{
	if (!uart) {
		uart = ml_io_map(HSUART0_PHYS, HSUART0_SIZE);
	}
	if (!uart) {
		return false;
	}
	uint32_t dis = rd(uart + GENI_IF_DISABLE_RO);
	uint32_t rev = rd(uart + GENI_FW_REVISION_RO);
	uint32_t hw0 = rd(uart + SE_HW_PARAM_0);
	uint32_t hw1 = rd(uart + SE_HW_PARAM_1);
	uint32_t sts = rd(uart + GENI_STATUS);
	say("bt: hsuart0 @ %#lx: if_disable=%#x fw_rev=%#x (proto=%u) hw=%#x/%#x status=%#x\n",
	    HSUART0_PHYS, dis, rev, (rev >> 8) & 0xff, hw0, hw1, sts);
	if (dis || ((rev >> 8) & 0xff) != 2) {
		say("bt: hsuart0 not ready in UART protocol mode\n");
		return false;
	}
	if (bt_level < 3) {
		return true;
	}
	/* Cancel any leftover commands */
	if (rd(uart + GENI_STATUS) & 1u) {
		wr(uart + GENI_M_CMD_CTRL, 1u << 4);
		sleep_ms(2);
		wr(uart + GENI_M_CMD_CTRL, 1u << 5);
		sleep_ms(2);
	}
	if (rd(uart + GENI_STATUS) & (1u << 12)) {
		wr(uart + GENI_S_CMD_CTRL, 1u << 4);
		sleep_ms(2);
		wr(uart + GENI_S_CMD_CTRL, 1u << 5);
		sleep_ms(2);
	}
	wr(uart + GENI_M_IRQ_EN, 0);
	wr(uart + GENI_S_IRQ_EN, 0);
	wr(uart + SE_IRQ_EN, 0);
	wr(uart + GENI_M_IRQ_CLEAR, ~0u);
	wr(uart + GENI_S_IRQ_CLEAR, ~0u);

	wr(uart + GENI_CLK_SEL, 0);
	wr(uart + GENI_SER_M_CLK_CFG, (4u << 4) | 1u);   /* 7.3728 MHz / (16 * 4) = 115200 */
	wr(uart + GENI_SER_S_CLK_CFG, (4u << 4) | 1u);
	wr(uart + GENI_BYTE_GRAN, 0);
	wr(uart + GENI_DMA_MODE_EN, 0);
	wr(uart + GENI_TX_PACKING_CFG0, 0xf);
	wr(uart + GENI_TX_PACKING_CFG1, 0);
	wr(uart + GENI_RX_PACKING_CFG0, 0xf);
	wr(uart + GENI_RX_PACKING_CFG1, 0);
	wr(uart + UART_TX_TRANS_CFG, 0x2);               /* no parity, ignore CTS for initial HCI */
	wr(uart + UART_TX_WORD_LEN, 8);
	wr(uart + UART_TX_STOP_BIT_LEN, 0);
	wr(uart + UART_TX_PARITY_CFG, 0);
	wr(uart + UART_RX_TRANS_CFG, 0);
	wr(uart + UART_RX_WORD_LEN, 8);
	wr(uart + UART_RX_STALE_CNT, 0xa0);
	wr(uart + UART_RX_PARITY_CFG, 0);
	wr(uart + UART_MANUAL_RFR, 0);                   /* auto-RFR */

	/* Drain any stale RX FIFO bytes and start RX sequencer */
	wr(uart + GENI_S_CMD0, 1u << 27);                /* UART_START_READ */
	sleep_ms(5);
	while ((rd(uart + GENI_RX_FIFO_STATUS) & 0x1ffffffu) > 0) {
		(void)rd(uart + GENI_RX_FIFO);
	}
	return true;
}

static bool
uart_tx(const uint8_t *buf, uint32_t len)
{
	wr(uart + GENI_M_IRQ_CLEAR, ~0u);
	wr(uart + UART_TX_TRANS_LEN, len);
	wr(uart + GENI_M_CMD0, 1u << 27);                /* UART_START_TX */
	for (uint32_t i = 0; i < len; i++) {
		for (unsigned w = 0; w < 5000 && ((rd(uart + GENI_TX_FIFO_STATUS) & 0xfffffffu) >= 16); w++) {
			sleep_us(10);
		}
		wr(uart + GENI_TX_FIFO, buf[i]);
	}
	for (unsigned w = 0; w < 50000; w++) {
		if (rd(uart + GENI_M_IRQ_STATUS) & 1u) {
			wr(uart + GENI_M_IRQ_CLEAR, ~0u);
			return true;
		}
		sleep_us(10);
	}
	return false;
}

static uint32_t
uart_rx(uint8_t *out, uint32_t max_len, uint32_t wait_ms)
{
	uint32_t got = 0;
	for (uint32_t elapsed = 0; elapsed < wait_ms * 100 && got < max_len; elapsed++) {
		uint32_t wc = rd(uart + GENI_RX_FIFO_STATUS) & 0x1ffffffu;
		while (wc > 0 && got < max_len) {
			out[got++] = (uint8_t)(rd(uart + GENI_RX_FIFO) & 0xffu);
			wc = rd(uart + GENI_RX_FIFO_STATUS) & 0x1ffffffu;
		}
		if (got > 0 && wc == 0 && elapsed >= 500) {
			break;
		}
		sleep_us(10);
	}
	return got;
}

static void
uart_stop(void)
{
	if (!uart) {
		return;
	}
	if (rd(uart + GENI_STATUS) & (1u << 12)) {
		wr(uart + GENI_S_CMD_CTRL, 1u << 4);
		sleep_ms(2);
	}
	if (rd(uart + GENI_STATUS) & 1u) {
		wr(uart + GENI_M_CMD_CTRL, 1u << 4);
		sleep_ms(2);
	}
}

static void
wcn6750_try_fw(void)
{
	static const char *const fw_names[] = { "msbtfw11.mbn", "msbtfw11.tlv", "msbtfw12.mbn", "msbtfw12.tlv" };
	static const char *const nv_names[] = { "msnv11.bin", "msnv12.bin" };
	const uint8_t *fw = NULL, *nv = NULL;
	uint64_t fw_sz = 0, nv_sz = 0;

	if (hakios_image_file == NULL) {
		return;
	}
	for (size_t i = 0; i < sizeof(fw_names) / sizeof(fw_names[0]) && fw == NULL; i++) {
		fw = hakios_image_file("fw", fw_names[i], &fw_sz, NULL);
		if (fw != NULL) {
			say("bt: found fw/%s (%llu bytes)\n", fw_names[i], fw_sz);
		}
	}
	for (size_t i = 0; i < sizeof(nv_names) / sizeof(nv_names[0]) && nv == NULL; i++) {
		nv = hakios_image_file("fw", nv_names[i], &nv_sz, NULL);
		if (nv != NULL) {
			say("bt: found fw/%s (%llu bytes)\n", nv_names[i], nv_sz);
		}
	}
	if (fw == NULL) {
		say("bt: no fw/msbtfw*.mbn in small image (pull /vendor/bt_firmware/image/* into fw-bt/)\n");
	}
}

static void
cliffs_bt_worker(__unused void *parameter, __unused wait_result_t result)
{
	if (bt_level >= 2) {
		if (!uart_clocks_enable()) {
			say("bt: GCC wrap1 se2 clocks did not enable\n");
			return;
		}
		uart_pinmux_active();
		if (!uart_init_115200()) {
			return;
		}
	}
	if (bt_level >= 3) {
		rsc = ml_io_map(RSC_PHYS, RSC_SIZE);
		if (rsc) {
			rpmh_init_offsets();
			for (size_t i = 0; i < sizeof(cliffs_bt_rails) / sizeof(cliffs_bt_rails[0]); i++) {
				const struct bt_rail *r = &cliffs_bt_rails[i];
				bool ok = rpmh_set_vrm(r->rpmh_name, r->target_mv, r->mode);
				say("bt: RPMh %s -> %u mV (ok=%d)\n", r->rpmh_name, r->target_mv, ok);
			}
			sleep_ms(10);
			dump_rails("post-vote");
		}
		/* btpower_configure_gpios sequence on GPIO 156 (qcom,bt-reset-gpio) */
		wr(GPIO_IO(156), 0);
		wr(GPIO_CFG(156), CFG_OUTPUT | CFG_DRV_2MA);
		sleep_ms(50);
		wr(GPIO_IO(156), 2);
		wr(GPIO_CFG(156), CFG_OUTPUT | CFG_DRV_2MA);
		sleep_ms(100);
		look("post-reset");

		static const uint8_t hci_reset[] = { 0x01, 0x03, 0x0c, 0x00 };
		static const uint8_t qca_ver[]   = { 0x01, 0x00, 0xfc, 0x01, 0x19 };
		uint8_t rx[64];
		if (uart_tx(hci_reset, sizeof(hci_reset))) {
			uint32_t n = uart_rx(rx, sizeof(rx), 300);
			say("bt: HCI_Reset tx ok, rx %u bytes%s\n", n, n ? ":" : "");
			if (n > 0) {
				for (uint32_t i = 0; i < n && i < 16; i++) {
					say(" %02x", rx[i]);
				}
				say("\n");
			}
		} else {
			say("bt: HCI_Reset tx timed out\n");
		}
		if (uart_tx(qca_ver, sizeof(qca_ver))) {
			uint32_t n = uart_rx(rx, sizeof(rx), 250);
			say("bt: QCA_EDL_VER tx ok, rx %u bytes\n", n);
		}
		wcn6750_try_fw();
		uart_stop();
	}
}

__startup_func
static void
bt_init(void)
{
	uint32_t value;
	uint64_t deadline;

	if (!PE_parse_boot_argn("bt", &bt_level, sizeof(bt_level)) || bt_level == 0) {
		return;
	}
	is_cliffs = bt_is_cliffs_soc();
	if (PE_parse_boot_argn("bt_again", &value, sizeof(value)) && value <= 3600) {
		again_seconds = value;
	}
	tlmm = ml_io_map(TLMM_PHYS, TLMM_SIZE);
	if (tlmm == 0) {
		say("bt: could not map the TLMM; off\n");
		return;
	}
	say("bt: level %u (%s WCN6750)\n", bt_level, is_cliffs ? "SM7675 Cliffs" : "SM7325 Redwood");
	look("at start");
	if (is_cliffs) {
		dump_rails("pre");
		if (!gcc) {
			gcc = ml_io_map(GCC_BASE_PHYS, GCC_MAP_SIZE);
		}
		if (gcc) {
			say("bt: GCC wrap1 vote=%#x, se10_cbcr=%#x\n",
			    rd(gcc + GCC_VOTE_WRAP1), rd(gcc + GCC_SE10_CBCR));
		}
		if (bt_level >= 2) {
			thread_t th;
			if (kernel_thread_start(cliffs_bt_worker, NULL, &th) == KERN_SUCCESS) {
				thread_deallocate(th);
			}
			return;
		}
	}
	if (again_seconds != 0) {
		again_call = thread_call_allocate(look_again, NULL);
		clock_interval_to_deadline(again_seconds, NSEC_PER_SEC, &deadline);
		thread_call_enter_delayed(again_call, deadline);
	}
}
STARTUP(EARLY_BOOT, STARTUP_RANK_LAST, bt_init);
