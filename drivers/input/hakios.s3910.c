/*
 * Synaptics S3910 touchscreen driver for OnePlus Nord 4 (avalon, SM7675 "cliffs").
 * Integrates with HakiOS touch event delivery subsystem (/dev/hakios-touch).
 *
 * Direct hardware implementation modelled on hakios.goodix.c with verified
 * Snapdragon 7+ Gen 3 (SM7675 "cliffs") hardware registers and
 * reverse-engineered Synaptics TCM protocol (TouchComm v1):
 *
 *   power    AVDD 3.1V from RPMh "ldob22" and VDDI 1.8V from RPMh "ldob8",
 *            resolved via the AOP command database (aop_cmd_db_region @ 0x81c60000)
 *            and written through APPS_RSC DRV2 ACTIVE_TCS (TCS3 @ 0x17a214e0).
 *            per Linux syna_spi_hw_reset to put Synaptics S3910 MCU in clean boot mode.
 *   pins     TLMM base at 0x0f100000 (tile REG_BASE 0x100000 from 0x0f000000).
 *            GPIO16 (MISO), GPIO17 (MOSI), GPIO18 (CLK), GPIO19 (CS) -> function 1 (qup0_se4_l0..l3).
 *            GPIO128 -> Reset (active low).
 *            GPIO129 -> Interrupt (active low: pulled low by chip when touched).
 *   clocks   QUPv3 Serial Engine 4 clock voted in GCC_APCS_CLOCK_BRANCH_ENA_VOTE_1
 *            at 0x52008 on SM7675 Cliffs (bit 18: Core 2x, bit 19: Core, bit 20: M_AHB,
 *            bit 21: S_AHB, bit 26: SE4).
 *   SPI      Dual mode: QUPv3 Serial Engine 4 FIFO mode (9.6 MHz) with seamless
 *            direct TLMM GPIO bitbang fallback on identical pins (ensures 100% operation
 *            even when bootloader leaves QUPv3 uninitialized).
 *   protocol Synaptics TouchComm (TCM v1) protocol:
 *            CMD_IDENTIFY (0x02): Query firmware information.
 *            CMD_ENABLE_REPORT (0x05): MUST be sent to enable touch reporting (0x11 and 0x23).
 *            Without CMD_ENABLE_REPORT, the MCU never generates touch packets.
 *            Coordinates: Panel 19840 x 44352 scaled to Darwin 0..65535 space and
 *            posted to /dev/hakios-touch for iOS 16.5 SpringBoard / backboardd.
 *
 * Authored by sathiya / sathiyasenpai for OnePlus Nord 4 (avalon).
 */

#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/hakios.input.h>
#include <sys/systm.h>
#include <stdarg.h>

extern void loader_log_putc(char c);
extern void hakios_tl(const char *format, ...) __printflike(1, 2);
extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern vm_offset_t ml_io_map_wcomb(vm_offset_t phys_addr, vm_size_t size);
extern kern_return_t thread_terminate(thread_t thread);
extern lck_mtx_t hakios_rsc_lock;
extern void hakios_touch_post(uint32_t type, uint32_t finger, uint32_t x, uint32_t y);
extern int hakios_soc_id(void) __attribute__((weak));
extern boolean_t PE_parse_boot_arg_str(const char *arg_string, char *arg_ptr, int strlen);
extern void hakios_spmi_lock(void);
extern void hakios_spmi_unlock(void);

static vm_offset_t tlmm, gcc, se, rsc, cmd_db, arb_core, arb_cnfg, arb_observer;
static bool use_bitbang = false;
static void s3910_log(const char *fmt, ...);

static inline uint32_t
rd(vm_offset_t address)
{
	return *(volatile uint32_t *)address;
}

static inline void
wr(vm_offset_t address, uint32_t value)
{
	*(volatile uint32_t *)address = value;
}

/* ---- SPMI PMIC Arbiter v7 (SM7675 / Cliffs) ---------------------------------------------------- */

#define ARB_CORE_PHYS           0x0c400000UL    /* core: version and peripheral channels table */
#define ARB_CORE_SIZE           0x3000UL
#define ARB_CNFG_PHYS           0x0c42d000UL    /* cnfg: channel ownership table (EE permissions) */
#define ARB_CNFG_SIZE           0x4000UL        /* 16 KB covers all 512 channels */
#define ARB_OBSERVER_PHYS       0x0c440000UL    /* obsrvr: 0x8000 per EE, 0x20 per channel on v7 */
#define ARB_OBSERVER_SIZE       0x8000UL        /* 32 KB covers all 512 channels for EE 0 */
#define ARB_CHNLS_PHYS          0x0c500000UL    /* chnls: 0x1000 per channel for writes on v7 */
#define ARB_SYSTEM              0               /* APSS / EE 0 */

#define OBSERVER_COMMAND        0x00
#define OBSERVER_STATUS         0x08
#define OBSERVER_DATA0          0x18
#define OBSERVER_DATA1          0x1c

#define CHANNEL_COMMAND         0x00
#define CHANNEL_STATUS          0x08
#define CHANNEL_WDATA0          0x10
#define CHANNEL_WDATA1          0x14

#define STATUS_DONE             0x1
#define STATUS_FAILURE          0x2
#define STATUS_DENIED           0x4
#define STATUS_DROPPED          0x8

#define COMMAND_WRITE           (0u << 27)
#define COMMAND_READ            (1u << 27)
#define SPMI_TIMEOUT_US         2000

static vm_offset_t
s3910_spmi_channel_base(uint32_t channel)
{
	static vm_offset_t ch_map[1024] = { 0 };
	if (channel >= 1024) return 0;
	if (!ch_map[channel]) {
		ch_map[channel] = ml_io_map(ARB_CHNLS_PHYS + 0x1000UL * channel, 0x1000);
	}
	return ch_map[channel];
}

static int
s3910_spmi_find_channel(uint16_t periph_id)
{
	if (!arb_core) return -1;
	uint32_t channels = rd(arb_core + 0x004) & 0x7ff;
	if (channels == 0 || channels > 1024) {
		channels = 1024;
	}
	int found_ch = -1;
	for (uint32_t n = 0; n < channels; n++) {
		uint32_t map = rd(arb_core + 0x2000 + 4 * n);
		if (map == 0) {
			continue;
		}
		if (((map >> 8) & 0xfff) == periph_id) {
			if (found_ch < 0) found_ch = (int)n;
			if (arb_cnfg) {
				uint32_t owner = rd(arb_cnfg + 4 * n) & 7;
				if (owner == ARB_SYSTEM) {
					return (int)n;
				}
			}
		}
	}
	return found_ch;
}

