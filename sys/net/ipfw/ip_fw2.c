/*
 * Copyright (c) 2002 Luigi Rizzo, Universita` di Pisa
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * $FreeBSD: src/sys/netinet/ip_fw2.c,v 1.6.2.12 2003/04/08 10:42:32 maxim Exp $
 */

/*
 * Implement IP packet firewall (new version)
 */

#include "opt_ipfw.h"
#include "opt_inet.h"
#ifndef INET
#error IPFIREWALL requires INET.
#endif /* INET */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/malloc.h>
#include <sys/mbuf.h>
#include <sys/kernel.h>
#include <sys/proc.h>
#include <sys/socket.h>
#include <sys/socketvar.h>
#include <sys/sysctl.h>
#include <sys/syslog.h>
#include <sys/ucred.h>
#include <sys/in_cksum.h>
#include <sys/limits.h>
#include <sys/lock.h>
#include <sys/tree.h>

#include <net/if.h>
#include <net/route.h>
#include <net/pfil.h>
#include <net/dummynet/ip_dummynet.h>

#include <sys/thread2.h>
#include <sys/mplock2.h>
#include <net/netmsg2.h>

#include <netinet/in.h>
#include <netinet/in_systm.h>
#include <netinet/in_var.h>
#include <netinet/in_pcb.h>
#include <netinet/ip.h>
#include <netinet/ip_var.h>
#include <netinet/ip_icmp.h>
#include <netinet/tcp.h>
#include <netinet/tcp_seq.h>
#include <netinet/tcp_timer.h>
#include <netinet/tcp_var.h>
#include <netinet/tcpip.h>
#include <netinet/udp.h>
#include <netinet/udp_var.h>
#include <netinet/ip_divert.h>
#include <netinet/if_ether.h> /* XXX for ETHERTYPE_IP */

#include <net/ipfw/ip_fw2.h>

#ifdef IPFIREWALL_DEBUG
#define DPRINTF(fmt, ...) \
do { \
	if (fw_debug > 0) \
		kprintf(fmt, __VA_ARGS__); \
} while (0)
#else
#define DPRINTF(fmt, ...)	((void)0)
#endif

/*
 * Description about per-CPU rule duplication:
 *
 * Module loading/unloading and all ioctl operations are serialized
 * by netisr0, so we don't have any ordering or locking problems.
 *
 * Following graph shows how operation on per-CPU rule list is
 * performed [2 CPU case]:
 *
 *   CPU0                 CPU1
 *
 * netisr0 <------------------------------------+
 *  domsg                                       |
 *    :                                         |
 *    :(delete/add...)                          |
 *    :                                         |
 *    :         netmsg                          | netmsg
 *  forwardmsg---------->netisr1                |
 *                          :                   |
 *                          :(delete/add...)    |
 *                          :                   |
 *                          :                   |
 *                        replymsg--------------+
 *
 *
 *
 * Rule structure [2 CPU case]
 *
 *    CPU0               CPU1
 *
 * layer3_chain       layer3_chain
 *     |                  |
 *     V                  V
 * +-------+ sibling  +-------+ sibling
 * | rule1 |--------->| rule1 |--------->NULL
 * +-------+          +-------+
 *     |                  |
 *     |next              |next
 *     V                  V
 * +-------+ sibling  +-------+ sibling
 * | rule2 |--------->| rule2 |--------->NULL
 * +-------+          +-------+
 *
 * ip_fw.sibling:
 * 1) Ease statistics calculation during IP_FW_GET.  We only need to
 *    iterate layer3_chain in netisr0; the current rule's duplication
 *    to the other CPUs could safely be read-only accessed through
 *    ip_fw.sibling.
 * 2) Accelerate rule insertion and deletion, e.g. rule insertion:
 *    a) In netisr0 rule3 is determined to be inserted between rule1
 *       and rule2.  To make this decision we need to iterate the
 *       layer3_chain in netisr0.  The netmsg, which is used to insert
 *       the rule, will contain rule1 in netisr0 as prev_rule and rule2
 *       in netisr0 as next_rule.
 *    b) After the insertion in netisr0 is done, we will move on to
 *       netisr1.  But instead of relocating the rule3's position in
 *       netisr1 by iterating the layer3_chain in netisr1, we set the
 *       netmsg's prev_rule to rule1->sibling and next_rule to
 *       rule2->sibling before the netmsg is forwarded to netisr1 from
 *       netisr0.
 */

/*
 * Description of states and tracks.
 *
 * Both states and tracks are stored in per-cpu RB trees instead of
 * per-cpu hash tables to avoid the worst case hash degeneration.
 *
 * The lifetimes of states and tracks are regulated by dyn_*_lifetime,
 * measured in seconds and depending on the flags.
 *
 * When a packet is received, its address fields are first masked with
 * the mask defined for the rule, then matched against the entries in
 * the per-cpu state RB tree.  States are generated by 'keep-state'
 * and 'limit' options.
 *
 * The max number of states is ipfw_state_max.  When we reach the
 * maximum number of states we do not create anymore.  This is done to
 * avoid consuming too much memory, but also too much time when
 * searching on each packet.
 *
 * Each state holds a pointer to the parent ipfw rule of the current
 * CPU so we know what action to perform.  States are removed when the
 * parent rule is deleted.  XXX we should make them survive.
 *
 * There are some limitations with states -- we do not obey the
 * 'randomized match', and we do not do multiple passes through the
 * firewall.  XXX check the latter!!!
 *
 * States grow independently on each CPU, e.g. 2 CPU case:
 *
 *        CPU0                     CPU1
 * ...................      ...................
 * :  state RB tree  :      :  state RB tree  :
 * :                 :      :                 :
 * : state1   state2 :      :      state3     :
 * :     |    |      :      :        |        :
 * :.....|....|......:      :........|........:
 *       |    |                      |
 *       |    |                      |st_rule
 *       |    |                      |
 *       V    V                      V
 *     +-------+                 +-------+
 *     | rule1 |                 | rule1 |
 *     +-------+                 +-------+
 *
 * Tracks are used to enforce limits on the number of sessions.  Tracks
 * are generated by 'limit' option.
 *
 * The max number of tracks is ipfw_track_max.  When we reach the
 * maximum number of tracks we do not create anymore.  This is done to
 * avoid consuming too much memory.
 *
 * Tracks are organized into two layers, track counter RB tree is
 * shared between CPUs, track RB tree is per-cpu.  States generated by
 * 'limit' option are linked to the track in addition to the per-cpu
 * state RB tree; mainly to ease expiration.  e.g. 2 CPU case:
 *
 *             ..............................
 *             :    track counter RB tree   :
 *             :                            :
 *             :        +-----------+       :
 *             :        |  trkcnt1  |       :
 *             :        |           |       :
 *             :      +--->counter<----+    :
 *             :      | |           |  |    :
 *             :      | +-----------+  |    :
 *             :......|................|....:
 *                    |                |
 *        CPU0        |                |         CPU1
 * .................  |t_count         |  .................
 * : track RB tree :  |                |  : track RB tree :
 * :               :  |                |  :               :
 * : +-->track1-------+                +--------track2    :
 * : |     A       :                      :               :
 * : |     |       :                      :               :
 * :.|.....|.......:                      :...............:
 *   |     +----------------+
 *   | .................... |
 *   | :   state RB tree  : |st_track
 *   | :                  : |
 *   +---state1    state2---+
 *     :     |       |    :
 *     :.....|.......|....:
 *           |       |
 *           |       |st_rule
 *           V       V
 *         +----------+
 *         |   rule1  |
 *         +----------+
 */

#define IPFW_AUTOINC_STEP_MIN	1
#define IPFW_AUTOINC_STEP_MAX	1000
#define IPFW_AUTOINC_STEP_DEF	100

#define	IPFW_DEFAULT_RULE	65535	/* rulenum for the default rule */
#define IPFW_DEFAULT_SET	31	/* set number for the default rule */

#define MATCH_REVERSE		0
#define MATCH_FORWARD		1
#define MATCH_NONE		2
#define MATCH_UNKNOWN		3

#define IPFW_STATE_TCPFLAGS	(TH_SYN | TH_FIN | TH_RST)
#define IPFW_STATE_TCPSTATES	(IPFW_STATE_TCPFLAGS |	\
				 (IPFW_STATE_TCPFLAGS << 8))

#define BOTH_SYN		(TH_SYN | (TH_SYN << 8))
#define BOTH_FIN		(TH_FIN | (TH_FIN << 8))
#define BOTH_RST		(TH_RST | (TH_RST << 8))
/* TH_ACK here means FIN was ACKed. */
#define BOTH_FINACK		(TH_ACK | (TH_ACK << 8))

#define IPFW_STATE_TCPCLOSED(s)	((s)->st_proto == IPPROTO_TCP &&	\
				 (((s)->st_state & BOTH_RST) ||		\
				  ((s)->st_state & BOTH_FINACK) == BOTH_FINACK))

#define O_ANCHOR		O_NOP

struct netmsg_ipfw {
	struct netmsg_base	base;
	const struct ipfw_ioc_rule *ioc_rule;
	struct ip_fw		*next_rule;
	struct ip_fw		*prev_rule;
	struct ip_fw		*sibling;
	uint32_t		rule_flags;
};

struct netmsg_del {
	struct netmsg_base	base;
	struct ip_fw		*start_rule;
	struct ip_fw		*prev_rule;
	uint16_t		rulenum;
	uint8_t			from_set;
	uint8_t			to_set;
};

struct netmsg_zent {
	struct netmsg_base	base;
	struct ip_fw		*start_rule;
	uint16_t		rulenum;
	uint16_t		log_only;
};

struct netmsg_cpstate {
	struct netmsg_base	base;
	struct ipfw_ioc_state	*ioc_state;
	int			state_cntmax;
	int			state_cnt;
};

struct ipfw_addrs {
	uint32_t		addr1;
	uint32_t		addr2;
};

struct ipfw_ports {
	uint16_t		port1;
	uint16_t		port2;
};

struct ipfw_key {
	union {
		struct ipfw_addrs addrs;
		uint64_t	value;
	} addr_u;
	union {
		struct ipfw_ports ports;
		uint32_t	value;
	} port_u;
	uint8_t			proto;
	uint8_t			swap;	/* IPFW_KEY_SWAP_ */
	uint16_t		rsvd2;
};

#define IPFW_KEY_SWAP_ADDRS	0x1
#define IPFW_KEY_SWAP_PORTS	0x2
#define IPFW_KEY_SWAP_ALL	(IPFW_KEY_SWAP_ADDRS | IPFW_KEY_SWAP_PORTS)

struct ipfw_trkcnt {
	RB_ENTRY(ipfw_trkcnt)	tc_rblink;
	struct ipfw_key		tc_key;
	uintptr_t		tc_ruleid;
	int			tc_refs;
	int			tc_count;
	time_t			tc_expire;	/* userland get-only */
	uint16_t		tc_rulenum;	/* userland get-only */
} __cachealign;

#define tc_addrs		tc_key.addr_u.value
#define tc_ports		tc_key.port_u.value
#define tc_proto		tc_key.proto
#define tc_saddr		tc_key.addr_u.addrs.addr1
#define tc_daddr		tc_key.addr_u.addrs.addr2
#define tc_sport		tc_key.port_u.ports.port1
#define tc_dport		tc_key.port_u.ports.port2

RB_HEAD(ipfw_trkcnt_tree, ipfw_trkcnt);

struct ipfw_state;

struct ipfw_track {
	RB_ENTRY(ipfw_track)	t_rblink;
	struct ipfw_key		t_key;
	struct ip_fw		*t_rule;
	time_t			t_lastexp;
	LIST_HEAD(, ipfw_state)	t_state_list;
	time_t			t_expire;
	volatile int		*t_count;
	struct ipfw_trkcnt	*t_trkcnt;
	TAILQ_ENTRY(ipfw_track)	t_link;
};

#define t_addrs			t_key.addr_u.value
#define t_ports			t_key.port_u.value
#define t_proto			t_key.proto
#define t_saddr			t_key.addr_u.addrs.addr1
#define t_daddr			t_key.addr_u.addrs.addr2
#define t_sport			t_key.port_u.ports.port1
#define t_dport			t_key.port_u.ports.port2

RB_HEAD(ipfw_track_tree, ipfw_track);
TAILQ_HEAD(ipfw_track_list, ipfw_track);

struct ipfw_state {
	RB_ENTRY(ipfw_state)	st_rblink;
	struct ipfw_key		st_key;

	time_t			st_expire;	/* expire time */
	struct ip_fw		*st_rule;

	uint64_t		st_pcnt;	/* packets */
	uint64_t		st_bcnt;	/* bytes */

	/*
	 * st_state:
	 * State of this rule, typically a combination of TCP flags.
	 *
	 * st_ack_fwd/st_ack_rev:
	 * Most recent ACKs in forward and reverse direction.  They
	 * are used to generate keepalives.
	 */
	uint32_t		st_state;
	uint32_t		st_ack_fwd;
	uint32_t		st_seq_fwd;
	uint32_t		st_ack_rev;
	uint32_t		st_seq_rev;

	uint16_t		st_flags;	/* IPFW_STATE_F_ */
	uint16_t		st_type;	/* O_KEEP_STATE/O_LIMIT */
	struct ipfw_track	*st_track;

	LIST_ENTRY(ipfw_state)	st_trklink;
	TAILQ_ENTRY(ipfw_state)	st_link;
};

#define st_addrs		st_key.addr_u.value
#define st_ports		st_key.port_u.value
#define st_proto		st_key.proto
#define st_swap			st_key.swap

#define IPFW_STATE_F_ACKFWD	0x0001
#define IPFW_STATE_F_SEQFWD	0x0002
#define IPFW_STATE_F_ACKREV	0x0004
#define IPFW_STATE_F_SEQREV	0x0008

TAILQ_HEAD(ipfw_state_list, ipfw_state);
RB_HEAD(ipfw_state_tree, ipfw_state);

struct ipfw_context {
	struct ip_fw		*ipfw_layer3_chain;	/* rules for layer3 */
	struct ip_fw		*ipfw_default_rule;	/* default rule */
	uint64_t		ipfw_norule_counter;	/* ipfw_log(NULL) stat*/

	/*
	 * ipfw_set_disable contains one bit per set value (0..31).
	 * If the bit is set, all rules with the corresponding set
	 * are disabled.  Set IPDW_DEFAULT_SET is reserved for the
	 * default rule and CANNOT be disabled.
	 */
	uint32_t		ipfw_set_disable;

	uint8_t			ipfw_flags;	/* IPFW_FLAG_ */

	struct ipfw_state_tree	ipfw_state_tree;
	struct ipfw_state_list	ipfw_state_list;
	int			ipfw_state_loosecnt;
	int			ipfw_state_cnt;

	union {
		struct ipfw_state state;
		struct ipfw_track track;
		struct ipfw_trkcnt trkcnt;
	} ipfw_tmpkey;

	struct ipfw_track_tree	ipfw_track_tree;
	struct ipfw_track_list	ipfw_track_list;
	struct ipfw_trkcnt	*ipfw_trkcnt_spare;

	struct callout		ipfw_stateto_ch;
	time_t			ipfw_state_lastexp;
	struct netmsg_base	ipfw_stateexp_nm;
	struct netmsg_base	ipfw_stateexp_more;
	struct ipfw_state	ipfw_stateexp_anch;

	struct callout		ipfw_trackto_ch;
	time_t			ipfw_track_lastexp;
	struct netmsg_base	ipfw_trackexp_nm;
	struct netmsg_base	ipfw_trackexp_more;
	struct ipfw_track	ipfw_trackexp_anch;

	struct callout		ipfw_keepalive_ch;
	struct netmsg_base	ipfw_keepalive_nm;
	struct netmsg_base	ipfw_keepalive_more;
	struct ipfw_state	ipfw_keepalive_anch;

	/*
	 * Statistics
	 */
	u_long			ipfw_sts_reap;
	u_long			ipfw_sts_reapfailed;
	u_long			ipfw_sts_overflow;
	u_long			ipfw_sts_nomem;
	u_long			ipfw_sts_tcprecycled;

	u_long			ipfw_tks_nomem;
	u_long			ipfw_tks_reap;
	u_long			ipfw_tks_reapfailed;
	u_long			ipfw_tks_overflow;
	u_long			ipfw_tks_cntnomem;
};

#define IPFW_FLAG_KEEPALIVE	0x01
#define IPFW_FLAG_STATEEXP	0x02
#define IPFW_FLAG_TRACKEXP	0x04
#define IPFW_FLAG_STATEREAP	0x08
#define IPFW_FLAG_TRACKREAP	0x10

#define ipfw_state_tmpkey	ipfw_tmpkey.state
#define ipfw_track_tmpkey	ipfw_tmpkey.track
#define ipfw_trkcnt_tmpkey	ipfw_tmpkey.trkcnt

struct ipfw_global {
	int			ipfw_state_loosecnt;	/* cache aligned */
	time_t			ipfw_state_globexp __cachealign;

	struct lwkt_token	ipfw_trkcnt_token __cachealign;
	struct ipfw_trkcnt_tree	ipfw_trkcnt_tree;
	int			ipfw_trkcnt_cnt;
	time_t			ipfw_track_globexp;

#ifdef KLD_MODULE
	/*
	 * Module can not be unloaded, if there are references to
	 * certains rules of ipfw(4), e.g. dummynet(4)
	 */
	int			ipfw_refcnt __cachealign;
#endif
} __cachealign;

static struct ipfw_context	*ipfw_ctx[MAXCPU];

MALLOC_DEFINE(M_IPFW, "IpFw/IpAcct", "IpFw/IpAcct chain's");

/*
 * Following two global variables are accessed and updated only
 * in netisr0.
 */
static uint32_t static_count;	/* # of static rules */
static uint32_t static_ioc_len;	/* bytes of static rules */

/*
 * If 1, then ipfw static rules are being flushed,
 * ipfw_chk() will skip to the default rule.
 */
static int ipfw_flushing;

static int fw_verbose;
static int verbose_limit;

static int fw_debug;
static int autoinc_step = IPFW_AUTOINC_STEP_DEF;

static int	ipfw_sysctl_enable(SYSCTL_HANDLER_ARGS);
static int	ipfw_sysctl_autoinc_step(SYSCTL_HANDLER_ARGS);

SYSCTL_NODE(_net_inet_ip, OID_AUTO, fw, CTLFLAG_RW, 0, "Firewall");
SYSCTL_NODE(_net_inet_ip_fw, OID_AUTO, stats, CTLFLAG_RW, 0,
    "Firewall statistics");

SYSCTL_PROC(_net_inet_ip_fw, OID_AUTO, enable, CTLTYPE_INT | CTLFLAG_RW,
    &fw_enable, 0, ipfw_sysctl_enable, "I", "Enable ipfw");
SYSCTL_PROC(_net_inet_ip_fw, OID_AUTO, autoinc_step, CTLTYPE_INT | CTLFLAG_RW,
    &autoinc_step, 0, ipfw_sysctl_autoinc_step, "I",
    "Rule number autincrement step");
SYSCTL_INT(_net_inet_ip_fw, OID_AUTO,one_pass,CTLFLAG_RW,
    &fw_one_pass, 0,
    "Only do a single pass through ipfw when using dummynet(4)");
SYSCTL_INT(_net_inet_ip_fw, OID_AUTO, debug, CTLFLAG_RW,
    &fw_debug, 0, "Enable printing of debug ip_fw statements");
SYSCTL_INT(_net_inet_ip_fw, OID_AUTO, verbose, CTLFLAG_RW,
    &fw_verbose, 0, "Log matches to ipfw rules");
SYSCTL_INT(_net_inet_ip_fw, OID_AUTO, verbose_limit, CTLFLAG_RW,
    &verbose_limit, 0, "Set upper limit of matches of ipfw rules logged");

static int	ipfw_sysctl_dyncnt(SYSCTL_HANDLER_ARGS);
static int	ipfw_sysctl_dynmax(SYSCTL_HANDLER_ARGS);
static int	ipfw_sysctl_statecnt(SYSCTL_HANDLER_ARGS);
static int	ipfw_sysctl_statemax(SYSCTL_HANDLER_ARGS);
static int	ipfw_sysctl_scancnt(SYSCTL_HANDLER_ARGS);
static int	ipfw_sysctl_stat(SYSCTL_HANDLER_ARGS);

/*
 * Timeouts for various events in handing states.
 *
 * NOTE:
 * 1 == 0~1 second.
 * 2 == 1~2 second(s).
 *
 * We use 2 seconds for FIN lifetime, so that the states will not be
 * ripped prematurely.
 */
static uint32_t dyn_ack_lifetime = 300;
static uint32_t dyn_syn_lifetime = 20;
static uint32_t dyn_finwait_lifetime = 20;
static uint32_t dyn_fin_lifetime = 2;
static uint32_t dyn_rst_lifetime = 2;
static uint32_t dyn_udp_lifetime = 10;
static uint32_t dyn_short_lifetime = 5;	/* used by tracks too */

/*
 * Keepalives are sent if dyn_keepalive is set. They are sent every
 * dyn_keepalive_period seconds, in the last dyn_keepalive_interval
 * seconds of lifetime of a rule.
 */
static uint32_t dyn_keepalive_interval = 20;
static uint32_t dyn_keepalive_period = 5;
static uint32_t dyn_keepalive = 1;	/* do send keepalives */

static struct ipfw_global	ipfw_gd;
static int	ipfw_state_loosecnt_updthr;
static int	ipfw_state_max = 4096;	/* max # of states */
static int	ipfw_track_max = 4096;	/* max # of tracks */

static int	ipfw_state_headroom;	/* setup at module load time */
static int	ipfw_state_reap_min = 8;
static int	ipfw_state_expire_max = 32;
static int	ipfw_state_scan_max = 256;
static int	ipfw_keepalive_max = 8;
static int	ipfw_track_reap_max = 4;
static int	ipfw_track_expire_max = 16;
static int	ipfw_track_scan_max = 128;

/* Compat */
SYSCTL_PROC(_net_inet_ip_fw, OID_AUTO, dyn_count,
    CTLTYPE_INT | CTLFLAG_RD, NULL, 0, ipfw_sysctl_dyncnt, "I",
    "Number of states and tracks");
SYSCTL_PROC(_net_inet_ip_fw, OID_AUTO, dyn_max,
    CTLTYPE_INT | CTLFLAG_RW, NULL, 0, ipfw_sysctl_dynmax, "I",
    "Max number of states and tracks");

SYSCTL_PROC(_net_inet_ip_fw, OID_AUTO, state_cnt,
    CTLTYPE_INT | CTLFLAG_RD, NULL, 0, ipfw_sysctl_statecnt, "I",
    "Number of states");
SYSCTL_PROC(_net_inet_ip_fw, OID_AUTO, state_max,
    CTLTYPE_INT | CTLFLAG_RW, NULL, 0, ipfw_sysctl_statemax, "I",
    "Max number of states");
SYSCTL_INT(_net_inet_ip_fw, OID_AUTO, state_headroom, CTLFLAG_RW,
    &ipfw_state_headroom, 0, "headroom for state reap");
SYSCTL_INT(_net_inet_ip_fw, OID_AUTO, track_cnt, CTLFLAG_RD,
    &ipfw_gd.ipfw_trkcnt_cnt, 0, "Number of tracks");
SYSCTL_INT(_net_inet_ip_fw, OID_AUTO, track_max, CTLFLAG_RW,
    &ipfw_track_max, 0, "Max number of tracks");
SYSCTL_INT(_net_inet_ip_fw, OID_AUTO, static_count, CTLFLAG_RD,
    &static_count, 0, "Number of static rules");
