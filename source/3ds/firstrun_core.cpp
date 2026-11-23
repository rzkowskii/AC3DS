// firstrun_core.cpp: the portable half of the first-run download (see firstrun_core.h)

#include "firstrun_core.h"

#include <bzlib.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

namespace firstrun
{
    const char release_url[] =
        "https://github.com/assaultcube/AC/releases/download/v1.3.0.2/AssaultCube_v1.3.0.2_LockdownEdition_RC1.tar.bz2";
    const char release_sha256[] = "9b3118e7929253f0114fa06cad8310faab646d177b4660406011152f34086b55";

    // The same folders and files as ROOTS and REQUIRED in scripts/stage-assets.py.
    const char *const roots[] = { "config", "packages", "bot", "mods", "demos", "docs", "screenshots" };
    const int root_count = int(sizeof(roots) / sizeof(*roots));
    const char *const required_files[] =
    {
        "config/font.cfg", "config/configtemplates.zip", "config/mapmodelattributes.cfg",
        "packages/misc/notexture.jpg", "packages/misc/icon.png", "docs/license.html"
    };
    const int required_count = int(sizeof(required_files) / sizeof(*required_files));

    namespace
    {
        bool same_name(const char *text, size_t length, const char *name)
        {
            return strlen(name) == length && !strncasecmp(text, name, length);
        }
    }

    bool parse_url(const char *text, Url &out, const Url *base)
    {
        if(!text) return false;
        const char *path = text;
        if(!strncasecmp(text, "https://", 8))
        {
            const char *p = text + 8;
            size_t n = 0;
            for(; isalnum((unsigned char)*p) || *p == '.' || *p == '-'; ++p)
            {
                if(n + 1 >= sizeof(out.host)) return false;
                out.host[n++] = (char)tolower((unsigned char)*p);
            }
            out.host[n] = '\0';
            if(!n || !isalnum((unsigned char)out.host[0]) || !isalnum((unsigned char)out.host[n - 1])) return false;
            out.port = 443;
            if(*p == ':')
            {
                int port = 0, digits = 0;
                for(++p; *p >= '0' && *p <= '9'; ++p)
                {
                    if(++digits > 5) return false;
                    port = port * 10 + (*p - '0');
                }
                if(!digits || port < 1 || port > 65535) return false;
                out.port = port;
            }
            // Anything else here (user@host, a backslash) is not a host we parsed.
            if(*p && *p != '/' && *p != '?' && *p != '#') return false;
            path = p;
        }
        else if(base && text[0] == '/' && text[1] != '/')
        {
            if(&out != base)
            {
                memcpy(out.host, base->host, sizeof(out.host));
                out.port = base->port;
            }
        }
        else return false;

        char buffer[max_url];
        size_t n = 0;
        if(*path != '/') buffer[n++] = '/';
        for(; *path && *path != '#'; ++path)
        {
            const unsigned char c = (unsigned char)*path;
            if(c <= ' ' || c >= 127 || n + 1 >= sizeof(buffer)) return false;
            buffer[n++] = (char)c;
        }
        buffer[n] = '\0';
        memcpy(out.path, buffer, n + 1);
        return true;
    }

    bool github_url(const Url &url)
    {
        if(url.port != 443) return false;
        if(!strcmp(url.host, "github.com")) return true;
        static const char suffix[] = ".githubusercontent.com";
        const size_t length = strlen(url.host), tail = sizeof(suffix) - 1;
        return length > tail && !strcmp(url.host + length - tail, suffix);
    }

    size_t head_end(const char *data, size_t size)
    {
        for(size_t i = 0; i < size; ++i)
        {
            if(data[i] != '\n') continue;
            if(i + 1 < size && data[i + 1] == '\n') return i + 2;
            if(i + 2 < size && data[i + 1] == '\r' && data[i + 2] == '\n') return i + 3;
        }
        return 0;
    }

