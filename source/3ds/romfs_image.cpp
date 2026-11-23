// romfs_image.cpp: the game data as one RomFS image file (see romfs_image.h)

#include "romfs_image.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

namespace romfsimage
{
    const char file_name[] = "assets.romfs";
    const char part_name[] = "assets.romfs.part";

    namespace
    {
        constexpr unsigned none = 0xFFFFFFFFu;
        constexpr unsigned dir_entry = 0x18, file_entry = 0x20;
        const char trailer_magic[8] = { 'A', 'C', '3', 'D', 'S', 'I', 'M', 'G' };
        constexpr unsigned trailer_version = 1;

        inline unsigned align4(unsigned value) { return (value + 3) & ~3u; }

        inline unsigned get32(const unsigned char *p) { return p[0] | p[1] << 8 | p[2] << 16 | unsigned(p[3]) << 24; }
        inline unsigned long long get64(const unsigned char *p) { return get32(p) | (unsigned long long)get32(p + 4) << 32; }

        inline void set32(unsigned char *p, unsigned value)
        {
            p[0] = (unsigned char)value;
            p[1] = (unsigned char)(value >> 8);
            p[2] = (unsigned char)(value >> 16);
            p[3] = (unsigned char)(value >> 24);
        }

        inline void set64(unsigned char *p, unsigned long long value)
        {
            set32(p, unsigned(value));
            set32(p + 4, unsigned(value >> 32));
        }

        // The number of hash buckets 3dsxtool would use for this many entries.
        unsigned table_length(unsigned count)
        {
            if(count < 3) return 3;
            if(count < 19) return count | 1;
            static const unsigned primes[] = { 2, 3, 5, 7, 11, 13, 17 };
            for(;; ++count)
            {
                bool divides = false;
                for(unsigned prime : primes) if(count % prime == 0) divides = true;
                if(!divides) return count;
            }
        }

        // libctru's calcHash over an ASCII name widened to UTF-16.
        unsigned name_hash(unsigned parent, const char *name, size_t length, unsigned total)
        {
            unsigned hash = parent ^ 123456789u;
            for(size_t i = 0; i < length; ++i)
            {
                hash = hash >> 5 | hash << 27;
                hash ^= (unsigned char)name[i];
            }
            return hash % total;
        }

        void put_name(unsigned char *to, const std::string &name)
        {
            for(size_t i = 0; i < name.size(); ++i)
            {
                to[i * 2] = (unsigned char)name[i];
                to[i * 2 + 1] = 0;
            }
        }

        bool same_name(const unsigned char *units, const char *name, size_t length)
        {
            for(size_t i = 0; i < length; ++i) if(units[i * 2] != (unsigned char)name[i] || units[i * 2 + 1]) return false;
            return true;
        }

        // The four tables of an image, read into memory and not yet trusted.
        struct Tables
        {
            const unsigned char *dir_hash, *dir_table, *file_hash, *file_table;
            unsigned dir_buckets, dir_size, file_buckets, file_size;

            // Offset of the entry in table whose parent and name match, by the
            // hash chain libctru follows. Every step is bounds checked.
            unsigned find(bool file, unsigned parent, const char *name, size_t length) const
            {
                const unsigned char *hash = file ? file_hash : dir_hash, *table = file ? file_table : dir_table;
                const unsigned buckets = file ? file_buckets : dir_buckets, size = file ? file_size : dir_size;
                const unsigned fixed = file ? file_entry : dir_entry, next_at = file ? 0x18 : 0x10;
                unsigned at = get32(hash + 4 * name_hash(parent, name, length, buckets));
                for(unsigned steps = 0; at != none; ++steps)
                {
                    if(size < fixed || at > size - fixed || steps > size / fixed) return none;
                    const unsigned char *entry = table + at;
                    const unsigned name_bytes = get32(entry + fixed - 4);
                    if(name_bytes > size - fixed - at) return none;
                    if(get32(entry) == parent && name_bytes == length * 2 && same_name(entry + fixed, name, length)) return at;
                    at = get32(entry + next_at);
                }
                return none;
            }

            bool has_file(const char *path) const
            {
                unsigned parent = 0;
                for(;;)
                {
                    const char *slash = strchr(path, '/');
                    if(!slash) return find(true, parent, path, strlen(path)) != none;
                    parent = find(false, parent, path, size_t(slash - path));
                    if(parent == none) return false;
                    path = slash + 1;
                }
            }
        };
    }

