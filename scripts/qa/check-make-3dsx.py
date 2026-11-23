#!/usr/bin/env python3
"""Check scripts/make-3dsx.py and tools/builder against each other and devkitPro.

Always: builds the RomFS image from the official archive, reads every file
back out of it the way libctru does (hash table lookup, then sibling chains),
and feeds the builder wrong inputs.

With --3dsxtool, --elf and --smdh: writes the same files to a scratch folder,
lets devkitPro's 3dsxtool pack them, and compares its RomFS region byte for
byte with ours laid out in the same directory order.

With --node: runs tools/builder/build-node.js on the same inputs and compares
the whole output file with the Python one.
"""
import argparse
import bz2
import hashlib
import importlib.util
import os
import pathlib
import random
import struct
import subprocess
import sys
import tempfile

REPO = pathlib.Path(__file__).resolve().parent.parent.parent
SCRIPT = REPO / "scripts" / "make-3dsx.py"
NODE_SCRIPT = REPO / "tools" / "builder" / "build-node.js"

spec = importlib.util.spec_from_file_location("make_3dsx", SCRIPT)
make_3dsx = importlib.util.module_from_spec(spec)
spec.loader.exec_module(make_3dsx)

failures = 0


def check(label, passed, detail=""):
    global failures
    failures += not passed
    print(f"{'ok  ' if passed else 'FAIL'} {label}{' - ' + detail if detail else ''}")


def flatten(tree, prefix=""):
    files, folders = {}, set()
    for name, child in tree.items():
        if isinstance(child, dict):
            folders.add(prefix + name)
            more_files, more_folders = flatten(child, prefix + name + "/")
            files.update(more_files)
            folders |= more_folders
        else:
            files[prefix + name] = child
    return files, folders


class RomFS:
    """Reader that follows libctru's romfs_dev.c."""

    def __init__(self, image):
        self.image = image
        (length, self.dir_hash_off, dir_hash_size, self.dir_off, self.dir_size, self.file_hash_off,
         file_hash_size, self.file_off, self.file_size, self.data_off) = struct.unpack_from("<10I", image, 0)
        assert length == 0x28
        self.dir_hash = struct.unpack_from(f"<{dir_hash_size // 4}I", image, self.dir_hash_off)
        self.file_hash = struct.unpack_from(f"<{file_hash_size // 4}I", image, self.file_hash_off)

    def dir_entry(self, offset):
        fields = struct.unpack_from("<6I", self.image, self.dir_off + offset)
        start = self.dir_off + offset + 0x18
        return fields + (self.image[start:start + fields[5]],)

    def file_entry(self, offset):
        fields = struct.unpack_from("<2I2Q2I", self.image, self.file_off + offset)
        start = self.file_off + offset + 0x20
        return fields + (self.image[start:start + fields[5]],)

    def find(self, path):
        """Return ("dir", offset), ("file", bytes) or None, by hash lookup."""
        parent = 0
        parts = path.split("/")
        for index, part in enumerate(parts):
            name = part.encode("utf-16-le")
            last = index == len(parts) - 1
            found = None
            offset = self.dir_hash[make_3dsx.name_hash(parent, name, len(self.dir_hash))]
            while offset != make_3dsx.ROMFS_NONE:
                entry = self.dir_entry(offset)
                if entry[0] == parent and entry[6] == name:
                    found = offset
                    break
                offset = entry[4]
            if found is not None:
                parent = found
                continue
            if not last:
                return None
            offset = self.file_hash[make_3dsx.name_hash(parent, name, len(self.file_hash))]
            while offset != make_3dsx.ROMFS_NONE:
                entry = self.file_entry(offset)
                if entry[0] == parent and entry[6] == name:
                    start = self.data_off + entry[2]
                    return "file", self.image[start:start + entry[3]]
                offset = entry[4]
            return None
        return "dir", parent

    def walk(self, offset=0, prefix=""):
        """List every file and folder by following the sibling chains."""
        files, folders = {}, set()
        entry = self.dir_entry(offset)
        child = entry[3]
        while child != make_3dsx.ROMFS_NONE:
            item = self.file_entry(child)
            assert item[0] == offset
            start = self.data_off + item[2]
            files[prefix + item[6].decode("utf-16-le")] = self.image[start:start + item[3]]
            child = item[1]
        child = entry[2]
        while child != make_3dsx.ROMFS_NONE:
            item = self.dir_entry(child)
            assert item[0] == offset
            name = prefix + item[6].decode("utf-16-le")
            folders.add(name)
            more_files, more_folders = self.walk(child, name + "/")
            files.update(more_files)
            folders |= more_folders
            child = item[1]
        return files, folders


