#!/usr/bin/env python3
"""Fetches the parts of an LLVM release the ahead-of-time tier needs on Windows.

LLVM's Windows installer has `LLVM-C.dll` and nothing to compile against
it, so this takes the C API headers, the import library and the DLL out
of the official release archive and leaves them where CMake looks:
`build/llvm-sdk` (D208).

    python tools/package/fetch_llvm.py [--version 22.1.8] [--arch x86_64]
"""

import argparse
import sys
import tarfile
import urllib.error
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
WANTED = ("/include/llvm-c/", "/include/llvm/Config/", "/lib/LLVM-C.lib", "/bin/LLVM-C.dll")


def open_archive(path):
    if path.suffix == ".xz":
        import lzma
        return lzma.open(path, "rb")
    from compression import zstd
    return zstd.ZstdFile(path, "rb", options={zstd.DecompressionParameter.window_log_max: 31})


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--version", default="22.1.8")
    parser.add_argument("--arch", default="x86_64", choices=["x86_64", "aarch64"])
    arguments = parser.parse_args()

    name = "clang+llvm-{}-{}-pc-windows-msvc".format(arguments.version, arguments.arch)
    destination = ROOT / "build" / "llvm-sdk"
    if (destination / name / "lib" / "LLVM-C.lib").exists():
        print("already there: {}".format(destination / name))
        return 0

    destination.mkdir(parents=True, exist_ok=True)
    archive_path = None
    for suffix in (".tar.zst", ".tar.xz"):
        candidate = destination / (name + suffix)
        if candidate.exists():
            archive_path = candidate
            break
        url = "https://github.com/llvm/llvm-project/releases/download/llvmorg-{}/{}{}".format(arguments.version, name, suffix)
        print("fetching {}".format(url))
        try:
            urllib.request.urlretrieve(url, candidate)
        except urllib.error.HTTPError:
            continue
        archive_path = candidate
        break
    if archive_path is None:
        print("no Windows archive for LLVM {}".format(arguments.version))
        return 1

    count = 0
    with open_archive(archive_path) as raw:
        with tarfile.open(fileobj=raw, mode="r|") as archive:
            for member in archive:
                if member.isfile() and any(part in member.name for part in WANTED):
                    archive.extract(member, destination, filter="data")
                    count += 1

    archive_path.unlink()
    print("{} files in {}".format(count, destination / name))
    return 0


if __name__ == "__main__":
    sys.exit(main())
