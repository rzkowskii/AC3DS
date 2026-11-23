#!/usr/bin/env python3
"""Host tests for the first-run download's portable half (source/3ds/firstrun_core.cpp
and romfs_image.cpp).

Builds scripts/qa/firstrun-core-test.cpp with the host C++ compiler, libbz2 and
zlib, runs its self-test, feeds it hand-made hostile and unusual archives,
damages the images it writes, and, given the official release with --archive,
checks that the image it makes holds exactly the tree scripts/stage-assets.py
stages when no JPEG is converted. Images are read back with the reader in
check-make-3dsx.py, which follows libctru's romfs_dev.c.
"""
import argparse
import bz2
import hashlib
import importlib.util
import io
import os
import pathlib
import random
import shutil
import struct
import subprocess
import sys
import tarfile
import tempfile

REPO = pathlib.Path(__file__).resolve().parent.parent.parent
RELEASE_SHA256 = "9b3118e7929253f0114fa06cad8310faab646d177b4660406011152f34086b55"
RELEASE_SIZE = 52071427
IMAGE_SIZE = 59588112  # firstrun::release_image_bytes
IMAGE_SHA256 = "e825d8bd70d8fa387d6113b8ad45abf4d3f7567445d15ffa739dac629ae5a2bd"

spec = importlib.util.spec_from_file_location("check_make_3dsx", REPO / "scripts" / "qa" / "check-make-3dsx.py")
check_make_3dsx = importlib.util.module_from_spec(spec)
spec.loader.exec_module(check_make_3dsx)


def build(work):
    compiler = os.environ.get("CXX") or shutil.which("c++") or shutil.which("clang++") or shutil.which("g++")
    if not compiler:
        raise SystemExit("no host C++ compiler found")
    binary = work / "firstrun-core-test"
    subprocess.run([compiler, "-std=c++17", "-O2", "-Wall", "-Wextra", "-I", str(REPO / "source/3ds"),
                    str(REPO / "scripts/qa/firstrun-core-test.cpp"), str(REPO / "source/3ds/firstrun_core.cpp"),
                    str(REPO / "source/3ds/romfs_image.cpp"), "-lbz2", "-lz", "-o", str(binary)], check=True)
    return binary


def unpack(binary, archive, out, chunk=16384, size=None, sha=None):
    """Make the image `out` from an archive. A failure must leave nothing behind."""
    data = archive.read_bytes()
    result = subprocess.run([str(binary), "image", str(archive), str(out), str(chunk),
                             str(len(data) if size is None else size), sha or hashlib.sha256(data).hexdigest()],
                            capture_output=True, text=True)
    line = result.stdout.strip()
    ok = result.returncode == 0 and line.startswith("OK")
    if not ok and (out.exists() or pathlib.Path(str(out) + ".part").exists()):
        line += " [a failed run left a file behind]"
    return ok, line


def accepted(binary, image, *required):
    """Whether the check the console runs before mounting takes this image."""
    result = subprocess.run([str(binary), "check", str(image), *required], capture_output=True, text=True)
    line = result.stdout.strip()
    return result.returncode == 0 and line.startswith("OK"), line


def image_tree(binary, image, required=()):
    """Relative path -> SHA-256 for files, None for directories, read as libctru would."""
    ok, line = accepted(binary, image, *required)
    if not ok:
        raise SystemExit(f"{image.name}: a freshly written image is refused: {line}")
    data = image.read_bytes()
    reader = check_make_3dsx.RomFS(data)
    files, folders = reader.walk()
    for path, content in files.items():
        if reader.find(path) != ("file", content):
            raise SystemExit(f"{image.name}: hash lookup of {path} disagrees with the directory listing")
    for path in folders:
        if (reader.find(path) or ("", 0))[0] != "dir":
            raise SystemExit(f"{image.name}: hash lookup does not find the folder {path}")
    found = {path: None for path in folders}
    found.update((path, hashlib.sha256(content).hexdigest()) for path, content in files.items())
    return found