    bool check(Source &source, const char *const *required, int required_count, Info &info, char *problem, size_t problem_size)
    {
        #define REFUSE(...) do { snprintf(problem, problem_size, __VA_ARGS__); return false; } while(0)
        memset(&info, 0, sizeof(info));
        const long long length = source.size();
        if(length < (long long)(data_start + trailer_size) || length > 0x7FFFFFFFLL) REFUSE("the file is %lld bytes long", length);
        unsigned char header[header_size], trailer[trailer_size];
        if(!source.read(0, header, sizeof(header)) || !source.read(length - trailer_size, trailer, sizeof(trailer)))
            REFUSE("the file could not be read");
        // An image cut short has lost its trailer; one never finished has no header.
        if(memcmp(trailer, trailer_magic, sizeof(trailer_magic)) || get32(trailer + 8) != trailer_version)
            REFUSE("the file is incomplete (no end mark)");
        const unsigned tables_offset = get32(trailer + 16), tables_size = get32(trailer + 20);
        if(get32(trailer + 12) != (unsigned)length) REFUSE("the file is %lld bytes long, it was written as %u", length, get32(trailer + 12));
        if(get32(header) != header_size || (unsigned)crc32(0, header, sizeof(header)) != get32(trailer + 28))
            REFUSE("the file's header is damaged");
        unsigned field[10];
        for(int i = 0; i < 10; ++i) field[i] = get32(header + 4 * i);
        // The four tables lie back to back between the data and the trailer.
        if(field[9] != data_start || tables_offset < data_start || tables_size > max_tables ||
           tables_size != (unsigned)length - trailer_size - tables_offset || tables_offset > (unsigned)length - trailer_size ||
           field[1] != tables_offset)
            REFUSE("the file's layout is not one this program writes");
        unsigned long long end = tables_offset;
        for(int i = 0; i < 4; ++i)
        {
            if(field[1 + 2 * i] != end || field[2 + 2 * i] % 4 || field[2 + 2 * i] > max_tables) REFUSE("the file's tables are misplaced");
            end += field[2 + 2 * i];
        }
        if(end != (unsigned long long)tables_offset + tables_size || field[2] < 4 || field[6] < 4 || field[4] < dir_entry)
            REFUSE("the file's tables are misplaced");

        unsigned char *memory = (unsigned char *)malloc(tables_size);
        if(!memory) REFUSE("not enough memory to check the file");
        struct Release { unsigned char *memory; ~Release() { free(memory); } } release = { memory };
        if(!source.read(tables_offset, memory, tables_size)) REFUSE("the file could not be read");
        if((unsigned)crc32(0, memory, tables_size) != get32(trailer + 24)) REFUSE("the file's tables are damaged");

        Tables tables;
        tables.dir_hash = memory;
        tables.dir_table = tables.dir_hash + field[2];
        tables.file_hash = tables.dir_table + field[4];
        tables.file_table = tables.file_hash + field[6];
        tables.dir_buckets = field[2] / 4;
        tables.dir_size = field[4];
        tables.file_buckets = field[6] / 4;
        tables.file_size = field[8];

        const unsigned long long data_size = tables_offset - data_start;
        unsigned files = 0, directories = 0;
        unsigned long long data_bytes = 0;
        for(unsigned at = 0; at < tables.file_size; ++files)
        {
            if(tables.file_size - at < file_entry) REFUSE("the file's tables are damaged");
            const unsigned char *entry = tables.file_table + at;
            const unsigned name_bytes = get32(entry + 0x1C);
            const unsigned long long offset = get64(entry + 8), size = get64(entry + 16);
            if(name_bytes > tables.file_size - at - file_entry || get32(entry) >= tables.dir_size || offset > data_size || size > data_size - offset)
                REFUSE("the file's tables are damaged");
            data_bytes += size;
            at = align4(at + file_entry + name_bytes);
        }
        for(unsigned at = 0; at < tables.dir_size; ++directories)
        {
            if(tables.dir_size - at < dir_entry) REFUSE("the file's tables are damaged");
            const unsigned name_bytes = get32(tables.dir_table + at + 0x14);
            if(name_bytes > tables.dir_size - at - dir_entry) REFUSE("the file's tables are damaged");
            at = align4(at + dir_entry + name_bytes);
        }
        if(files != get32(trailer + 32) || directories != get32(trailer + 36) + 1) REFUSE("the file's tables are damaged");
        for(int i = 0; i < required_count; ++i)
            if(!tables.has_file(required[i])) REFUSE("the file does not hold %s", required[i]);

        info.image_size = (unsigned)length;
        info.dir_hash_size = field[2];
        info.dir_table_size = field[4];
        info.file_hash_size = field[6];
        info.file_table_size = field[8];
        info.files = files;
        info.directories = directories - 1;
        info.data_bytes = data_bytes;
        memcpy(info.source_sha256, trailer + 40, 32);
        return true;
        #undef REFUSE
    }

