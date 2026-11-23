// https_3ds.cpp: TLS 1.2 GET over mbedTLS for the first-run download (see https_3ds.h)

#include <3ds.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <enet/enet.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>

#include "https_3ds.h"
#include "firstrun_core.h"

bool ac3ds_start_network();

namespace
{
    constexpr unsigned lookup_ms = 10000, connect_ms = 10000, handshake_ms = 30000, idle_ms = 20000;
    constexpr int max_redirects = 3;
    constexpr size_t head_limit = 16 * 1024, block_size = 16 * 1024;

    // Forward secrecy and an AEAD cipher only. ChaCha20 leads: the ARM11 has
    // no AES instructions.
    const int suites[] =
    {
        MBEDTLS_TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256,
        MBEDTLS_TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256,
        MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256,
        MBEDTLS_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256,
        MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384,
        MBEDTLS_TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384,
        0
    };
    const mbedtls_ecp_group_id curves[] =
    {
        MBEDTLS_ECP_DP_CURVE25519, MBEDTLS_ECP_DP_SECP256R1, MBEDTLS_ECP_DP_SECP384R1, MBEDTLS_ECP_DP_NONE
    };

    // The portlib's own entropy hook calls ssl:C and reports success whether
    // or not the service answered, so it cannot be relied on to fail. This
    // source is added beside it and does fail: it wants real bytes from ssl:C
    // or ps:ps, whichever the process may use.
    bool sslc_open = false, ps_open = false;

    int system_random(void *, unsigned char *out, size_t size, size_t *produced)
    {
        unsigned char other[MBEDTLS_ENTROPY_MAX_GATHER];
        *produced = 0;
        if(size > sizeof(other)) size = sizeof(other);
        bool filled = ps_open && R_SUCCEEDED(PS_GenerateRandomBytes(out, size));
        if(sslc_open && R_SUCCEEDED(sslcGenerateRandomData(other, size)))
        {
            if(filled) for(size_t i = 0; i < size; ++i) out[i] ^= other[i];
            else memcpy(out, other, size);
            filled = true;
        }
        if(!filled) return MBEDTLS_ERR_ENTROPY_SOURCE_FAILED;
        *produced = size;
        return 0;
    }

    int bio_send(void *context, const unsigned char *data, size_t size)
    {
        const int sent = send(*static_cast<ENetSocket *>(context), data, size, 0);
        if(sent >= 0) return sent;
        return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR ? MBEDTLS_ERR_SSL_WANT_WRITE : MBEDTLS_ERR_NET_SEND_FAILED;
    }

    int bio_receive(void *context, unsigned char *data, size_t size)
    {
        const int got = recv(*static_cast<ENetSocket *>(context), data, size, 0);
        if(got >= 0) return got; // 0: the other side closed
        return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR ? MBEDTLS_ERR_SSL_WANT_READ : MBEDTLS_ERR_NET_RECV_FAILED;
    }

    struct Session
    {
        const Ac3dsHttpsRequest &rq;
        Ac3dsHttpsResult &out;
        const u64 started = osGetTime();
        mbedtls_entropy_context entropy;
        mbedtls_ctr_drbg_context drbg;
        mbedtls_x509_crt roots;
        mbedtls_ssl_config conf;
        mbedtls_ssl_context ssl;
        bool tls_ready = false, ssl_ready = false, shaken = false, cut_off = false;
        ENetSocket sock = ENET_SOCKET_NULL;
        unsigned char *head = NULL, *block = NULL;
        const char *stage = "";

        Session(const Ac3dsHttpsRequest &rq, Ac3dsHttpsResult &out) : rq(rq), out(out) {}

        ~Session()
        {
            hang_up();
            if(tls_ready)
            {
                mbedtls_ssl_config_free(&conf);
                mbedtls_x509_crt_free(&roots);
                mbedtls_ctr_drbg_free(&drbg);
                mbedtls_entropy_free(&entropy);
            }
            if(sslc_open) sslcExit();
            if(ps_open) psExit();
            sslc_open = ps_open = false;
            free(head);
            free(block);
        }

        __attribute__((format(printf, 2, 3))) void note(const char *format, ...)
        {
            if(!rq.log) return;
            char line[224];
            va_list args;
            va_start(args, format);
            vsnprintf(line, sizeof(line), format, args);
            va_end(args);
            rq.log(rq.user, line);
        }

