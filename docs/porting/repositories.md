# How the repositories fit

HakiOS is put together from layers, the way an Android device's source is (`device/<vendor>/<name>`, a `<soc>-common`, a manifest
that names them):

```
hakios/                          common: the kernel port, every common driver, the loader, user space, the tools, the docs
├── devices/<name>/              a device, one repository: device.conf, configs/, drivers/, its UEFI platform, scripts
│   └── soc/                     ... and its SoC family's layer: soc.conf, configs/<soc>.config, the SoC's drivers
├── uefi/                        HakiOS's UEFI (Mu-Silicium with HakiOS's changes), one for every device
└── devices/manifest.conf        every device and the UEFI: kept in this tree, or a repository of its own at a revision
```

A SoC or device directory is the same whether it is kept in the common tree (`devices/avalon` is) or is a git repository of its own
that `tools/hakios-device.py sync` clones into place: the tools look for `socs/*/soc.conf` and
`devices/*/device.conf`, whatever put them there. The common repository ignores what `sync` clones.

## A device repository

| File | What | Required |
|---|---|---|
| `device.conf` | what the device is, and how far each part works | yes |
| `configs/<name>_defconfig` | `#include common_defconfig`, `#include <soc>.config`, `#include <name>.config` | yes |
| `configs/<name>.config` | what is this device's own: drivers left out or in, arguments, `CONFIG_DEVICE="<name>"` | yes |
| `drivers/<subsystem>/conf` and the files it names | drivers of its own ([Writing a driver](drivers.md)) | no |
| `soc/` | its SoC family's layer: `soc.conf`, `configs/<soc>.config`, the SoC's drivers ([below](#the-socs-layer)) | yes, unless `soc none` |
| `pull-firmware.sh` | the device's firmware, from your own device, into `fw-*/` | for a phone |
| `uefi/`, `prebuilt/` | the device's UEFI port and the images made of it | for a phone |
| `README.md` | anything else a person needs to know | yes |

### `device.conf`

```
device avalon
	title       OnePlus Nord 4
	vendor      OnePlus
	soc         cliffs
	defconfig   avalon_defconfig
	firmware    pull-firmware.sh
	maintainer  sathiya
	status      display     works     the panel through continuous splash framebuffer at 0xe3940000 (1240x2772, 120Hz)
	status      touch       works     Synaptics S3910 on QUPv3 SE4 SPI (CONFIG_HAKIOS_S3910, touchscreen=1)
	status      cellular    missing   Cliffs MPSS (0x8bc00000); soc.args populated, not yet ported
```

| Word | Means |
|---|---|
| `title`, `vendor` | what it is called |
| `soc` | the SoC family: `socs/<soc>/` or `devices/<name>/soc/` must be there |
| `defconfig` | its defconfig, in its `configs/` |
| `firmware` | the script that pulls its firmware |
| `maintainer` | who answers for it |
| `status <area> works\|partial\|missing <note>` | how far a part works, **as seen on this device**; the note says how it is turned on and what is not done. Areas: display, touch, buttons, cpu, gpu, storage, audio, battery, haptic, camera, torch, wifi, bluetooth, cellular, network, thermal, usb, sensors |

The device's page in this documentation ([avalon](../devices/avalon.md)) and the table of [all devices](../devices/index.md)
are made from it by `tools/docs-gen.py`: the status is written once, next to the code it describes.

## The SoC's layer

A device carries its SoC family's layer with it, in `soc/`, so that one repository is all a device needs:

```
soc cliffs
	title       Qualcomm SM7675 Cliffs
	vendor      Qualcomm
	cores       8, the biggest is core 7
	gpu         Adreno 732
	maintainer  sathiya
```

and `soc/configs/<soc>.config`, the SoC's facts: `CONFIG_SOC`, its cores, and **the drivers of the SoC family**
(`CONFIG_HAKIOS_CPUFREQ=y`, `CONFIG_HAKIOS_THERMAL=y`, ...), so that the device has them whatever it leaves out of its own.

Two devices with the same SoC each carry a copy. The copies must be the same: the tools take the SoC once, and refuse two copies that
differ, naming both, because a build would otherwise depend on which device was checked out. `tools/hakios-device.py new --soc cliffs`
copies the layer from a device that has it; a SoC nobody has yet gets a new layer to fill in. A SoC many devices share can also be a
repository of its own in `socs/<soc>/` (a `soc` block in the manifest); the tools read both.

## The manifest

`devices/manifest.conf` names every SoC and device:

```
device avalon
	path        devices/avalon

device example
	repository  https://github.com/<you>/hakios-device-example.git
	revision    0123456789abcdef0123456789abcdef01234567
device sheng
	repository  https://github.com/<you>/hakios-device-sheng.git
	revision    0123456789abcdef0123456789abcdef01234567
```

| Command | Does |
|---|---|
| `tools/hakios-device.py list` (`make devices`) | every device and SoC, where it is, how much of it works |
| `tools/hakios-device.py check [NAME...]` | a repository holds together: its conf reads, its defconfig includes the common kernel and its SoC's fragment and passes `hakios-config.py check`, its drivers' files are there, no two repositories have a configuration of the same name |
| `tools/hakios-device.py new NAME --soc SOC --title ... [--fdt FILE] [--repo]` | a whole new device repository, its SoC layer with it; `--repo` makes it a git repository ready to push ([Port a device](index.md)) |
| `tools/hakios-device.py sync [NAME...]` | clone or update the repositories that live elsewhere, at their revisions |
| `tools/hakios-device.py lock NAME` | write the revision checked out into the manifest |
| `tools/hakios-device.py split NAME DIR` | a device kept in this tree, as a git repository of its own with its history, ready to push |

## How a device's own driver reaches the kernel

A driver in `devices/<name>/drivers/<subsystem>/conf` is a `CONFIG_HAKIOS_<NAME>` symbol like the common ones. `make kernel` copies
its files into the kernel tree (`xnu/sync-drivers.sh`, through `tools/hakios-config.py external-files`) and writes it into XNU's
`conf/files` as `optional`; `xnu/export.sh` leaves it out of the common patch, because it is the device's. A device repository
cannot declare `standard` files: what every kernel has is the common repository's to decide.

## Publishing a device

1. `tools/hakios-device.py check NAME` passes.
2. `tools/hakios-device.py split NAME ../hakios-device-NAME` makes it a repository of its own.
3. Push it to a repository named `hakios-device-NAME` (or `hakios-soc-SOC` for a SoC).
4. In a pull request to the common repository: its block in `devices/manifest.conf` with `repository` and `revision`
   (`tools/hakios-device.py lock NAME` writes it), and nothing else.

## Compared with others

| | Android | UniXNU | HakiOS |
|---|---|---|---|
| the shared part | AOSP, the GKI kernel | `UniXNU` (tools, components, ARM64 support) | `hakios` (the kernel port, drivers, loader, tools) |
| a SoC | `<soc>-common` | `UniXNU-<soc>-common` (`soc.toml`) | `soc/` inside each device repository (`soc.conf`); `socs/<soc>` for a shared one |
| a device | `device/<vendor>/<name>` | `UniXNU-<name>` (`board.toml`) | `devices/<name>` (`device.conf`) |
| what names them | `repo` manifest | `devices.toml` | `devices/manifest.conf` |
| a driver | kernel module, `Kconfig` | a component repository | `drivers/<sub>/conf`, an XNU `option` |
| the status of a device | nothing of the kind | a page per device | `status` lines in `device.conf`, the page made from them, checked in CI |