static const char *
s3910_spmi_read(uint32_t channel, uint8_t reg, uint8_t *bytes, uint32_t count)
{
	if (!arb_observer) return "arbiter unmapped";
	vm_offset_t ch = arb_observer + 0x8000UL * ARB_SYSTEM + 0x20UL * channel;
	uint64_t start = mach_absolute_time(), now, interval;
	uint32_t status, data[2];

	nanoseconds_to_absolutetime(SPMI_TIMEOUT_US * (uint64_t)NSEC_PER_USEC, &interval);
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

static const char *
s3910_spmi_write(uint32_t channel, uint8_t reg, const uint8_t *bytes, uint32_t count)
{
	if (!arb_cnfg) return "cnfg unmapped";
	uint32_t owner = rd(arb_cnfg + 4 * channel) & 7;
	if (owner != ARB_SYSTEM) {
		s3910_log("s3910: REFUSING write to SPMI ch %u: owned by system %u (not APSS 0)\n", channel, owner);
		return "not owner";
	}
	if (count > 8 || count == 0) return "invalid count";
	vm_offset_t ch = s3910_spmi_channel_base(channel);
	if (!ch) return "chnl unmapped";
	uint64_t start = mach_absolute_time(), now, interval;
	uint32_t status;
	uint32_t wdata[2] = { 0, 0 };

	for (uint32_t i = 0; i < count; i++) {
		wdata[i / 4] |= ((uint32_t)bytes[i]) << (8 * (i % 4));
	}

	nanoseconds_to_absolutetime(SPMI_TIMEOUT_US * (uint64_t)NSEC_PER_USEC, &interval);
	hakios_spmi_lock();
	wr(ch + CHANNEL_WDATA0, wdata[0]);
	if (count > 4) {
		wr(ch + CHANNEL_WDATA1, wdata[1]);
	}
	wr(ch + CHANNEL_COMMAND, COMMAND_WRITE | ((uint32_t)reg << 4) | (count - 1));
	do {
		status = rd(ch + CHANNEL_STATUS);
		now = mach_absolute_time();
	} while (!(status & STATUS_DONE) && now - start < interval);
	hakios_spmi_unlock();

	if (!(status & STATUS_DONE)) return "timeout";
	if (status & STATUS_DENIED)  return "denied";
	if (status & STATUS_FAILURE) return "failed";
	if (status & STATUS_DROPPED) return "dropped";
	return NULL;
}

static void
s3910_log(const char *fmt, ...)
{
	char buf[256];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	printf("%s", buf);
	for (const char *p = buf; *p != 0; p++) {
		loader_log_putc(*p);
	}
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

/* ---- TLMM Pin Multiplexing (SM7675 / Cliffs) --------------------------------------------------- */

#define TLMM_PHYS               0x0f100000UL    /* Cliffs TLMM tile base (0x0f000000 + 0x100000 REG_BASE) */
#define TLMM_SIZE               0x84000UL       /* covers GPIO 0..131 (max pin 129) */
#define GPIO_CFG(n)             (tlmm + 0x1000UL * (n))         /* [1:0] pull, [5:2] func, [8:6] drv, [9] oe, [11] egpio_present, [12] egpio_enable */
#define GPIO_IO(n)              (tlmm + 0x1000UL * (n) + 4)     /* bit 0: in, bit 1: out */
#define PULL_NONE               0
#define PULL_DOWN               1
#define PULL_UP                 3

#define PIN_MISO                16              /* qup0_se4_l0 (func 1) */
#define PIN_MOSI                17              /* qup0_se4_l1 (func 1) */
#define PIN_CLK                 18              /* qup0_se4_l2 (func 1) */
#define PIN_CS                  19              /* qup0_se4_l3 (func 1) */
#define PIN_RESET               128             /* active low reset */
#define PIN_INT                 129             /* active low interrupt (pulled low by chip when touched) */
#define PIN_FUNC_QUP0_SE4       1
#define PIN_FUNC_GPIO           0

static void
gpio_configure(unsigned pin, unsigned function, unsigned pull, unsigned ma, bool output)
{
	/*
	 * Qualcomm TLMM configuration on SM7675 (Cliffs):
	 * [1:0]  pull (0: no pull / bias-disable, 1: pull-down, 3: pull-up)
	 * [5:2]  func (0: gpio, 1: qup0_se4)
	 * [8:6]  drv  ((ma / 2 - 1) & 7) -> 6mA = 2, 8mA = 3
	 * [9]    oe   (output enable: 1 = output, 0 = input)
	 *
	 * CRITICAL: Bit 12 (EGPIO_ENABLE = 0x1000) MUST BE 0!
	 * Setting bit 12 routes the physical pad to the LPI island eGPIO controller,
	 * isolating the pad from APSS TLMM registers so CPU writes never toggle the pins.
	 */
	uint32_t val = (pull & 3) |
	               ((function & 0xf) << 2) |
	               (((ma / 2 - 1) & 7) << 6) |
	               (output ? (1u << 9) : 0);
	wr(GPIO_CFG(pin), val);
	(void)rd(GPIO_CFG(pin));
}

static void
gpio_set(unsigned pin, bool high)
{
	uint32_t io = rd(GPIO_IO(pin));
	if (high) {
		io |= (1u << 1); /* out_bit = 1 */
	} else {
		io &= ~(1u << 1);
	}
	wr(GPIO_IO(pin), io);
	(void)rd(GPIO_IO(pin));
}

static bool
gpio_get(unsigned pin)
{
	return (rd(GPIO_IO(pin)) & 1) != 0;
}

static void
gpio_output(unsigned pin, bool high, unsigned pull, unsigned ma)
{
	gpio_configure(pin, PIN_FUNC_GPIO, pull, ma, true);
	gpio_set(pin, high);
}

static void
s3910_dump_pins(const char *phase)
{
	const struct {
		unsigned pin;
		const char *name;
	} pins[] = {
		{ PIN_RESET, "RESET(128)" },
		{ PIN_INT,   "INT(129)"   },
		{ PIN_CS,    "CS(19)"     },
		{ PIN_CLK,   "CLK(18)"    },
		{ PIN_MOSI,  "MOSI(17)"   },
		{ PIN_MISO,  "MISO(16)"   },
	};

	s3910_log("s3910: [pin status: %s]\n", phase);
	for (unsigned i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) {
		unsigned p = pins[i].pin;
		uint32_t cfg = rd(GPIO_CFG(p));
		uint32_t io = rd(GPIO_IO(p));
		unsigned pull = cfg & 3;
		unsigned func = (cfg >> 2) & 0xf;
		unsigned drv  = (((cfg >> 6) & 7) + 1) * 2;
		bool oe       = (cfg & (1u << 9)) != 0;
		bool egpio    = (cfg & (1u << 12)) != 0;
		bool in_val   = (io & 1) != 0;
		bool out_val  = (io & 2) != 0;

		s3910_log("s3910:   gpio%-3u (%-10s): cfg=%#x (func=%u pull=%u %uma oe=%d egpio=%d) io=%#x (in=%d out=%d -> %s)\n",
		          p, pins[i].name, cfg, func, pull, drv, oe, egpio, io, in_val, out_val,
		          oe ? (out_val ? "drives HIGH" : "drives LOW") : (in_val ? "reads HIGH" : "reads LOW"));
	}
}

/* ---- Power Supplies via RPMh (Cliffs Command DB & APPS_RSC DRV2) ------------------------------- */

#define CMD_DB_PHYS             0x81c60000UL    /* Cliffs AOP Command DB (cliffs-reserved-memory.dtsi) */
#define CMD_DB_SIZE             0x20000UL
#define RSC_PHYS                0x17a20000UL    /* Cliffs APPS_RSC DRV2 (cliffs.dtsi) */
#define RSC_SIZE                0x10000UL
/*
 * APPS_RSC DRV2 TCS layout:
 * On SM7675 Cliffs (vb_1.dts): qcom,tcs-offset = 0xd00, qcom,tcs-config =
 *   <2 3> (WAKE TCS 0..2), <0 2> (ACTIVE TCS 3..4), <1 2> (SLEEP TCS 5..6), <4 1> (GEN 7).
 * RSC_DRV_IRQ_ENABLE/STATUS/CLEAR sit once at tcs_base (0xd00) with 1 bit per TCS (bit 3 for TCS 3).
 * Per-TCS registers sit at tcs_base + 0x2a0 * tcs_id + reg.
 */
#define RSC_TCS_BASE            (rsc + 0xd00)
#define RSC_IRQ(reg)            (RSC_TCS_BASE + (reg))
#define TCS(reg)                (RSC_TCS_BASE + tcs_slot_stride * rpmh_tcs_id + (reg))
#define TCS_IRQ_ENABLE          0x00
#define TCS_IRQ_STATUS          0x04
#define TCS_IRQ_CLEAR           0x08
#define TCS_CONTROL_ENABLE      (1u << 16)
#define TCS_CONTROL_TRIGGER     (1u << 24)
#define MSGID_WRITE_4_BYTES     0x10008
#define MSGID_RESPONSE          0x100

#define VRM_VOLTAGE             0               /* regulator voltage register: mV */
#define VRM_ENABLE              4               /* regulator enable: 1 = on */
#define VRM_MODE                8               /* regulator mode */
#define VRM_MODE_LDO_HPM        7               /* High Power Mode */

/* Dynamic register offsets supporting both RSC v2.7 and v3.0+ (SM7675 Cliffs) */
static uint32_t rpmh_tcs_id = 3;                /* ACTIVE_TCS #0 on Cliffs */
static uint32_t tcs_slot_stride = 0x2a0;        /* 672 bytes per TCS */
static uint32_t tcs_reg_status = 0x28;
static uint32_t tcs_reg_control = 0x24;
static uint32_t tcs_reg_wait = 0x20;
static uint32_t tcs_reg_enable = 0x2c;
static uint32_t tcs_cmd_stride = 24;
static uint32_t tcs_cmd_msgid_base = 0x34;
static uint32_t tcs_cmd_addr_base = 0x38;
static uint32_t tcs_cmd_data_base = 0x3c;
static uint32_t tcs_cmd_status_base = 0x40;

static void
rpmh_init_offsets(void)
{
	uint32_t rsc_id = rd(rsc + 0);
	uint32_t major = (rsc_id >> 16) & 0xff;
	uint32_t minor = (rsc_id >> 8) & 0xff;

	if (major >= 3) {
		/* Version 3.0+ (SM7675 Cliffs) */
		rpmh_tcs_id = 3;                /* WAKE=0..2, ACTIVE=3..4 */
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
		/* Version 2.7 (SM8350 Lahaina, SC7280) */
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
	s3910_log("s3910: RPMh RSC ID %#x (version %u.%u, using ACTIVE TCS %u @ +%#x)\n",
	          rsc_id, major, minor, rpmh_tcs_id, 0xd00u + tcs_slot_stride * rpmh_tcs_id);
}
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

static bool
cmd_db_id_matches(const char *entry_id, const char *name)
{
	size_t len = strlen(name);
	if (strncmp(entry_id, name, len) != 0) {
		return false;
	}
	/* Match exact name or space/null-padded entries */
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

	if (!cmd_db) {
		return 0;
	}
	if (db->magic[0] != 0xdb || db->magic[1] != 0x30 || db->magic[2] != 0x03 || db->magic[3] != 0x0c) {
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
cmd_db_dump_vrm(void)
{
	const struct cmd_db_header *db = (const struct cmd_db_header *)cmd_db;
	if (!db || db->magic[0] != 0xdb) return;
	for (unsigned s = 0; s < 8; s++) {
		const struct cmd_db_slave *slave = &db->slave[s];
		const struct cmd_db_entry *entry = (const struct cmd_db_entry *)(db->data + slave->header_offset);
		if (slave->id == 0 || slave->count > 512) continue;
		for (unsigned i = 0; i < slave->count; i++) {
			if (entry[i].id[0] == 'l' && entry[i].id[1] == 'd' && entry[i].id[2] == 'o') {
				char id[9] = { 0 };
				memcpy(id, entry[i].id, 8);
				s3910_log("s3910: cmd_db VRM: '%s' -> addr %#x\n", id, entry[i].address);
			}
		}
	}
}

static bool
rpmh_wait_idle(const char *what)
{
	uint64_t deadline;
	clock_interval_to_deadline(50, NSEC_PER_MSEC, &deadline);
	while (mach_absolute_time() < deadline) {
		/* In Linux read_tcs_reg(..., RSC_DRV_STATUS, tcs_id): 0 if busy, non-zero if idle */
		if (rd(TCS(tcs_reg_status)) != 0) {
			return true;
		}
		sleep_us(10);
	}
	s3910_log("s3910: RPMh: timeout waiting for %s (TCS%u status reg %#x = %#x)\n",
	          what, rpmh_tcs_id, tcs_reg_status, rd(TCS(tcs_reg_status)));
	return false;
}

static bool
rpmh_wait_done(const char *what)
{
	uint32_t tcs_bit = 1u << rpmh_tcs_id;
	uint64_t deadline;
	clock_interval_to_deadline(50, NSEC_PER_MSEC, &deadline);
	while (mach_absolute_time() < deadline) {
		if ((rd(RSC_IRQ(TCS_IRQ_STATUS)) & tcs_bit) != 0) {
			return true;
		}
		sleep_us(10);
	}
	s3910_log("s3910: RPMh: timeout waiting for %s (TCS%u irq_status=%#x, tcs_status=%#x, cmd0_status=%#x)\n",
	          what, rpmh_tcs_id, rd(RSC_IRQ(TCS_IRQ_STATUS)), rd(TCS(tcs_reg_status)), rd(TCS(tcs_cmd_status_base)));
	return false;
}

static bool
rpmh_write(const uint32_t *address, const uint32_t *data, unsigned count)
{
	uint32_t irq_enable;
	uint32_t tcs_bit = 1u << rpmh_tcs_id;
	bool done;

	if (!rsc || count == 0 || count > 3) {
		return false;
	}
	lck_mtx_lock(&hakios_rsc_lock);
	if (!rpmh_wait_idle("RSC idle")) {
		lck_mtx_unlock(&hakios_rsc_lock);
		return false;
	}

	irq_enable = rd(RSC_IRQ(TCS_IRQ_ENABLE));

	/* Clear AMC mode trigger and enable (per Linux __tcs_set_trigger) */
	wr(TCS(tcs_reg_control), rd(TCS(tcs_reg_control)) & ~TCS_CONTROL_TRIGGER);
	(void)rd(TCS(tcs_reg_control));
	wr(TCS(tcs_reg_control), rd(TCS(tcs_reg_control)) & ~TCS_CONTROL_ENABLE);
	(void)rd(TCS(tcs_reg_control));
	wr(RSC_IRQ(TCS_IRQ_CLEAR), tcs_bit);

	for (unsigned i = 0; i < count; i++) {
		uint32_t msgid_offset = tcs_cmd_msgid_base + tcs_cmd_stride * i;
		uint32_t addr_offset  = tcs_cmd_addr_base  + tcs_cmd_stride * i;
		uint32_t data_offset  = tcs_cmd_data_base  + tcs_cmd_stride * i;

		wr(TCS(msgid_offset), MSGID_WRITE_4_BYTES | MSGID_RESPONSE);
		wr(TCS(addr_offset), address[i]);
		wr(TCS(data_offset), data[i]);
	}

	wr(TCS(tcs_reg_wait), (1u << count) - 1);
	wr(TCS(tcs_reg_enable), (1u << count) - 1);
	wr(RSC_IRQ(TCS_IRQ_ENABLE), irq_enable | tcs_bit);

	/* Enable AMC mode, then trigger (per Linux __tcs_set_trigger) */
	wr(TCS(tcs_reg_control), TCS_CONTROL_ENABLE);
	(void)rd(TCS(tcs_reg_control));
	wr(TCS(tcs_reg_control), TCS_CONTROL_ENABLE | TCS_CONTROL_TRIGGER);

	done = rpmh_wait_done("RPMh request");

	/* Clean up */
	wr(TCS(tcs_reg_control), rd(TCS(tcs_reg_control)) & ~TCS_CONTROL_TRIGGER);
	(void)rd(TCS(tcs_reg_control));
	wr(TCS(tcs_reg_control), rd(TCS(tcs_reg_control)) & ~TCS_CONTROL_ENABLE);
	(void)rd(TCS(tcs_reg_control));
	wr(TCS(tcs_reg_enable), 0);
	wr(TCS(tcs_reg_wait), 0);
	wr(RSC_IRQ(TCS_IRQ_CLEAR), tcs_bit);
	wr(RSC_IRQ(TCS_IRQ_ENABLE), irq_enable);
	lck_mtx_unlock(&hakios_rsc_lock);
	return done;
}

static bool
rpmh_set_vrm(const char *name, uint32_t mv)
{
	uint32_t addr_res = rpmh_address(name);
	if (addr_res != 0 && (addr_res >> 16 & 0xf) == 4) {
		uint32_t addr[3] = { addr_res + VRM_VOLTAGE, addr_res + VRM_ENABLE, addr_res + VRM_MODE };
		uint32_t val[3]  = { mv, 1, VRM_MODE_LDO_HPM };
		return rpmh_write(addr, val, 3);
	}
	return false;
}

struct touch_ldo {
	const char *name;
	const char *rpmh_name;
	uint16_t ppid;
	uint32_t fallback_channel;
	uint16_t target_mv;
};

/*
 * Power rails on OnePlus Nord 4 (avalon, SM7675):
 * - AVDD (vdd-supply in dtbo synaptics_tcm_hbp@0): PMXR2230 L22B (ldob22, slave 1 0xd600), 3100 mV
 * - VDDI (shared display/touch vddio-supply): PMXR2230 L8B (ldob8, slave 1 0xc800), 1800 mV
 * Note: ldoe5 is a 1.1-1.3V rail on Cliffs and must NOT be touched.
 */
static const struct touch_ldo touch_ldos[] = {
	{ .name = "AVDD(ldob22)", .rpmh_name = "ldob22", .ppid = 0x1d6, .fallback_channel = 180, .target_mv = 3100 },
	{ .name = "VDDI(ldob8)",  .rpmh_name = "ldob8",  .ppid = 0x1c8, .fallback_channel = 166, .target_mv = 1800 },
};

static void
s3910_dump_ldos(const char *phase)
{
	for (size_t i = 0; i < sizeof(touch_ldos) / sizeof(touch_ldos[0]); i++) {
		const struct touch_ldo *ldo = &touch_ldos[i];
		int ch = s3910_spmi_find_channel(ldo->ppid);
		uint32_t channel = (ch >= 0) ? (uint32_t)ch : ldo->fallback_channel;
		uint8_t status[2] = { 0 };
		uint8_t ctrl[8] = { 0 };

		const char *err_stat = s3910_spmi_read(channel, 0x08, status, 2);
		const char *err_ctrl = s3910_spmi_read(channel, 0x40, ctrl, 8);

		bool en = (err_ctrl == NULL) && (ctrl[6] & 0x80);
		bool ready = (err_stat == NULL) && (status[0] & 0x80);
		uint16_t v_set = (uint16_t)(ctrl[0] | ((uint16_t)ctrl[1] << 8));
		uint32_t owner = arb_cnfg ? (rd(arb_cnfg + 4 * channel) & 7) : 0xff;

		s3910_log("s3910: SPMI %s [%s] (ppid %#x -> ch %u, owner %u): %s, %s (set=%u mV, mode=%#x, st=%#x/%#x, en_ctl=%#x)\n",
		          phase, ldo->name, ldo->ppid, channel, owner,
		          en ? "ENABLED" : "DISABLED",
		          ready ? "READY" : "NOT READY",
		          v_set, ctrl[5], status[0], status[1], ctrl[6]);
	}
}

static bool
s3910_power_on(void)
{
	/* Step 0: Configure INT input without internal pull first so we observe real VDDI pull-up state */
	gpio_configure(PIN_INT, PIN_FUNC_GPIO, PULL_NONE, 8, false);
	s3910_log("s3910: INT(129) pre-power (no internal pull): %d\n", gpio_get(PIN_INT));
	s3910_dump_ldos("pre");

	/* Step 1: Send RPMh VRM votes via ACTIVE TCS for all touch power rails */
	for (size_t i = 0; i < sizeof(touch_ldos) / sizeof(touch_ldos[0]); i++) {
		const struct touch_ldo *ldo = &touch_ldos[i];
		uint32_t addr = rpmh_address(ldo->rpmh_name);
		bool ok = false;
		if (addr != 0) {
			ok = rpmh_set_vrm(ldo->rpmh_name, ldo->target_mv);
		}
		s3910_log("s3910: RPMh %s: addr=%#x, voted %u mV (ok=%d)\n",
		          ldo->name, addr, ldo->target_mv, ok);
	}
	sleep_ms(10);

	/* Step 2: Check optional PMIC GPIO (dtbo synaptics,avdd-gpio = <&pm8550vs_c_gpios 3 0x1> -> 0x28a) */
	int pmic_gpio3_ch = s3910_spmi_find_channel(0x28a);
	if (pmic_gpio3_ch >= 0) {
		uint8_t g_stat[2] = { 0 };
		uint8_t g_ctrl[8] = { 0 };
		s3910_spmi_read((uint32_t)pmic_gpio3_ch, 0x08, g_stat, 2);
		s3910_spmi_read((uint32_t)pmic_gpio3_ch, 0x40, g_ctrl, 8);
		uint32_t owner = arb_cnfg ? (rd(arb_cnfg + 4 * (uint32_t)pmic_gpio3_ch) & 7) : 0xff;
		s3910_log("s3910: PMIC GPIO3 (0x28a -> ch %d, owner %u) pre: st=%#x mode=%#x vin=%#x en=%#x\n",
		          pmic_gpio3_ch, owner, g_stat[0], g_ctrl[0], g_ctrl[1], g_ctrl[6]);

		if (owner == ARB_SYSTEM) {
			uint8_t vin = 0x01, pull = 0x00, out = 0x02, in_c = 0x00, mode = 0x01, src = 0x80, en = 0x80;
			s3910_spmi_write((uint32_t)pmic_gpio3_ch, 0x41, &vin, 1);
			s3910_spmi_write((uint32_t)pmic_gpio3_ch, 0x42, &pull, 1);
			s3910_spmi_write((uint32_t)pmic_gpio3_ch, 0x45, &out, 1);
			s3910_spmi_write((uint32_t)pmic_gpio3_ch, 0x43, &in_c, 1);
			s3910_spmi_write((uint32_t)pmic_gpio3_ch, 0x40, &mode, 1);
			s3910_spmi_write((uint32_t)pmic_gpio3_ch, 0x44, &src, 1);
			const char *w_en = s3910_spmi_write((uint32_t)pmic_gpio3_ch, 0x46, &en, 1);
			sleep_ms(5);
			s3910_spmi_read((uint32_t)pmic_gpio3_ch, 0x08, g_stat, 2);
			s3910_spmi_read((uint32_t)pmic_gpio3_ch, 0x40, g_ctrl, 8);
			s3910_log("s3910: PMIC GPIO3 post-write (%s): st=%#x mode=%#x src=%#x en=%#x\n",
			          w_en ? w_en : "ok", g_stat[0], g_ctrl[0], g_ctrl[4], g_ctrl[6]);
		}
	} else {
		s3910_log("s3910: PMIC GPIO3 (0x28a) not in SPMI arbiter table (board/bootloader controlled)\n");
	}

	/* Step 3: Read back live SPMI regulator status after RPMh votes */
	s3910_dump_ldos("post");

	/*
	 * CRITICAL: Configure SPI bus idle levels BEFORE releasing RESET!
	 * CS must be driven HIGH, CLK LOW, MOSI LOW when RESET rises.
	 */
	gpio_output(PIN_CS,   true,  PULL_NONE, 6);
	gpio_output(PIN_CLK,  false, PULL_NONE, 6);
	gpio_output(PIN_MOSI, false, PULL_NONE, 6);
	gpio_configure(PIN_MISO, PIN_FUNC_GPIO, PULL_NONE, 6, false);
	gpio_configure(PIN_INT,  PIN_FUNC_GPIO, PULL_NONE, 8, false);

	/* Delay after power enable before reset pulse (avalon dtsi: power-delay-ms = 200) */
	sleep_ms(200);
	s3910_dump_pins("pre-reset");

	/*
	 * Hardware Reset Toggle (dtbo_1.dts: reset-active-ms = 10, reset-delay-ms = 80;
	 * hbp_syna_s3910.ko uses 10 ms low, 100 ms high):
	 * Hold low 15 ms with CS firmly HIGH, then release and wait 120 ms for MCU boot.
	 */
	gpio_output(PIN_RESET, false, PULL_NONE, 8);
	sleep_ms(15);
	s3910_dump_pins("reset asserted (pin128 low)");

	gpio_set(PIN_RESET, true);
	sleep_ms(120);
	s3910_dump_pins("reset released (post-120ms)");

	s3910_log("s3910: power supplies enabled and hardware reset pulse completed\n");
	return true;
}

/* ---- Clocks (GCC on SM7675 Cliffs) ------------------------------------------------------------- */

#define GCC_PHYS                0x00150000UL    /* 16 KiB aligned page covering GCC_VOTE_CLIFFS @ +0x52008 */
#define GCC_SIZE                0x4000UL

/*
 * GCC branch clock voting for SM7675 Cliffs (from drivers/clk/qcom/gcc-cliffs.c):
 * Vote register: 0x52008 (offset 0x2008 inside 0x00150000 page)
 * BIT 18: gcc_qupv3_wrap0_core_2x_clk
 * BIT 19: gcc_qupv3_wrap0_core_clk
 * BIT 20: gcc_qupv3_wrap_0_m_ahb_clk
 * BIT 21: gcc_qupv3_wrap_0_s_ahb_clk
 * BIT 26: gcc_qupv3_wrap0_s4_clk (SE4 SPI clock branch)
 */
#define GCC_VOTE_CLIFFS         0x2008
#define VOTE_CLIFFS_WRAP0_SE4   ((1u << 18) | (1u << 19) | (1u << 20) | (1u << 21) | (1u << 26))

static bool
s3910_clocks_on(void)
{
	/* Vote for Wrap0 AHB/Core and SE4 clocks in GCC on SM7675 Cliffs */
	uint32_t current_vote = rd(gcc + GCC_VOTE_CLIFFS);
	wr(gcc + GCC_VOTE_CLIFFS, current_vote | VOTE_CLIFFS_WRAP0_SE4);

	s3910_log("s3910: GCC SE4 clock branch voted (0x52008 = %#x)\n", rd(gcc + GCC_VOTE_CLIFFS));
	return true;
}

/* ---- QUPv3 Serial Engine 4 (SE4) SPI Hardware Engine ------------------------------------------- */

#define SE_PHYS                 0x00a90000UL    /* QUPv3 Serial Engine 4 SPI base address */
#define SE_SIZE                 0x4000UL

#define GENI_FORCE_DEFAULT      (se + 0x020)
#define GENI_OUTPUT_CTRL        (se + 0x024)
#define GENI_CGC_CTRL           (se + 0x028)
#define GENI_STATUS             (se + 0x040)
#define GENI_SER_M_CLK_CFG      (se + 0x048)
#define GENI_IF_DISABLE_RO      (se + 0x064)
#define GENI_FW_REVISION_RO     (se + 0x068)    /* [15:8] protocol: 1 = SPI */
#define GENI_CLK_SEL            (se + 0x07c)
#define SPI_CPHA                (se + 0x224)
#define SPI_LOOPBACK            (se + 0x22c)
#define SPI_CPOL                (se + 0x230)
#define SPI_DEMUX_OUTPUT_INV    (se + 0x24c)
#define SPI_DEMUX_SEL           (se + 0x250)
#define GENI_BYTE_GRAN          (se + 0x254)
#define GENI_DMA_MODE_EN        (se + 0x258)
#define SPI_TRANS_CFG           (se + 0x25c)
#define GENI_TX_PACKING_CFG0    (se + 0x260)
#define GENI_TX_PACKING_CFG1    (se + 0x264)
#define SPI_WORD_LEN            (se + 0x268)
#define SPI_TX_TRANS_LEN        (se + 0x26c)
#define SPI_RX_TRANS_LEN        (se + 0x270)
#define SPI_DELAY_COUNTERS      (se + 0x278)
#define GENI_RX_PACKING_CFG0    (se + 0x284)
#define GENI_RX_PACKING_CFG1    (se + 0x288)
#define GENI_M_CMD0             (se + 0x600)
#define GENI_M_CMD_CTRL         (se + 0x604)
#define GENI_M_IRQ_STATUS       (se + 0x610)
#define GENI_M_IRQ_EN           (se + 0x614)
#define GENI_M_IRQ_CLEAR        (se + 0x618)
#define GENI_S_IRQ_CLEAR        (se + 0x648)
#define GENI_TX_FIFO            (se + 0x700)
#define GENI_RX_FIFO            (se + 0x780)
#define GENI_TX_FIFO_STATUS     (se + 0x800)
#define GENI_RX_FIFO_STATUS     (se + 0x804)
#define GENI_TX_WATERMARK       (se + 0x80c)
#define GENI_RX_WATERMARK       (se + 0x810)
#define GENI_RX_RFR_WATERMARK   (se + 0x814)
#define SE_DMA_TX_IRQ_CLR       (se + 0xc44)
#define SE_DMA_RX_IRQ_CLR       (se + 0xd44)
#define SE_GSI_EVENT_EN         (se + 0xe18)
#define SE_IRQ_EN               (se + 0xe1c)
#define SE_HW_PARAM_0           (se + 0xe24)
#define SE_DMA_GENERAL_CFG      (se + 0xe30)

#define M_CMD_DONE              (1u << 0)
#define M_CMD_CANCEL            (1u << 4)
#define M_CMD_ABORT             (1u << 5)
#define M_ERRORS                ((1u << 1) | (1u << 2) | (1u << 3) | (1u << 24) | (1u << 25) | (1u << 28) | (1u << 29))
#define SPI_FULL_DUPLEX         (3u << 27)

static uint32_t se_fifo_depth = 16;

/* ---- Direct TLMM Hardware Bitbang SPI (Fallback on identical pins) ----------------------------- */

static void
s3910_bitbang_init(void)
{
	/* Force TLMM GPIO function 0 (GPIO mode) without EGPIO */
	gpio_output(PIN_CS,   true,  PULL_NONE, 6);
	gpio_output(PIN_CLK,  false, PULL_NONE, 6);
	gpio_output(PIN_MOSI, false, PULL_NONE, 6);
	/* Configure MISO as input with PULL_NONE per stock DTS */
	gpio_configure(PIN_MISO, PIN_FUNC_GPIO, PULL_NONE, 6, false);

	/* Configure Interrupt Pin 129 as input with bias-disable per stock dtbo_1.dts ts_int_active */
	gpio_configure(PIN_INT, PIN_FUNC_GPIO, PULL_NONE, 8, false);
	s3910_dump_pins("bitbang init");
}

static bool
s3910_bitbang_transfer(const uint8_t *tx, uint8_t *rx, uint32_t length)
{
	gpio_set(PIN_CS, false);
	sleep_us(10);
	for (uint32_t i = 0; i < length; i++) {
		uint8_t out = tx ? tx[i] : 0xff, in = 0;

		for (int bit = 7; bit >= 0; bit--) {
			gpio_set(PIN_MOSI, (out >> bit) & 1);
			sleep_us(1);
			gpio_set(PIN_CLK, true);
			sleep_us(1);
			in = (uint8_t)((in << 1) | (gpio_get(PIN_MISO) ? 1 : 0));
			gpio_set(PIN_CLK, false);
			sleep_us(1);
		}
		if (rx) {
			rx[i] = in;
		}
	}
	sleep_us(10);
	gpio_set(PIN_CS, true);
	sleep_us(10);
	return true;
}

/*
 * Read header + payload in a SINGLE SPI transaction (CS held LOW throughout).
 *
 * TCM v1 async touch reports are only buffered once by the chip; if CS is
 * toggled between the 4-byte header read and the continued-payload read, the
 * chip resets its MISO pointer and the second transaction returns zeros.
 * This function clocks out the header and payload without ever de-asserting CS.
 */
static bool
s3910_bitbang_read_packet(uint8_t *hdr4, uint8_t *payload, uint32_t payload_len)
{
	uint32_t total = 4 + payload_len;
	gpio_set(PIN_CS, false);
	sleep_us(10);
	for (uint32_t i = 0; i < total; i++) {
		uint8_t in = 0;
		for (int bit = 7; bit >= 0; bit--) {
			gpio_set(PIN_MOSI, 1); /* 0xff dummy on MOSI during read */
			sleep_us(1);
			gpio_set(PIN_CLK, true);
			sleep_us(1);
			in = (uint8_t)((in << 1) | (gpio_get(PIN_MISO) ? 1 : 0));
			gpio_set(PIN_CLK, false);
			sleep_us(1);
		}
		if (i < 4) {
			hdr4[i] = in;
		} else {
			payload[i - 4] = in;
		}
	}
	sleep_us(10);
	gpio_set(PIN_CS, true);
	sleep_us(10);
	return true;
}

static bool
s3910_spi_init(void)
{
	uint32_t fw_rev = rd(GENI_FW_REVISION_RO);
	uint32_t proto = (fw_rev >> 8) & 0xff;

	if (proto != 1 || (rd(GENI_IF_DISABLE_RO) & 1)) {
		s3910_log("s3910: QUPv3 SE4 not configured by UEFI for SPI (proto %u, fw %#x) -> using direct TLMM bitbang\n",
		          proto, fw_rev);
		use_bitbang = true;
		s3910_bitbang_init();
		return true;
	}

	se_fifo_depth = (rd(SE_HW_PARAM_0) >> 16) & 0x3f;
	if (se_fifo_depth < 4) {
		se_fifo_depth = 16;
	}

	wr(SE_GSI_EVENT_EN, 0);
	wr(GENI_M_IRQ_CLEAR, 0xffffffff);
	wr(GENI_S_IRQ_CLEAR, 0xffffffff);
	wr(SE_DMA_TX_IRQ_CLR, 0xffffffff);
	wr(SE_DMA_RX_IRQ_CLR, 0xffffffff);
	wr(GENI_CGC_CTRL, rd(GENI_CGC_CTRL) | 0x7f);
	wr(SE_DMA_GENERAL_CFG, rd(SE_DMA_GENERAL_CFG) | 0xf);
	wr(GENI_OUTPUT_CTRL, 0x7f);
	wr(GENI_FORCE_DEFAULT, 1);
	wr(GENI_DMA_MODE_EN, rd(GENI_DMA_MODE_EN) & ~1u);      /* Select FIFO Mode */
	wr(GENI_RX_WATERMARK, se_fifo_depth - 3);
	wr(GENI_RX_RFR_WATERMARK, se_fifo_depth - 2);
	wr(GENI_TX_WATERMARK, 0);
	wr(SE_IRQ_EN, rd(SE_IRQ_EN) | 0xf);
	wr(GENI_M_IRQ_EN, rd(GENI_M_IRQ_EN) | 0x7fc0007f);

	/* SPI Mode 0, MSB first, 8 bits word length */
	wr(SPI_LOOPBACK, 0);
	wr(SPI_DEMUX_SEL, 0);
	wr(SPI_CPHA, 0);
	wr(SPI_CPOL, 0);
	wr(SPI_DEMUX_OUTPUT_INV, 0);
	wr(SPI_TRANS_CFG, rd(SPI_TRANS_CFG) & ~2u);
	wr(SPI_DELAY_COUNTERS, 0);
	wr(GENI_TX_PACKING_CFG0, 0x0007f8fe);
	wr(GENI_TX_PACKING_CFG1, 0x000ffefe);
	wr(GENI_RX_PACKING_CFG0, 0x0007f8fe);
	wr(GENI_RX_PACKING_CFG1, 0x000ffefe);
	wr(GENI_BYTE_GRAN, 0);
	wr(SPI_WORD_LEN, 8 - 4);

	/* Divide 19.2 MHz clock to ~9.6 MHz for stable high-speed transfers */
	wr(GENI_CLK_SEL, 0);
	wr(GENI_SER_M_CLK_CFG, (2 << 4) | 1);

	/* Mux TLMM GPIOs 16, 17, 18, 19 to QUP0_SE4 */
	gpio_configure(PIN_MISO, PIN_FUNC_QUP0_SE4, PULL_NONE, 6, false);
	gpio_configure(PIN_MOSI, PIN_FUNC_QUP0_SE4, PULL_NONE, 6, false);
	gpio_configure(PIN_CLK,  PIN_FUNC_QUP0_SE4, PULL_NONE, 6, false);
	gpio_configure(PIN_CS,   PIN_FUNC_QUP0_SE4, PULL_NONE, 6, false);

	s3910_log("s3910: QUPv3 SE4 SPI engine ready (FIFO depth %u words, 9.6 MHz)\n", se_fifo_depth);
	return true;
}

static void
s3910_spi_drain(uint8_t *rx, uint32_t length, uint32_t *received)
{
	uint32_t status = rd(GENI_RX_FIFO_STATUS);
	uint32_t words  = status & 0x01ffffff;
	uint32_t bytes  = words * 4;

	if (status & 0x80000000u) {
		uint32_t valid = (status >> 28) & 7;
		if (valid && valid < 4) {
			bytes -= 4 - valid;
		}
	}
	for (uint32_t i = 0; i < words; i++) {
		uint32_t word = rd(GENI_RX_FIFO);
		for (int j = 0; j < 4 && bytes; j++, bytes--) {
			if (*received < length) {
				if (rx) {
					rx[*received] = (uint8_t)(word >> (8 * j));
				}
				(*received)++;
			}
		}
	}
}

static void
s3910_spi_recover(void)
{
	wr(GENI_M_CMD_CTRL, 1u << 2);   /* Cancel command */
	for (int i = 0; i < 50 && !(rd(GENI_M_IRQ_STATUS) & M_CMD_CANCEL); i++) {
		sleep_us(10);
	}
	if (!(rd(GENI_M_IRQ_STATUS) & M_CMD_CANCEL)) {
		wr(GENI_M_CMD_CTRL, 1u << 1);   /* Abort command */
		for (int i = 0; i < 50 && !(rd(GENI_M_IRQ_STATUS) & M_CMD_ABORT); i++) {
			sleep_us(10);
		}
	}
	for (int i = 0; i < 64 && (rd(GENI_RX_FIFO_STATUS) & 0x01ffffff); i++) {
		(void)rd(GENI_RX_FIFO);
	}
	wr(GENI_M_IRQ_CLEAR, 0xffffffff);
}

static bool
s3910_spi_transfer(const uint8_t *tx, uint8_t *rx, uint32_t length)
{
	if (use_bitbang) {
		return s3910_bitbang_transfer(tx, rx, length);
	}

	uint32_t sent = 0, received = 0, status = 0;
	uint64_t deadline;

	for (int i = 0; rd(GENI_STATUS) & 1; i++) {
		if (i > 50) {
			s3910_spi_recover();
			break;
		}
		sleep_us(10);
	}

	wr(GENI_M_IRQ_CLEAR, 0xffffffff);
	wr(SPI_TX_TRANS_LEN, length);
	wr(SPI_RX_TRANS_LEN, length);
	wr(GENI_M_CMD0, SPI_FULL_DUPLEX);

	clock_interval_to_deadline(10, NSEC_PER_MSEC, &deadline);
	while (mach_absolute_time() < deadline) {
		while (sent < length && ((rd(GENI_TX_FIFO_STATUS) & 0x0fffffff) + 1 < se_fifo_depth)) {
			uint32_t word = 0;
			for (int j = 0; j < 4 && sent < length; j++, sent++) {
				word |= (uint32_t)(tx ? tx[sent] : 0xff) << (8 * j);
			}
			wr(GENI_TX_FIFO, word);
		}
		status = rd(GENI_M_IRQ_STATUS);
		s3910_spi_drain(rx, length, &received);
		if (status & M_ERRORS) {
			break;
		}
		if (status & M_CMD_DONE) {
			s3910_spi_drain(rx, length, &received);
			wr(GENI_M_IRQ_CLEAR, status);
			if (received == length) {
				return true;
			}
			break;
		}
		sleep_us(10);
	}

	s3910_spi_recover();
	return false;
}

/* Dedicated SPI command send */
static bool
s3910_write_cmd(const uint8_t *cmd, uint32_t length)
{
	return s3910_spi_transfer(cmd, NULL, length);
}

/* Dedicated SPI response/data read */
static bool
s3910_read_data(uint8_t *rx, uint32_t length)
{
	return s3910_spi_transfer(NULL, rx, length);
}

/* ---- Synaptics S3910 TouchComm v1 Framing & Dynamic Bitstream Parser --------------------------- */

#define S3910_PANEL_MAX_X       19840UL         /* Exact Avalon panel coords from DT */
#define S3910_PANEL_MAX_Y       44352UL
static uint32_t s3910_panel_max_x = S3910_PANEL_MAX_X;
static uint32_t s3910_panel_max_y = S3910_PANEL_MAX_Y;
#define S3910_MAX_FINGERS       10
#define S3910_MAX_PAYLOAD       512

#define TCM_MESSAGE_MARKER      0xA5
#define TCM_STATUS_IDLE         0x00
#define TCM_STATUS_OK           0x01
#define TCM_STATUS_CONTINUED    0x03
#define REPORT_IDENTIFY         0x10
#define REPORT_TOUCH            0x11
#define REPORT_HBP_FRAME        0x23
#define REPORT_LOG              0x9F

#define TCM_CMD_IDENTIFY                0x02
#define TCM_CMD_ENABLE_REPORT           0x05
#define TCM_CMD_DISABLE_REPORT          0x06
#define TCM_CMD_RUN_APP_FW              0x14
#define TCM_CMD_GET_APP_INFO            0x20
#define TCM_CMD_SET_DYNAMIC_CONFIG      0x24
#define TCM_CMD_GET_TOUCH_CONFIG        0x25

#define TOUCH_END                       0x00
#define TOUCH_FOREACH_ACTIVE_OBJECT     0x01
#define TOUCH_FOREACH_OBJECT            0x02
#define TOUCH_FOREACH_END               0x03
#define TOUCH_PAD_TO_NEXT_BYTE          0x04
#define TOUCH_OBJECT_N_INDEX            0x06
#define TOUCH_OBJECT_N_CLASSIFICATION   0x07
#define TOUCH_OBJECT_N_X_POSITION       0x08
#define TOUCH_OBJECT_N_Y_POSITION       0x09
#define TOUCH_NUM_OF_ACTIVE_OBJECTS     0x17

struct finger_state {
	bool down;
	uint32_t x;
	uint32_t y;
};
static struct finger_state fingers[S3910_MAX_FINGERS];

static uint8_t touch_config[256];
static uint32_t touch_config_len;
static uint32_t debug_log_flag;
static uint32_t valid_reports_count;

static bool
s3910_touching(void)
{
	for (unsigned f = 0; f < S3910_MAX_FINGERS; f++) {
		if (fingers[f].down) {
			return true;
		}
	}
	return false;
}

static bool
s3910_get_bits(const uint8_t *buf, uint32_t buf_bytes, uint32_t bit_off, uint32_t nbits, uint32_t *out)
{
	if (nbits == 0) {
		*out = 0;
		return true;
	}
	if (nbits > 32 || !buf || bit_off + nbits > buf_bytes * 8) {
		*out = 0;
		return false;
	}
	uint32_t byte_idx = bit_off >> 3;
	uint32_t bit_in_byte = bit_off & 7;
	uint32_t val = 0;
	uint32_t filled = 0;

	while (filled < nbits) {
		uint32_t avail = 8 - bit_in_byte;
		uint32_t take = nbits - filled;
		if (take > avail) take = avail;
		uint32_t mask = (1u << take) - 1u;
		val |= ((uint32_t)(buf[byte_idx] >> bit_in_byte) & mask) << filled;
		filled += take;
		byte_idx++;
		bit_in_byte = 0;
	}
	*out = val;
	return true;
}

/*
 * Read one complete TouchComm v1 packet.
 *
 * Protocol (confirmed by log analysis):
 *   TX1: CS low → read 4-byte header [0xa5, code, len_lo, len_hi] → CS high
 *   TX2: CS low → read continuation  [0xa5, 0x03, payload..., crc(2)] → CS high
 *
 * The two-transaction model is correct — the chip presents the continuation
 * on the NEXT CS assertion, not in a single continuous stream.
 */
static bool
s3910_tcm_read_packet(uint8_t *code_out, uint8_t *payload, uint32_t max_payload, uint32_t *len_out, uint32_t max_tries)
{
	static uint8_t cont_buf[2 + S3910_MAX_PAYLOAD + 4];
	uint8_t hdr[4];

	for (uint32_t t = 0; t < max_tries; t++) {
		memset(hdr, 0, sizeof(hdr));
		if (!s3910_read_data(hdr, 4)) {
			if (max_tries > 1) sleep_ms(2);
			continue;
		}
		if (hdr[0] != TCM_MESSAGE_MARKER) {
			if (max_tries > 1) sleep_ms(2);
			continue;
		}
		if (hdr[1] == TCM_STATUS_IDLE && max_tries > 1) {
			sleep_ms(2);
			continue;
		}
		uint32_t plen = (uint32_t)hdr[2] | ((uint32_t)hdr[3] << 8);
		*code_out = hdr[1];
		if (plen == 0) {
			*len_out = 0;
			return true;
		}
		uint32_t to_read = plen <= max_payload ? plen : max_payload;
		/* Read 2-byte continued header (0xa5 0x03) + payload + 2 CRC bytes */
		uint32_t xfer = 2 + to_read + 2;
		if (xfer > sizeof(cont_buf)) {
			xfer = sizeof(cont_buf);
			to_read = xfer - 4;
		}
		memset(cont_buf, 0, xfer);
		if (!s3910_read_data(cont_buf, xfer)) {
			return false;
		}
		if (cont_buf[0] != TCM_MESSAGE_MARKER || cont_buf[1] != TCM_STATUS_CONTINUED) {
			if (debug_log_flag) {
				s3910_log("s3910: continued read bad header [%#x %#x] for code=%#x plen=%u\n",
				          cont_buf[0], cont_buf[1], hdr[1], plen);
			}
			return false;
		}
		if (payload && to_read > 0) {
			memcpy(payload, cont_buf + 2, to_read);
		}
		*len_out = to_read;
		return true;
	}
	return false;
}

static bool
s3910_tcm_send_cmd(uint8_t cmd, const uint8_t *args, uint16_t arg_len,
                   uint8_t *resp, uint32_t max_resp, uint32_t *resp_len, uint8_t *status_out)
{
	uint8_t tx[3 + 32];
	if (arg_len > 32) {
		return false;
	}
	tx[0] = cmd;
	tx[1] = (uint8_t)(arg_len & 0xff);
	tx[2] = (uint8_t)(arg_len >> 8);
	if (arg_len > 0 && args) {
		memcpy(tx + 3, args, arg_len);
	}
	if (!s3910_write_cmd(tx, 3 + arg_len)) {
		return false;
	}
	sleep_ms(5);

	/* Read packets until we get a non-report status (code < 0x10) or run out of attempts */
	for (int p = 0; p < 5; p++) {
		uint8_t code = 0;
		uint32_t plen = 0;
		if (!s3910_tcm_read_packet(&code, resp, max_resp, &plen, 15)) {
			return false;
		}
		if (code < 0x10) {
			if (status_out) *status_out = code;
			if (resp_len) *resp_len = plen;
			return true;
		}
	}
	return false;
}

static void
s3910_parse_touch_report(const uint8_t *payload, uint32_t length)
{
	struct {
		bool valid;
		uint32_t status;
		uint32_t x;
		uint32_t y;
	} objs[S3910_MAX_FINGERS];
	bool seen[S3910_MAX_FINGERS];
	memset(objs, 0, sizeof(objs));
	memset(seen, 0, sizeof(seen));

	if (touch_config_len > 0) {
		uint32_t cfg_idx = 0;
		uint32_t bit_off = 0;
		uint32_t payload_bits = length * 8;
		uint32_t obj = 0;
		uint32_t cur_obj = 0;
		uint32_t num_active = 0;
		bool has_num_active = false;
		bool in_loop = false;
		bool active_only = false;
		uint32_t loop_start = 0;
		uint32_t iters = 0;

		while (cfg_idx < touch_config_len && iters++ < 1024) {
			uint8_t op = touch_config[cfg_idx++];
			if (op == TOUCH_END) {
				break;
			} else if (op == TOUCH_FOREACH_ACTIVE_OBJECT || op == TOUCH_FOREACH_OBJECT) {
				in_loop = true;
				active_only = (op == TOUCH_FOREACH_ACTIVE_OBJECT);
				obj = 0;
				cur_obj = 0;
				loop_start = cfg_idx;
			} else if (op == TOUCH_FOREACH_END) {
				if (!in_loop) {
					continue;
				}
				obj++;
				cur_obj = obj;
				bool more = (bit_off < payload_bits) && (obj < S3910_MAX_FINGERS);
				if (active_only && has_num_active && obj >= num_active) {
					more = false;
				}
				if (more) {
					cfg_idx = loop_start;
				} else {
					in_loop = false;
				}
			} else if (op == TOUCH_PAD_TO_NEXT_BYTE) {
				bit_off = (bit_off + 7u) & ~7u;
			} else {
				if (cfg_idx >= touch_config_len) {
					break;
				}
				uint8_t nbits = touch_config[cfg_idx++];
				uint32_t val = 0;
				/* Only extract a value for opcodes we act on.
				 * Vendor-specific fields (e.g. 0xc6 = 48 bits, 0xc7 = 192 bits)
				 * exceed the 32-bit cap in s3910_get_bits() and would abort the
				 * parser before it ever reaches the FOREACH finger loop.
				 * For those, just bounds-check and skip the bits. */
				bool need_val = (op == TOUCH_NUM_OF_ACTIVE_OBJECTS ||
				                 op == TOUCH_OBJECT_N_INDEX          ||
				                 op == TOUCH_OBJECT_N_CLASSIFICATION ||
				                 op == TOUCH_OBJECT_N_X_POSITION     ||
				                 op == TOUCH_OBJECT_N_Y_POSITION);
				if (need_val) {
					if (!s3910_get_bits(payload, length, bit_off, nbits, &val)) {
						break;
					}
				} else {
					/* Bounds-check without extraction (handles nbits > 32) */
					if ((uint32_t)nbits > payload_bits - bit_off) {
						break;
					}
				}
				bit_off += nbits;

				if (op == TOUCH_NUM_OF_ACTIVE_OBJECTS) {
					num_active = val;
					has_num_active = true;
					if (num_active == 0) {
						break;
					}
				} else if (op == TOUCH_OBJECT_N_INDEX) {
					if (val < S3910_MAX_FINGERS) {
						cur_obj = val;
					}
				} else if (op == TOUCH_OBJECT_N_CLASSIFICATION) {
					if (cur_obj < S3910_MAX_FINGERS) {
						objs[cur_obj].status = val;
						objs[cur_obj].valid = true;
					}
				} else if (op == TOUCH_OBJECT_N_X_POSITION) {
					if (cur_obj < S3910_MAX_FINGERS) {
						objs[cur_obj].x = val;
					}
				} else if (op == TOUCH_OBJECT_N_Y_POSITION) {
					if (cur_obj < S3910_MAX_FINGERS) {
						objs[cur_obj].y = val;
					}
				}
			}
		}
	} else {
		/* Fallback if touch_config was unavailable: 6-byte slots */
		uint32_t off = 0;
		for (unsigned f = 0; f < S3910_MAX_FINGERS && (off + 5) <= length; f++) {
			objs[f].status = payload[off];
			objs[f].x = payload[off + 1] | ((uint32_t)payload[off + 2] << 8);
			objs[f].y = payload[off + 3] | ((uint32_t)payload[off + 4] << 8);
			objs[f].valid = true;
			off += 6;
		}
	}

	for (unsigned f = 0; f < S3910_MAX_FINGERS; f++) {
		if (!objs[f].valid) {
			continue;
		}
		bool is_down = (objs[f].status == 1 || objs[f].status == 2);
		if (!is_down) {
			continue;
		}
		seen[f] = true;
		uint32_t raw_x = objs[f].x;
		uint32_t raw_y = objs[f].y;
		if (raw_x > s3910_panel_max_x) raw_x = s3910_panel_max_x;
		if (raw_y > s3910_panel_max_y) raw_y = s3910_panel_max_y;

		uint32_t norm_x = (raw_x * 65535UL) / s3910_panel_max_x;
		uint32_t norm_y = (raw_y * 65535UL) / s3910_panel_max_y;

		if (!fingers[f].down) {
			fingers[f].down = true;
			fingers[f].x = norm_x;
			fingers[f].y = norm_y;
			if (debug_log_flag || valid_reports_count <= 20) {
				s3910_log("s3910: touch down [%u] at (%u, %u) [raw %u, %u]\n", f, norm_x, norm_y, raw_x, raw_y);
			}
			hakios_touch_post(HAKIOS_TOUCH_DOWN, f, norm_x, norm_y);
		} else if (fingers[f].x != norm_x || fingers[f].y != norm_y) {
			fingers[f].x = norm_x;
			fingers[f].y = norm_y;
			hakios_touch_post(HAKIOS_TOUCH_MOVE, f, norm_x, norm_y);
		}
	}

	for (unsigned f = 0; f < S3910_MAX_FINGERS; f++) {
		if (fingers[f].down && !seen[f]) {
			fingers[f].down = false;
			if (debug_log_flag || valid_reports_count <= 20) {
				s3910_log("s3910: touch up [%u] at (%u, %u)\n", f, fingers[f].x, fingers[f].y);
			}
			hakios_touch_post(HAKIOS_TOUCH_UP, f, fingers[f].x, fingers[f].y);
		}
	}
}

static void
s3910_log_idinfo(const uint8_t *id, uint32_t len)
{
	if (len < 24) {
		s3910_log("s3910: identify payload too short (%u bytes)\n", len);
		return;
	}
	char part[17];
	memcpy(part, id + 2, 16);
	part[16] = 0;
	uint32_t build_id = (uint32_t)id[18] | ((uint32_t)id[19] << 8) |
	                    ((uint32_t)id[20] << 16) | ((uint32_t)id[21] << 24);
	uint16_t max_wr = (uint16_t)(id[22] | ((uint16_t)id[23] << 8));
	s3910_log("s3910: Identify: ver=%u mode=%#x part=\"%s\" build=%u max_wr=%u\n",
	          id[0], id[1], part, build_id, max_wr);
}

static void
s3910_enable_touch_reporting(void)
{
	uint8_t resp[256];
	uint32_t rlen = 0;
	uint8_t st = 0xff;
	uint8_t arg_hbp = REPORT_HBP_FRAME;
	uint8_t arg_touch = REPORT_TOUCH;
	uint8_t dc_lbp[3] = { 0x11, 0x02, 0x00 };
	uint8_t arg_log = REPORT_LOG;

	/* Ensure HBP raw frame (0x23) is disabled so SPI stays responsive */
	(void)s3910_tcm_send_cmd(TCM_CMD_DISABLE_REPORT, &arg_hbp, 1, resp, sizeof(resp), &rlen, &st);

	/* Switch S3910 from HBP (Host-Based Processing) mode to LBP (Local-Based Processing)
	 * before querying GET_TOUCH_CONFIG so touch_config reflects the active LBP layout.
	 * Stock syna_dev_disable_hbp_mode sends TCM_CMD_SET_DYNAMIC_CONFIG (0x24) [0x11, 0x02, 0x00]. */
	memset(resp, 0, sizeof(resp));
	rlen = 0;
	st = 0xff;
	bool dc_ok = s3910_tcm_send_cmd(TCM_CMD_SET_DYNAMIC_CONFIG, dc_lbp, sizeof(dc_lbp), resp, sizeof(resp), &rlen, &st);
	s3910_log("s3910: SET_DYNAMIC_CONFIG (0x24) LBP mode [0x11, 0x02, 0x00]: ok=%d status=%#x\n", dc_ok, st);

	/* Query Application Info (0x20) to verify panel coordinate bounds */
	memset(resp, 0, sizeof(resp));
	rlen = 0;
	st = 0xff;
	if (s3910_tcm_send_cmd(TCM_CMD_GET_APP_INFO, NULL, 0, resp, sizeof(resp), &rlen, &st) &&
	    st == TCM_STATUS_OK && rlen >= 36) {
		uint16_t max_x = (uint16_t)(resp[32] | ((uint16_t)resp[33] << 8));
		uint16_t max_y = (uint16_t)(resp[34] | ((uint16_t)resp[35] << 8));
		s3910_log("s3910: app_info: max_x=%u max_y=%u\n", max_x, max_y);
		if (max_x >= 10000 && max_y >= 10000) {
			s3910_panel_max_x = max_x;
			s3910_panel_max_y = max_y;
		}
	}

	/* Fetch exact TouchComm touch report bitstream layout (up to 256 bytes, avoiding 128-byte clip) */
	memset(resp, 0, sizeof(resp));
	rlen = 0;
	st = 0xff;
	if (s3910_tcm_send_cmd(TCM_CMD_GET_TOUCH_CONFIG, NULL, 0, resp, sizeof(resp), &rlen, &st) &&
	    st == TCM_STATUS_OK && rlen > 0 && rlen <= sizeof(touch_config)) {
		memcpy(touch_config, resp, rlen);
		touch_config_len = rlen;
		s3910_log("s3910: touch_config (%u bytes): [%02x %02x %02x %02x %02x %02x %02x %02x ...]\n",
		          rlen, resp[0], rlen > 1 ? resp[1] : 0, rlen > 2 ? resp[2] : 0, rlen > 3 ? resp[3] : 0,
		          rlen > 4 ? resp[4] : 0, rlen > 5 ? resp[5] : 0, rlen > 6 ? resp[6] : 0, rlen > 7 ? resp[7] : 0);
	} else {
		s3910_log("s3910: GET_TOUCH_CONFIG failed (st=%#x rlen=%u)\n", st, rlen);
	}

	/* Enable REPORT_TOUCH (0x11) and REPORT_LOG (0x9F) */
	rlen = 0;
	st = 0xff;
	bool en_ok = s3910_tcm_send_cmd(TCM_CMD_ENABLE_REPORT, &arg_touch, 1, resp, sizeof(resp), &rlen, &st);
	s3910_log("s3910: enable REPORT_TOUCH (0x11): ok=%d status=%#x\n", en_ok, st);

	rlen = 0;
	st = 0xff;
	bool log_ok = s3910_tcm_send_cmd(TCM_CMD_ENABLE_REPORT, &arg_log, 1, resp, sizeof(resp), &rlen, &st);
	s3910_log("s3910: enable REPORT_LOG (0x9F): ok=%d status=%#x\n", log_ok, st);
}

/* ---- Driver Main Thread ------------------------------------------------------------------------ */

static void
s3910_thread(__unused void *parameter, __unused wait_result_t result)
{
	uint32_t poll_us = 4000; /* 250 Hz touch sampling rate (4 ms) */
	uint32_t poll_ms = 0;
	static uint8_t pkt_buf[S3910_MAX_PAYLOAD];

	if (PE_parse_boot_argn("touch_poll_us", &poll_us, sizeof(poll_us))) {
		if (poll_us < 250 || poll_us > 100000) {
			poll_us = 4000;
		}
	} else if (PE_parse_boot_argn("touch_poll_ms", &poll_ms, sizeof(poll_ms))) {
		if (poll_ms >= 1 && poll_ms <= 100) {
			poll_us = poll_ms * 1000;
		}
	}
	PE_parse_boot_argn("touch_debug", &debug_log_flag, sizeof(debug_log_flag));

	s3910_log("s3910: starting hardware initialization on OnePlus Nord 4 (avalon)...\n");

	/* Step 1: Map MMIO peripherals */
	cmd_db       = ml_io_map_wcomb(CMD_DB_PHYS, CMD_DB_SIZE);
	rsc          = ml_io_map(RSC_PHYS, RSC_SIZE);
	gcc          = ml_io_map(GCC_PHYS, GCC_SIZE);
	tlmm         = ml_io_map(TLMM_PHYS, TLMM_SIZE);
	se           = ml_io_map(SE_PHYS, SE_SIZE);
	arb_core     = ml_io_map(ARB_CORE_PHYS, ARB_CORE_SIZE);
	arb_cnfg     = ml_io_map(ARB_CNFG_PHYS, ARB_CNFG_SIZE);
	arb_observer = ml_io_map(ARB_OBSERVER_PHYS, ARB_OBSERVER_SIZE);

	if (!rsc || !gcc || !tlmm || !se) {
		s3910_log("s3910: failed to map MMIO peripherals (rsc=%p, gcc=%p, tlmm=%p, se=%p)\n",
		       (void *)rsc, (void *)gcc, (void *)tlmm, (void *)se);
		thread_terminate(current_thread());
		return;
	}

	/* Initialize RPMh RSC version, ACTIVE TCS index, and offsets */
	rpmh_init_offsets();

	/* Step 2: Power on touch rails via RPMh ACTIVE TCS and toggle hardware reset */
	s3910_power_on();

	/* Step 3: Enable QUPv3 SE4 clock branch in GCC */
	s3910_clocks_on();

	/* Step 4: Configure TLMM pinmux and initialize SPI engine (or bitbang fallback) */
	s3910_spi_init();

	/* Step 5: Configure Interrupt GPIO 129 as input (bias-disable per dtbo) */
	gpio_configure(PIN_INT, PIN_FUNC_GPIO, PULL_NONE, 8, false);
	s3910_dump_pins("post-spi-init");

	/* Wait up to 100ms for S3910 MCU boot packet (INT goes low) */
	uint32_t wait_int_ms = 0;
	while (gpio_get(PIN_INT) && wait_int_ms < 100) {
		sleep_ms(5);
		wait_int_ms += 5;
	}
	bool int_init = gpio_get(PIN_INT);
	s3910_log("s3910: Pin 129 INT initial level: %d (%s, waited %u ms)\n",
	          int_init, int_init ? "idle high" : "asserted low (ready)", wait_int_ms);

	/* Step 6: TouchComm startup sync (write 0x07, read 4-byte header + continued payload) */
	uint8_t code = 0;
	uint32_t plen = 0;
	bool identified = false;
	uint8_t fw_mode = 0;
	uint8_t sync_byte = 0x07;

	s3910_write_cmd(&sync_byte, 1);
	sleep_ms(5);
	if (s3910_tcm_read_packet(&code, pkt_buf, sizeof(pkt_buf), &plen, 5)) {
		s3910_log("s3910: startup sync packet: code=%#x plen=%u\n", code, plen);
		if (code == REPORT_IDENTIFY && plen >= 24) {
			s3910_log_idinfo(pkt_buf, plen);
			fw_mode = pkt_buf[1];
			identified = true;
		}
	}

	if (!identified) {
		uint8_t st = 0xff;
		if (s3910_tcm_send_cmd(TCM_CMD_IDENTIFY, NULL, 0, pkt_buf, sizeof(pkt_buf), &plen, &st) &&
		    st == TCM_STATUS_OK && plen >= 24) {
			s3910_log_idinfo(pkt_buf, plen);
			fw_mode = pkt_buf[1];
			identified = true;
		} else {
			s3910_log("s3910: CMD_IDENTIFY failed (st=%#x plen=%u MISO=%d INT=%d RST=%d)\n",
			          st, plen, gpio_get(PIN_MISO), gpio_get(PIN_INT), gpio_get(PIN_RESET));
		}
	}

	/* If chip booted into Bootloader/ROMBoot instead of Application FW, jump to Application FW */
	if (identified && fw_mode != 0x01) {
		uint8_t st = 0xff;
		s3910_log("s3910: chip in mode %#x, requesting CMD_RUN_APP_FW (0x14)...\n", fw_mode);
		(void)s3910_tcm_send_cmd(TCM_CMD_RUN_APP_FW, NULL, 0, pkt_buf, sizeof(pkt_buf), &plen, &st);
		sleep_ms(200);
		if (s3910_tcm_send_cmd(TCM_CMD_IDENTIFY, NULL, 0, pkt_buf, sizeof(pkt_buf), &plen, &st) &&
		    st == TCM_STATUS_OK && plen >= 24) {
			s3910_log_idinfo(pkt_buf, plen);
			fw_mode = pkt_buf[1];
		}
	}

	if (identified && fw_mode == 0x01) {
		s3910_enable_touch_reporting();
	} else {
		s3910_log("s3910: WARNING: S3910 not in application firmware (identified=%d mode=%#x)\n",
		          identified, fw_mode);
	}

	s3910_log("s3910: physical touch polling active every %u us (Pin 129 IRQ monitored)\n", poll_us);

	uint32_t ticks = 0;
	uint32_t zero_reads = 0;

	for (;;) {
		ticks++;
		bool irq_asserted = !gpio_get(PIN_INT);
		bool touching = s3910_touching();

		if (irq_asserted || (touching && (ticks % 12) == 0)) {
			for (int burst = 0; burst < 4; burst++) {
				code = 0;
				plen = 0;
				if (!s3910_tcm_read_packet(&code, pkt_buf, sizeof(pkt_buf), &plen, 1)) {
					if (burst == 0 && !gpio_get(PIN_INT)) {
						zero_reads++;
					}
					break;
				}
				if (code == REPORT_TOUCH && plen > 0) {
					valid_reports_count++;
					if (debug_log_flag && valid_reports_count <= 30) {
						/* Log finger bytes (offset 28+) to confirm coordinate data */
						s3910_log("s3910: REPORT_TOUCH len=%u hdr=[%02x %02x %02x] fgr=[%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x]\n",
						          plen,
						          plen > 28 ? pkt_buf[28] : 0,
						          plen > 29 ? pkt_buf[29] : 0,
						          plen > 30 ? pkt_buf[30] : 0,
						          plen > 31 ? pkt_buf[31] : 0,
						          plen > 32 ? pkt_buf[32] : 0,
						          plen > 33 ? pkt_buf[33] : 0,
						          plen > 34 ? pkt_buf[34] : 0,
						          plen > 35 ? pkt_buf[35] : 0,
						          plen > 36 ? pkt_buf[36] : 0,
						          plen > 37 ? pkt_buf[37] : 0,
						          plen > 38 ? pkt_buf[38] : 0,
						          plen > 39 ? pkt_buf[39] : 0,
						          plen > 40 ? pkt_buf[40] : 0,
						          plen > 41 ? pkt_buf[41] : 0,
						          plen > 42 ? pkt_buf[42] : 0);
					}
					s3910_parse_touch_report(pkt_buf, plen);
				} else if (code == REPORT_IDENTIFY && plen >= 24) {
					s3910_log("s3910: unexpected REPORT_IDENTIFY (reset?), re-enabling touch\n");
					s3910_log_idinfo(pkt_buf, plen);
					if (pkt_buf[1] == 0x01) {
						s3910_enable_touch_reporting();
					}
				} else if (code == REPORT_LOG) {
					if (debug_log_flag) {
						s3910_log("s3910: REPORT_LOG (0x9F) received len=%u\n", plen);
					}
				}
				if (gpio_get(PIN_INT)) {
					break;
				}
			}
			/* Skip the poll sleep when INT was asserted — read immediately on the next
			 * edge so we never miss the touch frame by sleeping through it. */
			if (irq_asserted) {
				ticks++;
				if ((ticks % (2000000 / poll_us)) == 0) {
					bool int_pin = gpio_get(PIN_INT);
					bool miso_pin = gpio_get(PIN_MISO);
					bool rst_pin = gpio_get(PIN_RESET);
					if (debug_log_flag || valid_reports_count == 0) {
						s3910_log("s3910: heartbeat [%us]: INT(129)=%d (%s), MISO(16)=%d, RST(128)=%d, touching=%d, reports=%u, misses=%u\n",
						          (ticks * poll_us) / 1000000, int_pin, int_pin ? "idle high" : "LOW/TOUCH",
						          miso_pin, rst_pin, s3910_touching(), valid_reports_count, zero_reads);
					}
				}
				continue; /* no sleep — go straight back to IRQ check */
			}
		}

		if ((ticks % (2000000 / poll_us)) == 0) {
			bool int_pin = gpio_get(PIN_INT);
			bool miso_pin = gpio_get(PIN_MISO);
			bool rst_pin = gpio_get(PIN_RESET);
			if (debug_log_flag || valid_reports_count == 0) {
				s3910_log("s3910: heartbeat [%us]: INT(129)=%d (%s), MISO(16)=%d, RST(128)=%d, touching=%d, reports=%u, misses=%u\n",
				          (ticks * poll_us) / 1000000, int_pin, int_pin ? "idle high" : "LOW/TOUCH",
				          miso_pin, rst_pin, s3910_touching(), valid_reports_count, zero_reads);
			}
		}

		sleep_us(poll_us); /* idle wait — only reached when INT is not asserted */
	}
}

void
hakios_s3910_init(void)
{
	uint32_t enabled = 0;
	uint32_t s3910_arg = 0;
	thread_t thread;

	bool is_cliffs = false;
	uint32_t soc_smem = 0;
	char soc_arg[32] = { 0 };

	if (PE_parse_boot_argn("soc_smem", &soc_smem, sizeof(soc_smem)) && soc_smem == 0x81d00000) {
		is_cliffs = true;
	}
	if (PE_parse_boot_arg_str("soc", soc_arg, sizeof(soc_arg)) && strcmp(soc_arg, "cliffs") == 0) {
		is_cliffs = true;
	}
	if (hakios_soc_id != NULL && (hakios_soc_id() == 632 || hakios_soc_id() == 618)) {
		is_cliffs = true;
	}

	bool touch_disabled = false;
	if (PE_parse_boot_argn("touchscreen", &enabled, sizeof(enabled)) && !enabled) {
		touch_disabled = true;
	}
	if (PE_parse_boot_argn("touch_s3910", &s3910_arg, sizeof(s3910_arg)) && !s3910_arg) {
		touch_disabled = true;
	}
	if (touch_disabled) {
		return;
	}

	/* Auto-enable on Cliffs (OnePlus Nord 4) unless disabled; allow explicit touchscreen=1/touch_s3910=1 */
	if (!is_cliffs && !enabled && !s3910_arg) {
		return;
	}

	s3910_log("s3910: starting hardware touch driver thread for OnePlus Nord 4 (avalon)\n");
	if (kernel_thread_start(s3910_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