    bool parse_head(const char *head, size_t size, Head &out)
    {
        out.status = 0;
        out.length = -1;
        out.chunked = false;
        out.location[0] = '\0';
        if(size < 13 || memcmp(head, "HTTP/1.", 7) || !isdigit((unsigned char)head[7]) || head[8] != ' ') return false;
        for(int i = 9; i < 12; ++i)
        {
            if(!isdigit((unsigned char)head[i])) return false;
            out.status = out.status * 10 + (head[i] - '0');
        }
        if(head[12] != ' ' && head[12] != '\r' && head[12] != '\n') return false;

        const char *end = head + size;
        const char *line = (const char *)memchr(head, '\n', size);
        while(line && ++line < end)
        {
            const char *next = (const char *)memchr(line, '\n', end - line);
            const char *stop = next ? next : end;
            if(stop > line && stop[-1] == '\r') --stop;
            if(stop == line) break;
            const char *colon = (const char *)memchr(line, ':', stop - line);
            if(colon)
            {
                const char *value = colon + 1;
                while(value < stop && (*value == ' ' || *value == '\t')) ++value;
                while(stop > value && (stop[-1] == ' ' || stop[-1] == '\t')) --stop;
                const size_t name = colon - line, length = stop - value;
                if(same_name(line, name, "content-length"))
                {
                    long long bytes = 0;
                    if(!length || length > 18) return false;
                    for(size_t i = 0; i < length; ++i)
                    {
                        if(value[i] < '0' || value[i] > '9') return false;
                        bytes = bytes * 10 + (value[i] - '0');
                    }
                    if(out.length >= 0 && out.length != bytes) return false;
                    out.length = bytes;
                }
                else if(same_name(line, name, "transfer-encoding"))
                {
                    if(same_name(value, length, "chunked")) out.chunked = true;
                    else if(!same_name(value, length, "identity")) return false;
                }
                else if(same_name(line, name, "content-encoding"))
                {
                    // The request asks for the bytes as stored.
                    if(!same_name(value, length, "identity")) return false;
                }
                else if(same_name(line, name, "location"))
                {
                    if(length >= sizeof(out.location)) return false;
                    memcpy(out.location, value, length);
                    out.location[length] = '\0';
                }
            }
            line = next;
        }
        return true;
    }

    namespace
    {
        const unsigned int sha_k[64] =
        {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
            0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
            0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
            0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
            0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
            0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
            0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
        };

        inline unsigned int rotr(unsigned int x, int n) { return x >> n | x << (32 - n); }
    }

    Sha256::Sha256() : count(0)
    {
        static const unsigned int initial[8] =
        {
            0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
        };
        memcpy(state, initial, sizeof(state));
    }

    void Sha256::transform(const unsigned char *data)
    {
        unsigned int w[64], s[8];
        for(int i = 0; i < 16; ++i)
            w[i] = (unsigned int)data[i * 4] << 24 | data[i * 4 + 1] << 16 | data[i * 4 + 2] << 8 | data[i * 4 + 3];
        for(int i = 16; i < 64; ++i)
        {
            const unsigned int a = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ w[i - 15] >> 3;
            const unsigned int b = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ w[i - 2] >> 10;
            w[i] = w[i - 16] + a + w[i - 7] + b;
        }
        memcpy(s, state, sizeof(s));
        // The eight working variables rotate one place a round; index them
        // from a moving origin instead of copying them down.
        for(int i = 0; i < 64; ++i)
        {
            unsigned int &a = s[(0 - i) & 7], &b = s[(1 - i) & 7], &c = s[(2 - i) & 7], &d = s[(3 - i) & 7];
            unsigned int &e = s[(4 - i) & 7], &f = s[(5 - i) & 7], &g = s[(6 - i) & 7], &h = s[(7 - i) & 7];
            const unsigned int t1 = h + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + sha_k[i] + w[i];
            const unsigned int t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            d += t1;
            h = t1 + t2;
        }
        for(int i = 0; i < 8; ++i) state[i] += s[i];
    }

