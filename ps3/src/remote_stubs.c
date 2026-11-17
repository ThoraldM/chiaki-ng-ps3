// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
// PS3 port: stubs for features that need PSN/internet (hole punching, AIA
// certificate recovery). The PS3 build supports LAN streaming only.

#include <netinet/in.h>
#include <chiaki/common.h>
#include <chiaki/remote/holepunch.h>
#include <string.h>

ChiakiErrorCode chiaki_aia_init(void) { return CHIAKI_ERR_SUCCESS; }
void chiaki_aia_fini(void) {}

ChiakiHolepunchRegistInfo chiaki_get_regist_info(ChiakiHolepunchSession session)
{
	(void)session;
	ChiakiHolepunchRegistInfo info;
	memset(&info, 0, sizeof(info));
	return info;
}

chiaki_socket_t *chiaki_get_holepunch_sock(ChiakiHolepunchSession session, ChiakiHolepunchPortType type)
{
	(void)session; (void)type;
	return NULL;
}

ChiakiErrorCode holepunch_session_create_offer(ChiakiHolepunchSession session)
{
	(void)session;
	return CHIAKI_ERR_INVALID_DATA;
}

ChiakiErrorCode chiaki_holepunch_session_punch_hole(ChiakiHolepunchSession session, ChiakiHolepunchPortType port_type)
{
	(void)session; (void)port_type;
	return CHIAKI_ERR_INVALID_DATA;
}

void chiaki_holepunch_session_fini(ChiakiHolepunchSession session) { (void)session; }

void chiaki_get_ps_selected_addr(ChiakiHolepunchSession session, char *ps_ip)
{
	(void)session;
	if(ps_ip)
		ps_ip[0] = '\0';
}

uint16_t chiaki_get_ps_ctrl_port(ChiakiHolepunchSession session)
{
	(void)session;
	return 0;
}
