// firstrun_3ds.cpp: the first-run download of the game data (see firstrun_3ds.h)

#include <3ds.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#include <atomic>
#include <new>

#include "firstrun_3ds.h"
#include "firstrun_core.h"
#include "firstrun_roots.h"
#include "https_3ds.h"
#include "romfs_image.h"

#if defined(AC3DS_FIRSTRUN_TEST) && defined(PRODUCTION)
#error "AC3DS_FIRSTRUN_TEST lets a file on the SD card redirect the download; it cannot be part of a production build"
#endif

bool ac3ds_save_bottom_screen(const char *path);

namespace
{
    constexpr int path_size = 320;
    // The download writes one file, romfsimage::part_name, and renames it to
    // romfsimage::file_name when it is complete and checked. Nothing else on
    // the card changes, so there is no other unfinished state to find.
    const char log_name[] = "profile/firstrun.log";
    constexpr long long megabyte = 1000 * 1000;
    constexpr long long space_margin = 8 * megabyte;

    bool present(const char *path)
    {
        struct stat info;
        return stat(path, &info) == 0;
    }

    // A page being put together: Ac3dsTextPage only points at its lines.
    struct Text
    {
        Ac3dsTextPage page;
        char store[Ac3dsTextPage::max_lines][40];

        explicit Text(const char *title, const char *footer = NULL)
        {
            page.title = title;
            page.footer = footer;
        }

        __attribute__((format(printf, 3, 4))) void line(bool bright, const char *format, ...)
        {
            if(page.count >= Ac3dsTextPage::max_lines) return;
            va_list args;
            va_start(args, format);
            vsnprintf(store[page.count], sizeof(store[page.count]), format, args);
            va_end(args);
            if(bright) page.bright |= 1u << page.count;
            page.lines[page.count] = store[page.count];
            ++page.count;
        }

        // A full-width line, kept by reference: 40 columns do not fit store.
        void wide(const char *text)
        {
            if(page.count < Ac3dsTextPage::max_lines) page.lines[page.count++] = text;
        }

        void blank() { line(false, "%s", ""); }

        // A sentence of any length, broken at spaces.
        void wrapped(bool bright, const char *text)
        {
            constexpr size_t columns = sizeof(store[0]) - 1;
            while(*text)
            {
                size_t take = strlen(text);
                if(take > columns)
                {
                    take = columns;
                    while(take > columns / 2 && text[take] != ' ') --take;
                    if(text[take] != ' ') take = columns;
                }
                line(bright, "%.*s", int(take), text);
                text += take;
                while(*text == ' ') ++text;
            }
        }

        void show() const { ac3ds_text_page(page); }
    };

#ifdef AC3DS_FIRSTRUN_TEST
    // Development builds only. sdmc:/3ds/assaultcube/firstrun-test.cfg holds
    // "key=value" lines:
    //   url=https://host[:port]/path   another archive; its host may be contacted
    //   sha256=<64 hex digits>         the hash that archive must have
    //   size=<bytes>                   and its length
    //   auto=accept | quit             answers the offer without a button press
    //   cancel_at=<bytes>              presses B once that much has arrived
    //   worker=0                       unpacks on the main thread, as on a console without a free core
    //   shots=1                        saves each page to profile/firstrun-*.bmp
    // and firstrun-test-root.pem beside it is one more trusted root.
    struct TestConfig
    {
        bool loaded = false, shots = false, worker = true;
        int automatic = 0; // 1 accept, 2 quit
        long long size = -1, cancel_at = -1;
        char url[firstrun::max_url] = "", sha256[65] = "", host[128] = "";
        char *roots = NULL;
        size_t roots_size = 0;