SYSCTL_INT(_net_inet_ip_fw, OID_AUTO, dyn_ack_lifetime, CTLFLAG_RW,
    &dyn_ack_lifetime, 0, "Lifetime of dyn. rules for acks");
SYSCTL_INT(_net_inet_ip_fw, OID_AUTO, dyn_syn_lifetime, CTLFLAG_RW,
    &dyn_syn_lifetime, 0, "Lifetime of dyn. rules for syn");
SYSCTL_INT(_net_inet_ip_fw, OID_AUTO, dyn_fin_lifetime, CTLFLAG_RW,
    &dyn_fin_lifetime, 0, "Lifetime of dyn. rules for fin");
SYSCTL_INT(_net_inet_ip_fw, OID_AUTO, dyn_finwait_lifetime, CTLFLAG_RW,
    &dyn_finwait_lifetime, 0, "Lifetime of dyn. rules for fin wait");
SYSCTL_INT(_net_inet_ip_fw, OID_AUTO, dyn_rst_lifetime, CTLFLAG_RW,
    &dyn_rst_lifetime, 0, "Lifetime of dyn. rules for rst");
SYSCTL_INT(_net_inet_ip_fw, OID_AUTO, dyn_udp_lifetime, CTLFLAG_RW,
    &dyn_udp_lifetime, 0, "Lifetime of dyn. rules for UDP");
SYSCTL_INT(_net_inet_ip_fw, OID_AUTO, dyn_short_lifetime, CTLFLAG_RW,
    &dyn_short_lifetime, 0, "Lifetime of dyn. rules for other situations");
SYSCTL_INT(_net_inet_ip_fw, OID_AUTO, dyn_keepalive, CTLFLAG_RW,
    &dyn_keepalive, 0, "Enable keepalives for dyn. rules");
SYSCTL_PROC(_net_inet_ip_fw, OID_AUTO, state_scan_max,
    CTLTYPE_INT | CTLFLAG_RW, &ipfw_state_scan_max, 0, ipfw_sysctl_scancnt,
    "I", "# of states to scan for each expire iteration");
SYSCTL_PROC(_net_inet_ip_fw, OID_AUTO, state_expire_max,
    CTLTYPE_INT | CTLFLAG_RW, &ipfw_state_expire_max, 0, ipfw_sysctl_scancnt,
    "I", "# of states to expire for each expire iteration");
SYSCTL_PROC(_net_inet_ip_fw, OID_AUTO, keepalive_max,
    CTLTYPE_INT | CTLFLAG_RW, &ipfw_keepalive_max, 0, ipfw_sysctl_scancnt,
    "I", "# of states to expire for each expire iteration");
SYSCTL_PROC(_net_inet_ip_fw, OID_AUTO, state_reap_min,
    CTLTYPE_INT | CTLFLAG_RW, &ipfw_state_reap_min, 0, ipfw_sysctl_scancnt,
    "I", "# of states to reap for state shortage");
SYSCTL_PROC(_net_inet_ip_fw, OID_AUTO, track_scan_max,
    CTLTYPE_INT | CTLFLAG_RW, &ipfw_track_scan_max, 0, ipfw_sysctl_scancnt,
    "I", "# of tracks to scan for each expire iteration");
SYSCTL_PROC(_net_inet_ip_fw, OID_AUTO, track_expire_max,
    CTLTYPE_INT | CTLFLAG_RW, &ipfw_track_expire_max, 0, ipfw_sysctl_scancnt,
    "I", "# of tracks to expire for each expire iteration");
SYSCTL_PROC(_net_inet_ip_fw, OID_AUTO, track_reap_max,
    CTLTYPE_INT | CTLFLAG_RW, &ipfw_track_reap_max, 0, ipfw_sysctl_scancnt,
    "I", "# of tracks to reap for track shortage");

SYSCTL_PROC(_net_inet_ip_fw_stats, OID_AUTO, state_reap,
    CTLTYPE_ULONG | CTLFLAG_RW, NULL,
    __offsetof(struct ipfw_context, ipfw_sts_reap), ipfw_sysctl_stat,
    "LU", "# of state reaps due to states shortage");
SYSCTL_PROC(_net_inet_ip_fw_stats, OID_AUTO, state_reapfailed,
    CTLTYPE_ULONG | CTLFLAG_RW, NULL,
    __offsetof(struct ipfw_context, ipfw_sts_reapfailed), ipfw_sysctl_stat,
    "LU", "# of state reap failure");
SYSCTL_PROC(_net_inet_ip_fw_stats, OID_AUTO, state_overflow,
    CTLTYPE_ULONG | CTLFLAG_RW, NULL,
    __offsetof(struct ipfw_context, ipfw_sts_overflow), ipfw_sysctl_stat,
    "LU", "# of state overflow");
SYSCTL_PROC(_net_inet_ip_fw_stats, OID_AUTO, state_nomem,
    CTLTYPE_ULONG | CTLFLAG_RW, NULL,
    __offsetof(struct ipfw_context, ipfw_sts_nomem), ipfw_sysctl_stat,
    "LU", "# of state allocation failure");
SYSCTL_PROC(_net_inet_ip_fw_stats, OID_AUTO, state_tcprecycled,
    CTLTYPE_ULONG | CTLFLAG_RW, NULL,
    __offsetof(struct ipfw_context, ipfw_sts_tcprecycled), ipfw_sysctl_stat,
    "LU", "# of state deleted due to fast TCP port recycling");

SYSCTL_PROC(_net_inet_ip_fw_stats, OID_AUTO, track_nomem,
    CTLTYPE_ULONG | CTLFLAG_RW, NULL,
    __offsetof(struct ipfw_context, ipfw_tks_nomem), ipfw_sysctl_stat,
    "LU", "# of track allocation failure");
SYSCTL_PROC(_net_inet_ip_fw_stats, OID_AUTO, track_reap,
    CTLTYPE_ULONG | CTLFLAG_RW, NULL,
    __offsetof(struct ipfw_context, ipfw_tks_reap), ipfw_sysctl_stat,
    "LU", "# of track reap due to tracks shortage");
SYSCTL_PROC(_net_inet_ip_fw_stats, OID_AUTO, track_reapfailed,
    CTLTYPE_ULONG | CTLFLAG_RW, NULL,
    __offsetof(struct ipfw_context, ipfw_tks_reapfailed), ipfw_sysctl_stat,
    "LU", "# of track reap failure");
SYSCTL_PROC(_net_inet_ip_fw_stats, OID_AUTO, track_overflow,
    CTLTYPE_ULONG | CTLFLAG_RW, NULL,
    __offsetof(struct ipfw_context, ipfw_tks_overflow), ipfw_sysctl_stat,
    "LU", "# of track overflow");
SYSCTL_PROC(_net_inet_ip_fw_stats, OID_AUTO, track_cntnomem,
    CTLTYPE_ULONG | CTLFLAG_RW, NULL,
    __offsetof(struct ipfw_context, ipfw_tks_cntnomem), ipfw_sysctl_stat,
    "LU", "# of track counter allocation failure");

static int		ipfw_state_cmp(struct ipfw_state *,
			    struct ipfw_state *);
static int		ipfw_trkcnt_cmp(struct ipfw_trkcnt *,
			    struct ipfw_trkcnt *);
static int		ipfw_track_cmp(struct ipfw_track *,
			    struct ipfw_track *);

RB_PROTOTYPE(ipfw_state_tree, ipfw_state, st_rblink, ipfw_state_cmp);
RB_GENERATE(ipfw_state_tree, ipfw_state, st_rblink, ipfw_state_cmp);

RB_PROTOTYPE(ipfw_trkcnt_tree, ipfw_trkcnt, tc_rblink, ipfw_trkcnt_cmp);
RB_GENERATE(ipfw_trkcnt_tree, ipfw_trkcnt, tc_rblink, ipfw_trkcnt_cmp);

RB_PROTOTYPE(ipfw_track_tree, ipfw_track, t_rblink, ipfw_track_cmp);
RB_GENERATE(ipfw_track_tree, ipfw_track, t_rblink, ipfw_track_cmp);

static ip_fw_chk_t	ipfw_chk;
static void		ipfw_track_expire_ipifunc(void *);
static void		ipfw_state_expire_ipifunc(void *);
static void		ipfw_keepalive(void *);
static int		ipfw_state_expire_start(struct ipfw_context *,
			    int, int);

#define IPFW_TRKCNT_TOKGET	lwkt_gettoken(&ipfw_gd.ipfw_trkcnt_token)
#define IPFW_TRKCNT_TOKREL	lwkt_reltoken(&ipfw_gd.ipfw_trkcnt_token)
#define IPFW_TRKCNT_TOKINIT	\
	lwkt_token_init(&ipfw_gd.ipfw_trkcnt_token, "ipfw_trkcnt");

static __inline void
ipfw_key_build(struct ipfw_key *key, in_addr_t saddr, uint16_t sport,
    in_addr_t daddr, uint16_t dport, uint8_t proto)
{

	key->proto = proto;
	key->swap = 0;

	if (saddr < daddr) {
		key->addr_u.addrs.addr1 = daddr;
		key->addr_u.addrs.addr2 = saddr;
		key->swap |= IPFW_KEY_SWAP_ADDRS;
	} else {
		key->addr_u.addrs.addr1 = saddr;
		key->addr_u.addrs.addr2 = daddr;
	}

	if (sport < dport) {
		key->port_u.ports.port1 = dport;
		key->port_u.ports.port2 = sport;
		key->swap |= IPFW_KEY_SWAP_PORTS;
	} else {
		key->port_u.ports.port1 = sport;
		key->port_u.ports.port2 = dport;
	}

	if (sport == dport && (key->swap & IPFW_KEY_SWAP_ADDRS))
		key->swap |= IPFW_KEY_SWAP_PORTS;
	if (saddr == daddr && (key->swap & IPFW_KEY_SWAP_PORTS))
		key->swap |= IPFW_KEY_SWAP_ADDRS;
}

static __inline void
ipfw_key_4tuple(const struct ipfw_key *key, in_addr_t *saddr, uint16_t *sport,
    in_addr_t *daddr, uint16_t *dport)
{

	if (key->swap & IPFW_KEY_SWAP_ADDRS) {
		*saddr = key->addr_u.addrs.addr2;
		*daddr = key->addr_u.addrs.addr1;
	} else {
		*saddr = key->addr_u.addrs.addr1;
		*daddr = key->addr_u.addrs.addr2;
	}

	if (key->swap & IPFW_KEY_SWAP_PORTS) {
		*sport = key->port_u.ports.port2;
		*dport = key->port_u.ports.port1;
	} else {
		*sport = key->port_u.ports.port1;
		*dport = key->port_u.ports.port2;
	}
}

static int
ipfw_state_cmp(struct ipfw_state *s1, struct ipfw_state *s2)
{

	if (s1->st_proto > s2->st_proto)
		return (1);
	if (s1->st_proto < s2->st_proto)
		return (-1);

	if (s1->st_addrs > s2->st_addrs)
		return (1);
	if (s1->st_addrs < s2->st_addrs)
		return (-1);

	if (s1->st_ports > s2->st_ports)
		return (1);
	if (s1->st_ports < s2->st_ports)
		return (-1);

	if (s1->st_swap == s2->st_swap ||
	    (s1->st_swap ^ s2->st_swap) == IPFW_KEY_SWAP_ALL)
		return (0);

	if (s1->st_swap > s2->st_swap)
		return (1);
	else
		return (-1);
}

static int
ipfw_trkcnt_cmp(struct ipfw_trkcnt *t1, struct ipfw_trkcnt *t2)
{

	if (t1->tc_proto > t2->tc_proto)
		return (1);
	if (t1->tc_proto < t2->tc_proto)
		return (-1);

	if (t1->tc_addrs > t2->tc_addrs)
		return (1);
	if (t1->tc_addrs < t2->tc_addrs)
		return (-1);

	if (t1->tc_ports > t2->tc_ports)
		return (1);
	if (t1->tc_ports < t2->tc_ports)
		return (-1);

	if (t1->tc_ruleid > t2->tc_ruleid)
		return (1);
	if (t1->tc_ruleid < t2->tc_ruleid)
		return (-1);

	return (0);
}

static int
ipfw_track_cmp(struct ipfw_track *t1, struct ipfw_track *t2)
{

	if (t1->t_proto > t2->t_proto)
		return (1);
	if (t1->t_proto < t2->t_proto)
		return (-1);

	if (t1->t_addrs > t2->t_addrs)
		return (1);
	if (t1->t_addrs < t2->t_addrs)
		return (-1);

	if (t1->t_ports > t2->t_ports)
		return (1);
	if (t1->t_ports < t2->t_ports)
		return (-1);

	if ((uintptr_t)t1->t_rule > (uintptr_t)t2->t_rule)
		return (1);
	if ((uintptr_t)t1->t_rule < (uintptr_t)t2->t_rule)
		return (-1);

	return (0);
}

static void
ipfw_state_max_set(int state_max)
{

	ipfw_state_max = state_max;
	/* Allow 5% states over-allocation. */
	ipfw_state_loosecnt_updthr = (state_max / 20) / netisr_ncpus;
}

static __inline int
ipfw_state_cntcoll(void)
{
	int cpu, state_cnt = 0;

	for (cpu = 0; cpu < netisr_ncpus; ++cpu)
		state_cnt += ipfw_ctx[cpu]->ipfw_state_cnt;
	return (state_cnt);
}

static __inline int
ipfw_state_cntsync(void)
{
	int state_cnt;

	state_cnt = ipfw_state_cntcoll();
	ipfw_gd.ipfw_state_loosecnt = state_cnt;
	return (state_cnt);
}

static __inline int
ipfw_free_rule(struct ip_fw *rule)
{
	KASSERT(rule->cpuid == mycpuid, ("rule freed on cpu%d", mycpuid));
	KASSERT(rule->refcnt > 0, ("invalid refcnt %u", rule->refcnt));
	rule->refcnt--;
	if (rule->refcnt == 0) {
		kfree(rule, M_IPFW);
		return 1;
	}
	return 0;
}

static void
ipfw_unref_rule(void *priv)
{
	ipfw_free_rule(priv);
#ifdef KLD_MODULE
	atomic_subtract_int(&ipfw_gd.ipfw_refcnt, 1);
#endif
}

static __inline void
ipfw_ref_rule(struct ip_fw *rule)
{
	KASSERT(rule->cpuid == mycpuid, ("rule used on cpu%d", mycpuid));
#ifdef KLD_MODULE
	atomic_add_int(&ipfw_gd.ipfw_refcnt, 1);
#endif
	rule->refcnt++;
}

/*
 * This macro maps an ip pointer into a layer3 header pointer of type T
 */
#define	L3HDR(T, ip) ((T *)((uint32_t *)(ip) + (ip)->ip_hl))

static __inline int
icmptype_match(struct ip *ip, ipfw_insn_u32 *cmd)
{
	int type = L3HDR(struct icmp,ip)->icmp_type;

	return (type <= ICMP_MAXTYPE && (cmd->d[0] & (1 << type)));
}

#define TT	((1 << ICMP_ECHO) | \
		 (1 << ICMP_ROUTERSOLICIT) | \
		 (1 << ICMP_TSTAMP) | \
		 (1 << ICMP_IREQ) | \
		 (1 << ICMP_MASKREQ))

static int
is_icmp_query(struct ip *ip)
{
	int type = L3HDR(struct icmp, ip)->icmp_type;

	return (type <= ICMP_MAXTYPE && (TT & (1 << type)));
}

#undef TT

/*
 * The following checks use two arrays of 8 or 16 bits to store the
 * bits that we want set or clear, respectively. They are in the
 * low and high half of cmd->arg1 or cmd->d[0].
 *
 * We scan options and store the bits we find set. We succeed if
 *
 *	(want_set & ~bits) == 0 && (want_clear & ~bits) == want_clear
 *
 * The code is sometimes optimized not to store additional variables.
 */
static int
flags_match(ipfw_insn *cmd, uint8_t bits)
{
	u_char want_clear;
	bits = ~bits;

	if (((cmd->arg1 & 0xff) & bits) != 0)
		return 0; /* some bits we want set were clear */

	want_clear = (cmd->arg1 >> 8) & 0xff;
	if ((want_clear & bits) != want_clear)
		return 0; /* some bits we want clear were set */
	return 1;
}

static int
ipopts_match(struct ip *ip, ipfw_insn *cmd)
{
	int optlen, bits = 0;
	u_char *cp = (u_char *)(ip + 1);
	int x = (ip->ip_hl << 2) - sizeof(struct ip);

	for (; x > 0; x -= optlen, cp += optlen) {
		int opt = cp[IPOPT_OPTVAL];

		if (opt == IPOPT_EOL)
			break;

		if (opt == IPOPT_NOP) {
			optlen = 1;
		} else {
			optlen = cp[IPOPT_OLEN];
			if (optlen <= 0 || optlen > x)
				return 0; /* invalid or truncated */
		}

		switch (opt) {
		case IPOPT_LSRR:
			bits |= IP_FW_IPOPT_LSRR;
			break;

		case IPOPT_SSRR:
			bits |= IP_FW_IPOPT_SSRR;
			break;

		case IPOPT_RR:
			bits |= IP_FW_IPOPT_RR;
			break;

		case IPOPT_TS:
			bits |= IP_FW_IPOPT_TS;
			break;

		default:
			break;
		}
	}
	return (flags_match(cmd, bits));
}

static int
tcpopts_match(struct ip *ip, ipfw_insn *cmd)
{
	int optlen, bits = 0;
	struct tcphdr *tcp = L3HDR(struct tcphdr,ip);
	u_char *cp = (u_char *)(tcp + 1);
	int x = (tcp->th_off << 2) - sizeof(struct tcphdr);

	for (; x > 0; x -= optlen, cp += optlen) {
		int opt = cp[0];

		if (opt == TCPOPT_EOL)
			break;

		if (opt == TCPOPT_NOP) {
			optlen = 1;
		} else {
			optlen = cp[1];
			if (optlen <= 0)
				break;
		}

		switch (opt) {
		case TCPOPT_MAXSEG:
			bits |= IP_FW_TCPOPT_MSS;
			break;

		case TCPOPT_WINDOW:
			bits |= IP_FW_TCPOPT_WINDOW;
			break;

		case TCPOPT_SACK_PERMITTED:
		case TCPOPT_SACK:
			bits |= IP_FW_TCPOPT_SACK;
			break;

		case TCPOPT_TIMESTAMP:
			bits |= IP_FW_TCPOPT_TS;
			break;

		case TCPOPT_CC:
		case TCPOPT_CCNEW:
		case TCPOPT_CCECHO:
			bits |= IP_FW_TCPOPT_CC;
			break;

		default:
			break;
		}
	}
	return (flags_match(cmd, bits));
}

static int
iface_match(struct ifnet *ifp, ipfw_insn_if *cmd)
{
	if (ifp == NULL)	/* no iface with this packet, match fails */
		return 0;

	/* Check by name or by IP address */
	if (cmd->name[0] != '\0') { /* match by name */
		/* Check name */
		if (cmd->p.glob) {
			if (kfnmatch(cmd->name, ifp->if_xname, 0) == 0)
				return(1);
		} else {
			if (strncmp(ifp->if_xname, cmd->name, IFNAMSIZ) == 0)
				return(1);
		}
	} else {
		struct ifaddr_container *ifac;

		TAILQ_FOREACH(ifac, &ifp->if_addrheads[mycpuid], ifa_link) {
			struct ifaddr *ia = ifac->ifa;

			if (ia->ifa_addr == NULL)
				continue;
			if (ia->ifa_addr->sa_family != AF_INET)
				continue;
			if (cmd->p.ip.s_addr == ((struct sockaddr_in *)
			    (ia->ifa_addr))->sin_addr.s_addr)
				return(1);	/* match */
		}
	}
	return(0);	/* no match, fail ... */
}

#define SNPARGS(buf, len) buf + len, sizeof(buf) > len ? sizeof(buf) - len : 0

/*
 * We enter here when we have a rule with O_LOG.
 * XXX this function alone takes about 2Kbytes of code!
 */
static void
ipfw_log(struct ipfw_context *ctx, struct ip_fw *f, u_int hlen,
    struct ether_header *eh, struct mbuf *m, struct ifnet *oif)
{
	char *action;
	int limit_reached = 0;
	char action2[40], proto[48], fragment[28], abuf[INET_ADDRSTRLEN];

	fragment[0] = '\0';
	proto[0] = '\0';

	if (f == NULL) {	/* bogus pkt */
		if (verbose_limit != 0 &&
		    ctx->ipfw_norule_counter >= verbose_limit)
			return;
		ctx->ipfw_norule_counter++;
		if (ctx->ipfw_norule_counter == verbose_limit)
			limit_reached = verbose_limit;
		action = "Refuse";
	} else {	/* O_LOG is the first action, find the real one */
		ipfw_insn *cmd = ACTION_PTR(f);
		ipfw_insn_log *l = (ipfw_insn_log *)cmd;

		if (l->max_log != 0 && l->log_left == 0)
			return;
		l->log_left--;
		if (l->log_left == 0)
			limit_reached = l->max_log;
		cmd += F_LEN(cmd);	/* point to first action */
		if (cmd->opcode == O_PROB)
			cmd += F_LEN(cmd);

		action = action2;
		switch (cmd->opcode) {
		case O_DENY:
			action = "Deny";
			break;

		case O_REJECT:
			if (cmd->arg1==ICMP_REJECT_RST) {
				action = "Reset";
			} else if (cmd->arg1==ICMP_UNREACH_HOST) {
				action = "Reject";
			} else {
				ksnprintf(SNPARGS(action2, 0), "Unreach %d",
					  cmd->arg1);
			}
			break;

		case O_ACCEPT:
			action = "Accept";
			break;

		case O_COUNT:
			action = "Count";
			break;

		case O_DIVERT:
			ksnprintf(SNPARGS(action2, 0), "Divert %d", cmd->arg1);
			break;

		case O_TEE:
			ksnprintf(SNPARGS(action2, 0), "Tee %d", cmd->arg1);
			break;

		case O_SKIPTO:
			ksnprintf(SNPARGS(action2, 0), "SkipTo %d", cmd->arg1);
			break;

		case O_PIPE:
			ksnprintf(SNPARGS(action2, 0), "Pipe %d", cmd->arg1);
			break;

		case O_QUEUE:
			ksnprintf(SNPARGS(action2, 0), "Queue %d", cmd->arg1);
			break;

		case O_FORWARD_IP:
			{
				ipfw_insn_sa *sa = (ipfw_insn_sa *)cmd;
				int len;

				len = ksnprintf(SNPARGS(action2, 0),
				    "Forward to %s",
				    kinet_ntoa(sa->sa.sin_addr, abuf));
				if (sa->sa.sin_port) {
					ksnprintf(SNPARGS(action2, len), ":%d",
						  sa->sa.sin_port);
				}
			}
			break;

		default:
			action = "UNKNOWN";
			break;
		}
	}

