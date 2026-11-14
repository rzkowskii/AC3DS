/**
 @file  unix.c
 @brief ENet Unix system specific functions
*/
#ifndef WIN32

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <time.h>

#ifdef __3DS__
#include <limits.h>
#include <poll.h>
#include <3ds/os.h>
#include <3ds/svc.h>
#include <3ds/synchronization.h>
#include <3ds/thread.h>
#include <stdlib.h>
#include <stdio.h>
#define HAS_FCNTL 1
#define HAS_INET_NTOP 1
#define HAS_SOCKLEN_T 1
#ifndef SOMAXCONN
#define SOMAXCONN 16
#endif
#endif

#define ENET_BUILDING_LIB 1
#include "enet/enet.h"

#ifdef __APPLE__
#ifdef HAS_POLL
#undef HAS_POLL
#endif
#ifndef HAS_FCNTL
#define HAS_FCNTL 1
#endif
#ifndef HAS_INET_PTON
#define HAS_INET_PTON 1
#endif
#ifndef HAS_INET_NTOP
#define HAS_INET_NTOP 1
#endif
#ifndef HAS_MSGHDR_FLAGS
#define HAS_MSGHDR_FLAGS 1
#endif
#ifndef HAS_SOCKLEN_T
#define HAS_SOCKLEN_T 1
#endif
#endif

#ifdef HAS_FCNTL
#include <fcntl.h>
#endif

#ifdef HAS_POLL
#include <sys/poll.h>
#endif

#ifndef HAS_SOCKLEN_T
typedef int socklen_t;
#endif

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

static enet_uint32 timeBase = 0;

#ifdef __3DS__
static enet_uint32
enet_3ds_clock (void)
{
    return (enet_uint32) (svcGetSystemTick () * 1000 / SYSCLOCK_ARM11);
}
#endif

int
enet_initialize (void)
{
    return 0;
}

void
enet_deinitialize (void)
{
}

enet_uint32
enet_time_get (void)
{
#ifdef __3DS__
    return enet_3ds_clock () - timeBase;
#else
    struct timeval timeVal;

    gettimeofday (& timeVal, NULL);

    return timeVal.tv_sec * 1000 + timeVal.tv_usec / 1000 - timeBase;
#endif
}

void
enet_time_set (enet_uint32 newTimeBase)
{
#ifdef __3DS__
    timeBase = enet_3ds_clock () - newTimeBase;
#else
    struct timeval timeVal;

    gettimeofday (& timeVal, NULL);

    timeBase = timeVal.tv_sec * 1000 + timeVal.tv_usec / 1000 - newTimeBase;
#endif
}

int
enet_address_set_host (ENetAddress * address, const char * name)
{
#ifdef __3DS__
    struct addrinfo hints = { 0 };
    struct addrinfo * results = NULL;
    if (inet_pton (AF_INET, name, & address -> host) == 1)
      return 0;

    /* libctru's gethostbyname returns shared storage. DNS lookups can run on
       the server-browser and media-download threads at the same time. */
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo (name, NULL, & hints, & results) != 0 || results == NULL)
      return -1;
    address -> host = ((const struct sockaddr_in *) results -> ai_addr) -> sin_addr.s_addr;
    freeaddrinfo (results);
    return 0;
#else
    struct hostent * hostEntry = NULL;
#ifdef HAS_GETHOSTBYNAME_R
    struct hostent hostData;
    char buffer [2048];
    int errnum;

#if defined(linux) || defined(__linux) || defined(__linux__) || defined(__FreeBSD__) || defined(__FreeBSD_kernel__)
    gethostbyname_r (name, & hostData, buffer, sizeof (buffer), & hostEntry, & errnum);
#else
    hostEntry = gethostbyname_r (name, & hostData, buffer, sizeof (buffer), & errnum);
#endif
#else
    hostEntry = gethostbyname (name);