        void load(const char *game)
        {
            char path[path_size];
            snprintf(path, sizeof(path), "%s/firstrun-test.cfg", game);
            FILE *f = fopen(path, "r");
            if(!f) return;
            loaded = true;
            char row[firstrun::max_url + 16];
            while(fgets(row, sizeof(row), f))
            {
                row[strcspn(row, "\r\n")] = '\0';
                char *value = strchr(row, '=');
                if(!value) continue;
                *value++ = '\0';
                if(!strcmp(row, "url")) snprintf(url, sizeof(url), "%s", value);
                else if(!strcmp(row, "sha256")) snprintf(sha256, sizeof(sha256), "%s", value);
                else if(!strcmp(row, "size")) size = atoll(value);
                else if(!strcmp(row, "cancel_at")) cancel_at = atoll(value);
                else if(!strcmp(row, "shots")) shots = atoi(value) != 0;
                else if(!strcmp(row, "worker")) worker = atoi(value) != 0;
                else if(!strcmp(row, "auto")) automatic = !strcmp(value, "accept") ? 1 : 2;
            }
            fclose(f);
            static firstrun::Url parsed;
            if(url[0] && firstrun::parse_url(url, parsed)) snprintf(host, sizeof(host), "%s", parsed.host);

            snprintf(path, sizeof(path), "%s/firstrun-test-root.pem", game);
            if((f = fopen(path, "r")))
            {
                constexpr size_t extra_limit = 16 * 1024;
                const size_t pinned = sizeof(ac3ds_firstrun_roots) - 1;
                roots = static_cast<char *>(malloc(pinned + extra_limit + 1));
                if(roots)
                {
                    memcpy(roots, ac3ds_firstrun_roots, pinned);
                    const size_t extra = fread(roots + pinned, 1, extra_limit, f);
                    roots[pinned + extra] = '\0';
                    roots_size = pinned + extra + 1;
                }
                fclose(f);
            }
        }
    };
#endif

    // Bytes on their way from the main thread, which receives them, to the
    // worker, which unpacks them. One thread puts, one takes.
    struct Pipe
    {
        static constexpr size_t size = 1 << 20;
        unsigned char *data = NULL;
        size_t head = 0, used = 0; // under lock; head is the oldest byte
        LightLock lock;
        LightEvent readable, writable;
        // end: no more bytes will be put. abort: stop without finishing.
        // done: the worker has returned.
        std::atomic<bool> end{false}, abort{false}, done{false};

        bool start()
        {
            if(!data) data = static_cast<unsigned char *>(malloc(size));
            head = used = 0;
            end = abort = done = false;
            LightLock_Init(&lock);
            LightEvent_Init(&readable, RESET_ONESHOT);
            LightEvent_Init(&writable, RESET_ONESHOT);
            return data != NULL;
        }

        // Copies in what fits and returns how much that was.
        size_t put(const unsigned char *from, size_t count)
        {
            LightLock_Lock(&lock);
            const size_t tail = (head + used) % size, take = count < size - used ? count : size - used;
            LightLock_Unlock(&lock);
            // Only this thread moves the tail, and the taker never looks past it.
            const size_t first = take < size - tail ? take : size - tail;
            memcpy(data + tail, from, first);
            memcpy(data, from + first, take - first);
            LightLock_Lock(&lock);
            used += take;
            LightLock_Unlock(&lock);
            if(take) LightEvent_Signal(&readable);
            return take;
        }

        // The oldest bytes, in place: they stay put until taken() frees them.
        size_t peek(const unsigned char *&at, size_t most)
        {
            LightLock_Lock(&lock);
            size_t count = used < size - head ? used : size - head;
            at = data + head;
            LightLock_Unlock(&lock);
            return count < most ? count : most;
        }

        void taken(size_t count)
        {
            LightLock_Lock(&lock);
            head = (head + count) % size;
            used -= count;
            LightLock_Unlock(&lock);
            LightEvent_Signal(&writable);
        }
    };

    class Installer
    {
        enum Outcome { DONE, FAILED, CANCELLED, CLOSING };

        // The image file while it is being written. Every call here is one
        // call to the FS service: 256 KiB at a time, and nothing per game file.
        struct Card final : romfsimage::Output
        {
            int fd = -1, calls = 0, code = 0;
            u64 ticks = 0, longest = 0;
            const char *failed = NULL; // what was being done

            bool all(const unsigned char *data, size_t bytes)
            {
                while(bytes)
                {
                    const ssize_t wrote = ::write(fd, data, bytes);
                    if(wrote <= 0)
                    {
                        code = wrote ? errno : EIO;
                        failed = "writing";
                        return false;
                    }
                    data += wrote;
                    bytes -= size_t(wrote);
                }
                return true;
            }

            bool append(const unsigned char *data, size_t bytes) override
            {
                const u64 began = svcGetSystemTick();
                const bool ok = all(data, bytes);
                const u64 took = svcGetSystemTick() - began;
                ticks += took;
                if(took > longest) longest = took;
                ++calls;
                return ok;
            }

