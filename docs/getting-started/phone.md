# On the phone

The supported device is the **OnePlus Nord 4** (`avalon`, SM7675 Cliffs): [its page](../devices/avalon.md). Another
phone needs a port first: [Port a device](../porting/index.md).

## What happens to the phone

Nothing is installed. The loader goes into RAM with `fastboot boot`, the system image is sent into RAM, a session runs for as long
as you say, and a reboot brings the phone back to its own Android. The one thing written by default is the loader's own log, a
few KB, to the phone's `logdump` partition. With `ufs_write=1` (off unless you ask) the kernel writes to a partition named `darwin`
that you create yourself, and nowhere else. [What is written, exactly](../concepts/architecture.md#what-is-written-to-the-phone).

## What you need

- the phone, its boot loader unlocked;
- `adb` and `fastboot` on the PC (Google's platform-tools; on Windows, see [Windows](../windows.md));
- the user space images (`userland/root.apfs` and `userland/system.apfs`) built via `userland/build.sh` from your own IPSW, and `make loader` done once;
- the phone's own firmware, pulled from your vendor dump or phone (`devices/avalon/pull-firmware.sh`). It is never in the repositories.
- HakiOS's U-Boot (`devices/avalon/prebuilt/u-boot.bin`, [U-Boot](../u-boot.md)), booted from RAM via `fastboot boot` (never flashed).

## Step by step

```sh
devices/avalon/pull-firmware.sh     # optional: extract S3910 touch & GPU firmware -> fw-*/
make avalon_defconfig               # the phone's configuration: common, cliffs, avalon
make config                         # what it is: the drivers built in, the kernel's arguments, the firmware it wants
make phone SECS=600                 # a 10-minute session with the .config's arguments
```

`make phone` checks the `.config` and runs `tools/modem-cycle.sh`: it waits for the phone, enters the loader, uploads the system
image, runs the kernel with the configuration's arguments and summarises the log in `logs/`. The phone is never flashed. It is left in our
loader for the next run; `XL_END=android make phone ...` sends it back to Android at the end.

The same by hand, one step at a time:

```sh
./xl enter                          # Android -> fastboot -> our loader in RAM
./xl ramdisk userland/system.apfs   # the system image into the phone's RAM (about 80 s), once per loader session
XL_SECS=600 XL_CMDLINE="$(tools/hakios-config.py args)" ./xl run     # a session; the log goes to logs/
./xl reboot                         # back to Android
```

## The arguments that matter

A configuration sets them for you (`CONFIG_GPU=y` is `gpu=1`); [every argument](../boot-args.md) is listed with what reads it.

| Argument | What it does |
|---|---|
| `cpus=8 bootcpu=7 cpufreq=1` | all eight Kryo cores, X4 prime core (CPU 7) at speed |
| `touchscreen=1 touch_s3910=1` | Synaptics S3910 SPI touchscreen and hardware buttons |
| `fb_hz=120 fbscale=2 ca_vbl=1` | 120 Hz display pacing and `2x` display scaling (`1240x2772` panel) |
| `haptic=5` | Awinic AW86927 LRA vibration motor (`0x5a`) |
| `battery=1` | BQ28Z610 I2C fuel gauge probe with `powerd` fallback |
| `var_store=1 ufs_write=1` | keep `/private/var` across reboots (**writes** to the partition `darwin`) |
| `thermal=1 thermal_stop=95` | read 35 TSENS v2 sensors across 3 blocks (`thermal_stop=95` hands back to loader at 95 °C) |
| `battery_stop=15` | hand back to the loader before the battery is flat |
## When something goes wrong

| You see | Do |
|---|---|
| a session that does not end | `./xl` leaves its log in `logs/` and brings the phone back from the boot loader, or tells you which keys to press |
| the phone getting hot | with `thermal=1` (`avalon.config` sets `thermal_stop=95`) the run is handed back to the loader at 95 °C; `thermal_stop=N` changes the limit |
| a run left alone on battery | `battery_stop=<percent>` ends it before the battery is flat |
| a problem in `make config` | it names the symbol and why: a driver left out that an argument needs, firmware the open profile does not load |
| anything else | the log in `logs/`, and `make doctor` with the phone on `adb` |
