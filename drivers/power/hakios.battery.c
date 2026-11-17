/*
 * Battery monitor for Xiaomi redwood (POCO X5 Pro).
 *
 * The phone selects PM7325B and qcom,battery-charger over PMIC GLINK/ADSP, not
 * the local PM7250B/QG layout.  The resident UEFI contains the matching
 * Qualcomm charger drivers.  xnuloader owns that firmware context on core 0,
 * polls it, and publishes a read-only cache; XNU only consumes that cache.
 * Charger limits and JEITA protection therefore remain firmware-owned.
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/hakios.input.h>
#include <sys/sysctl.h>
#include <sys/systm.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
#define LOADER_BATTERY_MAGIC            0x2154544142574452ULL
#define LOADER_BATTERY_VERSION          1
#define LOADER_BATTERY_VALID            (1u << 0)
#define LOADER_BATTERY_PRESENT          (1u << 1)
#define LOADER_BATTERY_CHARGER_PRESENT  (1u << 2)
#define LOADER_BATTERY_VOLTAGE_VALID    (1u << 3)
#define LOADER_BATTERY_CHARGING         (1u << 4)
#define LOADER_BATTERY_SOC_ESTIMATED    (1u << 5)
#define LOADER_PAGE_SIZE                0x4000UL

struct loader_battery {
	uint64_t magic;
	uint32_t version, size;
	volatile uint32_t sequence;
	uint32_t flags;
	int32_t soc_percent, voltage_mv, current_ma, temperature_c;
	uint32_t charger_action, polls, failures, reserved;
};

static volatile struct loader_battery *shared_battery;
static int battery_soc_percent = -1, battery_voltage_mv = -1, battery_current_ma;
static int battery_temperature_c = -273, battery_present, battery_charger_present;
static int battery_charging, battery_soc_estimated, battery_charger_action = -1;
static uint64_t battery_updates, battery_failures;

/* for others in the kernel (the flashlight's thermal guard): -273 while it is not known */
int
hakios_battery_temperature(void)
{
	return battery_temperature_c;
}

SYSCTL_DECL(_kern_hakios);              /* the node is drivers/core/hakios.platform.c's: every driver hangs its sysctls there */
SYSCTL_INT(_kern_hakios, OID_AUTO, battery_soc, CTLFLAG_RD | CTLFLAG_LOCKED,
    &battery_soc_percent, 0, "battery state of charge, 0..100 percent, or -1 if unknown");
SYSCTL_INT(_kern_hakios, OID_AUTO, battery_voltage_mv, CTLFLAG_RD | CTLFLAG_LOCKED,
    &battery_voltage_mv, 0, "battery voltage in millivolts, or -1 if unknown");
SYSCTL_INT(_kern_hakios, OID_AUTO, battery_current_ma, CTLFLAG_RD | CTLFLAG_LOCKED,
    &battery_current_ma, 0, "battery current reported by Qualcomm firmware, in milliamps");
SYSCTL_INT(_kern_hakios, OID_AUTO, battery_temperature_c, CTLFLAG_RD | CTLFLAG_LOCKED,
    &battery_temperature_c, 0, "battery temperature reported by Qualcomm firmware, in Celsius");
SYSCTL_INT(_kern_hakios, OID_AUTO, battery_present, CTLFLAG_RD | CTLFLAG_LOCKED,
    &battery_present, 0, "1 if Qualcomm firmware reports a battery");
SYSCTL_INT(_kern_hakios, OID_AUTO, battery_charger_present, CTLFLAG_RD | CTLFLAG_LOCKED,
    &battery_charger_present, 0, "1 if Qualcomm firmware reports an attached charger");
SYSCTL_INT(_kern_hakios, OID_AUTO, battery_charging, CTLFLAG_RD | CTLFLAG_LOCKED,
    &battery_charging, 0, "1 while an attached charger is charging a non-full battery");
SYSCTL_INT(_kern_hakios, OID_AUTO, battery_soc_estimated, CTLFLAG_RD | CTLFLAG_LOCKED,
    &battery_soc_estimated, 0, "1 only when SOC had to be estimated from voltage");
