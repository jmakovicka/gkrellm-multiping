/*____________________________________________________________________________
        
        pinger - gkrellm multiping helper app

        Copyright (C) 2002 Jindrich Makovicka

        This program is free software; you can redistribute it and/or modify
        it under the terms of the GNU General Public License as published by
        the Free Software Foundation; either version 2 of the License, or
        (at your option) any later version.

        This program is distributed in the hope that it will be useful,
        but WITHOUT ANY WARRANTY; without even the implied warranty of
        MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
        GNU General Public License for more details.

        You should have received a copy of the GNU General Public License
        along with this program; if not, Write to the Free Software
        Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
____________________________________________________________________________*/

#include <sys/param.h>
#include <sys/socket.h>
#include <sys/file.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/signal.h>

#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/ip_icmp.h>
#include <arpa/inet.h>
#include <netdb.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <glib.h>

#define STORM_PHASE 0
#define STANDBY_PHASE 1

#define	DEFDATALEN	(64 - 8)	/* default data length */
#define	MAXIPLEN	60
#define	MAXICMPLEN	76
#define	MAXPACKET	(65536 - 60 - 8)	/* max packet size */

#define	MAX_DUP_CHK	(8 * 128)

#define	A(bit)		h->rcvd_tbl[(bit)>>3]	/* identify byte in array */
#define	B(bit)		(1 << ((bit) & 0x07))	/* identify bit in byte */
#define	SET(bit)	(A(bit) |= B(bit))
#define	CLR(bit)	(A(bit) &= (~B(bit)))
#define	TST(bit)	(A(bit) & B(bit))

int icmp_socket;
static int ident;		/* process id to identify our packets */
static int datalen = DEFDATALEN;
static long ntransmitted = 0;	/* sequence # for outbound packets = #sent */
u_char outpack[MAXPACKET];
u_char packet[DEFDATALEN + MAXIPLEN + MAXICMPLEN];
int packlen = DEFDATALEN + MAXIPLEN + MAXICMPLEN;

int hostcnt = 0;

int has_pinged;

typedef struct _host_data {
    int nhost;			// cislo poce
    GString *hostname, *percentage, *sent_str, *recv_str, *msg, *shortmsg;
    int dynamic;
    int dummy;
    struct sockaddr addr;
    int sent, recv, rep;
    int tmp_sent, tmp_recv, tmp_rep;
    int dupflag, error_flag;
    long tsum, tmp_tsum;
    struct icmp icp;
    char rcvd_tbl[MAX_DUP_CHK / 8];
    int phase;
    int counter;
    int updatefreq;
    int delay;
} host_data;

GList *hosts = NULL;

void update_host_stats(host_data * h);
void update_host_packinfo(host_data * h);

static host_data *host_malloc()
{
    host_data *h = (host_data *) g_malloc(sizeof(host_data));

    memset(h, 0, sizeof(host_data));

    h->hostname = g_string_new(NULL);

    h->percentage = g_string_new(NULL);
    h->sent_str = g_string_new(NULL);
    h->recv_str = g_string_new(NULL);
    h->msg = g_string_new(NULL);
    h->shortmsg = g_string_new(NULL);
    return h;
}

static void host_free(host_data * h)
{
    g_string_free(h->hostname, TRUE);

    g_string_free(h->percentage, TRUE);
    g_string_free(h->sent_str, TRUE);
    g_string_free(h->recv_str, TRUE);
    g_string_free(h->msg, TRUE);
    g_string_free(h->shortmsg, TRUE);
    g_free(h);
}

static gint compare_nhost(gconstpointer a, gconstpointer b)
{
    return ((host_data *) a)->nhost - *(int *) b;
}

static gint compare_nhost2(gconstpointer a, gconstpointer b)
{
    return ((host_data *) a)->nhost - ((host_data *) b)->nhost;
}

static gint compare_delay(gconstpointer a, gconstpointer b)
{
    return ((host_data *) b)->delay - ((host_data *) a)->delay;
}

/*
 * in_cksum --
 *	Checksum routine for Internet Protocol family headers (C Version)
 */