	if (hlen == 0) {	/* non-ip */
		ksnprintf(SNPARGS(proto, 0), "MAC");
	} else {
		struct ip *ip = mtod(m, struct ip *);
		/* these three are all aliases to the same thing */
		struct icmp *const icmp = L3HDR(struct icmp, ip);
		struct tcphdr *const tcp = (struct tcphdr *)icmp;
		struct udphdr *const udp = (struct udphdr *)icmp;

		int ip_off, offset, ip_len;
		int len;

		if (eh != NULL) { /* layer 2 packets are as on the wire */
			ip_off = ntohs(ip->ip_off);
			ip_len = ntohs(ip->ip_len);
		} else {
			ip_off = ip->ip_off;
			ip_len = ip->ip_len;
		}
		offset = ip_off & IP_OFFMASK;
		switch (ip->ip_p) {
		case IPPROTO_TCP:
			len = ksnprintf(SNPARGS(proto, 0), "TCP %s",
					kinet_ntoa(ip->ip_src, abuf));
			if (offset == 0) {
				ksnprintf(SNPARGS(proto, len), ":%d %s:%d",
					  ntohs(tcp->th_sport),
					  kinet_ntoa(ip->ip_dst, abuf),
					  ntohs(tcp->th_dport));
			} else {
				ksnprintf(SNPARGS(proto, len), " %s",
					  kinet_ntoa(ip->ip_dst, abuf));
			}
			break;

		case IPPROTO_UDP:
			len = ksnprintf(SNPARGS(proto, 0), "UDP %s",
					kinet_ntoa(ip->ip_src, abuf));
			if (offset == 0) {
				ksnprintf(SNPARGS(proto, len), ":%d %s:%d",
					  ntohs(udp->uh_sport),
					  kinet_ntoa(ip->ip_dst, abuf),
					  ntohs(udp->uh_dport));
			} else {
				ksnprintf(SNPARGS(proto, len), " %s",
					  kinet_ntoa(ip->ip_dst, abuf));
			}
			break;

		case IPPROTO_ICMP:
			if (offset == 0) {
				len = ksnprintf(SNPARGS(proto, 0),
						"ICMP:%u.%u ",
						icmp->icmp_type,
						icmp->icmp_code);
			} else {
				len = ksnprintf(SNPARGS(proto, 0), "ICMP ");
			}
			len += ksnprintf(SNPARGS(proto, len), "%s",
					 kinet_ntoa(ip->ip_src, abuf));
			ksnprintf(SNPARGS(proto, len), " %s",
				  kinet_ntoa(ip->ip_dst, abuf));
			break;

		default:
			len = ksnprintf(SNPARGS(proto, 0), "P:%d %s", ip->ip_p,
					kinet_ntoa(ip->ip_src, abuf));
			ksnprintf(SNPARGS(proto, len), " %s",
				  kinet_ntoa(ip->ip_dst, abuf));
			break;
		}

		if (ip_off & (IP_MF | IP_OFFMASK)) {
			ksnprintf(SNPARGS(fragment, 0), " (frag %d:%d@%d%s)",
				  ntohs(ip->ip_id), ip_len - (ip->ip_hl << 2),
				  offset << 3, (ip_off & IP_MF) ? "+" : "");
		}
	}

	if (oif || m->m_pkthdr.rcvif) {
		log(LOG_SECURITY | LOG_INFO,
		    "ipfw: %d %s %s %s via %s%s\n",
		    f ? f->rulenum : -1,
		    action, proto, oif ? "out" : "in",
		    oif ? oif->if_xname : m->m_pkthdr.rcvif->if_xname,
		    fragment);
	} else {
		log(LOG_SECURITY | LOG_INFO,
		    "ipfw: %d %s %s [no if info]%s\n",
		    f ? f->rulenum : -1,
		    action, proto, fragment);
	}

	if (limit_reached) {
		log(LOG_SECURITY | LOG_NOTICE,
		    "ipfw: limit %d reached on entry %d\n",
		    limit_reached, f ? f->rulenum : -1);
	}
}

#undef SNPARGS

#define TIME_LEQ(a, b)	((a) - (b) <= 0)

static void
ipfw_state_del(struct ipfw_context *ctx, struct ipfw_state *s)
{

	KASSERT(s->st_type == O_KEEP_STATE || s->st_type == O_LIMIT,
	    ("invalid state type %u", s->st_type));
	KASSERT(ctx->ipfw_state_cnt > 0,
	    ("invalid state count %d", ctx->ipfw_state_cnt));

	if (s->st_track != NULL) {
		struct ipfw_track *t = s->st_track;

		KASSERT(!LIST_EMPTY(&t->t_state_list),
		    ("track state list is empty"));
		LIST_REMOVE(s, st_trklink);

		KASSERT(*t->t_count > 0,
		    ("invalid track count %d", *t->t_count));
		atomic_subtract_int(t->t_count, 1);
	}

	TAILQ_REMOVE(&ctx->ipfw_state_list, s, st_link);
	RB_REMOVE(ipfw_state_tree, &ctx->ipfw_state_tree, s);
	kfree(s, M_IPFW);

	ctx->ipfw_state_cnt--;
	if (ctx->ipfw_state_loosecnt > 0)
		ctx->ipfw_state_loosecnt--;
}

static int
ipfw_state_reap(struct ipfw_context *ctx, int reap_max)
{
	struct ipfw_state *s, *anchor;
	int expired;

	if (reap_max < ipfw_state_reap_min)
		reap_max = ipfw_state_reap_min;

	if ((ctx->ipfw_flags & IPFW_FLAG_STATEEXP) == 0) {
		/*
		 * Kick start state expiring.  Ignore scan limit,
		 * we are short of states.
		 */
		ctx->ipfw_flags |= IPFW_FLAG_STATEREAP;
		expired = ipfw_state_expire_start(ctx, INT_MAX, reap_max);
		ctx->ipfw_flags &= ~IPFW_FLAG_STATEREAP;
		return (expired);
	}

	/*
	 * States are being expired.
	 */

	if (ctx->ipfw_state_cnt == 0)
		return (0);

	expired = 0;
	anchor = &ctx->ipfw_stateexp_anch;
	while ((s = TAILQ_NEXT(anchor, st_link)) != NULL) {
		/*
		 * Ignore scan limit; we are short of states.
		 */

		TAILQ_REMOVE(&ctx->ipfw_state_list, anchor, st_link);
		TAILQ_INSERT_AFTER(&ctx->ipfw_state_list, s, anchor, st_link);

		if (s->st_type == O_ANCHOR)
			continue;

		if (IPFW_STATE_TCPCLOSED(s) ||
		    TIME_LEQ(s->st_expire, time_uptime)) {
			ipfw_state_del(ctx, s);
			if (++expired >= reap_max)
				break;
			if ((expired & 0xff) == 0 && 
			    ipfw_state_cntcoll() + ipfw_state_headroom <=
			    ipfw_state_max)
				break;
		}
	}
	/*
	 * NOTE:
	 * Leave the anchor on the list, even if the end of the list has
	 * been reached.  ipfw_state_expire_more_dispatch() will handle
	 * the removal.
	 */
	return (expired);
}

static void
ipfw_state_flush(struct ipfw_context *ctx, const struct ip_fw *rule)
{
	struct ipfw_state *s, *sn;

	TAILQ_FOREACH_MUTABLE(s, &ctx->ipfw_state_list, st_link, sn) {
		if (s->st_type == O_ANCHOR)
			continue;
		if (rule != NULL && s->st_rule != rule)
			continue;
		ipfw_state_del(ctx, s);
	}
}

static void
ipfw_state_expire_done(struct ipfw_context *ctx)
{

	KASSERT(ctx->ipfw_flags & IPFW_FLAG_STATEEXP,
	    ("stateexp is not in progress"));
	ctx->ipfw_flags &= ~IPFW_FLAG_STATEEXP;
	callout_reset(&ctx->ipfw_stateto_ch, hz,
	    ipfw_state_expire_ipifunc, NULL);
}

static void
ipfw_state_expire_more(struct ipfw_context *ctx)
{
	struct netmsg_base *nm = &ctx->ipfw_stateexp_more;

	KASSERT(ctx->ipfw_flags & IPFW_FLAG_STATEEXP,
	    ("stateexp is not in progress"));
	KASSERT(nm->lmsg.ms_flags & MSGF_DONE,
	    ("stateexp more did not finish"));
	netisr_sendmsg_oncpu(nm);
}

static int
ipfw_state_expire_loop(struct ipfw_context *ctx, struct ipfw_state *anchor,
    int scan_max, int expire_max)
{
	struct ipfw_state *s;
	int scanned = 0, expired = 0;

	KASSERT(ctx->ipfw_flags & IPFW_FLAG_STATEEXP,
	    ("stateexp is not in progress"));

	while ((s = TAILQ_NEXT(anchor, st_link)) != NULL) {
		if (scanned++ >= scan_max) {
			ipfw_state_expire_more(ctx);
			return (expired);
		}

		TAILQ_REMOVE(&ctx->ipfw_state_list, anchor, st_link);
		TAILQ_INSERT_AFTER(&ctx->ipfw_state_list, s, anchor, st_link);

		if (s->st_type == O_ANCHOR)
			continue;

		if (TIME_LEQ(s->st_expire, time_uptime) ||
		    ((ctx->ipfw_flags & IPFW_FLAG_STATEREAP) &&
		     IPFW_STATE_TCPCLOSED(s))) {
			ipfw_state_del(ctx, s);
			if (++expired >= expire_max) {
				ipfw_state_expire_more(ctx);
				return (expired);
			}
			if ((ctx->ipfw_flags & IPFW_FLAG_STATEREAP) &&
			    (expired & 0xff) == 0 &&
			    ipfw_state_cntcoll() + ipfw_state_headroom <=
			    ipfw_state_max) {
				ipfw_state_expire_more(ctx);
				return (expired);
			}
		}
	}
	TAILQ_REMOVE(&ctx->ipfw_state_list, anchor, st_link);
	ipfw_state_expire_done(ctx);
	return (expired);
}

static void
ipfw_state_expire_more_dispatch(netmsg_t nm)
{
	struct ipfw_context *ctx = ipfw_ctx[mycpuid];
	struct ipfw_state *anchor;

	ASSERT_NETISR_NCPUS(mycpuid);
	KASSERT(ctx->ipfw_flags & IPFW_FLAG_STATEEXP,
	    ("statexp is not in progress"));

	/* Reply ASAP */
	netisr_replymsg(&nm->base, 0);

	anchor = &ctx->ipfw_stateexp_anch;
	if (ctx->ipfw_state_cnt == 0) {
		TAILQ_REMOVE(&ctx->ipfw_state_list, anchor, st_link);
		ipfw_state_expire_done(ctx);
		return;
	}
	ipfw_state_expire_loop(ctx, anchor,
	    ipfw_state_scan_max, ipfw_state_expire_max);
}

static int
ipfw_state_expire_start(struct ipfw_context *ctx, int scan_max, int expire_max)
{
	struct ipfw_state *anchor;

	KASSERT((ctx->ipfw_flags & IPFW_FLAG_STATEEXP) == 0,
	    ("stateexp is in progress"));
	ctx->ipfw_flags |= IPFW_FLAG_STATEEXP;

	if (ctx->ipfw_state_cnt == 0) {
		ipfw_state_expire_done(ctx);
		return (0);
	}

	/*
	 * Do not expire more than once per second, it is useless.
	 */
	if ((ctx->ipfw_flags & IPFW_FLAG_STATEREAP) == 0 &&
	    ctx->ipfw_state_lastexp == time_uptime) {
		ipfw_state_expire_done(ctx);
		return (0);
	}
	ctx->ipfw_state_lastexp = time_uptime;

	anchor = &ctx->ipfw_stateexp_anch;
	TAILQ_INSERT_HEAD(&ctx->ipfw_state_list, anchor, st_link);
	return (ipfw_state_expire_loop(ctx, anchor, scan_max, expire_max));
}

static void
ipfw_state_expire_dispatch(netmsg_t nm)
{
	struct ipfw_context *ctx = ipfw_ctx[mycpuid];

	ASSERT_NETISR_NCPUS(mycpuid);

	/* Reply ASAP */
	crit_enter();
	netisr_replymsg(&nm->base, 0);
	crit_exit();

	if (ctx->ipfw_flags & IPFW_FLAG_STATEEXP) {
		/* Running; done. */
		return;
	}
	ipfw_state_expire_start(ctx,
	    ipfw_state_scan_max, ipfw_state_expire_max);
}

static void
ipfw_state_expire_ipifunc(void *dummy __unused)
{
	struct netmsg_base *msg;

	KKASSERT(mycpuid < netisr_ncpus);
	msg = &ipfw_ctx[mycpuid]->ipfw_stateexp_nm;

	crit_enter();
	if (msg->lmsg.ms_flags & MSGF_DONE)
		netisr_sendmsg_oncpu(msg);
	crit_exit();
}

static boolean_t
ipfw_state_update_tcp(struct ipfw_state *s, int dir, const struct tcphdr *tcp)
{
	uint32_t seq = ntohl(tcp->th_seq);
	uint32_t ack = ntohl(tcp->th_ack);

	if (tcp->th_flags & TH_RST)
		return (TRUE);

	if (dir == MATCH_FORWARD) {
		if ((s->st_flags & IPFW_STATE_F_SEQFWD) == 0) {
			s->st_flags |= IPFW_STATE_F_SEQFWD;
			s->st_seq_fwd = seq;
		} else if (SEQ_GEQ(seq, s->st_seq_fwd)) {
			s->st_seq_fwd = seq;
		} else {
			/* Out-of-sequence; done. */
			return (FALSE);
		}
		if (tcp->th_flags & TH_ACK) {
			if ((s->st_flags & IPFW_STATE_F_ACKFWD) == 0) {
				s->st_flags |= IPFW_STATE_F_ACKFWD;
				s->st_ack_fwd = ack;
			} else if (SEQ_GEQ(ack, s->st_ack_fwd)) {
				s->st_ack_fwd = ack;
			} else {
				/* Out-of-sequence; done. */
				return (FALSE);
			}

			if ((s->st_state & ((TH_FIN | TH_ACK) << 8)) ==
			    (TH_FIN << 8) && s->st_ack_fwd == s->st_seq_rev + 1)
				s->st_state |= (TH_ACK << 8);
		}
	} else {
		if ((s->st_flags & IPFW_STATE_F_SEQREV) == 0) {
			s->st_flags |= IPFW_STATE_F_SEQREV;
			s->st_seq_rev = seq;
		} else if (SEQ_GEQ(seq, s->st_seq_rev)) {
			s->st_seq_rev = seq;
		} else {
			/* Out-of-sequence; done. */
			return (FALSE);
		}
		if (tcp->th_flags & TH_ACK) {
			if ((s->st_flags & IPFW_STATE_F_ACKREV) == 0) {
				s->st_flags |= IPFW_STATE_F_ACKREV;
				s->st_ack_rev= ack;
			} else if (SEQ_GEQ(ack, s->st_ack_rev)) {
				s->st_ack_rev = ack;
			} else {
				/* Out-of-sequence; done. */
				return (FALSE);
			}

			if ((s->st_state & (TH_FIN | TH_ACK)) == TH_FIN &&
			    s->st_ack_rev == s->st_seq_fwd + 1)
				s->st_state |= TH_ACK;
		}
	}
	return (TRUE);
}

static void
ipfw_state_update(const struct ipfw_flow_id *pkt, int dir,
    const struct tcphdr *tcp, struct ipfw_state *s)
{

	if (pkt->proto == IPPROTO_TCP) { /* update state according to flags */
		u_char flags = pkt->flags & IPFW_STATE_TCPFLAGS;

		if (tcp != NULL && !ipfw_state_update_tcp(s, dir, tcp))
			return;

		s->st_state |= (dir == MATCH_FORWARD) ? flags : (flags << 8);
		switch (s->st_state & IPFW_STATE_TCPSTATES) {
		case TH_SYN:				/* opening */
			s->st_expire = time_uptime + dyn_syn_lifetime;
			break;

		case BOTH_SYN:			/* move to established */
		case BOTH_SYN | TH_FIN:		/* one side tries to close */
		case BOTH_SYN | (TH_FIN << 8):
			s->st_expire = time_uptime + dyn_ack_lifetime;
			break;

		case BOTH_SYN | BOTH_FIN:	/* both sides closed */
			if ((s->st_state & BOTH_FINACK) == BOTH_FINACK) {
				/* And both FINs were ACKed. */
				s->st_expire = time_uptime + dyn_fin_lifetime;
			} else {
				s->st_expire = time_uptime +
				    dyn_finwait_lifetime;
			}
			break;

		default:
#if 0
			/*
			 * reset or some invalid combination, but can also
			 * occur if we use keep-state the wrong way.
			 */
			if ((s->st_state & ((TH_RST << 8) | TH_RST)) == 0)
				kprintf("invalid state: 0x%x\n", s->st_state);
#endif
			s->st_expire = time_uptime + dyn_rst_lifetime;
			break;
		}
	} else if (pkt->proto == IPPROTO_UDP) {
		s->st_expire = time_uptime + dyn_udp_lifetime;
	} else {
		/* other protocols */
		s->st_expire = time_uptime + dyn_short_lifetime;
	}
}

/*
 * Lookup a state.
 */
static struct ipfw_state *
ipfw_state_lookup(struct ipfw_context *ctx, const struct ipfw_flow_id *pkt,
    int *match_direction, const struct tcphdr *tcp)
{
	struct ipfw_state *key, *s;
	int dir = MATCH_NONE;

	key = &ctx->ipfw_state_tmpkey;
	ipfw_key_build(&key->st_key, pkt->src_ip, pkt->src_port,
	    pkt->dst_ip, pkt->dst_port, pkt->proto);
	s = RB_FIND(ipfw_state_tree, &ctx->ipfw_state_tree, key);
	if (s == NULL)
		goto done; /* not found. */
	if (TIME_LEQ(s->st_expire, time_uptime)) {
		/* Expired. */
		ipfw_state_del(ctx, s);
		s = NULL;
		goto done;
	}
	if ((pkt->flags & TH_SYN) && IPFW_STATE_TCPCLOSED(s)) {
		/* TCP ports recycling is too fast. */
		ctx->ipfw_sts_tcprecycled++;
		ipfw_state_del(ctx, s);
		s = NULL;
		goto done;
	}

	if (s->st_swap == key->st_swap) {
		dir = MATCH_FORWARD;
	} else {
		KASSERT((s->st_swap & key->st_swap) == 0,
		    ("found mismatch state"));
		dir = MATCH_REVERSE;
	}

	/* Update this state. */
	ipfw_state_update(pkt, dir, tcp, s);

	if (s->st_track != NULL) {
		/* This track has been used. */
		s->st_track->t_expire = time_uptime + dyn_short_lifetime;
	}
done:
	if (match_direction)
		*match_direction = dir;
	return (s);
}

static __inline struct ip_fw *
ipfw_state_lookup_rule(struct ipfw_context *ctx, const struct ipfw_flow_id *pkt,
    int *match_direction, const struct tcphdr *tcp, uint16_t len)
{
	struct ipfw_state *s;

	s = ipfw_state_lookup(ctx, pkt, match_direction, tcp);
	if (s == NULL)
		return (NULL);

	KASSERT(s->st_rule->cpuid == mycpuid,
	    ("rule %p (cpu%d) does not belong to the current cpu%d",
	     s->st_rule, s->st_rule->cpuid, mycpuid));

	s->st_pcnt++;
	s->st_bcnt += len;

	return (s->st_rule);
}

static struct ipfw_state *
ipfw_state_add(struct ipfw_context *ctx, const struct ipfw_flow_id *id,
    uint16_t type, struct ip_fw *rule, struct ipfw_track *t,
    const struct tcphdr *tcp)
{
	struct ipfw_state *s, *dup;

	KASSERT(type == O_KEEP_STATE || type == O_LIMIT,
	    ("invalid state type %u", type));

	s = kmalloc(sizeof(*s), M_IPFW, M_INTWAIT | M_NULLOK | M_ZERO);
	if (s == NULL) {
		ctx->ipfw_sts_nomem++;
		return (NULL);
	}

	ipfw_key_build(&s->st_key, id->src_ip, id->src_port,
	    id->dst_ip, id->dst_port, id->proto);

	s->st_rule = rule;
	s->st_type = type;

	ctx->ipfw_state_cnt++;
	ctx->ipfw_state_loosecnt++;
	if (ctx->ipfw_state_loosecnt >= ipfw_state_loosecnt_updthr) {
		ipfw_gd.ipfw_state_loosecnt += ctx->ipfw_state_loosecnt;
		ctx->ipfw_state_loosecnt = 0;
	}

	dup = RB_INSERT(ipfw_state_tree, &ctx->ipfw_state_tree, s);
	if (dup != NULL)
		panic("ipfw: state exists");
	TAILQ_INSERT_TAIL(&ctx->ipfw_state_list, s, st_link);

	/*
	 * Update this state:
	 * Set st_expire and st_state.
	 */
	ipfw_state_update(id, MATCH_FORWARD, tcp, s);

	if (t != NULL) {
		/* Keep the track referenced. */
		LIST_INSERT_HEAD(&t->t_state_list, s, st_trklink);
		s->st_track = t;
	}
	return (s);
}

static boolean_t
ipfw_track_free(struct ipfw_context *ctx, struct ipfw_track *t)
{
	struct ipfw_trkcnt *trk;
	boolean_t trk_freed = FALSE;

	KASSERT(t->t_count != NULL, ("track anchor"));
	KASSERT(LIST_EMPTY(&t->t_state_list),
	    ("invalid track is still referenced"));

	trk = t->t_trkcnt;
	KASSERT(trk != NULL, ("track has no trkcnt"));

	RB_REMOVE(ipfw_track_tree, &ctx->ipfw_track_tree, t);
	TAILQ_REMOVE(&ctx->ipfw_track_list, t, t_link);
	kfree(t, M_IPFW);

	/*
	 * fdrop() style reference counting.
	 * See kern/kern_descrip.c fdrop().
	 */
	for (;;) {
		int refs = trk->tc_refs;

		cpu_ccfence();
		KASSERT(refs > 0, ("invalid trkcnt refs %d", refs));
		if (refs == 1) {
			IPFW_TRKCNT_TOKGET;
			if (atomic_cmpset_int(&trk->tc_refs, refs, 0)) {
				KASSERT(trk->tc_count == 0,
				    ("%d states reference this trkcnt",
				     trk->tc_count));
				RB_REMOVE(ipfw_trkcnt_tree,
				    &ipfw_gd.ipfw_trkcnt_tree, trk);

				KASSERT(ipfw_gd.ipfw_trkcnt_cnt > 0,
				    ("invalid trkcnt cnt %d",
				     ipfw_gd.ipfw_trkcnt_cnt));
				ipfw_gd.ipfw_trkcnt_cnt--;
				IPFW_TRKCNT_TOKREL;

				if (ctx->ipfw_trkcnt_spare == NULL)
					ctx->ipfw_trkcnt_spare = trk;
				else
					kfree(trk, M_IPFW);
				trk_freed = TRUE;
				break; /* done! */
			}
			IPFW_TRKCNT_TOKREL;
			/* retry */
		} else if (atomic_cmpset_int(&trk->tc_refs, refs, refs - 1)) {
			break; /* done! */
		}
		/* retry */
	}
	return (trk_freed);
}

static void
ipfw_track_flush(struct ipfw_context *ctx, struct ip_fw *rule)
{
	struct ipfw_track *t, *tn;

	TAILQ_FOREACH_MUTABLE(t, &ctx->ipfw_track_list, t_link, tn) {
		if (t->t_count == NULL) /* anchor */
			continue;
		if (rule != NULL && t->t_rule != rule)
			continue;
		ipfw_track_free(ctx, t);
	}
}

