#!/usr/bin/env python3
"""Join the engine-only assaultcube.3dsx with the official game files.

Reads the official Lockdown Edition RC1 tar.bz2 and an engine-only
assaultcube.3dsx, and writes one assaultcube.3dsx with the game data attached
as RomFS. No devkitPro tools are needed. The result is for your own SD card;
it contains AssaultCube's media and must not be shared.

The same steps run in the browser from tools/builder/; both produce the same
bytes for the same inputs.
"""
import argparse
import hashlib
import os
import pathlib
import struct
import sys
import tarfile

RELEASE_NAME = "AssaultCube_v1.3.0.2_LockdownEdition_RC1.tar.bz2"
RELEASE_SIZE = 52071427
RELEASE_SHA256 = "9b3118e7929253f0114fa06cad8310faab646d177b4660406011152f34086b55"
DOWNLOAD_PAGE = "https://assault.cubers.net/download.html"
# Extraction rules shared with stage-assets.py.
ROOTS = {"config", "packages", "bot", "mods", "demos", "docs", "screenshots"}
REQUIRED = {
    "config/font.cfg", "config/configtemplates.zip", "config/mapmodelattributes.cfg",
    "packages/misc/notexture.jpg", "packages/misc/icon.png", "docs/license.html",
}
# The RomFS image built from the release is the same for every engine build.
ROMFS_SIZE = 59588032
ROMFS_SHA256 = "91fc1f0f890ad4f3f5abe0971e07ef148845d0961e3234cfb6c78dd8d07e916d"

MAGIC_3DSX = b"3DSX"
EXT_HEADER_SIZE = 44  # base header plus smdhOffset, smdhSize, fsOffset
FS_OFFSET_FIELD = 0x28
ENGINE_SIGNATURE = b"/3ds/assaultcube"
ROMFS_NONE = 0xFFFFFFFF


class BuildError(Exception):
    pass


def align4(value):
    return (value + 3) & ~3


def read_release(archive):
    """Return the game data as nested dicts: name -> bytes, or dict for a folder."""
    size = archive.stat().st_size
    if size != RELEASE_SIZE:
        raise BuildError(f"{archive.name} is {size:,} bytes; {RELEASE_NAME} is {RELEASE_SIZE:,} bytes. "
                         f"Download it again from {DOWNLOAD_PAGE}")
    digest = hashlib.sha256()
    with archive.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    if digest.hexdigest() != RELEASE_SHA256:
        raise BuildError(f"{archive.name} is not the official {RELEASE_NAME} (SHA-256 differs). "
                         f"Download it again from {DOWNLOAD_PAGE}")
    tree = {}
    try:
        with tarfile.open(archive, "r:bz2") as release:
            for member in release:
                path = pathlib.PurePosixPath(member.name)
                if path.is_absolute() or ".." in path.parts:
                    raise BuildError(f"Unsafe archive member: {member.name}")
                if not path.parts or path.parts[0] not in ROOTS:
                    continue
                if not member.isdir() and not member.isfile():
                    raise BuildError(f"Unexpected release member type: {member.name}")
                folder = tree
                for part in path.parts[:-1]:
                    folder = folder.setdefault(part, {})
                    if not isinstance(folder, dict):
                        raise BuildError(f"Conflicting archive member: {member.name}")
                name = path.parts[-1]
                if member.isdir():
                    if not isinstance(folder.setdefault(name, {}), dict):
                        raise BuildError(f"Conflicting archive member: {member.name}")
                else:
                    if name in folder:
                        raise BuildError(f"Duplicate archive member: {member.name}")
                    with release.extractfile(member) as original:
                        folder[name] = original.read()
    except (tarfile.TarError, EOFError, OSError) as error:
        raise BuildError(f"Could not read {archive.name}: {error}")
    missing = sorted(path for path in REQUIRED if not isinstance(lookup(tree, path), bytes))
    if missing:
        raise BuildError("Release is missing: " + ", ".join(missing))
    return tree


def lookup(tree, path):
    node = tree
    for part in path.split("/"):
        if not isinstance(node, dict) or part not in node:
            return None
        node = node[part]
    return node