    void Sha256::update(const unsigned char *data, size_t size)
    {
        size_t have = size_t(count % 64);
        count += size;
        if(have)
        {
            const size_t take = size < 64 - have ? size : 64 - have;
            memcpy(block + have, data, take);
            data += take;
            size -= take;
            if(have + take < 64) return;
            transform(block);
        }
        for(; size >= 64; data += 64, size -= 64) transform(data);
        memcpy(block, data, size);
    }

    void Sha256::finish(unsigned char out[32])
    {
        const unsigned long long bits = count * 8;
        unsigned char pad[72] = { 0x80 };
        const size_t have = size_t(count % 64), fill = (have < 56 ? 56 : 120) - have;
        for(int i = 0; i < 8; ++i) pad[fill + i] = (unsigned char)(bits >> (56 - 8 * i));
        update(pad, fill + 8);
        for(int i = 0; i < 32; ++i) out[i] = (unsigned char)(state[i / 4] >> (24 - 8 * (i % 4)));
    }

    void to_hex(const unsigned char *bytes, size_t count, char *out)
    {
        static const char digits[] = "0123456789abcdef";
        for(size_t i = 0; i < count; ++i)
        {
            *out++ = digits[bytes[i] >> 4];
            *out++ = digits[bytes[i] & 15];
        }
        *out = '\0';
    }

    PathKind classify_path(const char *name, char *out, size_t out_size)
    {
        if(name[0] == '/') return PATH_UNSAFE;
        // The whole name is looked at before any of it is used.
        for(const char *p = name; *p;)
        {
            const char *end = strchr(p, '/');
            const size_t length = end ? size_t(end - p) : strlen(p);
            if(length == 2 && p[0] == '.' && p[1] == '.') return PATH_UNSAFE;
            p += length + (end ? 1 : 0);
        }
        size_t n = 0;
        int depth = 0;
        bool first = true, wanted = false;
        for(const char *p = name; *p;)
        {
            const char *end = strchr(p, '/');
            const size_t length = end ? size_t(end - p) : strlen(p);
            if(length && !(length == 1 && p[0] == '.'))
            {
                // The release goes six folders deep.
                if(++depth > 16) return PATH_UNSAFE;
                if(first)
                {
                    for(int i = 0; i < root_count; ++i)
                        if(strlen(roots[i]) == length && !memcmp(roots[i], p, length)) wanted = true;
                    if(!wanted) return PATH_SKIP;
                    first = false;
                }
                // FAT keeps neither these characters nor a trailing dot or space.
                if(length > size_t(max_name) || p[0] == ' ' || p[length - 1] == ' ' || p[length - 1] == '.') return PATH_UNSAFE;
                for(size_t i = 0; i < length; ++i)
                {
                    const unsigned char c = (unsigned char)p[i];
                    if(c < 0x20 || c > 0x7e || strchr("\\:*?\"<>|", c)) return PATH_UNSAFE;
                }
                if(n + length + 2 > out_size || n + length + 1 > size_t(max_path)) return PATH_UNSAFE;
                if(n) out[n++] = '/';
                memcpy(out + n, p, length);
                n += length;
            }
            p += length + (end ? 1 : 0);
        }
        if(first) return PATH_SKIP; // "", "." or "./": the archive's own top folder
        out[n] = '\0';
        return PATH_TAKE;
    }

    namespace
    {
        constexpr size_t max_pax = 64 * 1024;

        // A tar number: octal text, or GNU's binary form for large values.
        bool tar_number(const unsigned char *field, size_t width, long long &value)
        {
            value = 0;
            if(field[0] & 0x80)
            {
                if(field[0] != 0x80) return false; // negative, or too large to matter here
                for(size_t i = 1; i < width; ++i)
                {
                    if(value >> 55) return false;
                    value = value << 8 | field[i];
                }
                return true;
            }
            size_t i = 0;
            while(i < width && field[i] == ' ') ++i;
            for(; i < width && field[i] >= '0' && field[i] <= '7'; ++i)
            {
                if(value >> 59) return false;
                value = value * 8 + (field[i] - '0');
            }
            for(; i < width; ++i) if(field[i] != ' ' && field[i] != '\0') return false;
            return true;
        }

