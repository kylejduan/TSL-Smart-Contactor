"""Private local files, per-device certificates, and explicit USB handoff."""
from __future__ import annotations
import datetime as dt
import ipaddress
import json
import os
from pathlib import Path
import subprocess
import time
from tesla_setup import SetupError, decode_json

def private_dir(path: Path):
    path.mkdir(mode=0o700, parents=True, exist_ok=True)
    if os.name == "nt":
        # Restrict to this local user. Fail rather than silently leave inherited ACLs.
        identity = subprocess.check_output(["whoami"], text=True).strip()
        r = subprocess.run(["icacls", str(path), "/inheritance:r", "/grant:r", f"{identity}:(OI)(CI)F"],
                           capture_output=True)
        if r.returncode:
            raise SetupError("Could not restrict Windows provisioning-directory permissions.")
    else:
        path.chmod(0o700)

def write_new(path: Path, data: bytes, secret=True):
    flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL
    with os.fdopen(os.open(path, flags, 0o600 if secret else 0o644), "wb") as f:
        f.write(data)
        f.flush()
        os.fsync(f.fileno())

def prepare_files(root: Path, domain: str, host: str):
    from cryptography import x509
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import ec
    from cryptography.x509.oid import NameOID
    if root.exists():
        raise FileExistsError("Choose a new provisioning directory; keys are never overwritten.")
    private_dir(root)
    public = root / "public"
    public.mkdir(exist_ok=True)
    key_dir = root / "private"
    private_dir(key_dir)
    public_key_dir = public / ".well-known" / "appspecific"
    public_key_dir.mkdir(parents=True, exist_ok=True)
    def key_bytes(k):
        return k.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8,
                               serialization.NoEncryption())
    def pub_bytes(k):
        return k.public_key().public_bytes(serialization.Encoding.PEM, serialization.PublicFormat.SubjectPublicKeyInfo)
    partner = ec.generate_private_key(ec.SECP256R1())
    write_new(key_dir / "tesla-private.pem", key_bytes(partner))
    write_new(public_key_dir / "com.tesla.3p.public-key.pem", pub_bytes(partner), False)
    ca = ec.generate_private_key(ec.SECP256R1())
    device = ec.generate_private_key(ec.SECP256R1())
    now = dt.datetime.now(dt.timezone.utc)
    ca_name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, "TSL private local CA")])
    ca_cert = (x509.CertificateBuilder().subject_name(ca_name).issuer_name(ca_name).public_key(ca.public_key())
        .serial_number(x509.random_serial_number()).not_valid_before(now-dt.timedelta(days=1))
        .not_valid_after(now+dt.timedelta(days=3650)).add_extension(x509.BasicConstraints(ca=True, path_length=0), True)
        .add_extension(x509.KeyUsage(False, False, False, False, False, True, True, False, False), True)
        .sign(ca, hashes.SHA256()))
    try:
        san = x509.IPAddress(ipaddress.ip_address(host))
    except ValueError:
        san = x509.DNSName(host)
    cert = (x509.CertificateBuilder().subject_name(x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, host)]))
        .issuer_name(ca_name).public_key(device.public_key()).serial_number(x509.random_serial_number())
        .not_valid_before(now-dt.timedelta(days=1)).not_valid_after(now+dt.timedelta(days=825))
        .add_extension(x509.BasicConstraints(ca=False, path_length=None), True)
        .add_extension(x509.SubjectAlternativeName([san]), False)
        .add_extension(x509.ExtendedKeyUsage([x509.oid.ExtendedKeyUsageOID.SERVER_AUTH]), False)
        .sign(ca, hashes.SHA256()))
    write_new(key_dir / "local-ca-private.pem", key_bytes(ca))
    write_new(key_dir / "device-key.pem", key_bytes(device))
    write_new(root / "local-ca.pem", ca_cert.public_bytes(serialization.Encoding.PEM), False)
    write_new(root / "device-cert.pem", cert.public_bytes(serialization.Encoding.PEM), False)
    callback = b'''<!doctype html><html lang="en"><meta charset="utf-8">
<meta name="referrer" content="no-referrer">
<meta http-equiv="Content-Security-Policy" content="default-src 'none'; base-uri 'none'; form-action 'none'">
<title>Tesla authorization callback</title><h1>Return to the local setup helper</h1>
<p>Copy the entire address-bar URL into its hidden callback prompt. Do not share it.
This page runs no scripts and sends no requests. Close this tab after the exchange.</p></html>'''
    write_new(public / "callback.html", callback, False)

def usb_exchange(port: str, payload: dict, timeout=30):
    import serial
    wire = (json.dumps(payload, ensure_ascii=False, separators=(",", ":")) + "\n").encode()
    if len(wire) > 16384:
        raise SetupError("Provisioning payload exceeds the firmware limit.")
    connection = serial.Serial(port=None, baudrate=115200, timeout=0.1, write_timeout=10)
    connection.dtr = False
    connection.rts = False
    connection.port = port
    try:
        connection.open()
        connection.reset_input_buffer()
        connection.write(wire)
        connection.flush()
        deadline = time.monotonic() + timeout
        line = bytearray()
        while time.monotonic() < deadline:
            byte = connection.read(1)
            if not byte:
                continue
            if byte == b"\n":
                try:
                    result = decode_json(bytes(line))
                except SetupError:
                    line.clear()
                    continue
                if isinstance(result, dict):
                    return result
                line.clear()
            else:
                line.extend(byte)
                if len(line) > 4096:
                    raise SetupError("USB response too large; details suppressed.")
        raise SetupError("USB acknowledgement missing. Do not automatically resend tokens; inspect USB status first.")
    except (serial.SerialException, OSError):
        raise SetupError("USB I/O failed. Handoff outcome may be unknown; do not automatically resend tokens.") from None
    finally:
        connection.close()