static boolean_t
ipfw_track_state_expire(struct ipfw_context *ctx, struct ipfw_track *t,
    boolean_t reap)
{
	struct ipfw_state *s, *sn;
	boolean_t ret = FALSE;

	KASSERT(t->t_count != NULL, ("track anchor"));

	if (LIST_EMPTY(&t->t_state_list))
		return (FALSE);

	/*
	 * Do not expire more than once per second, it is useless.
	 */
	if (t->t_lastexp == time_uptime)
		return (FALSE);
	t->t_lastexp = time_uptime;

	LIST_FOREACH_MUTABLE(s, &t->t_state_list, st_trklink, sn) {
		if (TIME_LEQ(s->st_expire, time_uptime) ||
		    (reap && IPFW_STATE_TCPCLOSED(s))) {
			KASSERT(s->st_track == t,
			    ("state track %p does not match %p",
			     s->st_track, t));
			ipfw_state_del(ctx, s);
			ret = TRUE;
		}
	}
	return (ret);
}

static __inline struct ipfw_trkcnt *
ipfw_trkcnt_alloc(struct ipfw_context *ctx)
{
	struct ipfw_trkcnt *trk;

	if (ctx->ipfw_trkcnt_spare != NULL) {
		trk = ctx->ipfw_trkcnt_spare;
		ctx->ipfw_trkcnt_spare = NULL;
	} else {
		trk = kmalloc_cachealign(sizeof(*trk), M_IPFW,
		    M_INTWAIT | M_NULLOK);
	}
	return (trk);
}

static void
ipfw_track_expire_done(struct ipfw_context *ctx)
{

	KASSERT(ctx->ipfw_flags & IPFW_FLAG_TRACKEXP,
	    ("trackexp is not in progress"));
	ctx->ipfw_flags &= ~IPFW_FLAG_TRACKEXP;
	callout_reset(&ctx->ipfw_trackto_ch, hz,
	    ipfw_track_expire_ipifunc, NULL);
}

static void
ipfw_track_expire_more(struct ipfw_context *ctx)
{
	struct netmsg_base *nm = &ctx->ipfw_trackexp_more;

	KASSERT(ctx->ipfw_flags & IPFW_FLAG_TRACKEXP,
	    ("trackexp is not in progress"));
	KASSERT(nm->lmsg.ms_flags & MSGF_DONE,
	    ("trackexp more did not finish"));
	netisr_sendmsg_oncpu(nm);
}

static int
ipfw_track_expire_loop(struct ipfw_context *ctx, struct ipfw_track *anchor,
    int scan_max, int expire_max)
{
	struct ipfw_track *t;
	int scanned = 0, expired = 0;
	boolean_t reap = FALSE;

	KASSERT(ctx->ipfw_flags & IPFW_FLAG_TRACKEXP,
	    ("trackexp is not in progress"));

	if (ctx->ipfw_flags & IPFW_FLAG_TRACKREAP)
		reap = TRUE;

	while ((t = TAILQ_NEXT(anchor, t_link)) != NULL) {
		if (scanned++ >= scan_max) {
			ipfw_track_expire_more(ctx);
			return (expired);
		}

		TAILQ_REMOVE(&ctx->ipfw_track_list, anchor, t_link);
		TAILQ_INSERT_AFTER(&ctx->ipfw_track_list, t, anchor, t_link);

		if (t->t_count == NULL) /* anchor */
			continue;

		ipfw_track_state_expire(ctx, t, reap);
		if (!LIST_EMPTY(&t->t_state_list)) {
			/* There are states referencing this track. */
			continue;
		}

		if (TIME_LEQ(t->t_expire, time_uptime) || reap) {
			/* Expired. */
			if (ipfw_track_free(ctx, t)) {
				if (++expired >= expire_max) {
					ipfw_track_expire_more(ctx);
					return (expired);
				}
			}
		}
	}
	TAILQ_REMOVE(&ctx->ipfw_track_list, anchor, t_link);
	ipfw_track_expire_done(ctx);
	return (expired);
}

static int
ipfw_track_expire_start(struct ipfw_context *ctx, int scan_max, int expire_max)
{
	struct ipfw_track *anchor;

	KASSERT((ctx->ipfw_flags & IPFW_FLAG_TRACKEXP) == 0,
	    ("trackexp is in progress"));
	ctx->ipfw_flags |= IPFW_FLAG_TRACKEXP;

	if (RB_EMPTY(&ctx->ipfw_track_tree)) {
		ipfw_track_expire_done(ctx);
		return (0);
	}

	/*
	 * Do not expire more than once per second, it is useless.
	 */
	if ((ctx->ipfw_flags & IPFW_FLAG_TRACKREAP) == 0 &&
	    ctx->ipfw_track_lastexp == time_uptime) {
		ipfw_track_expire_done(ctx);
		return (0);
	}
	ctx->ipfw_track_lastexp = time_uptime;

	anchor = &ctx->ipfw_trackexp_anch;
	TAILQ_INSERT_HEAD(&ctx->ipfw_track_list, anchor, t_link);
	return (ipfw_track_expire_loop(ctx, anchor, scan_max, expire_max));
}

static void
ipfw_track_expire_more_dispatch(netmsg_t nm)
{
	struct ipfw_context *ctx = ipfw_ctx[mycpuid];
	struct ipfw_track *anchor;

	ASSERT_NETISR_NCPUS(mycpuid);
	KASSERT(ctx->ipfw_flags & IPFW_FLAG_TRACKEXP,
	    ("trackexp is not in progress"));

	/* Reply ASAP */
	netisr_replymsg(&nm->base, 0);

	anchor = &ctx->ipfw_trackexp_anch;
	if (RB_EMPTY(&ctx->ipfw_track_tree)) {
		TAILQ_REMOVE(&ctx->ipfw_track_list, anchor, t_link);
		ipfw_track_expire_done(ctx);
		return;
	}
	ipfw_track_expire_loop(ctx, anchor,
	    ipfw_track_scan_max, ipfw_track_expire_max);
}

static void
ipfw_track_expire_dispatch(netmsg_t nm)
{
	struct ipfw_context *ctx = ipfw_ctx[mycpuid];

	ASSERT_NETISR_NCPUS(mycpuid);

	/* Reply ASAP */
	crit_enter();
	netisr_replymsg(&nm->base, 0);
	crit_exit();

	if (ctx->ipfw_flags & IPFW_FLAG_TRACKEXP) {
		/* Running; done. */
		return;
	}
	ipfw_track_expire_start(ctx,
	    ipfw_track_scan_max, ipfw_track_expire_max);
}

static void
ipfw_track_expire_ipifunc(void *dummy __unused)
{
	struct netmsg_base *msg;

	KKASSERT(mycpuid < netisr_ncpus);
	msg = &ipfw_ctx[mycpuid]->ipfw_trackexp_nm;

	crit_enter();
	if (msg->lmsg.ms_flags & MSGF_DONE)
		netisr_sendmsg_oncpu(msg);
	crit_exit();
}

static int
ipfw_track_reap(struct ipfw_context *ctx)
{
	struct ipfw_track *t, *anchor;
	int expired;

	if ((ctx->ipfw_flags & IPFW_FLAG_TRACKEXP) == 0) {
		/*
		 * Kick start track expiring.  Ignore scan limit,
		 * we are short of tracks.
		 */
		ctx->ipfw_flags |= IPFW_FLAG_TRACKREAP;
		expired = ipfw_track_expire_start(ctx, INT_MAX,
		    ipfw_track_reap_max);
		ctx->ipfw_flags &= ~IPFW_FLAG_TRACKREAP;
		return (expired);
	}

	/*
	 * Tracks are being expired.
	 */

	if (RB_EMPTY(&ctx->ipfw_track_tree))
		return (0);

	expired = 0;
	anchor = &ctx->ipfw_trackexp_anch;
	while ((t = TAILQ_NEXT(anchor, t_link)) != NULL) {
		/*
		 * Ignore scan limit; we are short of tracks.
		 */

		TAILQ_REMOVE(&ctx->ipfw_track_list, anchor, t_link);
		TAILQ_INSERT_AFTER(&ctx->ipfw_track_list, t, anchor, t_link);

		if (t->t_count == NULL) /* anchor */
			continue;

		ipfw_track_state_expire(ctx, t, TRUE);
		if (!LIST_EMPTY(&t->t_state_list)) {
			/* There are states referencing this track. */
			continue;
		}

		if (ipfw_track_free(ctx, t)) {
			if (++expired >= ipfw_track_reap_max) {
				ipfw_track_expire_more(ctx);
				break;
			}
		}
	}
	/*
	 * NOTE:
	 * Leave the anchor on the list, even if the end of the list has
	 * been reached.  ipfw_track_expire_more_dispatch() will handle
	 * the removal.
	 */
	return (expired);
}

static struct ipfw_track *
ipfw_track_alloc(struct ipfw_context *ctx, const struct ipfw_flow_id *id,
    uint16_t limit_mask, struct ip_fw *rule)
{
	struct ipfw_track *key, *t, *dup;
	struct ipfw_trkcnt *trk, *ret;
	boolean_t do_expire = FALSE;

	KASSERT(rule->track_ruleid != 0,
	    ("rule %u has no track ruleid", rule->rulenum));

	key = &ctx->ipfw_track_tmpkey;
	key->t_proto = id->proto;
	key->t_addrs = 0;
	key->t_ports = 0;
	key->t_rule = rule;
	if (limit_mask & DYN_SRC_ADDR)
		key->t_saddr = id->src_ip;
	if (limit_mask & DYN_DST_ADDR)
		key->t_daddr = id->dst_ip;
	if (limit_mask & DYN_SRC_PORT)
		key->t_sport = id->src_port;
	if (limit_mask & DYN_DST_PORT)
		key->t_dport = id->dst_port;

	t = RB_FIND(ipfw_track_tree, &ctx->ipfw_track_tree, key);
	if (t != NULL)
		goto done;

	t = kmalloc(sizeof(*t), M_IPFW, M_INTWAIT | M_NULLOK);
	if (t == NULL) {
		ctx->ipfw_tks_nomem++;
		return (NULL);
	}

	t->t_key = key->t_key;
	t->t_rule = rule;
	t->t_lastexp = 0;
	LIST_INIT(&t->t_state_list);

	if (ipfw_gd.ipfw_trkcnt_cnt >= ipfw_track_max) {
		time_t globexp, uptime;

		trk = NULL;
		do_expire = TRUE;

		/*
		 * Do not expire globally more than once per second,
		 * it is useless.
		 */
		uptime = time_uptime;
		globexp = ipfw_gd.ipfw_track_globexp;
		if (globexp != uptime &&
		    atomic_cmpset_long(&ipfw_gd.ipfw_track_globexp,
		    globexp, uptime)) {
			int cpu;

			/* Expire tracks on other CPUs. */
			for (cpu = 0; cpu < netisr_ncpus; ++cpu) {
				if (cpu == mycpuid)
					continue;
				lwkt_send_ipiq(globaldata_find(cpu),
				    ipfw_track_expire_ipifunc, NULL);
			}
		}
	} else {
		trk = ipfw_trkcnt_alloc(ctx);
	}
	if (trk == NULL) {
		struct ipfw_trkcnt *tkey;

		tkey = &ctx->ipfw_trkcnt_tmpkey;
		key = NULL; /* tkey overlaps key */

		tkey->tc_key = t->t_key;
		tkey->tc_ruleid = rule->track_ruleid;

		IPFW_TRKCNT_TOKGET;
		trk = RB_FIND(ipfw_trkcnt_tree, &ipfw_gd.ipfw_trkcnt_tree,
		    tkey);
		if (trk == NULL) {
			IPFW_TRKCNT_TOKREL;
			if (do_expire) {
				ctx->ipfw_tks_reap++;
				if (ipfw_track_reap(ctx) > 0) {
					if (ipfw_gd.ipfw_trkcnt_cnt <
					    ipfw_track_max) {
						trk = ipfw_trkcnt_alloc(ctx);
						if (trk != NULL)
							goto install;
						ctx->ipfw_tks_cntnomem++;
					} else {
						ctx->ipfw_tks_overflow++;
					}
				} else {
					ctx->ipfw_tks_reapfailed++;
					ctx->ipfw_tks_overflow++;
				}
			} else {
				ctx->ipfw_tks_cntnomem++;
			}
			kfree(t, M_IPFW);
			return (NULL);
		}
		KASSERT(trk->tc_refs > 0 && trk->tc_refs < netisr_ncpus,
		    ("invalid trkcnt refs %d", trk->tc_refs));
		atomic_add_int(&trk->tc_refs, 1);
		IPFW_TRKCNT_TOKREL;
	} else {
install:
		trk->tc_key = t->t_key;
		trk->tc_ruleid = rule->track_ruleid;
		trk->tc_refs = 0;
		trk->tc_count = 0;
		trk->tc_expire = 0;
		trk->tc_rulenum = rule->rulenum;

		IPFW_TRKCNT_TOKGET;
		ret = RB_INSERT(ipfw_trkcnt_tree, &ipfw_gd.ipfw_trkcnt_tree,
		    trk);
		if (ret != NULL) {
			KASSERT(ret->tc_refs > 0 &&
			    ret->tc_refs < netisr_ncpus,
			    ("invalid trkcnt refs %d", ret->tc_refs));
			KASSERT(ctx->ipfw_trkcnt_spare == NULL,
			    ("trkcnt spare was installed"));
			ctx->ipfw_trkcnt_spare = trk;
			trk = ret;
		} else {
			ipfw_gd.ipfw_trkcnt_cnt++;
		}
		atomic_add_int(&trk->tc_refs, 1);
		IPFW_TRKCNT_TOKREL;
	}
	t->t_count = &trk->tc_count;
	t->t_trkcnt = trk;

	dup = RB_INSERT(ipfw_track_tree, &ctx->ipfw_track_tree, t);
	if (dup != NULL)
		panic("ipfw: track exists");
	TAILQ_INSERT_TAIL(&ctx->ipfw_track_list, t, t_link);
done:
	t->t_expire = time_uptime + dyn_short_lifetime;
	return (t);
}

/*
 * Install state for rule type cmd->o.opcode
 *
 * Returns 1 (failure) if state is not installed because of errors or because
 * states limitations are enforced.
 */
static int
ipfw_state_install(struct ipfw_context *ctx, struct ip_fw *rule,
    ipfw_insn_limit *cmd, struct ip_fw_args *args, const struct tcphdr *tcp)
{
	struct ipfw_state *s;
	struct ipfw_track *t;
	int count, diff;

	if (ipfw_gd.ipfw_state_loosecnt >= ipfw_state_max &&
	    (diff = (ipfw_state_cntsync() - ipfw_state_max)) >= 0) {
		boolean_t overflow = TRUE;

		ctx->ipfw_sts_reap++;
		if (ipfw_state_reap(ctx, diff) == 0)
			ctx->ipfw_sts_reapfailed++;
		if (ipfw_state_cntsync() < ipfw_state_max)
			overflow = FALSE;

		if (overflow) {
			time_t globexp, uptime;
			int cpu;

			/*
			 * Do not expire globally more than once per second,
			 * it is useless.
			 */
			uptime = time_uptime;
			globexp = ipfw_gd.ipfw_state_globexp;
			if (globexp == uptime ||
			    !atomic_cmpset_long(&ipfw_gd.ipfw_state_globexp,
			    globexp, uptime)) {
				ctx->ipfw_sts_overflow++;
				return (1);
			}

			/* Expire states on other CPUs. */
			for (cpu = 0; cpu < netisr_ncpus; ++cpu) {
				if (cpu == mycpuid)
					continue;
				lwkt_send_ipiq(globaldata_find(cpu),
				    ipfw_state_expire_ipifunc, NULL);
			}
			ctx->ipfw_sts_overflow++;
			return (1);
		}
	}

	switch (cmd->o.opcode) {
	case O_KEEP_STATE: /* bidir rule */
		s = ipfw_state_add(ctx, &args->f_id, O_KEEP_STATE, rule, NULL,
		    tcp);
		if (s == NULL)
			return (1);
		break;

	case O_LIMIT: /* limit number of sessions */
		t = ipfw_track_alloc(ctx, &args->f_id, cmd->limit_mask, rule);
		if (t == NULL)
			return (1);

		if (*t->t_count >= cmd->conn_limit) {
			if (!ipfw_track_state_expire(ctx, t, TRUE))
				return (1);
		}
		for (;;) {
			count = *t->t_count;
			if (count >= cmd->conn_limit)
				return (1);
			if (atomic_cmpset_int(t->t_count, count, count + 1))
				break;
		}

		s = ipfw_state_add(ctx, &args->f_id, O_LIMIT, rule, t, tcp);
		if (s == NULL) {
			/* Undo damage. */
			atomic_subtract_int(t->t_count, 1);
			return (1);
		}
		break;

	default:
		panic("unknown state type %u\n", cmd->o.opcode);
	}
	return (0);
}

/*
 * Transmit a TCP packet, containing either a RST or a keepalive.
 * When flags & TH_RST, we are sending a RST packet, because of a
 * "reset" action matched the packet.
 * Otherwise we are sending a keepalive, and flags & TH_
 *
 * Only {src,dst}_{ip,port} of "id" are used.
 */
static void
send_pkt(const struct ipfw_flow_id *id, uint32_t seq, uint32_t ack, int flags)
{
	struct mbuf *m;
	struct ip *ip;
	struct tcphdr *tcp;
	struct route sro;	/* fake route */

	MGETHDR(m, M_NOWAIT, MT_HEADER);
	if (m == NULL)
		return;
	m->m_pkthdr.rcvif = NULL;
	m->m_pkthdr.len = m->m_len = sizeof(struct ip) + sizeof(struct tcphdr);
	m->m_data += max_linkhdr;

	ip = mtod(m, struct ip *);
	bzero(ip, m->m_len);
	tcp = (struct tcphdr *)(ip + 1); /* no IP options */
	ip->ip_p = IPPROTO_TCP;
	tcp->th_off = 5;

	/*
	 * Assume we are sending a RST (or a keepalive in the reverse
	 * direction), swap src and destination addresses and ports.
	 */
	ip->ip_src.s_addr = htonl(id->dst_ip);
	ip->ip_dst.s_addr = htonl(id->src_ip);
	tcp->th_sport = htons(id->dst_port);
	tcp->th_dport = htons(id->src_port);
	if (flags & TH_RST) {	/* we are sending a RST */
		if (flags & TH_ACK) {
			tcp->th_seq = htonl(ack);
			tcp->th_ack = htonl(0);
			tcp->th_flags = TH_RST;
		} else {
			if (flags & TH_SYN)
				seq++;
			tcp->th_seq = htonl(0);
			tcp->th_ack = htonl(seq);
			tcp->th_flags = TH_RST | TH_ACK;
		}
	} else {
		/*
		 * We are sending a keepalive. flags & TH_SYN determines
		 * the direction, forward if set, reverse if clear.
		 * NOTE: seq and ack are always assumed to be correct
		 * as set by the caller. This may be confusing...
		 */
		if (flags & TH_SYN) {
			/*
			 * we have to rewrite the correct addresses!
			 */
			ip->ip_dst.s_addr = htonl(id->dst_ip);
			ip->ip_src.s_addr = htonl(id->src_ip);
			tcp->th_dport = htons(id->dst_port);
			tcp->th_sport = htons(id->src_port);
		}
		tcp->th_seq = htonl(seq);
		tcp->th_ack = htonl(ack);
		tcp->th_flags = TH_ACK;
	}

	/*
	 * set ip_len to the payload size so we can compute
	 * the tcp checksum on the pseudoheader
	 * XXX check this, could save a couple of words ?
	 */
	ip->ip_len = htons(sizeof(struct tcphdr));
	tcp->th_sum = in_cksum(m, m->m_pkthdr.len);

	/*
	 * now fill fields left out earlier
	 */
	ip->ip_ttl = ip_defttl;
	ip->ip_len = m->m_pkthdr.len;

	bzero(&sro, sizeof(sro));
	ip_rtaddr(ip->ip_dst, &sro);

	m->m_pkthdr.fw_flags |= IPFW_MBUF_GENERATED;
	ip_output(m, NULL, &sro, 0, NULL, NULL);
	if (sro.ro_rt)
		RTFREE(sro.ro_rt);
}

/*
 * Send a reject message, consuming the mbuf passed as an argument.
 */
static void
send_reject(struct ip_fw_args *args, int code, int offset, int ip_len)
{
	if (code != ICMP_REJECT_RST) { /* Send an ICMP unreach */
		/* We need the IP header in host order for icmp_error(). */
		if (args->eh != NULL) {
			struct ip *ip = mtod(args->m, struct ip *);

			ip->ip_len = ntohs(ip->ip_len);
			ip->ip_off = ntohs(ip->ip_off);
		}
		icmp_error(args->m, ICMP_UNREACH, code, 0L, 0);
	} else if (offset == 0 && args->f_id.proto == IPPROTO_TCP) {
		struct tcphdr *const tcp =
		    L3HDR(struct tcphdr, mtod(args->m, struct ip *));

		if ((tcp->th_flags & TH_RST) == 0) {
			send_pkt(&args->f_id, ntohl(tcp->th_seq),
				 ntohl(tcp->th_ack), tcp->th_flags | TH_RST);
		}
		m_freem(args->m);
	} else {
		m_freem(args->m);
	}
	args->m = NULL;
}

/*
 * Given an ip_fw *, lookup_next_rule will return a pointer
 * to the next rule, which can be either the jump
 * target (for skipto instructions) or the next one in the list (in
 * all other cases including a missing jump target).
 * The result is also written in the "next_rule" field of the rule.
 * Backward jumps are not allowed, so start looking from the next
 * rule...
 *
 * This never returns NULL -- in case we do not have an exact match,
 * the next rule is returned. When the ruleset is changed,
 * pointers are flushed so we are always correct.
 */
static struct ip_fw *
lookup_next_rule(struct ip_fw *me)
{
	struct ip_fw *rule = NULL;
	ipfw_insn *cmd;

	/* look for action, in case it is a skipto */
	cmd = ACTION_PTR(me);
	if (cmd->opcode == O_LOG)
		cmd += F_LEN(cmd);
	if (cmd->opcode == O_SKIPTO) {
		for (rule = me->next; rule; rule = rule->next) {
			if (rule->rulenum >= cmd->arg1)
				break;
		}
	}
	if (rule == NULL)			/* failure or not a skipto */
		rule = me->next;
	me->next_rule = rule;
	return rule;
}

static int
ipfw_match_uid(const struct ipfw_flow_id *fid, struct ifnet *oif,
		enum ipfw_opcodes opcode, uid_t uid)
{
	struct in_addr src_ip, dst_ip;
	struct inpcbinfo *pi;
	boolean_t wildcard;
	struct inpcb *pcb;

	if (fid->proto == IPPROTO_TCP) {
		wildcard = FALSE;
		pi = &tcbinfo[mycpuid];
	} else if (fid->proto == IPPROTO_UDP) {
		wildcard = TRUE;
		pi = &udbinfo[mycpuid];
	} else {
		return 0;
	}

	/*
	 * Values in 'fid' are in host byte order
	 */
	dst_ip.s_addr = htonl(fid->dst_ip);
	src_ip.s_addr = htonl(fid->src_ip);
	if (oif) {
		pcb = in_pcblookup_hash(pi,
			dst_ip, htons(fid->dst_port),
			src_ip, htons(fid->src_port),
			wildcard, oif);
	} else {
		pcb = in_pcblookup_hash(pi,
			src_ip, htons(fid->src_port),
			dst_ip, htons(fid->dst_port),
			wildcard, NULL);
	}
	if (pcb == NULL || pcb->inp_socket == NULL)
		return 0;

	if (opcode == O_UID) {
#define socheckuid(a,b)	((a)->so_cred->cr_uid != (b))
		return !socheckuid(pcb->inp_socket, uid);
#undef socheckuid
	} else  {
		return groupmember(uid, pcb->inp_socket->so_cred);
	}
}

