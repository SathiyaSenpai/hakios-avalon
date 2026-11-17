#!/usr/bin/env python3
"""Extract OnePlus Nord 4 (avalon) firmware blobs into HakiOS fw-*/ from any pointed dump directory, stock.zip, or payload.bin."""
import argparse
import bz2
import hashlib
import lzma
import shutil
import struct
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path

HAKIOS_ROOT = Path(__file__).resolve().parents[2]


def read_varint(buf: bytes, pos: int):
    val = shift = 0
    while True:
        b = buf[pos]
        pos += 1
        val |= (b & 0x7f) << shift
        if not (b & 0x80):
            return val, pos
        shift += 7


def iter_fields(buf: bytes):
    pos = 0
    while pos < len(buf):
        tag, pos = read_varint(buf, pos)
        fn, wt = tag >> 3, tag & 7
        if wt == 0:
            v, pos = read_varint(buf, pos)
        elif wt == 1:
            v = buf[pos:pos + 8]
            pos += 8
        elif wt == 2:
            ln, pos = read_varint(buf, pos)
            v = buf[pos:pos + ln]
            pos += ln
        elif wt == 5:
            v = buf[pos:pos + 4]
            pos += 4
        else:
            raise ValueError(f'unexpected protobuf wire type {wt}')
        yield fn, wt, v


def extract_from_payload(source: Path, out_dir: Path, wanted: set[str]) -> dict[str, Path]:
    if not wanted:
        return {}
    out_dir.mkdir(parents=True, exist_ok=True)
    if zipfile.is_zipfile(source):
        zf = zipfile.ZipFile(source)
        info = zf.getinfo('payload.bin')
        if info.compress_type != 0:
            payload_file = out_dir / '_payload.bin'
            with zf.open(info) as src, open(payload_file, 'wb') as dst:
                shutil.copyfileobj(src, dst, 1 << 20)
            raw_path, base_offset = payload_file, 0
        else:
            with open(source, 'rb') as rf:
                rf.seek(info.header_offset)
                lh = rf.read(30)
                sig, _, _, _, _, _, _, _, _, fnlen, exlen = struct.unpack('<IHHHHHIIIHH', lh)
                if sig != 0x04034b50:
                    raise SystemExit(f'{source}: invalid ZIP local header')
                raw_path, base_offset = source, info.header_offset + 30 + fnlen + exlen
    else:
        raw_path, base_offset = source, 0

    extracted = {}
    with open(raw_path, 'rb') as rawf:
        rawf.seek(base_offset)
        magic, ver, mlen, slen = struct.unpack('>4sQQI', rawf.read(24))
        if magic != b'CrAU' or ver != 2:
            raise SystemExit(f'{source}: unsupported payload header {magic!r} v{ver}')
        manifest = rawf.read(mlen)
        data_base = base_offset + 24 + mlen + slen
        block_size = 4096

        for fn, wt, v in iter_fields(manifest):
            if fn == 3 and wt == 0:
                block_size = v
            elif fn == 13 and wt == 2:
                name, size, expected_sha, ops = None, 0, None, []
                for pfn, pwt, pv in iter_fields(v):
                    if pfn == 1 and pwt == 2:
                        name = pv.decode()
                    elif pfn == 7 and pwt == 2:
                        for ifn, iwt, iv in iter_fields(pv):
                            if ifn == 1 and iwt == 0:
                                size = iv
                            elif ifn == 2 and iwt == 2:
                                expected_sha = iv.hex()
                    elif pfn == 8 and pwt == 2:
                        op_type = doff = dlen = 0
                        extents = []
                        for ofn, owt, ov in iter_fields(pv):
                            if ofn == 1 and owt == 0:
                                op_type = ov
                            elif ofn == 2 and owt == 0:
                                doff = ov
                            elif ofn == 3 and owt == 0:
                                dlen = ov
                            elif ofn == 6 and owt == 2:
                                sb = nb = 0
                                for efn, ewt, ev in iter_fields(ov):
                                    if efn == 1 and ewt == 0:
                                        sb = ev
                                    elif efn == 2 and ewt == 0:
                                        nb = ev
                                extents.append((sb, nb))
                        ops.append((op_type, doff, dlen, extents))
                if name in wanted:
                    img = out_dir / f'{name}.img'
                    h = hashlib.sha256()
                    with open(img, 'w+b') as out:
                        out.truncate(size)
                        for op_type, doff, dlen, extents in ops:
                            if op_type == 6:
                                continue
                            rawf.seek(data_base + doff)
                            chunk = rawf.read(dlen)
                            if op_type == 1:
                                chunk = bz2.decompress(chunk)
                            elif op_type == 8:
                                chunk = lzma.decompress(chunk)
                            elif op_type != 0:
                                raise RuntimeError(f'{name}: unsupported payload op {op_type}')
                            pos = 0
                            for sb, nb in extents:
                                out.seek(sb * block_size)
                                take = nb * block_size
                                out.write(chunk[pos:pos + take])
                                pos += take
                        out.seek(0)
                        while buf := out.read(1 << 20):
                            h.update(buf)
                    if expected_sha and h.hexdigest() != expected_sha:
                        raise SystemExit(f'{name}.img SHA-256 mismatch: {h.hexdigest()} != {expected_sha}')
                    extracted[name] = img
                    print(f'  extracted {name}.img ({size} bytes, sha256={h.hexdigest()[:16]}...)')
    return extracted


