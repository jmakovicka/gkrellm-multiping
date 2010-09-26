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

#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/ip_icmp.h>
#include <netinet/ip6.h>
#include <netinet/icmp6.h>
#include <arpa/inet.h>
#include <netdb.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <signal.h>
#include <poll.h>
#include <fcntl.h>
#include <unistd.h>
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <glib.h>

#define STORM_PHASE 0
#define STANDBY_PHASE 1

#define         MAX_DUP_CHK     (8 * 128)

#define         A(bit)          rcvd_tbl[(bit)>>3]      /* identify byte in array */
#define         B(bit)          (1 << ((bit) & 0x07))   /* identify bit in byte */
#define         SET(bit)        (A(bit) |= B(bit))
#define         CLR(bit)        (A(bit) &= (~B(bit)))
#define         TST(bit)        (A(bit) & B(bit))

int icmp_socket, icmp6_socket;
static int ident;               /* process id to identify our packets */
static long ntransmitted = 0;   /* sequence # for outbound packets = #sent */
static u_char rcvd_tbl[MAX_DUP_CHK / 8];
static u_char outpack[56];
static u_char packet[1024];

int hostcnt = 0;

int has_pinged;
int terminated = 0;

typedef struct _host_data {
    int nhost;                  // cislo poce
    GString *hostname, *percentage, *sent_str, *recv_str, *msg, *shortmsg;
    int dynamic;
    int dummy;
    union {
        struct sockaddr addr;
        struct sockaddr_in in4;
        struct sockaddr_in6 in6;
    } addr;
    int sent, recv, rep;
    int tmp_sent, tmp_recv, tmp_rep;
    int dupflag, error_flag;
    long tsum, tmp_tsum;
    union {
        struct icmp v4;
        struct icmp6_hdr v6;
    } icmp;
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
    return ((host_data *) a)->nhost - *(int32_t *) b;
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
 *      Checksum routine for Internet Protocol family headers (C Version)
 */
static int in_cksum(u_char * addr, int len)
{
    int nleft = len;
    u_short *w = (u_short *)addr;
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
    sum = (sum >> 16) + (sum & 0xffff);         /* add hi 16 to low 16 */
    sum += (sum >> 16);                 /* add carry */
    answer = ~sum;              /* truncate to 16 bits */
    return (answer);
}

static void write_result(host_data * h, gchar * msg, gchar * shortmsg)
{
    g_string_assign(h->msg, msg);
    g_string_assign(h->shortmsg, shortmsg);
}

static void host_check_dup(host_data * h, int seq)
{
    if (TST(seq % MAX_DUP_CHK)) {
        ++h->rep;
        ++h->tmp_rep;
        --h->recv;
        --h->tmp_recv;
        h->dupflag = 1;
    } else {
        SET(seq % MAX_DUP_CHK);
        h->dupflag = 0;
    }
}

static void set_packet_data(u_char *buf, host_data *h)
{
    (void) gettimeofday((struct timeval *) buf,
                        (struct timezone *) NULL);

    *(int32_t *) (buf + sizeof(struct timeval)) = h->nhost;
}

static host_data * get_packet_data(u_char *buf, host_data **h, struct timeval *tv)
{
    GList *l;
    int32_t *hidp = (int32_t *) (buf + sizeof(struct timeval));

    *tv = *(struct timeval *) buf;

    l = g_list_find_custom(hosts, hidp, compare_nhost);

    if (l) {
        *h = (host_data *) l->data;
    } else {
        *h = 0;
        fprintf(stderr, "Unknown host ID (%" PRIi32 ")\n", *hidp);
    }

    return *h;
}

/*
 * pinger --
 *      Compose and transmit an ICMP ECHO REQUEST packet.  The IP packet
 * will be added on by the kernel.  The ID field is our UNIX process ID,
 * and the sequence number is an ascending integer.  The first 8 bytes
 * of the data portion are used to hold a UNIX "timeval" struct in VAX
 * byte-order, to compute the round-trip time.
 *
 * Another 4 bytes are the index of the host being pinged -JM
 */
static void pinger4(host_data * h)
{
    struct icmp *icp;
    int i;

    has_pinged = 1;

    CLR(ntransmitted % MAX_DUP_CHK);

    icp = (struct icmp *) outpack;
    icp->icmp_type = ICMP_ECHO;
    icp->icmp_code = 0;
    icp->icmp_cksum = 0;
    icp->icmp_seq = htons(ntransmitted++);
    icp->icmp_id = ident;       /* ID */

    h->sent++;
    h->tmp_sent++;

    set_packet_data(icp->icmp_data, h);

    /* compute ICMP checksum here */
    icp->icmp_cksum = in_cksum(outpack, sizeof(outpack));

    i = sendto(icmp_socket, outpack, sizeof(outpack), 0,
               (struct sockaddr *)&h->addr, sizeof(h->addr));

    if (i < 0 || i != sizeof(outpack)) {
        perror("pinger: sendto");
        write_result(h, "Error sending packet", "Err");
    }
}

static void pinger6(host_data * h)
{
    struct icmp6_hdr *icmp;
    int i;

    has_pinged = 1;

    CLR(ntransmitted % MAX_DUP_CHK);

    icmp = (struct icmp6_hdr *) outpack;
    icmp->icmp6_type = ICMP6_ECHO_REQUEST;
    icmp->icmp6_code = 0;
    icmp->icmp6_cksum = 0;
    icmp->icmp6_seq = htons(ntransmitted++);
    icmp->icmp6_id = ident;       /* ID */

    h->sent++;
    h->tmp_sent++;

    set_packet_data(outpack + sizeof(*icmp), h);

    i = sendto(icmp6_socket, outpack, sizeof(outpack), 0,
               &h->addr.addr, sizeof(struct sockaddr_in6));

    if (i < 0 || i != sizeof(outpack)) {
        perror("pinger: sendto");
        write_result(h, "Error sending packet", "Err");
    }
}

static void pinger(host_data * h)
{
    if (h->addr.addr.sa_family == AF_INET)
        pinger4(h);
    else if (h->addr.addr.sa_family == AF_INET6)
        pinger6(h);
}

/*
 * pr_icmph --
 *      Print a descriptive string about an ICMP header.
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
 * pr_icmph --
 *      Print a descriptive string about an ICMPv6 header.
 */
static gchar *pr_icmph6(struct icmp6_hdr *icmp)
{
    GString *s = g_string_new(NULL);
    gchar *c;

    switch (icmp->icmp6_type) {
    case ICMP6_ECHO_REPLY:
        g_string_assign(s, "Echo Reply");
        /* XXX ID + Seq + Data */
        break;
    case ICMP6_DST_UNREACH:
        switch (icmp->icmp6_code) {
        case ICMP6_DST_UNREACH_NOROUTE:
            g_string_assign(s, "No route to destination");
            break;
        case ICMP6_DST_UNREACH_ADMIN:
            g_string_assign(s, "Communication with destination administratively prohibited");
            break;
        case ICMP6_DST_UNREACH_BEYONDSCOPE:
            g_string_assign(s, "Beyond scope of source address");
            break;
        case ICMP6_DST_UNREACH_ADDR:
            g_string_assign(s, "Address unreachable");
            break;
        case ICMP6_DST_UNREACH_NOPORT:
            g_string_assign(s, "Bad port");
            break;
        default:
            g_string_sprintf(s, "Dest Unreachable, Unknown Code: %d",
                             icmp->icmp6_code);
            break;
        }
        break;
    case ICMP6_ECHO_REQUEST:
        g_string_assign(s, "Echo Request");
        /* XXX ID + Seq + Data */
        break;
    case ICMP6_TIME_EXCEEDED:
        switch (icmp->icmp6_code) {
        case ICMP6_TIME_EXCEED_TRANSIT:
            g_string_assign(s, "Hop Limit == 0 in transit");
            break;
        case ICMP6_TIME_EXCEED_REASSEMBLY:
            g_string_assign(s, "Frag reassembly time exceeded");
            break;
        default:
            g_string_sprintf(s, "Time exceeded, Bad Code: %d",
                             icmp->icmp6_code);
            break;
        }
        break;
    case ICMP_PARAMETERPROB:
        switch (icmp->icmp6_code) {
        case ICMP6_PARAMPROB_HEADER:
            g_string_assign(s, "Erroneous header field");
            break;
        case ICMP6_PARAMPROB_NEXTHEADER:
            g_string_assign(s, "Unrecognized Next Header");
            break;
        case ICMP6_PARAMPROB_OPTION:
            g_string_assign(s, "Unrecognized IPv6 option");
            break;
        default:
            g_string_sprintf(s, "Parameter problem, Unknown Code: %d",
                             icmp->icmp6_code);
            break;
        }
        break;
    case ICMP6_PACKET_TOO_BIG:
        g_string_sprintf(s, "Packet too big, Bad Code: %d",
                         icmp->icmp6_code);
        break;
    default:
        g_string_sprintf(s, "Bad ICMP type: %d", icmp->icmp6_type);
    }

    c = s->str;
    g_string_free(s, FALSE);
    return c;
}

/*
 * tvsub --
 *      Subtract 2 timeval structs:  out = out - in.  Out is assumed to
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
void pr_pack(u_char *buf, int cc, struct sockaddr_in *from)
{
    struct icmp *icp;
    struct ip *ip;
    struct timeval tv, tvs;
    long triptime = 0;
    int hlen;
    host_data *h;

    (void) gettimeofday(&tv, (struct timezone *) NULL);

    if (cc < sizeof(outpack))
        return;

    /* Check the IP header */
    ip = (struct ip *) buf;
    hlen = ip->ip_hl << 2;

    /* Now the ICMP part */
    cc -= hlen;
    icp = (struct icmp *) (buf + hlen);

    if (icp->icmp_type == ICMP_ECHOREPLY) {
        if (icp->icmp_id != ident)
            return;             /* 'Twas not our ECHO */

        if (!get_packet_data(icp->icmp_data, &h, &tvs))
            return;

        ++h->recv;
        ++h->tmp_recv;

        tvsub(&tv, &tvs);
        triptime = tv.tv_sec * 1000000 + tv.tv_usec;
        h->tsum += triptime;
        h->tmp_tsum += triptime;

        host_check_dup(h, ntohs(icp->icmp_seq));
    } else {
        switch (icp->icmp_type) {
        case ICMP_ECHO:
            return;
        case ICMP_DEST_UNREACH:
        case ICMP_TIME_EXCEEDED:
        case ICMP_PARAMETERPROB:
            {
                struct ip *iph = (struct ip *) (&icp->icmp_data);
                struct icmp *icp1 =
                    (struct icmp *) ((u_char *) iph +
                                     iph->ip_hl * 4);

                if (icp1->icmp_type != ICMP_ECHO ||
                    iph->ip_src.s_addr != ip->ip_dst.s_addr ||
                    icp1->icmp_id != ident)
                {
                    return;
                }

                if (!get_packet_data(icp1->icmp_data, &h, &tvs))
                    return;

                h->icmp.v4 = *icp;
                h->error_flag = 1;
            }
        }
    }
}