def damaged(binary, image, work, required=()):
    """Cut, extend and flip bits in a good image: the check must refuse each one."""
    failures = []
    good = image.read_bytes()
    tables = struct.unpack_from("<I", good, 4)[0]  # the directory hash table starts the tables
    trailer = len(good) - 72
    noise = random.Random(1302)

    def flip(at, bit=None):
        changed = bytearray(good)
        changed[at] ^= 1 << (noise.randrange(8) if bit is None else bit)
        return bytes(changed)

    cases = {
        "empty": b"",
        "header only": good[:40],
        "cut in the data": good[:tables // 2],
        "cut at the tables": good[:tables],
        "cut in the tables": good[:(tables + trailer) // 2],
        "cut before the end mark": good[:trailer],
        "one byte short": good[:-1],
        "one byte more": good + b"\0",
        "another image appended": good + good,
        "unfinished (no header)": bytes(48) + good[48:],
        "no end mark": good[:trailer] + bytes(72),
        "data start moved": good[:36] + struct.pack("<I", 0x40) + good[40:],
    }
    for at in range(0, 40):
        cases[f"header byte {at}"] = flip(at)
    for at in range(trailer, trailer + 40):
        cases[f"end mark byte {at - trailer}"] = flip(at)
    for index in range(300):
        at = noise.randrange(tables, trailer)
        cases[f"tables byte {at} ({index})"] = flip(at)
    scratch = work / "damaged.romfs"
    for label, data in cases.items():
        scratch.write_bytes(data)
        ok, line = accepted(binary, scratch, *required)
        if ok:
            failures.append(f"damaged image accepted: {label}")
    # Nothing but the release's SHA-256 covers the file data; the tables do
    # not. A changed data byte is not found by the launch check.
    scratch.write_bytes(flip(48 + (tables - 48) // 2, 0) if tables > 48 else good)
    ok, line = accepted(binary, scratch, *required)
    if not ok:
        failures.append(f"an image with one changed data byte is refused, which the check is not meant to do: {line}")
    return failures


def tree(root):
    """Relative path -> SHA-256 for files, None for directories."""
    found = {}
    for path in sorted(root.rglob("*")):
        relative = path.relative_to(root).as_posix()
        if path.is_symlink():
            found[relative] = "symlink"
        elif path.is_dir():
            found[relative] = None
        else:
            found[relative] = hashlib.sha256(path.read_bytes()).hexdigest()
    return found


def member(name, data=b"", kind=tarfile.REGTYPE, link=""):
    info = tarfile.TarInfo(name)
    info.type = kind
    info.linkname = link
    info.mode = 0o775 if kind == tarfile.DIRTYPE else 0o664
    info.size = len(data) if kind == tarfile.REGTYPE else 0
    return info, data


def make_tar(members, fmt=tarfile.GNU_FORMAT):
    raw = io.BytesIO()
    with tarfile.open(fileobj=raw, mode="w", format=fmt) as archive:
        for info, data in members:
            archive.addfile(info, io.BytesIO(data) if info.type == tarfile.REGTYPE else None)
    return raw.getvalue()


def expected(members):
    """What stage-assets.py's rules take from these members."""
    roots = {"config", "packages", "bot", "mods", "demos", "docs", "screenshots"}
    found = {}
    for info, data in members:
        parts = pathlib.PurePosixPath(info.name).parts
        if not parts or parts[0] not in roots:
            continue
        for depth in range(1, len(parts) + (1 if info.type == tarfile.DIRTYPE else 0)):
            found["/".join(parts[:depth])] = None
        if info.type == tarfile.REGTYPE:
            found["/".join(parts)] = hashlib.sha256(data).hexdigest()
    return found


def synthetic(binary, work):
    failures = []
    long_name = "./packages/" + "d" * 60 + "/" + "e" * 60 + "/file.txt"
    base = [
        member("./", kind=tarfile.DIRTYPE),
        member("./config/", kind=tarfile.DIRTYPE),
        member("./config/a.cfg", b"alias a b\n"),
        member("./packages/maps/official/x.cgz", bytes(range(256)) * 300),  # several blocks, parent not listed
        member("./packages/misc/exact.bin", b"\x01" * 1024),               # ends on a block boundary
        member("./docs/empty"),
        member("./bot/", kind=tarfile.DIRTYPE),
        member("./bot/waypoints/", kind=tarfile.DIRTYPE),                   # stays empty
        member("./source/src/main.cpp", b"int main() {}\n" * 100),
        member("./bin_unix/link", kind=tarfile.SYMTYPE, link="/bin/sh"),   # outside the game's folders: skipped
        member("./README.md", b"readme"),
        member(long_name, b"long"),
    ]
    good = {
        "gnu": make_tar(base),                              # long name as a GNU 'L' record
        "pax": make_tar(base, tarfile.PAX_FORMAT),          # long name as a pax path record
        "ustar": make_tar(base[:-1] + [member("./packages/" + "p" * 90 + "/" + "q" * 80, b"prefix")], tarfile.USTAR_FORMAT),
    }
    wanted = {"gnu": expected(base), "pax": expected(base),
              "ustar": expected(base[:-1] + [member("./packages/" + "p" * 90 + "/" + "q" * 80, b"prefix")])}
    assert b"././@LongLink" in good["gnu"] and b"path=" in good["pax"]
    for name, raw in good.items():
        archive = work / f"{name}.tar.bz2"
        archive.write_bytes(bz2.compress(raw))
        for chunk in (1, 7, 511, 4096, 1 << 20):
            out = work / f"out-{name}-{chunk}.romfs"
            ok, line = unpack(binary, archive, out, chunk)
            if not ok or image_tree(binary, out, ["config/a.cfg"]) != wanted[name]:
                failures.append(f"{name} in chunks of {chunk}: {line}")
        failures += damaged(binary, work / f"out-{name}-4096.romfs", work, ["config/a.cfg"])
        ok, line = accepted(binary, work / f"out-{name}-4096.romfs", "config/a.cfg", "config/missing.cfg")
        if ok or "does not hold config/missing.cfg" not in line:
            failures.append(f"{name}: an image without a required file is accepted: {line}")
        ok, line = accepted(binary, work / f"out-{name}-4096.romfs", "config", "config/a.cfg")
        if ok:
            failures.append(f"{name}: a folder passes for a required file: {line}")

    def bad(name, members=None, raw=None, compressed=None, want="", **options):
        if raw is None and compressed is None:
            raw = make_tar(members)
        archive = work / f"bad-{name}.tar.bz2"
        archive.write_bytes(bz2.compress(raw) if compressed is None else compressed)
        out = work / f"out-bad-{name}.romfs"
        ok, line = unpack(binary, archive, out, **options)
        if ok or want not in line or "left a file behind" in line:
            failures.append(f"{name}: expected an error containing {want!r}, got: {line}")

    keep = [member("./config/a.cfg", b"a")]
    bad("parent", keep + [member("../evil", b"x")], want="unsafe")
    bad("parent-inside", keep + [member("./config/../../evil", b"x")], want="unsafe")
    bad("parent-skipped-folder", keep + [member("./source/../../evil", b"x")], want="unsafe")
    bad("absolute", keep + [member("/etc/evil", b"x")], want="unsafe")
    bad("absolute-root", keep + [member("/config/evil", b"x")], want="unsafe")
    bad("backslash", keep + [member("./packages/a\\b", b"x")], want="unsafe")
    bad("colon", keep + [member("./packages/a:b", b"x")], want="unsafe")
    bad("symlink", keep + [member("./config/link", kind=tarfile.SYMTYPE, link="/etc/passwd")], want="unexpected archive member type")
    bad("hardlink", keep + [member("./packages/link", kind=tarfile.LNKTYPE, link="config/a.cfg")], want="unexpected archive member type")
    bad("fifo", keep + [member("./config/fifo", kind=tarfile.FIFOTYPE)], want="unexpected archive member type")
    bad("device", keep + [member("./config/dev", kind=tarfile.CHRTYPE)], want="unexpected archive member type")
    long_link = make_tar(keep + [member("./packages/" + "l" * 80, kind=tarfile.SYMTYPE, link="t" * 150)])
    assert long_link.count(b"././@LongLink") == 1  # a GNU 'K' record for the target
    bad("long-symlink", raw=long_link, want="unexpected archive member type")
    bad("long-name", keep + [member("./packages/" + "n" * 120, b"x")], want="unsafe")
    bad("duplicate", keep + [member("./config/a.cfg", b"second")], want="appears twice")
    bad("file-then-folder", keep + [member("./config/a.cfg/", kind=tarfile.DIRTYPE)], want="both a file and a folder")
    bad("file-as-parent", keep + [member("./config/a.cfg/b", b"x")], want="both a file and a folder")
    bad("folder-then-file", [member("./config/sub/", kind=tarfile.DIRTYPE), member("./config/sub", b"x")], want="both a file and a folder")
    bad("implied-folder-then-file", [member("./config/sub/a", b"x"), member("./config/sub", b"x")], want="both a file and a folder")
    # The image keeps names as they are, so these two stay apart, as they do
    # in an asset-embedded build.
    case = work / "case.tar.bz2"
    case.write_bytes(bz2.compress(make_tar([member("./config/a.cfg", b"lower"), member("./config/A.cfg", b"upper")])))
    ok, line = unpack(binary, case, work / "case.romfs")
    if not ok or len(image_tree(binary, work / "case.romfs", ["--none"])) != 3:
        failures.append(f"names differing in case: {line}")
    nothing = work / "nothing.tar.bz2"
    nothing.write_bytes(bz2.compress(make_tar([member("./README.md", b"readme")])))
    ok, line = unpack(binary, nothing, work / "nothing.romfs")
    if not ok or image_tree(binary, work / "nothing.romfs", ["--none"]) != {}:
        failures.append(f"an archive with none of the game's folders: {line}")
    ok, line = accepted(binary, work / "nothing.romfs")
    if ok or "does not hold" not in line:
        failures.append(f"an image without the game's files is accepted: {line}")

    plain = make_tar(keep + [member("./packages/b", b"b" * 5000)])
    packed = bz2.compress(plain)
    bad("wrong-hash", compressed=packed, want="SHA-256 mismatch", sha="0" * 64)
    bad("wrong-size", compressed=packed, want="bytes arrived", size=len(packed) + 1)
    bad("cut-compressed", compressed=packed[:-20], want="ends early")
    bad("no-end-marker", raw=plain[:plain.index(b"\0" * 1024, 512 * 12)], want="ends early")
    bad("cut-in-file", raw=plain[:512 * 4 + 100], want="ends early")
    bad("after-compressed-end", compressed=packed + b"extra", want="after the end")
    bad("second-stream", compressed=packed + packed, want="after the end")
    bad("after-tar-end", raw=plain + b"x", want="after the end")
    bad("not-bzip2", compressed=b"<html>404 Not Found</html>" * 100, want="damaged")
    broken = bytearray(plain)
    broken[512 * 2 + 3] ^= 1  # second header, in the name
    bad("checksum", raw=bytes(broken), want="checksum")
    bad("bomb", raw=b"\0" * (200 << 20), want="larger than expected")
    return failures


def reference(archive, work):
    """Stage with scripts/stage-assets.py, with no jpegtran on the PATH."""
    path = "/usr/bin:/bin"
    if shutil.which("jpegtran", path=path):
        raise SystemExit("jpegtran is in /usr/bin or /bin: cannot produce an unconverted reference tree")
    staged = work / "reference"
    subprocess.run([sys.executable, str(REPO / "scripts/stage-assets.py"), str(archive), str(staged)],
                   check=True, env={**os.environ, "PATH": path}, capture_output=True)
    marker = (staged / ".assaultcube-assets").read_text()
    if marker != RELEASE_SHA256 + "\n":
        raise SystemExit(f"reference tree was post-processed (marker {marker!r})")
    found = tree(staged)
    del found[".assaultcube-assets"]
    return found, staged


def release(binary, archive, work):
    failures = []
    wanted, staged = reference(archive, work)
    files = [path for path, digest in wanted.items() if digest]
    size = sum((staged / path).stat().st_size for path in files)
    print(f"reference: {len(files)} files, {len(wanted) - len(files)} folders, {size} bytes")
    for chunk in (16384, 1000):
        out = work / f"release-{chunk}.romfs"
        ok, line = unpack(binary, archive, out, chunk, RELEASE_SIZE, RELEASE_SHA256)
        print(f"release in chunks of {chunk}: {line}")
        got = image_tree(binary, out) if ok else {}
        if ok and out.stat().st_size != IMAGE_SIZE:
            failures.append(f"the release image is {out.stat().st_size} bytes; firstrun_core.h says {IMAGE_SIZE}")
        if ok and hashlib.sha256(out.read_bytes()).hexdigest() != IMAGE_SHA256:
            failures.append("the release image is not the one this test pins; if the layout changed on purpose, update IMAGE_SHA256")
        if ok and (" appends=228 " not in line or " largest=262144 " not in line):
            failures.append(f"the release image was not written in 228 blocks of 256 KiB: {line}")
        if got != wanted:
            missing = sorted(set(wanted) - set(got))[:5]
            extra = sorted(set(got) - set(wanted))[:5]
            differ = sorted(path for path in set(got) & set(wanted) if got[path] != wanted[path])[:5]
            failures.append(f"release in chunks of {chunk}: tree differs (missing {missing}, extra {extra}, changed {differ})")
        lowered = {}
        for path in got:
            if lowered.setdefault(path.lower(), path) != path:
                failures.append(f"names differ only in case: {path} and {lowered[path.lower()]}")
    failures += damaged(binary, work / "release-16384.romfs", work)
    ok, line = unpack(binary, archive, work / "release-badhash.romfs", 16384, RELEASE_SIZE, "0" * 64)
    if ok or "SHA-256 mismatch" not in line:
        failures.append(f"release with a wrong hash: {line}")
    return failures


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--archive", type=pathlib.Path,
                        help="the official AssaultCube_v1.3.0.2_LockdownEdition_RC1.tar.bz2, for the comparison with stage-assets.py")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="ac3ds-firstrun-test-") as temp:
        work = pathlib.Path(temp)
        binary = build(work)
        failures = []
        if subprocess.run([str(binary), "selftest"]).returncode != 0:
            failures.append("selftest")
        failures += synthetic(binary, work)
        print(f"synthetic archives: {'ok' if len(failures) == 0 else 'FAILED'}")
        if args.archive:
            failures += release(binary, args.archive, work)
        for failure in failures:
            print("FAILED:", failure)
        print("all passed" if not failures else f"{len(failures)} failed")
        return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