        __attribute__((format(printf, 3, 4))) bool fail(Ac3dsHttpsError error, const char *format, ...)
        {
            if(out.error != AC3DS_HTTPS_OK) return false; // the first failure is the one to report
            out.error = error;
            va_list args;
            va_start(args, format);
            vsnprintf(out.detail, sizeof(out.detail), format, args);
            va_end(args);
            return false;
        }

        bool tls_fail(Ac3dsHttpsError error, int code, const char *what)
        {
            char text[96];
            mbedtls_strerror(code, text, sizeof(text));
            out.tls_error = code;
            return fail(error, "%s: %s", what, text);
        }

        // Progress, cancel and the overall time limit. False stops the request.
        bool tick()
        {
            if(osGetTime() - started > rq.total_ms)
                return fail(AC3DS_HTTPS_TIMEOUT, "not finished after %u s", rq.total_ms / 1000);
            if(rq.progress && rq.progress(rq.user, stage, out.host, out.received, out.length))
                return fail(AC3DS_HTTPS_CANCELLED, "cancelled");
            return true;
        }

        bool allowed(const firstrun::Url &url) const
        {
#ifdef AC3DS_FIRSTRUN_TEST
            if(rq.test_host && !strcmp(url.host, rq.test_host)) return true;
#endif
            return firstrun::github_url(url);
        }

        bool open_random()
        {
            const Result ssl_result = sslcInit(0), ps_result = psInit();
            sslc_open = R_SUCCEEDED(ssl_result);
            ps_open = R_SUCCEEDED(ps_result);
            unsigned char probe[32] = { 0 };
            size_t produced = 0;
            bool live = false;
            if(!system_random(NULL, probe, sizeof(probe), &produced))
                for(unsigned char byte : probe) live |= byte != 0;
            note("random sources: ssl:C %08lX, ps:ps %08lX, %s",
                 (unsigned long)ssl_result, (unsigned long)ps_result, live ? "usable" : "none usable");
            if(!live)
                return fail(AC3DS_HTTPS_NO_RANDOM, "no system random source (ssl:C %08lX, ps:ps %08lX)",
                            (unsigned long)ssl_result, (unsigned long)ps_result);
            return true;
        }

