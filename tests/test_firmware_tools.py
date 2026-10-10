"""Synthetic flash layout and injected migration; no USB/network/credentials."""
import hashlib
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch
from types import SimpleNamespace
import contextlib
import io
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
import firmware_sign as signing
import ota_migrate as migration
import onboard


def table(entries):
    data=b''.join(struct.pack('<2sBBII16sI', b'\xaa\x50', kind, subtype, offset, size,
        name.encode(), 0) for name,kind,subtype,offset,size in entries)
    data+=b'\xeb\xeb'+b'\xff'*14+hashlib.md5(data).digest()
    return data+b'\xff'*(3072-len(data))


def application():
    data=bytearray(8192);data[0]=0xe9;data[1]=4;data[12]=9
    struct.pack_into('<I',data,32,0xABCD5432)
    data[48:53]=b'0.2.0';data[80:98]=b'tsl_smart_contactor'
    return bytes(data)


class FakeFlash:
    def __init__(self, fault=None, layout=migration.OLD_LAYOUT):
        self.flash=bytearray(b'\xff'*migration.FLASH_BYTES)
        raw=table(layout);self.flash[0x8000:0x8000+len(raw)]=raw
        self.flash[0x9000:0x21000]=b'\x5a'*0x18000
        self.fault=fault;self.writes=[];self.rebooted=False
        self.security=dict(chip_id=9, flash_crypt_cnt=0,
            parsed_flags=dict(SECURE_BOOT_EN=False,SECURE_DOWNLOAD_ENABLE=False))
    def get_security_info(self):return self.security
    def read_flash(self, offset, size):
        result=bytes(self.flash[offset:offset+size])
        if self.fault=='verify' and self.writes and offset==0x30000:result=b'\0'*size
        return result
    def flash_set_parameters(self, size):self.assert_size=size
    def flash_begin(self,size,offset):
        if self.fault=='write':raise OSError('synthetic')
        self.offset=offset;self.writes.append((offset,size))
        self.flash[offset:offset+size]=b'\xff'*size
    def flash_block(self, raw, seq):
        assert len(raw)==self.FLASH_WRITE_SIZE==4096
        offset=self.offset+seq*4096;self.flash[offset:offset+4096]=raw
        if self.fault=='nvs':self.flash[0x9000]=0
    def flash_finish(self,reboot):assert reboot is False
    def hard_reset(self):self.rebooted=True


class FirmwareToolsTests(unittest.TestCase):
    def test_usb_firmware_status_is_read_only_without_secret_prompt(self):
        with patch.object(onboard,'usb_exchange',return_value={'slot':'ota_0'}) as exchange, patch.object(onboard,'hidden') as prompt, contextlib.redirect_stdout(io.StringIO()):
            onboard.usb(SimpleNamespace(command='firmware_status',port='FAKE'))
        prompt.assert_not_called();exchange.assert_called_once_with('FAKE',{'op':'firmware_status'})

    def test_descriptor_rejects_old_wrong_chip_and_diagnostics(self):
        raw=application();self.assertEqual(signing.descriptor(raw),'0.2.0')
        for offset,value in [(12,0),(80,ord('X')),(50,ord('1'))]:
            damaged=bytearray(raw);damaged[offset]=value
            with self.assertRaises(ValueError):signing.descriptor(bytes(damaged))
    def test_key_generation_is_private_and_never_overwrites(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d)/'keys';signing.keygen(root)
            self.assertEqual((root/'signing-key.pem').stat().st_mode&0o777,0o600)
            with self.assertRaises(FileExistsError):signing.keygen(root)
    def test_exact_partition_layout_and_flags(self):
        self.assertEqual(signing.partition_entries(table(signing.LAYOUT)),signing.LAYOUT)
        raw=bytearray(table(signing.LAYOUT));struct.pack_into('<I',raw,28,1)
        with self.assertRaises(ValueError):signing.partition_entries(bytes(raw))
    def images(self):
        return {'application.bin':application(),'bootloader.bin':b'\x01'*0x3650,
                'partition-table.bin':table(signing.LAYOUT),'otadata.bin':b'\xff'*0x2000}
    def test_migration_preserves_nvs_phy_and_writes_table_last(self):
        with tempfile.TemporaryDirectory() as d:
            hw=FakeFlash();root=Path(d)/'backup';before=bytes(hw.flash)
            migration.migrate(hw,self.images(),root)
            self.assertEqual(bytes(hw.flash[0x9000:0x21000]),before[0x9000:0x21000])
            self.assertEqual(hw.writes[-1],(0x8000,4096));self.assertTrue(hw.rebooted)
            self.assertEqual((root/'original-flash.bin').read_bytes(),before)
            self.assertTrue((root/'migration-verified.json').is_file())
    def test_unknown_or_already_ota_layout_is_not_written(self):
        for layout in [signing.LAYOUT,[],[('nvs',1,2,0x9000,0x10000)]]:
            with tempfile.TemporaryDirectory() as d:
                hw=FakeFlash(layout=layout)
                with self.assertRaises(ValueError):migration.migrate(hw,self.images(),Path(d)/'backup')
                self.assertFalse(hw.writes)
    def test_security_efuses_are_not_changed_or_bypassed(self):
        for change in [{'flash_crypt_cnt':1},{'chip_id':0},
                       {'parsed_flags':{'SECURE_BOOT_EN':True}}, {'parsed_flags':{}}]:
            with tempfile.TemporaryDirectory() as d:
                hw=FakeFlash();hw.security.update(change)
                with self.assertRaises(ValueError):migration.migrate(hw,self.images(),Path(d)/'backup')
                self.assertFalse(hw.writes)
    def test_failures_preserve_backup_and_do_not_reboot(self):
        for failure in ['write','verify','nvs']:
            with tempfile.TemporaryDirectory() as d:
                hw=FakeFlash(fault=failure);root=Path(d)/'backup'
                with self.assertRaises((ValueError,OSError)):migration.migrate(hw,self.images(),root)
                self.assertTrue((root/'original-flash.bin').is_file());self.assertFalse(hw.rebooted)
                self.assertFalse((root/'migration-verified.json').exists())
    def test_no_reused_backup(self):
        with tempfile.TemporaryDirectory() as d:
            hw=FakeFlash()
            with self.assertRaises(FileExistsError):migration.migrate(hw,self.images(),Path(d))
            self.assertFalse(hw.writes)
    def test_bundle_hash_validation_before_signatures(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d);images=self.images();manifest=dict(schema=1,chip='esp32s3',flash_bytes=migration.FLASH_BYTES,
                version='0.2.0',build_sha256=application()[176:208].hex(),files={})
            for name,raw in images.items():
                (root/name).write_bytes(raw);manifest['files'][name]=dict(offset=signing.FILES[name][0],bytes=len(raw),
                    sha256=hashlib.sha256(raw).hexdigest())
            (root/'manifest.json').write_text(json.dumps(manifest))
            with patch.object(migration,'verify') as verify:
                self.assertEqual(migration.load_bundle(root),images);verify.assert_called_once()
                (root/'application.bin').write_bytes(b'broken')
                with self.assertRaises(ValueError):migration.load_bundle(root)
                verify.assert_called_once()

if __name__=='__main__':unittest.main()
