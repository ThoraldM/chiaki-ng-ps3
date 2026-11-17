// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include <chiaki/stoppipe.h>
#include <chiaki/sock.h>

#ifndef __PS3__
#include <fcntl.h>
#endif

#ifdef _WIN32
#include <ws2tcpip.h>
#elif defined(__PS3__)
#include <errno.h>
#include <unistd.h>
#include <net/socket.h>
#include <net/poll.h>
// Implemented by the app (ps3/src/main.c): logs a network error with its errno.
void chiaki_ps3_net_error(const char *what, int err, int fd);
#include <netinet/in.h>
#include <arpa/inet.h>
#else
#include <unistd.h>
#include <sys/socket.h>
#include <sys/select.h>
#endif

CHIAKI_EXPORT ChiakiErrorCode chiaki_stop_pipe_init(ChiakiStopPipe *stop_pipe)
{
#ifdef _WIN32
	stop_pipe->event = WSACreateEvent();
	if(stop_pipe->event == WSA_INVALID_EVENT)
		return CHIAKI_ERR_UNKNOWN;
#elif defined(__SWITCH__) || defined(__PS3__)
	// currently pipe or socketpare are not available on switch/ps3
	// use a custom udp socket as pipe

	// struct sockaddr_in addr;
	int addr_size = sizeof(stop_pipe->addr);

	stop_pipe->fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if(stop_pipe->fd < 0)
		return CHIAKI_ERR_UNKNOWN;
#ifdef __PS3__
	stop_pipe->stopped = 0;
#endif
	stop_pipe->addr.sin_family = AF_INET;
	// bind to localhost
	stop_pipe->addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	// use a random port (dedicate one socket per object)
	stop_pipe->addr.sin_port = htons(0);
	// bind on localhost
	bind(stop_pipe->fd, (struct sockaddr *) &stop_pipe->addr, addr_size);
	// listen
	getsockname(stop_pipe->fd, (struct sockaddr *) &stop_pipe->addr, (socklen_t *)&addr_size);
#ifdef __PS3__
	int nbio = 1;
	int r = setsockopt(stop_pipe->fd, SOL_SOCKET, SO_NBIO, &nbio, sizeof(nbio));
#else
	int r = fcntl(stop_pipe->fd, F_SETFL, O_NONBLOCK);
#endif
	if(r == -1)
	{
		close(stop_pipe->fd);
		return CHIAKI_ERR_UNKNOWN;
	}
#else
	int r = pipe(stop_pipe->fds);
	if(r < 0)
		return CHIAKI_ERR_UNKNOWN;
	r = fcntl(stop_pipe->fds[0], F_SETFL, O_NONBLOCK);
	if(r == -1)
	{
		close(stop_pipe->fds[0]);
		close(stop_pipe->fds[1]);
		return CHIAKI_ERR_UNKNOWN;
	}
#endif
	return CHIAKI_ERR_SUCCESS;
}

CHIAKI_EXPORT void chiaki_stop_pipe_fini(ChiakiStopPipe *stop_pipe)
{
#ifdef _WIN32
	WSACloseEvent(stop_pipe->event);
#elif defined(__SWITCH__) || defined(__PS3__)
	close(stop_pipe->fd);
#else
	close(stop_pipe->fds[0]);
	close(stop_pipe->fds[1]);
#endif
}

CHIAKI_EXPORT void chiaki_stop_pipe_stop(ChiakiStopPipe *stop_pipe)
{
#ifdef _WIN32
	WSASetEvent(stop_pipe->event);
#elif defined(__SWITCH__) || defined(__PS3__)
#ifdef __PS3__
	stop_pipe->stopped = 1;
	__sync_synchronize();
#endif
	// send to local socket (FIXME MSG_CONFIRM)
	sendto(stop_pipe->fd, "\x00", 1, 0,
		(struct sockaddr*)&stop_pipe->addr, sizeof(struct sockaddr_in));
#else
	write(stop_pipe->fds[1], "\x00", 1);
#endif
}

