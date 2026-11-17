# Device: Qualcomm Technologies, Inc. Cliffs SoC (avalon)

Made by `tools/new-device.py` on 2026-11-03, from the file avalon_merged.dtb. Edit it as the port goes.

| What | Value |
|---|---|
| Board (the tree's `model`) | Qualcomm Technologies, Inc. Cliffs SoC |
| Compatible | `qcom,cliffs` |

## The SoC addresses

Reference device tree: `devices/avalon/prebuilt/device-tree.dtb`.

| Argument | Value | Against the reference |
|---|---|---|
| `soc_ipa_base` | `0x3e40000` |  |
| `soc_gsi_base` | `0x3e04000` |  |
| `soc_ipcc` | `0x406000` |  |
| `soc_aoss` | `0xc300000` | AOP QMP mailbox (`power-controller@c300000`) |
| `soc_rsc` | `0x17a20000` | APPS RSC DRV2 (`qcom,tcs-config` ACTIVE=TCS3..4) |
| `soc_apps_smmu` | `0x15000000` |  |
| `soc_cmd_db` | `0x81c60000` | `aop_cmd_db_region@81c60000` |
| `soc_mpss_mem` | `0x8bc00000` | `mpss_region@8bc00000` |
| `modem_ipa_fw_at` | `0x98980000` | `ipa_fw_region@98980000` |
| `soc_smem` | `0x81d00000` | `smem_region@81d00000` (size `0x200000`) |
| `soc_imem` | `0x14600000` |  |
| `soc_ipa_stream` | `0x4a0` |  |
| `soc_ipa_stream_uc` | `0x4a2` |  |
| `soc_pas_mpss` | `(not found)` | optional; omitted when the DTB lacks an explicit `qcom,pas-id` on the remoteproc node |
| `soc_pas_ipa` | `0xf` |  |
| `soc_ipa_fw` | `ipa_fws` |  |

`soc.args` is what `tools/soc-args.py` read out of the tree. The run scripts add it to the kernel command line.

## What is this phone's own

The kernel, loader and user space know nothing about a phone: addresses, interrupts, rails and firmware
come from its tree and its Android. What is written for one phone is the list below. The head of each file
says where its numbers came from; that is the map of what to rewrite.

- [x] **UEFI**: U-Boot boot image with continuous splash framebuffer at `0xe3940000` (`1240 x 2772`)
- [x] **Display, buttons, touch**: display framebuffer (`1240x2772`), buttons (`PMK8550 PON` + `PMXR2230 GPIO6`), and Synaptics S3910 touch (`drivers/input/hakios.s3910.c`, QUPv3 SE4 SPI, RPMh ACTIVE_TCS3, `APP_INFO`, TouchComm v1)
- [ ] **Storage**: `hakios.ufs.c` works read-only via UEFI (`ufs=1/2`); native `hakios.ufshc.c` unported
- [x] **CPU clocks & Thermal**: EPSS v2 (`0x17d91000`, `PSTATE_STATUS` `0x3c4`) + 35 TSENS v2 sensors (`0xc228000`..`0xc22a000`) with closed-loop thermal DVFS throttling (`bootcpu=7 cpus=8`)
- [ ] **GPU**: Adreno 732 (`0x3d00000`, `gen7-11-0`); `hakios-metal` currently targets Adreno 642L (`0x06030500`) and safely refuses `gpu=1`/`adreno=` on Cliffs
- [ ] **Wi-Fi**: WCN6750/Kiwi not yet ported
- [ ] **Audio and battery**: Cliffs ADSP/audio not yet ported; `hakios.battery.c` probes BQ28Z610 I2C fuel gauge (`gpio176/177 @ 0x55`) with fallback for `powerd`
- [ ] **Cellular**: `soc.args` populated from stock DTB (`0x8bc00000`, `0x98980000`); MPSS/IPA not yet ported
- [x] **Haptics**: Awinic AW86927 on QUPv3 SE3 I2C (`0x5a`, gpio 12/13/145/147)

## Configuration

The kernel's settings are layered the way an Android kernel's are: the common kernel, the SoC's, this phone's
(`docs/configs.md`). Two short files make this phone a configuration:

    configs/avalon.config          CONFIG_DEVICE="avalon"   and whatever is true of this phone only
    configs/avalon_defconfig       #include common_defconfig
                                   #include cliffs.config
                                   #include avalon.config

Then `make avalon_defconfig`. Turn on only what this phone has been shown to do; `tools/hakios-config.py check` says what the
choice needs.

The drivers are part of the configuration too (`docs/drivers-config.md`): every one is built unless a configuration leaves it out,
and the addresses, pins and rails must match the target hardware. Leave out in `configs/avalon.config` what has not been
ported to this phone yet (`# CONFIG_HAKIOS_GOODIX is not set`, ...), and put
each one back when it has been made to work here. `tools/hakios-config.py show` lists what is built in.

## Firmware

Pulled from your own phone or vendor dump, never committed: `devices/avalon/pull-firmware.sh`. Check with `make doctor`.

## Before you open a pull request

`tools/provenance-check.py` must pass: no vendor firmware, no UEFI drivers, no Apple files in the tree.

## The repository

A HakiOS device repository: what is true of this device and of no other. The kernel, the loader, the drivers and the tools are
the common repository's; the cliffs family's facts are in `soc/`, carried with the device. How the three are put together:
[docs/porting/repositories.md](https://github.com/oosexgang/hakios-xnu-kernel/blob/main/docs/porting/repositories.md).

| File | What |
|---|---|
| `device.conf` | what the device is, and how far each part of it works: its page in the documentation is made from it |
| `configs/avalon_defconfig` | the common kernel, the SoC's fragment, this device's: `make avalon_defconfig` |
| `configs/avalon.config` | what is this device's own: the drivers built in, the arguments it needs |
| `soc/` | its SoC family's layer (cliffs): `soc.conf`, `configs/cliffs.config`, the SoC's drivers; carried here so that this one repository is all the device needs |
| `drivers/` | drivers of its own, if it needs any (`drivers/<subsystem>/conf`, as in the common repository) |

Every phone driver of the common repository is left out to begin with: their addresses and pins are the phone they were written
for. Put one back in `configs/avalon.config` when it has been made to work here, and say so in `device.conf`.

Check with `tools/hakios-device.py check avalon`.
