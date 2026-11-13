#!/usr/bin/env python3
"""Stage the user-supplied stock Lockdown Edition files without executing them."""
import argparse
import hashlib
import pathlib
import shutil
import tarfile
import tempfile

RELEASE_SHA256 = "9b3118e7929253f0114fa06cad8310faab646d177b4660406011152f34086b55"
ROOTS = {"config", "packages", "bot", "mods", "demos", "docs", "screenshots"}
REQUIRED = {
    "config/font.cfg", "config/configtemplates.zip", "config/mapmodelattributes.cfg",
    "packages/misc/notexture.jpg", "packages/misc/icon.png", "docs/license.html",
}


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as archive:
        for chunk in iter(lambda: archive.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def stage(archive, destination):
    if sha256(archive) != RELEASE_SHA256:
        raise SystemExit("Archive SHA-256 does not match the supplied Lockdown Edition RC1.")
    destination.parent.mkdir(parents=True, exist_ok=True)
    if destination.is_symlink():
        raise SystemExit(f"Will not replace a symlink: {destination}")
    if destination.exists() and not (destination / ".assaultcube-assets").is_file():
        # Some generators create the directory for an output marker before
        # invoking the staging command. Accept it only if completely empty.
        if not destination.is_dir() or any(destination.iterdir()):
            raise SystemExit(f"Will not replace an unmanaged asset directory: {destination}")
    with tempfile.TemporaryDirectory(prefix=".ac-stage-", dir=destination.parent) as temp:
        staged = pathlib.Path(temp) / "romfs"
        staged.mkdir()
        with tarfile.open(archive, "r:bz2") as release:
            for member in release:
                path = pathlib.PurePosixPath(member.name)
                if path.is_absolute() or ".." in path.parts:
                    raise SystemExit(f"Unsafe archive member: {member.name}")
                if not path.parts or path.parts[0] not in ROOTS:
                    continue
                out = staged.joinpath(*path.parts)
                if member.isdir():
                    out.mkdir(parents=True, exist_ok=True)
                elif member.isfile():
                    out.parent.mkdir(parents=True, exist_ok=True)
                    with release.extractfile(member) as original, out.open("xb") as target:
                        shutil.copyfileobj(original, target)
                else:
                    raise SystemExit(f"Unexpected release member type: {member.name}")
        missing = sorted(path for path in REQUIRED if not (staged / path).is_file())
        if missing:
            raise SystemExit("Release is missing: " + ", ".join(missing))
        (staged / ".assaultcube-assets").write_text(RELEASE_SHA256 + "\n", encoding="ascii")
        backup = pathlib.Path(temp) / "previous"
        if destination.exists():
            destination.rename(backup)
        try:
            staged.rename(destination)
        except OSError:
            if backup.exists():
                backup.rename(destination)
            raise


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("archive", type=pathlib.Path)
    parser.add_argument("destination", type=pathlib.Path)
    args = parser.parse_args()
    stage(args.archive, args.destination.absolute())
