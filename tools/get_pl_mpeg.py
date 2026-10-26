#!/usr/bin/env python3
"""Fetch the pinned MIT-licensed video decoder for the cutscene mod (src/cutscene.c) on any platform.

    python tools/get_pl_mpeg.py            fetch third_party/pl_mpeg/pl_mpeg.h (kept when it is already the pinned file)
    python tools/get_pl_mpeg.py --check    verify the file present against the pin; prints `pl_mpeg: ...` (exit 1 when not)

pl_mpeg (github.com/phoboslab/pl_mpeg, MIT, Dominic Szablewski): one header -- an MPEG-1 video and MP2 audio decoder with an
MPEG-PS demuxer. Pinned by commit and by the SHA-256 of the header: a file whose hash is not the pin is refused, so a
build never compiles a decoder nobody read. Without it Belthasar still builds -- the cutscene mod is compiled out
(CT_HAVE_PL_MPEG undefined) and its option says so.

The raw file of the pinned commit is fetched first; where it cannot be (a cloud session's proxy), a shallow fetch of the
same commit with git gives the same bytes.
"""
import hashlib
from pathlib import Path
import subprocess
import sys
import tempfile
import urllib.error
import urllib.request

COMMIT = "c871f2be022ece7ef4f64230b4fb8e1fb9eb6023"
SHA256 = "3a8cb30c83c2a1147719c30fe0c8b93da2987aa43140077c575b39aaa75fc2c9"
REPO = "https://github.com/phoboslab/pl_mpeg"
ROOT = Path(__file__).resolve().parent.parent
DEST = ROOT / "third_party" / "pl_mpeg" / "pl_mpeg.h"


def sha(data):
    return hashlib.sha256(data).hexdigest()


def from_raw():
    url = f"https://raw.githubusercontent.com/phoboslab/pl_mpeg/{COMMIT}/pl_mpeg.h"
    print(f"Fetching pl_mpeg {COMMIT[:7]}: {url}", flush=True)
    with urllib.request.urlopen(url, timeout=60) as response:
        return response.read()


def from_git():
    print(f"Fetching pl_mpeg {COMMIT[:7]} with git: {REPO}.git", flush=True)
    with tempfile.TemporaryDirectory() as tmp:
        for args in (["init", "-q"], ["remote", "add", "origin", REPO + ".git"],
                     ["fetch", "-q", "--depth", "1", "origin", COMMIT], ["checkout", "-q", "FETCH_HEAD"]):
            subprocess.run(["git", "-C", tmp] + args, check=True)
        return (Path(tmp) / "pl_mpeg.h").read_bytes()


def check():
    if not DEST.exists():
        print(f"pl_mpeg: MISSING {DEST.relative_to(ROOT)} (python tools/get_pl_mpeg.py)")
        return 1
    got = sha(DEST.read_bytes())
    if got != SHA256:
        print(f"pl_mpeg: NOT THE PIN -- {DEST.relative_to(ROOT)} sha256 {got}, pinned {SHA256} (commit {COMMIT[:7]})")
        return 1
    print(f"pl_mpeg: pinned {COMMIT[:7]} sha256 {SHA256[:16]} ok")
    return 0


def main():
    if "--check" in sys.argv[1:]:
        return check()
    if DEST.exists() and sha(DEST.read_bytes()) == SHA256:
        print(f"pl_mpeg {COMMIT[:7]} already present at {DEST}")
        return 0
    try:
        data = from_raw()
    except (urllib.error.URLError, OSError) as e:
        print(f"The raw file could not be fetched ({e}); trying git", flush=True)
        data = from_git()
    if sha(data) != SHA256:
        print(f"pl_mpeg: the fetched file's sha256 is {sha(data)}, not the pinned {SHA256}: refused")
        return 1
    DEST.parent.mkdir(parents=True, exist_ok=True)
    DEST.write_bytes(data)
    print(f"pl_mpeg {COMMIT[:7]} installed in {DEST}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
