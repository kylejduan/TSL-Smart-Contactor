# Contributing

This is a reusable open-source program. Keep public commits about source,
reproducible tests, generic setup and sanitized technical findings.

Do not commit installation diaries, trip/charging schedules, status exports,
actual VIN/home coordinates, network identifiers, transaction IDs, real capture
timestamps, credentials, generated keys/certificates or machine-specific paths.
Use fully synthetic test inputs. Keep real records in ignored local storage or a
protected directory outside the repository. Generated application public keys
must be hosted for Tesla registration, but each installation deploys its own;
they do not belong in this generic source tree. Review issue/support attachments.

Run `python3 tools/check_public_tree.py --history` before publication and inspect
the exact staged diff. The automated check catches common record/key/path leaks;
it cannot recognize every secret or personal narrative. CI runs the same guard.
Do not blindly stage ignored or unowned files. Commit as the public GitHub handle
with its noreply email. Never reintroduce superseded history after a privacy
rewrite; use a fresh clone or carefully transplant only reviewed clean changes.

Follow [software verification](docs/verification.md) for build/test commands.
Changes to firmware require relevant production tests and a target build.
Hardware flashing, relay-ON tests, live API requests and fault injection require
explicit operator approval and the appropriate isolated bench setup.