def name_units(name):
    return name.encode("utf-16-le")


def sort_key(name):
    # UTF-16 code unit order, which is also how JavaScript compares strings.
    return name.encode("utf-16-be")


def table_length(count):
    if count < 3:
        return 3
    if count < 19:
        return count | 1
    while any(count % prime == 0 for prime in (2, 3, 5, 7, 11, 13, 17)):
        count += 1
    return count


def name_hash(parent, name, total):
    value = parent ^ 123456789
    for (unit,) in struct.iter_unpack("<H", name):
        value = ((value >> 5) | (value << 27)) & 0xFFFFFFFF
        value ^= unit
    return value % total


def build_romfs(tree, order=None):
    """Lay the tree out as the RomFS image 3dsxtool writes and libctru mounts.

    Entries are visited in sorted name order, files and folders together, and
    names starting with a dot are left out, as 3dsxtool does. `order` replaces
    the sort; the test uses it to follow the host's directory listing.
    """
    order = order or (lambda path, names: sorted(names, key=sort_key))
    root = dict(offset=0, parent=None, sibling=None, first_dir=None, first_file=None, name=b"")
    root["parent"] = root
    dirs, files = [root], []
    dir_size, file_size, data_size = 0x18, 0, 0

    def scan(entry, node, path):
        nonlocal dir_size, file_size, data_size
        for name in order(path, list(node)):
            if name.startswith("."):
                continue
            child = node[name]
            units = name_units(name)
            if isinstance(child, dict):
                item = dict(offset=dir_size, parent=entry, sibling=entry["first_dir"], first_dir=None,
                            first_file=None, name=units)
                dir_size = align4(dir_size + 0x18 + len(units))
                entry["first_dir"] = item
                dirs.append(item)
                scan(item, child, path + (name,))
            else:
                item = dict(offset=file_size, parent=entry, sibling=entry["first_file"], name=units,
                            data_offset=data_size, data=child)
                file_size = align4(file_size + 0x20 + len(units))
                data_size = align4(data_size + len(child))
                entry["first_file"] = item
                files.append(item)

    scan(root, tree, ())

    def chain(items):
        table = [ROMFS_NONE] * table_length(len(items))
        for item in items:
            slot = name_hash(item["parent"]["offset"], item["name"], len(table))
            item["next_hash"] = table[slot]
            table[slot] = item["offset"]
        return table

    dir_table, file_table = chain(dirs), chain(files)

    def offset(item):
        return item["offset"] if item else ROMFS_NONE

    header = [0x28]
    position = 0x28
    for size in (len(dir_table) * 4, dir_size, len(file_table) * 4, file_size):
        header += [position, size]
        position += size
    header.append(position)
    out = bytearray(position + data_size)
    struct.pack_into("<10I", out, 0, *header)
    position = 0x28
    struct.pack_into(f"<{len(dir_table)}I", out, position, *dir_table)
    position += len(dir_table) * 4
    for item in dirs:
        struct.pack_into("<6I", out, position + item["offset"], offset(item["parent"]), offset(item["sibling"]),
                         offset(item["first_dir"]), offset(item["first_file"]), item["next_hash"], len(item["name"]))
        out[position + item["offset"] + 0x18:position + item["offset"] + 0x18 + len(item["name"])] = item["name"]
    position += dir_size
    struct.pack_into(f"<{len(file_table)}I", out, position, *file_table)
    position += len(file_table) * 4
    for item in files:
        struct.pack_into("<2I2Q2I", out, position + item["offset"], offset(item["parent"]), offset(item["sibling"]),
                         item["data_offset"], len(item["data"]), item["next_hash"], len(item["name"]))
        out[position + item["offset"] + 0x20:position + item["offset"] + 0x20 + len(item["name"])] = item["name"]
    position += file_size
    for item in files:
        start = position + item["data_offset"]
        out[start:start + len(item["data"])] = item["data"]
    return bytes(out)


