#pragma once

#include <stddef.h>

// The parts of the first-run download that need neither the console nor a
// TLS library: the pinned release, addresses and HTTP response heads, SHA-256,
// the tar reader with the staging rules of scripts/stage-assets.py, and the
// bzip2 -> tar pipeline. What the tar reader takes goes to a Sink; the one in
// romfs_image.h lays it out as a single image file. No engine headers, so the
// same file builds and is tested on a PC.

namespace firstrun
{
    // The official AssaultCube 1.3.0.2 package. scripts/stage-assets.py pins
    // the same file.
    extern const char release_url[];
    extern const char release_sha256[]; // lower-case hex
    constexpr long long release_size = 52071427;
    // The image made from the pinned release, for the free-space estimate.
    constexpr long long release_image_bytes = 59588112;

    constexpr int max_url = 2048;          // a release asset redirect is about 1000 characters
    constexpr int max_path = 200;          // inside the game folder
    constexpr int max_name = 100;          // one path component
    constexpr int max_files = 8000;
    constexpr long long max_tar_bytes = 160LL << 20; // the release is 75 MB unpacked

    struct Url
    {
        char host[128];
        int port;
        char path[max_url]; // starts with '/', query included, fragment dropped
    };

    // "https://host[:port][/path]" only. With a base, also "/path" on the
    // base's host. Plain http and everything else is refused.
    bool parse_url(const char *text, Url &out, const Url *base = NULL);
    // github.com or a githubusercontent.com host, on port 443.
    bool github_url(const Url &url);

    struct Head
    {
        int status;
        long long length; // Content-Length, -1 when absent
        bool chunked;
        char location[max_url];
    };

    // Offset just past the blank line that ends a response head, 0 if it has
    // not arrived yet.
    size_t head_end(const char *data, size_t size);
    bool parse_head(const char *head, size_t size, Head &out);

    class Sha256
    {
        unsigned int state[8];
        unsigned long long count;
        unsigned char block[64];
        void transform(const unsigned char *data);

    public:
        Sha256();
        void update(const unsigned char *data, size_t size);
        void finish(unsigned char out[32]); // once
    };

    void to_hex(const unsigned char *bytes, size_t count, char *out); // out holds 2 * count + 1

    // One of the top-level folders the game uses.
    extern const char *const roots[];
    extern const int root_count;
    // Files that must exist before an unpacked release is taken as complete.
    extern const char *const required_files[];
    extern const int required_count;

    enum PathKind { PATH_TAKE, PATH_SKIP, PATH_UNSAFE };
    // Applies the staging rules to an archive member name. "." components and
    // repeated slashes are dropped, so the "./" the release puts in front of
    // every name goes. PATH_UNSAFE: absolute, a ".." component, or a name the
    // SD card's file system cannot hold. PATH_SKIP: not under one of the
    // game's folders. PATH_TAKE: out holds the relative path.
    PathKind classify_path(const char *name, char *out, size_t out_size);

    // Receives what the tar reader takes from the archive. Paths are relative,
    // '/'-separated and already checked. False from any call stops the reader.
    struct Sink
    {
        virtual ~Sink() {}
        virtual bool directory(const char *path) = 0;
        virtual bool begin_file(const char *path, long long size) = 0;
        virtual bool write(const unsigned char *data, size_t size) = 0;
        virtual bool end_file() = 0;
        // Called between blocks of work. False cancels.
        virtual bool keep_going() { return true; }
    };

    // A streaming tar reader: ustar and GNU headers, GNU long names and pax
    // path/size records. Only regular files and directories under the game's
    // folders reach the sink; any other member type there is an error.
    class TarReader
    {
        enum { HEADER, DATA, SKIP, PADDING, LONG_NAME, PAX, TRAILER, FAILED };
        Sink &sink;
        int state = HEADER;
        unsigned char header[512];
        size_t header_have = 0;
        long long left = 0, padding = 0, total = 0;
        char *meta = NULL;       // long name or pax data being collected
        size_t meta_have = 0, meta_size = 0;
        bool pax_global = false, have_long = false, have_pax_path = false, have_pax_size = false;
        char long_name[1024], pax_path[1024];
        long long pax_size = 0;
        int files = 0, directories = 0;
        long long file_bytes = 0;
        const char *problem = NULL;
        char problem_text[160];
        bool stopped = false;

        bool fail(const char *what, const char *name = NULL);
        bool take_header();
        bool finish_meta();
        void after_data();

    public:
        explicit TarReader(Sink &sink) : sink(sink) {}
        ~TarReader();
        TarReader(const TarReader &) = delete;
        TarReader &operator=(const TarReader &) = delete;

        bool feed(const unsigned char *data, size_t size);
        // The end-of-archive block has been read and nothing but padding followed.
        bool complete() const { return state == TRAILER; }
        const char *error() const { return problem; }
        // True when the sink refused, as opposed to the archive being wrong.
        bool sink_stopped() const { return stopped; }
        int file_count() const { return files; }
        int directory_count() const { return directories; }
        long long bytes_written() const { return file_bytes; }
    };

    // Compressed release bytes in; files out. Hashes what it is fed. hash()
    // and decode() share no state, so one thread may hash the bytes as they
    // arrive while another decodes them in the same order; feed() is both.
    class Unpacker
    {
        struct Bz;
        Bz *bz = NULL;
        Sha256 sha;
        TarReader tar;
        Sink &sink;
        long long received = 0;
        bool stream_end = false;
        const char *problem = NULL;
        char problem_text[160];

    public:
        explicit Unpacker(Sink &sink);
        ~Unpacker();
        Unpacker(const Unpacker &) = delete;
        Unpacker &operator=(const Unpacker &) = delete;

        void hash(const unsigned char *data, size_t size);
        bool decode(const unsigned char *data, size_t size);
        bool feed(const unsigned char *data, size_t size)
        {
            hash(data, size);
            return decode(data, size);
        }
        // After the last byte, with no decode() running: the whole archive arrived, it is size bytes
        // long, its SHA-256 is the one given and the tar inside ended properly.
        bool finish(long long size, const char *sha256_hex);
        const char *error() const { return problem; }
        // True when the failure was the sink's (or a cancel), not the data's.
        bool sink_stopped() const { return stopped; }
        long long bytes_in() const { return received; }
        const TarReader &archive() const { return tar; }
        // The SHA-256 finish() computed.
        const unsigned char *digest() const { return sha_digest; }

    private:
        bool stopped = false;
        unsigned char sha_digest[32] = {};
    };
}
