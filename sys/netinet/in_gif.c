/*
 * Copyright (C) 1995, 1996, 1997, and 1998 WIDE Project.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the project nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE PROJECT AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE PROJECT OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * $FreeBSD$
 */

/*
 * in_gif.c
 */

#include "opt_mrouting.h"
#include "opt_inet6.h"

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/socket.h>
#include <sys/sockio.h>
#include <sys/mbuf.h>
#include <sys/errno.h>
#include <sys/kernel.h>
#include <sys/sysctl.h>
#include <sys/protosw.h>

#include <net/if.h>
#include <net/route.h>

#include <netinet/in.h>
#include <netinet/in_systm.h>
#include <netinet/ip.h>
#include <netinet/ip_var.h>
#include <netinet/in_gif.h>

#ifdef INET6
#include <netinet/ip6.h>
#endif

#ifdef MROUTING
#include <netinet/ip_mroute.h>
#endif /* MROUTING */

#include <net/if_gif.h>

#include "gif.h"

#include <machine/stdarg.h>

#include <net/net_osdep.h>

#if NGIF > 0
int ip_gif_ttl = GIF_TTL;
#else
int ip_gif_ttl = 0;
#endif

SYSCTL_DECL(_net_inet_ip);
SYSCTL_INT(_net_inet_ip, IPCTL_GIF_TTL, gifttl, CTLFLAG_RW,
	&ip_gif_ttl,	0, "");

int
in_gif_output(ifp, family, m, rt)
	struct ifnet	*ifp;
	int		family;
	struct mbuf	*m;
	struct rtentry *rt;
{
	register struct gif_softc *sc = (struct gif_softc*)ifp;
	struct sockaddr_in *dst = (struct sockaddr_in *)&sc->gif_ro.ro_dst;
	struct sockaddr_in *sin_src = (struct sockaddr_in *)sc->gif_psrc;
	struct sockaddr_in *sin_dst = (struct sockaddr_in *)sc->gif_pdst;
	struct ip iphdr;	/* capsule IP header, host byte ordered */
	int proto, error;
	u_int8_t tos;

	if (sin_src == NULL || sin_dst == NULL ||
	    sin_src->sin_family != AF_INET ||
	    sin_dst->sin_family != AF_INET) {
		m_freem(m);
		return EAFNOSUPPORT;
	}

	switch (family) {
	case AF_INET:
	    {
		struct ip *ip;

		proto = IPPROTO_IPV4;
		if (m->m_len < sizeof(*ip)) {
			m = m_pullup(m, sizeof(*ip));
			if (!m)
				return ENOBUFS;
		}
		ip = mtod(m, struct ip *);
		tos = ip->ip_tos;
		break;
	    }
#ifdef INET6
	case AF_INET6:
	    {
		struct ip6_hdr *ip6;
		proto = IPPROTO_IPV6;
		if (m->m_len < sizeof(*ip6)) {
			m = m_pullup(m, sizeof(*ip6));
			if (!m)
				return ENOBUFS;
		}
		ip6 = mtod(m, struct ip6_hdr *);
		tos = (ntohl(ip6->ip6_flow) >> 20) & 0xff;
		break;
	    }
#endif /*INET6*/
	default:
#ifdef DIAGNOSTIC
		printf("in_gif_output: warning: unknown family %d passed\n",
			family);
#endif
		m_freem(m);
		return EAFNOSUPPORT;
	}

	bzero(&iphdr, sizeof(iphdr));
	iphdr.ip_src = sin_src->sin_addr;
	if (ifp->if_flags & IFF_LINK0) {
		/* multi-destination mode */
		if (sin_dst->sin_addr.s_addr != INADDR_ANY)
			iphdr.ip_dst = sin_dst->sin_addr;
		else if (rt) {
			iphdr.ip_dst = ((struct sockaddr_in *)
					(rt->rt_gateway))->sin_addr;
		} else {
			m_freem(m);
			return ENETUNREACH;
		}
	} else {
		/* bidirectional configured tunnel mode */
		if (sin_dst->sin_addr.s_addr != INADDR_ANY)
			iphdr.ip_dst = sin_dst->sin_addr;
		else {
			m_freem(m);
			return ENETUNREACH;
		}
	}
	iphdr.ip_p = proto;
	/* version will be set in ip_output() */
	iphdr.ip_ttl = ip_gif_ttl;
	iphdr.ip_len = m->m_pkthdr.len + sizeof(struct ip);