#endif

    if (hostEntry == NULL ||
        hostEntry -> h_addrtype != AF_INET)
    {
#ifdef HAS_INET_PTON
        if (! inet_pton (AF_INET, name, & address -> host))
#else
        if (! inet_aton (name, (struct in_addr *) & address -> host))
#endif
            return -1;
        return 0;
    }

    address -> host = * (enet_uint32 *) hostEntry -> h_addr_list [0];

    return 0;
#endif
}

int
enet_address_get_host_ip (const ENetAddress * address, char * name, size_t nameLength)
{
#ifdef HAS_INET_NTOP
    if (inet_ntop (AF_INET, & address -> host, name, nameLength) == NULL)
#else
    char * addr = inet_ntoa (* (struct in_addr *) & address -> host);
    if (addr != NULL)
    {
        strncpy (name, addr, nameLength);
        name[nameLength - 1] = '\0';
    }
    else
#endif
        return -1;
    return 0;
}

int
enet_address_get_host (const ENetAddress * address, char * name, size_t nameLength)
{
#ifdef __3DS__
    struct sockaddr_in sin = { 0 };
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = address -> host;
    if (getnameinfo ((struct sockaddr *) & sin, sizeof (sin), name, nameLength,
                     NULL, 0, NI_NAMEREQD) == 0)
      return 0;
    return enet_address_get_host_ip (address, name, nameLength);
#else
    struct in_addr in;
    struct hostent * hostEntry = NULL;
#ifdef HAS_GETHOSTBYADDR_R
    struct hostent hostData;
    char buffer [2048];
    int errnum;

    in.s_addr = address -> host;

#if defined(linux) || defined(__linux) || defined(__linux__) || defined(__FreeBSD__) || defined(__FreeBSD_kernel__)
    gethostbyaddr_r ((char *) & in, sizeof (struct in_addr), AF_INET, & hostData, buffer, sizeof (buffer), & hostEntry, & errnum);
#else
    hostEntry = gethostbyaddr_r ((char *) & in, sizeof (struct in_addr), AF_INET, & hostData, buffer, sizeof (buffer), & errnum);
#endif
#else
    in.s_addr = address -> host;

    hostEntry = gethostbyaddr ((char *) & in, sizeof (struct in_addr), AF_INET);
#endif

    if (hostEntry == NULL)
      return enet_address_get_host_ip (address, name, nameLength);

    strncpy (name, hostEntry -> h_name, nameLength);
    name[nameLength - 1] = '\0';

    return 0;
#endif
}

int
enet_socket_bind (ENetSocket socket, const ENetAddress * address)
{
    struct sockaddr_in sin;

    memset (& sin, 0, sizeof (struct sockaddr_in));

    sin.sin_family = AF_INET;

    if (address != NULL)
    {
       sin.sin_port = ENET_HOST_TO_NET_16 (address -> port);
       sin.sin_addr.s_addr = address -> host;
    }
    else
    {
       sin.sin_port = 0;
       sin.sin_addr.s_addr = INADDR_ANY;
    }

    return bind (socket,
                 (struct sockaddr *) & sin,
                 sizeof (struct sockaddr_in));
}

int
enet_socket_listen (ENetSocket socket, int backlog)
{
    return listen (socket, backlog < 0 ? SOMAXCONN : backlog);
}

ENetSocket
enet_socket_create (ENetSocketType type)
{
    return socket (PF_INET, type == ENET_SOCKET_TYPE_DATAGRAM ? SOCK_DGRAM : SOCK_STREAM, 0);
}

