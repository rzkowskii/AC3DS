// Host test for source/3ds/firstrun_core.cpp. Built and driven by
// scripts/qa/firstrun-test.py; not part of the 3DS build.
//
//   firstrun-core-test selftest
//   firstrun-core-test image ARCHIVE IMAGE CHUNK SIZE SHA256
//   firstrun-core-test check IMAGE [--none | REQUIRED-FILE...]

#include "firstrun_core.h"
#include "romfs_image.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <sys/stat.h>

using namespace firstrun;

namespace
{
    int failures = 0;

    void check(bool ok, const char *what)
    {
        if(ok) return;
        ++failures;
        fprintf(stderr, "FAILED: %s\n", what);
    }

    std::string sha_of(const std::string &text, size_t step)
    {
        Sha256 sha;
        for(size_t at = 0; at < text.size(); at += step)
            sha.update((const unsigned char *)text.data() + at, text.size() - at < step ? text.size() - at : step);
        unsigned char digest[32];
        char hex[65];
        sha.finish(digest);
        to_hex(digest, 32, hex);
        return hex;
    }

    PathKind kind(const char *name, std::string *out = NULL)
    {
        char path[max_path + 1] = "";
        const PathKind result = classify_path(name, path, sizeof(path));
        if(out) *out = path;
        return result;
    }