            bool rewrite_start(const unsigned char *data, size_t bytes) override
            {
                if(lseek(fd, 0, SEEK_SET) == 0) return all(data, bytes);
                code = errno;
                failed = "seeking in";
                return false;
            }
        };

        // The image writer, as the unpacker sees it.
        struct Feed final : romfsimage::Writer
        {
            Installer &owner;

            Feed(Installer &owner, romfsimage::Output &output) : romfsimage::Writer(output), owner(owner) {}

            bool end_file() override
            {
                if(!romfsimage::Writer::end_file()) return false;
                ++owner.files;
                return true;
            }

            // On the worker the main thread's wish to stop arrives through the
            // pipe; without one, this is the main thread and serves the system.
            bool keep_going() override { return owner.threaded ? !owner.pipe.abort : owner.service(); }
        };

        char game[path_size], part[path_size], image[path_size];
        char log[8192];
        size_t log_used = 0;
        const char *damaged; // why the image already on the card was refused, or NULL

        // The download in progress.
        Card card;
        Feed *writer = NULL;
        firstrun::Unpacker *unpacker = NULL;
        Pipe pipe;
        Thread thread = NULL;
        bool threaded = false;
        std::atomic<int> files{0};
        bool cancelled = false, closing = false, can_cancel = false;
        const char *title = "", *stage = "", *drawn_stage = NULL;
        char host[128] = "";
        long long done = 0, total = -1;
        u64 drawn = 0;
        // Where the time went, for the log. On the main thread: hashing,
        // waiting for room in the pipe, drawing. On the worker (or the main
        // thread without one): unpacking, which includes the SD card writes
        // that Card counts, and waiting for bytes.
        u64 hash_ticks = 0, wait_ticks = 0, draw_ticks = 0, unpack_ticks = 0, idle_ticks = 0;
        // Why it stopped: a sentence for the player and the detail for the log.
        char why[160] = "", detail[224] = "";
        const char *url = firstrun::release_url, *sha256 = firstrun::release_sha256;
        long long size = firstrun::release_size;
#ifdef AC3DS_FIRSTRUN_TEST
        TestConfig test;
        bool shot_progress = false, answered = false;
#endif

    public:
        Installer(const char *game_dir, const char *damaged) : damaged(damaged)
        {
            snprintf(game, sizeof(game), "%s", game_dir);
            snprintf(part, sizeof(part), "%s/%s", game_dir, romfsimage::part_name);
            snprintf(image, sizeof(image), "%s/%s", game_dir, romfsimage::file_name);
        }

        ~Installer()
        {
            drop(true);
            free(pipe.data);
#ifdef AC3DS_FIRSTRUN_TEST
            free(test.roots);
#endif
        }

        __attribute__((format(printf, 2, 3))) void note(const char *format, ...)
        {
            if(log_used + 2 >= sizeof(log)) return;
            va_list args;
            va_start(args, format);
            const int wrote = vsnprintf(log + log_used, sizeof(log) - log_used - 1, format, args);
            va_end(args);
            if(wrote < 0) return;
            log_used += size_t(wrote) < sizeof(log) - log_used - 1 ? size_t(wrote) : sizeof(log) - log_used - 2;
            log[log_used++] = '\n';
        }

        // The log holds hosts, timings and error text: nothing about the
        // console or its owner.
        void write_log()
        {
            char path[path_size];
            snprintf(path, sizeof(path), "%s/profile", game);
            mkdir(path, 0777);
            snprintf(path, sizeof(path), "%s/%s", game, log_name);
            if(FILE *f = fopen(path, "w"))
            {
                fwrite(log, 1, log_used, f);
                fclose(f);
            }
        }