def check_engine(engine):
    """Return the offset the RomFS image goes at, after validating the 3DSX."""
    if len(engine) < 32 or engine[:4] != MAGIC_3DSX:
        raise BuildError("The engine file is not a 3DSX program. Use assaultcube.3dsx from the AC-3DS release.")
    header_size = struct.unpack_from("<H", engine, 4)[0]
    if header_size < EXT_HEADER_SIZE or len(engine) < EXT_HEADER_SIZE:
        raise BuildError("The engine 3DSX has no extended header, so game data cannot be attached to it.")
    smdh_offset, smdh_size, fs_offset = struct.unpack_from("<3I", engine, 0x20)
    if fs_offset != 0:
        raise BuildError("This assaultcube.3dsx already has game data inside. "
                         "Use the small engine-only assaultcube.3dsx from the AC-3DS release.")
    end = smdh_offset + smdh_size
    if smdh_offset < header_size or not end <= len(engine) <= align4(end):
        raise BuildError("The engine 3DSX is damaged or incomplete. Download the AC-3DS release again.")
    if ENGINE_SIGNATURE not in engine:
        raise BuildError("The engine file is a 3DSX program, but not AC-3DS. "
                         "Use assaultcube.3dsx from the AC-3DS release.")
    return align4(end)


def attach(engine, romfs):
    fs_offset = check_engine(engine)
    out = bytearray(engine) + bytes(fs_offset - len(engine)) + romfs
    struct.pack_into("<I", out, FS_OFFSET_FIELD, fs_offset)
    return bytes(out)


def write_tree(tree, destination):
    """Write the loose files stage-assets.py would, for an unconverted tree."""
    if destination.exists():
        raise BuildError(f"Will not write into an existing path: {destination}")

    def write(node, folder):
        folder.mkdir(parents=True)
        for name, child in node.items():
            if isinstance(child, dict):
                write(child, folder / name)
            else:
                (folder / name).write_bytes(child)

    write(tree, destination)
    (destination / ".assaultcube-assets").write_text(RELEASE_SHA256 + "\n", encoding="ascii")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("archive", type=pathlib.Path, help=RELEASE_NAME)
    parser.add_argument("engine", type=pathlib.Path, help="engine-only assaultcube.3dsx from the AC-3DS release")
    parser.add_argument("output", type=pathlib.Path, help="assaultcube.3dsx to write, with the game data inside")
    parser.add_argument("--stage-dir", type=pathlib.Path,
                        help="also write the game files as loose folders (must not exist yet)")
    args = parser.parse_args()
    try:
        output = args.output.absolute()
        for source, what in ((args.engine, "engine file"), (args.archive, "game archive")):
            if not source.is_file():
                raise BuildError(f"Could not read {source}: no such file")
            if output.exists() and output.samefile(source):
                raise BuildError(f"The output would overwrite the {what}. Choose another output path.")
        if output.is_dir():
            raise BuildError(f"{output} is a folder. Give the name of the file to write.")
        try:
            engine = args.engine.read_bytes()
        except OSError as error:
            raise BuildError(f"Could not read {args.engine}: {error}")
        check_engine(engine)
        tree = read_release(args.archive)
        romfs = build_romfs(tree)
        if len(romfs) != ROMFS_SIZE or hashlib.sha256(romfs).hexdigest() != ROMFS_SHA256:
            raise BuildError("The game data image did not come out as expected. Nothing was written.")
        if args.stage_dir:
            write_tree(tree, args.stage_dir.absolute())
        result = attach(engine, romfs)
        output.parent.mkdir(parents=True, exist_ok=True)
        partial = output.with_name(f".{output.name}.{os.getpid()}.partial")
        try:
            partial.write_bytes(result)
            os.replace(partial, output)
        except OSError as error:
            raise BuildError(f"Could not write {output}: {error}")
        finally:
            partial.unlink(missing_ok=True)
    except BuildError as error:
        raise SystemExit(f"make-3dsx: {error}")
    print(f"make-3dsx: wrote {output} ({len(result):,} bytes, SHA-256 {hashlib.sha256(result).hexdigest()})")
    print("make-3dsx: copy it to /3ds/assaultcube/assaultcube.3dsx on the SD card. Do not share this file.")


if __name__ == "__main__":
    main()
