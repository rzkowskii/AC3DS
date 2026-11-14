#!/usr/bin/env python3
"""Stage the user-supplied stock Lockdown Edition files without executing them.

Progressive JPEG textures are losslessly transcoded to baseline with jpegtran
when it is available, because stb_image (the decoder SDL_image uses on the
3DS) decodes baseline roughly twice as fast. A file is only replaced when
stb_image decodes the original and the transcode to identical pixels.
"""
import argparse
import hashlib
import os
import pathlib
import shutil
import subprocess
import sys
import tarfile
import tempfile

RELEASE_SHA256 = "9b3118e7929253f0114fa06cad8310faab646d177b4660406011152f34086b55"
# Bump when the post-extraction processing changes, so staged dirs are redone.
STAGING_VERSION = "+baseline-jpeg-v1"
ROOTS = {"config", "packages", "bot", "mods", "demos", "docs", "screenshots"}
REQUIRED = {
    "config/font.cfg", "config/configtemplates.zip", "config/mapmodelattributes.cfg",
    "packages/misc/notexture.jpg", "packages/misc/icon.png", "docs/license.html",
}
# Only image directories. packages/maps is never touched: servers check map CRCs.
TRANSCODE_DIRS = ("packages/textures", "packages/models", "packages/misc")

# Host helper built against SDL_image's bundled stb_image.h with the same
# configuration IMG_stb.c uses (minus SIMD, which the ARM11 build lacks).
STB_HELPER_SOURCE = r"""
/* SDL_image's copy of stb_image.h expects SDL types and helpers. */
#include <limits.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
typedef uint8_t Uint8;
typedef uint16_t Uint16;
typedef int16_t Sint16;
typedef uint32_t Uint32;
typedef int32_t Sint32;
#define IMG_SetError(...) ((void)0)
#define SDL_HasNEON() 0
#define STB_IMAGE_STATIC
#define STBI_NO_THREAD_LOCALS
#define STBI_FAILURE_USERMSG
#define STBI_NO_SIMD
#define STBI_NO_STDIO
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_GIF
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_ASSERT(x) ((void)0)
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct { const unsigned char *data; size_t size, pos; } membuf;

static int cb_read(void *user, char *out, int size) {
    membuf *m = (membuf *)user;
    size_t left = m->size - m->pos, n = (size_t)size < left ? (size_t)size : left;
    memcpy(out, m->data + m->pos, n);
    m->pos += n;
    return (int)n;
}
static void cb_skip(void *user, int n) {
    membuf *m = (membuf *)user;
    if (n < 0 && (size_t)-n > m->pos) m->pos = 0;
    else if (n > 0 && (size_t)n > m->size - m->pos) m->pos = m->size;
    else m->pos += n;
}
static int cb_eof(void *user) { membuf *m = (membuf *)user; return m->pos >= m->size; }

static unsigned char *slurp(const char *path, size_t *size) {
    FILE *f = fopen(path, "rb");
    unsigned char *data;
    long len;
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) || (len = ftell(f)) < 0 || fseek(f, 0, SEEK_SET)) { fclose(f); return NULL; }
    data = (unsigned char *)malloc(len ? (size_t)len : 1);
    if (data && fread(data, 1, (size_t)len, f) != (size_t)len) { free(data); data = NULL; }
    fclose(f);
    *size = (size_t)len;
    return data;
}

/* Same call as IMG_LoadSTB_RW for non-paletted images: callbacks, STBI_default. */
static stbi_uc *decode(const unsigned char *data, size_t size, int *w, int *h, int *c) {
    stbi_io_callbacks cb = { cb_read, cb_skip, cb_eof };
    membuf m = { data, size, 0 };
    *w = *h = *c = 0;
    return stbi_load_from_callbacks(&cb, &m, w, h, c, STBI_default);
}

int main(int argc, char **argv) {
    if (argc == 4 && !strcmp(argv[1], "compare")) {
        size_t sa, sb;
        unsigned char *a = slurp(argv[2], &sa), *b = slurp(argv[3], &sb);
        int wa, ha, ca, wb, hb, cb;
        stbi_uc *pa, *pb;
        int same;
        if (!a || !b) return 2;
        pa = decode(a, sa, &wa, &ha, &ca);
        pb = decode(b, sb, &wb, &hb, &cb);
        if (!pa || !pb) return 3;
        same = wa == wb && ha == hb && ca == cb &&
               !memcmp(pa, pb, (size_t)wa * (size_t)ha * (size_t)ca);
        return same ? 0 : 1;
    }
    if (argc >= 3 && !strcmp(argv[1], "time")) {
        /* Decode every file once (after loading all into memory); print ns. */
        int i, n = argc - 2, failed = 0;
        unsigned char **bufs = (unsigned char **)calloc((size_t)n, sizeof *bufs);
        size_t *sizes = (size_t *)calloc((size_t)n, sizeof *sizes);
        struct timespec t0, t1;
        for (i = 0; i < n; ++i) if (!(bufs[i] = slurp(argv[i + 2], &sizes[i]))) return 2;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (i = 0; i < n; ++i) {
            int w, h, c;
            stbi_uc *p = decode(bufs[i], sizes[i], &w, &h, &c);
            if (p) stbi_image_free(p); else ++failed;
        }
        clock_gettime(CLOCK_MONOTONIC, &t1);
        printf("%lld %d\n", (long long)(t1.tv_sec - t0.tv_sec) * 1000000000LL + (t1.tv_nsec - t0.tv_nsec), failed);
        return 0;
    }
    fprintf(stderr, "usage: %s compare A B | time FILE...\n", argv[0]);
    return 64;
}
"""


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as archive:
        for chunk in iter(lambda: archive.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def warn(message):
    print(f"stage-assets: warning: {message}", file=sys.stderr)


def find_stb_image(explicit):
    if explicit:
        return explicit if explicit.is_file() else None
    # Standalone use: fall back to a FetchContent checkout in a build dir.
    repo = pathlib.Path(__file__).resolve().parent.parent
    for candidate in sorted(repo.glob("build*/_deps/sdl2_image-src/src/stb_image.h")):
        if candidate.is_file():
            return candidate
    return None


def build_stb_helper(stb_image):
    """Compile (or reuse) the host stb_image helper; return its path or None."""
    compiler = os.environ.get("AC3DS_HOST_CC") or shutil.which("cc") or shutil.which("clang") or shutil.which("gcc")
    if not compiler:
        warn("no host C compiler found; skipping progressive JPEG transcode")
        return None
    key = hashlib.sha256()
    for part in (STB_HELPER_SOURCE.encode(), stb_image.read_bytes(), compiler.encode()):
        key.update(hashlib.sha256(part).digest())
    cache = pathlib.Path(tempfile.gettempdir()) / f"ac3ds-stb-helper-{key.hexdigest()[:16]}"
    helper = cache / "stb-helper"
    if helper.is_file() and os.access(helper, os.X_OK):
        return helper
    cache.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".build-", dir=cache) as work:
        work = pathlib.Path(work)
        (work / "stb-helper.c").write_text(STB_HELPER_SOURCE, encoding="ascii")
        result = subprocess.run(
            [compiler, "-O2", "-std=c99", "-D_POSIX_C_SOURCE=199309L", "-w",
             "-I", str(stb_image.parent), str(work / "stb-helper.c"), "-o", str(work / "stb-helper"), "-lm"],
            capture_output=True, text=True)
        if result.returncode != 0:
            warn("could not compile stb_image helper; skipping progressive JPEG transcode\n" + result.stderr[-2000:])
            return None
        os.replace(work / "stb-helper", helper)
    return helper


