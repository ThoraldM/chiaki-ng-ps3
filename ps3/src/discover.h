// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
// Find a PS4/PS5 on the local network and wake it from rest mode.
#ifndef CHIAKI_PS3_DISCOVER_H
#define CHIAKI_PS3_DISCOVER_H

#include <chiaki/discovery.h>
#include <chiaki/log.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct
{
	char addr[64];
	char name[64];
	char id[32];   // host id = MAC address in hex
	ChiakiDiscoveryHostState state;
	ChiakiTarget target;
} FoundHost;

// Searches for up to timeout_ms for a console of the given type. Picks the one
// whose IP equals want_addr or whose host id equals want_id (case-insensitive);
// with neither set, the first that answers. tick() is called about every 16 ms
// so the caller can keep the screen and system callbacks running; it returns
// false to cancel the search.
bool discover_console(ChiakiLog *log, bool ps5, const char *want_addr, const char *want_id, uint32_t timeout_ms,
		bool (*tick)(void), FoundHost *out);

// Low-level helpers for a caller running its own discovery thread (the menu).
// PS4 and PS5 listen on different ports with different protocol versions.
void discover_send_search(ChiakiDiscovery *discovery, const char *addr, bool ps5);
// Directed broadcast address of the PS3's subnet, e.g. 192.168.1.255.
bool discover_subnet_broadcast(char *out, size_t out_size);

// regist_key is the 16 byte key from registration (an 8 character hex string).
bool discover_wakeup(ChiakiLog *log, const char *addr, const char *regist_key, bool ps5);

#endif