int
enet_socket_set_option (ENetSocket socket, ENetSocketOption option, int value)
{
    int result = -1;
    switch (option)
    {
        case ENET_SOCKOPT_NONBLOCK:
#if defined(__3DS__)
            result = fcntl (socket, F_SETFL, value ? O_NONBLOCK : 0);
#elif defined(HAS_FCNTL)
            result = fcntl (socket, F_SETFL, O_NONBLOCK | fcntl (socket, F_GETFL));
#else
            result = ioctl (socket, FIONBIO, & value);
#endif
            break;

        case ENET_SOCKOPT_BROADCAST:
#ifdef __3DS__
            /* SOC allows IPv4 broadcast without a socket option. */
            result = 0;
#else
            result = setsockopt (socket, SOL_SOCKET, SO_BROADCAST, (char *) & value, sizeof (int));
#endif
            break;

        case ENET_SOCKOPT_REUSEADDR:
            result = setsockopt (socket, SOL_SOCKET, SO_REUSEADDR, (char *) & value, sizeof (int));
            break;

        case ENET_SOCKOPT_RCVBUF:
            result = setsockopt (socket, SOL_SOCKET, SO_RCVBUF, (char *) & value, sizeof (int));
            break;

        case ENET_SOCKOPT_SNDBUF:
            result = setsockopt (socket, SOL_SOCKET, SO_SNDBUF, (char *) & value, sizeof (int));
            break;

        case ENET_SOCKOPT_RCVTIMEO:
#ifdef SO_RCVTIMEO
            result = setsockopt (socket, SOL_SOCKET, SO_RCVTIMEO, (char *) & value, sizeof (int));
#endif
            break;

        case ENET_SOCKOPT_SNDTIMEO:
#ifdef SO_SNDTIMEO
            result = setsockopt (socket, SOL_SOCKET, SO_SNDTIMEO, (char *) & value, sizeof (int));
#endif
            break;

        default:
            break;
    }
    return result == -1 ? -1 : 0;
}

int
enet_socket_connect (ENetSocket socket, const ENetAddress * address)
{
    struct sockaddr_in sin;
    int result;

    memset (& sin, 0, sizeof (struct sockaddr_in));

    sin.sin_family = AF_INET;
    sin.sin_port = ENET_HOST_TO_NET_16 (address -> port);
    sin.sin_addr.s_addr = address -> host;

    result = connect (socket, (struct sockaddr *) & sin, sizeof (struct sockaddr_in));
    if (result == -1 && errno == EINPROGRESS)
      return 0;

    return result;
}

ENetSocket
enet_socket_accept (ENetSocket socket, ENetAddress * address)
{
    int result;
    struct sockaddr_in sin;
    socklen_t sinLength = sizeof (struct sockaddr_in);

    result = accept (socket,
                     address != NULL ? (struct sockaddr *) & sin : NULL,
                     address != NULL ? & sinLength : NULL);

    if (result == -1)
      return ENET_SOCKET_NULL;

    if (address != NULL)
    {
        address -> host = (enet_uint32) sin.sin_addr.s_addr;
        address -> port = ENET_NET_TO_HOST_16 (sin.sin_port);
    }

    return result;
}

int
enet_socket_shutdown (ENetSocket socket, ENetSocketShutdown how)
{
    return shutdown (socket, (int) how);
}

void
enet_socket_destroy (ENetSocket socket)
{
    if (socket != -1)
      close (socket);
}

#ifdef __3DS__
/* SOC has sendto/recvfrom, but no scatter/gather message calls. ENet protocol
   writes must still produce a single datagram. */
static size_t
enet_3ds_buffer_size (const ENetBuffer * buffers, size_t count)
{
    size_t size = 0;
    size_t i;
    for (i = 0; i < count; ++i)
    {
        if (buffers [i].dataLength > INT_MAX - size)
          return 0;
        size += buffers [i].dataLength;
    }
    return size;
}

typedef struct
{
    ENetSocket socket;
    struct sockaddr_in address;
    LightLock lock;
    int state; /* 0 pending, 1 connected, -1 failed */
    int error;
    int abandoned;
} Enet3dsConnect;

static char enet_3ds_diag [96] = "none";

const char *
enet_3ds_connect_diag (void)
{
    return enet_3ds_diag;
}

