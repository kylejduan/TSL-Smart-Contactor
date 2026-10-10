#!/usr/bin/env python3
"""Explicit USB-only, factory-to-OTA migration with private flash backup.

Never runs automatically. App OTA does not use this tool and cannot change the
partition table, bootloader, NVS or PHY. A failed migration needs USB recovery.
"""
from __future__ import annotations
import argparse
import contextlib
import hashlib
import io
import json
from pathlib import Path
from device_setup import private_dir, write_new, usb_exchange
from firmware_sign import FILES, LAYOUT, SLOT_BYTES, descriptor, partition_entries, verify, sdk_signer

FLASH_BYTES = 8*1024*1024
CONFIRMATION = 'USB_ONLY_MAINS_DISCONNECTED_INSTALL_OTA'
OLD_LAYOUT = [('nvs', 1, 2, 0x9000, 0x17000), ('phy_init', 1, 1, 0x20000, 0x1000),
              ('factory', 0, 0, 0x30000, SLOT_BYTES)]


def load_bundle(root: Path) -> dict[str, bytes]:
    manifest = json.loads((root/'manifest.json').read_text())
    if (manifest.get('schema') != 1 or manifest.get('chip') != 'esp32s3' or
            manifest.get('flash_bytes') != FLASH_BYTES or set(manifest.get('files', {})) != set(FILES)):
        raise ValueError('Unexpected migration manifest')
    data = {}
    for name, (address, _) in FILES.items():
        raw = (root/name).read_bytes();entry=manifest['files'][name]
        if (entry.get('offset') != address or entry.get('bytes') != len(raw) or
                entry.get('sha256') != hashlib.sha256(raw).hexdigest()):
            raise ValueError('Migration artifact changed')
        data[name] = raw
    if (partition_entries(data['partition-table.bin']) != LAYOUT or
            data['otadata.bin'] != b'\xff'*0x2000 or
            not 32 <= len(data['bootloader.bin']) <= 0x8000 or
            manifest.get('version') != descriptor(data['application.bin']) or
            manifest.get('build_sha256') != data['application.bin'][176:208].hex()):
        raise ValueError('Unexpected migration layout/image')
    verify(root/'application.bin', root/'signing-public.pem')
    return data