/*
 * The main check routine for the firewall.
 *
 * All arguments are in args so we can modify them and return them
 * back to the caller.
 *
 * Parameters:
 *
 *	args->m	(in/out) The packet; we set to NULL when/if we nuke it.
 *		Starts with the IP header.
 *	args->eh (in)	Mac header if present, or NULL for layer3 packet.
 *	args->oif	Outgoing interface, or NULL if packet is incoming.
 *		The incoming interface is in the mbuf. (in)
 *
 *	args->rule	Pointer to the last matching rule (in/out)
 *	args->f_id	Addresses grabbed from the packet (out)
 *
 * Return value:
 *
 *	If the packet was denied/rejected and has been dropped, *m is equal
 *	to NULL upon return.
 *
 *	IP_FW_DENY	the packet must be dropped.
 *	IP_FW_PASS	The packet is to be accepted and routed normally.
 *	IP_FW_DIVERT	Divert the packet to port (args->cookie)
 *	IP_FW_TEE	Tee the packet to port (args->cookie)
 *	IP_FW_DUMMYNET	Send the packet to pipe/queue (args->cookie)
 */
static int
ipfw_chk(struct ip_fw_args *args)
{
	/*
	 * Local variables hold state during the processing of a packet.
	 *
	 * IMPORTANT NOTE: to speed up the processing of rules, there
	 * are some assumption on the values of the variables, which
	 * are documented here. Should you change them, please check
	 * the implementation of the various instructions to make sure
	 * that they still work.
	 *
	 * args->eh	The MAC header. It is non-null for a layer2
	 *	packet, it is NULL for a layer-3 packet.
	 *
	 * m | args->m	Pointer to the mbuf, as received from the caller.
	 *	It may change if ipfw_chk() does an m_pullup, or if it
	 *	consumes the packet because it calls send_reject().
	 *	XXX This has to change, so that ipfw_chk() never modifies
	 *	or consumes the buffer.
	 * ip	is simply an alias of the value of m, and it is kept
	 *	in sync with it (the packet is	supposed to start with
	 *	the ip header).
	 */
	struct mbuf *m = args->m;
	struct ip *ip = mtod(m, struct ip *);

	/*
	 * oif | args->oif	If NULL, ipfw_chk has been called on the
	 *	inbound path (ether_input, ip_input).
	 *	If non-NULL, ipfw_chk has been called on the outbound path
	 *	(ether_output, ip_output).
	 */
	struct ifnet *oif = args->oif;

	struct ip_fw *f = NULL;		/* matching rule */
	int retval = IP_FW_PASS;
	struct m_tag *mtag;
	struct divert_info *divinfo;

	/*
	 * hlen	The length of the IPv4 header.
	 *	hlen >0 means we have an IPv4 packet.
	 */
	u_int hlen = 0;		/* hlen >0 means we have an IP pkt */

	/*
	 * offset	The offset of a fragment. offset != 0 means that
	 *	we have a fragment at this offset of an IPv4 packet.
	 *	offset == 0 means that (if this is an IPv4 packet)
	 *	this is the first or only fragment.
	 */
	u_short offset = 0;

	/*
	 * Local copies of addresses. They are only valid if we have
	 * an IP packet.
	 *
	 * proto	The protocol. Set to 0 for non-ip packets,
	 *	or to the protocol read from the packet otherwise.
	 *	proto != 0 means that we have an IPv4 packet.
	 *
	 * src_port, dst_port	port numbers, in HOST format. Only
	 *	valid for TCP and UDP packets.
	 *
	 * src_ip, dst_ip	ip addresses, in NETWORK format.
	 *	Only valid for IPv4 packets.
	 */
	uint8_t proto;
	uint16_t src_port = 0, dst_port = 0;	/* NOTE: host format	*/
	struct in_addr src_ip, dst_ip;		/* NOTE: network format	*/
	uint16_t ip_len = 0;

	/*
	 * dyn_dir = MATCH_UNKNOWN when rules unchecked,
	 * 	MATCH_NONE when checked and not matched (dyn_f = NULL),
	 *	MATCH_FORWARD or MATCH_REVERSE otherwise (dyn_f != NULL)
	 */
	int dyn_dir = MATCH_UNKNOWN;
	struct ip_fw *dyn_f = NULL;
	int cpuid = mycpuid;
	struct ipfw_context *ctx;

	ASSERT_NETISR_NCPUS(cpuid);
	ctx = ipfw_ctx[cpuid];

	if (m->m_pkthdr.fw_flags & IPFW_MBUF_GENERATED)
		return IP_FW_PASS;	/* accept */

	if (args->eh == NULL ||		/* layer 3 packet */
	    (m->m_pkthdr.len >= sizeof(struct ip) &&
	     ntohs(args->eh->ether_type) == ETHERTYPE_IP))
		hlen = ip->ip_hl << 2;

	/*
	 * Collect parameters into local variables for faster matching.
	 */
	if (hlen == 0) {	/* do not grab addresses for non-ip pkts */
		proto = args->f_id.proto = 0;	/* mark f_id invalid */
		goto after_ip_checks;
	}

	proto = args->f_id.proto = ip->ip_p;
	src_ip = ip->ip_src;
	dst_ip = ip->ip_dst;
	if (args->eh != NULL) { /* layer 2 packets are as on the wire */
		offset = ntohs(ip->ip_off) & IP_OFFMASK;
		ip_len = ntohs(ip->ip_len);
	} else {
		offset = ip->ip_off & IP_OFFMASK;
		ip_len = ip->ip_len;
	}

#define PULLUP_TO(len)				\
do {						\
	if (m->m_len < (len)) {			\
		args->m = m = m_pullup(m, (len));\
		if (m == NULL)			\
			goto pullup_failed;	\
		ip = mtod(m, struct ip *);	\
	}					\
} while (0)

	if (offset == 0) {
		switch (proto) {
		case IPPROTO_TCP:
			{
				struct tcphdr *tcp;

				PULLUP_TO(hlen + sizeof(struct tcphdr));
				tcp = L3HDR(struct tcphdr, ip);
				dst_port = tcp->th_dport;
				src_port = tcp->th_sport;
				args->f_id.flags = tcp->th_flags;
			}
			break;

		case IPPROTO_UDP:
			{
				struct udphdr *udp;

				PULLUP_TO(hlen + sizeof(struct udphdr));
				udp = L3HDR(struct udphdr, ip);
				dst_port = udp->uh_dport;
				src_port = udp->uh_sport;
			}
			break;

		case IPPROTO_ICMP:
			PULLUP_TO(hlen + 4);	/* type, code and checksum. */
			args->f_id.flags = L3HDR(struct icmp, ip)->icmp_type;
			break;

		default:
			break;
		}
	}

#undef PULLUP_TO

	args->f_id.src_ip = ntohl(src_ip.s_addr);
	args->f_id.dst_ip = ntohl(dst_ip.s_addr);
	args->f_id.src_port = src_port = ntohs(src_port);
	args->f_id.dst_port = dst_port = ntohs(dst_port);

after_ip_checks:
	if (args->rule) {
		/*
		 * Packet has already been tagged. Look for the next rule
		 * to restart processing.
		 *
		 * If fw_one_pass != 0 then just accept it.
		 * XXX should not happen here, but optimized out in
		 * the caller.
		 */
		if (fw_one_pass)
			return IP_FW_PASS;

		/* This rule is being/has been flushed */
		if (ipfw_flushing)
			return IP_FW_DENY;

		KASSERT(args->rule->cpuid == cpuid,
			("rule used on cpu%d", cpuid));

		/* This rule was deleted */
		if (args->rule->rule_flags & IPFW_RULE_F_INVALID)
			return IP_FW_DENY;

		f = args->rule->next_rule;
		if (f == NULL)
			f = lookup_next_rule(args->rule);
	} else {
		/*
		 * Find the starting rule. It can be either the first
		 * one, or the one after divert_rule if asked so.
		 */
		int skipto;

		mtag = m_tag_find(m, PACKET_TAG_IPFW_DIVERT, NULL);
		if (mtag != NULL) {
			divinfo = m_tag_data(mtag);
			skipto = divinfo->skipto;
		} else {
			skipto = 0;
		}

		f = ctx->ipfw_layer3_chain;
		if (args->eh == NULL && skipto != 0) {
			/* No skipto during rule flushing */
			if (ipfw_flushing)
				return IP_FW_DENY;

			if (skipto >= IPFW_DEFAULT_RULE)
				return IP_FW_DENY; /* invalid */

			while (f && f->rulenum <= skipto)
				f = f->next;
			if (f == NULL)	/* drop packet */
				return IP_FW_DENY;
		} else if (ipfw_flushing) {
			/* Rules are being flushed; skip to default rule */
			f = ctx->ipfw_default_rule;
		}
	}
	if ((mtag = m_tag_find(m, PACKET_TAG_IPFW_DIVERT, NULL)) != NULL)
		m_tag_delete(m, mtag);

	/*
	 * Now scan the rules, and parse microinstructions for each rule.
	 */
	for (; f; f = f->next) {
		int l, cmdlen;
		ipfw_insn *cmd;
		int skip_or; /* skip rest of OR block */

again:
		if (ctx->ipfw_set_disable & (1 << f->set))
			continue;

		skip_or = 0;
		for (l = f->cmd_len, cmd = f->cmd; l > 0;
		     l -= cmdlen, cmd += cmdlen) {
			int match;

			/*
			 * check_body is a jump target used when we find a
			 * CHECK_STATE, and need to jump to the body of
			 * the target rule.
			 */

check_body:
			cmdlen = F_LEN(cmd);
			/*
			 * An OR block (insn_1 || .. || insn_n) has the
			 * F_OR bit set in all but the last instruction.
			 * The first match will set "skip_or", and cause
			 * the following instructions to be skipped until
			 * past the one with the F_OR bit clear.
			 */
			if (skip_or) {		/* skip this instruction */
				if ((cmd->len & F_OR) == 0)
					skip_or = 0;	/* next one is good */
				continue;
			}
			match = 0; /* set to 1 if we succeed */

			switch (cmd->opcode) {
			/*
			 * The first set of opcodes compares the packet's
			 * fields with some pattern, setting 'match' if a
			 * match is found. At the end of the loop there is
			 * logic to deal with F_NOT and F_OR flags associated
			 * with the opcode.
			 */
			case O_NOP:
				match = 1;
				break;

			case O_FORWARD_MAC:
				kprintf("ipfw: opcode %d unimplemented\n",
					cmd->opcode);
				break;

			case O_GID:
			case O_UID:
				/*
				 * We only check offset == 0 && proto != 0,
				 * as this ensures that we have an IPv4
				 * packet with the ports info.
				 */
				if (offset!=0)
					break;

				match = ipfw_match_uid(&args->f_id, oif,
					cmd->opcode,
					(uid_t)((ipfw_insn_u32 *)cmd)->d[0]);
				break;

			case O_RECV:
				match = iface_match(m->m_pkthdr.rcvif,
				    (ipfw_insn_if *)cmd);
				break;

			case O_XMIT:
				match = iface_match(oif, (ipfw_insn_if *)cmd);
				break;

			case O_VIA:
				match = iface_match(oif ? oif :
				    m->m_pkthdr.rcvif, (ipfw_insn_if *)cmd);
				break;

			case O_MACADDR2:
				if (args->eh != NULL) {	/* have MAC header */
					uint32_t *want = (uint32_t *)
						((ipfw_insn_mac *)cmd)->addr;
					uint32_t *mask = (uint32_t *)
						((ipfw_insn_mac *)cmd)->mask;
					uint32_t *hdr = (uint32_t *)args->eh;

					match =
					(want[0] == (hdr[0] & mask[0]) &&
					 want[1] == (hdr[1] & mask[1]) &&
					 want[2] == (hdr[2] & mask[2]));
				}
				break;

			case O_MAC_TYPE:
				if (args->eh != NULL) {
					uint16_t t =
					    ntohs(args->eh->ether_type);
					uint16_t *p =
					    ((ipfw_insn_u16 *)cmd)->ports;
					int i;

					/* Special vlan handling */
					if (m->m_flags & M_VLANTAG)
						t = ETHERTYPE_VLAN;

					for (i = cmdlen - 1; !match && i > 0;
					     i--, p += 2) {
						match =
						(t >= p[0] && t <= p[1]);
					}
				}
				break;

			case O_FRAG:
				match = (hlen > 0 && offset != 0);
				break;

			case O_IN:	/* "out" is "not in" */
				match = (oif == NULL);
				break;

			case O_LAYER2:
				match = (args->eh != NULL);
				break;

			case O_PROTO:
				/*
				 * We do not allow an arg of 0 so the
				 * check of "proto" only suffices.
				 */
				match = (proto == cmd->arg1);
				break;

			case O_IP_SRC:
				match = (hlen > 0 &&
				    ((ipfw_insn_ip *)cmd)->addr.s_addr ==
				    src_ip.s_addr);
				break;

			case O_IP_SRC_MASK:
				match = (hlen > 0 &&
				    ((ipfw_insn_ip *)cmd)->addr.s_addr ==
				     (src_ip.s_addr &
				     ((ipfw_insn_ip *)cmd)->mask.s_addr));
				break;

			case O_IP_SRC_ME:
				if (hlen > 0) {
					struct ifnet *tif;

					tif = INADDR_TO_IFP(&src_ip);
					match = (tif != NULL);
				}
				break;

			case O_IP_DST_SET:
			case O_IP_SRC_SET:
				if (hlen > 0) {
					uint32_t *d = (uint32_t *)(cmd + 1);
					uint32_t addr =
					    cmd->opcode == O_IP_DST_SET ?
						args->f_id.dst_ip :
						args->f_id.src_ip;

					if (addr < d[0])
						break;
					addr -= d[0]; /* subtract base */
					match =
					(addr < cmd->arg1) &&
					 (d[1 + (addr >> 5)] &
					  (1 << (addr & 0x1f)));
				}
				break;

			case O_IP_DST:
				match = (hlen > 0 &&
				    ((ipfw_insn_ip *)cmd)->addr.s_addr ==
				    dst_ip.s_addr);
				break;

			case O_IP_DST_MASK:
				match = (hlen > 0) &&
				    (((ipfw_insn_ip *)cmd)->addr.s_addr ==
				     (dst_ip.s_addr &
				     ((ipfw_insn_ip *)cmd)->mask.s_addr));
				break;

			case O_IP_DST_ME:
				if (hlen > 0) {
					struct ifnet *tif;

					tif = INADDR_TO_IFP(&dst_ip);
					match = (tif != NULL);
				}
				break;

			case O_IP_SRCPORT:
			case O_IP_DSTPORT:
				/*
				 * offset == 0 && proto != 0 is enough
				 * to guarantee that we have an IPv4
				 * packet with port info.
				 */
				if ((proto==IPPROTO_UDP || proto==IPPROTO_TCP)
				    && offset == 0) {
					uint16_t x =
					    (cmd->opcode == O_IP_SRCPORT) ?
						src_port : dst_port ;
					uint16_t *p =
					    ((ipfw_insn_u16 *)cmd)->ports;
					int i;

					for (i = cmdlen - 1; !match && i > 0;
					     i--, p += 2) {
						match =
						(x >= p[0] && x <= p[1]);
					}
				}
				break;

			case O_ICMPTYPE:
				match = (offset == 0 && proto==IPPROTO_ICMP &&
				    icmptype_match(ip, (ipfw_insn_u32 *)cmd));
				break;

			case O_IPOPT:
				match = (hlen > 0 && ipopts_match(ip, cmd));
				break;

			case O_IPVER:
				match = (hlen > 0 && cmd->arg1 == ip->ip_v);
				break;

			case O_IPTTL:
				match = (hlen > 0 && cmd->arg1 == ip->ip_ttl);
				break;

			case O_IPID:
				match = (hlen > 0 &&
				    cmd->arg1 == ntohs(ip->ip_id));
				break;

			case O_IPLEN:
				match = (hlen > 0 && cmd->arg1 == ip_len);
				break;

			case O_IPPRECEDENCE:
				match = (hlen > 0 &&
				    (cmd->arg1 == (ip->ip_tos & 0xe0)));
				break;

			case O_IPTOS:
				match = (hlen > 0 &&
				    flags_match(cmd, ip->ip_tos));
				break;

			case O_TCPFLAGS:
				match = (proto == IPPROTO_TCP && offset == 0 &&
				    flags_match(cmd,
					L3HDR(struct tcphdr,ip)->th_flags));
				break;

			case O_TCPOPTS:
				match = (proto == IPPROTO_TCP && offset == 0 &&
				    tcpopts_match(ip, cmd));
				break;

			case O_TCPSEQ:
				match = (proto == IPPROTO_TCP && offset == 0 &&
				    ((ipfw_insn_u32 *)cmd)->d[0] ==
					L3HDR(struct tcphdr,ip)->th_seq);
				break;

			case O_TCPACK:
				match = (proto == IPPROTO_TCP && offset == 0 &&
				    ((ipfw_insn_u32 *)cmd)->d[0] ==
					L3HDR(struct tcphdr,ip)->th_ack);
				break;

			case O_TCPWIN:
				match = (proto == IPPROTO_TCP && offset == 0 &&
				    cmd->arg1 ==
					L3HDR(struct tcphdr,ip)->th_win);
				break;

			case O_ESTAB:
				/* reject packets which have SYN only */
				/* XXX should i also check for TH_ACK ? */
				match = (proto == IPPROTO_TCP && offset == 0 &&
				    (L3HDR(struct tcphdr,ip)->th_flags &
				     (TH_RST | TH_ACK | TH_SYN)) != TH_SYN);
				break;

			case O_LOG:
				if (fw_verbose) {
					ipfw_log(ctx, f, hlen, args->eh, m,
					    oif);
				}
				match = 1;
				break;

			case O_PROB:
				match = (krandom() <
					((ipfw_insn_u32 *)cmd)->d[0]);
				break;

			/*
			 * The second set of opcodes represents 'actions',
			 * i.e. the terminal part of a rule once the packet
			 * matches all previous patterns.
			 * Typically there is only one action for each rule,
			 * and the opcode is stored at the end of the rule
			 * (but there are exceptions -- see below).
			 *
			 * In general, here we set retval and terminate the
			 * outer loop (would be a 'break 3' in some language,
			 * but we need to do a 'goto done').
			 *
			 * Exceptions:
			 * O_COUNT and O_SKIPTO actions:
			 *   instead of terminating, we jump to the next rule
			 *   ('goto next_rule', equivalent to a 'break 2'),
			 *   or to the SKIPTO target ('goto again' after
			 *   having set f, cmd and l), respectively.
			 *
			 * O_LIMIT and O_KEEP_STATE: these opcodes are
			 *   not real 'actions', and are stored right
			 *   before the 'action' part of the rule.
			 *   These opcodes try to install an entry in the
			 *   state tables; if successful, we continue with
			 *   the next opcode (match=1; break;), otherwise
			 *   the packet must be dropped ('goto done' after
			 *   setting retval).  If static rules are changed
			 *   during the state installation, the packet will
			 *   be dropped and rule's stats will not beupdated
			 *   ('return IP_FW_DENY').
			 *
			 * O_PROBE_STATE and O_CHECK_STATE: these opcodes
			 *   cause a lookup of the state table, and a jump
			 *   to the 'action' part of the parent rule
			 *   ('goto check_body') if an entry is found, or
			 *   (CHECK_STATE only) a jump to the next rule if
			 *   the entry is not found ('goto next_rule').
			 *   The result of the lookup is cached to make
			 *   further instances of these opcodes are
			 *   effectively NOPs.  If static rules are changed
			 *   during the state looking up, the packet will
			 *   be dropped and rule's stats will not be updated
			 *   ('return IP_FW_DENY').
			 */
			case O_LIMIT:
			case O_KEEP_STATE:
				if (ipfw_state_install(ctx, f,
				    (ipfw_insn_limit *)cmd, args,
				    (offset == 0 && proto == IPPROTO_TCP) ?
				    L3HDR(struct tcphdr, ip) : NULL)) {
					retval = IP_FW_DENY;
					goto done; /* error/limit violation */
				}
				match = 1;
				break;

			case O_PROBE_STATE:
			case O_CHECK_STATE:
				/*
				 * States are checked at the first keep-state 
				 * check-state occurrence, with the result
				 * being stored in dyn_dir.  The compiler
				 * introduces a PROBE_STATE instruction for
				 * us when we have a KEEP_STATE/LIMIT (because
				 * PROBE_STATE needs to be run first).
				 */
				if (dyn_dir == MATCH_UNKNOWN) {
					dyn_f = ipfw_state_lookup_rule(ctx,
					    &args->f_id, &dyn_dir,
					    (offset == 0 &&
					     proto == IPPROTO_TCP) ?
					    L3HDR(struct tcphdr, ip) : NULL,
					    ip_len);
					if (dyn_f != NULL) {
						/*
						 * Found a rule from a state;
						 * jump to the 'action' part
						 * of the rule.
						 */
						f = dyn_f;
						cmd = ACTION_PTR(f);
						l = f->cmd_len - f->act_ofs;
						goto check_body;
					}
				}
				/*
				 * State not found. If CHECK_STATE, skip to
				 * next rule, if PROBE_STATE just ignore and
				 * continue with next opcode.
				 */
				if (cmd->opcode == O_CHECK_STATE)
					goto next_rule;
				match = 1;
				break;

			case O_ACCEPT:
				retval = IP_FW_PASS;	/* accept */
				goto done;

			case O_PIPE:
			case O_QUEUE:
				args->rule = f; /* report matching rule */
				args->cookie = cmd->arg1;
				retval = IP_FW_DUMMYNET;
				goto done;

			case O_DIVERT:
			case O_TEE:
				if (args->eh) /* not on layer 2 */
					break;

				mtag = m_tag_get(PACKET_TAG_IPFW_DIVERT,
						 sizeof(*divinfo), M_NOWAIT);
				if (mtag == NULL) {
					retval = IP_FW_DENY;
					goto done;
				}
				divinfo = m_tag_data(mtag);

				divinfo->skipto = f->rulenum;
				divinfo->port = cmd->arg1;
				divinfo->tee = (cmd->opcode == O_TEE);
				m_tag_prepend(m, mtag);

				args->cookie = cmd->arg1;
				retval = (cmd->opcode == O_DIVERT) ?
					 IP_FW_DIVERT : IP_FW_TEE;
				goto done;

			case O_COUNT:
			case O_SKIPTO:
				f->pcnt++;	/* update stats */
				f->bcnt += ip_len;
				f->timestamp = time_second;
				if (cmd->opcode == O_COUNT)
					goto next_rule;
				/* handle skipto */
				if (f->next_rule == NULL)
					lookup_next_rule(f);
				f = f->next_rule;
				goto again;

			case O_REJECT:
				/*
				 * Drop the packet and send a reject notice
				 * if the packet is not ICMP (or is an ICMP
				 * query), and it is not multicast/broadcast.
				 */
				if (hlen > 0 &&
				    (proto != IPPROTO_ICMP ||
				     is_icmp_query(ip)) &&
				    !(m->m_flags & (M_BCAST|M_MCAST)) &&
				    !IN_MULTICAST(ntohl(dst_ip.s_addr))) {
					/*
					 * Update statistics before the possible
					 * blocking 'send_reject'
					 */
					f->pcnt++;
					f->bcnt += ip_len;
					f->timestamp = time_second;

					send_reject(args, cmd->arg1,
					    offset,ip_len);
					m = args->m;

					/*
					 * Return directly here, rule stats
					 * have been updated above.
					 */
					return IP_FW_DENY;
				}
				/* FALLTHROUGH */
			case O_DENY:
				retval = IP_FW_DENY;
				goto done;

			case O_FORWARD_IP:
				if (args->eh)	/* not valid on layer2 pkts */
					break;
				if (!dyn_f || dyn_dir == MATCH_FORWARD) {
					struct sockaddr_in *sin;

					mtag = m_tag_get(PACKET_TAG_IPFORWARD,
					       sizeof(*sin), M_NOWAIT);
					if (mtag == NULL) {
						retval = IP_FW_DENY;
						goto done;
					}
					sin = m_tag_data(mtag);

					/* Structure copy */
					*sin = ((ipfw_insn_sa *)cmd)->sa;

					m_tag_prepend(m, mtag);
					m->m_pkthdr.fw_flags |=
						IPFORWARD_MBUF_TAGGED;
					m->m_pkthdr.fw_flags &=
						~BRIDGE_MBUF_TAGGED;
				}
				retval = IP_FW_PASS;
				goto done;

			default:
				panic("-- unknown opcode %d", cmd->opcode);
			} /* end of switch() on opcodes */

			if (cmd->len & F_NOT)
				match = !match;

			if (match) {
				if (cmd->len & F_OR)
					skip_or = 1;
			} else {
				if (!(cmd->len & F_OR)) /* not an OR block, */
					break;		/* try next rule    */
			}

		}	/* end of inner for, scan opcodes */

next_rule:;		/* try next rule		*/

	}		/* end of outer for, scan rules */
	kprintf("+++ ipfw: ouch!, skip past end of rules, denying packet\n");
	return IP_FW_DENY;

done:
	/* Update statistics */
	f->pcnt++;
	f->bcnt += ip_len;
	f->timestamp = time_second;
	return retval;

pullup_failed:
	if (fw_verbose)
		kprintf("pullup failed\n");
	return IP_FW_DENY;
}

