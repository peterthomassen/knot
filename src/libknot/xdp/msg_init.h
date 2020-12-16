/*  Copyright (C) 2021 CZ.NIC, z.s.p.o. <knot-dns@labs.nic.cz>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#pragma once

#include <stdbool.h>
#include <string.h>

#include "libknot/xdp/msg.h"

inline static bool empty_msg(const knot_xdp_msg_t *msg)
{
	const unsigned tcp_flags = KNOT_XDP_MSG_SYN | KNOT_XDP_MSG_ACK |
	                           KNOT_XDP_MSG_FIN | KNOT_XDP_MSG_RST;

	return (msg->payload.iov_len == 0 && !(msg->flags & tcp_flags));
}

// FIXME do we care for better random?
inline static uint32_t rnd_uint32(void)
{
	uint32_t res = rand() & 0xffff;
	res <<= 16;
	res |= rand() & 0xffff;
	return res;
}

inline static void msg_init_base(knot_xdp_msg_t *msg, knot_xdp_msg_flag_t flags)
{
	memset(msg, 0, sizeof(*msg));

	msg->flags = flags;
}

inline static void msg_init(knot_xdp_msg_t *msg, knot_xdp_msg_flag_t flags)
{
	msg_init_base(msg, flags);

	if (flags & KNOT_XDP_MSG_TCP) {
		msg->ackno = 0;
		msg->seqno = rnd_uint32();
	}
}

inline static void msg_init_reply(knot_xdp_msg_t *msg, const knot_xdp_msg_t *query)
{
	msg_init_base(msg, query->flags & (KNOT_XDP_MSG_IPV6 | KNOT_XDP_MSG_TCP));

	memcpy(msg->eth_from, query->eth_to,   ETH_ALEN);
	memcpy(msg->eth_to,   query->eth_from, ETH_ALEN);

	memcpy(&msg->ip_from, &query->ip_to,   sizeof(msg->ip_from));
	memcpy(&msg->ip_to,   &query->ip_from, sizeof(msg->ip_to));

	if (msg->flags & KNOT_XDP_MSG_TCP) {
		msg->ackno = query->seqno;
		msg->ackno += query->payload.iov_len;
		if (query->flags & (KNOT_XDP_MSG_SYN | KNOT_XDP_MSG_FIN)) {
			msg->ackno++;
		}
		msg->seqno = query->ackno;
		if (msg->seqno == 0) {
			msg->seqno = rnd_uint32();
		}
	}
}
