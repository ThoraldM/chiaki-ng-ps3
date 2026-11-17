// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
// PS3 port: force-included (-include) into every chiaki source file.
// Fills POSIX networking gaps in PSL1GHT. The PS3 build is IPv4 only, but the
// IPv6 types must exist so the shared code compiles.
#ifndef CHIAKI_PS3_COMPAT_H
#define CHIAKI_PS3_COMPAT_H

#ifdef __PS3__

#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/time.h>
#include <net/socket.h>
#include <netinet/in.h>
#include <net/netdb.h>

#ifndef INET6_ADDRSTRLEN
#define INET6_ADDRSTRLEN 46
#endif

struct in6_addr { uint8_t s6_addr[16]; };

struct sockaddr_in6
{
	uint8_t sin6_len;
	sa_family_t sin6_family;
	in_port_t sin6_port;
	uint32_t sin6_flowinfo;
	struct in6_addr sin6_addr;
	uint32_t sin6_scope_id;
};

struct sockaddr_storage
{
	uint8_t ss_len;
	sa_family_t ss_family;
	char __ss_pad[126 - sizeof(sa_family_t)];
} __attribute__((aligned(8)));

#define IN6ADDR_ANY_INIT {{0}}
static const struct in6_addr in6addr_any = IN6ADDR_ANY_INIT;

#define EAI_NONAME  -2
#define EAI_FAIL    -4
#define EAI_FAMILY  -6
#define EAI_MEMORY  -10

#ifdef __cplusplus
extern "C" {
#endif
int getaddrinfo(const char *node, const char *service, const struct addrinfo *hints, struct addrinfo **res);
void freeaddrinfo(struct addrinfo *res);
int getnameinfo(const struct sockaddr *sa, socklen_t salen, char *host, socklen_t hostlen, char *serv, socklen_t servlen, int flags);
const char *gai_strerror(int errcode);
#ifdef __cplusplus
}
#endif

#endif // __PS3__
#endif // CHIAKI_PS3_COMPAT_H