static int in_cksum(u_short * addr, int len)
{
    int nleft = len;
    u_short *w = addr;
    int sum = 0;
    u_short answer = 0;

    /*
     * Our algorithm is simple, using a 32 bit accumulator (sum), we add
     * sequential 16 bit words to it, and at the end, fold back all the
     * carry bits from the top 16 bits into the lower 16 bits.
     */
    while (nleft > 1) {
	sum += *w++;
	nleft -= 2;
    }

    /* mop up an odd byte, if necessary */
    if (nleft == 1) {
	*(u_char *) (&answer) = *(u_char *) w;
	sum += answer;
    }

    /* add back carry outs from top 16 bits to low 16 bits */
    sum = (sum >> 16) + (sum & 0xffff);	/* add hi 16 to low 16 */
    sum += (sum >> 16);		/* add carry */
    answer = ~sum;		/* truncate to 16 bits */
    return (answer);
}

static void write_result(host_data * h, gchar * msg, gchar * shortmsg)
{
    g_string_assign(h->msg, msg);
    g_string_assign(h->shortmsg, shortmsg);
}

/*
 * pinger --
 * 	Compose and transmit an ICMP ECHO REQUEST packet.  The IP packet
 * will be added on by the kernel.  The ID field is our UNIX process ID,
 * and the sequence number is an ascending integer.  The first 8 bytes
 * of the data portion are used to hold a UNIX "timeval" struct in VAX
 * byte-order, to compute the round-trip time.
 *
 * Another 4 bytes are the index of the host being pinged -JM
 */
static void pinger(host_data * h)
{
    struct icmp *icp;
    int cc;
    int i;

    has_pinged = 1;

//    fprintf(stderr, "pinger: pinging host No. %d\n", h->nhost);

    icp = (struct icmp *) outpack;
    icp->icmp_type = ICMP_ECHO;
    icp->icmp_code = 0;
    icp->icmp_cksum = 0;
    icp->icmp_seq = ntransmitted++;
    icp->icmp_id = ident;	/* ID */

    h->sent++;
    h->tmp_sent++;

    CLR(icp->icmp_seq % MAX_DUP_CHK);

    (void) gettimeofday((struct timeval *) &outpack[8],
			(struct timezone *) NULL);

    *(int *) &outpack[8 + sizeof(struct timeval)] = h->nhost;

    cc = datalen + 8;		/* skips ICMP portion */

    /* compute ICMP checksum here */
    icp->icmp_cksum = in_cksum((u_short *) icp, cc);

    i = sendto(icmp_socket, (char *) outpack, cc, 0, &h->addr,
	       sizeof(struct sockaddr));

    if (i < 0 || i != cc) {
	write_result(h, "Error sending packet", "Err");
    }
}

/*
 * pr_icmph --
 *	Print a descriptive string about an ICMP header.
 */