def check_image(label, image, files, folders):
    reader = RomFS(image)
    walked_files, walked_folders = reader.walk()
    check(f"{label}: sibling chains list every file and folder",
          walked_files == files and walked_folders == folders, f"{len(walked_files)} files, {len(walked_folders)} folders")
    by_hash = all(reader.find(path) == ("file", data) for path, data in files.items())
    by_hash = by_hash and all((reader.find(path) or ("", 0))[0] == "dir" for path in folders)
    check(f"{label}: hash lookup finds every file and folder with the right bytes", by_hash)
    check(f"{label}: a missing name is not found", reader.find("config/no-such-file.cfg") is None)


def check_bzip2(node, temp):
    """Compare the JavaScript bzip2 decoder with Python's on awkward inputs."""
    noise = random.Random(1302)
    samples = {
        "empty": b"",
        "one byte": b"a",
        "runs of 3, 4, 5, 255, 259 and 260": b"".join(bytes([65 + i]) * n for i, n in enumerate((3, 4, 5, 255, 259, 260))),
        "long run": b"\0" * 3000000,
        "two blocks of noise": noise.randbytes(250000),
        "text": b"".join(b"line %d of some repetitive text\n" % (i % 977) for i in range(60000)),
        "all byte values": bytes(range(256)) * 700,
    }
    packed, plain = temp / "sample.bz2", temp / "sample.out"
    for label, data in samples.items():
        for level in (1, 9):
            packed.write_bytes(bz2.compress(data, level))
            run = subprocess.run([node, str(NODE_SCRIPT), "--bunzip2", str(packed), str(plain)], capture_output=True, text=True)
            check(f"bzip2 decoder: {label}, level {level}", run.returncode == 0 and plain.read_bytes() == data,
                  run.stderr.strip())
    packed.write_bytes(bz2.compress(b"first ") + bz2.compress(b"second"))
    run = subprocess.run([node, str(NODE_SCRIPT), "--bunzip2", str(packed), str(plain)], capture_output=True, text=True)
    check("bzip2 decoder: two streams in one file", run.returncode == 0 and plain.read_bytes() == b"first second")
    good = bytearray(bz2.compress(samples["text"]))
    for label, bad in (("cut short", good[:len(good) // 2]), ("one changed bit", good[:5000] + bytes([good[5000] ^ 4]) + good[5001:]),
                       ("not bzip2", b"MZ" + bytes(100))):
        packed.write_bytes(bad)
        run = subprocess.run([node, str(NODE_SCRIPT), "--bunzip2", str(packed), str(plain)], capture_output=True, text=True)
        check(f"bzip2 decoder refuses input that is {label}", run.returncode == 1, run.stderr.strip())


def expect_error(label, call, needle):
    try:
        call()
    except make_3dsx.BuildError as error:
        check(label, needle in str(error), str(error))
    else:
        check(label, False, "no error raised")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("archive", type=pathlib.Path)
    parser.add_argument("engine", type=pathlib.Path, help="engine-only assaultcube.3dsx")
    parser.add_argument("--3dsxtool", dest="tool", type=pathlib.Path)
    parser.add_argument("--elf", type=pathlib.Path, help="any 3DS ELF, for the throwaway 3dsxtool run")
    parser.add_argument("--smdh", type=pathlib.Path, help="any SMDH, for the throwaway 3dsxtool run")
    parser.add_argument("--node", help="node executable; compares the JavaScript builder with the Python one")
    args = parser.parse_args()

    engine = args.engine.read_bytes()
    tree = make_3dsx.read_release(args.archive)
    files, folders = flatten(tree)
    romfs = make_3dsx.build_romfs(tree)
    check("RomFS image matches the pinned size and SHA-256",
          len(romfs) == make_3dsx.ROMFS_SIZE and hashlib.sha256(romfs).hexdigest() == make_3dsx.ROMFS_SHA256,
          f"{len(romfs)} {hashlib.sha256(romfs).hexdigest()}")
    check_image("sorted order", romfs, files, folders)

    result = make_3dsx.attach(engine, romfs)
    fs_offset = struct.unpack_from("<I", result, 0x28)[0]
    check("output keeps the engine bytes and points fsOffset at the image",
          result[:0x28] == engine[:0x28] and result[0x2C:len(engine)] == engine[0x2C:]
          and fs_offset % 4 == 0 and fs_offset >= len(engine) and result[fs_offset:] == romfs)

    with tempfile.TemporaryDirectory(prefix="check-make-3dsx-") as temp:
        temp = pathlib.Path(temp)
        staged = temp / "romfs"
        make_3dsx.write_tree(tree, staged)
        check("staged folder has the marker stage-assets.py writes for an unconverted tree",
              (staged / ".assaultcube-assets").read_text() == make_3dsx.RELEASE_SHA256 + "\n")

        if args.tool:
            tool_out = temp / "tool.3dsx"
            subprocess.run([str(args.tool), str(args.elf), str(tool_out), f"--smdh={args.smdh}", f"--romfs={staged}"],
                           check=True)
            packed = tool_out.read_bytes()
            tool_romfs = packed[struct.unpack_from("<I", packed, 0x28)[0]:]
            # 3dsxtool lays entries out in readdir() order; os.listdir() returns the same order.
            listed = make_3dsx.build_romfs(tree, order=lambda path, names: os.listdir(staged.joinpath(*path)))
            check("same directory order as 3dsxtool: RomFS region is byte-identical", listed == tool_romfs,
                  f"{len(tool_romfs)} bytes, SHA-256 {hashlib.sha256(tool_romfs).hexdigest()}")
            check("3dsxtool leaves the dot-named marker out, as we do",
                  ".assaultcube-assets".encode("utf-16-le") not in tool_romfs)
            check_image("3dsxtool image", tool_romfs, files, folders)
            check("sorted order differs from 3dsxtool's only in layout (same size)",
                  len(romfs) == len(tool_romfs), f"identical bytes: {romfs == tool_romfs}")

        python_out = temp / "python.3dsx"
        run = subprocess.run([sys.executable, str(SCRIPT), str(args.archive), str(args.engine), str(python_out)],
                             capture_output=True, text=True)
        check("command line run writes the same file", run.returncode == 0 and python_out.read_bytes() == result,
              run.stderr.strip())

        if args.node:
            node_out = temp / "node.3dsx"
            for mode in ("webcrypto", "fallback"):
                node_out.unlink(missing_ok=True)
                run = subprocess.run([args.node, str(NODE_SCRIPT), str(args.archive), str(args.engine), str(node_out)]
                                     + (["--no-webcrypto"] if mode == "fallback" else []),
                                     capture_output=True, text=True)
                same = run.returncode == 0 and node_out.read_bytes() == result
                check(f"JavaScript builder ({mode} SHA-256) output is byte-identical to Python", same,
                      run.stderr.strip() or f"SHA-256 {hashlib.sha256(result).hexdigest()}")

        if args.node:
            check_bzip2(args.node, temp)

        # Wrong inputs: each must fail with a clear message and write nothing.
        archive_bytes = args.archive.read_bytes()
        truncated = temp / make_3dsx.RELEASE_NAME
        truncated.write_bytes(archive_bytes[:len(archive_bytes) // 2])
        expect_error("truncated archive is refused", lambda: make_3dsx.read_release(truncated), "Download it again")
        flipped = bytearray(archive_bytes)
        flipped[len(flipped) // 2] ^= 1
        truncated.write_bytes(flipped)
        expect_error("archive with one changed bit is refused", lambda: make_3dsx.read_release(truncated), "SHA-256")
        wrong = temp / "AssaultCube_v1.3.0.2_LockdownEdition_RC1.exe"
        wrong.write_bytes(b"MZ" + bytes(4096))
        expect_error("a different file is refused", lambda: make_3dsx.read_release(wrong), make_3dsx.RELEASE_NAME)
        expect_error("engine that already has RomFS is refused", lambda: make_3dsx.check_engine(result),
                     "already has game data")
        expect_error("non-3DSX engine is refused", lambda: make_3dsx.check_engine(archive_bytes[:65536]),
                     "not a 3DSX")
        expect_error("truncated engine is refused", lambda: make_3dsx.check_engine(engine[:len(engine) // 2]),
                     "damaged or incomplete")
        plain = bytearray(engine[:32])
        struct.pack_into("<H", plain, 4, 32)
        expect_error("3DSX without the extended header is refused", lambda: make_3dsx.check_engine(bytes(plain)),
                     "no extended header")
        other = engine.replace(make_3dsx.ENGINE_SIGNATURE, b"/3ds/otherprogra")
        expect_error("another program's 3DSX is refused", lambda: make_3dsx.check_engine(other), "not AC-3DS")
        refused = temp / "refused.3dsx"
        for label, archive, engine_path in (("wrong archive", wrong, args.engine),
                                            ("engine with RomFS", args.archive, python_out)):
            run = subprocess.run([sys.executable, str(SCRIPT), str(archive), str(engine_path), str(refused)],
                                 capture_output=True, text=True)
            check(f"command line run with {label} fails and writes nothing",
                  run.returncode != 0 and not refused.exists() and len(list(temp.glob(".refused*"))) == 0,
                  run.stderr.strip())
            if args.node:
                run = subprocess.run([args.node, str(NODE_SCRIPT), str(archive), str(engine_path), str(refused)],
                                     capture_output=True, text=True)
                check(f"JavaScript run with {label} fails and writes nothing",
                      run.returncode != 0 and not refused.exists(), run.stderr.strip())

    print("check-make-3dsx: " + ("all checks passed" if not failures else f"{failures} check(s) FAILED"))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
