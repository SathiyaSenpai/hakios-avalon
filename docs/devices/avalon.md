<!-- Made by tools/docs-gen.py from devices/avalon/device.conf. Do not edit: change that, then run tools/docs-gen.py. -->


# OnePlus Nord 4 (`avalon`)

| | |
|---|---|
| Vendor | OnePlus |
| SoC | `cliffs` |
| Configuration | `make avalon_defconfig` |
| Maintainer | sathiya |
| Repository | [`devices/avalon/`](https://github.com/SathiyaSenpai/hakios-sm7675/tree/main/devices/avalon) |
| Firmware | `devices/avalon/pull-firmware.sh` pulls it from your own phone |

## What works

| Part | State | |
|---|---|---|
| display | works | the panel through continuous splash framebuffer at 0xe3940000 (1240x2772, 120Hz) |
| touch | works | Synaptics S3910 on QUPv3 SE4 SPI (CONFIG_HAKIOS_S3910, touchscreen=1); RPMh ACTIVE_TCS3, APP_INFO, and TouchComm v1 parser |
| buttons | works | power and volume through PMK8550 PON & PMXR2230 GPIO6 (CONFIG_HAKIOS_BUTTONS) |
| cpu | works | all 8 Kryo cores (3 A520, 4 A720, 1 X4) via EPSS v2 with thermal DVFS throttling; bootcpu=7 cpus=8 default |
| thermal | works | 34/35 active TSENS v2 sensors across 3 blocks (CONFIG_HAKIOS_THERMAL, thermal=1) + DVFS feedback |
| haptic | works | Awinic AW86927 LRA on QUPv3 SE3 I2C @ 0x5a, gpio 12/13/145/147 (haptic=5) |
| battery | partly | BQ28Z610 I2C fuel gauge probe on gpio176/177 @ 0x55 with powerd fallback when UEFI charger absent |
| storage | partly | UFS read-only survey via UEFI block service (ufs=1/2); native UFS HC unported |
| gpu | no | Adreno 732 (gen7-11-0 @ 0x3d00000); hakios-metal targets Adreno 642L and Cliffs safely falls back to software rendering |
| wifi | no | WCN6750/Kiwi not yet ported for avalon |
| audio | no | Cliffs ADSP/LPASS not yet ported |
| cellular | no | Cliffs MPSS (0x8bc00000); soc.args populated, not yet ported |

The state is what has been seen on this device, written down in `devices/avalon/device.conf`; a part the table does not name has not been looked at.