        bool tar_checksum(const unsigned char *header)
        {
            long long stored = 0;
            if(!tar_number(header + 148, 8, stored)) return false;
            long long plain = 0, with_sign = 0;
            for(int i = 0; i < 512; ++i)
            {
                const unsigned char c = i >= 148 && i < 156 ? ' ' : header[i];
                plain += c;
                with_sign += (signed char)c; // some old writers summed signed bytes
            }
            return stored == plain || stored == with_sign;
        }
    }

    TarReader::~TarReader() { free(meta); }

    bool TarReader::fail(const char *what, const char *name)
    {
        if(state == FAILED) return false;
        state = FAILED;
        if(name)
        {
            // The name comes from the archive: keep what is printable.
            char shown[64];
            size_t n = 0;
            for(; name[n] && n + 1 < sizeof(shown); ++n)
                shown[n] = (unsigned char)name[n] >= 0x20 && (unsigned char)name[n] < 0x7f ? name[n] : '?';
            shown[n] = '\0';
            snprintf(problem_text, sizeof(problem_text), "%s: %s", what, shown);
        }
        else snprintf(problem_text, sizeof(problem_text), "%s", what);
        problem = problem_text;
        return false;
    }

    void TarReader::after_data()
    {
        state = padding ? PADDING : HEADER;
    }

    bool TarReader::finish_meta()
    {
        if(state == LONG_NAME)
        {
            // meta_size was checked against long_name when the header was read.
            memcpy(long_name, meta, meta_size);
            long_name[meta_size] = '\0';
            if(strlen(long_name) != meta_size && strlen(long_name) + 1 != meta_size) return fail("bad long name record");
            have_long = true;
        }
        else if(!pax_global)
        {
            // Records of "<length> <key>=<value>\n", the length covering the whole record.
            for(size_t at = 0; at < meta_size;)
            {
                size_t length = 0, digits = 0;
                while(at + digits < meta_size && meta[at + digits] >= '0' && meta[at + digits] <= '9' && digits < 9)
                    length = length * 10 + (meta[at + digits++] - '0');
                if(!digits || at + digits >= meta_size || meta[at + digits] != ' ' || length <= digits + 1 ||
                   length > meta_size - at || meta[at + length - 1] != '\n') return fail("bad pax header");
                const char *record = meta + at + digits + 1, *stop = meta + at + length - 1;
                const char *equals = (const char *)memchr(record, '=', stop - record);
                if(!equals) return fail("bad pax header");
                const size_t key = equals - record, count = stop - equals - 1;
                if(key == 4 && !memcmp(record, "path", 4))
                {
                    if(count >= sizeof(pax_path) || memchr(equals + 1, '\0', count)) return fail("bad pax path");
                    memcpy(pax_path, equals + 1, count);
                    pax_path[count] = '\0';
                    have_pax_path = true;
                }
                else if(key == 4 && !memcmp(record, "size", 4))
                {
                    if(!count || count > 15) return fail("bad pax size");
                    pax_size = 0;
                    for(size_t i = 0; i < count; ++i)
                    {
                        if(equals[1 + i] < '0' || equals[1 + i] > '9') return fail("bad pax size");
                        pax_size = pax_size * 10 + (equals[1 + i] - '0');
                    }
                    have_pax_size = true;
                }
                at += length;
            }
        }
        free(meta);
        meta = NULL;
        meta_have = meta_size = 0;
        after_data();
        return true;
    }