        Ac3dsFirstRun run()
        {
            note("first-run download, build %d", AC3DS_BUILD_NUMBER);
#ifdef AC3DS_FIRSTRUN_TEST
            test.load(game);
            if(test.loaded)
            {
                if(test.url[0]) url = test.url;
                if(test.sha256[0]) sha256 = test.sha256;
                if(test.size >= 0) size = test.size;
                note("TEST BUILD: firstrun-test.cfg read; url %s", url);
            }
#endif
            if(damaged) note("%s refused: %s", romfsimage::file_name, damaged);
            if(present(part))
            {
                // A download the console did not get to finish or to tidy up.
                const bool removed = unlink(part) == 0;
                note("found an unfinished download; %s", removed ? "removed it" : "it could not be removed");
            }
            if(!offer()) return AC3DS_FIRSTRUN_QUIT;
            for(;;)
            {
                const u64 began = osGetTime();
                const Outcome outcome = download();
                note("%s after %llu s: %s%s%s", outcome == DONE ? "installed" : outcome == CANCELLED ? "cancelled" :
                     outcome == CLOSING ? "closing" : "failed", (unsigned long long)((osGetTime() - began) / 1000),
                     why, detail[0] ? " / " : "", detail);
                if(outcome == DONE) return installed();
                if(outcome == CLOSING) return AC3DS_FIRSTRUN_QUIT;
                if(!failure_page(outcome == CANCELLED ? "Download cancelled" : "Download failed", true))
                    return AC3DS_FIRSTRUN_QUIT;
            }
        }

    private:
        Ac3dsFirstRun installed()
        {
            Text page("Game data installed");
            page.line(true, "The official AssaultCube 1.3.0.2");
            page.line(true, "game data is on the SD card.");
            page.blank();
            page.line(false, "Starting AssaultCube...");
            page.show();
            shot("installed");
            return AC3DS_FIRSTRUN_INSTALLED;
        }

        void shot(const char *name)
        {
#ifdef AC3DS_FIRSTRUN_TEST
            if(!test.shots) return;
            char path[path_size];
            snprintf(path, sizeof(path), "%s/profile", game);
            mkdir(path, 0777);
            snprintf(path, sizeof(path), "%s/profile/firstrun-%s.bmp", game, name);
            ac3ds_save_bottom_screen(path);
#else
            (void)name;
#endif
        }

        // Waits for one of the buttons. 0: the console wants the program closed.
        u32 wait_buttons(const Text &page, u32 wanted, u32 scripted)
        {
            page.show();
#ifdef AC3DS_FIRSTRUN_TEST
            if(test.automatic)
            {
                // One accepted offer at most, so a failing test cannot loop.
                const u32 answer = test.automatic == 1 && !answered && (scripted & KEY_A) ? KEY_A : KEY_B;
                answered = true;
                note("TEST BUILD: answering %s", answer == KEY_A ? "A" : "B");
                return answer;
            }
#else
            (void)scripted;
#endif
            // A button still held from the launcher is not an answer.
            bool armed = false;
            for(unsigned frame = 1;; ++frame)
            {
                if(!aptMainLoop())
                {
                    closing = true;
                    return 0;
                }
                hidScanInput();
                if(!(hidKeysHeld() & wanted)) armed = true;
                else if(armed && (hidKeysDown() & wanted)) return hidKeysDown() & wanted;
                if(frame % 30 == 0) { ac3ds_blank_top_screen(); page.show(); } // also repaints after the HOME menu
                gspWaitForVBlank();
            }
        }

        // The one file takes whole clusters; an image being replaced is still
        // on the card until the new one is complete.
        void space(long long &needed, long long &available) const
        {
            struct statvfs info;
            needed = available = -1;
            if(statvfs(game, &info) != 0 || !info.f_bsize) return;
            const long long cluster = (long long)info.f_bsize;
            needed = (firstrun::release_image_bytes + space_margin + cluster - 1) / cluster * cluster;
            available = (long long)info.f_bfree * cluster;
        }

        bool offer()
        {
            long long needed, available;
            space(needed, available);
            Text page("Game data needed", "A  Download          B / START  Quit");
            if(damaged) page.line(true, "The game data file is damaged.");
            else page.line(true, "No game data was found on the SD card.");
            page.blank();
            page.line(false, "The game data belongs to AssaultCube.");
            page.line(false, "It can be fetched now, unmodified,");
            page.line(false, "from the official AssaultCube 1.3.0.2");
            page.line(false, "release. It is free for non-commercial");
            page.line(false, "use under the AssaultCube licence:");
            page.wide("https://assault.cubers.net/download.html");
            page.line(false, "assault.cubers.net/docs/license.html");
            page.blank();
            page.line(true, "Download: about %lld MB over Wi-Fi.", (size + megabyte / 2) / megabyte);
            if(needed >= 0)
                page.line(available < needed, "SD card: %lld MB needed, %lld MB free.",
                          (needed + megabyte - 1) / megabyte, available / megabyte);
            else page.line(false, "SD card: about 70 MB needed.");
            page.line(false, "Nothing is contacted until you press A");
            page.show();
            shot("offer");
            const bool accepted = wait_buttons(page, KEY_A | KEY_B | KEY_START, KEY_A) & KEY_A;
            note(accepted ? "offer accepted" : "offer declined");
            return accepted;
        }