    Writer::Writer(Output &output) : output(output), buffer(buffer_size), dir_table_size(dir_entry)
    {
        dirs.push_back(Dir{ 0, -1, -1, -1, 0, "" });
        // Where the header goes once the tables' places are known.
        memset(buffer.data(), 0, data_start);
        buffered = position = data_start;
    }

    bool Writer::fail(const char *what, const char *path)
    {
        if(path) snprintf(problem_text, sizeof(problem_text), "%s: %.80s", what, path);
        else snprintf(problem_text, sizeof(problem_text), "%s", what);
        return false;
    }

    bool Writer::flush()
    {
        if(buffered && !output.append(buffer.data(), buffered))
        {
            output_refused = true;
            return false;
        }
        buffered = 0;
        return true;
    }

    bool Writer::put(const unsigned char *data, size_t size)
    {
        position += size;
        while(size)
        {
            const size_t take = size < buffer_size - buffered ? size : buffer_size - buffered;
            memcpy(buffer.data() + buffered, data, take);
            buffered += take;
            data += take;
            size -= take;
            if(buffered == buffer_size && !flush()) return false;
        }
        return true;
    }

    int Writer::folder(const std::string &path)
    {
        if(path.empty()) return 0;
        const auto known = dir_index.find(path);
        if(known != dir_index.end()) return known->second;
        if(file_index.count(path))
        {
            fail("archive member is both a file and a folder", path.c_str());
            return -1;
        }
        const size_t slash = path.rfind('/');
        const int parent = folder(slash == std::string::npos ? std::string() : path.substr(0, slash));
        if(parent < 0) return -1;
        // Folders implied by file paths count too, or names could fill memory.
        if(dirs.size() >= size_t(firstrun::max_files))
        {
            fail("too many directories", path.c_str());
            return -1;
        }
        const int index = int(dirs.size());
        Dir dir = { parent, dirs[parent].first_dir, -1, -1, dir_table_size, path.substr(slash == std::string::npos ? 0 : slash + 1) };
        dir_table_size = align4(dir_table_size + dir_entry + 2 * unsigned(dir.name.size()));
        dirs[parent].first_dir = index;
        dirs.push_back(dir);
        dir_index[path] = index;
        return index;
    }

    bool Writer::directory(const char *path)
    {
        if(in_file || finished) return fail("archive members out of order");
        return folder(path) >= 0;
    }

    bool Writer::begin_file(const char *path, long long size)
    {
        if(in_file || finished || size < 0) return fail("archive members out of order");
        const std::string name = path;
        if(file_index.count(name)) return fail("archive member appears twice", path);
        if(dir_index.count(name)) return fail("archive member is both a file and a folder", path);
        const size_t slash = name.rfind('/');
        const int parent = folder(slash == std::string::npos ? std::string() : name.substr(0, slash));
        if(parent < 0) return false;
        const int index = int(files.size());
        File file = { parent, dirs[parent].first_file, file_table_size, position - data_start, (unsigned long long)size,
                      name.substr(slash == std::string::npos ? 0 : slash + 1) };
        file_table_size = align4(file_table_size + file_entry + 2 * unsigned(file.name.size()));
        dirs[parent].first_file = index;
        files.push_back(file);
        file_index[name] = index;
        left = (unsigned long long)size;
        in_file = true;
        return true;
    }

    bool Writer::write(const unsigned char *data, size_t size)
    {
        if(!in_file || size > left) return fail("archive member longer than it said");
        left -= size;
        return put(data, size);
    }

    bool Writer::end_file()
    {
        if(!in_file || left) return fail("archive member shorter than it said");
        in_file = false;
        static const unsigned char zero[4] = { 0, 0, 0, 0 };
        return put(zero, size_t(align4(unsigned(position)) - unsigned(position)));
    }