    bool TarReader::take_header()
    {
        bool blank = true;
        for(int i = 0; i < 512 && blank; ++i) blank = !header[i];
        if(blank)
        {
            if(have_long || have_pax_path || have_pax_size) return fail("archive ends after a name record");
            state = TRAILER;
            return true;
        }
        if(!tar_checksum(header)) return fail("bad tar header checksum");
        const char type = (char)header[156];
        long long size = 0;
        if(!tar_number(header + 124, 12, size) || size < 0) return fail("bad tar size");
        left = size;
        padding = (512 - size % 512) % 512;

        if(type == 'L' || type == 'x' || type == 'g')
        {
            const bool name_record = type == 'L';
            if(size < 1 || size > (long long)(name_record ? sizeof(long_name) - 1 : max_pax))
                return fail(name_record ? "archive member name too long" : "pax header too large");
            meta = (char *)malloc(size_t(size));
            if(!meta) return fail("out of memory");
            meta_size = size_t(size);
            meta_have = 0;
            pax_global = type == 'g';
            state = name_record ? LONG_NAME : PAX;
            return true;
        }
        if(type == 'K') // the long target of a link; the link itself is judged below
        {
            state = SKIP;
            if(!left) after_data();
            return true;
        }

        char field[257];
        const char *name = field;
        if(have_pax_path) name = pax_path;
        else if(have_long) name = long_name;
        else
        {
            size_t n = 0;
            // The prefix field belongs to POSIX ustar only; GNU tar keeps other things there.
            if(!memcmp(header + 257, "ustar\0" "00", 8) && header[345])
            {
                n = strnlen((const char *)header + 345, 155);
                memcpy(field, header + 345, n);
                field[n++] = '/';
            }
            const size_t length = strnlen((const char *)header, 100);
            memcpy(field + n, header, length);
            field[n + length] = '\0';
        }
        if(have_pax_size)
        {
            left = size = pax_size;
            padding = (512 - size % 512) % 512;
        }
        have_long = have_pax_path = have_pax_size = false;

        const bool directory = type == '5' || (type == '\0' && name[0] && name[strlen(name) - 1] == '/');
        const bool regular = !directory && (type == '0' || type == '\0');
        // Links, devices and FIFOs carry no data blocks whatever their size field says.
        const bool has_data = !(type >= '1' && type <= '6');
        if(!has_data) left = padding = 0;

        char path[max_path + 1];
        switch(classify_path(name, path, sizeof(path)))
        {
            case PATH_UNSAFE: return fail("unsafe archive member", name);
            case PATH_SKIP:
                state = SKIP;
                if(!left) after_data();
                return true;
            default: break;
        }
        if(directory)
        {
            if(left) return fail("directory with data", name);
            if(++directories > max_files) return fail("too many directories");
            if(!sink.directory(path)) { stopped = true; return fail("stopped"); }
            after_data();
            return true;
        }
        if(!regular) return fail("unexpected archive member type", name);
        if(++files > max_files) return fail("too many files");
        if(size > max_tar_bytes - total) return fail("archive larger than expected", name);
        if(!sink.begin_file(path, size)) { stopped = true; return fail("stopped"); }
        state = DATA;
        if(!left)
        {
            if(!sink.end_file()) { stopped = true; return fail("stopped"); }
            after_data();
        }
        return true;
    }

