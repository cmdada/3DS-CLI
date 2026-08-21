#ifndef CORE_PLAT_SOCK_H
#define CORE_PLAT_SOCK_H

/* BSD sockets, spelled the same everywhere.
 *
 * The 3DS, Wii U and Switch all expose the standard names through their libc,
 * so this header is a passthrough there. libogc is the exception: its stack
 * prefixes every call net_* and has no fcntl on sockets, so the Wii and
 * GameCube backend defines PLAT_SOCK_OGC and the mapping below applies.
 */

#ifdef PLAT_SOCK_OGC

#include <network.h>

#define socket(a, b, c)         net_socket(a, b, c)
#define bind(a, b, c)           net_bind(a, b, c)
#define listen(a, b)            net_listen(a, b)
#define accept(a, b, c)         net_accept(a, b, c)
#define connect(a, b, c)        net_connect(a, b, c)
#define send(a, b, c, d)        net_send(a, b, c, d)
#define sendto(a, b, c, d, e, f) net_sendto(a, b, c, d, e, f)
#define recv(a, b, c, d)        net_recv(a, b, c, d)
#define recvfrom(a, b, c, d, e, f) net_recvfrom(a, b, c, d, e, f)
#define setsockopt(a, b, c, d, e)  net_setsockopt(a, b, c, d, e)
#define getsockopt(a, b, c, d, e)  net_getsockopt(a, b, c, d, e)
#define select(a, b, c, d, e)   net_select(a, b, c, d, e)
#define closesocket(fd)         net_close(fd)

/* libogc has no socket fcntl; non-blocking is a dedicated ioctl instead.
   Only ever used to set O_NONBLOCK, so that is all this covers. */
#define PLAT_SOCK_SET_NONBLOCK(fd) do {                 \
    u32 _nb = 1;                                        \
    net_ioctl((fd), FIONBIO, &_nb);                     \
  } while (0)

#else

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/select.h>
#include <fcntl.h>
#include <unistd.h>

#define closesocket(fd)         close(fd)

#define PLAT_SOCK_SET_NONBLOCK(fd) \
    fcntl((fd), F_SETFL, fcntl((fd), F_GETFL, 0) | O_NONBLOCK)

#endif

#endif /* CORE_PLAT_SOCK_H */