        bool setup_tls()
        {
            mbedtls_entropy_init(&entropy);
            mbedtls_ctr_drbg_init(&drbg);
            mbedtls_x509_crt_init(&roots);
            mbedtls_ssl_config_init(&conf);
            tls_ready = true;
            int ret = mbedtls_entropy_add_source(&entropy, system_random, NULL, 32, MBEDTLS_ENTROPY_SOURCE_STRONG);
            // Timing goes in as the personalisation string; the seed itself is the system's.
            u64 personal[2] = { svcGetSystemTick(), osGetTime() };
            if(!ret) ret = mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy,
                                                 reinterpret_cast<const unsigned char *>(personal), sizeof(personal));
            if(ret) return tls_fail(AC3DS_HTTPS_NO_RANDOM, ret, "random generator");
            ret = mbedtls_x509_crt_parse(&roots, reinterpret_cast<const unsigned char *>(rq.roots), rq.roots_size);
            if(ret) return ret > 0 ? fail(AC3DS_HTTPS_TLS, "%d trusted roots could not be read", ret)
                                   : tls_fail(AC3DS_HTTPS_TLS, ret, "trusted roots");
            ret = mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
            if(ret) return tls_fail(AC3DS_HTTPS_MEMORY, ret, "TLS settings");
            mbedtls_ssl_conf_min_version(&conf, MBEDTLS_SSL_MAJOR_VERSION_3, MBEDTLS_SSL_MINOR_VERSION_3); // TLS 1.2
            mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_REQUIRED);
            mbedtls_ssl_conf_ca_chain(&conf, &roots, NULL);
            mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &drbg);
            mbedtls_ssl_conf_ciphersuites(&conf, suites);
            mbedtls_ssl_conf_curves(&conf, curves);
            mbedtls_ssl_conf_session_tickets(&conf, MBEDTLS_SSL_SESSION_TICKETS_DISABLED);
            return true;
        }

        // The socket service has no timeout for a name lookup, so it runs on
        // ENet's helper thread, as the TCP connect does (source/enet/unix.c).
        bool lookup(const char *host, enet_uint32 &address)
        {
            void *pending = enet_3ds_resolve_begin(host);
            if(!pending) return fail(AC3DS_HTTPS_LOOKUP, "lookup of %s did not start", host);
            stage = "Looking up the address";
            const u64 begin = osGetTime();
            for(;;)
            {
                ENetAddress found = { ENET_HOST_ANY, 0 };
                const int state = enet_3ds_resolve_poll(pending, &found, 20);
                if(!state)
                {
                    address = found.host;
                    return true;
                }
                if(state < 0) return fail(AC3DS_HTTPS_LOOKUP, "%s did not resolve", host);
                const bool late = osGetTime() - begin > lookup_ms;
                if(late || !tick())
                {
                    enet_3ds_resolve_abandon(pending);
                    return late ? fail(AC3DS_HTTPS_LOOKUP, "no answer for %s in %u s", host, lookup_ms / 1000) : false;
                }
            }
        }

        bool connect_to(const char *host, enet_uint32 address, int port)
        {
            ENetAddress target = { address, enet_uint16(port) };
            sock = enet_socket_create(ENET_SOCKET_TYPE_STREAM);
            if(sock == ENET_SOCKET_NULL) return fail(AC3DS_HTTPS_CONNECT, "could not open a socket");
            // Set before connecting, when it still sizes the TCP window. 32 KiB
            // is what other 3DS software ships; 64 KiB and above have been
            // reported to break the TLS handshake on the console.
            enet_socket_set_option(sock, ENET_SOCKOPT_RCVBUF, 32 * 1024);
            void *pending = enet_3ds_connect_begin(sock, &target);
            stage = "Connecting";
            const u64 begin = osGetTime();
            while(pending)
            {
                const int status = enet_3ds_connect_poll(pending, 50); // leaves the socket nonblocking
                if(!status) return true;
                if(status < 0) break;
                const bool late = osGetTime() - begin > connect_ms;
                if(late || !tick())
                {
                    enet_3ds_connect_abandon(pending); // closes the socket when the connect returns
                    sock = ENET_SOCKET_NULL;
                    return late ? fail(AC3DS_HTTPS_CONNECT, "no answer from %s in %u s", host, connect_ms / 1000) : false;
                }
            }
            enet_socket_destroy(sock);
            sock = ENET_SOCKET_NULL;
            return fail(AC3DS_HTTPS_CONNECT, "%s: %s", host, enet_3ds_connect_diag());
        }

        void wait_for(int want)
        {
            enet_uint32 events = want == MBEDTLS_ERR_SSL_WANT_WRITE ? ENET_SOCKET_WAIT_SEND : ENET_SOCKET_WAIT_RECEIVE;
            if(enet_socket_wait(sock, &events, 50) < 0) svcSleepThread(20000000LL); // never spin on a dead socket
        }

        bool handshake(const char *host)
        {
            mbedtls_ssl_init(&ssl);
            ssl_ready = true;
            int ret = mbedtls_ssl_setup(&ssl, &conf);
            if(!ret) ret = mbedtls_ssl_set_hostname(&ssl, host); // server name indication and the name the certificate must carry
            if(ret) return tls_fail(AC3DS_HTTPS_MEMORY, ret, "TLS setup");
            mbedtls_ssl_set_bio(&ssl, &sock, bio_send, bio_receive, NULL);
            stage = "Securing the connection";
            const u64 begin = osGetTime();
            // One step at a time, so the display and the cancel button stay
            // alive between the slow public-key operations.
            while(ssl.state != MBEDTLS_SSL_HANDSHAKE_OVER)
            {
                ret = mbedtls_ssl_handshake_step(&ssl);
                if(ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) wait_for(ret);
                else if(ret) break;
                if(osGetTime() - begin > handshake_ms)
                    return fail(AC3DS_HTTPS_TIMEOUT, "handshake with %s not finished in %u s", host, handshake_ms / 1000);
                if(!tick()) return false;
            }
            const unsigned elapsed = unsigned(osGetTime() - begin);
            if(ret)
            {
                const unsigned flags = mbedtls_ssl_get_verify_result(&ssl);
                if(!flags || flags == 0xFFFFFFFFu) return tls_fail(AC3DS_HTTPS_TLS, ret, "handshake");
                const unsigned dates = MBEDTLS_X509_BADCERT_EXPIRED | MBEDTLS_X509_BADCERT_FUTURE;
                char why[160];
                mbedtls_x509_crt_verify_info(why, sizeof(why), "", flags);
                for(char *c = why; *c; ++c) if(*c == '\n') *c = c[1] ? ';' : '\0';
                out.verify_flags = flags;
                out.tls_error = ret;
                return fail(flags & ~dates ? AC3DS_HTTPS_CERT_UNTRUSTED : AC3DS_HTTPS_CERT_DATE,
                            "certificate of %s refused: %s", host, why);
            }
            shaken = true;
            note("%s: handshake %u ms, %s, %s", host, elapsed, mbedtls_ssl_get_version(&ssl), mbedtls_ssl_get_ciphersuite(&ssl));
            return true;
        }

        bool send_request(const firstrun::Url &url)
        {
            static char request[firstrun::max_url + 512];
            char port[8] = "";
            if(url.port != 443) snprintf(port, sizeof(port), ":%d", url.port);
            // Nothing about the console or its owner goes out.
            const int length = snprintf(request, sizeof(request),
                "GET %s HTTP/1.1\r\nHost: %s%s\r\nUser-Agent: AC3DS\r\nAccept: */*\r\n"
                "Accept-Encoding: identity\r\nConnection: close\r\n\r\n", url.path, url.host, port);
            if(length <= 0 || length >= int(sizeof(request))) return fail(AC3DS_HTTPS_BAD_URL, "address too long");
            stage = "Sending the request";
            const u64 begin = osGetTime();
            for(int sent = 0; sent < length;)
            {
                const int ret = mbedtls_ssl_write(&ssl, reinterpret_cast<const unsigned char *>(request) + sent, length - sent);
                if(ret > 0) { sent += ret; continue; }
                if(ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE)
                    return tls_fail(AC3DS_HTTPS_TLS, ret, "sending the request");
                if(osGetTime() - begin > idle_ms) return fail(AC3DS_HTTPS_TIMEOUT, "%s did not take the request", url.host);
                if(!tick()) return false;
                wait_for(ret);
            }
            return true;
        }

        // Bytes read, 0 when the other side has finished, -1 with the error set.
        int read_some(unsigned char *buffer, size_t size)
        {
            const u64 begin = osGetTime();
            for(;;)
            {
                const int ret = mbedtls_ssl_read(&ssl, buffer, size);
                if(ret > 0) return ret;
                if(ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) return 0;
                if(!ret)
                {
                    cut_off = true; // closed without the TLS goodbye: cannot be told from a cut connection
                    return 0;
                }
                if(ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE)
                {
                    tls_fail(AC3DS_HTTPS_TLS, ret, "receiving");
                    return -1;
                }
                if(osGetTime() - begin > idle_ms)
                {
                    fail(AC3DS_HTTPS_TIMEOUT, "%s sent nothing for %u s", out.host, idle_ms / 1000);
                    return -1;
                }
                if(!tick()) return -1;
                wait_for(ret);
            }
        }

        bool read_head(firstrun::Head &parsed, size_t &body, size_t &have)
        {
            stage = "Waiting for the answer";
            for(have = 0;;)
            {
                if(have == head_limit) return fail(AC3DS_HTTPS_PROTOCOL, "response headers over %u KB", unsigned(head_limit / 1024));
                const int got = read_some(head + have, head_limit - have);
                if(got < 0) return false;
                if(!got) return fail(AC3DS_HTTPS_PROTOCOL, "%s closed the connection without answering", out.host);
                have += got;
                if((body = firstrun::head_end(reinterpret_cast<const char *>(head), have))) break;
            }
            if(!firstrun::parse_head(reinterpret_cast<const char *>(head), body, parsed))
                return fail(AC3DS_HTTPS_PROTOCOL, "%s sent a response that cannot be read", out.host);
            return true;
        }

        bool deliver(const unsigned char *data, size_t size)
        {
            if(size && rq.sink && !rq.sink(rq.user, data, size)) return fail(AC3DS_HTTPS_SINK, "the data could not be stored");
            out.received += size;
            return true;
        }

        bool read_body(const firstrun::Head &parsed, const unsigned char *first, size_t first_size)
        {
            out.length = parsed.chunked ? -1 : parsed.length;
            // The release has one known size; a server that will not commit to
            // it before the first byte is not sending the release.
            if(out.length != rq.length)
                return fail(AC3DS_HTTPS_LENGTH, "%s offers %lld bytes, %lld expected", out.host, out.length, rq.length);
            stage = "Receiving";
            if(first_size > size_t(rq.length)) first_size = size_t(rq.length);
            if(!deliver(first, first_size)) return false;
            while(out.received < rq.length)
            {
                const long long want = rq.length - out.received;
                const int got = read_some(block, want < (long long)block_size ? size_t(want) : block_size);
                if(got < 0) return false;
                if(!got) return fail(AC3DS_HTTPS_PROTOCOL, "%s closed the connection early, at %lld of %lld bytes",
                                     out.host, out.received, rq.length);
                if(!deliver(block, got) || !tick()) return false;
            }
            return true;
        }

        void hang_up()
        {
            if(ssl_ready)
            {
                if(shaken) mbedtls_ssl_close_notify(&ssl); // best effort, the socket is closed next
                mbedtls_ssl_free(&ssl);
            }
            ssl_ready = shaken = false;
            if(sock != ENET_SOCKET_NULL) enet_socket_destroy(sock);
            sock = ENET_SOCKET_NULL;
        }

        void run()
        {
            static firstrun::Url url, next;
            static firstrun::Head parsed;
            if(!rq.url || !rq.roots || !firstrun::parse_url(rq.url, url))
            {
                fail(AC3DS_HTTPS_BAD_URL, "not an https address");
                return;
            }
            snprintf(out.host, sizeof(out.host), "%s", url.host);
            if(!allowed(url))
            {
                fail(AC3DS_HTTPS_BAD_URL, "%s is not a GitHub host", url.host);
                return;
            }
            if(!ac3ds_start_network())
            {
                fail(AC3DS_HTTPS_NO_NETWORK, "the socket service is not available");
                return;
            }
            head = static_cast<unsigned char *>(malloc(head_limit));
            block = static_cast<unsigned char *>(malloc(block_size));
            if(!head || !block)
            {
                fail(AC3DS_HTTPS_MEMORY, "out of memory");
                return;
            }
            if(!open_random() || !setup_tls()) return;
            for(;;)
            {
                enet_uint32 address = 0;
                size_t body = 0, have = 0;
                snprintf(out.host, sizeof(out.host), "%s", url.host);
                if(!lookup(url.host, address) || !connect_to(url.host, address, url.port) || !handshake(url.host) ||
                   !send_request(url) || !read_head(parsed, body, have)) return;
                out.status = parsed.status;
                note("%s: HTTP %d", url.host, parsed.status);
                if(parsed.status == 301 || parsed.status == 302 || parsed.status == 303 || parsed.status == 307 || parsed.status == 308)
                {
                    if(out.redirects >= max_redirects)
                        fail(AC3DS_HTTPS_REDIRECT, "more than %d redirects", max_redirects);
                    else if(!parsed.location[0] || !firstrun::parse_url(parsed.location, next, &url))
                        fail(AC3DS_HTTPS_REDIRECT, "%s redirects to something that is not https", url.host);
                    else if(!allowed(next))
                        fail(AC3DS_HTTPS_REDIRECT, "redirect from %s to %s refused", url.host, next.host);
                    if(out.error) return;
                    ++out.redirects;
                    hang_up();
                    url = next;
                    continue;
                }
                if(parsed.status != 200) fail(AC3DS_HTTPS_STATUS, "%s answered %d", url.host, parsed.status);
                else read_body(parsed, head + body, have - body);
                return;
            }
        }
    };
}

Ac3dsHttpsResult ac3ds_https_get(const Ac3dsHttpsRequest &request)
{
    Ac3dsHttpsResult result = {};
    result.length = -1;
    {
        Session session(request, result);
        session.run();
    }
    return result;
}