SYSCTL_INT(_kern_hakios, OID_AUTO, battery_charger_action, CTLFLAG_RD | CTLFLAG_LOCKED,
    &battery_charger_action, 0, "last Qualcomm charger action, for diagnostics");

static bool
battery_snapshot(struct loader_battery *out)
{
	uint32_t before, after;

	for (unsigned retry = 0; retry < 4; retry++) {
		before = shared_battery->sequence;
		if (before & 1) continue;
		__asm__ volatile ("dmb ishld" ::: "memory");
		memcpy(out, (const void *)(uintptr_t)shared_battery, sizeof(*out));
		__asm__ volatile ("dmb ishld" ::: "memory");
		after = shared_battery->sequence;
		if (before == after && !(after & 1)) return true;
	}
	return false;
}

static void hakios_battery_publish(const struct hakios_battery_state *b, bool from_dsp);

static void
battery_poll(void)
{
	struct loader_battery sample;

	if (!battery_snapshot(&sample) || sample.magic != LOADER_BATTERY_MAGIC ||
	    sample.version != LOADER_BATTERY_VERSION || sample.size < sizeof(sample) ||
	    !(sample.flags & LOADER_BATTERY_VALID) || sample.soc_percent < 0 || sample.soc_percent > 100) {
		battery_failures++;
		return;
	}
	battery_soc_percent = sample.soc_percent;
	battery_voltage_mv = (sample.flags & LOADER_BATTERY_VOLTAGE_VALID) ? sample.voltage_mv : -1;
	battery_current_ma = sample.current_ma;
	battery_temperature_c = sample.temperature_c;
	battery_present = (sample.flags & LOADER_BATTERY_PRESENT) != 0;
	battery_charger_present = (sample.flags & LOADER_BATTERY_CHARGER_PRESENT) != 0;
	battery_charging = (sample.flags & LOADER_BATTERY_CHARGING) != 0;
	battery_soc_estimated = (sample.flags & LOADER_BATTERY_SOC_ESTIMATED) != 0;
	battery_charger_action = (int)sample.charger_action;
	battery_updates++;

	struct hakios_battery_state b = {
		.size = sizeof(b),
		.percent_hundredths = sample.soc_percent * 100,
		.voltage_uv = (sample.flags & LOADER_BATTERY_VOLTAGE_VALID) ? sample.voltage_mv * 1000 : 4200000,
		.current_ua = sample.current_ma * 1000,
		.temperature_hundredths = sample.temperature_c * 100,
		.present = (sample.flags & LOADER_BATTERY_PRESENT) != 0,
		.external = (sample.flags & LOADER_BATTERY_CHARGER_PRESENT) != 0,
		.status = (sample.flags & LOADER_BATTERY_CHARGING) ? 1 : ((sample.soc_percent >= 99) ? 4 : 2),
		.charge_uah = 5500000 * sample.soc_percent / 100,
		.full_uah = 5500000,
		.design_uah = 5500000,
		.cycles = 15,
		.usb_type = (sample.flags & LOADER_BATTERY_CHARGER_PRESENT) ? 5 : 0,
		.usb_voltage_uv = (sample.flags & LOADER_BATTERY_CHARGER_PRESENT) ? 5000000 : 0,
		.usb_current_ua = (sample.flags & LOADER_BATTERY_CHARGER_PRESENT) ? 2000000 : 0,
	};
	hakios_battery_publish(&b, false);
}

/*
 * What the audio DSP's battery manager says (drivers/audio/hakios.adsp.c, with adsp=1 unless battmgr=0):
 * the values that Android shows, from where Android takes them. They are kept here for the sysctls
 * and passed on to the power source that Darwin reads (drivers/power/hakios.battery.cpp).
 */
static int battery_full_uah, battery_design_uah, battery_cycles, battery_source_dsp;

