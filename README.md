# HakiOS for OnePlus Nord 4 (avalon)

Hey everyone! Welcome to my port of Apple Darwin XNU kernel for the OnePlus Nord 4 (codename avalon, powered by Qualcomm SM7675 Snapdragon 7 plus Gen 3 Cliffs).

This is a hobby research project exploring how to boot Apple Darwin XNU kernel directly on modern Android hardware with custom U Boot, our own UEFI loader, and custom drivers.

Best part: it runs purely in RAM via fastboot. It does not flash or mess up your Android system partitions, so when you restart your phone you are immediately back to your normal Android setup.

## What is in this repository

1. Custom drivers for Qualcomm SM7675 Cliffs (GPU, Touch, Display, WiFi, Bluetooth, Audio, Power).
2. UEFI loader and U Boot setup tailored for avalon.
3. Apple Darwin XNU kernel patches and configuration.
4. Minimal userspace and testing tools.

## Firmware Blobs Note

To respect copyright, absolutely zero proprietary firmware blobs are hosted in this repository.

You have to pull firmware blobs from your own device or an official OxygenOS or ColorOS update package (payload.bin).

Here is how you extract the blobs:

1. Obtain your stock ROM dump folder, or grab payload.bin from the official OnePlus update zip.
2. Run the extraction tool:
```bash
python3 devices/avalon/extract-blobs.py /path/to/stock_dump_or_payload.bin
```
3. The tool automatically finds and parses modem, bluetooth, adsp, wpss wifi, touch firmware, and gpu zap shaders, placing them into local fw folders.

## How to Build

Follow these steps in your terminal to build the project from source:

1. Sync the hardware drivers into the kernel source tree:
```bash
./xnu/sync-drivers.sh
```

2. Compile the Darwin XNU kernel:
```bash
./xnu-build.sh
```

3. Optionally build the userspace rootfs:
```bash
./userland/build.sh
```

4. Package the fastboot boot image:
```bash
./tools/boot-avalon.sh
```

This generates the bootable image file in the project root.

## Manual Testing on Your Phone

You do not need any automated scripts to test this on your phone. Everything can be done manually using plain fastboot commands.

1. Turn off your OnePlus Nord 4 completely.
2. Hold down the Volume Down and Power buttons together until you see the Fastboot mode screen.
3. Connect your phone to your computer using a USB cable.
4. Verify that fastboot sees your device:
```bash
fastboot devices
```
5. Boot the image directly from RAM without flashing anything:
```bash
fastboot boot hakios-avalon-ios.img
```
6. What to expect on your phone screen:
The display keeps the continuous splash, U Boot initializes, hands the framebuffer over to Darwin, the Apple logo appears, and the XNU kernel boots with console messages.
7. Returning back to Android:
Whenever you want to exit or return to Android, hold down the Power button to force restart, or run:
```bash
fastboot reboot
```
Because the image was loaded purely into RAM, your internal UFS storage was never touched and your Android data is completely safe.

## Disclaimer and License

Kernel modifications are licensed under Apple Public Source License 2.0 (APSL). Our original loader code, custom drivers, and helper tools are licensed under the MIT License.

This project is an independent educational effort and is not affiliated with, sponsored by, or endorsed by Apple Inc or OnePlus Technology.