// process a received packet
void pr_pack6(u_char *buf, int cc, struct sockaddr_in6 *from)
{
    struct icmp6_hdr *icmp;
    u_char *data;
    struct timeval tv, tvs;
    long triptime = 0;
    host_data *h;

    (void) gettimeofday(&tv, (struct timezone *) NULL);

    if (cc < sizeof(outpack))
        return;

    /* Now the ICMP part */
    icmp = (struct icmp6_hdr *) buf;
    data = (u_char *)(icmp + 1);

    if (icmp->icmp6_type == ICMP6_ECHO_REPLY) {
        if (icmp->icmp6_id != ident)
            return;             /* 'Twas not our ECHO */

        if (!get_packet_data(data, &h, &tvs))
            return;

        ++h->recv;
        ++h->tmp_recv;

        tvsub(&tv, &tvs);
        triptime = tv.tv_sec * 1000000 + tv.tv_usec;
        h->tsum += triptime;
        h->tmp_tsum += triptime;

        host_check_dup(h, ntohs(icmp->icmp6_seq));
    } else {
        switch (icmp->icmp6_type) {
        case ICMP6_ECHO_REPLY:
            return;
        case ICMP6_PACKET_TOO_BIG:
        case ICMP6_DST_UNREACH:
        case ICMP6_TIME_EXCEEDED:
        case ICMP6_PARAM_PROB:
            {
                struct ip6_hdr *orig_ip = (struct ip6_hdr *) (icmp + 1);
                struct icmp6_hdr *orig_icmp = (struct icmp6_hdr *) (orig_ip + 1);
                u_char *orig_data = (u_char *)(orig_icmp + 1);

                if (orig_icmp->icmp6_id != ident
                    || orig_ip->ip6_nxt != IPPROTO_ICMPV6
                    || orig_icmp->icmp6_type != ICMP6_ECHO_REQUEST)
                {
                    return;
                }

                if (!get_packet_data(orig_data, &h, &tvs))
                    return;

                if (!IN6_ARE_ADDR_EQUAL (&orig_ip->ip6_dst, &h->addr.in6.sin6_addr))
                    return;

                h->icmp.v6 = *icmp;
                h->error_flag = 1;

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
    printf("%s\n", h->percentage->str);
    printf("%s\n", h->sent_str->str);
    printf("%s\n", h->recv_str->str);
    printf("%s\n", h->msg->str);
    printf("%s\n", h->shortmsg->str);
}

int hostname_to_addr(const char *hostname, struct sockaddr *addr)
{
    struct addrinfo hints;
    struct addrinfo *result = 0, *rp = 0;
    int s, ret = -1;

    memset(&hints, 0, sizeof(struct addrinfo));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = 0;
    hints.ai_flags = 0;
    hints.ai_protocol = 0;          /* Any protocol */
    hints.ai_canonname = NULL;
    hints.ai_addr = NULL;
    hints.ai_next = NULL;

    s = getaddrinfo(hostname, NULL, &hints, &result);
    if (s != 0) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(s));
        return -1;
    }

    for (rp = result; rp != NULL; rp = rp->ai_next) {
        if (rp->ai_addr->sa_family == AF_INET
            || rp->ai_addr->sa_family == AF_INET6)
        {
            if (rp->ai_addr->sa_family == AF_INET) {
                assert(rp->ai_addrlen == sizeof(struct sockaddr_in));
            }
            if (rp->ai_addr->sa_family == AF_INET6) {
                assert(rp->ai_addrlen == sizeof(struct sockaddr_in6));
            }
            memcpy(addr, rp->ai_addr, rp->ai_addrlen);
            ret = 0;
            break;
        }
    }

    freeaddrinfo(result);

    return ret;
}

// recheck the dns (needed for dialup users or dynamic DNS)
int update_dns(host_data *h)
{
    return hostname_to_addr(h->hostname->str, &h->addr.addr);
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
        if (h->addr.addr.sa_family == AF_INET)
            msg = pr_icmph(&h->icmp.v4);
        else
            msg = pr_icmph6(&h->icmp.v6);
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
    socklen_t fromlen;
    struct timeval tv_old,tv_new;
    int avail;
    struct pollfd fds[2];

    fds[0].fd = icmp_socket;
    fds[1].fd = icmp6_socket;

    gettimeofday(&tv_old, NULL);
    for (;!terminated;) {
        fds[0].events = fds[1].events = POLLIN;
        fds[0].revents = fds[1].revents = 0;

        avail = poll(fds, 2, 100);

        if (avail > 0) {
            if (fds[0].revents & POLLIN) {
                struct sockaddr_in from;
                fromlen = sizeof(from);
                if ((cc = recvfrom(icmp_socket, packet, sizeof(packet), 0,
                                   (struct sockaddr *) &from, &fromlen)) < 0) {
                    if (errno != EAGAIN)
                        perror("pinger: recvfrom");
                } else {
                    pr_pack(packet, cc, &from);
                }
            }
            if (fds[1].revents & POLLIN) {
                struct sockaddr_in6 from;
                fromlen = sizeof(from);
                if ((cc = recvfrom(icmp6_socket, packet, sizeof(packet), 0,
                                   (struct sockaddr *) &from, &fromlen)) < 0) {
                    if (errno != EAGAIN)
                        perror("pinger: recvfrom");
                } else {
                    pr_pack6(packet, cc, &from);
                }
            }
        } else if (avail < 0) {
            perror("poll");
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


void append_host(struct sockaddr *addr, char * hostname, char * updatefreq, char * dynamic, int dummy)
{
    host_data *h = host_malloc();

    if (addr->sa_family == AF_INET)
        h->addr.in4 = *(struct sockaddr_in *)addr;
    else if (addr->sa_family == AF_INET6)
        h->addr.in6 = *(struct sockaddr_in6 *)addr;

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

void term_signal(int signum, siginfo_t *info, void *data)
{
    terminated = 1;
}

int main(int argc, char **argv)
{
    int i;
    struct sigaction sig;

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

    struct icmp6_filter filter;
    const int on = 1;
    int err;
    if ((icmp6_socket = socket(PF_INET6, SOCK_RAW, IPPROTO_ICMPV6)) < 0) {
        if (errno == EPERM) {
            fprintf(stderr, "pinger: must run as root\n");
        } else
            perror("pinger: socket");
        exit(2);
    }

    /* Tell which ICMPs we are interested in.  */
    ICMP6_FILTER_SETBLOCKALL (&filter);
    ICMP6_FILTER_SETPASS (ICMP6_ECHO_REPLY, &filter);
    ICMP6_FILTER_SETPASS (ICMP6_DST_UNREACH, &filter);
    ICMP6_FILTER_SETPASS (ICMP6_PACKET_TOO_BIG, &filter);
    ICMP6_FILTER_SETPASS (ICMP6_TIME_EXCEEDED, &filter);
    ICMP6_FILTER_SETPASS (ICMP6_PARAM_PROB, &filter);

    err = setsockopt (icmp6_socket, IPPROTO_ICMPV6, ICMP6_FILTER,
                      &filter, sizeof (filter));
    if (err)
    {
        close (icmp6_socket);
        exit(2);
    }

    err = setsockopt (icmp6_socket, IPPROTO_IPV6, IPV6_RECVHOPLIMIT,
                      &on, sizeof (on));
    if (err)
    {
        close (icmp6_socket);
        exit(2);
    }

    setuid(getuid());

    fcntl(icmp_socket, F_SETFL, O_NONBLOCK);
    fcntl(icmp6_socket, F_SETFL, O_NONBLOCK);
    ident = getpid() & 0xFFFF;

    for (i = 1; i < argc - 2; i += 3) {
        union {
            struct sockaddr addr;
            struct sockaddr_in in4;
            struct sockaddr_in6 in6;
        } addr;
        int res;

        res = hostname_to_addr(argv[i], &addr.addr);
        if (res == 0) {
            append_host(&addr.addr, argv[i], argv[i+1], argv[i+2], 0);
        } else if (i <= argc-3) {
            memset(&addr, 0, sizeof(addr));
            addr.addr.sa_family = AF_INET;
            append_host(&addr.addr, argv[i], argv[i+1], argv[i+2], 1); // dummy host
        }
    }

    sigfillset(&sig.sa_mask);
    sig.sa_flags = SA_SIGINFO | SA_RESTART;
    sig.sa_sigaction = term_signal;

    sigaction(SIGINT, &sig, 0);
    sigaction(SIGTERM, &sig, 0);
    sigaction(SIGPIPE, &sig, 0);
    sigaction(SIGHUP, &sig, 0);

    receiver();

    free_hosts();
    close(icmp_socket);
    close(icmp6_socket);
    return 0;
}