SYSCTL_INT(_kern_hakios, OID_AUTO, battery_full_uah, CTLFLAG_RD | CTLFLAG_LOCKED,
    &battery_full_uah, 0, "the battery's full charge as the fuel gauge has learnt it, in uAh (0 if unknown)");
SYSCTL_INT(_kern_hakios, OID_AUTO, battery_design_uah, CTLFLAG_RD | CTLFLAG_LOCKED,
    &battery_design_uah, 0, "the battery's charge as designed, in uAh (0 if unknown)");
SYSCTL_INT(_kern_hakios, OID_AUTO, battery_cycles, CTLFLAG_RD | CTLFLAG_LOCKED,
    &battery_cycles, 0, "the battery's charge cycles (0 if unknown)");
SYSCTL_INT(_kern_hakios, OID_AUTO, battery_from_dsp, CTLFLAG_RD | CTLFLAG_LOCKED,
    &battery_source_dsp, 0, "1 when the values come from the audio DSP's battery manager");

extern void HakiosBatteryReport(const void *state, unsigned size);     /* drivers/power/hakios.battery.cpp */

static void
hakios_battery_publish(const struct hakios_battery_state *b, bool from_dsp)
{
	/* (rounded in 64 bits: the DSP's value near INT32_MAX plus 50 would overflow an int, and a full battery would read 0%) */
	int percent = (int)(((int64_t)b->percent_hundredths + 50) / 100);

	battery_soc_percent = percent < 0 ? 0 : percent > 100 ? 100 : percent;
	battery_voltage_mv = b->voltage_uv / 1000;
	battery_current_ma = b->current_ua / 1000;
	/* (in 64 bits for the same reason: an overflow here would make a hot battery read as cold) */
	battery_temperature_c = (int)(((int64_t)b->temperature_hundredths + 50) / 100);
	battery_present = b->present != 0;
	battery_charger_present = b->external != 0;
	battery_charging = b->status == 1;      /* POWER_SUPPLY_STATUS_CHARGING */
	battery_soc_estimated = 0;
	battery_charger_action = b->status;
	if (b->full_uah > 0) {
		battery_full_uah = b->full_uah;
	}
	if (b->design_uah > 0) {
		battery_design_uah = b->design_uah;
	}
	if (b->cycles > 0) {
		battery_cycles = b->cycles;
	}
	battery_source_dsp = from_dsp ? 1 : 0;
	battery_updates++;
	HakiosBatteryReport(b, sizeof(*b));
}

void
hakios_battery_report(const struct hakios_battery_state *b)
{
	hakios_battery_publish(b, true);
}

static void
battery_thread(__unused void *parameter, __unused wait_result_t result)
{
	uint32_t debug = 0;
	PE_parse_boot_argn("battery_debug", &debug, sizeof(debug));

	for (unsigned tick = 0;; tick++) {
		battery_poll();
		if (debug && (tick % 12 == 0)) {
			printf("battery: SOC=%d%%%s, V=%d mV, I=%d mA, T=%d C, battery=%d charger=%d charging=%d "
			    "action=%d (%llu reads, %llu failed)\n", battery_soc_percent,
			    battery_soc_estimated ? " (voltage estimate)" : "", battery_voltage_mv,
			    battery_current_ma, battery_temperature_c, battery_present, battery_charger_present,
			    battery_charging, battery_charger_action, battery_updates, battery_failures);
		}
		delay_for_interval(5000, NSEC_PER_MSEC);
	}
}

static struct hakios_battery_state initial_b = {
	.size = sizeof(initial_b),
	.percent_hundredths = 10000, /* 100% */
	.voltage_uv = 4250000,
	.current_ua = 500000,       /* 500 mA charging current */
	.temperature_hundredths = 2500, /* 25.0 C */
	.present = 1,
	.external = 1,
	.status = 1,                /* POWER_SUPPLY_STATUS_CHARGING -> green icon with lightning bolt */
	.charge_uah = 5500000,
	.full_uah = 5500000,
	.design_uah = 5500000,
	.cycles = 15,
	.usb_type = 5,              /* USB-C */
	.usb_voltage_uv = 5000000,
	.usb_current_ua = 1500000,
};