        // The page after a download that did not finish. True: try again.
        bool failure_page(const char *heading, bool retry, const char *last = "Nothing was installed.")
        {
            Text page(heading, retry ? "A  Try again         B / START  Quit" : "B / START  Quit");
            if(why[0]) page.wrapped(true, why);
            if(detail[0])
            {
                page.blank();
                page.wrapped(false, detail);
            }
            if(page.page.count) page.blank();
            page.line(false, "%s", last);
            page.show();
            shot("stopped");
            write_log(); // now, in case the console is switched off on this page
            return wait_buttons(page, (retry ? u32(KEY_A) : 0u) | KEY_B | KEY_START, 0) & KEY_A;
        }

        void draw_progress()
        {
            const u64 began = svcGetSystemTick();
            Text page(title, can_cancel ? "B  Cancel" : NULL);
            page.line(true, "%s", stage);
            if(host[0]) page.line(false, "Host: %s", host);
            if(total > 0)
            {
                page.blank();
                page.line(false, "%lld.%lld of %lld.%lld MB (%d%%)", done / megabyte, done % megabyte / (megabyte / 10),
                          total / megabyte, total % megabyte / (megabyte / 10), int(done * 100 / total));
                page.line(false, "Files unpacked: %d", int(files));
                page.blank();
                page.line(false, "Leave the console switched on and");
                page.line(false, "close to the Wi-Fi access point.");
                page.page.percent = int(done * 100 / total);
            }
            page.show();
            drawn = osGetTime();
            drawn_stage = stage;
            draw_ticks += svcGetSystemTick() - began;
#ifdef AC3DS_FIRSTRUN_TEST
            if(total > 0 && done > total / 3 && !shot_progress)
            {
                shot_progress = true;
                shot("progress");
            }
#endif
        }

        // Serves the system, the B button and the display. False: stop. Main
        // thread only.
        bool service()
        {
            if(closing) return false;
            if(!aptMainLoop())
            {
                closing = true;
                return false;
            }
            hidScanInput();
            if(can_cancel && ((hidKeysDown() | hidKeysHeld()) & KEY_B)) cancelled = true;
#ifdef AC3DS_FIRSTRUN_TEST
            if(can_cancel && test.cancel_at >= 0 && done >= test.cancel_at) cancelled = true;
#endif
            if(stage != drawn_stage || osGetTime() - drawn >= 100) draw_progress();
            return !cancelled;
        }

        void sd_failed(const char *what, int code)
        {
            if(code == ENOSPC) snprintf(why, sizeof(why), "The SD card is full.");
            else snprintf(why, sizeof(why), "Could not write to the SD card.");
            snprintf(detail, sizeof(detail), "%s %s: error %d", what, romfsimage::part_name, code);
        }

        // The worker: takes the release's bytes from the pipe in the order
        // they arrived and unpacks them into the image. It touches the
        // unpacker, the writer and the file, and the main thread leaves those
        // alone until it has joined this thread.
        void work()
        {
            bool ok = true;
            while(ok && !pipe.abort)
            {
                const bool last = pipe.end; // read before looking, so nothing put before it is missed
                const unsigned char *at;
                const size_t count = pipe.peek(at, 64 * 1024);
                if(!count)
                {
                    if(last) break;
                    const u64 began = svcGetSystemTick();
                    LightEvent_WaitTimeout(&pipe.readable, 50 * 1000 * 1000LL);
                    idle_ticks += svcGetSystemTick() - began;
                    continue;
                }
                const u64 began = svcGetSystemTick();
                ok = unpacker->decode(at, count);
                unpack_ticks += svcGetSystemTick() - began;
                pipe.taken(count);
            }
            pipe.done = true;
            LightEvent_Signal(&pipe.writable);
        }

        static void work_entry(void *user) { static_cast<Installer *>(user)->work(); }

