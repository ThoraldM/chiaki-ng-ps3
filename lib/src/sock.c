// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include <chiaki/sock.h>
#ifdef __PS3__
#include <net/socket.h>
#else
#include <fcntl.h>
#endif

CHIAKI_EXPORT ChiakiErrorCode chiaki_socket_set_nonblock(chiaki_socket_t sock, bool nonblock)
{
#ifdef _WIN32
	u_long nbio = nonblock ? 1 : 0;
	if(ioctlsocket(sock, FIONBIO, &nbio) != NO_ERROR)
		return CHIAKI_ERR_UNKNOWN;
#elif defined(__PS3__)
	int nbio = nonblock ? 1 : 0;
	if(setsockopt(sock, SOL_SOCKET, SO_NBIO, &nbio, sizeof(nbio)) < 0)
		return CHIAKI_ERR_UNKNOWN;
#else
	int flags = fcntl(sock, F_GETFL, 0);
	if(flags == -1)
		return CHIAKI_ERR_UNKNOWN;
	flags = nonblock ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
	if(fcntl(sock, F_SETFL, flags) == -1)
		return CHIAKI_ERR_UNKNOWN;
#endif
	return CHIAKI_ERR_SUCCESS;
}