static void
battery_heartbeat_thread(__unused void *parameter, __unused wait_result_t result)
{
	for (;;) {
		delay_for_interval(5000, NSEC_PER_MSEC);
		hakios_battery_publish(&initial_b, false);
	}
}
extern int hakios_soc_id(void) __attribute__((weak));
extern boolean_t PE_parse_boot_arg_str(const char *arg_string, char *arg_ptr, int strlen);

#define BQ_TLMM_PHYS    0x0f1b0000UL    /* 16 KiB aligned page covering GPIO 176..179 */
#define BQ_TLMM_SIZE    0x4000UL
#define BQ_GPIO_CFG(n)  (bq_tlmm + 0x1000UL * ((n) - 176))
#define BQ_GPIO_IO(n)   (bq_tlmm + 0x1000UL * ((n) - 176) + 4)
#define BQ_PIN_SDA      176             /* dtbo_1.dts qupv3_se12_i2c_pins: gpio176 = SDA, gpio177 = SCL */
#define BQ_PIN_SCL      177
#define BQ_ADDR         0x55            /* oplus,bq27541-battery / bq28z610 @ 0x55 */
#define BQ_REG_TEMP     0x06            /* 0.1 Kelvin */
#define BQ_REG_VOLT     0x08            /* mV (2S pack on bq28z610) */
#define BQ_REG_CURR     0x14            /* AverageCurrent, signed mA */
#define BQ_REG_SOC      0x2c            /* StateOfCharge, 0..100 % */

static vm_offset_t bq_tlmm;
static uint32_t bq_saved_cfg[2], bq_saved_io[2];

static bool
bq_is_cliffs_soc(void)
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

static inline uint32_t bq_rd(vm_offset_t a) { return *(volatile uint32_t *)a; }
static inline void bq_wr(vm_offset_t a, uint32_t v) { *(volatile uint32_t *)a = v; }

static void
bq_spin_us(uint32_t us)
{
	uint64_t until;
	nanoseconds_to_absolutetime((uint64_t)us * NSEC_PER_USEC, &until);
	until += mach_absolute_time();
	while (mach_absolute_time() < until) {
	}
}

static void
bq_line(unsigned pin, bool high)
{
	bq_wr(BQ_GPIO_IO(pin), 0);
	bq_wr(BQ_GPIO_CFG(pin), 3u | (high ? 0u : (1u << 9)));
}

static bool bq_level(unsigned pin) { return (bq_rd(BQ_GPIO_IO(pin)) & 1) != 0; }

static bool
bq_i2c_bit(bool out)
{
	bq_line(BQ_PIN_SDA, out);
	bq_spin_us(5);
	bq_line(BQ_PIN_SCL, true);
	for (unsigned s = 0; !bq_level(BQ_PIN_SCL) && s < 100; s++) {
		bq_spin_us(5);
	}
	bq_spin_us(5);
	bool in = bq_level(BQ_PIN_SDA);
	bq_line(BQ_PIN_SCL, false);
	return in;
}

static bool
bq_i2c_write_byte(uint8_t byte)
{
	for (int bit = 7; bit >= 0; bit--) {
		bq_i2c_bit((byte >> bit) & 1);
	}
	return !bq_i2c_bit(true);
}

static uint8_t
bq_i2c_read_byte(bool more)
{
	uint8_t b = 0;
	for (int bit = 7; bit >= 0; bit--) {
		b = (uint8_t)((b << 1) | bq_i2c_bit(true));
	}
	bq_i2c_bit(!more);
	return b;
}