static void
ipfw_dummynet_io(struct mbuf *m, int pipe_nr, int dir, struct ip_fw_args *fwa)
{
	struct m_tag *mtag;
	struct dn_pkt *pkt;
	ipfw_insn *cmd;
	const struct ipfw_flow_id *id;
	struct dn_flow_id *fid;

	M_ASSERTPKTHDR(m);

	mtag = m_tag_get(PACKET_TAG_DUMMYNET, sizeof(*pkt), M_NOWAIT);
	if (mtag == NULL) {
		m_freem(m);
		return;
	}
	m_tag_prepend(m, mtag);

	pkt = m_tag_data(mtag);
	bzero(pkt, sizeof(*pkt));

	cmd = fwa->rule->cmd + fwa->rule->act_ofs;
	if (cmd->opcode == O_LOG)
		cmd += F_LEN(cmd);
	KASSERT(cmd->opcode == O_PIPE || cmd->opcode == O_QUEUE,
		("Rule is not PIPE or QUEUE, opcode %d", cmd->opcode));

	pkt->dn_m = m;
	pkt->dn_flags = (dir & DN_FLAGS_DIR_MASK);
	pkt->ifp = fwa->oif;
	pkt->pipe_nr = pipe_nr;

	pkt->cpuid = mycpuid;
	pkt->msgport = netisr_curport();

	id = &fwa->f_id;
	fid = &pkt->id;
	fid->fid_dst_ip = id->dst_ip;
	fid->fid_src_ip = id->src_ip;
	fid->fid_dst_port = id->dst_port;
	fid->fid_src_port = id->src_port;
	fid->fid_proto = id->proto;
	fid->fid_flags = id->flags;

	ipfw_ref_rule(fwa->rule);
	pkt->dn_priv = fwa->rule;
	pkt->dn_unref_priv = ipfw_unref_rule;

	if (cmd->opcode == O_PIPE)
		pkt->dn_flags |= DN_FLAGS_IS_PIPE;

	m->m_pkthdr.fw_flags |= DUMMYNET_MBUF_TAGGED;
}

/*
 * When a rule is added/deleted, clear the next_rule pointers in all rules.
 * These will be reconstructed on the fly as packets are matched.
 */
static void
ipfw_flush_rule_ptrs(struct ipfw_context *ctx)
{
	struct ip_fw *rule;

	for (rule = ctx->ipfw_layer3_chain; rule; rule = rule->next)
		rule->next_rule = NULL;
}

static __inline void
ipfw_inc_static_count(struct ip_fw *rule)
{
	/* Static rule's counts are updated only on CPU0 */
	KKASSERT(mycpuid == 0);

	static_count++;
	static_ioc_len += IOC_RULESIZE(rule);
}

static __inline void
ipfw_dec_static_count(struct ip_fw *rule)
{
	int l = IOC_RULESIZE(rule);

	/* Static rule's counts are updated only on CPU0 */
	KKASSERT(mycpuid == 0);

	KASSERT(static_count > 0, ("invalid static count %u", static_count));
	static_count--;

	KASSERT(static_ioc_len >= l,
		("invalid static len %u", static_ioc_len));
	static_ioc_len -= l;
}

static void
ipfw_link_sibling(struct netmsg_ipfw *fwmsg, struct ip_fw *rule)
{
	if (fwmsg->sibling != NULL) {
		KKASSERT(mycpuid > 0 && fwmsg->sibling->cpuid == mycpuid - 1);
		fwmsg->sibling->sibling = rule;
	}
	fwmsg->sibling = rule;
}

static struct ip_fw *
ipfw_create_rule(const struct ipfw_ioc_rule *ioc_rule, uint32_t rule_flags)
{
	struct ip_fw *rule;

	rule = kmalloc(RULESIZE(ioc_rule), M_IPFW, M_WAITOK | M_ZERO);

	rule->act_ofs = ioc_rule->act_ofs;
	rule->cmd_len = ioc_rule->cmd_len;
	rule->rulenum = ioc_rule->rulenum;
	rule->set = ioc_rule->set;
	rule->usr_flags = ioc_rule->usr_flags;

	bcopy(ioc_rule->cmd, rule->cmd, rule->cmd_len * 4 /* XXX */);

	rule->refcnt = 1;
	rule->cpuid = mycpuid;
	rule->rule_flags = rule_flags;

	return rule;
}

static void
ipfw_add_rule_dispatch(netmsg_t nmsg)
{
	struct netmsg_ipfw *fwmsg = (struct netmsg_ipfw *)nmsg;
	struct ipfw_context *ctx = ipfw_ctx[mycpuid];
	struct ip_fw *rule;

	ASSERT_NETISR_NCPUS(mycpuid);

	rule = ipfw_create_rule(fwmsg->ioc_rule, fwmsg->rule_flags);

	/*
	 * Insert rule into the pre-determined position
	 */
	if (fwmsg->prev_rule != NULL) {
		struct ip_fw *prev, *next;

		prev = fwmsg->prev_rule;
		KKASSERT(prev->cpuid == mycpuid);

		next = fwmsg->next_rule;
		KKASSERT(next->cpuid == mycpuid);

		rule->next = next;
		prev->next = rule;

		/*
		 * Move to the position on the next CPU
		 * before the msg is forwarded.
		 */
		fwmsg->prev_rule = prev->sibling;
		fwmsg->next_rule = next->sibling;
	} else {
		KKASSERT(fwmsg->next_rule == NULL);
		rule->next = ctx->ipfw_layer3_chain;
		ctx->ipfw_layer3_chain = rule;
	}

	/* Link rule CPU sibling */
	ipfw_link_sibling(fwmsg, rule);

	ipfw_flush_rule_ptrs(ctx);

	if (mycpuid == 0) {
		/* Statistics only need to be updated once */
		ipfw_inc_static_count(rule);

		/* Return the rule on CPU0 */
		nmsg->lmsg.u.ms_resultp = rule;
	}

	if (rule->rule_flags & IPFW_RULE_F_GENTRACK)
		rule->track_ruleid = (uintptr_t)nmsg->lmsg.u.ms_resultp;

	netisr_forwardmsg(&nmsg->base, mycpuid + 1);
}

/*
 * Add a new rule to the list.  Copy the rule into a malloc'ed area,
 * then possibly create a rule number and add the rule to the list.
 * Update the rule_number in the input struct so the caller knows
 * it as well.
 */
static void
ipfw_add_rule(struct ipfw_ioc_rule *ioc_rule, uint32_t rule_flags)
{
	struct ipfw_context *ctx = ipfw_ctx[mycpuid];
	struct netmsg_ipfw fwmsg;
	struct netmsg_base *nmsg;
	struct ip_fw *f, *prev, *rule;

	ASSERT_NETISR0;

	/*
	 * If rulenum is 0, find highest numbered rule before the
	 * default rule, and add rule number incremental step.
	 */
	if (ioc_rule->rulenum == 0) {
		int step = autoinc_step;

		KKASSERT(step >= IPFW_AUTOINC_STEP_MIN &&
			 step <= IPFW_AUTOINC_STEP_MAX);

		/*
		 * Locate the highest numbered rule before default
		 */
		for (f = ctx->ipfw_layer3_chain; f; f = f->next) {
			if (f->rulenum == IPFW_DEFAULT_RULE)
				break;
			ioc_rule->rulenum = f->rulenum;
		}
		if (ioc_rule->rulenum < IPFW_DEFAULT_RULE - step)
			ioc_rule->rulenum += step;
	}
	KASSERT(ioc_rule->rulenum != IPFW_DEFAULT_RULE &&
		ioc_rule->rulenum != 0,
		("invalid rule num %d", ioc_rule->rulenum));

	/*
	 * Now find the right place for the new rule in the sorted list.
	 */
	for (prev = NULL, f = ctx->ipfw_layer3_chain; f;
	     prev = f, f = f->next) {
		if (f->rulenum > ioc_rule->rulenum) {
			/* Found the location */
			break;
		}
	}
	KASSERT(f != NULL, ("no default rule?!"));

	/*
	 * Duplicate the rule onto each CPU.
	 * The rule duplicated on CPU0 will be returned.
	 */
	bzero(&fwmsg, sizeof(fwmsg));
	nmsg = &fwmsg.base;
	netmsg_init(nmsg, NULL, &curthread->td_msgport, MSGF_PRIORITY,
	    ipfw_add_rule_dispatch);
	fwmsg.ioc_rule = ioc_rule;
	fwmsg.prev_rule = prev;
	fwmsg.next_rule = prev == NULL ? NULL : f;
	fwmsg.rule_flags = rule_flags;

	netisr_domsg_global(nmsg);
	KKASSERT(fwmsg.prev_rule == NULL && fwmsg.next_rule == NULL);

	rule = nmsg->lmsg.u.ms_resultp;
	KKASSERT(rule != NULL && rule->cpuid == mycpuid);

	DPRINTF("++ installed rule %d, static count now %d\n",
		rule->rulenum, static_count);
}

/*
 * Free storage associated with a static rule (including derived
 * states/tracks).
 * The caller is in charge of clearing rule pointers to avoid
 * dangling pointers.
 * @return a pointer to the next entry.
 * Arguments are not checked, so they better be correct.
 */
static struct ip_fw *
ipfw_delete_rule(struct ipfw_context *ctx,
		 struct ip_fw *prev, struct ip_fw *rule)
{
	struct ip_fw *n;

	n = rule->next;
	if (prev == NULL)
		ctx->ipfw_layer3_chain = n;
	else
		prev->next = n;

	/* Mark the rule as invalid */
	rule->rule_flags |= IPFW_RULE_F_INVALID;
	rule->next_rule = NULL;
	rule->sibling = NULL;
#ifdef foo
	/* Don't reset cpuid here; keep various assertion working */
	rule->cpuid = -1;
#endif

	/* Statistics only need to be updated once */
	if (mycpuid == 0)
		ipfw_dec_static_count(rule);

	/* Try to free this rule */
	ipfw_free_rule(rule);

	/* Return the next rule */
	return n;
}

static void
ipfw_flush_dispatch(netmsg_t nmsg)
{
	int kill_default = nmsg->lmsg.u.ms_result;
	struct ipfw_context *ctx = ipfw_ctx[mycpuid];
	struct ip_fw *rule;

	ASSERT_NETISR_NCPUS(mycpuid);

	/*
	 * Flush states.
	 */
	ipfw_state_flush(ctx, NULL);
	KASSERT(ctx->ipfw_state_cnt == 0,
	    ("%d pcpu states remain", ctx->ipfw_state_cnt));
	ctx->ipfw_state_loosecnt = 0;
	ctx->ipfw_state_lastexp = 0;

	/*
	 * Flush tracks.
	 */
	ipfw_track_flush(ctx, NULL);
	ctx->ipfw_track_lastexp = 0;
	if (ctx->ipfw_trkcnt_spare != NULL) {
		kfree(ctx->ipfw_trkcnt_spare, M_IPFW);
		ctx->ipfw_trkcnt_spare = NULL;
	}

	ipfw_flush_rule_ptrs(ctx); /* more efficient to do outside the loop */

	while ((rule = ctx->ipfw_layer3_chain) != NULL &&
	       (kill_default || rule->rulenum != IPFW_DEFAULT_RULE))
		ipfw_delete_rule(ctx, NULL, rule);

	netisr_forwardmsg(&nmsg->base, mycpuid + 1);
}

/*
 * Deletes all rules from a chain (including the default rule
 * if the second argument is set).
 */
static void
ipfw_flush(int kill_default)
{
	struct netmsg_base nmsg;
#ifdef INVARIANTS
	struct ipfw_context *ctx = ipfw_ctx[mycpuid];
	int state_cnt;
#endif

	ASSERT_NETISR0;

	/*
	 * If 'kill_default' then caller has done the necessary
	 * msgport syncing; unnecessary to do it again.
	 */
	if (!kill_default) {
		/*
		 * Let ipfw_chk() know the rules are going to
		 * be flushed, so it could jump directly to
		 * the default rule.
		 */
		ipfw_flushing = 1;
		/* XXX use priority sync */
		netmsg_service_sync();
	}

	/*
	 * Press the 'flush' button
	 */
	bzero(&nmsg, sizeof(nmsg));
	netmsg_init(&nmsg, NULL, &curthread->td_msgport, MSGF_PRIORITY,
	    ipfw_flush_dispatch);
	nmsg.lmsg.u.ms_result = kill_default;
	netisr_domsg_global(&nmsg);
	ipfw_gd.ipfw_state_loosecnt = 0;
	ipfw_gd.ipfw_state_globexp = 0;
	ipfw_gd.ipfw_track_globexp = 0;

#ifdef INVARIANTS
	state_cnt = ipfw_state_cntcoll();
	KASSERT(state_cnt == 0, ("%d states remain", state_cnt));

	KASSERT(ipfw_gd.ipfw_trkcnt_cnt == 0,
	    ("%d trkcnts remain", ipfw_gd.ipfw_trkcnt_cnt));

	if (kill_default) {
		KASSERT(static_count == 0,
			("%u static rules remain", static_count));
		KASSERT(static_ioc_len == 0,
			("%u bytes of static rules remain", static_ioc_len));
	} else {
		KASSERT(static_count == 1,
			("%u static rules remain", static_count));
		KASSERT(static_ioc_len == IOC_RULESIZE(ctx->ipfw_default_rule),
			("%u bytes of static rules remain, should be %lu",
			 static_ioc_len,
			 (u_long)IOC_RULESIZE(ctx->ipfw_default_rule)));
	}
#endif

	/* Flush is done */
	ipfw_flushing = 0;
}

static void
ipfw_alt_delete_rule_dispatch(netmsg_t nmsg)
{
	struct netmsg_del *dmsg = (struct netmsg_del *)nmsg;
	struct ipfw_context *ctx = ipfw_ctx[mycpuid];
	struct ip_fw *rule, *prev;

	ASSERT_NETISR_NCPUS(mycpuid);

	rule = dmsg->start_rule;
	KKASSERT(rule->cpuid == mycpuid);
	dmsg->start_rule = rule->sibling;

	prev = dmsg->prev_rule;
	if (prev != NULL) {
		KKASSERT(prev->cpuid == mycpuid);

		/*
		 * Move to the position on the next CPU
		 * before the msg is forwarded.
		 */
		dmsg->prev_rule = prev->sibling;
	}

	/*
	 * flush pointers outside the loop, then delete all matching
	 * rules.  'prev' remains the same throughout the cycle.
	 */
	ipfw_flush_rule_ptrs(ctx);
	while (rule && rule->rulenum == dmsg->rulenum) {
		if (rule->rule_flags & IPFW_RULE_F_GENSTATE) {
			/* Flush states generated by this rule. */
			ipfw_state_flush(ctx, rule);
		}
		if (rule->rule_flags & IPFW_RULE_F_GENTRACK) {
			/* Flush tracks generated by this rule. */
			ipfw_track_flush(ctx, rule);
		}
		rule = ipfw_delete_rule(ctx, prev, rule);
	}

	netisr_forwardmsg(&nmsg->base, mycpuid + 1);
}

static int
ipfw_alt_delete_rule(uint16_t rulenum)
{
	struct ip_fw *prev, *rule;
	struct ipfw_context *ctx = ipfw_ctx[mycpuid];
	struct netmsg_del dmsg;

	/*
	 * Locate first rule to delete
	 */
	for (prev = NULL, rule = ctx->ipfw_layer3_chain;
	     rule && rule->rulenum < rulenum;
	     prev = rule, rule = rule->next)
		; /* EMPTY */
	if (rule->rulenum != rulenum)
		return EINVAL;

	/*
	 * Get rid of the rule duplications on all CPUs
	 */
	bzero(&dmsg, sizeof(dmsg));
	netmsg_init(&dmsg.base, NULL, &curthread->td_msgport, MSGF_PRIORITY,
	    ipfw_alt_delete_rule_dispatch);
	dmsg.prev_rule = prev;
	dmsg.start_rule = rule;
	dmsg.rulenum = rulenum;

	netisr_domsg_global(&dmsg.base);
	KKASSERT(dmsg.prev_rule == NULL && dmsg.start_rule == NULL);
	return 0;
}

static void
ipfw_alt_delete_ruleset_dispatch(netmsg_t nmsg)
{
	struct netmsg_del *dmsg = (struct netmsg_del *)nmsg;
	struct ipfw_context *ctx = ipfw_ctx[mycpuid];
	struct ip_fw *prev, *rule;
#ifdef INVARIANTS
	int del = 0;
#endif

	ASSERT_NETISR_NCPUS(mycpuid);

	ipfw_flush_rule_ptrs(ctx);

	prev = NULL;
	rule = ctx->ipfw_layer3_chain;
	while (rule != NULL) {
		if (rule->set == dmsg->from_set) {
			if (rule->rule_flags & IPFW_RULE_F_GENSTATE) {
				/* Flush states generated by this rule. */
				ipfw_state_flush(ctx, rule);
			}
			if (rule->rule_flags & IPFW_RULE_F_GENTRACK) {
				/* Flush tracks generated by this rule. */
				ipfw_track_flush(ctx, rule);
			}
			rule = ipfw_delete_rule(ctx, prev, rule);
#ifdef INVARIANTS
			del = 1;
#endif
		} else {
			prev = rule;
			rule = rule->next;
		}
	}
	KASSERT(del, ("no match set?!"));

	netisr_forwardmsg(&nmsg->base, mycpuid + 1);
}

static int
ipfw_alt_delete_ruleset(uint8_t set)
{
	struct netmsg_del dmsg;
	int del;
	struct ip_fw *rule;
	struct ipfw_context *ctx = ipfw_ctx[mycpuid];

	/*
	 * Check whether the 'set' exists.  If it exists,
	 * then check whether any rules within the set will
	 * try to create states.
	 */
	del = 0;
	for (rule = ctx->ipfw_layer3_chain; rule; rule = rule->next) {
		if (rule->set == set)
			del = 1;
	}
	if (!del)
		return 0; /* XXX EINVAL? */

	/*
	 * Delete this set
	 */
	bzero(&dmsg, sizeof(dmsg));
	netmsg_init(&dmsg.base, NULL, &curthread->td_msgport, MSGF_PRIORITY,
	    ipfw_alt_delete_ruleset_dispatch);
	dmsg.from_set = set;
	netisr_domsg_global(&dmsg.base);

	return 0;
}

static void
ipfw_alt_move_rule_dispatch(netmsg_t nmsg)
{
	struct netmsg_del *dmsg = (struct netmsg_del *)nmsg;
	struct ip_fw *rule;

	ASSERT_NETISR_NCPUS(mycpuid);

	rule = dmsg->start_rule;
	KKASSERT(rule->cpuid == mycpuid);

	/*
	 * Move to the position on the next CPU
	 * before the msg is forwarded.
	 */
	dmsg->start_rule = rule->sibling;

	while (rule && rule->rulenum <= dmsg->rulenum) {
		if (rule->rulenum == dmsg->rulenum)
			rule->set = dmsg->to_set;
		rule = rule->next;
	}
	netisr_forwardmsg(&nmsg->base, mycpuid + 1);
}

static int
ipfw_alt_move_rule(uint16_t rulenum, uint8_t set)
{
	struct netmsg_del dmsg;
	struct netmsg_base *nmsg;
	struct ip_fw *rule;
	struct ipfw_context *ctx = ipfw_ctx[mycpuid];

	/*
	 * Locate first rule to move
	 */
	for (rule = ctx->ipfw_layer3_chain; rule && rule->rulenum <= rulenum;
	     rule = rule->next) {
		if (rule->rulenum == rulenum && rule->set != set)
			break;
	}
	if (rule == NULL || rule->rulenum > rulenum)
		return 0; /* XXX error? */

	bzero(&dmsg, sizeof(dmsg));
	nmsg = &dmsg.base;
	netmsg_init(nmsg, NULL, &curthread->td_msgport, MSGF_PRIORITY,
	    ipfw_alt_move_rule_dispatch);
	dmsg.start_rule = rule;
	dmsg.rulenum = rulenum;
	dmsg.to_set = set;

	netisr_domsg_global(nmsg);
	KKASSERT(dmsg.start_rule == NULL);
	return 0;
}

static void
ipfw_alt_move_ruleset_dispatch(netmsg_t nmsg)
{
	struct netmsg_del *dmsg = (struct netmsg_del *)nmsg;
	struct ipfw_context *ctx = ipfw_ctx[mycpuid];
	struct ip_fw *rule;

	ASSERT_NETISR_NCPUS(mycpuid);

	for (rule = ctx->ipfw_layer3_chain; rule; rule = rule->next) {
		if (rule->set == dmsg->from_set)
			rule->set = dmsg->to_set;
	}
	netisr_forwardmsg(&nmsg->base, mycpuid + 1);
}

static int
ipfw_alt_move_ruleset(uint8_t from_set, uint8_t to_set)
{
	struct netmsg_del dmsg;
	struct netmsg_base *nmsg;

	bzero(&dmsg, sizeof(dmsg));
	nmsg = &dmsg.base;
	netmsg_init(nmsg, NULL, &curthread->td_msgport, MSGF_PRIORITY,
	    ipfw_alt_move_ruleset_dispatch);
	dmsg.from_set = from_set;
	dmsg.to_set = to_set;

	netisr_domsg_global(nmsg);
	return 0;
}

static void
ipfw_alt_swap_ruleset_dispatch(netmsg_t nmsg)
{
	struct netmsg_del *dmsg = (struct netmsg_del *)nmsg;
	struct ipfw_context *ctx = ipfw_ctx[mycpuid];
	struct ip_fw *rule;

	ASSERT_NETISR_NCPUS(mycpuid);

	for (rule = ctx->ipfw_layer3_chain; rule; rule = rule->next) {
		if (rule->set == dmsg->from_set)
			rule->set = dmsg->to_set;
		else if (rule->set == dmsg->to_set)
			rule->set = dmsg->from_set;
	}
	netisr_forwardmsg(&nmsg->base, mycpuid + 1);
}

