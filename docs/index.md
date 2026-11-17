# HakiOS

**Apple's open-source Darwin kernel (XNU) on the OnePlus Nord 4 (`avalon`, Qualcomm SM7675 Cliffs)**, with a boot loader of its
own, drivers of its own, U-Boot chain and a Linux-style configuration system.

**Where it stands.** OnePlus Nord 4 (`avalon`) boots XNU + Darwin 16.5 user space from RAM via `fastboot boot` using HakiOS U-Boot
and `xnuloader`, with 120 Hz continuous splash display (`1240x2772`), Synaptics S3910 SPI touchscreen, PMIC keys, all 8 Kryo cores
with EPSS v2 + TSENS thermal DVFS, Awinic AW86927 haptics, and BQ28Z610 battery probing.


## Guide: run it

| Page | What |
|---|---|
| [Getting started](getting-started/index.md) | what your machine needs and how to build and boot on OnePlus Nord 4 |
| [On the phone](getting-started/phone.md) | OnePlus Nord 4 (`avalon`), in RAM with `fastboot boot`, back to Android with a reboot |
| [Windows](windows.md) | the same from a Windows PC: what runs in WSL 2, what in Git Bash |
| [Installing beside Android](dual-boot.md) | HakiOS as a second system, a menu at every start, your data kept |
| [Building from source](getting-started/building.md) | when you change something |

## Porting: bring a device

| Page | What |
|---|---|
| [Port a device](porting/index.md) | a device is a repository of its own over the common one: a defconfig, a status file, its drivers |
| [How the repositories fit](porting/repositories.md) | common, SoC, device, UEFI, and the manifest that names them |
| [Writing a driver](porting/drivers.md) | a driver as an option of the kernel's own configuration |
| [Drivers in the configuration](drivers-config.md) | `CONFIG_HAKIOS_*`, and how a choice reaches XNU |

## Drivers and GPU: what each does, and how far

| Page | What |
|---|---|
| [Metal on the Adreno](reference/metal.md) | the Metal paths: interfaces, build, boot arguments, how to measure, known gaps |
| [Metal measurements](reference/metal-measurements.md) | Metal measurements and notes |
| [Metal plan](metal-plan.md) | architecture and milestones |
| [The shader compiler](air.md), [its report](air-report.md) | Apple's shader bitcode into the Adreno's code |
| [Devices](devices/index.md) | how far each part works: [avalon](devices/avalon.md) |
| [Cellular](cellular.md) | the modem bring-up architecture |
| [Driver reviews](driver-review/README.md) | each driver read for what it does and what was and was not verified |
| [Network review](net-review.md) | the network stack, read the same way |

## Concepts: how it fits together

| Page | What |
|---|---|
| [Architecture](concepts/architecture.md) | the three programs between a phone's firmware and a user space; what is written to the phone |
| [Configuration](configs.md) | defconfigs and fragments, like Linux's |
| [Versioning](versioning.md) | a version string that says what it is built from |
| [UEFI boot](uefi-boot.md), [U-Boot](u-boot.md) | the two firmwares that can start the loader |
| [Installing: the design](install.md), [safely](install-safe.md) | what installing means here, and what it must never do |

## Reference: look something up

| Page | What |
|---|---|
| [Drivers](reference/drivers.md) | every driver, made from the source |
| [Boot arguments](boot-args.md) | every argument any part of the system reads, made from the source |
| [Commands](reference/tools.md) | `make` targets and the scripts below them |

## The project

| Page | What |
|---|---|
| [Changelog](../CHANGELOG.md) | what changed, newest first |
| [Comparison](comparison.md) | HakiOS next to the other XNU-on-a-phone projects |
| [Legal review](legal-review.md), [LEGAL](../LEGAL.md), [compliance](../COMPLIANCE.md), [third parties](../THIRD_PARTY_LICENSES.md) | what is and is not distributed, and what is still to verify |
| [Contributing](../CONTRIBUTING.md), [community](../COMMUNITY.md) | how to take part |

## What works

As each device's own `device.conf` records it: [all devices](devices/index.md). Where the Metal row is ahead of a device's file, the
measurements are the source. The rows about the lock screen and the apps describe what happens when you add layer 2 below yourself.