def find_first_file(root: Path, rel_candidates: tuple[str, ...]) -> Path | None:
    if root.is_file():
        return None
    for rel in rel_candidates:
        p = root / rel
        if p.is_file():
            return p
    return None


def find_first_dir(root: Path, rel_candidates: tuple[str, ...]) -> Path | None:
    if root.is_file():
        return None
    for rel in rel_candidates:
        p = root / rel
        if p.is_dir():
            return p
    return None


def copy_glob(src_dir: Path | None, pattern: str, dst_dir: Path) -> int:
    if not src_dir or not src_dir.is_dir():
        return 0
    dst_dir.mkdir(parents=True, exist_ok=True)
    n = 0
    for p in sorted(src_dir.glob(pattern)):
        if p.is_file():
            shutil.copy2(p, dst_dir / p.name)
            n += 1
    return n


def copy_if_file(src: Path | None, dst: Path) -> bool:
    if src and src.is_file():
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src, dst)
        return True
    return False


def unpack_image(img: Path, out_dir: Path, seven_z: str, erofs_fsck: str | None) -> Path:
    out_dir.mkdir(parents=True, exist_ok=True)
    head = img.read_bytes()[:4096]
    if len(head) >= 1028 and struct.unpack_from('<I', head, 1024)[0] == 0xE0F5E1E2:
        if not erofs_fsck:
            raise SystemExit(f'{img}: EROFS image requires fsck.erofs (install erofs-utils) or pre-extracted directories')
        subprocess.run([erofs_fsck, f'--extract={out_dir}', str(img)], check=True)
        return out_dir
    subprocess.run([seven_z, 'x', '-y', f'-o{out_dir}', str(img)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return out_dir


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('source', nargs='?', type=Path, help='Pointed stock dump directory, OTA zip (stock.zip), or payload.bin')
    ap.add_argument('--from', dest='from_opt', type=Path, help='Alias for source path')
    ap.add_argument('--out', type=Path, default=HAKIOS_ROOT, help='HakiOS repository root (default: repo root)')
    args = ap.parse_args()

    src = args.from_opt or args.source
    if src is None:
        if sys.stdin.isatty():
            entered = input('Enter path to Nord 4 stock dump directory, stock.zip, or payload.bin: ').strip()
            if not entered:
                raise SystemExit('Aborted: no source path provided.')
            src = Path(entered)
        else:
            ap.print_help(sys.stderr)
            raise SystemExit(2)

    src = src.expanduser().resolve()
    seven_z = shutil.which('7zz') or shutil.which('7z')
    if not seven_z:
        raise SystemExit('7z or 7zz is required to unpack partition images')
    erofs_fsck = shutil.which('fsck.erofs')
    hakios_root = args.out.expanduser().resolve()

    with tempfile.TemporaryDirectory(prefix='hakios-avalon-extract-') as td:
        work = Path(td)
        payload_src: Path | None = None
        if src.is_file():
            if zipfile.is_zipfile(src) or src.read_bytes()[:4] == b'CrAU':
                payload_src = src
            else:
                raise SystemExit(f'Unsupported file (expected OTA zip or payload.bin): {src}')
        elif src.is_dir():
            for cand in (src / 'payload.bin', src / 'stock.zip'):
                if cand.is_file():
                    payload_src = cand
                    break
            if not payload_src:
                for z in sorted(src.glob('*.zip')):
                    if zipfile.is_zipfile(z) and 'payload.bin' in zipfile.ZipFile(z).namelist():
                        payload_src = z
                        break
        else:
            raise SystemExit(f'Source path does not exist: {src}')

        vendor_dir = find_first_dir(src, ('vendor', 'rootfs/vendor', 'images/vendor'))
        odm_dir = find_first_dir(src, ('odm', 'rootfs/odm', 'images/odm'))
        modem_img_dir = find_first_dir(src, ('modem/image', 'vendor/firmware_mnt/image', 'firmware_mnt/image', 'firmware/image', 'image'))
        if not modem_img_dir and src.is_dir() and any((src / f).is_file() for f in ('modem.mdt', 'adsp2.mdt', 'adsp.mdt')):
            modem_img_dir = src
        bt_dir = find_first_dir(src, ('bluetooth/image', 'bt_firmware/image', 'vendor/bt_firmware/image', 'bluetooth', 'bt_firmware'))

        vendor_img = find_first_file(src, ('vendor.img', 'vendor_a.img', 'images/vendor.img'))
        odm_img = find_first_file(src, ('odm.img', 'odm_a.img', 'images/odm.img'))
        modem_img = find_first_file(src, ('modem.img', 'NON-HLOS.bin', 'modem_a.img', 'images/modem.img', 'images/NON-HLOS.bin'))
        bt_img = find_first_file(src, ('bluetooth.img', 'bt_firmware.img', 'BTFM.bin', 'bluetooth_a.img', 'images/bluetooth.img', 'images/bt_firmware.img'))

        need_payload = set()
        if not vendor_dir and not vendor_img:
            need_payload.add('vendor')
        if not odm_dir and not odm_img:
            need_payload.add('odm')
        if not modem_img_dir and not modem_img:
            need_payload.add('modem')
        if not bt_dir and not bt_img:
            need_payload.add('bluetooth')

        if need_payload and payload_src:
            p_imgs = extract_from_payload(payload_src, work / 'payload', need_payload)
            vendor_img = vendor_img or p_imgs.get('vendor')
            odm_img = odm_img or p_imgs.get('odm')
            modem_img = modem_img or p_imgs.get('modem')
            bt_img = bt_img or p_imgs.get('bluetooth')

        if not vendor_dir and vendor_img:
            vendor_dir = unpack_image(vendor_img, work / 'vendor', seven_z, erofs_fsck)
        if not odm_dir and odm_img:
            odm_dir = unpack_image(odm_img, work / 'odm', seven_z, erofs_fsck)
        if not modem_img_dir and modem_img:
            m_ext = unpack_image(modem_img, work / 'modem', seven_z, erofs_fsck)
            modem_img_dir = (m_ext / 'image') if (m_ext / 'image').is_dir() else m_ext
        if not bt_dir and bt_img:
            b_ext = unpack_image(bt_img, work / 'bt', seven_z, erofs_fsck)
            bt_dir = (b_ext / 'image') if (b_ext / 'image').is_dir() else b_ext

        for sub in ('fw-touch', 'fw-gpu', 'fw-audio', 'fw-camera', 'fw-haptic', 'fw-wifi', 'fw-bt', 'fw-modem', 'fw-modem/image'):
            (hakios_root / sub).mkdir(parents=True, exist_ok=True)

        if odm_dir:
            copy_glob(odm_dir / 'firmware/tp/24211', 'FW_S3910_TIANMA*.img', hakios_root / 'fw-touch')
            copy_glob(odm_dir / 'firmware/tp/24211', 'LIMIT_S3910_TIANMA*.img', hakios_root / 'fw-touch')
            copy_glob(odm_dir / 'firmware', 'aw8697_haptic*.bin', hakios_root / 'fw-haptic')
            copy_if_file(odm_dir / 'firmware/aw8697_rtp.bin', hakios_root / 'fw-haptic/aw8697_rtp.bin')
            copy_glob(odm_dir / 'etc/wifi/qca6750', 'bdwlan*', hakios_root / 'fw-wifi')
            copy_if_file(odm_dir / 'etc/wifi/regdb.bin', hakios_root / 'fw-wifi/regdb.bin')
            copy_if_file(odm_dir / 'etc/wifi/qca6750/regdb.bin', hakios_root / 'fw-wifi/regdb.bin')

        if vendor_dir:
            copy_glob(vendor_dir / 'firmware', 'gen71100*.fw', hakios_root / 'fw-gpu')
            copy_glob(vendor_dir / 'firmware', 'gen71100*.bin', hakios_root / 'fw-gpu')
            copy_glob(vendor_dir / 'firmware', 'gen7*zap.mbn', hakios_root / 'fw-gpu')
            copy_glob(vendor_dir / 'firmware', 'evass*', hakios_root / 'fw-audio')
            copy_glob(vendor_dir / 'firmware', 'CAMERA_ICP*', hakios_root / 'fw-camera')
            copy_if_file(vendor_dir / 'etc/wifi/qca6750/WCNSS_qcom_cfg.ini', hakios_root / 'fw-wifi/WCNSS_qcom_cfg.ini')
            copy_if_file(vendor_dir / 'etc/wifi/kiwi_v2/WCNSS_qcom_cfg.ini', hakios_root / 'fw-wifi/WCNSS_qcom_cfg_kiwi_v2.ini')

        copy_glob(bt_dir, 'msbtfw*', hakios_root / 'fw-bt')
        copy_glob(bt_dir, 'msnv*', hakios_root / 'fw-bt')

        if modem_img_dir:
            copy_glob(modem_img_dir, 'modem.mdt', hakios_root / 'fw-modem')
            copy_glob(modem_img_dir, 'modem.b[0-9][0-9]', hakios_root / 'fw-modem')
            copy_glob(modem_img_dir, 'modem_dtb.*', hakios_root / 'fw-modem')
            copy_glob(modem_img_dir, 'ipa_fws.*', hakios_root / 'fw-modem')
            copy_if_file(modem_img_dir / 'fw_ipa_gsi_5.5_h.elf', hakios_root / 'fw-modem/fw_ipa_gsi_5.5_h.elf')

            m_dst = hakios_root / 'fw-modem/image'
            copy_glob(modem_img_dir, '*.jsn', m_dst)
            copy_glob(modem_img_dir, 'modem_dtb.*', m_dst)
            copy_glob(modem_img_dir, 'ipa_fws.*', m_dst)
            copy_if_file(modem_img_dir / 'fw_ipa_gsi_5.5_h.elf', m_dst / 'fw_ipa_gsi_5.5_h.elf')
            if (modem_img_dir / 'modem_pr').is_dir():
                shutil.copytree(modem_img_dir / 'modem_pr', m_dst / 'modem_pr', dirs_exist_ok=True)
            if (modem_img_dir / 'palawan').is_dir():
                shutil.copytree(modem_img_dir / 'palawan', m_dst / 'palawan', dirs_exist_ok=True)

            copy_glob(modem_img_dir, 'adsp2.*', hakios_root / 'fw-audio')
            copy_glob(modem_img_dir, 'adsp2_dtb.*', hakios_root / 'fw-audio')
            copy_glob(modem_img_dir, 'adsp.*', hakios_root / 'fw-audio')
            copy_glob(modem_img_dir, 'adsp_dtb.*', hakios_root / 'fw-audio')
            for p in sorted(modem_img_dir.glob('adsp2.*')):
                copy_if_file(p, hakios_root / 'fw-audio' / ('adsp' + p.suffix))
            for p in sorted(modem_img_dir.glob('adsp2_dtb.*')):
                copy_if_file(p, hakios_root / 'fw-audio' / ('adsp_dtb' + p.suffix))
            for f in ('adspr.jsn', 'adsps.jsn', 'adspua.jsn', 'battmgr.jsn'):
                copy_if_file(modem_img_dir / f, hakios_root / 'fw-audio' / f)

            qca_m = (modem_img_dir / 'qca6750') if (modem_img_dir / 'qca6750').is_dir() else modem_img_dir
            copy_glob(qca_m, 'wpss.*', hakios_root / 'fw-wifi')
            copy_glob(qca_m, 'wpss.*', hakios_root / 'fw-wifi/qca6750')
            for f in ('Data.msc', 'qdss_trace_config.cfg'):
                copy_if_file(qca_m / f, hakios_root / 'fw-wifi' / f)
                copy_if_file(qca_m / f, hakios_root / 'fw-wifi/qca6750' / f)
            copy_if_file(hakios_root / 'fw-wifi/bdwlan.elf', hakios_root / 'fw-wifi/qca6750/bdwlan.elf')
            copy_if_file(hakios_root / 'fw-wifi/regdb.bin', hakios_root / 'fw-wifi/qca6750/regdb.bin')

        print('[+] Firmware extraction complete:')
        for d in ('fw-touch', 'fw-gpu', 'fw-audio', 'fw-camera', 'fw-haptic', 'fw-wifi', 'fw-bt', 'fw-modem'):
            cnt = sum(1 for p in (hakios_root / d).rglob('*') if p.is_file())
            print(f'    {d}: {cnt} files')


if __name__ == '__main__':
    main()
