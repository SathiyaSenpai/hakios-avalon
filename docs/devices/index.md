<!-- Made by tools/docs-gen.py from devices/*/device.conf and socs/*/soc.conf. Do not edit: change that, then run tools/docs-gen.py. -->


# Devices

Every device with a repository, and how far each part of it works. A device of yours: [Port a device](../porting/index.md).

| Device | SoC | display | touch | buttons | cpu | gpu | storage | audio | battery | haptic | wifi | cellular | thermal |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| [OnePlus Nord 4](avalon.md) | `cliffs` | yes | yes | yes | yes | no | partly | no | partly | yes | no | no | yes |

An empty cell is a part nobody has looked at on that device yet.

## SoC families

| SoC | What | |
|---|---|---|
| `cliffs` | Qualcomm SM7675 Cliffs | carried by avalon (`devices/avalon/soc/`) |