CHIAKI_EXPORT ChiakiErrorCode chiaki_stop_pipe_select_single(ChiakiStopPipe *stop_pipe, chiaki_socket_t fd, bool write, uint64_t timeout_ms)
{
#ifdef _WIN32
	WSAEVENT events[2];
	DWORD events_count = 1;
	events[0] = stop_pipe->event;

	if(!CHIAKI_SOCKET_IS_INVALID(fd))
	{
		events_count = 2;
		events[1] = WSACreateEvent();
		if(events[1] == WSA_INVALID_EVENT)
			return CHIAKI_ERR_UNKNOWN;
		WSAEventSelect(fd, events[1], write ? FD_WRITE : FD_READ);
	}

	DWORD r = WSAWaitForMultipleEvents(events_count, events, FALSE, timeout_ms == UINT64_MAX ? WSA_INFINITE : (DWORD)timeout_ms, FALSE);

	if(events_count == 2)
		WSACloseEvent(events[1]);

	switch(r)
	{
		case WSA_WAIT_EVENT_0:
			return CHIAKI_ERR_CANCELED;
		case WSA_WAIT_EVENT_0+1:
			return CHIAKI_ERR_SUCCESS;
		case WSA_WAIT_TIMEOUT:
			return CHIAKI_ERR_TIMEOUT;
		default:
			return CHIAKI_ERR_UNKNOWN;
	}
#elif defined(__PS3__)
	// PSL1GHT socket descriptors carry SOCKET_FD_MASK (0x40000000), which
	// makes FD_SET() write out of bounds, so use poll() instead of select().
	struct pollfd pfds[2];
	nfds_t nfds = 1;
	pfds[0].fd = stop_pipe->fd;
	pfds[0].events = POLLIN;
	pfds[0].revents = 0;
	if(!CHIAKI_SOCKET_IS_INVALID(fd))
	{
		pfds[1].fd = fd;
		pfds[1].events = write ? POLLOUT : POLLIN;
		pfds[1].revents = 0;
		nfds = 2;
	}
	// Wait in slices of at most 250 ms and look at the stopped flag in between.
	const uint64_t slice_ms = 250;
	uint64_t waited_ms = 0;
	int r;
	for(;;)
	{
		if(stop_pipe->stopped)
			return CHIAKI_ERR_CANCELED;
		uint64_t left_ms = timeout_ms == UINT64_MAX ? slice_ms : (timeout_ms > waited_ms ? timeout_ms - waited_ms : 0);
		int timeout = (int)(left_ms < slice_ms ? left_ms : slice_ms);
		do
		{
			r = poll(pfds, nfds, timeout);
		} while(r < 0 && errno == EINTR);
		if(r != 0)
			break; // an event, or an error
		waited_ms += (uint64_t)timeout;
		if(timeout_ms != UINT64_MAX && waited_ms >= timeout_ms)
			break;
	}

	if(r < 0)
	{
		int e = errno;
		chiaki_ps3_net_error("poll", e, nfds == 2 ? fd : -1);
		// lv2 may report a reset/refused connection as a poll() error; let the
		// caller's recv()/getpeername() turn it into a normal network error.
		if(nfds == 2 && (e == ECONNRESET || e == ECONNREFUSED || e == ECONNABORTED || e == ENOTCONN || e == EPIPE))
			return CHIAKI_ERR_SUCCESS;
		return CHIAKI_ERR_UNKNOWN;
	}
	if(pfds[0].revents & POLLIN)
		return CHIAKI_ERR_CANCELED;
	if(nfds == 2 && (pfds[1].revents & (write ? POLLOUT : POLLIN) || pfds[1].revents & (POLLERR | POLLHUP)))
		return CHIAKI_ERR_SUCCESS;
	return CHIAKI_ERR_TIMEOUT;
#else
	fd_set rfds;
	FD_ZERO(&rfds);
#if defined(__SWITCH__)
	// push udp local socket as fd
	int stop_fd = stop_pipe->fd;
#else
	int stop_fd = stop_pipe->fds[0];
#endif
	FD_SET(stop_fd, &rfds);
	int nfds = stop_fd;

	fd_set wfds;
	FD_ZERO(&wfds);
	if(!CHIAKI_SOCKET_IS_INVALID(fd))
	{
		FD_SET(fd, write ? &wfds : &rfds);
		if(fd > nfds)
			nfds = fd;
	}
	nfds++;

	struct timeval timeout_s;
	struct timeval *timeout = NULL;
	if(timeout_ms != UINT64_MAX)
	{
		timeout_s.tv_sec = timeout_ms / 1000;
		timeout_s.tv_usec = (timeout_ms % 1000) * 1000;
		timeout = &timeout_s;
	}

	int r;
	do
	{
		r = select(nfds, &rfds, write ? &wfds : NULL, NULL, timeout);
#ifdef _WIN32
	} while(r < 0 && WSAGetLastError() == WSAEINTR)
#else
	} while(r < 0 && errno == EINTR);
#endif

	if(r < 0)
		return CHIAKI_ERR_UNKNOWN;

	if(FD_ISSET(stop_fd, &rfds))
		return CHIAKI_ERR_CANCELED;

	if(!CHIAKI_SOCKET_IS_INVALID(fd) && FD_ISSET(fd, write ? &wfds : &rfds))
		return CHIAKI_ERR_SUCCESS;

	return CHIAKI_ERR_TIMEOUT;
#endif
}

