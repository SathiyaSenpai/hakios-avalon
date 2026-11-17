#!/bin/sh
# Check that tools/soc-args.py still reproduces the OnePlus Nord 4 (avalon) soc_* arguments from its own
# device tree, and that devices/avalon/soc.args is in step with it. A guard against a change
# to the generator (or the parser) silently shifting the addresses a port would get.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
dtb="$root/devices/avalon/prebuilt/device-tree.dtb"
want="$root/devices/avalon/soc.args"
got=$(python3 "$root/tools/soc-args.py" "$dtb")

expected='soc_ipa_base=0x3e40000
soc_gsi_base=0x3e04000
soc_ipcc=0x406000
soc_aoss=0xc300000
soc_rsc=0x17a20000
soc_apps_smmu=0x15000000
soc_cmd_db=0x81c60000
soc_mpss_mem=0x8bc00000
modem_ipa_fw_at=0x98980000
soc_smem=0x81d00000
soc_imem=0x14600000
soc_ipa_stream=0x4a0
soc_ipa_stream_uc=0x4a2
soc_pas_ipa=0xf
soc_ipa_fw=ipa_fws'

if [ "$got" != "$expected" ]; then
	echo "soc-args-test: FAIL -- the generator no longer reproduces the OnePlus Nord 4 (avalon) values"
	tmp=$(mktemp); printf '%s\n' "$expected" > "$tmp"
	printf '%s\n' "$got" | diff -u "$tmp" - || true
	rm -f "$tmp"
	exit 1
fi
if [ "$got" != "$(cat "$want")" ]; then
	echo "soc-args-test: FAIL -- devices/avalon/soc.args is out of step with the generator (regenerate it)"
	exit 1
fi
echo "soc-args-test: the generator reproduces all 15 OnePlus Nord 4 (avalon) soc_* values, and soc.args matches"