| | OnePlus Nord 4 (`avalon`, SM7675) |
|---|---|
| Lock screen, home screen, Control Center, the stock apps | yes (software rendering / CoreAnimation) |
| Display | yes: continuous splash framebuffer at `0xe3940000` (`1240x2772`, 120 Hz, `fb_hz=120 ca_vbl=1`) |
| Touch, buttons | yes: Synaptics S3910 SPI on QUPv3 SE4 (`touch_s3910=1 touchscreen=1`), PMK8550/PMXR2230 keys |
| All 8 CPU cores | yes: 3 A520 + 4 A720 + 1 X4 via EPSS v2 + TSENS thermal DVFS (`cpus=8 bootcpu=7 cpufreq=1`) |
| Thermal sensors | yes: 34/35 active TSENS v2 sensors across 3 blocks (`thermal=1`) |
| Haptics | yes: Awinic AW86927 on QUPv3 SE3 I2C @ `0x5a` (`haptic=5`) |
| Battery | partial: BQ28Z610 I2C fuel gauge probe on `gpio176/177 @ 0x55` (`battery=1`) |
| Storage | partial: UFS read-only via UEFI block service (`ufs=1`/`2`); native UFS HC unported |
| GPU | no: Adreno 732 (`gen7-11-0 @ 0x3d00000`) not yet supported by `hakios-metal` (falls back to software rendering) |
| Wi-Fi, audio, cellular, bluetooth | no: not yet ported for `avalon` |

## The three layers

1. **The open core**: the kernel port, the loader, the drivers, the tools. All source, in these repositories.
2. **What you put on top**: the kernel can start a user space that you supply. It was built so that it can start the Darwin 16.5 user
   space, which you lay out yourself from files you download from Apple; nothing of it is shipped, and nothing is circumvented to get
   it. Whether you may do that, and whether you want to, is your decision.
3. **Apple's services** (activation, Apple ID, iCloud, the App Store): not supported and not a goal.

By default the loader goes into RAM through `fastboot boot`, the system lives in RAM for a session, and a reboot brings the phone back to
its own Android. The one thing written by default is the loader's own log (a few KB) to the `logdump` partition; with `ufs_write=1` the
kernel writes only to a partition `darwin` that you create yourself. What is and is not written to a phone:
[Architecture](concepts/architecture.md#what-is-written-to-the-phone).

## What is in the repository, and what never is

In it: only our own code and data: the patch to Apple's open-source XNU (`xnu/hakios.patch`, APSL 2.0) and our files, the boot loader, our
programs and libraries, the tools, `prebuilt/` (built from this source; the loader **without** any Apple data: `make loader` makes the one
that starts Darwin, on your machine, from your own IPSW), and the device and SoC directories. Third parties' files and patches to their
code are listed with their licenses in [THIRD_PARTY_LICENSES.md](../THIRD_PARTY_LICENSES.md).

Never in it: any file of Darwin (you build the system image on your machine from an IPSW you get yourself and hand to `sysimage/unpack.sh`;
nothing is downloaded for you, decrypted or circumvented); the phone's firmware (pulled from your own phone into `fw-*/`, which git
ignores); keys, certificates, activation, or anything that would make the phone pass as an iPhone to Apple.

The core kernel, the boot loader and the open tools build with nothing of Apple's. Booting the stock Darwin user space (`make phone`, `tools/boot-avalon.sh`) needs the IPSW and the phone's firmware, which you supply from your own device. Licenses: the XNU patch under the Apple Public Source License 2.0, the terms met as [COMPLIANCE.md](../COMPLIANCE.md) says; the rest as the file headers say. None of this is legal advice.

## The repositories

| Repository | What |
|---|---|
| `hakios-sm7675` | **OnePlus Nord 4 (`avalon`)**: the kernel port, drivers, loader, user space, tools, this documentation, and `devices/avalon` |
| [`hakios-metal/`](https://github.com/SathiyaSenpai/hakios-sm7675/tree/main/hakios-metal) | Metal's command model on the Adreno, through the kernel's GPU driver |
| [hakios-uefi](https://github.com/oosexgang/hakios-uefi) | HakiOS's U-Boot base (`u-boot/`) |
| [hackios-xnu-userland](https://github.com/oosexgang/hackios-xnu-userland) | old: a pointer. User space is `userland/` in this repository |

`devices/manifest.conf` in the common repository names the device and UEFI repositories and the revision each is at;
`tools/hakios-device.py sync` clones them into `devices/` and `uefi/`. One repository per device: it carries its SoC's layer, and
`tools/hakios-device.py new` makes one from the phone's own device tree, with every phone driver left out until it is ported. Each driver is
an option of the kernel's own configuration (`CONFIG_HAKIOS_*`, XNU's `options` and `conf/files`), so a device's defconfig builds exactly the
drivers it has. [How the repositories fit](porting/repositories.md).