    int selftest()
    {
        check(sha_of("", 1) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "sha256 of nothing");
        check(sha_of("abc", 1) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "sha256 abc");
        check(sha_of("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 7) ==
              "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1", "sha256 two blocks");
        check(sha_of(std::string(1000000, 'a'), 4093) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0", "sha256 million a");
        for(size_t n = 54; n < 67; ++n) check(sha_of(std::string(n, 'x'), 64) == sha_of(std::string(n, 'x'), 3), "sha256 padding edge");

        static Url url, next;
        check(parse_url(release_url, url) && !strcmp(url.host, "github.com") && url.port == 443 && github_url(url), "release url");
        check(!strncmp(url.path, "/assaultcube/AC/releases/download/v1.3.0.2/", 43), "release path");
        check(!parse_url("http://github.com/x", url), "plain http refused");
        check(!parse_url("https://user@github.com/x", url), "userinfo refused");
        check(!parse_url("https://github.com\\@evil.example/x", url), "backslash refused");
        check(!parse_url("https://github.com:99999/x", url), "bad port refused");
        check(!parse_url("https:///x", url), "empty host refused");
        check(parse_url("https://Release-Assets.GitHubUserContent.com/a?b=c#frag", url) && github_url(url) &&
              !strcmp(url.host, "release-assets.githubusercontent.com") && !strcmp(url.path, "/a?b=c"), "asset host");
        check(parse_url("https://githubusercontent.com.evil.example/a", url) && !github_url(url), "lookalike host");
        check(parse_url("https://evilgithubusercontent.com/a", url) && !github_url(url), "suffix without dot");
        check(parse_url("https://github.com:444/a", url) && !github_url(url), "other port is not GitHub");
        check(parse_url("https://github.com/a", url) && parse_url("/b/c", next, &url) && !strcmp(next.host, "github.com") &&
              !strcmp(next.path, "/b/c"), "relative redirect");
        check(!parse_url("//evil.example/x", next, &url), "scheme-relative redirect refused");
        check(!parse_url("https://github.com/a b", url), "space in path refused");

        static Head head;
        const char ok[] = "HTTP/1.1 200 OK\r\nContent-Length: 52071427\r\nContent-Type: application/octet-stream\r\n\r\nBODY";
        size_t end = head_end(ok, sizeof(ok) - 1);
        check(end == sizeof(ok) - 5 && parse_head(ok, end, head) && head.status == 200 && head.length == 52071427 && !head.chunked, "200 head");
        const char moved[] = "HTTP/1.1 302 Found\r\nLocation: https://release-assets.githubusercontent.com/x?y=1\r\nContent-Length: 0\r\n\r\n";
        end = head_end(moved, sizeof(moved) - 1);
        check(end && parse_head(moved, end, head) && head.status == 302 &&
              !strcmp(head.location, "https://release-assets.githubusercontent.com/x?y=1"), "302 head");
        const char twice[] = "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nContent-Length: 6\r\n\r\n";
        check(!parse_head(twice, sizeof(twice) - 1, head), "conflicting lengths refused");
        const char packed[] = "HTTP/1.1 200 OK\r\nContent-Encoding: gzip\r\n\r\n";
        check(!parse_head(packed, sizeof(packed) - 1, head), "content-encoding refused");
        const char chunked[] = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n";
        check(parse_head(chunked, sizeof(chunked) - 1, head) && head.chunked, "chunked flagged");
        check(!head_end("HTTP/1.1 200 OK\r\nContent-Le", 27), "incomplete head");
        check(!parse_head("HTTP/2 200\r\n\r\n", 14, head) && !parse_head("garbage", 7, head), "bad status line");

        std::string out;
        check(kind("./config/font.cfg", &out) == PATH_TAKE && out == "config/font.cfg", "leading ./ dropped");
        check(kind("packages//maps/./official/ac_desert.cgz", &out) == PATH_TAKE && out == "packages/maps/official/ac_desert.cgz", "normalised");
        check(kind("./docs/", &out) == PATH_TAKE && out == "docs", "directory name");
        check(kind("./") == PATH_SKIP && kind("") == PATH_SKIP && kind(".") == PATH_SKIP, "top folder skipped");
        check(kind("./source/src/main.cpp") == PATH_SKIP && kind("bin_unix/x") == PATH_SKIP && kind("README.md") == PATH_SKIP, "other folders skipped");
        check(kind("configs/x") == PATH_SKIP && kind("Config/x") == PATH_SKIP && kind("conf") == PATH_SKIP, "near-miss roots skipped");
        check(kind("/config/x") == PATH_UNSAFE && kind("/etc/passwd") == PATH_UNSAFE, "absolute refused");
        check(kind("config/../../x") == PATH_UNSAFE && kind("../config/x") == PATH_UNSAFE && kind("source/../x") == PATH_UNSAFE &&
              kind("config/..") == PATH_UNSAFE, ".. refused anywhere");
        check(kind("config/..x") == PATH_TAKE && kind("config/x..y") == PATH_TAKE, "dots inside a name are fine");
        check(kind("config/a\\b") == PATH_UNSAFE && kind("config/a:b") == PATH_UNSAFE && kind("config/a\tb") == PATH_UNSAFE &&
              kind("config/caf\xc3\xa9") == PATH_UNSAFE && kind("config/a.") == PATH_UNSAFE && kind("config/a ") == PATH_UNSAFE &&
              kind("config/ a") == PATH_UNSAFE && kind("packages/what?.jpg") == PATH_UNSAFE, "names FAT cannot hold refused");
        check(kind(("config/" + std::string(max_name, 'a')).c_str()) == PATH_TAKE &&
              kind(("config/" + std::string(max_name + 1, 'a')).c_str()) == PATH_UNSAFE, "component length bound");
        std::string deep = "config";
        while(deep.size() + 40 <= size_t(max_path)) deep += "/" + std::string(39, 'a');
        check(kind(deep.c_str()) == PATH_TAKE && kind((deep + "/" + std::string(39, 'a')).c_str()) == PATH_UNSAFE, "path length bound");
        std::string nested = "config";
        for(int i = 1; i < 16; ++i) nested += "/a";
        check(kind(nested.c_str()) == PATH_TAKE && kind((nested + "/a").c_str()) == PATH_UNSAFE, "folder depth bound");

        printf(failures ? "selftest: %d FAILED\n" : "selftest: ok\n", failures);
        return failures ? 1 : 0;
    }

    // The image file on a host disk, written the way the console writes it:
    // under another name until it is complete.
    struct HostOutput final : romfsimage::Output
    {
        FILE *f = NULL;
        int appends = 0;
        size_t largest = 0;

        bool append(const unsigned char *data, size_t size) override
        {
            ++appends;
            if(size > largest) largest = size;
            return fwrite(data, 1, size, f) == size;
        }

        bool rewrite_start(const unsigned char *data, size_t size) override
        {
            return fseek(f, 0, SEEK_SET) == 0 && fwrite(data, 1, size, f) == size;
        }
    };

    struct HostSource final : romfsimage::Source
    {
        FILE *f = NULL;

        long long size() override
        {
            struct stat info;
            return fstat(fileno(f), &info) == 0 ? (long long)info.st_size : -1;
        }

        bool read(long long offset, void *out, size_t size) override
        {
            return fseek(f, (long)offset, SEEK_SET) == 0 && fread(out, 1, size, f) == size;
        }
    };

    void print_info(const romfsimage::Info &info)
    {
        char hex[65];
        to_hex(info.source_sha256, 32, hex);
        printf(" image=%u files=%u directories=%u data=%llu dirhash=%u dirtable=%u filehash=%u filetable=%u source=%s", info.image_size,
               info.files, info.directories, info.data_bytes, info.dir_hash_size, info.dir_table_size, info.file_hash_size,
               info.file_table_size, hex);
    }

    int image(const char *archive, const char *out, size_t chunk, long long size, const char *sha)
    {
        FILE *f = fopen(archive, "rb");
        if(!f) { perror(archive); return 2; }
        const std::string part = std::string(out) + ".part";
        HostOutput output;
        output.f = fopen(part.c_str(), "wb");
        if(!output.f) { perror(part.c_str()); return 2; }
        romfsimage::Writer writer(output);
        Unpacker unpacker(writer);
        unsigned char *buffer = (unsigned char *)malloc(chunk);
        bool ok = !unpacker.error();
        for(size_t got; ok && (got = fread(buffer, 1, chunk, f)) > 0;) ok = unpacker.feed(buffer, got);
        if(ok) ok = unpacker.finish(size, sha);
        fclose(f);
        free(buffer);
        romfsimage::Info info;
        const bool unpacked = ok;
        // The self-made test archives do not hold the game's own files.
        if(ok) ok = writer.finish(required_files, strcmp(sha, release_sha256) ? 0 : required_count, unpacker.digest(), info);
        if(fclose(output.f) != 0) ok = false;
        if(ok) ok = rename(part.c_str(), out) == 0;
        else remove(part.c_str());
        printf("%s files=%d directories=%d bytes=%lld in=%lld sink_stopped=%d appends=%d largest=%zu", ok ? "OK" : "ERROR",
               unpacker.archive().file_count(), unpacker.archive().directory_count(), unpacker.archive().bytes_written(),
               unpacker.bytes_in(), unpacker.sink_stopped(), output.appends, output.largest);
        if(ok) print_info(info);
        else printf(" error=%s%s%s", !unpacked && unpacker.error() ? unpacker.error() : "", writer.error()[0] ? " / " : "", writer.error());
        printf("\n");
        return ok ? 0 : 1;
    }

    // With names: each must be a file in the image (--none: no such demand).
    // Without: the game's own.
    int check_image(int count, char **names)
    {
        HostSource source;
        source.f = fopen(names[0], "rb");
        if(!source.f) { perror(names[0]); return 2; }
        romfsimage::Info info;
        char problem[160] = "";
        const bool none = count == 2 && !strcmp(names[1], "--none");
        const bool ok = count > 1 ? romfsimage::check(source, names + 1, none ? 0 : count - 1, info, problem, sizeof(problem))
                                  : romfsimage::check(source, required_files, required_count, info, problem, sizeof(problem));
        fclose(source.f);
        printf(ok ? "OK" : "ERROR error=%s", problem);
        if(ok) print_info(info);
        printf("\n");
        return ok ? 0 : 1;
    }
}

int main(int argc, char **argv)
{
    if(argc == 2 && !strcmp(argv[1], "selftest")) return selftest();
    if(argc == 7 && !strcmp(argv[1], "image")) return image(argv[2], argv[3], size_t(atol(argv[4])), atoll(argv[5]), argv[6]);
    if(argc >= 3 && !strcmp(argv[1], "check")) return check_image(argc - 2, argv + 2);
    fprintf(stderr, "usage: %s selftest | image ARCHIVE IMAGE CHUNK SIZE SHA256 | check IMAGE [--none | REQUIRED-FILE...]\n", argv[0]);
    return 64;
}
