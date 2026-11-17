// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include "discover.h"

#include <chiaki/session.h>
#include <chiaki/thread.h>
#include <chiaki/time.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <arpa/inet.h>
#include <net/netctl.h>

typedef struct
{
	ChiakiMutex mutex;
	const char *want_addr;
	const char *want_id;
	bool ps5;
	bool found;
	FoundHost host;
} SearchCtx;

static bool matches(SearchCtx *ctx, ChiakiDiscoveryHost *h)
{
	bool have_addr = ctx->want_addr && *ctx->want_addr;
	bool have_id = ctx->want_id && *ctx->want_id;
	if(have_addr && h->host_addr && !strcmp(ctx->want_addr, h->host_addr))
		return true;
	if(have_id && h->host_id && !strcasecmp(ctx->want_id, h->host_id))
		return true;
	return !have_addr && !have_id;
}

static void discovery_cb(ChiakiDiscoveryHost *h, void *user)
{
	SearchCtx *ctx = user;
	if(!h->host_addr || chiaki_discovery_host_is_ps5(h) != ctx->ps5)
		return;
	chiaki_mutex_lock(&ctx->mutex);
	if(!ctx->found && matches(ctx, h))
	{
		snprintf(ctx->host.addr, sizeof(ctx->host.addr), "%s", h->host_addr);
		snprintf(ctx->host.name, sizeof(ctx->host.name), "%s", h->host_name ? h->host_name : (ctx->ps5 ? "PS5" : "PS4"));
		snprintf(ctx->host.id, sizeof(ctx->host.id), "%s", h->host_id ? h->host_id : "");
		ctx->host.state = h->state;
		ctx->host.target = chiaki_discovery_host_system_version_target(h);
		ctx->found = true;
	}
	chiaki_mutex_unlock(&ctx->mutex);
}

void discover_send_search(ChiakiDiscovery *discovery, const char *addr_str, bool ps5)
{
	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(ps5 ? CHIAKI_DISCOVERY_PORT_PS5 : CHIAKI_DISCOVERY_PORT_PS4);
	if(!inet_aton(addr_str, &addr.sin_addr))
		return;
	ChiakiDiscoveryPacket packet;
	memset(&packet, 0, sizeof(packet));
	packet.cmd = CHIAKI_DISCOVERY_CMD_SRCH;
	packet.protocol_version = ps5 ? CHIAKI_DISCOVERY_PROTOCOL_VERSION_PS5 : CHIAKI_DISCOVERY_PROTOCOL_VERSION_PS4;
	chiaki_discovery_send(discovery, &packet, (struct sockaddr *)&addr, sizeof(addr));
}

// Directed broadcast address of the PS3's own subnet, e.g. 192.168.1.255.
bool discover_subnet_broadcast(char *out, size_t out_size)
{
	union net_ctl_info ip, mask;
	if(netCtlGetInfo(NET_CTL_INFO_IP_ADDRESS, &ip) != 0 || netCtlGetInfo(NET_CTL_INFO_NETMASK, &mask) != 0)
		return false;
	struct in_addr a, m;
	if(!inet_aton(ip.ip_address, &a) || !inet_aton(mask.netmask, &m))
		return false;
	struct in_addr b;
	b.s_addr = a.s_addr | ~m.s_addr;
	return inet_ntop(AF_INET, &b, out, out_size) != NULL;
}

bool discover_console(ChiakiLog *log, bool ps5, const char *want_addr, const char *want_id, uint32_t timeout_ms,
		bool (*tick)(void), FoundHost *out)
{
	ChiakiDiscovery discovery;
	if(chiaki_discovery_init(&discovery, log, AF_INET) != CHIAKI_ERR_SUCCESS)
		return false;

	SearchCtx ctx;
	memset(&ctx, 0, sizeof(ctx));
	chiaki_mutex_init(&ctx.mutex, false);
	ctx.want_addr = want_addr;
	ctx.want_id = want_id;
	ctx.ps5 = ps5;

	ChiakiDiscoveryThread thread;
	if(chiaki_discovery_thread_start(&thread, &discovery, discovery_cb, &ctx) != CHIAKI_ERR_SUCCESS)
	{
		chiaki_mutex_fini(&ctx.mutex);
		chiaki_discovery_fini(&discovery);
		return false;
	}

	char bcast[32];
	bool have_bcast = discover_subnet_broadcast(bcast, sizeof(bcast));
	CHIAKI_LOGI(log, "Searching for a %s (broadcast%s%s%s%s)", ps5 ? "PS5" : "PS4", have_bcast ? " " : "", have_bcast ? bcast : "",
			want_addr && *want_addr ? ", host " : "", want_addr && *want_addr ? want_addr : "");

	uint64_t start = chiaki_time_now_monotonic_ms();
	uint64_t last_send = 0;
	bool found = false;
	bool cancelled = false;
	while(!found && !cancelled && chiaki_time_now_monotonic_ms() - start < timeout_ms)
	{
		uint64_t now = chiaki_time_now_monotonic_ms();
		if(!last_send || now - last_send >= 1000)
		{
			last_send = now;
			discover_send_search(&discovery, "255.255.255.255", ps5);
			if(have_bcast)
				discover_send_search(&discovery, bcast, ps5);
			if(want_addr && *want_addr)
				discover_send_search(&discovery, want_addr, ps5);
		}
		if(tick)
			cancelled = !tick();
		chiaki_mutex_lock(&ctx.mutex);
		found = ctx.found;
		chiaki_mutex_unlock(&ctx.mutex);
	}

	chiaki_discovery_thread_stop(&thread);
	chiaki_discovery_fini(&discovery);
	chiaki_mutex_fini(&ctx.mutex);

	if(found)
	{
		*out = ctx.host;
		CHIAKI_LOGI(log, "Found %s \"%s\" at %s (%s)", chiaki_target_is_ps5(out->target) ? "PS5" : "PS4",
				out->name, out->addr, chiaki_discovery_host_state_string(out->state));
	}
	else
		CHIAKI_LOGW(log, "No %s answered within %u ms", ps5 ? "PS5" : "PS4", (unsigned)timeout_ms);
	return found;
}

bool discover_wakeup(ChiakiLog *log, const char *addr, const char *regist_key, bool ps5)
{
	// The wakeup credential is the regist key's text (up to 8 hex digits) as a number.
	char key[CHIAKI_SESSION_AUTH_SIZE + 1];
	memcpy(key, regist_key, CHIAKI_SESSION_AUTH_SIZE);
	key[CHIAKI_SESSION_AUTH_SIZE] = '\0';
	char *end = NULL;
	uint64_t credential = strtoull(key, &end, 16);
	if(strlen(key) > 8 || !end || *end != '\0')
	{
		CHIAKI_LOGE(log, "Regist key is not usable for wakeup");
		return false;
	}
	CHIAKI_LOGI(log, "Sending wakeup to %s", addr);
	return chiaki_discovery_wakeup(log, NULL, addr, credential, ps5) == CHIAKI_ERR_SUCCESS;
}
