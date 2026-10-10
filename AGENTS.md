# Public repository boundaries

This is reusable open-source firmware. Follow CONTRIBUTING.md. Public documents
may contain generic procedures, primary-source findings, reproducible software
checks and clearly synthetic examples. Never commit a personal installation diary:
trip/charging schedules, actual VIN/home coordinates, LAN/MAC identifiers, raw
capture timestamps, transaction IDs, machine paths or live status exports.
Retain detailed observations in ignored private storage, not in public acceptance
or verification documents. Generated keys/certificates stay ignored, including
installation-specific public application keys.

Before publication, run `python3 tools/check_public_tree.py --history` and review
the exact staged diff for personal narrative and secrets the guard cannot detect.
Use the public GitHub handle/noreply identity. Preserve unrelated changes. Do not
rewrite history without explicit authorization or reintroduce superseded history.

An operator's confirmed USB setup and authorization cover the requested firmware
installation and supervised update/recovery tests; do not repeatedly ask for the
same confirmation. Relay-ON tests, live Fleet requests and billing changes still
require explicit authorization. Do not change a running installation or its
spending/presence policy as part of documentation work.