        // Ends the worker and waits for it: after the last byte with finish
        // set, letting it unpack what is still in the pipe; otherwise at once.
        void join(bool finish)
        {
            if(!thread) return;
            if(finish) pipe.end = true;
            else pipe.abort = true;
            LightEvent_Signal(&pipe.readable);
            while(!pipe.done)
            {
                if(!service() && !pipe.abort)
                {
                    pipe.abort = true;
                    LightEvent_Signal(&pipe.readable);
                }
                LightEvent_WaitTimeout(&pipe.writable, 20 * 1000 * 1000LL);
            }
            threadJoin(thread, U64_MAX);
            threadFree(thread);
            thread = NULL;
            threaded = false;
        }

        bool body(const unsigned char *data, size_t bytes)
        {
            u64 began = svcGetSystemTick();
            unpacker->hash(data, bytes);
            hash_ticks += svcGetSystemTick() - began;
            if(!threaded)
            {
                began = svcGetSystemTick();
                const bool ok = unpacker->decode(data, bytes);
                unpack_ticks += svcGetSystemTick() - began;
                return ok;
            }
            for(;;)
            {
                if(pipe.done) return false; // it stopped by itself and knows why
                const size_t put = pipe.put(data, bytes);
                data += put;
                bytes -= put;
                if(!bytes) return true;
                began = svcGetSystemTick();
                LightEvent_WaitTimeout(&pipe.writable, 20 * 1000 * 1000LL);
                wait_ticks += svcGetSystemTick() - began;
                if(!service()) return false;
            }
        }

        static bool on_body(void *user, const unsigned char *data, size_t bytes)
        {
            return static_cast<Installer *>(user)->body(data, bytes);
        }

        static bool on_progress(void *user, const char *stage, const char *host, long long done, long long total)
        {
            Installer *self = static_cast<Installer *>(user);
            self->stage = stage;
            snprintf(self->host, sizeof(self->host), "%s", host);
            self->done = done;
            self->total = total;
            return !self->service() || (self->threaded && self->pipe.done);
        }

        static void on_log(void *user, const char *line) { static_cast<Installer *>(user)->note("%s", line); }

        void explain(const Ac3dsHttpsResult &result)
        {
            snprintf(detail, sizeof(detail), "%s", result.detail);
            const char *text = "The connection failed.";
            switch(result.error)
            {
                case AC3DS_HTTPS_NO_NETWORK: text = "The console's network service is not available."; break;
                case AC3DS_HTTPS_NO_RANDOM: text = "The system's random number service is not available, so no secure connection can be made."; break;
                case AC3DS_HTTPS_LOOKUP:
                case AC3DS_HTTPS_CONNECT: text = "Could not reach the download server. Check the internet connection."; break;
                case AC3DS_HTTPS_CERT_DATE: text = "The server's certificate is not valid at the console's date. Check the date and time in System Settings."; break;
                case AC3DS_HTTPS_CERT_UNTRUSTED: text = "The server's identity could not be verified, so nothing was downloaded."; break;
                case AC3DS_HTTPS_BAD_URL:
                case AC3DS_HTTPS_REDIRECT: text = "The download server pointed to an address that is not accepted."; break;
                case AC3DS_HTTPS_STATUS: text = "The download server did not provide the file."; break;
                case AC3DS_HTTPS_LENGTH: text = "The file offered is not the official release."; break;
                case AC3DS_HTTPS_TIMEOUT: text = "The connection stalled."; break;
                case AC3DS_HTTPS_PROTOCOL: text = "The connection was cut off."; break;
                case AC3DS_HTTPS_MEMORY: text = "Not enough memory."; break;
                default: break;
            }
            snprintf(why, sizeof(why), "%s", text);
        }

        // Ends an attempt: the worker, the unpacker and, unless the image was
        // completed and renamed, the unfinished file. That file is the only
        // thing a download leaves on the card, so removing it is all the
        // tidying there is.
        void drop(bool remove_part)
        {
            join(false);
            delete unpacker;
            unpacker = NULL;
            delete writer;
            writer = NULL;
            if(card.fd >= 0)
            {
                close(card.fd);
                card.fd = -1;
                if(remove_part) unlink(part);
            }
        }

        Outcome stopped()
        {
            drop(true);
            return closing ? CLOSING : cancelled ? CANCELLED : FAILED;
        }

