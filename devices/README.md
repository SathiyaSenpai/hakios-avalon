# Device repositories

`devices/manifest.conf` names the device (`avalon`, OnePlus Nord 4 / SM7675 Cliffs) and HakiOS U-Boot/UEFI repository.
`tools/hakios-device.py sync` fetches any external repositories listed in the manifest.
To add another device: `tools/hakios-device.py new` ([Port a device](../docs/porting/index.md)).
