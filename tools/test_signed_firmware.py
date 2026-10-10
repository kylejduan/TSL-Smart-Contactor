#!/usr/bin/env python3
"""Offline real ESP-IDF signature verification, using ephemeral synthetic keys."""
import argparse
from pathlib import Path
import tempfile
from firmware_sign import keygen, sign, verify, package, SLOT_BYTES
from ota_migrate import load_bundle


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--build',type=Path,required=True);args=p.parse_args()
    with tempfile.TemporaryDirectory() as d:
        root=Path(d);keygen(root/'keys');keygen(root/'other')
        key=root/'keys/signing-key.pem';public=root/'keys/signing-public.pem'
        bundle=root/'bundle';package(args.build,key,public,bundle);load_bundle(bundle)
        image=bundle/'application.bin';verify(image,public)
        assert image.stat().st_size<=SLOT_BYTES
        for label,raw,pub in [
            ('wrong signing key', image.read_bytes(), root/'other/signing-public.pem'),
            ('modified application', image.read_bytes()[:400]+bytes([image.read_bytes()[400]^1])+image.read_bytes()[401:],public),
            ('truncated signature', image.read_bytes()[:-1],public),
            ('unsigned image',(args.build/'tsl_smart_contactor.bin').read_bytes(),public)]:
            bad=root/'invalid.bin';bad.write_bytes(raw)
            try:verify(bad,pub)
            except Exception:print('PASS rejects '+label)
            else:raise AssertionError('accepted '+label)
        print('PASS real RSA-3072 signature and migration-bundle verification')
    return 0

if __name__=='__main__':raise SystemExit(main())
