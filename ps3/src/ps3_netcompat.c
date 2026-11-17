// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
// PS3 port: minimal IPv4-only getaddrinfo()/getnameinfo() on top of PSL1GHT.

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <arpa/inet.h>
#include <net/netdb.h>

int getaddrinfo(const char *node, const char *service, const struct addrinfo *hints, struct addrinfo **res)
{
	if(!node || !res)
		return EAI_NONAME;
	if(hints && hints->ai_family != AF_UNSPEC && hints->ai_family != AF_INET)
		return EAI_FAMILY;

	struct in_addr addr;
	if(!inet_aton(node, &addr))
	{
		if(hints && (hints->ai_flags & AI_NUMERICHOST))
			return EAI_NONAME;
		struct hostent *he = gethostbyname(node);
		if(!he || he->h_addrtype != AF_INET || !he->h_addr_list || !he->h_addr_list[0])
			return EAI_NONAME;
		memcpy(&addr, he->h_addr_list[0], sizeof(addr));
	}

	struct addrinfo *ai = calloc(1, sizeof(struct addrinfo) + sizeof(struct sockaddr_in));
	if(!ai)
		return EAI_MEMORY;
	struct sockaddr_in *sin = (struct sockaddr_in *)(ai + 1);
	sin->sin_family = AF_INET;
	sin->sin_addr = addr;
	sin->sin_port = service ? htons((uint16_t)atoi(service)) : 0;

	ai->ai_family = AF_INET;
	ai->ai_socktype = hints ? hints->ai_socktype : 0;
	ai->ai_protocol = hints ? hints->ai_protocol : 0;
	ai->ai_addrlen = sizeof(struct sockaddr_in);
	ai->ai_addr = (struct sockaddr *)sin;
	*res = ai;
	return 0;
}

void freeaddrinfo(struct addrinfo *res)
{
	while(res)
	{
		struct addrinfo *next = res->ai_next;
		free(res); // sockaddr is allocated in the same block
		res = next;
	}
}

int getnameinfo(const struct sockaddr *sa, socklen_t salen, char *host, socklen_t hostlen, char *serv, socklen_t servlen, int flags)
{
	(void)flags;
	if(!sa || sa->sa_family != AF_INET || salen < sizeof(struct sockaddr_in))
		return EAI_FAMILY;
	const struct sockaddr_in *sin = (const struct sockaddr_in *)sa;
	if(host && hostlen)
	{
		if(!inet_ntop(AF_INET, &sin->sin_addr, host, hostlen))
			return EAI_FAIL;
	}
	if(serv && servlen)
		snprintf(serv, servlen, "%u", (unsigned)ntohs(sin->sin_port));
	return 0;
}

const char *gai_strerror(int errcode)
{
	switch(errcode)
	{
		case 0: return "Success";
		case EAI_NONAME: return "Name does not resolve";
		case EAI_FAMILY: return "Address family not supported";
		case EAI_MEMORY: return "Out of memory";
		default: return "Address lookup failed";
	}
}
