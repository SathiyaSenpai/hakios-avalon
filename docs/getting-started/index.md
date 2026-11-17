# Getting started

Everything needed to build and boot HakiOS on the **OnePlus Nord 4 (`avalon`, SM7675 Cliffs)** from RAM via `fastboot boot`.

| | What you get | What you need |
|---|---|---|
| [**On the phone**](phone.md) | XNU + Darwin user space on OnePlus Nord 4, in RAM | unlocked bootloader, `adb` and `fastboot`, your own IPSW |
| [**Building from source**](building.md) | kernel, loader, user space images | cross toolchain + iPhoneOS 16.5 SDK |

## Quick start

```sh
git clone https://github.com/SathiyaSenpai/hakios-sm7675.git hakios
cd hakios
make doctor           # what this machine lacks, and what to do
make avalon_defconfig # OnePlus Nord 4 configuration: common + cliffs.config + avalon.config
make kernel           # the kernel with the drivers that configuration builds
make phone SECS=600   # a 10-minute session on the phone, in RAM
```

The SoC, device and UEFI repositories, at the revisions `devices/manifest.conf` names:

```sh
tools/hakios-device.py sync
```

## Your machine

| Host | What works | Notes |
|---|---|---|
| Linux x86-64 | everything | the reference: CI runs here |
| macOS (Apple silicon) | building the kernel, the loader, user space; the tests | QEMU from Homebrew for running; the phone through `adb`/`fastboot` |
| Windows | everything, split between WSL 2 and Git Bash | [Windows](../windows.md) says what runs where |

`make doctor` checks the machine (and a phone on `adb`) for what HakiOS needs, and says what to install.

## Where to go next

- [Architecture](../concepts/architecture.md): how a boot goes, from UEFI to the home screen, and which part does what.
- [Configuration](../configs.md): defconfigs, fragments, and what a `CONFIG_` symbol turns into.
- [Port a device](../porting/index.md): your own phone, step by step.