static gchar *pr_icmph(struct icmp *icp)
{
    GString *s = g_string_new(NULL);
    gchar *c;

    switch (icp->icmp_type) {
    case ICMP_ECHOREPLY:
	g_string_assign(s, "Echo Reply");
	/* XXX ID + Seq + Data */
	break;
    case ICMP_DEST_UNREACH:
	switch (icp->icmp_code) {
	case ICMP_NET_UNREACH:
	    g_string_assign(s, "Destination Net Unreachable");
	    break;
	case ICMP_HOST_UNREACH:
	    g_string_assign(s, "Destination Host Unreachable");
	    break;
	case ICMP_PROT_UNREACH:
	    g_string_assign(s, "Destination Protocol Unreachable");
	    break;
	case ICMP_PORT_UNREACH:
	    g_string_assign(s, "Destination Port Unreachable");
	    break;
	case ICMP_FRAG_NEEDED:
	    g_string_assign(s, "Frag needed and DF set");
	    break;
	case ICMP_SR_FAILED:
	    g_string_assign(s, "Source Route Failed");
	    break;
	case ICMP_NET_UNKNOWN:
	    g_string_assign(s, "Network Unknown");
	    break;
	case ICMP_HOST_UNKNOWN:
	    g_string_assign(s, "Host Unknown");
	    break;
	case ICMP_HOST_ISOLATED:
	    g_string_assign(s, "Host Isolated");
	    break;
	case ICMP_NET_UNR_TOS:
	    g_string_assign(s,
			    "Destination Network Unreachable At This TOS");
	    break;
	case ICMP_HOST_UNR_TOS:
	    g_string_assign(s, "Destination Host Unreachable At This TOS");
	    break;
#ifdef ICMP_PKT_FILTERED
	case ICMP_PKT_FILTERED:
	    g_string_assign(s, "Packet Filtered");
	    break;
#endif
#ifdef ICMP_PREC_VIOLATION
	case ICMP_PREC_VIOLATION:
	    g_string_assign(s, "Precedence Violation");
	    break;
#endif
#ifdef ICMP_PREC_CUTOFF
	case ICMP_PREC_CUTOFF:
	    g_string_assign(s, "Precedence Cutoff");
	    break;
#endif
	default:
	    g_string_sprintf(s, "Dest Unreachable, Unknown Code: %d",
			     icp->icmp_code);
	    break;
	}
	break;
    case ICMP_SOURCE_QUENCH:
	g_string_assign(s, "Source Quench");
	break;
    case ICMP_REDIRECT:
	switch (icp->icmp_code) {
	case ICMP_REDIR_NET:
	    g_string_assign(s, "Redirect Network");
	    break;
	case ICMP_REDIR_HOST:
	    g_string_assign(s, "Redirect Host");
	    break;
	case ICMP_REDIR_NETTOS:
	    g_string_assign(s, "Redirect Type of Service and Network");
	    break;
	case ICMP_REDIR_HOSTTOS:
	    g_string_assign(s, "Redirect Type of Service and Host");
	    break;
	default:
	    g_string_sprintf(s, "Redirect, Bad Code: %d", icp->icmp_code);
	    break;
	}
	g_string_sprintfa(s, " (New addr: %s)",
			  inet_ntoa(icp->icmp_gwaddr));
	break;
    case ICMP_ECHO:
	g_string_assign(s, "Echo Request");
	/* XXX ID + Seq + Data */
	break;
    case ICMP_TIME_EXCEEDED:
	switch (icp->icmp_code) {
	case ICMP_EXC_TTL:
	    g_string_assign(s, "Time to live exceeded");
	    break;
	case ICMP_EXC_FRAGTIME:
	    g_string_assign(s, "Frag reassembly time exceeded");
	    break;
	default:
	    g_string_sprintf(s, "Time exceeded, Bad Code: %d",
			     icp->icmp_code);
	    break;
	}
	break;
    case ICMP_PARAMETERPROB:
	g_string_sprintf(s, "Parameter problem: IP address = %s",
			 inet_ntoa(icp->icmp_gwaddr));
	break;
    case ICMP_TIMESTAMP:
	g_string_assign(s, "Timestamp");
	/* XXX ID + Seq + 3 timestamps */
	break;
    case ICMP_TIMESTAMPREPLY:
	g_string_assign(s, "Timestamp Reply");
	/* XXX ID + Seq + 3 timestamps */
	break;
    case ICMP_INFO_REQUEST:
	g_string_assign(s, "Information Request");
	/* XXX ID + Seq */
	break;
    case ICMP_INFO_REPLY:
	g_string_assign(s, "Information Reply");
	/* XXX ID + Seq */
	break;
#ifdef ICMP_MASKREQ
    case ICMP_MASKREQ:
	g_string_assign(s, "Address Mask Request");
	break;
#endif
#ifdef ICMP_MASKREPLY
    case ICMP_MASKREPLY:
	g_string_assign(s, "Address Mask Reply");
	break;
#endif
    default:
	g_string_sprintf(s, "Bad ICMP type: %d", icp->icmp_type);
    }

    c = s->str;
    g_string_free(s, FALSE);
    return c;
}

/*
 * tvsub --
 *	Subtract 2 timeval structs:  out = out - in.  Out is assumed to
 * be >= in.
 */
static void tvsub(struct timeval *out, struct timeval *in)
{
    if ((out->tv_usec -= in->tv_usec) < 0) {
	--out->tv_sec;
	out->tv_usec += 1000000;
    }
    out->tv_sec -= in->tv_sec;
}