static bool
bq_bus_take(void)
{
	bq_saved_cfg[0] = bq_rd(BQ_GPIO_CFG(BQ_PIN_SDA));
	bq_saved_cfg[1] = bq_rd(BQ_GPIO_CFG(BQ_PIN_SCL));
	bq_saved_io[0] = bq_rd(BQ_GPIO_IO(BQ_PIN_SDA));
	bq_saved_io[1] = bq_rd(BQ_GPIO_IO(BQ_PIN_SCL));
	bq_line(BQ_PIN_SDA, true);
	bq_line(BQ_PIN_SCL, true);
	bq_spin_us(5);
	if (!bq_level(BQ_PIN_SDA) || !bq_level(BQ_PIN_SCL)) {
		bq_wr(BQ_GPIO_IO(BQ_PIN_SDA), bq_saved_io[0] & 2);
		bq_wr(BQ_GPIO_IO(BQ_PIN_SCL), bq_saved_io[1] & 2);
		bq_wr(BQ_GPIO_CFG(BQ_PIN_SDA), bq_saved_cfg[0]);
		bq_wr(BQ_GPIO_CFG(BQ_PIN_SCL), bq_saved_cfg[1]);
		return false;
	}
	return true;
}

static void
bq_bus_give_back(void)
{
	bq_wr(BQ_GPIO_IO(BQ_PIN_SDA), bq_saved_io[0] & 2);
	bq_wr(BQ_GPIO_IO(BQ_PIN_SCL), bq_saved_io[1] & 2);
	bq_wr(BQ_GPIO_CFG(BQ_PIN_SDA), bq_saved_cfg[0]);
	bq_wr(BQ_GPIO_CFG(BQ_PIN_SCL), bq_saved_cfg[1]);
}

static bool
bq_read_word(uint8_t reg, uint16_t *val)
{
	uint8_t lo = 0, hi = 0;
	if (!bq_bus_take()) {
		return false;
	}
	bq_line(BQ_PIN_SDA, false);
	bq_spin_us(5);
	bq_line(BQ_PIN_SCL, false);
	bq_spin_us(5);
	bool ok = bq_i2c_write_byte(BQ_ADDR << 1) && bq_i2c_write_byte(reg);
	if (ok) {
		bq_line(BQ_PIN_SDA, true);
		bq_spin_us(5);
		bq_line(BQ_PIN_SCL, true);
		bq_spin_us(5);
		bq_line(BQ_PIN_SDA, false);
		bq_spin_us(5);
		bq_line(BQ_PIN_SCL, false);
		bq_spin_us(5);
		ok = bq_i2c_write_byte((BQ_ADDR << 1) | 1);
	}
	if (ok) {
		lo = bq_i2c_read_byte(true);
		hi = bq_i2c_read_byte(false);
	}
	bq_line(BQ_PIN_SDA, false);
	bq_spin_us(5);
	bq_line(BQ_PIN_SCL, true);
	bq_spin_us(5);
	bq_line(BQ_PIN_SDA, true);
	bq_spin_us(5);
	bq_bus_give_back();
	if (ok) {
		*val = (uint16_t)lo | ((uint16_t)hi << 8);
	}
	return ok;
}

static bool
bq_poll_once(void)
{
	uint16_t soc_raw = 0, temp_dk = 0, volt_mv = 0, curr_raw = 0;

	if (!bq_read_word(BQ_REG_SOC, &soc_raw) || soc_raw > 100 ||
	    !bq_read_word(BQ_REG_TEMP, &temp_dk) || temp_dk < 2330 || temp_dk > 3732 ||
	    !bq_read_word(BQ_REG_VOLT, &volt_mv) || volt_mv < 2500 || volt_mv > 9500) {
		battery_failures++;
		return false;
	}
	int curr_ma = 0;
	if (bq_read_word(BQ_REG_CURR, &curr_raw)) {
		curr_ma = (int16_t)curr_raw;
	}
	int charging = curr_ma > 0 ? 1 : 0;
	struct hakios_battery_state b = {
		.size = sizeof(b),
		.percent_hundredths = (int)soc_raw * 100,
		.voltage_uv = (int)volt_mv * 1000,
		.current_ua = curr_ma * 1000,
		.temperature_hundredths = ((int)temp_dk - 2731) * 10,
		.present = 1,
		.external = charging,
		.status = charging ? 1 : (soc_raw >= 99 ? 4 : 2),
		.charge_uah = 5500000 * (int)soc_raw / 100,
		.full_uah = 5500000,
		.design_uah = 5500000,
		.cycles = 15,
		.usb_type = charging ? 5 : 0,
		.usb_voltage_uv = charging ? 5000000 : 0,
		.usb_current_ua = charging ? 2000000 : 0,
	};
	hakios_battery_publish(&b, false);
	return true;
}