    bool Writer::finish(const char *const *required, int required_count, const unsigned char source_sha256[32], Info &info)
    {
        if(in_file || finished) return fail("archive members out of order");
        for(int i = 0; i < required_count; ++i)
            if(!file_index.count(required[i])) return fail("the release is missing a file the game needs", required[i]);
        const unsigned dir_buckets = table_length(unsigned(dirs.size())), file_buckets = table_length(unsigned(files.size()));
        const unsigned long long tables_size = 4ull * dir_buckets + dir_table_size + 4ull * file_buckets + file_table_size;
        if(tables_size > max_tables || position + tables_size + trailer_size > 0x7FFFFFFFull) return fail("the release is too large for one image");

        std::vector<unsigned char> tables(size_t(tables_size), 0);
        unsigned char *dir_hash = tables.data(), *dir_table = dir_hash + 4 * dir_buckets;
        unsigned char *file_hash = dir_table + dir_table_size, *file_table = file_hash + 4 * file_buckets;
        memset(dir_hash, 0xFF, 4 * dir_buckets);
        memset(file_hash, 0xFF, 4 * file_buckets);
        const auto dir_offset = [this](int index) { return index < 0 ? none : dirs[index].offset; };
        const auto file_offset = [this](int index) { return index < 0 ? none : files[index].offset; };
        for(const Dir &dir : dirs)
        {
            unsigned char *entry = dir_table + dir.offset;
            unsigned char *bucket = dir_hash + 4 * name_hash(dirs[dir.parent].offset, dir.name.data(), dir.name.size(), dir_buckets);
            set32(entry, dirs[dir.parent].offset);
            set32(entry + 4, dir_offset(dir.sibling));
            set32(entry + 8, dir_offset(dir.first_dir));
            set32(entry + 12, file_offset(dir.first_file));
            set32(entry + 16, get32(bucket));
            set32(entry + 20, 2 * unsigned(dir.name.size()));
            put_name(entry + dir_entry, dir.name);
            set32(bucket, dir.offset);
        }
        for(const File &file : files)
        {
            unsigned char *entry = file_table + file.offset;
            unsigned char *bucket = file_hash + 4 * name_hash(dirs[file.parent].offset, file.name.data(), file.name.size(), file_buckets);
            set32(entry, dirs[file.parent].offset);
            set32(entry + 4, file_offset(file.sibling));
            set64(entry + 8, file.data_offset);
            set64(entry + 16, file.size);
            set32(entry + 24, get32(bucket));
            set32(entry + 28, 2 * unsigned(file.name.size()));
            put_name(entry + file_entry, file.name);
            set32(bucket, file.offset);
        }

        const unsigned tables_offset = unsigned(position);
        unsigned char header[data_start] = {}, trailer[trailer_size] = {};
        const unsigned sizes[4] = { 4 * dir_buckets, dir_table_size, 4 * file_buckets, file_table_size };
        set32(header, header_size);
        for(unsigned i = 0, at = tables_offset; i < 4; at += sizes[i++])
        {
            set32(header + 4 + 8 * i, at);
            set32(header + 8 + 8 * i, sizes[i]);
        }
        set32(header + 36, data_start);
        memcpy(trailer, trailer_magic, sizeof(trailer_magic));
        set32(trailer + 8, trailer_version);
        set32(trailer + 12, tables_offset + unsigned(tables_size) + trailer_size);
        set32(trailer + 16, tables_offset);
        set32(trailer + 20, unsigned(tables_size));
        set32(trailer + 24, (unsigned)crc32(0, tables.data(), (unsigned)tables.size()));
        set32(trailer + 28, (unsigned)crc32(0, header, header_size));
        set32(trailer + 32, unsigned(files.size()));
        set32(trailer + 36, unsigned(dirs.size()) - 1);
        memcpy(trailer + 40, source_sha256, 32);

        finished = true;
        if(!put(tables.data(), tables.size()) || !put(trailer, sizeof(trailer)) || !flush()) return false;
        if(!output.rewrite_start(header, sizeof(header)))
        {
            output_refused = true;
            return false;
        }
        memset(&info, 0, sizeof(info));
        info.image_size = unsigned(position);
        info.dir_hash_size = sizes[0];
        info.dir_table_size = sizes[1];
        info.file_hash_size = sizes[2];
        info.file_table_size = sizes[3];
        info.files = unsigned(files.size());
        info.directories = unsigned(dirs.size()) - 1;
        info.data_bytes = 0;
        for(const File &file : files) info.data_bytes += file.size;
        memcpy(info.source_sha256, source_sha256, 32);
        return true;
    }
}