        Outcome download()
        {
            cancelled = false;
            why[0] = detail[0] = host[0] = '\0';
            files = 0;
            hash_ticks = wait_ticks = draw_ticks = unpack_ticks = idle_ticks = 0;
            card = Card();
            done = 0;
            total = -1;
            title = "Downloading game data";
            stage = "Getting ready";
            draw_progress();

            long long needed, available;
            space(needed, available);
            note("SD card: %lld bytes needed, %lld free", needed, available);
            if(needed >= 0 && available < needed)
            {
                snprintf(why, sizeof(why), "Not enough free space on the SD card: %lld MB needed, %lld MB free.",
                         (needed + megabyte - 1) / megabyte, available / megabyte);
                return FAILED;
            }
            // Asked first so that a console with wireless off gets a plain answer.
            if(R_SUCCEEDED(acInit()))
            {
                u32 wifi = 0;
                const Result asked = ACU_GetWifiStatus(&wifi);
                acExit();
                note("Wi-Fi status: %08lX, %lu", (unsigned long)asked, (unsigned long)wifi);
                if(R_SUCCEEDED(asked) && !wifi)
                {
                    snprintf(why, sizeof(why), "The console is not connected to the internet. Turn wireless on and set up a connection in System Settings.");
                    return FAILED;
                }
            }
            card.fd = open(part, O_WRONLY | O_CREAT | O_TRUNC, 0666);
            if(card.fd < 0)
            {
                sd_failed("creating", errno);
                return FAILED;
            }
            writer = new(std::nothrow) Feed(*this, card);
            unpacker = writer ? new(std::nothrow) firstrun::Unpacker(*writer) : NULL;
            if(!unpacker || unpacker->error())
            {
                snprintf(why, sizeof(why), "Not enough memory.");
                return stopped();
            }
            // Unpacking and the SD card writes go to the core the game uses
            // for texture loading, so receiving does not wait for them. With
            // no such core, or no memory for the pipe, this thread does both.
            bool wanted = true;
#ifdef AC3DS_FIRSTRUN_TEST
            wanted = test.worker;
#endif
            if(wanted && pipe.start())
            {
                s32 priority = 0x30;
                svcGetThreadPriority(&priority, CUR_THREAD_HANDLE);
                threaded = true; // before the thread can ask
                thread = threadCreate(work_entry, this, 64 * 1024, priority, 2, false);
                if(!thread) threaded = false;
            }
            note("unpacking on %s", threaded ? "core 2" : "the main thread");

            // HOME and sleep would drop the connection half way; hold them off
            // until the file is in place. B cancels at any time.
            struct Hold
            {
                Hold() { aptSetHomeAllowed(false); aptSetSleepAllowed(false); }
                ~Hold() { aptSetHomeAllowed(true); aptSetSleepAllowed(true); }
            } hold;

            Ac3dsHttpsRequest request;
            request.url = url;
            request.roots = ac3ds_firstrun_roots;
            request.roots_size = sizeof(ac3ds_firstrun_roots);
            request.length = size;
            request.sink = on_body;
            request.progress = on_progress;
            request.log = on_log;
            request.user = this;
#ifdef AC3DS_FIRSTRUN_TEST
            if(test.host[0]) request.test_host = test.host;
            if(test.roots)
            {
                request.roots = test.roots;
                request.roots_size = test.roots_size;
            }
#endif
            const u64 began = osGetTime();
            const bool on_worker = threaded;
            can_cancel = true;
            const Ac3dsHttpsResult result = ac3ds_https_get(request);
            const u64 received = osGetTime();
            if(result.error == AC3DS_HTTPS_OK)
            {
                stage = "Unpacking the last files";
                host[0] = '\0';
            }
            join(result.error == AC3DS_HTTPS_OK);
            can_cancel = false;
            // The worker is gone: everything it used is this thread's again.
            const u64 per_ms = SYSCLOCK_ARM11 / 1000, ended = osGetTime();
            const u64 transfer = received - began, hashing = hash_ticks / per_ms, waiting = wait_ticks / per_ms, drawing = draw_ticks / per_ms;
            const u64 inline_unpack = on_worker ? 0 : unpack_ticks / per_ms, busy = hashing + waiting + drawing + inline_unpack;
            note("transfer: error %d, HTTP %d, %d redirects, %lld bytes, %d files, %llu ms; %s", int(result.error), result.status,
                 result.redirects, result.received, int(files), (unsigned long long)transfer, result.detail);
            note("main thread: SHA-256 %llu ms, waiting for the unpacker %llu ms, drawing %llu ms, network and TLS %llu ms (the rest)",
                 (unsigned long long)hashing, (unsigned long long)waiting, (unsigned long long)drawing,
                 (unsigned long long)(transfer > busy ? transfer - busy : 0));
            note("%s: decoding %llu ms, SD card writes %d calls %llu ms (longest %llu ms), waiting for data %llu ms; %llu ms more after the last byte arrived",
                 on_worker ? "core 2" : "main thread", (unsigned long long)((unpack_ticks - card.ticks) / per_ms), card.calls,
                 (unsigned long long)(card.ticks / per_ms), (unsigned long long)(card.longest / per_ms), (unsigned long long)(idle_ticks / per_ms), (unsigned long long)(ended - received));

            if(closing || cancelled) return stopped();
            if(card.failed)
            {
                sd_failed(card.failed, card.code);
                return stopped();
            }
            if(unpacker->error() && (!unpacker->sink_stopped() || writer->error()[0]))
            {
                // The bytes are not a tar.bz2 of the right shape.
                snprintf(why, sizeof(why), "The download is not the official release. It was discarded.");
                snprintf(detail, sizeof(detail), "%s", writer->error()[0] ? writer->error() : unpacker->error());
                return stopped();
            }
            if(result.error != AC3DS_HTTPS_OK)
            {
                if(result.error == AC3DS_HTTPS_SINK || result.error == AC3DS_HTTPS_CANCELLED)
                    snprintf(why, sizeof(why), "Unpacking stopped.");
                else explain(result);
                return stopped();
            }

            stage = "Checking the download";
            draw_progress();
            if(!unpacker->finish(size, sha256))
            {
                snprintf(why, sizeof(why), "The download does not match the official release. It was discarded.");
                snprintf(detail, sizeof(detail), "%s", unpacker->error());
                return stopped();
            }
            note("SHA-256 and size match; %d files, %d folders, %lld bytes unpacked", unpacker->archive().file_count(),
                 unpacker->archive().directory_count(), unpacker->archive().bytes_written());

            // The tables and the header complete the image; the rename makes
            // it the game data. Until the rename there is only the unfinished
            // file, which the next launch removes.
            stage = "Finishing the game data file";
            draw_progress();
            const u64 finishing = osGetTime();
            romfsimage::Info info;
            if(!writer->finish(firstrun::required_files, firstrun::required_count, unpacker->digest(), info))
            {
                if(card.failed) sd_failed(card.failed, card.code);
                else
                {
                    snprintf(why, sizeof(why), "The release is missing a file the game needs.");
                    snprintf(detail, sizeof(detail), "%s", writer->error());
                }
                return stopped();
            }
            const bool flushed = fsync(card.fd) == 0;
            int code = errno;
            const bool closed = close(card.fd) == 0;
            if(flushed && !closed) code = errno;
            card.fd = -1;
            if(!flushed || !closed)
            {
                unlink(part);
                sd_failed(flushed ? "closing" : "flushing", code);
                return stopped();
            }
            // A damaged image goes only now that a whole one can take its place.
            if(present(image) && unlink(image) != 0)
            {
                code = errno;
                unlink(part);
                snprintf(why, sizeof(why), "The damaged file /3ds/assaultcube/%s could not be replaced. Delete it and start again.", romfsimage::file_name);
                snprintf(detail, sizeof(detail), "error %d", code);
                return stopped();
            }
            if(rename(part, image) != 0)
            {
                code = errno;
                unlink(part);
                sd_failed("renaming", code);
                return stopped();
            }
            note("image: %u bytes, %u files, %u folders; tables %u bytes in memory when mounted; tables, flush and rename %llu ms",
                 info.image_size, info.files, info.directories,
                 info.dir_hash_size + info.dir_table_size + info.file_hash_size + info.file_table_size,
                 (unsigned long long)(osGetTime() - finishing));
            drop(false);
            return DONE;
        }
    };
}

Ac3dsFirstRun ac3ds_first_run(const char *game_dir, const char *damaged)
{
    ac3ds_blank_top_screen();
    Installer *installer = new(std::nothrow) Installer(game_dir, damaged);
    if(!installer) return AC3DS_FIRSTRUN_QUIT;
    const Ac3dsFirstRun result = installer->run();
    installer->write_log();
    delete installer;
    return result;
}

void ac3ds_first_run_tidy(const char *game_dir)
{
    // Only a download that did not finish leaves this file.
    char part[path_size];
    snprintf(part, sizeof(part), "%s/%s", game_dir, romfsimage::part_name);
    unlink(part);
}