static void
enet_3ds_connect_main (void * arg)
{
    Enet3dsConnect * c = (Enet3dsConnect *) arg;
    int result = connect (c -> socket, (struct sockaddr *) & c -> address, sizeof (struct sockaddr_in));
    int error = result == 0 ? 0 : errno;
    int abandoned;
    LightLock_Lock (& c -> lock);
    c -> error = error;
    c -> state = result == 0 ? 1 : -1;
    abandoned = c -> abandoned;
    LightLock_Unlock (& c -> lock);
    if (abandoned)
    {
        close (c -> socket);
        free (c);
    }
}

void *
enet_3ds_connect_begin (ENetSocket socket, const ENetAddress * address)
{
    Enet3dsConnect * c;
    s32 priority = 0x30;
    if (fcntl (socket, F_SETFL, 0) != 0)
    {
        snprintf (enet_3ds_diag, sizeof (enet_3ds_diag), "blocking mode failed, errno %d", errno);
        return NULL;
    }
    c = (Enet3dsConnect *) calloc (1, sizeof (Enet3dsConnect));
    if (c == NULL)
      return NULL;
    c -> socket = socket;
    c -> address.sin_family = AF_INET;
    c -> address.sin_port = ENET_HOST_TO_NET_16 (address -> port);
    c -> address.sin_addr.s_addr = address -> host;
    LightLock_Init (& c -> lock);
    svcGetThreadPriority (& priority, CUR_THREAD_HANDLE);
    /* Detached: libctru frees the thread object when it returns. */
    if (threadCreate (enet_3ds_connect_main, c, 16 * 1024, priority, -2, true) == NULL)
    {
        snprintf (enet_3ds_diag, sizeof (enet_3ds_diag), "connect thread did not start");
        free (c);
        return NULL;
    }
    return c;
}

int
enet_3ds_connect_poll (void * handle, int timeout)
{
    Enet3dsConnect * c = (Enet3dsConnect *) handle;
    int waited = 0;
    for (;;)
    {
        int state, error;
        LightLock_Lock (& c -> lock);
        state = c -> state;
        error = c -> error;
        LightLock_Unlock (& c -> lock);
        if (state != 0)
        {
            if (state > 0)
              fcntl (c -> socket, F_SETFL, O_NONBLOCK);
            else
              snprintf (enet_3ds_diag, sizeof (enet_3ds_diag), "blocking connect failed, errno %d", error);
            free (c);
            return state > 0 ? 0 : -1;
        }
        if (waited >= timeout)
          return 1;
        svcSleepThread (10000000LL);
        waited += 10;
    }
}

void
enet_3ds_connect_abandon (void * handle)
{
    Enet3dsConnect * c = (Enet3dsConnect *) handle;
    int finished;
    LightLock_Lock (& c -> lock);
    finished = c -> state != 0;
    c -> abandoned = 1;
    LightLock_Unlock (& c -> lock);
    if (finished)
    {
        close (c -> socket);
        free (c);
    }
    snprintf (enet_3ds_diag, sizeof (enet_3ds_diag), "connect abandoned (timeout or cancel)");
}

int
enet_3ds_connect_status (ENetSocket socket, int timeout)
{
    struct pollfd fd = { socket, POLLOUT, 0 };
    int ready = poll (& fd, 1, timeout);
    int error = 0;
    socklen_t length = sizeof (error);
    if (ready <= 0)
      return ready == 0 ? 1 : -1;
    if (getsockopt (socket, SOL_SOCKET, SO_ERROR, & error, & length) != 0 || error != 0)
      return -1;
    return (fd.revents & POLLOUT) ? 0 : -1;
}
#endif