def migrate(loader, images: dict[str, bytes], backup: Path):
    """All flash accesses injected in tests; writing never overlaps NVS or PHY."""
    if backup.exists():
        raise FileExistsError('Backup directories are never reused or overwritten')
    security = loader.get_security_info()
    if (security.get('chip_id') != 9 or
            security.get('parsed_flags', {}).get('SECURE_BOOT_EN') is not False or
            security.get('parsed_flags', {}).get('SECURE_DOWNLOAD_ENABLE') is not False or
            int(security['flash_crypt_cnt']).bit_count() % 2):
        raise ValueError('Hardware security settings require a separate recovery procedure')
    original = loader.read_flash(0, FLASH_BYTES)
    if len(original) != FLASH_BYTES:
        raise ValueError('Incomplete flash backup')
    if partition_entries(original[0x8000:0x9000]) != OLD_LAYOUT:
        raise ValueError('Only the known legacy factory layout may be migrated; use OTA otherwise')
    private_dir(backup)
    write_new(backup/'original-flash.bin', original)
    write_new(backup/'backup.json', (json.dumps({'bytes': len(original),
        'sha256': hashlib.sha256(original).hexdigest(), 'nvs_sha256': hashlib.sha256(original[0x9000:0x20000]).hexdigest(),
        'complete': True, 'migration_verified': False}, indent=2)+'\n').encode())
    # Bound padding to a flash sector. The stub's default 16 KiB padded block
    # could cross the partition-table/NVS boundary; never use that default here.
    loader.FLASH_WRITE_SIZE = 4096
    loader.flash_set_parameters(FLASH_BYTES)
    # Partition table is the final write, so the old mapping remains until the
    # signed application, blank OTA selection and new bootloader are present.
    for name in ('application.bin', 'otadata.bin', 'bootloader.bin', 'partition-table.bin'):
        offset = FILES[name][0];raw = images[name]
        padded = raw + b'\xff'*(-len(raw) % 4096)
        end = offset+len(padded)
        if not (end <= 0x8000 or offset == 0x8000 and end <= 0x9000 or
                offset == 0x21000 and end <= 0x23000 or offset == 0x30000 and end <= 0x330000):
            raise ValueError('Flash write overlaps protected storage')
        loader.flash_begin(len(padded), offset)
        for seq in range(len(padded)//4096):
            loader.flash_block(padded[seq*4096:(seq+1)*4096], seq)
        loader.flash_finish(reboot=False)
        if loader.read_flash(offset, len(padded)) != padded:
            raise ValueError('Flash verification failed; do not reconnect mains')
    if loader.read_flash(0x9000, 0x18000) != original[0x9000:0x21000]:
        raise ValueError('NVS/PHY preservation failed; stay isolated and recover over USB')
    write_new(backup/'migration-verified.json', b'{"verified":true,"nvs_and_phy_unchanged":true}\n')
    loader.hard_reset()


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--bundle', type=Path, required=True);p.add_argument('--port', required=True)
    p.add_argument('--backup', type=Path, required=True)
    args=p.parse_args()
    print('Disconnect wall power and ALL contactor/mains wiring. USB power only.')
    print('Select OFF / DISABLED first. This tool preserves its saved configuration.')
    if input('Type '+CONFIRMATION+': ').strip() != CONFIRMATION:
        print('Canceled; no device changed.');return 1
    loader=None
    try:
        sdk_signer();images=load_bundle(args.bundle)
        hello=usb_exchange(args.port, {'op':'hello'}, timeout=5)
        state=usb_exchange(args.port, {'op':'status'}, timeout=5)
        diagnostic=usb_exchange(args.port, {'op':'diagnostics'}, timeout=5)
        if (hello.get('board') != 'ESP32-S3-Relay-1CH' or
                any(state.get(k) is not v for k,v in {'ready':True,'disabled':True,'commanded_on':False,'fault':False}.items()) or
                diagnostic.get('profile_integrity') is not True or diagnostic.get('provision_pending') is not False):
            raise ValueError('Expected a healthy, DISABLED legacy controller')
        if hello.get('firmware_ota'):
            update=usb_exchange(args.port, {'op':'firmware_status'}, timeout=5)
            # Recover a partially completed migration whose application was
            # written before the table. Never reset an already-OTA/pending boot.
            if (update.get('slot') != 'factory' or update.get('available') is not False
                    or update.get('busy') is not False):
                raise ValueError('Use OTA for an already migrated device')
        mac = diagnostic.get('station_mac')
        from esptool.targets.esp32s3 import ESP32S3ROM
        # Keep vendor output (including identifiers) out of ordinary terminal logs.
        with contextlib.redirect_stdout(io.StringIO()):
            loader=ESP32S3ROM(args.port);loader.connect()
            if ':'.join(f'{x:02x}' for x in loader.read_mac()) != mac:
                raise ValueError('USB and ROM device identities differ')
            loader=loader.run_stub();loader.change_baud(460800)
            if not 23 <= (loader.flash_id() >> 16) <= 26:
                raise ValueError("Flash capacity is below the configured 8 MiB")
            migrate(loader,images,args.backup)
        print('Migration flash verified; NVS/PHY unchanged. Wait for reboot, then verify USB and HTTPS.')
        print('Leave mains disconnected until startup and rollback checks are complete.')
        return 0
    except Exception as exc:
        print('Migration stopped ('+type(exc).__name__+'). Keep mains disconnected. Preserve the private backup; use USB recovery.')
        return 1
    finally:
        if loader is not None:loader._port.close()

if __name__=='__main__':raise SystemExit(main())