CHIAKI_EXPORT ChiakiErrorCode chiaki_stop_pipe_connect(ChiakiStopPipe *stop_pipe, chiaki_socket_t fd, struct sockaddr *addr, size_t addrlen, uint64_t timeout_ms)
{
	int r = connect(fd, addr, (socklen_t)addrlen);
	if(r >= 0)
		return CHIAKI_ERR_SUCCESS;

	if(CHIAKI_SOCKET_EINPROGRESS)
	{
		ChiakiErrorCode err = chiaki_stop_pipe_select_single(stop_pipe, fd, true, timeout_ms);
		if(err != CHIAKI_ERR_SUCCESS)
			return err;
	}
	else
	{
#ifdef _WIN32
		int err = WSAGetLastError();
		if(err == WSAECONNREFUSED)
			return CHIAKI_ERR_CONNECTION_REFUSED;
		else
			return CHIAKI_ERR_NETWORK;
#else
		if(errno == ECONNREFUSED)
			return CHIAKI_ERR_CONNECTION_REFUSED;
		else
			return CHIAKI_ERR_NETWORK;
#endif
	}

	struct sockaddr_storage peer;
	socklen_t peerlen = sizeof(peer);
	if(getpeername(fd, (struct sockaddr *)(&peer), &peerlen) == 0)
		return CHIAKI_ERR_SUCCESS;

#ifdef _WIN32
	int err = WSAGetLastError();
	if(err != WSAENOTCONN)
		return CHIAKI_ERR_UNKNOWN;
#else
#ifdef __PS3__
	if(errno != ENOTCONN)
		chiaki_ps3_net_error("getpeername", errno, fd);
	if(errno == ECONNREFUSED)
		return CHIAKI_ERR_CONNECTION_REFUSED;
	if(errno == ECONNRESET || errno == ETIMEDOUT || errno == EHOSTUNREACH || errno == ENETUNREACH)
		return CHIAKI_ERR_NETWORK;
#endif
	if(errno != ENOTCONN)
		return CHIAKI_ERR_UNKNOWN;
#endif

#ifdef _WIN32
	int sockerr;
	socklen_t sockerr_sz = sizeof(sockerr);
	if(getsockopt(fd, SOL_SOCKET, SO_ERROR, (char*)(&sockerr), &sockerr_sz) < 0)
		return CHIAKI_ERR_UNKNOWN;
#else
	int sockerr;
	socklen_t sockerr_sz = sizeof(sockerr);
	if(getsockopt(fd, SOL_SOCKET, SO_ERROR, &sockerr, &sockerr_sz) < 0)
		return CHIAKI_ERR_UNKNOWN;
#endif

#ifdef _WIN32
	switch(sockerr)
	{
		case WSAETIMEDOUT:
			return CHIAKI_ERR_TIMEOUT;
		case WSAECONNREFUSED:
			return CHIAKI_ERR_CONNECTION_REFUSED;
		case WSAEHOSTDOWN:
			return CHIAKI_ERR_HOST_DOWN;
		case WSAEHOSTUNREACH:
			return CHIAKI_ERR_HOST_UNREACH;
		default:
			return CHIAKI_ERR_UNKNOWN;
	}
#else
#ifdef __PS3__
	chiaki_ps3_net_error("connect (SO_ERROR)", sockerr, fd);
	// SO_ERROR returns the raw lv2 (BSD numbered) code, not a newlib errno
	switch(sockerr)
	{
		case 60: return CHIAKI_ERR_TIMEOUT;
		case 61: return CHIAKI_ERR_CONNECTION_REFUSED;
		case 64: return CHIAKI_ERR_HOST_DOWN;
		case 65: return CHIAKI_ERR_HOST_UNREACH;
		default: break;
	}
#endif
	switch(sockerr)
	{
		case ETIMEDOUT:
			return CHIAKI_ERR_TIMEOUT;
		case ECONNREFUSED:
			return CHIAKI_ERR_CONNECTION_REFUSED;
		case EHOSTDOWN:
			return CHIAKI_ERR_HOST_DOWN;
		case EHOSTUNREACH:
			return CHIAKI_ERR_HOST_UNREACH;
		default:
			return CHIAKI_ERR_UNKNOWN;
	}
#endif
}

CHIAKI_EXPORT ChiakiErrorCode chiaki_stop_pipe_reset(ChiakiStopPipe *stop_pipe)
{
#ifdef _WIN32
	BOOL r = WSAResetEvent(stop_pipe->event);
	return r ? CHIAKI_ERR_SUCCESS : CHIAKI_ERR_UNKNOWN;
#elif defined(__SWITCH__)
	//FIXME
	uint8_t v;
	int r;
	while((r = read(stop_pipe->fd, &v, sizeof(v))) > 0);
	return r < 0 ? CHIAKI_ERR_UNKNOWN : CHIAKI_ERR_SUCCESS;
#elif defined(__PS3__)
	uint8_t v;
	stop_pipe->stopped = 0;
	__sync_synchronize();
	while(recv(stop_pipe->fd, &v, sizeof(v), 0) > 0);
	return CHIAKI_ERR_SUCCESS;
#else
	uint8_t v;
	int r;
	while((r = read(stop_pipe->fds[0], &v, sizeof(v))) > 0);
	return r < 0 ? CHIAKI_ERR_UNKNOWN : CHIAKI_ERR_SUCCESS;
#endif
}