// process a received packet
void pr_pack(char *buf, int cc, struct sockaddr_in *from)
{
    struct icmp *icp;
    struct ip *ip;
    struct timeval tv, *tp;
    long triptime = 0;
    int hlen;
    host_data *h;

    (void) gettimeofday(&tv, (struct timezone *) NULL);

    if (cc < datalen + ICMP_MINLEN)
	return;

    /* Check the IP header */
    ip = (struct ip *) buf;
    hlen = ip->ip_hl << 2;

    /* Now the ICMP part */
    cc -= hlen;
    icp = (struct icmp *) (buf + hlen);

    if (icp->icmp_type == ICMP_ECHOREPLY) {
	if (icp->icmp_id != ident)
	    return;		/* 'Twas not our ECHO */

	h = (host_data *) g_list_find_custom(hosts,
					     (int *) &icp->
					     icmp_data[sizeof
						       (struct timeval)],
					     compare_nhost)->data;
	if (h == NULL) return; /* host not found */

	++h->recv;
	++h->tmp_recv;

	tp = (struct timeval *) icp->icmp_data;
	tvsub(&tv, tp);
	triptime = tv.tv_sec * 1000000 + tv.tv_usec;
	h->tsum += triptime;
	h->tmp_tsum += triptime;

	if (TST(icp->icmp_seq % MAX_DUP_CHK)) {
	    ++h->rep;
	    ++h->tmp_rep;
	    --h->recv;
	    --h->tmp_recv;
	    h->dupflag = 1;
	} else {
	    SET(icp->icmp_seq % MAX_DUP_CHK);
	    h->dupflag = 0;
	}
    } else {
	switch (icp->icmp_type) {
	case ICMP_ECHO:
	    return;
	case ICMP_SOURCE_QUENCH:
	case ICMP_REDIRECT:
	case ICMP_DEST_UNREACH:
	case ICMP_TIME_EXCEEDED:
	case ICMP_PARAMETERPROB:
	    {
		struct ip *iph = (struct ip *) (&icp->icmp_data);
		struct icmp *icp1 =
		    (struct icmp *) ((unsigned char *) iph +
				     iph->ip_hl * 4);
		int error_pkt;

		if (icp1->icmp_type != ICMP_ECHO ||
		    iph->ip_src.s_addr != ip->ip_dst.s_addr ||
		    icp1->icmp_id != ident)
		    return;
		error_pkt = (icp->icmp_type != ICMP_REDIRECT &&
			     icp->icmp_type != ICMP_SOURCE_QUENCH);

		h = (host_data *) g_list_find_custom(hosts,
						     (int *) &icp1->
						     icmp_data[sizeof
							       (struct
								timeval)],
						     compare_nhost)->data;
		if (h) {
		    h->icp = *icp;
		    h->error_flag = 1;
		}

	    }
	}
    }
}

void clear_tmp_flags(host_data * h)
{
    h->tmp_tsum = 0;
    h->tmp_sent = 0;
    h->tmp_recv = 0;
    h->tmp_rep = 0;
    h->dupflag = 0;
    h->error_flag = 0;
}

void dump_host(host_data * h)
{
//    fprintf(stderr, "dump_host: %d\n", h->nhost);
    printf("%s\n", h->percentage->str);
    printf("%s\n", h->sent_str->str);
    printf("%s\n", h->recv_str->str);
    printf("%s\n", h->msg->str);
    printf("%s\n", h->shortmsg->str);
}

// recheck the dns (needed for dialup users or dynamic DNS)
int update_dns(host_data *h) 
{
    struct hostent *he;

    he = gethostbyname(h->hostname->str);
    if (he && he->h_addr_list[0]) {
	((struct sockaddr_in *) &h->addr)->sin_addr  = *(struct in_addr*)he->h_addr_list[0];
	return 0;
    }

    return 1;
}


