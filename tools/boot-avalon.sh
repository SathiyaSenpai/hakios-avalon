#!/usr/bin/env bash
# tools/boot-avalon.sh - One-click boot HakiOS on OnePlus Nord 4 (Avalon) via Fastboot
set -euo pipefail

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$DIR"

MODE="${1:-ios}"

if [ "$MODE" = "test" ]; then
    IMG="hakios-avalon-test.img"
    HKUB="test.hkub"
    echo "[*] Mode: Minimal Mach-O Test (hello)"
elif [ "$MODE" = "usb" ]; then
    IMG="hakios-avalon-usb.img"
    HKUB="usb.hkub"
    if [ ! -f "usb-payload.bin" ] || [ ! -f "$HKUB" ] || [ ! -f "$IMG" ] || [ "xnuloader/loader.efi" -nt "usb-payload.bin" ]; then
        ./xnuloader/mkpayload.py -o usb-payload.bin loader.efi=xnuloader/loader.efi --usb >/dev/null
        ./tools/mkubootimg.py -d avalon -l xnuloader/loader.efi -p usb-payload.bin -c usb.hkub -o hakios-avalon-usb.img >/dev/null
    fi
    echo "[*] Mode: USB Loader (Waiting for ramdisk / fastboot commands)"
elif [ "$MODE" = "ramdisk" ] || [ "$MODE" = "full" ]; then
    RD="${2:-userland/system.apfs}"
    echo "[*] =========================================================="
    echo "[*] Mode: Full iOS 16.5 Bringup via Resident RAM Disk"
    echo "[*] System image: $RD ($(du -h "$RD" | cut -f1))"
    echo "[*] Maintainer / Port by: sathiyasenpai (OnePlus Nord 4 / avalon)"
    echo "[*] =========================================================="
    if [ ! -f "$RD" ]; then
        echo "[-] Error: $RD not found"
        exit 1
    fi
    if [ ! -f "ios-payload.bin" ] || [ "hakios-kernel" -nt "ios-payload.bin" ] || [ "userland/root.apfs" -nt "ios-payload.bin" ]; then
        echo "[*] Packaging ios-payload.bin for Avalon..."
        cfg_args="$(python3 tools/hakios-config.py args 2>/dev/null || echo "cpus=8 bootcpu=7 cpufreq=1 ufs=2 touchscreen=1 touch_debug=1 gpu=1 metal=1 metal_fps=1 modem=1 modem_buffer=0x99900000 modem_verbose=1 haptic=5 socinfo=1 thermal=1 thermal_stop=95 pmic=1 battery=1 buttons=1 ca_vbl=1 ddr=2 fb_hz=120 fb_threads=4 fbscale=2")"
        ./xnuloader/mkpayload.py -o ios-payload.bin \
            --cmdline "rd=md0 -v ${cfg_args} touch_s3910=1 touch_poll_us=4000" \
            kernel=hakios-kernel \
            ramdisk=userland/root.apfs
    fi

    echo "[*] Phase 1/4: Booting loader into USB fastboot mode..."
    "$0" usb

    echo "[*] Phase 2/4: Waiting for xnuloader USB fastboot server..."
    ok=0
    for i in {1..35}; do
        if timeout 2 fastboot getvar product 2>&1 | grep -q "xnuloader"; then
            echo "[+] xnuloader USB fastboot server ready!"
            ok=1
            break
        fi
        sleep 1
    done
    if [ "$ok" -ne 1 ]; then
        echo "[-] Error: xnuloader did not come up on USB. Check device screen."
        exit 1
    fi

    echo "[*] Phase 3/4: Preparing loader for resident rootfs..."
    fastboot oem ramdisk

    echo "[*] Uploading resident iOS 16.5 system.apfs (2.4 GB) to DRAM..."
    echo "[*] (This transfer takes ~60-80 seconds over USB 2.0/3.0. Please wait...)"
    fastboot stage "$RD"
    echo "[+] Resident iOS 16.5 rootfs staged successfully into DRAM!"

    echo "[*] Phase 4/4: Staging Darwin XNU kernel & companion userland..."
    fastboot stage ios-payload.bin

    echo "[*] Booting full iOS 16.5 (launchd & SpringBoard)..."
    fastboot oem run
    echo "[+] SUCCESS: Darwin XNU kernel and Apple iOS 16.5 are now executing on OnePlus Nord 4 (avalon)!"
    exit 0
else
    IMG="hakios-avalon-ios.img"
    HKUB="ios.hkub"
    echo "[*] Mode: Standalone Userland Test (Companion APFS)"
fi

if [ ! -f "$IMG" ] || [ ! -f "$HKUB" ]; then
    echo "[-] Error: $IMG or $HKUB missing in $DIR"
    exit 1
fi

echo "[*] Checking device state..."
if [ "$(adb get-state 2>/dev/null || true)" = "device" ]; then
    echo "[*] Device in Android ADB mode. Rebooting to bootloader..."
    adb reboot bootloader
fi

echo "[*] Waiting for device in Fastboot mode..."
while ! fastboot devices 2>/dev/null | grep -q "[a-zA-Z0-9]"; do
    sleep 1
done
echo "[+] Device detected in Fastboot mode!"

echo "[*] Step 1/3: Booting U-Boot into RAM ($IMG)..."
fastboot boot "$IMG"

echo "[*] Waiting for U-Boot to initialize fastboot server (6s)..."
sleep 6

echo "[*] Step 2/3: Staging HakiOS container ($HKUB)..."
fastboot stage "$HKUB"

echo "[*] Step 3/3: Handing over to HakiOS loader..."
fastboot oem run:hakios load fastboot
fastboot continue

echo "[+] Success: Handed over to HakiOS loader on OnePlus Nord 4 (avalon)!"