    bool TarReader::feed(const unsigned char *data, size_t size)
    {
        total += size;
        if(total > max_tar_bytes) return fail("archive larger than expected");
        while(size)
        {
            switch(state)
            {
                case HEADER:
                {
                    const size_t take = size < 512 - header_have ? size : 512 - header_have;
                    memcpy(header + header_have, data, take);
                    header_have += take;
                    data += take;
                    size -= take;
                    if(header_have < 512) break;
                    header_have = 0;
                    if(!take_header()) return false;
                    break;
                }
                case DATA:
                case SKIP:
                {
                    const size_t take = (long long)size < left ? size : size_t(left);
                    if(state == DATA)
                    {
                        if(!sink.write(data, take)) { stopped = true; return fail("stopped"); }
                        file_bytes += take;
                    }
                    left -= take;
                    data += take;
                    size -= take;
                    if(left) break;
                    if(state == DATA && !sink.end_file()) { stopped = true; return fail("stopped"); }
                    after_data();
                    break;
                }
                case LONG_NAME:
                case PAX:
                {
                    const size_t take = size < meta_size - meta_have ? size : meta_size - meta_have;
                    memcpy(meta + meta_have, data, take);
                    meta_have += take;
                    data += take;
                    size -= take;
                    if(meta_have == meta_size && !finish_meta()) return false;
                    break;
                }
                case PADDING:
                {
                    const size_t take = (long long)size < padding ? size : size_t(padding);
                    padding -= take;
                    data += take;
                    size -= take;
                    if(!padding) state = HEADER;
                    break;
                }
                case TRAILER:
                    for(size_t i = 0; i < size; ++i) if(data[i]) return fail("data after the end of the archive");
                    return true;
                default:
                    return false;
            }
        }
        return true;
    }

    struct Unpacker::Bz
    {
        static constexpr size_t out_size = 64 * 1024;
        bz_stream stream;
        unsigned char out[out_size];
    };

    Unpacker::Unpacker(Sink &sink) : tar(sink), sink(sink)
    {
        bz = (Bz *)calloc(1, sizeof(Bz));
        if(bz && BZ2_bzDecompressInit(&bz->stream, 0, 0) != BZ_OK)
        {
            free(bz);
            bz = NULL;
        }
        if(!bz) problem = "out of memory";
    }

    Unpacker::~Unpacker()
    {
        if(bz) BZ2_bzDecompressEnd(&bz->stream);
        free(bz);
    }

    void Unpacker::hash(const unsigned char *data, size_t size)
    {
        sha.update(data, size);
        received += size;
    }

    bool Unpacker::decode(const unsigned char *data, size_t size)
    {
        if(problem) return false;
        if(stream_end && size)
        {
            problem = "data after the end of the compressed archive";
            return false;
        }
        bz->stream.next_in = (char *)data;
        bz->stream.avail_in = (unsigned int)size;
        while(bz->stream.avail_in && !stream_end)
        {
            bz->stream.next_out = (char *)bz->out;
            bz->stream.avail_out = (unsigned int)Bz::out_size;
            const int result = BZ2_bzDecompress(&bz->stream);
            if(result != BZ_OK && result != BZ_STREAM_END)
            {
                snprintf(problem_text, sizeof(problem_text), result == BZ_MEM_ERROR ? "out of memory" : "the archive is damaged (bzip2 %d)", result);
                problem = problem_text;
                return false;
            }
            const size_t produced = Bz::out_size - bz->stream.avail_out;
            if(produced && !tar.feed(bz->out, produced))
            {
                stopped = tar.sink_stopped();
                snprintf(problem_text, sizeof(problem_text), "%s", tar.error() ? tar.error() : "the archive is damaged");
                problem = problem_text;
                return false;
            }
            if(result == BZ_STREAM_END)
            {
                stream_end = true;
                if(bz->stream.avail_in)
                {
                    problem = "data after the end of the compressed archive";
                    return false;
                }
            }
            if(!sink.keep_going())
            {
                stopped = true;
                problem = "stopped";
                return false;
            }
        }
        return true;
    }

    bool Unpacker::finish(long long size, const char *sha256_hex)
    {
        if(problem) return false;
        char hex[65];
        sha.finish(sha_digest);
        to_hex(sha_digest, sizeof(sha_digest), hex);
        if(received != size)
            snprintf(problem_text, sizeof(problem_text), "%lld bytes arrived, the release is %lld", received, size);
        else if(strcmp(hex, sha256_hex))
            snprintf(problem_text, sizeof(problem_text), "SHA-256 mismatch: got %.16s..., the release is %.16s...", hex, sha256_hex);
        else if(!stream_end || !tar.complete())
            snprintf(problem_text, sizeof(problem_text), "the archive ends early");
        else return true;
        problem = problem_text;
        return false;
    }
}