void ping_host(host_data * h)
{
    gchar *msg;

    if (h->dummy) {
	if (h->counter == 120) {
	    if (update_dns(h) == 0) {
		h->dummy = 0;
		update_host_stats(h);
		clear_tmp_flags(h);
		update_host_packinfo(h);
		h->phase = STORM_PHASE;
	    } else {
		h->phase = STANDBY_PHASE;
	    }
	    h->delay = 0;
	    h->counter = -1;
	}
	h->counter++;
	return;
    } 

    if (!h->dummy && h->dynamic && h->counter == 0) {
	update_dns(h);
    }

    if (h->error_flag) {
	msg = pr_icmph(&h->icp);
	write_result(h, msg, "Err");
	g_free(msg);
    }

//    fprintf(stderr, "pinger: ping_host, No. %d, delay = %d\n", h->nhost, h->delay);

    switch (h->phase) {
    case STORM_PHASE:
	if (h->counter == 7 || h->counter == 15 || h->counter == 23) {
	    update_host_stats(h);
	    clear_tmp_flags(h);
	}
	if ((h->counter >= 0 && h->counter < 4)
	    || (h->counter >= 8 && h->counter < 12)
	    || (h->counter >= 16 && h->counter < 20)) {
	    if (has_pinged) {
		h->delay++;
		goto dontpingyet;
	    }
	    pinger(h);
	    h->delay = 0;
	}
	
	if (h->counter == 59) {
	    h->counter = -1;
	    h->phase = STANDBY_PHASE;
	}
	break;
    case STANDBY_PHASE:
	if (h->counter == 7) {
	    update_host_stats(h);
	    clear_tmp_flags(h);
	}
	if (h->counter >= 0 && h->counter < 4) {
	    if (has_pinged) {
		h->delay++;
		goto dontpingyet;
	    }
	    pinger(h);
	    h->delay = 0;
	}
	if (h->counter == h->updatefreq) {
	    h->counter = -1;
	    h->phase = STANDBY_PHASE;
	}
	break;
    }

    h->counter++;
dontpingyet:
    update_host_packinfo(h);
}

gint timeout_callback()
{
    has_pinged = 0;
    hosts = g_list_sort(hosts, compare_delay);
    g_list_foreach(hosts, (GFunc) ping_host, NULL);
    hosts = g_list_sort(hosts, compare_nhost2);
    g_list_foreach(hosts, (GFunc) dump_host, NULL);
    fflush(stdout);
    return TRUE;
}

void receiver()
{
    int cc;
    struct sockaddr_in from;
    size_t fromlen;
    struct timeval tv,tv_old,tv_new;
    fd_set rfds;
    int avail;

    FD_ZERO(&rfds);
    FD_SET(icmp_socket, &rfds);

    tv.tv_usec = 500000;
    tv.tv_sec = 0;

    gettimeofday(&tv_old, NULL);
    fromlen = sizeof(from);
    for (;;) {
	FD_ZERO(&rfds);
	FD_SET(icmp_socket, &rfds);

	tv.tv_usec = 100000;
	tv.tv_sec = 0;

	avail = select(icmp_socket + 1, &rfds, NULL, NULL, &tv);

	if (avail) {
	    if ((cc = recvfrom(icmp_socket, (char *) packet, packlen, 0,
			       (struct sockaddr *) &from, &fromlen)) < 0) {
		perror("ping: recvfrom");
	    } else {
		pr_pack((char *) packet, cc, &from);
	    }
	}
	gettimeofday(&tv_new, NULL);
	tvsub(&tv_new, &tv_old);
	if (tv_new.tv_sec >= 1) {
	    gettimeofday(&tv_old, NULL);
	    timeout_callback();
	}
    }
}

void update_host_packinfo(host_data * h)
{
    g_string_sprintf(h->sent_str, "%d", h->sent);
    g_string_sprintf(h->recv_str, "%d", h->recv);
}