static int
ipfw_alt_swap_ruleset(uint8_t set1, uint8_t set2)
{
	struct netmsg_del dmsg;
	struct netmsg_base *nmsg;

	bzero(&dmsg, sizeof(dmsg));
	nmsg = &dmsg.base;
	netmsg_init(nmsg, NULL, &curthread->td_msgport, MSGF_PRIORITY,
	    ipfw_alt_swap_ruleset_dispatch);
	dmsg.from_set = set1;
	dmsg.to_set = set2;

	netisr_domsg_global(nmsg);
	return 0;
}

/*
 * Remove all rules with given number, and also do set manipulation.
 *
 * The argument is an uint32_t. The low 16 bit are the rule or set number,
 * the next 8 bits are the new set, the top 8 bits are the command:
 *
 *	0	delete rules with given number
 *	1	delete rules with given set number
 *	2	move rules with given number to new set
 *	3	move rules with given set number to new set
 *	4	swap sets with given numbers
 */
static int
ipfw_ctl_alter(uint32_t arg)
{
	uint16_t rulenum;
	uint8_t cmd, new_set;
	int error = 0;

	rulenum = arg & 0xffff;
	cmd = (arg >> 24) & 0xff;
	new_set = (arg >> 16) & 0xff;

	if (cmd > 4)
		return EINVAL;
	if (new_set >= IPFW_DEFAULT_SET)
		return EINVAL;
	if (cmd == 0 || cmd == 2) {
		if (rulenum == IPFW_DEFAULT_RULE)
			return EINVAL;
	} else {
		if (rulenum >= IPFW_DEFAULT_SET)
			return EINVAL;
	}

	switch (cmd) {
	case 0:	/* delete rules with given number */
		error = ipfw_alt_delete_rule(rulenum);
		break;

	case 1:	/* delete all rules with given set number */
		error = ipfw_alt_delete_ruleset(rulenum);
		break;

	case 2:	/* move rules with given number to new set */
		error = ipfw_alt_move_rule(rulenum, new_set);
		break;

	case 3: /* move rules with given set number to new set */
		error = ipfw_alt_move_ruleset(rulenum, new_set);
		break;

	case 4: /* swap two sets */
		error = ipfw_alt_swap_ruleset(rulenum, new_set);
		break;
	}
	return error;
}

/*
 * Clear counters for a specific rule.
 */
static void
clear_counters(struct ip_fw *rule, int log_only)
{
	ipfw_insn_log *l = (ipfw_insn_log *)ACTION_PTR(rule);

	if (log_only == 0) {
		rule->bcnt = rule->pcnt = 0;
		rule->timestamp = 0;
	}
	if (l->o.opcode == O_LOG)
		l->log_left = l->max_log;
}

static void
ipfw_zero_entry_dispatch(netmsg_t nmsg)
{
	struct netmsg_zent *zmsg = (struct netmsg_zent *)nmsg;
	struct ipfw_context *ctx = ipfw_ctx[mycpuid];
	struct ip_fw *rule;

	ASSERT_NETISR_NCPUS(mycpuid);

	if (zmsg->rulenum == 0) {
		KKASSERT(zmsg->start_rule == NULL);

		ctx->ipfw_norule_counter = 0;
		for (rule = ctx->ipfw_layer3_chain; rule; rule = rule->next)
			clear_counters(rule, zmsg->log_only);
	} else {
		struct ip_fw *start = zmsg->start_rule;

		KKASSERT(start->cpuid == mycpuid);
		KKASSERT(start->rulenum == zmsg->rulenum);

		/*
		 * We can have multiple rules with the same number, so we
		 * need to clear them all.
		 */
		for (rule = start; rule && rule->rulenum == zmsg->rulenum;
		     rule = rule->next)
			clear_counters(rule, zmsg->log_only);

		/*
		 * Move to the position on the next CPU
		 * before the msg is forwarded.
		 */
		zmsg->start_rule = start->sibling;
	}
	netisr_forwardmsg(&nmsg->base, mycpuid + 1);
}

/*
 * Reset some or all counters on firewall rules.
 * @arg frwl is null to clear all entries, or contains a specific
 * rule number.
 * @arg log_only is 1 if we only want to reset logs, zero otherwise.
 */
static int
ipfw_ctl_zero_entry(int rulenum, int log_only)
{
	struct netmsg_zent zmsg;
	struct netmsg_base *nmsg;
	const char *msg;
	struct ipfw_context *ctx = ipfw_ctx[mycpuid];

	bzero(&zmsg, sizeof(zmsg));
	nmsg = &zmsg.base;
	netmsg_init(nmsg, NULL, &curthread->td_msgport, MSGF_PRIORITY,
	    ipfw_zero_entry_dispatch);
	zmsg.log_only = log_only;

	if (rulenum == 0) {
		msg = log_only ? "ipfw: All logging counts reset.\n"
			       : "ipfw: Accounting cleared.\n";
	} else {
		struct ip_fw *rule;

		/*
		 * Locate the first rule with 'rulenum'
		 */
		for (rule = ctx->ipfw_layer3_chain; rule; rule = rule->next) {
			if (rule->rulenum == rulenum)
				break;
		}
		if (rule == NULL) /* we did not find any matching rules */
			return (EINVAL);
		zmsg.start_rule = rule;
		zmsg.rulenum = rulenum;

		msg = log_only ? "ipfw: Entry %d logging count reset.\n"
			       : "ipfw: Entry %d cleared.\n";
	}
	netisr_domsg_global(nmsg);
	KKASSERT(zmsg.start_rule == NULL);

	if (fw_verbose)
		log(LOG_SECURITY | LOG_NOTICE, msg, rulenum);
	return (0);
}

/*
 * Check validity of the structure before insert.
 * Fortunately rules are simple, so this mostly need to check rule sizes.
 */
static int
ipfw_check_ioc_rule(struct ipfw_ioc_rule *rule, int size, uint32_t *rule_flags)
{
	int l, cmdlen = 0;
	int have_action = 0;
	ipfw_insn *cmd;

	*rule_flags = 0;

	/* Check for valid size */
	if (size < sizeof(*rule)) {
		kprintf("ipfw: rule too short\n");
		return EINVAL;
	}
	l = IOC_RULESIZE(rule);
	if (l != size) {
		kprintf("ipfw: size mismatch (have %d want %d)\n", size, l);
		return EINVAL;
	}

	/* Check rule number */
	if (rule->rulenum == IPFW_DEFAULT_RULE) {
		kprintf("ipfw: invalid rule number\n");
		return EINVAL;
	}

	/*
	 * Now go for the individual checks. Very simple ones, basically only
	 * instruction sizes.
	 */
	for (l = rule->cmd_len, cmd = rule->cmd; l > 0;
	     l -= cmdlen, cmd += cmdlen) {
		cmdlen = F_LEN(cmd);
		if (cmdlen > l) {
			kprintf("ipfw: opcode %d size truncated\n",
				cmd->opcode);
			return EINVAL;
		}

		DPRINTF("ipfw: opcode %d\n", cmd->opcode);

		if (cmd->opcode == O_KEEP_STATE || cmd->opcode == O_LIMIT) {
			/* This rule will generate states. */
			*rule_flags |= IPFW_RULE_F_GENSTATE;
			if (cmd->opcode == O_LIMIT)
				*rule_flags |= IPFW_RULE_F_GENTRACK;
		}

		switch (cmd->opcode) {
		case O_NOP:
		case O_PROBE_STATE:
		case O_KEEP_STATE:
		case O_PROTO:
		case O_IP_SRC_ME:
		case O_IP_DST_ME:
		case O_LAYER2:
		case O_IN:
		case O_FRAG:
		case O_IPOPT:
		case O_IPLEN:
		case O_IPID:
		case O_IPTOS:
		case O_IPPRECEDENCE:
		case O_IPTTL:
		case O_IPVER:
		case O_TCPWIN:
		case O_TCPFLAGS:
		case O_TCPOPTS:
		case O_ESTAB:
			if (cmdlen != F_INSN_SIZE(ipfw_insn))
				goto bad_size;
			break;

		case O_UID:
		case O_GID:
		case O_IP_SRC:
		case O_IP_DST:
		case O_TCPSEQ:
		case O_TCPACK:
		case O_PROB:
		case O_ICMPTYPE:
			if (cmdlen != F_INSN_SIZE(ipfw_insn_u32))
				goto bad_size;
			break;

		case O_LIMIT:
			if (cmdlen != F_INSN_SIZE(ipfw_insn_limit))
				goto bad_size;
			break;

		case O_LOG:
			if (cmdlen != F_INSN_SIZE(ipfw_insn_log))
				goto bad_size;

			((ipfw_insn_log *)cmd)->log_left =
			    ((ipfw_insn_log *)cmd)->max_log;

			break;

		case O_IP_SRC_MASK:
		case O_IP_DST_MASK:
			if (cmdlen != F_INSN_SIZE(ipfw_insn_ip))
				goto bad_size;
			if (((ipfw_insn_ip *)cmd)->mask.s_addr == 0) {
				kprintf("ipfw: opcode %d, useless rule\n",
					cmd->opcode);
				return EINVAL;
			}
			break;

		case O_IP_SRC_SET:
		case O_IP_DST_SET:
			if (cmd->arg1 == 0 || cmd->arg1 > 256) {
				kprintf("ipfw: invalid set size %d\n",
					cmd->arg1);
				return EINVAL;
			}
			if (cmdlen != F_INSN_SIZE(ipfw_insn_u32) +
			    (cmd->arg1+31)/32 )
				goto bad_size;
			break;

		case O_MACADDR2:
			if (cmdlen != F_INSN_SIZE(ipfw_insn_mac))
				goto bad_size;
			break;

		case O_MAC_TYPE:
		case O_IP_SRCPORT:
		case O_IP_DSTPORT: /* XXX artificial limit, 30 port pairs */
			if (cmdlen < 2 || cmdlen > 31)
				goto bad_size;
			break;

		case O_RECV:
		case O_XMIT:
		case O_VIA:
			if (cmdlen != F_INSN_SIZE(ipfw_insn_if))
				goto bad_size;
			break;

		case O_PIPE:
		case O_QUEUE:
			if (cmdlen != F_INSN_SIZE(ipfw_insn_pipe))
				goto bad_size;
			goto check_action;

		case O_FORWARD_IP:
			if (cmdlen != F_INSN_SIZE(ipfw_insn_sa)) {
				goto bad_size;
			} else {
				in_addr_t fwd_addr;

				fwd_addr = ((ipfw_insn_sa *)cmd)->
					   sa.sin_addr.s_addr;
				if (IN_MULTICAST(ntohl(fwd_addr))) {
					kprintf("ipfw: try forwarding to "
						"multicast address\n");
					return EINVAL;
				}
			}
			goto check_action;

		case O_FORWARD_MAC: /* XXX not implemented yet */
		case O_CHECK_STATE:
		case O_COUNT:
		case O_ACCEPT:
		case O_DENY:
		case O_REJECT:
		case O_SKIPTO:
		case O_DIVERT:
		case O_TEE:
			if (cmdlen != F_INSN_SIZE(ipfw_insn))
				goto bad_size;
check_action:
			if (have_action) {
				kprintf("ipfw: opcode %d, multiple actions"
					" not allowed\n",
					cmd->opcode);
				return EINVAL;
			}
			have_action = 1;
			if (l != cmdlen) {
				kprintf("ipfw: opcode %d, action must be"
					" last opcode\n",
					cmd->opcode);
				return EINVAL;
			}
			break;
		default:
			kprintf("ipfw: opcode %d, unknown opcode\n",
				cmd->opcode);
			return EINVAL;
		}
	}
	if (have_action == 0) {
		kprintf("ipfw: missing action\n");
		return EINVAL;
	}
	return 0;

bad_size:
	kprintf("ipfw: opcode %d size %d wrong\n",
		cmd->opcode, cmdlen);
	return EINVAL;
}

static int
ipfw_ctl_add_rule(struct sockopt *sopt)
{
	struct ipfw_ioc_rule *ioc_rule;
	size_t size;
	uint32_t rule_flags;
	int error;
	
	size = sopt->sopt_valsize;
	if (size > (sizeof(uint32_t) * IPFW_RULE_SIZE_MAX) ||
	    size < sizeof(*ioc_rule)) {
		return EINVAL;
	}
	if (size != (sizeof(uint32_t) * IPFW_RULE_SIZE_MAX)) {
		sopt->sopt_val = krealloc(sopt->sopt_val, sizeof(uint32_t) *
					  IPFW_RULE_SIZE_MAX, M_TEMP, M_WAITOK);
	}
	ioc_rule = sopt->sopt_val;

	error = ipfw_check_ioc_rule(ioc_rule, size, &rule_flags);
	if (error)
		return error;

	ipfw_add_rule(ioc_rule, rule_flags);

	if (sopt->sopt_dir == SOPT_GET)
		sopt->sopt_valsize = IOC_RULESIZE(ioc_rule);
	return 0;
}

static void *
ipfw_copy_rule(const struct ipfw_context *ctx, const struct ip_fw *rule,
    struct ipfw_ioc_rule *ioc_rule)
{
	const struct ip_fw *sibling;
#ifdef INVARIANTS
	int i;
#endif

	KASSERT(rule->cpuid == 0, ("rule does not belong to cpu0"));

	ioc_rule->act_ofs = rule->act_ofs;
	ioc_rule->cmd_len = rule->cmd_len;
	ioc_rule->rulenum = rule->rulenum;
	ioc_rule->set = rule->set;
	ioc_rule->usr_flags = rule->usr_flags;

	ioc_rule->set_disable = ctx->ipfw_set_disable;
	ioc_rule->static_count = static_count;
	ioc_rule->static_len = static_ioc_len;

	/*
	 * Visit (read-only) all of the rule's duplications to get
	 * the necessary statistics
	 */
#ifdef INVARIANTS
	i = 0;
#endif
	ioc_rule->pcnt = 0;
	ioc_rule->bcnt = 0;
	ioc_rule->timestamp = 0;
	for (sibling = rule; sibling != NULL; sibling = sibling->sibling) {
		ioc_rule->pcnt += sibling->pcnt;
		ioc_rule->bcnt += sibling->bcnt;
		if (sibling->timestamp > ioc_rule->timestamp)
			ioc_rule->timestamp = sibling->timestamp;
#ifdef INVARIANTS
		++i;
#endif
	}
	KASSERT(i == netisr_ncpus,
	    ("static rule is not duplicated on netisr_ncpus %d", netisr_ncpus));

	bcopy(rule->cmd, ioc_rule->cmd, ioc_rule->cmd_len * 4 /* XXX */);

	return ((uint8_t *)ioc_rule + IOC_RULESIZE(ioc_rule));
}

static boolean_t
ipfw_track_copy(const struct ipfw_trkcnt *trk, struct ipfw_ioc_state *ioc_state)
{
	struct ipfw_ioc_flowid *ioc_id;

	if (trk->tc_expire == 0) {
		/* Not a scanned one. */
		return (FALSE);
	}

	ioc_state->expire = TIME_LEQ(trk->tc_expire, time_uptime) ?
	    0 : trk->tc_expire - time_uptime;
	ioc_state->pcnt = 0;
	ioc_state->bcnt = 0;

	ioc_state->dyn_type = O_LIMIT_PARENT;
	ioc_state->count = trk->tc_count;

	ioc_state->rulenum = trk->tc_rulenum;

	ioc_id = &ioc_state->id;
	ioc_id->type = ETHERTYPE_IP;
	ioc_id->u.ip.proto = trk->tc_proto;
	ioc_id->u.ip.src_ip = trk->tc_saddr;
	ioc_id->u.ip.dst_ip = trk->tc_daddr;
	ioc_id->u.ip.src_port = trk->tc_sport;
	ioc_id->u.ip.dst_port = trk->tc_dport;

	return (TRUE);
}

static boolean_t
ipfw_state_copy(const struct ipfw_state *s, struct ipfw_ioc_state *ioc_state)
{
	struct ipfw_ioc_flowid *ioc_id;

	if (s->st_type == O_ANCHOR)
		return (FALSE);

	ioc_state->expire = TIME_LEQ(s->st_expire, time_uptime) ?
	    0 : s->st_expire - time_uptime;
	ioc_state->pcnt = s->st_pcnt;
	ioc_state->bcnt = s->st_bcnt;

	ioc_state->dyn_type = s->st_type;
	ioc_state->count = 0;

	ioc_state->rulenum = s->st_rule->rulenum;

	ioc_id = &ioc_state->id;
	ioc_id->type = ETHERTYPE_IP;
	ioc_id->u.ip.proto = s->st_proto;
	ipfw_key_4tuple(&s->st_key,
	    &ioc_id->u.ip.src_ip, &ioc_id->u.ip.src_port,
	    &ioc_id->u.ip.dst_ip, &ioc_id->u.ip.dst_port);

	return (TRUE);
}

static void
ipfw_state_copy_dispatch(netmsg_t nmsg)
{
	struct netmsg_cpstate *nm = (struct netmsg_cpstate *)nmsg;
	struct ipfw_context *ctx = ipfw_ctx[mycpuid];
	const struct ipfw_state *s;
	const struct ipfw_track *t;

	ASSERT_NETISR_NCPUS(mycpuid);
	KASSERT(nm->state_cnt < nm->state_cntmax,
	    ("invalid state count %d, max %d",
	     nm->state_cnt, nm->state_cntmax));

	TAILQ_FOREACH(s, &ctx->ipfw_state_list, st_link) {
		if (ipfw_state_copy(s, nm->ioc_state)) {
			nm->ioc_state++;
			nm->state_cnt++;
			if (nm->state_cnt == nm->state_cntmax)
				goto done;
		}
	}

	/*
	 * Prepare tracks in the global track tree for userland.
	 */
	TAILQ_FOREACH(t, &ctx->ipfw_track_list, t_link) {
		struct ipfw_trkcnt *trk;

		if (t->t_count == NULL) /* anchor */
			continue;
		trk = t->t_trkcnt;

		/*
		 * Only one netisr can run this function at
		 * any time, and only this function accesses
		 * trkcnt's tc_expire, so this is safe w/o
		 * ipfw_gd.ipfw_trkcnt_token.
		 */
		if (trk->tc_expire > t->t_expire)
			continue;
		trk->tc_expire = t->t_expire;
	}

	/*
	 * Copy tracks in the global track tree to userland in
	 * the last netisr.
	 */
	if (mycpuid == netisr_ncpus - 1) {
		struct ipfw_trkcnt *trk;

		KASSERT(nm->state_cnt < nm->state_cntmax,
		    ("invalid state count %d, max %d",
		     nm->state_cnt, nm->state_cntmax));

		IPFW_TRKCNT_TOKGET;
		RB_FOREACH(trk, ipfw_trkcnt_tree, &ipfw_gd.ipfw_trkcnt_tree) {
			if (ipfw_track_copy(trk, nm->ioc_state)) {
				nm->ioc_state++;
				nm->state_cnt++;
				if (nm->state_cnt == nm->state_cntmax) {
					IPFW_TRKCNT_TOKREL;
					goto done;
				}
			}
		}
		IPFW_TRKCNT_TOKREL;
	}
done:
	if (nm->state_cnt == nm->state_cntmax) {
		/* No more space; done. */
		netisr_replymsg(&nm->base, 0);
	} else {
		netisr_forwardmsg(&nm->base, mycpuid + 1);
	}
}

static int
ipfw_ctl_get_rules(struct sockopt *sopt)
{
	struct ipfw_context *ctx = ipfw_ctx[mycpuid];
	struct ip_fw *rule;
	void *bp;
	size_t size;
	int state_cnt;

	/*
	 * pass up a copy of the current rules. Static rules
	 * come first (the last of which has number IPFW_DEFAULT_RULE),
	 * followed by a possibly empty list of states.
	 */

	size = static_ioc_len;	/* size of static rules */

	/*
	 * Size of the states.
	 * XXX take tracks as state for userland compat.
	 */
	state_cnt = ipfw_state_cntcoll() + ipfw_gd.ipfw_trkcnt_cnt;
	state_cnt = (state_cnt * 5) / 4; /* leave 25% headroom */
	size += state_cnt * sizeof(struct ipfw_ioc_state);

	if (sopt->sopt_valsize < size) {
		/* short length, no need to return incomplete rules */
		/* XXX: if superuser, no need to zero buffer */
		bzero(sopt->sopt_val, sopt->sopt_valsize); 
		return 0;
	}
	bp = sopt->sopt_val;

	for (rule = ctx->ipfw_layer3_chain; rule; rule = rule->next)
		bp = ipfw_copy_rule(ctx, rule, bp);

	if (state_cnt) {
		struct netmsg_cpstate nm;
#ifdef INVARIANTS
		size_t old_size = size;
#endif

		netmsg_init(&nm.base, NULL, &curthread->td_msgport,
		    MSGF_PRIORITY, ipfw_state_copy_dispatch);
		nm.ioc_state = bp;
		nm.state_cntmax = state_cnt;
		nm.state_cnt = 0;
		netisr_domsg_global(&nm.base);

		/*
		 * The # of states may be shrinked after the snapshot
		 * of the state count was taken.  To give user a correct
		 * state count, nm->state_cnt is used to recalculate
		 * the actual size.
		 */
		size = static_ioc_len +
		    (nm.state_cnt * sizeof(struct ipfw_ioc_state));
		KKASSERT(size <= old_size);
	}

	sopt->sopt_valsize = size;
	return 0;
}

static void
ipfw_set_disable_dispatch(netmsg_t nmsg)
{
	struct ipfw_context *ctx = ipfw_ctx[mycpuid];

	ASSERT_NETISR_NCPUS(mycpuid);

	ctx->ipfw_set_disable = nmsg->lmsg.u.ms_result32;
	netisr_forwardmsg(&nmsg->base, mycpuid + 1);
}

static void
ipfw_ctl_set_disable(uint32_t disable, uint32_t enable)
{
	struct netmsg_base nmsg;
	uint32_t set_disable;

	/* IPFW_DEFAULT_SET is always enabled */
	enable |= (1 << IPFW_DEFAULT_SET);
	set_disable = (ipfw_ctx[mycpuid]->ipfw_set_disable | disable) & ~enable;

	bzero(&nmsg, sizeof(nmsg));
	netmsg_init(&nmsg, NULL, &curthread->td_msgport, MSGF_PRIORITY,
	    ipfw_set_disable_dispatch);
	nmsg.lmsg.u.ms_result32 = set_disable;

	netisr_domsg_global(&nmsg);
}

/*
 * {set|get}sockopt parser.
 */
static int
ipfw_ctl(struct sockopt *sopt)
{
	int error, rulenum;
	uint32_t *masks;
	size_t size;

	error = 0;

	switch (sopt->sopt_name) {
	case IP_FW_GET:
		error = ipfw_ctl_get_rules(sopt);
		break;

	case IP_FW_FLUSH:
		ipfw_flush(0 /* keep default rule */);
		break;

	case IP_FW_ADD:
		error = ipfw_ctl_add_rule(sopt);
		break;

	case IP_FW_DEL:
		/*
		 * IP_FW_DEL is used for deleting single rules or sets,
		 * and (ab)used to atomically manipulate sets.
		 * Argument size is used to distinguish between the two:
		 *    sizeof(uint32_t)
		 *	delete single rule or set of rules,
		 *	or reassign rules (or sets) to a different set.
		 *    2 * sizeof(uint32_t)
		 *	atomic disable/enable sets.
		 *	first uint32_t contains sets to be disabled,
		 *	second uint32_t contains sets to be enabled.
		 */
		masks = sopt->sopt_val;
		size = sopt->sopt_valsize;
		if (size == sizeof(*masks)) {
			/*
			 * Delete or reassign static rule
			 */
			error = ipfw_ctl_alter(masks[0]);
		} else if (size == (2 * sizeof(*masks))) {
			/*
			 * Set enable/disable
			 */
			ipfw_ctl_set_disable(masks[0], masks[1]);
		} else {
			error = EINVAL;
		}
		break;

	case IP_FW_ZERO:
	case IP_FW_RESETLOG: /* argument is an int, the rule number */
		rulenum = 0;

		if (sopt->sopt_val != 0) {
		    error = soopt_to_kbuf(sopt, &rulenum,
			    sizeof(int), sizeof(int));
		    if (error)
			break;
		}
		error = ipfw_ctl_zero_entry(rulenum,
			sopt->sopt_name == IP_FW_RESETLOG);
		break;

	default:
		kprintf("ipfw_ctl invalid option %d\n", sopt->sopt_name);
		error = EINVAL;
	}
	return error;
}