	/* prepend new IP header */
	M_PREPEND(m, sizeof(struct ip), M_DONTWAIT);
	if (m && m->m_len < sizeof(struct ip))
		m = m_pullup(m, sizeof(struct ip));
	if (m == NULL) {
		printf("ENOBUFS in in_gif_output %d\n", __LINE__);
		return ENOBUFS;
	}

	*(mtod(m, struct ip *)) = iphdr;

	if (dst->sin_family != sin_dst->sin_family ||
	    dst->sin_addr.s_addr != sin_dst->sin_addr.s_addr) {
		/* cache route doesn't match */
		dst->sin_family = sin_dst->sin_family;
		dst->sin_len = sizeof(struct sockaddr_in);
		dst->sin_addr = sin_dst->sin_addr;
		if (sc->gif_ro.ro_rt) {
			RTFREE(sc->gif_ro.ro_rt);
			sc->gif_ro.ro_rt = NULL;
		}
	}

	if (sc->gif_ro.ro_rt == NULL) {
		rtalloc(&sc->gif_ro);
		if (sc->gif_ro.ro_rt == NULL) {
			m_freem(m);
			return ENETUNREACH;
		}
	}

#ifdef IPSEC
	m->m_pkthdr.rcvif = NULL;
#endif /*IPSEC*/
	error = ip_output(m, 0, &sc->gif_ro, 0, 0);
	return(error);
}

void
#if __STDC__
in_gif_input(struct mbuf *m, ...)
#else
in_gif_input(m, va_alist)
	struct mbuf *m;
	va_dcl
#endif
{
	int off, proto;
	struct gif_softc *sc;
	struct ifnet *gifp = NULL;
	struct ip *ip;
	int i, af;
	va_list ap;

	va_start(ap, m);
	off = va_arg(ap, int);
	proto = va_arg(ap, int);
	va_end(ap);

	ip = mtod(m, struct ip *);

	/* this code will be soon improved. */
#define	satosin(sa)	((struct sockaddr_in *)(sa))
	for (i = 0, sc = gif; i < ngif; i++, sc++) {
		if (sc->gif_psrc == NULL
		 || sc->gif_pdst == NULL
		 || sc->gif_psrc->sa_family != AF_INET
		 || sc->gif_pdst->sa_family != AF_INET) {
			continue;
		}

		if ((sc->gif_if.if_flags & IFF_UP) == 0)
			continue;

		if ((sc->gif_if.if_flags & IFF_LINK0)
		 && satosin(sc->gif_psrc)->sin_addr.s_addr == ip->ip_dst.s_addr
		 && satosin(sc->gif_pdst)->sin_addr.s_addr == INADDR_ANY) {
			gifp = &sc->gif_if;
			continue;
		}

		if (satosin(sc->gif_psrc)->sin_addr.s_addr == ip->ip_dst.s_addr
		 && satosin(sc->gif_pdst)->sin_addr.s_addr == ip->ip_src.s_addr)
		{
			gifp = &sc->gif_if;
			break;
		}
	}

	if (gifp == NULL) {
#ifdef MROUTING
		/* for backward compatibility */
		if (proto == IPPROTO_IPV4) {
			ipip_input(m, off, proto);
			return;
		}
#endif /*MROUTING*/
		m_freem(m);
		ipstat.ips_nogif++;
		return;
	}

	m_adj(m, off);

	switch (proto) {
	case IPPROTO_IPV4:
	    {
		struct ip *ip;
		af = AF_INET;
		if (m->m_len < sizeof(*ip)) {
			m = m_pullup(m, sizeof(*ip));
			if (!m)
				return;
		}
		ip = mtod(m, struct ip *);
		break;
	    }
#ifdef INET6
	case IPPROTO_IPV6:
	    {
		struct ip6_hdr *ip6;
		af = AF_INET6;
		if (m->m_len < sizeof(*ip6)) {
			m = m_pullup(m, sizeof(*ip6));
			if (!m)
				return;
		}
		ip6 = mtod(m, struct ip6_hdr *);
		ip6->ip6_flow &= ~htonl(0xff << 20);
		break;
	    }
#endif /* INET6 */
	default:
		ipstat.ips_nogif++;
		m_freem(m);
		return;
	}
	gif_input(m, af, gifp);
	return;
}