void update_host_stats(host_data * h)
{
    long trip;
    GString *s = g_string_new(NULL);
    GString *s2 = g_string_new(NULL);

    if (h->tmp_sent > 0) {
	g_string_sprintf(s, "%d", h->tmp_recv * 100 / h->tmp_sent);
	g_string_assign(h->percentage, s->str);
    } else {
	g_string_assign(h->percentage, "");
    }

    if (h->tmp_recv > 0) {
	trip = h->tmp_tsum / (h->tmp_recv + h->tmp_rep);
	if (trip >= 1000000) {
	    g_string_sprintf(s, "%ld.%03ld s", trip / 1000000,
			     (trip % 1000000) / 1000);
	    g_string_sprintf(s2, ">s");
	} else if (trip >= 10000) {
	    g_string_sprintf(s, "%ld.%03ld ms", trip / 1000, trip % 1000);
	    g_string_sprintf(s2, "%ld", trip / 1000);
	} else if (trip >= 1000) {
	    g_string_sprintf(s, "%ld.%03ld ms", trip / 1000, trip % 1000);
	    g_string_sprintf(s2, "%ld.%01ld", trip / 1000,
			     (trip % 1000) / 100);
	} else {
	    g_string_sprintf(s, "0.%01ld ms", trip / 100);
	    g_string_sprintf(s2, "0.%01ld", trip / 100);
	}
	write_result(h, s->str, s2->str);
    }

    g_string_free(s, TRUE);
    g_string_free(s2, TRUE);

    if (h->dummy) {
	write_result(h, "Dummy host", "##");
	return;
    }

    if (!h->error_flag) {
	if (h->tmp_sent > 0 && h->tmp_recv == 0)
	    write_result(h, "Request timed out", "TO");
    }

}


void append_host(struct in_addr ip, char * hostname, char * updatefreq, char * dynamic, int dummy)
{
    host_data *h = host_malloc();

    ((struct sockaddr_in *) &h->addr)->sin_addr = ip;
    ((struct sockaddr_in *) &h->addr)->sin_family = AF_INET;

    g_string_assign(h->hostname, hostname);

    h->dummy = dummy;

    h->nhost = hostcnt++;

    h->sent = 0;
    h->recv = 0;
    h->rep = 0;
    h->dupflag = 0;
    h->error_flag = 0;
    h->tsum = 0;

    h->tmp_sent = 0;
    h->tmp_recv = 0;
    h->tmp_rep = 0;
    h->tmp_tsum = 0;

    h->phase = STORM_PHASE;
    h->counter = 0;
    h->delay = 0;
    if (updatefreq) {
	h->updatefreq = atoi(updatefreq);
	if (h->updatefreq < 30) {
	    h->updatefreq = 30;
	}
	if (h->updatefreq > 3600) {
	    h->updatefreq = 3600;
	}
    } else {
	h->updatefreq = 59;
    }

    h->dynamic = atoi(dynamic) ? 1 : 0;

    hosts = g_list_append(hosts, h);

    update_host_stats(h);
    clear_tmp_flags(h);
    update_host_packinfo(h);
}

void free_hosts()
{
    g_list_foreach(hosts, (GFunc) host_free, NULL);
    g_list_free(hosts);
}

void term_signal(int i)
{
    free_hosts();
    exit(0);
}

void pipe_signal(int i)
{
    free_hosts();
    exit(0);
}

void hup_signal(int i)
{
    free_hosts();
    exit(0);
}

int main(int argc, char **argv)
{
    struct in_addr ip;
    struct hostent *h;
    int i;

    struct protoent *proto;
    if (!(proto = getprotobyname("icmp"))) {
	(void) fprintf(stderr, "pinger: unknown protocol icmp.\n");
	exit(2);
    }
    if ((icmp_socket = socket(AF_INET, SOCK_RAW, proto->p_proto)) < 0) {
	if (errno == EPERM) {
	    fprintf(stderr, "pinger: must run as root\n");
	} else
	    perror("pinger: socket");
	exit(2);
    }

    setuid(getuid());

    fcntl(icmp_socket, F_SETFL, O_NONBLOCK);
    ident = getpid() & 0xFFFF;

    for (i = 1; i < argc; i++) {
	h = gethostbyname(argv[i]);
	if (h && h->h_addr_list[0]) {
	    if (i <= argc-3) {
		append_host(*(struct in_addr*)h->h_addr_list[0], argv[i], argv[i+1], argv[i+2], 0);
		i+=2;
	    }
	} else if (i <= argc-3) {
	    memset(&ip, 0, sizeof(ip));
	    append_host((struct in_addr)ip, argv[i], argv[i+1], argv[i+2], 1); // dummy host
	    i+=2;
	}
    }

    signal(SIGTERM, term_signal);
    signal(SIGPIPE, pipe_signal);
    signal(SIGHUP, hup_signal);

    receiver();

    /* actually unreachable */

    free_hosts();
    return 0;
}