int
enet_socket_send (ENetSocket socket,
                  const ENetAddress * address,
                  const ENetBuffer * buffers,
                  size_t bufferCount)
{
#ifdef __3DS__
    struct sockaddr_in sin = { 0 };
    enet_uint8 packet [ENET_PROTOCOL_MAXIMUM_MTU];
    size_t size = enet_3ds_buffer_size (buffers, bufferCount);
    const void * data;
    enet_uint8 * joined = NULL;
    int sentLength;

    if (size == 0)
      return 0;
    if (bufferCount == 1)
      data = buffers [0].data;
    else
    {
        size_t i, offset = 0;
        joined = size <= sizeof (packet) ? packet : enet_malloc (size);
        if (joined == NULL)
          return -1;
        for (i = 0; i < bufferCount; ++i)
        {
            memcpy (joined + offset, buffers [i].data, buffers [i].dataLength);
            offset += buffers [i].dataLength;
        }
        data = joined;
    }
    if (address != NULL)
    {
        sin.sin_family = AF_INET;
        sin.sin_port = ENET_HOST_TO_NET_16 (address -> port);
        sin.sin_addr.s_addr = address -> host;
    }
    sentLength = sendto (socket, data, size, 0,
                        address != NULL ? (const struct sockaddr *) & sin : NULL,
                        address != NULL ? sizeof (sin) : 0);
    if (joined != NULL && joined != packet)
      enet_free (joined);
    if (sentLength < 0)
      return errno == EWOULDBLOCK || errno == EAGAIN ? 0 : -1;
    return sentLength;
#else
    struct msghdr msgHdr;
    struct sockaddr_in sin;
    int sentLength;

    memset (& msgHdr, 0, sizeof (struct msghdr));

    if (address != NULL)
    {
        memset (& sin, 0, sizeof (struct sockaddr_in));

        sin.sin_family = AF_INET;
        sin.sin_port = ENET_HOST_TO_NET_16 (address -> port);
        sin.sin_addr.s_addr = address -> host;

        msgHdr.msg_name = & sin;
        msgHdr.msg_namelen = sizeof (struct sockaddr_in);
    }

    msgHdr.msg_iov = (struct iovec *) buffers;
    msgHdr.msg_iovlen = bufferCount;

    sentLength = sendmsg (socket, & msgHdr, MSG_NOSIGNAL);

    if (sentLength == -1)
    {
       if (errno == EWOULDBLOCK)
         return 0;

       return -1;
    }

    return sentLength;
#endif
}

int
enet_socket_receive (ENetSocket socket,
                     ENetAddress * address,
                     ENetBuffer * buffers,
                     size_t bufferCount)
{
#ifdef __3DS__
    struct sockaddr_in sin = { 0 };
    socklen_t addressLength = sizeof (sin);
    /* Reserve an extra byte for datagrams so oversized packets cannot be
       delivered as apparently valid truncated data. SOC has no MSG_TRUNC. */
    enet_uint8 packet [8192];
    size_t size = enet_3ds_buffer_size (buffers, bufferCount);
    size_t capacity = size + (address != NULL);
    int direct = address == NULL && bufferCount == 1;
    enet_uint8 * data;
    int recvLength;

    if (size == 0)
      return 0;
    data = direct ? buffers [0].data : capacity <= sizeof (packet) ? packet : enet_malloc (capacity);
    if (data == NULL)
      return -1;
    recvLength = recvfrom (socket, data, capacity, 0,
                          address != NULL ? (struct sockaddr *) & sin : NULL,
                          address != NULL ? & addressLength : NULL);
    if (recvLength < 0)
      recvLength = errno == EWOULDBLOCK || errno == EAGAIN ? 0 : -1;
    if (recvLength > 0 && (size_t) recvLength > size)
      recvLength = -1;
    if (recvLength > 0 && !direct)
    {
        size_t i, offset = 0;
        for (i = 0; i < bufferCount && offset < (size_t) recvLength; ++i)
        {
            size_t remaining = (size_t) recvLength - offset;
            size_t length = remaining < buffers [i].dataLength ? remaining : buffers [i].dataLength;
            memcpy (buffers [i].data, data + offset, length);
            offset += length;
        }
        if (address != NULL)
        {
            address -> host = sin.sin_addr.s_addr;
            address -> port = ENET_NET_TO_HOST_16 (sin.sin_port);
        }
    }
    if (!direct && data != packet)
      enet_free (data);
    return recvLength;
#else
    struct msghdr msgHdr;
    struct sockaddr_in sin;
    int recvLength;

    memset (& msgHdr, 0, sizeof (struct msghdr));

    if (address != NULL)
    {
        msgHdr.msg_name = & sin;
        msgHdr.msg_namelen = sizeof (struct sockaddr_in);
    }

    msgHdr.msg_iov = (struct iovec *) buffers;
    msgHdr.msg_iovlen = bufferCount;

    recvLength = recvmsg (socket, & msgHdr, MSG_NOSIGNAL);

    if (recvLength == -1)
    {
       if (errno == EWOULDBLOCK)
         return 0;

       return -1;
    }

#ifdef HAS_MSGHDR_FLAGS
    if (msgHdr.msg_flags & MSG_TRUNC)
      return -1;
#endif

    if (address != NULL)
    {
        address -> host = (enet_uint32) sin.sin_addr.s_addr;
        address -> port = ENET_NET_TO_HOST_16 (sin.sin_port);
    }

    return recvLength;
#endif
}

