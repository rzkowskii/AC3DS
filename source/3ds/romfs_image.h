#pragma once

#include <stddef.h>
#include <map>
#include <string>
#include <vector>

#include "firstrun_core.h"

// The game data as one file on the SD card: a RomFS image that libctru mounts
// with romfsMountFromFile, so the game reads romfs:/... exactly as it does
// from an asset-embedded .3dsx. The image is written front to back while the
// release is unpacked:
//
//   0x00  RomFS header (offsets and sizes of the four tables and of the data)
//   0x30  file data, each file padded to 4 bytes, in archive order
//         directory hash table, directory table, file hash table, file table
//         trailer (below)
//
// libctru finds the tables through the header, so they can follow the data;
// it reads the header and the four tables into memory at mount and nothing
// else. The header is zero until everything after it is written, and the
// trailer carries checksums of the header and the tables: libctru checks
// nothing itself, and a table it cannot trust would make it read outside its
// memory. No engine headers, so the same file builds and is tested on a PC.

namespace romfsimage
{
    extern const char file_name[];  // in the game folder
    extern const char part_name[];  // the same file while it is being written

    constexpr unsigned header_size = 0x28;
    constexpr unsigned data_start = 0x30;
    constexpr unsigned trailer_size = 72;
    constexpr unsigned max_tables = 8u << 20; // the release needs about 170 KB

    struct Info
    {
        unsigned image_size;
        unsigned dir_hash_size, dir_table_size, file_hash_size, file_table_size; // what a mount keeps in memory
        unsigned files, directories;
        unsigned long long data_bytes;
        unsigned char source_sha256[32]; // of the archive the image was made from
    };

    // Random access to an image, for check().
    struct Source
    {
        virtual ~Source() {}
        virtual long long size() = 0;
        virtual bool read(long long offset, void *out, size_t size) = 0;
    };

    // Decides whether an image can be handed to libctru: the length, header
    // and trailer agree, the tables are the ones that were written, every
    // file's data lies inside the image and each path in required names a
    // file. Reads the header, the trailer and the tables; not the file data.
    // On failure problem says why.
    bool check(Source &source, const char *const *required, int required_count, Info &info, char *problem, size_t problem_size);

    // Where a Writer puts the image.
    struct Output
    {
        virtual ~Output() {}
        // The next bytes of the file, in order.
        virtual bool append(const unsigned char *data, size_t size) = 0;
        // Replaces the first bytes of the file, once, after the last append.
        virtual bool rewrite_start(const unsigned char *data, size_t size) = 0;
    };

    // Takes what the tar reader hands over and lays it out as an image. File
    // data goes to the output through one large buffer; names and offsets are
    // kept in memory until finish() writes the tables.
    class Writer : public firstrun::Sink
    {
        struct Dir { int parent, sibling, first_dir, first_file; unsigned offset; std::string name; };
        struct File { int parent, sibling; unsigned offset; unsigned long long data_offset, size; std::string name; };

        Output &output;
        std::vector<Dir> dirs;
        std::vector<File> files;
        std::map<std::string, int> dir_index, file_index; // by path
        std::vector<unsigned char> buffer;
        size_t buffered = 0;
        unsigned long long position = 0, left = 0;
        unsigned dir_table_size, file_table_size = 0;
        bool in_file = false, output_refused = false, finished = false;
        char problem_text[160] = "";

        bool fail(const char *what, const char *path = NULL);
        bool put(const unsigned char *data, size_t size);
        bool flush();
        int folder(const std::string &path);

    public:
        static constexpr size_t buffer_size = 256 * 1024;

        explicit Writer(Output &output);

        bool directory(const char *path) override;
        bool begin_file(const char *path, long long size) override;
        bool write(const unsigned char *data, size_t size) override;
        bool end_file() override;

        // After the last file: checks that each path in required is a file in
        // the image, then writes the tables, the trailer and the header.
        bool finish(const char *const *required, int required_count, const unsigned char source_sha256[32], Info &info);
        // Why a call returned false, "" when the output refused the bytes.
        const char *error() const { return problem_text; }
        bool output_failed() const { return output_refused; }
        int file_count() const { return int(files.size()); }
    };
}
