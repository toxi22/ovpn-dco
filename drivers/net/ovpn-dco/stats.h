/* SPDX-License-Identifier: GPL-2.0 */
/*  OpenVPN data channel accelerator
 *
 *  Copyright (C) 2020-2022 OpenVPN, Inc.
 *
 *  Author:	James Yonan <james@openvpn.net>
 *		Antonio Quartulli <antonio@openvpn.net>
 *		Lev Stipakov <lev@openvpn.net>
 */

#ifndef _NET_OVPN_DCO_OVPNSTATS_H_
#define _NET_OVPN_DCO_OVPNSTATS_H_

#include <linux/jiffies.h>
#include <linux/u64_stats_sync.h>

struct ovpn_struct;

/* per-peer stats, measured on transport layer */

/* one stat */
struct ovpn_peer_stat {
	atomic64_t bytes;
	atomic_t packets;
};

/* rx and tx stats, enabled by notify_per != 0 or period != 0 */
struct ovpn_peer_stats {
	struct ovpn_peer_stat rx;
	struct ovpn_peer_stat tx;
};

/* struct for OVPN_ERR_STATS */

struct ovpn_err_stat {
	unsigned int category;
	int errcode;
	u64 count;
};

struct ovpn_err_stats {
	/* total stats, returned by kovpn */
	unsigned int total_stats;
	/* number of stats dimensioned below */
	unsigned int n_stats;
	struct ovpn_err_stat stats[];
};

void ovpn_peer_stats_init(struct ovpn_peer_stats *ps);

#endif /* _NET_OVPN_DCO_OVPNSTATS_H_ */
