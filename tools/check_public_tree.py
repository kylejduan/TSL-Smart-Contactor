"""Check tracked content/history for installation records; never print matched data."""
from __future__ import annotations
import argparse
import re
import subprocess
from pathlib import Path

# Generic guards. Examples/tests may use clearly synthetic identities and times.
RULES = {
    'private_key': rb'-----BEGIN (?:RSA |EC |OPENSSH |ENCRYPTED )?PRIVATE KEY-----',
    'personal_machine_path': rb'(?:/home/(?!runner/|user/|<)[A-Za-z0-9_.-]+/|[A-Za-z]:\\Users\\(?!<)[A-Za-z0-9_.-]+\\|/mnt/c/Users/(?!<)[A-Za-z0-9_.-]+/)',
    'installation_record': rb'(?:controller-[0-9]{8}T[0-9]{6}|gps-independent-[0-9]{8}T[0-9]{6}|\.tools/local-' + rb'recovery/windows-usb)',
}
DOC_RULES = {
    'hardware_identifier': rb'\b(?:[0-9a-f]{2}[:-]){5}[0-9a-f]{2}\b',
    'personal_runtime_timing': rb'\b[0-9]{2}:[0-9]{2}(?::[0-9]{2})?\b',
}
PRIVATE_SUFFIXES = {'.pem', '.key', '.p12', '.pfx', '.jsonl', '.log', '.bin', '.elf', '.map'}
PRIVATE_PARTS = {'provisioning', '.tools', '.research', '.venv', '.vercel', 'local-records'}


def violations(path: str, data: bytes) -> list[str]:
    p = Path(path)
    found = []
    if (p.suffix in PRIVATE_SUFFIXES or set(p.parts) & PRIVATE_PARTS
            or p.name == 'env.txt' or p.name == '.env' or p.name.startswith('.env.')):
        found.append('private_file')
    for name, pattern in RULES.items():
        if re.search(pattern, data, re.I):
            found.append(name)
    if path == 'README.md' or path.startswith('docs/'):
        for name, pattern in DOC_RULES.items():
            if re.search(pattern, data, re.I):
                found.append(name)
    return found


def git(*args: str) -> bytes:
    return subprocess.check_output(['git', *args])


def check(history: bool) -> list[tuple[str, str]]:
    errors = set()
    for name in git('ls-files', '-z').decode().split('\0'):
        if not name:
            continue
        p = Path(name)
        if p.is_file():
            errors.update((name, issue) for issue in violations(name, p.read_bytes()))
    if history:
        objects = {}
        for line in git('rev-list', '--objects', '--all').splitlines():
            parts = line.split(b' ', 1)
            if len(parts) == 2:
                objects[parts[0]] = parts[1].decode()
        with subprocess.Popen(['git', 'cat-file', '--batch'], stdin=subprocess.PIPE,
                              stdout=subprocess.PIPE) as process:
            for oid, name in objects.items():
                process.stdin.write(oid + b'\n')
                process.stdin.flush()
                header = process.stdout.readline().split()
                if len(header) != 3:
                    raise RuntimeError('Cannot inspect Git object')
                data = process.stdout.read(int(header[2]))
                process.stdout.read(1)
                if header[1] == b'blob':
                    errors.update(('history:' + name, issue) for issue in violations(name, data))
            process.stdin.close()
            if process.wait() != 0:
                raise RuntimeError('Git object inspection failed')
    return sorted(errors)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--history', action='store_true')
    args = parser.parse_args()
    errors = check(args.history)
    for path, issue in errors:
        print(f'{path}: {issue}')
    if errors:
        print(f'Public-tree check failed: {len(errors)} finding(s); values suppressed.')
        return 1
    print('Public-tree check passed. Review is still required for secrets and personal narrative.')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