int
enet_socketset_select (ENetSocket maxSocket, ENetSocketSet * readSet, ENetSocketSet * writeSet, enet_uint32 timeout)
{
    struct timeval timeVal;

    timeVal.tv_sec = timeout / 1000;
    timeVal.tv_usec = (timeout % 1000) * 1000;

    return select (maxSocket + 1, readSet, writeSet, NULL, & timeVal);
}

int
enet_socket_wait (ENetSocket socket, enet_uint32 * condition, enet_uint32 timeout)
{
#if defined(HAS_POLL) || defined(__3DS__)
    struct pollfd pollSocket;
    int pollCount;
#ifdef __3DS__
    const enet_uint32 requested = * condition;
#endif

    pollSocket.fd = socket;
    pollSocket.events = 0;

    if (* condition & ENET_SOCKET_WAIT_SEND)
      pollSocket.events |= POLLOUT;

    if (* condition & ENET_SOCKET_WAIT_RECEIVE)
      pollSocket.events |= POLLIN;

    pollCount = poll (& pollSocket, 1, timeout);

    if (pollCount < 0)
      return -1;

    * condition = ENET_SOCKET_WAIT_NONE;

    if (pollCount == 0)
      return 0;

#ifdef __3DS__
    if (pollSocket.revents & POLLNVAL)
      return -1;
    if (pollSocket.revents & (POLLHUP | POLLERR))
      * condition |= requested;
#endif

    if (pollSocket.revents & POLLOUT)
      * condition |= ENET_SOCKET_WAIT_SEND;

    if (pollSocket.revents & POLLIN)
      * condition |= ENET_SOCKET_WAIT_RECEIVE;

    return 0;
#else
    fd_set readSet, writeSet;
    struct timeval timeVal;
    int selectCount;

    timeVal.tv_sec = timeout / 1000;
    timeVal.tv_usec = (timeout % 1000) * 1000;

    FD_ZERO (& readSet);
    FD_ZERO (& writeSet);

    if (* condition & ENET_SOCKET_WAIT_SEND)
      FD_SET (socket, & writeSet);

    if (* condition & ENET_SOCKET_WAIT_RECEIVE)
      FD_SET (socket, & readSet);

    selectCount = select (socket + 1, & readSet, & writeSet, NULL, & timeVal);

    if (selectCount < 0)
      return -1;

    * condition = ENET_SOCKET_WAIT_NONE;

    if (selectCount == 0)
      return 0;

    if (FD_ISSET (socket, & writeSet))
      * condition |= ENET_SOCKET_WAIT_SEND;

    if (FD_ISSET (socket, & readSet))
      * condition |= ENET_SOCKET_WAIT_RECEIVE;

    return 0;
#endif
}

#endif

