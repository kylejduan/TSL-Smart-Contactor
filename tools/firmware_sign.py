#!/usr/bin/env python3
"""Offline RSA-3072 signing and validated USB migration bundles; never flashes."""
from __future__ import annotations
import argparse
import contextlib
import hashlib
import importlib.metadata
import io
import json
from pathlib import Path
import re
import struct
from types import SimpleNamespace
from device_setup import private_dir, write_new

SLOT_BYTES = 0x300000
LAYOUT = [('nvs', 1, 2, 0x9000, 0x17000), ('phy_init', 1, 1, 0x20000, 0x1000),
          ('otadata', 1, 0, 0x21000, 0x2000), ('ota_0', 0, 16, 0x30000, SLOT_BYTES),
          ('ota_1', 0, 17, 0x330000, SLOT_BYTES)]
FILES = {'bootloader.bin': (0x0, 'bootloader/bootloader.bin'),
         'partition-table.bin': (0x8000, 'partition_table/partition-table.bin'),
         'otadata.bin': (0x21000, 'ota_data_initial.bin'),
         'application.bin': (0x30000, 'tsl_smart_contactor.bin')}


def descriptor(data: bytes) -> str:
    if (len(data) < 288 or len(data) > SLOT_BYTES or data[0] != 0xE9 or
            not 1 <= data[1] <= 16 or struct.unpack_from('<H', data, 12)[0] != 9 or
            struct.unpack_from('<I', data, 32)[0] != 0xABCD5432 or
            data[80:112].split(b'\0')[0] != b'tsl_smart_contactor'):
        raise ValueError('Not a compatible production ESP32-S3 image')
    raw = data[48:80]
    if b'\0' not in raw:
        raise ValueError('Invalid firmware version')
    version = raw.split(b'\0')[0].decode('ascii')
    match = re.fullmatch(r'0\.([0-9]{1,6})\.[0-9]+', version)
    if not match or int(match[1]) < 2:
        raise ValueError('Pre-OTA or incompatible firmware version')
    return version


def partition_entries(raw: bytes) -> list[tuple]:
    entries = []
    for offset in range(0, len(raw) - 31, 32):
        block = raw[offset:offset+32]
        if block[:2] != b'\xaa\x50':
            if (block[:16] != b'\xeb\xeb'+b'\xff'*14 or block[16:] != hashlib.md5(raw[:offset]).digest()
                    or raw[offset+32:] != b'\xff'*(len(raw)-offset-32)):
                raise ValueError('Partition checksum/padding is invalid')
            return entries
        _, kind, subtype, address, size, name, flags = struct.unpack('<2sBBII16sI', block)
        if flags:
            raise ValueError('Unexpected partition flags')
        entries.append((name.split(b'\0')[0].decode('ascii'), kind, subtype, address, size))
    raise ValueError('Partition checksum is missing')


def sdk_signer():
    # Supplied by the pinned ESP-IDF environment; no moving tool dependency.
    if importlib.metadata.version('esptool') != '4.12.0':
        raise ValueError('Use ESP-IDF v5.5.2 environment with esptool 4.12.0')
    import espsecure
    return espsecure


def keygen(root: Path):
    from cryptography.hazmat.primitives import serialization
    from cryptography.hazmat.primitives.asymmetric import rsa
    if root.exists():
        raise FileExistsError('Signing directories/keys are never overwritten')
    private_dir(root)
    key = rsa.generate_private_key(public_exponent=65537, key_size=3072)
    write_new(root/'signing-key.pem', key.private_bytes(serialization.Encoding.PEM,
        serialization.PrivateFormat.PKCS8, serialization.NoEncryption()))
    write_new(root/'signing-public.pem', key.public_key().public_bytes(serialization.Encoding.PEM,
        serialization.PublicFormat.SubjectPublicKeyInfo))