def jpeg_frame_type(path):
    """Return the SOFn marker byte of a JPEG, or None if not a parseable JPEG."""
    with path.open("rb") as handle:
        data = handle.read()
    if data[:2] != b"\xff\xd8":
        return None
    pos = 2
    while pos + 4 <= len(data):
        if data[pos] != 0xFF:
            return None
        while pos < len(data) and data[pos] == 0xFF:
            pos += 1
        if pos >= len(data):
            return None
        marker = data[pos]
        pos += 1
        if marker == 0x01 or 0xD0 <= marker <= 0xD7:
            continue
        if marker in (0xD9, 0xDA):  # EOI or SOS before any frame header
            return None
        if 0xC0 <= marker <= 0xCF and marker not in (0xC4, 0xC8, 0xCC):
            return marker
        if pos + 2 > len(data):
            return None
        pos += int.from_bytes(data[pos:pos + 2], "big")
    return None


def transcode_progressive_jpegs(staged, jpegtran, helper):
    stats = dict(examined=0, progressive=0, transcoded=0, rejected=0, failed=0, before=0, after=0)
    for root in TRANSCODE_DIRS:
        base = staged / root
        if not base.is_dir():
            continue
        for path in sorted(base.rglob("*")):
            if path.is_symlink() or not path.is_file():
                continue
            frame = jpeg_frame_type(path)
            if frame is None:
                continue
            stats["examined"] += 1
            if frame != 0xC2:
                continue
            stats["progressive"] += 1
            candidate = path.with_name(path.name + ".baseline-tmp")
            try:
                with candidate.open("xb") as out:
                    ok = subprocess.run([jpegtran, "-copy", "all", "-optimize", str(path)],
                                        stdout=out, stderr=subprocess.DEVNULL, timeout=60).returncode == 0
                ok = ok and jpeg_frame_type(candidate) not in (None, 0xC2)
                if not ok:
                    stats["failed"] += 1
                    continue
                verdict = subprocess.run([str(helper), "compare", str(path), str(candidate)],
                                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=60).returncode
                if verdict != 0:
                    stats["rejected" if verdict == 1 else "failed"] += 1
                    continue
                stats["before"] += path.stat().st_size
                stats["after"] += candidate.stat().st_size
                os.replace(candidate, path)
                stats["transcoded"] += 1
            except subprocess.TimeoutExpired:
                stats["failed"] += 1
            finally:
                candidate.unlink(missing_ok=True)
    print("stage-assets: baseline JPEG transcode: {examined} JPEGs examined, {progressive} progressive, "
          "{transcoded} transcoded, {rejected} rejected (pixel mismatch), {failed} failed; "
          "transcoded bytes {before} -> {after}".format(**stats))
    return stats


def stage(archive, destination, stb_image=None):
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
        marker = RELEASE_SHA256
        jpegtran = shutil.which("jpegtran")
        stb_header = find_stb_image(stb_image)
        if not jpegtran:
            warn("jpegtran not found; staging progressive JPEGs unchanged (slower texture loads)")
        elif not stb_header:
            warn("SDL_image stb_image.h not found (pass --stb-image); staging progressive JPEGs unchanged")
        else:
            helper = build_stb_helper(stb_header)
            if helper:
                transcode_progressive_jpegs(staged, jpegtran, helper)
                marker += STAGING_VERSION
        (staged / ".assaultcube-assets").write_text(marker + "\n", encoding="ascii")
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
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("archive", type=pathlib.Path)
    parser.add_argument("destination", type=pathlib.Path)
    parser.add_argument("--stb-image", type=pathlib.Path,
                        help="SDL_image's bundled stb_image.h, used to verify JPEG transcodes")
    args = parser.parse_args()
    stage(args.archive, args.destination.absolute(), args.stb_image)
