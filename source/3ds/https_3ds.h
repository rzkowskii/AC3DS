#pragma once

#include <stddef.h>

// A small HTTPS GET for the first-run download. The system's own TLS stops at
// 1.1, which GitHub refuses, so this runs TLS 1.2 through mbedTLS. Every host
// contacted, redirects included, must present a certificate that chains to one
// of the roots passed in and names that host; there is no way to continue past
// a failed check. Main thread only: the caller's progress function is the only
// place the display and the buttons are served while a request runs.

enum Ac3dsHttpsError
{
    AC3DS_HTTPS_OK = 0,
    AC3DS_HTTPS_NO_NETWORK,     // the socket service is not running
    AC3DS_HTTPS_NO_RANDOM,      // no system random source, so no safe handshake
    AC3DS_HTTPS_BAD_URL,
    AC3DS_HTTPS_LOOKUP,         // the host name did not resolve
    AC3DS_HTTPS_CONNECT,
    AC3DS_HTTPS_TLS,            // handshake or record failure other than the certificate
    AC3DS_HTTPS_CERT_DATE,      // certificate expired or not yet valid by the console clock
    AC3DS_HTTPS_CERT_UNTRUSTED, // not signed by a given root, or issued for another name
    AC3DS_HTTPS_PROTOCOL,       // malformed, oversized or cut-off response
    AC3DS_HTTPS_REDIRECT,       // a redirect that is not followed
    AC3DS_HTTPS_STATUS,         // an HTTP status other than 200, see status
    AC3DS_HTTPS_LENGTH,         // the body offered is not the length asked for
    AC3DS_HTTPS_TIMEOUT,
    AC3DS_HTTPS_CANCELLED,
    AC3DS_HTTPS_SINK,           // the receiver refused the data
    AC3DS_HTTPS_MEMORY
};

struct Ac3dsHttpsRequest
{
    const char *url = NULL;        // https://github.com/...; redirects may lead to a githubusercontent.com host
    const char *roots = NULL;      // PEM trust anchors
    size_t roots_size = 0;         // with the terminating NUL
    long long length = 0;          // the body must be exactly this long, and say so up front
    unsigned total_ms = 3600000;   // for the whole request, redirects included
    // Receives the body of the final 200 response in order. False stops.
    bool (*sink)(void *user, const unsigned char *data, size_t size) = NULL;
    // Called at least ten times a second with what is going on and with which
    // host; total is -1 until the server has said. True cancels.
    bool (*progress)(void *user, const char *stage, const char *host, long long done, long long total) = NULL;
    void (*log)(void *user, const char *line) = NULL;
    void *user = NULL;
#ifdef AC3DS_FIRSTRUN_TEST
    const char *test_host = NULL;  // QA: one more host, on any port, that may be contacted
#endif
};

struct Ac3dsHttpsResult
{
    Ac3dsHttpsError error;
    int status;                // last HTTP status seen, 0 if none
    int redirects;
    long long received;        // body bytes handed to the sink
    long long length;          // Content-Length of the final response, -1 if none
    unsigned verify_flags;     // mbedTLS certificate verification flags, 0 when it passed
    int tls_error;             // mbedTLS error code of a failed TLS call
    char host[128];            // the host last contacted
    char detail[192];          // for the log
};

Ac3dsHttpsResult ac3ds_https_get(const Ac3dsHttpsRequest &request);