static void
ipfw_keepalive_done(struct ipfw_context *ctx)
{

	KASSERT(ctx->ipfw_flags & IPFW_FLAG_KEEPALIVE,
	    ("keepalive is not in progress"));
	ctx->ipfw_flags &= ~IPFW_FLAG_KEEPALIVE;
	callout_reset(&ctx->ipfw_keepalive_ch, dyn_keepalive_period * hz,
	    ipfw_keepalive, NULL);
}

static void
ipfw_keepalive_more(struct ipfw_context *ctx)
{
	struct netmsg_base *nm = &ctx->ipfw_keepalive_more;

	KASSERT(ctx->ipfw_flags & IPFW_FLAG_KEEPALIVE,
	    ("keepalive is not in progress"));
	KASSERT(nm->lmsg.ms_flags & MSGF_DONE,
	    ("keepalive more did not finish"));
	netisr_sendmsg_oncpu(nm);
}

static void
ipfw_keepalive_loop(struct ipfw_context *ctx, struct ipfw_state *anchor)
{
	struct ipfw_state *s;
	int scanned = 0, expired = 0, kept = 0;

	KASSERT(ctx->ipfw_flags & IPFW_FLAG_KEEPALIVE,
	    ("keepalive is not in progress"));

	while ((s = TAILQ_NEXT(anchor, st_link)) != NULL) {
		uint32_t ack_rev, ack_fwd;
		struct ipfw_flow_id id;

		if (scanned++ >= ipfw_state_scan_max) {
			ipfw_keepalive_more(ctx);
			return;
		}

		TAILQ_REMOVE(&ctx->ipfw_state_list, anchor, st_link);
		TAILQ_INSERT_AFTER(&ctx->ipfw_state_list, s, anchor, st_link);

		if (s->st_type == O_ANCHOR)
			continue;

		if (TIME_LEQ(s->st_expire, time_uptime)) {
			/* State expired. */
			ipfw_state_del(ctx, s);
			if (++expired >= ipfw_state_expire_max) {
				ipfw_keepalive_more(ctx);
				return;
			}
			continue;
		}

		/*
		 * Keep alive processing
		 */

		if (s->st_proto != IPPROTO_TCP)
			continue;
		if ((s->st_state & IPFW_STATE_TCPSTATES) != BOTH_SYN)
			continue;
		if (TIME_LEQ(time_uptime + dyn_keepalive_interval,
		    s->st_expire))
			continue;	/* too early */

		ipfw_key_4tuple(&s->st_key, &id.src_ip, &id.src_port,
		    &id.dst_ip, &id.dst_port);
		ack_rev = s->st_ack_rev;
		ack_fwd = s->st_ack_fwd;

		send_pkt(&id, ack_rev - 1, ack_fwd, TH_SYN);
		send_pkt(&id, ack_fwd - 1, ack_rev, 0);

		if (++kept >= ipfw_keepalive_max) {
			ipfw_keepalive_more(ctx);
			return;
		}
	}
	TAILQ_REMOVE(&ctx->ipfw_state_list, anchor, st_link);
	ipfw_keepalive_done(ctx);
}

static void
ipfw_keepalive_more_dispatch(netmsg_t nm)
{
	struct ipfw_context *ctx = ipfw_ctx[mycpuid];
	struct ipfw_state *anchor;

	ASSERT_NETISR_NCPUS(mycpuid);
	KASSERT(ctx->ipfw_flags & IPFW_FLAG_KEEPALIVE,
	    ("keepalive is not in progress"));

	/* Reply ASAP */
	netisr_replymsg(&nm->base, 0);

	anchor = &ctx->ipfw_keepalive_anch;
	if (!dyn_keepalive || ctx->ipfw_state_cnt == 0) {
		TAILQ_REMOVE(&ctx->ipfw_state_list, anchor, st_link);
		ipfw_keepalive_done(ctx);
		return;
	}
	ipfw_keepalive_loop(ctx, anchor);
}

/*
 * This procedure is only used to handle keepalives. It is invoked
 * every dyn_keepalive_period
 */
static void
ipfw_keepalive_dispatch(netmsg_t nm)
{
	struct ipfw_context *ctx = ipfw_ctx[mycpuid];
	struct ipfw_state *anchor;

	ASSERT_NETISR_NCPUS(mycpuid);
	KASSERT((ctx->ipfw_flags & IPFW_FLAG_KEEPALIVE) == 0,
	    ("keepalive is in progress"));
	ctx->ipfw_flags |= IPFW_FLAG_KEEPALIVE;

	/* Reply ASAP */
	crit_enter();
	netisr_replymsg(&nm->base, 0);
	crit_exit();

	if (!dyn_keepalive || ctx->ipfw_state_cnt == 0) {
		ipfw_keepalive_done(ctx);
		return;
	}

	anchor = &ctx->ipfw_keepalive_anch;
	TAILQ_INSERT_HEAD(&ctx->ipfw_state_list, anchor, st_link);
	ipfw_keepalive_loop(ctx, anchor);
}

/*
 * This procedure is only used to handle keepalives. It is invoked
 * every dyn_keepalive_period
 */
static void
ipfw_keepalive(void *dummy __unused)
{
	struct netmsg_base *msg;

	KKASSERT(mycpuid < netisr_ncpus);
	msg = &ipfw_ctx[mycpuid]->ipfw_keepalive_nm;

	crit_enter();
	if (msg->lmsg.ms_flags & MSGF_DONE)
		netisr_sendmsg_oncpu(msg);
	crit_exit();
}

static int
ipfw_check_in(void *arg, struct mbuf **m0, struct ifnet *ifp, int dir)
{
	struct ip_fw_args args;
	struct mbuf *m = *m0;
	struct m_tag *mtag;
	int tee = 0, error = 0, ret;

	if (m->m_pkthdr.fw_flags & DUMMYNET_MBUF_TAGGED) {
		/* Extract info from dummynet tag */
		mtag = m_tag_find(m, PACKET_TAG_DUMMYNET, NULL);
		KKASSERT(mtag != NULL);
		args.rule = ((struct dn_pkt *)m_tag_data(mtag))->dn_priv;
		KKASSERT(args.rule != NULL);

		m_tag_delete(m, mtag);
		m->m_pkthdr.fw_flags &= ~DUMMYNET_MBUF_TAGGED;
	} else {
		args.rule = NULL;
	}

	args.eh = NULL;
	args.oif = NULL;
	args.m = m;
	ret = ipfw_chk(&args);
	m = args.m;

	if (m == NULL) {
		error = EACCES;
		goto back;
	}

	switch (ret) {
	case IP_FW_PASS:
		break;

	case IP_FW_DENY:
		m_freem(m);
		m = NULL;
		error = EACCES;
		break;

	case IP_FW_DUMMYNET:
		/* Send packet to the appropriate pipe */
		ipfw_dummynet_io(m, args.cookie, DN_TO_IP_IN, &args);
		break;

	case IP_FW_TEE:
		tee = 1;
		/* FALL THROUGH */

	case IP_FW_DIVERT:
		/*
		 * Must clear bridge tag when changing
		 */
		m->m_pkthdr.fw_flags &= ~BRIDGE_MBUF_TAGGED;
		if (ip_divert_p != NULL) {
			m = ip_divert_p(m, tee, 1);
		} else {
			m_freem(m);
			m = NULL;
			/* not sure this is the right error msg */
			error = EACCES;
		}
		break;

	default:
		panic("unknown ipfw return value: %d", ret);
	}
back:
	*m0 = m;
	return error;
}

static int
ipfw_check_out(void *arg, struct mbuf **m0, struct ifnet *ifp, int dir)
{
	struct ip_fw_args args;
	struct mbuf *m = *m0;
	struct m_tag *mtag;
	int tee = 0, error = 0, ret;

	if (m->m_pkthdr.fw_flags & DUMMYNET_MBUF_TAGGED) {
		/* Extract info from dummynet tag */
		mtag = m_tag_find(m, PACKET_TAG_DUMMYNET, NULL);
		KKASSERT(mtag != NULL);
		args.rule = ((struct dn_pkt *)m_tag_data(mtag))->dn_priv;
		KKASSERT(args.rule != NULL);

		m_tag_delete(m, mtag);
		m->m_pkthdr.fw_flags &= ~DUMMYNET_MBUF_TAGGED;
	} else {
		args.rule = NULL;
	}

	args.eh = NULL;
	args.m = m;
	args.oif = ifp;
	ret = ipfw_chk(&args);
	m = args.m;

	if (m == NULL) {
		error = EACCES;
		goto back;
	}

	switch (ret) {
	case IP_FW_PASS:
		break;

	case IP_FW_DENY:
		m_freem(m);
		m = NULL;
		error = EACCES;
		break;

	case IP_FW_DUMMYNET:
		ipfw_dummynet_io(m, args.cookie, DN_TO_IP_OUT, &args);
		break;

	case IP_FW_TEE:
		tee = 1;
		/* FALL THROUGH */

	case IP_FW_DIVERT:
		if (ip_divert_p != NULL) {
			m = ip_divert_p(m, tee, 0);
		} else {
			m_freem(m);
			m = NULL;
			/* not sure this is the right error msg */
			error = EACCES;
		}
		break;

	default:
		panic("unknown ipfw return value: %d", ret);
	}
back:
	*m0 = m;
	return error;
}

static void
ipfw_hook(void)
{
	struct pfil_head *pfh;

	ASSERT_NETISR0;

	pfh = pfil_head_get(PFIL_TYPE_AF, AF_INET);
	if (pfh == NULL)
		return;

	pfil_add_hook(ipfw_check_in, NULL, PFIL_IN, pfh);
	pfil_add_hook(ipfw_check_out, NULL, PFIL_OUT, pfh);
}

static void
ipfw_dehook(void)
{
	struct pfil_head *pfh;

	ASSERT_NETISR0;

	pfh = pfil_head_get(PFIL_TYPE_AF, AF_INET);
	if (pfh == NULL)
		return;

	pfil_remove_hook(ipfw_check_in, NULL, PFIL_IN, pfh);
	pfil_remove_hook(ipfw_check_out, NULL, PFIL_OUT, pfh);
}

static int
ipfw_sysctl_dyncnt(SYSCTL_HANDLER_ARGS)
{
	int dyn_cnt;

	dyn_cnt = ipfw_state_cntcoll();
	dyn_cnt += ipfw_gd.ipfw_trkcnt_cnt;

	return (sysctl_handle_int(oidp, &dyn_cnt, 0, req));
}

static int
ipfw_sysctl_statecnt(SYSCTL_HANDLER_ARGS)
{
	int state_cnt;

	state_cnt = ipfw_state_cntcoll();
	return (sysctl_handle_int(oidp, &state_cnt, 0, req));
}

static int
ipfw_sysctl_statemax(SYSCTL_HANDLER_ARGS)
{
	int state_max, error;

	state_max = ipfw_state_max;
	error = sysctl_handle_int(oidp, &state_max, 0, req);
	if (error || req->newptr == NULL)
		return (error);

	if (state_max < 1)
		return (EINVAL);

	ipfw_state_max_set(state_max);
	return (0);
}

static int
ipfw_sysctl_dynmax(SYSCTL_HANDLER_ARGS)
{
	int dyn_max, error;

	dyn_max = ipfw_state_max + ipfw_track_max;

	error = sysctl_handle_int(oidp, &dyn_max, 0, req);
	if (error || req->newptr == NULL)
		return (error);

	if (dyn_max < 2)
		return (EINVAL);

	ipfw_state_max_set(dyn_max / 2);
	ipfw_track_max = dyn_max / 2;
	return (0);
}

static void
ipfw_sysctl_enable_dispatch(netmsg_t nmsg)
{
	int enable = nmsg->lmsg.u.ms_result;

	if (fw_enable == enable)
		goto reply;

	fw_enable = enable;
	if (fw_enable)
		ipfw_hook();
	else
		ipfw_dehook();
reply:
	netisr_replymsg(&nmsg->base, 0);
}

static int
ipfw_sysctl_enable(SYSCTL_HANDLER_ARGS)
{
	struct netmsg_base nmsg;
	int enable, error;

	enable = fw_enable;
	error = sysctl_handle_int(oidp, &enable, 0, req);
	if (error || req->newptr == NULL)
		return error;

	netmsg_init(&nmsg, NULL, &curthread->td_msgport, MSGF_PRIORITY,
	    ipfw_sysctl_enable_dispatch);
	nmsg.lmsg.u.ms_result = enable;

	return netisr_domsg(&nmsg, 0);
}

static int
ipfw_sysctl_autoinc_step(SYSCTL_HANDLER_ARGS)
{
	return sysctl_int_range(oidp, arg1, arg2, req,
	       IPFW_AUTOINC_STEP_MIN, IPFW_AUTOINC_STEP_MAX);
}

static int
ipfw_sysctl_scancnt(SYSCTL_HANDLER_ARGS)
{

	return sysctl_int_range(oidp, arg1, arg2, req, 1, INT_MAX);
}

static int
ipfw_sysctl_stat(SYSCTL_HANDLER_ARGS)
{
	u_long stat = 0;
	int cpu, error;

	for (cpu = 0; cpu < netisr_ncpus; ++cpu)
		stat += *((u_long *)((uint8_t *)ipfw_ctx[cpu] + arg2));

	error = sysctl_handle_long(oidp, &stat, 0, req);
	if (error || req->newptr == NULL)
		return (error);

	/* Zero out this stat. */
	for (cpu = 0; cpu < netisr_ncpus; ++cpu)
		*((u_long *)((uint8_t *)ipfw_ctx[cpu] + arg2)) = 0;
	return (0);
}

static void
ipfw_ctx_init_dispatch(netmsg_t nmsg)
{
	struct netmsg_ipfw *fwmsg = (struct netmsg_ipfw *)nmsg;
	struct ipfw_context *ctx;
	struct ip_fw *def_rule;

	ASSERT_NETISR_NCPUS(mycpuid);

	ctx = kmalloc(sizeof(*ctx), M_IPFW, M_WAITOK | M_ZERO);

	RB_INIT(&ctx->ipfw_state_tree);
	TAILQ_INIT(&ctx->ipfw_state_list);

	RB_INIT(&ctx->ipfw_track_tree);
	TAILQ_INIT(&ctx->ipfw_track_list);

	callout_init_mp(&ctx->ipfw_stateto_ch);
	netmsg_init(&ctx->ipfw_stateexp_nm, NULL, &netisr_adone_rport,
	    MSGF_DROPABLE | MSGF_PRIORITY, ipfw_state_expire_dispatch);
	ctx->ipfw_stateexp_anch.st_type = O_ANCHOR;
	netmsg_init(&ctx->ipfw_stateexp_more, NULL, &netisr_adone_rport,
	    MSGF_DROPABLE, ipfw_state_expire_more_dispatch);

	callout_init_mp(&ctx->ipfw_trackto_ch);
	netmsg_init(&ctx->ipfw_trackexp_nm, NULL, &netisr_adone_rport,
	    MSGF_DROPABLE | MSGF_PRIORITY, ipfw_track_expire_dispatch);
	netmsg_init(&ctx->ipfw_trackexp_more, NULL, &netisr_adone_rport,
	    MSGF_DROPABLE, ipfw_track_expire_more_dispatch);

	callout_init_mp(&ctx->ipfw_keepalive_ch);
	netmsg_init(&ctx->ipfw_keepalive_nm, NULL, &netisr_adone_rport,
	    MSGF_DROPABLE | MSGF_PRIORITY, ipfw_keepalive_dispatch);
	ctx->ipfw_keepalive_anch.st_type = O_ANCHOR;
	netmsg_init(&ctx->ipfw_keepalive_more, NULL, &netisr_adone_rport,
	    MSGF_DROPABLE, ipfw_keepalive_more_dispatch);

	ipfw_ctx[mycpuid] = ctx;

	def_rule = kmalloc(sizeof(*def_rule), M_IPFW, M_WAITOK | M_ZERO);

	def_rule->act_ofs = 0;
	def_rule->rulenum = IPFW_DEFAULT_RULE;
	def_rule->cmd_len = 1;
	def_rule->set = IPFW_DEFAULT_SET;

	def_rule->cmd[0].len = 1;
#ifdef IPFIREWALL_DEFAULT_TO_ACCEPT
	def_rule->cmd[0].opcode = O_ACCEPT;
#else
	if (filters_default_to_accept)
		def_rule->cmd[0].opcode = O_ACCEPT;
	else
		def_rule->cmd[0].opcode = O_DENY;
#endif

	def_rule->refcnt = 1;
	def_rule->cpuid = mycpuid;

	/* Install the default rule */
	ctx->ipfw_default_rule = def_rule;
	ctx->ipfw_layer3_chain = def_rule;

	/* Link rule CPU sibling */
	ipfw_link_sibling(fwmsg, def_rule);

	/* Statistics only need to be updated once */
	if (mycpuid == 0)
		ipfw_inc_static_count(def_rule);

	netisr_forwardmsg(&nmsg->base, mycpuid + 1);
}

static void
ipfw_init_dispatch(netmsg_t nmsg)
{
	struct netmsg_ipfw fwmsg;
	int error = 0, cpu;

	if (IPFW_LOADED) {
		kprintf("IP firewall already loaded\n");
		error = EEXIST;
		goto reply;
	}

	/* Initialize global track tree. */
	RB_INIT(&ipfw_gd.ipfw_trkcnt_tree);
	IPFW_TRKCNT_TOKINIT;

	ipfw_state_max_set(ipfw_state_max);
	ipfw_state_headroom = 8 * netisr_ncpus;

	bzero(&fwmsg, sizeof(fwmsg));
	netmsg_init(&fwmsg.base, NULL, &curthread->td_msgport, MSGF_PRIORITY,
	    ipfw_ctx_init_dispatch);
	netisr_domsg_global(&fwmsg.base);

	ip_fw_chk_ptr = ipfw_chk;
	ip_fw_ctl_ptr = ipfw_ctl;
	ip_fw_dn_io_ptr = ipfw_dummynet_io;

	kprintf("ipfw2 initialized, default to %s, logging ",
		ipfw_ctx[mycpuid]->ipfw_default_rule->cmd[0].opcode ==
		O_ACCEPT ? "accept" : "deny");

#ifdef IPFIREWALL_VERBOSE
	fw_verbose = 1;
#endif
#ifdef IPFIREWALL_VERBOSE_LIMIT
	verbose_limit = IPFIREWALL_VERBOSE_LIMIT;
#endif
	if (fw_verbose == 0) {
		kprintf("disabled\n");
	} else if (verbose_limit == 0) {
		kprintf("unlimited\n");
	} else {
		kprintf("limited to %d packets/entry by default\n",
			verbose_limit);
	}

	ip_fw_loaded = 1;
	for (cpu = 0; cpu < netisr_ncpus; ++cpu) {
		callout_reset_bycpu(&ipfw_ctx[cpu]->ipfw_stateto_ch, hz,
		    ipfw_state_expire_ipifunc, NULL, cpu);
		callout_reset_bycpu(&ipfw_ctx[cpu]->ipfw_trackto_ch, hz,
		    ipfw_track_expire_ipifunc, NULL, cpu);
		callout_reset_bycpu(&ipfw_ctx[cpu]->ipfw_keepalive_ch, hz,
		    ipfw_keepalive, NULL, cpu);
	}

	if (fw_enable)
		ipfw_hook();
reply:
	netisr_replymsg(&nmsg->base, error);
}

static int
ipfw_init(void)
{
	struct netmsg_base smsg;

	netmsg_init(&smsg, NULL, &curthread->td_msgport, MSGF_PRIORITY,
	    ipfw_init_dispatch);
	return netisr_domsg(&smsg, 0);
}

#ifdef KLD_MODULE

static void
ipfw_ctx_fini_dispatch(netmsg_t nmsg)
{
	struct ipfw_context *ctx = ipfw_ctx[mycpuid];

	ASSERT_NETISR_NCPUS(mycpuid);

	callout_stop_sync(&ctx->ipfw_stateto_ch);
	callout_stop_sync(&ctx->ipfw_trackto_ch);
	callout_stop_sync(&ctx->ipfw_keepalive_ch);

	crit_enter();
	netisr_dropmsg(&ctx->ipfw_stateexp_more);
	netisr_dropmsg(&ctx->ipfw_stateexp_nm);
	netisr_dropmsg(&ctx->ipfw_trackexp_more);
	netisr_dropmsg(&ctx->ipfw_trackexp_nm);
	netisr_dropmsg(&ctx->ipfw_keepalive_more);
	netisr_dropmsg(&ctx->ipfw_keepalive_nm);
	crit_exit();

	netisr_forwardmsg(&nmsg->base, mycpuid + 1);
}

static void
ipfw_fini_dispatch(netmsg_t nmsg)
{
	struct netmsg_base nm;
	int error = 0, cpu;

	if (ipfw_gd.ipfw_refcnt != 0) {
		error = EBUSY;
		goto reply;
	}

	ip_fw_loaded = 0;
	ipfw_dehook();

	/* Synchronize any inflight state/track expire IPIs. */
	lwkt_synchronize_ipiqs("ipfwfini");

	netmsg_init(&nm, NULL, &curthread->td_msgport, MSGF_PRIORITY,
	    ipfw_ctx_fini_dispatch);
	netisr_domsg_global(&nm);

	ip_fw_chk_ptr = NULL;
	ip_fw_ctl_ptr = NULL;
	ip_fw_dn_io_ptr = NULL;
	ipfw_flush(1 /* kill default rule */);

	/* Free pre-cpu context */
	for (cpu = 0; cpu < netisr_ncpus; ++cpu)
		kfree(ipfw_ctx[cpu], M_IPFW);

	kprintf("IP firewall unloaded\n");
reply:
	netisr_replymsg(&nmsg->base, error);
}

static int
ipfw_fini(void)
{
	struct netmsg_base smsg;

	netmsg_init(&smsg, NULL, &curthread->td_msgport, MSGF_PRIORITY,
	    ipfw_fini_dispatch);
	return netisr_domsg(&smsg, 0);
}

#endif	/* KLD_MODULE */

static int
ipfw_modevent(module_t mod, int type, void *unused)
{
	int err = 0;

	switch (type) {
	case MOD_LOAD:
		err = ipfw_init();
		break;

	case MOD_UNLOAD:
#ifndef KLD_MODULE
		kprintf("ipfw statically compiled, cannot unload\n");
		err = EBUSY;
#else
		err = ipfw_fini();
#endif
		break;
	default:
		break;
	}
	return err;
}

static moduledata_t ipfwmod = {
	"ipfw",
	ipfw_modevent,
	0
};
DECLARE_MODULE(ipfw, ipfwmod, SI_SUB_PROTO_END, SI_ORDER_ANY);
MODULE_VERSION(ipfw, 1);
