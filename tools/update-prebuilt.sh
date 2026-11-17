#!/bin/bash
# Refresh prebuilt/ from what is built in this tree: the kernel, the loader, our user-space programs
# and the pieces of the phone's U-Boot. With them somebody without the cross toolchain can still make the
# images and run the system on the phone; see README.md.
set -e
cd "$(dirname "$(readlink -f "$0")")/.."
# KERNEL=... to take the kernel from a tree built elsewhere (xnu/apply.sh makes one anywhere)
K=${KERNEL:-hakios-kernel}
mkdir -p prebuilt/userland devices/avalon/prebuilt
cp $K prebuilt/hakios-kernel
# The loader in prebuilt/ is built with the PLACEHOLDER identity (xnuloader/mkidentity.sh --placeholder: no Apple data), in a
# temporary copy so that the real xnuloader/identity.h of this tree, made from your own IPSW, is left alone and never copied
# here. To start Darwin, build the loader yourself (make loader).
if command -v lld-link >/dev/null && command -v clang >/dev/null; then
	T=$(mktemp -d)
	mkdir "$T/xnuloader"
	cp xnuloader/loader.c xnuloader/efi.h xnuloader/build.sh xnuloader/mkidentity.sh xnuloader/mkidentity.py xnuloader/im4p.py "$T/xnuloader/"
	( cd "$T/xnuloader" && PLACEHOLDER=1 ./mkidentity.sh >/dev/null && ./build.sh >/dev/null ) && cp "$T/xnuloader/loader.efi" prebuilt/loader.efi ||
		echo "update-prebuilt.sh: the placeholder loader did not build; prebuilt/loader.efi kept"
	rm -rf "$T"
else
	echo "update-prebuilt.sh: no lld-link or clang here; prebuilt/loader.efi kept"
fi
cp userland/hello prebuilt/userland/
for f in init child stage0 hello_dyn svc_test surf_test rwtouch mg_test cpu_test sound_test libhakios.dylib libhakios.sb.dylib \
	libhakios.media.dylib libhakios.app.dylib libhakios.tweaks.dylib HakiosAudio net_test; do
	cp userland/obj/$f prebuilt/userland/
done
if [ -f "u-boot/build-avalon/u-boot.bin" ]; then
	cp u-boot/build-avalon/u-boot.bin devices/avalon/prebuilt/
fi
{
	echo "Built $(date '+%Y-%m-%d %H:%M') from this tree."
	echo
	for f in prebuilt/hakios-kernel prebuilt/loader.efi prebuilt/userland/* devices/avalon/prebuilt/*; do
		[ -f "$f" ] && sha256sum "$f"
	done
} > prebuilt/SHA256SUMS
ls -la prebuilt prebuilt/userland devices/avalon/prebuilt