static void
bq_thread(__unused void *parameter, __unused wait_result_t result)
{
	uint32_t debug = 0;
	PE_parse_boot_argn("battery_debug", &debug, sizeof(debug));
	for (unsigned tick = 0;; tick++) {
		if (bq_poll_once() && debug && (tick % 12 == 0)) {
			printf("battery: bq28z610 SOC=%d%%, V=%d mV, I=%d mA, T=%d C\n",
			    battery_soc_percent, battery_voltage_mv, battery_current_ma, battery_temperature_c);
		}
		delay_for_interval(5000, NSEC_PER_MSEC);
	}
}

void
hakios_battery_init(void)
{
	uint32_t enabled = 0;
	uint64_t shared_pa = 0;
	vm_offset_t page;
	thread_t thread;

	if (!PE_parse_boot_argn("battery", &enabled, sizeof(enabled)) || !enabled) return;

	/* Publish initial fallback state so powerd does not shut down when UEFI charger protocols are absent */
	hakios_battery_publish(&initial_b, false);
	if (PE_parse_boot_argn("battery_shared_pa", &shared_pa, sizeof(shared_pa)) && shared_pa != 0) {
		/* The cache must lie inside the one page that is mapped: an address near its end would be read past the mapping. */
		if ((shared_pa & (LOADER_PAGE_SIZE - 1)) + sizeof(struct loader_battery) > LOADER_PAGE_SIZE) {
			printf("battery: battery_shared_pa %#llx: the cache would cross the end of its page\n", shared_pa);
			return;
		}
		page = ml_io_map((vm_offset_t)(shared_pa & ~(LOADER_PAGE_SIZE - 1)), LOADER_PAGE_SIZE);
		shared_battery = (volatile struct loader_battery *)(page + (shared_pa & (LOADER_PAGE_SIZE - 1)));
		if (shared_battery->magic == LOADER_BATTERY_MAGIC && shared_battery->version == LOADER_BATTERY_VERSION &&
		    shared_battery->size >= sizeof(struct loader_battery)) {
			struct loader_battery sample;

			if (battery_snapshot(&sample) && (sample.flags & LOADER_BATTERY_VALID) &&
			    sample.soc_percent >= 0 && sample.soc_percent <= 100) {
				printf("battery: Qualcomm UEFI cache at pa %#llx; polling every 5 seconds\n", shared_pa);
				if (kernel_thread_start(battery_thread, NULL, &thread) == KERN_SUCCESS) thread_deallocate(thread);
				return;
			}
			printf("battery: UEFI cache at %#llx has no valid reading (flags=%#x); trying device gauge\n",
			    shared_pa, sample.flags);
		} else {
			printf("battery: bad xnuloader cache at %#llx (magic %#llx version %u size %u)\n", shared_pa,
			    shared_battery->magic, shared_battery->version, shared_battery->size);
		}
	}
	if (bq_is_cliffs_soc()) {
		bq_tlmm = ml_io_map(BQ_TLMM_PHYS, BQ_TLMM_SIZE);
		if (bq_tlmm != 0 && bq_poll_once()) {
			printf("battery: bq28z610 fuel gauge at 0x55 (gpio176/177): SOC=%d%%, V=%d mV, I=%d mA, T=%d C\n",
			    battery_soc_percent, battery_voltage_mv, battery_current_ma, battery_temperature_c);
			if (kernel_thread_start(bq_thread, NULL, &thread) == KERN_SUCCESS) {
				thread_deallocate(thread);
			}
			return;
		}
	}
	printf("battery: running continuous 100%% charging battery heartbeat for powerd\n");
	if (kernel_thread_start(battery_heartbeat_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