def verify(image: Path, public: Path):
    from cryptography.hazmat.primitives import serialization
    from cryptography.hazmat.primitives.asymmetric import rsa
    data = image.read_bytes(); descriptor(data)
    if len(data) < 8192 or len(data) % 4096:
        raise ValueError('Incomplete signature sector')
    # Match the pinned SDK's metadata-based signature lookup, rather than only
    # assuming the last sector is where the running app will look for its key.
    at = 24
    for _ in range(data[1]):
        if at+8 > len(data)-4096:
            raise ValueError('Truncated image segment')
        at += 8+struct.unpack_from('<I', data, at+4)[0]
        if at > len(data)-4096:
            raise ValueError('Image segment exceeds signed payload')
    if data[23] not in (0, 1):
        raise ValueError('Invalid image digest flag')
    image_end = (at//16+1)*16 + (32 if data[23] else 0)
    if (image_end+4095)//4096*4096 != len(data)-4096:
        raise ValueError('SDK signature offset differs from signed sector')
    key = serialization.load_pem_public_key(public.read_bytes())
    if not isinstance(key, rsa.RSAPublicKey) or key.key_size != 3072:
        raise ValueError('Firmware verification requires RSA-3072')
    api = sdk_signer()
    # Exactly one trusted signature, including sector padding. Reject alternate
    # signature blocks; production trusts one signing digest in the running app.
    if data[-4096:-4094] != b'\xe7\x02' or data[-4096+1216:] != b'\xff'*(4096-1216):
        raise ValueError('Unexpected RSA signature sector')
    with image.open('rb') as f, public.open('rb') as k, contextlib.redirect_stdout(io.StringIO()):
        api.verify_signature(SimpleNamespace(version='2', keyfile=k, datafile=f, hsm=False))


def sign(image: Path, key: Path, output: Path):
    from cryptography.hazmat.primitives import serialization
    from cryptography.hazmat.primitives.asymmetric import rsa
    data = image.read_bytes();descriptor(data)
    if output.exists() or len(data) > SLOT_BYTES-4096:
        raise ValueError('Output exists or signed image will not fit')
    private_key = serialization.load_pem_private_key(key.read_bytes(), None)
    if not isinstance(private_key, rsa.RSAPrivateKey) or private_key.key_size != 3072:
        raise ValueError('Signing requires a separate RSA-3072 firmware key')
    api = sdk_signer()
    # SDK writes to a frozen, exclusive output; failure never makes it deployable.
    write_new(output, b'')
    try:
        with image.open('rb') as f, key.open('rb') as k, contextlib.redirect_stdout(io.StringIO()):
            api.sign_data(SimpleNamespace(version='2', keyfile=[k], datafile=f, output=str(output),
                signature=None, pub_key=None, hsm=False, append_signatures=False))
        if len(output.read_bytes()) > SLOT_BYTES:
            raise ValueError('Signed image exceeds application slot')
        public = private_key.public_key().public_bytes(serialization.Encoding.PEM,
            serialization.PublicFormat.SubjectPublicKeyInfo)
        # No temporary private material beyond the operator-supplied key.
        with output.open('rb') as f, contextlib.redirect_stdout(io.StringIO()):
            api.verify_signature(SimpleNamespace(version='2', keyfile=io.BytesIO(public), datafile=f, hsm=False))
    except BaseException:
        output.unlink(missing_ok=True)  # Only this function's newly created output.
        raise


def package(build: Path, key: Path, public: Path, output: Path):
    if output.exists():
        raise FileExistsError('Migration bundles are never overwritten')
    settings = json.loads((build/'config/sdkconfig.json').read_text())
    required = ['BOOTLOADER_APP_ROLLBACK_ENABLE', 'SECURE_SIGNED_APPS_NO_SECURE_BOOT',
                'SECURE_SIGNED_APPS_RSA_SCHEME', 'SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT']
    forbidden = ['SECURE_BOOT', 'BOOTLOADER_APP_ANTI_ROLLBACK', 'BOOTLOADER_ANTI_ROLLBACK_ENABLE', 'SECURE_FLASH_ENC_ENABLED',
                 'SECURE_BOOT_BUILD_SIGNED_BINARIES']
    if (any(settings.get(k) is not True for k in required) or any(settings.get(k) for k in forbidden)
            or settings.get('IDF_TARGET') != 'esp32s3'):
        raise ValueError('Unexpected OTA/security build settings')
    source = {name: (build/path).read_bytes() for name, (_, path) in FILES.items()}
    if partition_entries(source['partition-table.bin']) != LAYOUT:
        raise ValueError('Unexpected partition table; NVS must retain its exact range')
    if not 32 <= len(source['bootloader.bin']) <= 0x8000:
        raise ValueError('Bootloader does not fit')
    if source['otadata.bin'] != b'\xff'*0x2000:
        raise ValueError('Initial OTA selection must be erased')
    version = descriptor(source['application.bin']);private_dir(output)
    for name, data in source.items():
        if name != 'application.bin':write_new(output/name, data)
    write_new(output/'signing-public.pem', public.read_bytes())
    unsigned = output/'unsigned.bin'
    write_new(unsigned, source['application.bin'])
    sign(unsigned, key, output/'application.bin')
    unsigned.unlink()
    verify(output/'application.bin', output/'signing-public.pem')
    entries = {name: {'offset': address, 'bytes': (output/name).stat().st_size,
        'sha256': hashlib.sha256((output/name).read_bytes()).hexdigest()} for name, (address, _) in FILES.items()}
    write_new(output/'manifest.json', (json.dumps({'schema': 1, 'version': version,
        'build_sha256': source['application.bin'][176:208].hex(),
        'chip': 'esp32s3', 'flash_bytes': 8*1024*1024, 'files': entries}, indent=2)+'\n').encode())


def main():
    p = argparse.ArgumentParser(description=__doc__);commands=p.add_subparsers(dest='command',required=True)
    k=commands.add_parser('keygen');k.add_argument('--out',type=Path,required=True)
    s=commands.add_parser('sign');s.add_argument('--image',type=Path,required=True);s.add_argument('--key',type=Path,required=True);s.add_argument('--out',type=Path,required=True)
    v=commands.add_parser('verify');v.add_argument('--image',type=Path,required=True);v.add_argument('--public',type=Path,required=True)
    b=commands.add_parser('package');b.add_argument('--build',type=Path,required=True);b.add_argument('--key',type=Path,required=True);b.add_argument('--public',type=Path,required=True);b.add_argument('--out',type=Path,required=True)
    args=p.parse_args()
    try:
        if args.command=='keygen':keygen(args.out)
        elif args.command=='sign':sign(args.image,args.key,args.out)
        elif args.command=='verify':verify(args.image,args.public)
        else:package(args.build,args.key,args.public,args.out)
    except Exception as exc:
        print('Offline firmware operation failed ('+type(exc).__name__+'); check inputs. No device changed.')
        return 1
    print('Offline firmware operation succeeded. No device changed.');return 0

if __name__=='__main__':raise SystemExit(main())
