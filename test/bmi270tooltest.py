#!/usr/bin/env python3
"""
bmi270tooltest.py -- tools/fetch_bmi270_config.py on synthetic sources:
the array taken out byte for byte, and an empty image, not a failed
build, for a wrong hash, a wrong size, no array, or no file. Nothing of
Bosch's is used: the sources are made up here.

SPDX-License-Identifier: MIT
"""
import hashlib
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.path.join(HERE, "..", "tools", "fetch_bmi270_config.py")

checks = failures = 0


def check(ok, what):
    global checks, failures
    checks += 1
    if not ok:
        failures += 1
        print("FAIL: " + what)


def source(data, name="bmi270_config_file"):
    rows = ",\n    ".join(", ".join("0x%02x" % b for b in data[i:i + 12])
                          for i in range(0, len(data), 12))
    return ("/* made up */\nconst uint8_t other[] = { 0x01, 0x02 };\n"
            "const uint8_t %s[] = {\n    %s\n};\nint after = 0x7f;\n" % (name, rows))


def run(tmp, text, sha):
    src = os.path.join(tmp, "in.c")
    out = os.path.join(tmp, "out.bin")
    if text is not None:
        with open(src, "w") as f:
            f.write(text)
    elif os.path.exists(src):
        os.remove(src)
    r = subprocess.run([sys.executable, "-I", TOOL, out, "--file", src, "--sha256", sha],
                       capture_output=True, text=True)
    with open(out, "rb") as f:
        return r.returncode, f.read()


def main():
    data = bytes((i * 37 + 11) & 0xFF for i in range(8192))
    sha = hashlib.sha256(data).hexdigest()
    with tempfile.TemporaryDirectory() as tmp:
        rc, got = run(tmp, source(data), sha)
        check(rc == 0 and got == data, "the array, byte for byte")
        rc, got = run(tmp, source(data), sha.upper())
        check(got == data, "the hash is not case-sensitive")
        rc, got = run(tmp, source(data), "0" * 64)
        check(rc == 0 and got == b"", "a wrong hash wrote %d bytes" % len(got))
        short = data[:8190]
        rc, got = run(tmp, source(short), hashlib.sha256(short).hexdigest())
        check(rc == 0 and got == b"", "8190 bytes accepted")
        rc, got = run(tmp, source(data, "something_else"), sha)
        check(rc == 0 and got == b"", "no array, and %d bytes" % len(got))
        rc, got = run(tmp, None, sha)
        check(rc == 0 and got == b"", "no file, and %d bytes" % len(got))
    print("bmi270tooltest: %d checks, %d failures" % (checks, failures))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
