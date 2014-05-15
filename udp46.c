/*
 * $Id: udp46.c $
 *
 * Author: Markus Stenberg <markus stenberg@iki.fi>
 *
 * Copyright (c) 2014 cisco Systems, Inc.
 *
 * Created:       Thu May 15 12:33:19 2014 mstenber
 * Last modified: Thu May 15 19:35:22 2014 mstenber
 * Edit time:     86 min
 *
 */

#include "udp46.h"
#include "shared.h"

#include <errno.h>
#include <unistd.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <libubox/uloop.h>
#include <libubox/usock.h>
#include <unistd.h>
#include <sys/socket.h>
#include <string.h>
#include <arpa/inet.h>
#include <stdlib.h>
#include <assert.h>
#include <fcntl.h>

struct udp46_t {
  int s4;
  int s6;
  uint16_t port;
};

#define IN_ADDR_TO_MAPPED_IN6_ADDR(a, a6)       \
do {                                            \
  memset(a6, 0, sizeof(*(a6)));                 \
  (a6)->s6_addr[10] = 0xff;                     \
  (a6)->s6_addr[11] = 0xff;                     \
  ((uint32_t *)a6)[3] = *((uint32_t *)a);       \
 } while (0)

#define MAPPED_IN6_ADDR_TO_IN_ADDR(a6, a)       \
do {                                            \
  *((uint32_t *)a) = ((uint32_t *)a6)[3];       \
 } while (0)

static int init_listening_socket(int pf, uint16_t port)
{
  int on = 1;
  int s = socket(pf, SOCK_DGRAM, 0);
  struct sockaddr_storage ss;
  int ss_len;

  if (s < 0)
    perror("socket");
  else if (fcntl(s, F_SETFL, O_NONBLOCK) < 0)
    perror("fnctl O_NONBLOCK");
#ifdef IP_PKTINFO
  else if (pf == PF_INET
           && setsockopt(s, IPPROTO_IP, IP_PKTINFO, &on, sizeof(on)) < 0)
    perror("setsockopt IP_PKTINFO");
#endif /* IP_PKTINFO */
#ifdef IP_REVCDSTADDR
  else if (pf == PF_INET
           && setsockopt(s, IPPROTO_IP, IP_RECVDSTADDR, &on, sizeof(on)) < 0)
    perror("setsockopt IP_RECVDSTADDR");
#endif /* IP_REVCDSTADDR */
  else if (pf == PF_INET6
           && setsockopt(s, IPPROTO_IPV6, IPV6_RECVPKTINFO, &on, sizeof(on)) < 0)
    perror("setsockopt IPV6_RECVPKTINFO");
  else if (pf == PF_INET6
           && setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, &on, sizeof(on)) < 0)
    perror("setsockopt IPV6_V6ONLY");
  else if (setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) < 0)
    perror("setsockopt SO_REUSEADDR");
  else
    {
      if (pf == PF_INET6)
        {
          struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)&ss;
          memset(sin6, 0, sizeof(*sin6));
          sin6->sin6_family = AF_INET6;
          sin6->sin6_port = htons(port);
          ss_len = sizeof(*sin6);
        }
      else
        {
          struct sockaddr_in *sin = (struct sockaddr_in *)&ss;
          memset(sin, 0, sizeof(*sin));
          sin->sin_family = AF_INET;
          sin->sin_port = htons(port);
          ss_len = sizeof(*sin);
        }

      if (bind(s, (struct sockaddr *)&ss, ss_len) >= 0)
        return s;
      /* Don't return errors on bind, due to it being spammy when probing */
    }
  return - 1;
}

udp46 udp46_create(uint16_t port)
{
  udp46 s;
  int fd1 = -1, fd2 = -1;

  s = calloc(1, sizeof(*s));
  if (!s)
    return NULL;
  if (port)
    {
      fd1 = init_listening_socket(PF_INET, port);
      fd2 = init_listening_socket(PF_INET6, port);
    }
  else
    {
      /*
       * XXX - correct way to do this would be to allocate one, try
       * getting similar, and then start incrementing from there. This
       * for loop is simpler and stupider, though..
       */
      for (port = 1024; port; port++)
        {
          if (fd1 >= 0)
            close(fd1);
          fd1 = init_listening_socket(PF_INET, port);
          if (fd1 >= 0)
            {
              fd2 = init_listening_socket(PF_INET6, port);
              if (fd2 >= 0)
                break;
            }
        }
    }
  if (fd1 >= 0 && fd2 >= 0)
    {
      s->s4 = fd1;
      s->s6 = fd2;
      s->port = port;
      DEBUG("udp46_create succeeded at port %d", port);
      return s;
    }
  if (fd1 >= 0)
    close(fd1);
  if (fd2 >= 0)
    close(fd2);
  free(s);
  return NULL;
}

void udp46_get_fds(udp46 s, int *fd1, int *fd2)
{
  *fd1 = s->s4;
  *fd2 = s->s6;
}

ssize_t udp46_recv(udp46 s,
                   struct sockaddr_in6 *src,
                   struct sockaddr_in6 *dst,
                   void *buf, size_t buf_size)
{
  struct iovec iov[1] = {
    {.iov_base = buf,
     .iov_len = buf_size },
  };
  uint8_t c[1000];
  struct msghdr msg = {
    .msg_iov = iov,
    .msg_iovlen = sizeof(iov) / sizeof(*iov),
    .msg_name = src,
    .msg_namelen = src ? sizeof(*src) : 0,
    .msg_flags = 0,
    .msg_control = c,
    .msg_controllen = sizeof(c)
  };
  ssize_t l;

  /* If we can't find a packet on IPv4 or IPv6 socket, return -1. */
  if ((l = recvmsg(s->s6, &msg, 0)) < 0)
    if ((l = recvmsg(s->s4, &msg, 0)) < 0)
      return -1;

  /* Convert source address to IPv6 if it already isn't */
  if (src && src->sin6_family != AF_INET6)
    {
      struct sockaddr_in *sa = (struct sockaddr_in *)src;
      struct in_addr a = sa->sin_addr;
      uint16_t port = sa->sin_port;

      memset(src, 0, sizeof(*src));
      IN_ADDR_TO_MAPPED_IN6_ADDR(&a, &src->sin6_addr);
      src->sin6_port = port;
      src->sin6_family = AF_INET6;
    }

  /* If we don't care about destination address, we're already done */
  if (!dst)
    return l;

  memset(dst, 0, sizeof(*dst));
  dst->sin6_port = htons(s->port);

  struct cmsghdr *h;
  /* Iterate through the message headers looking for destination
   * address, and if finding it, return it (in dst, as V4 mapped if
   * need be). */
  for (h = CMSG_FIRSTHDR(&msg); h;
       h = CMSG_NXTHDR(&msg, h))
    if (h->cmsg_level == IPPROTO_IPV6
        && h->cmsg_type == IPV6_PKTINFO)
      {
        struct in6_pktinfo *ipi6 = (struct in6_pktinfo *)CMSG_DATA(h);
        dst->sin6_family = AF_INET6;
        dst->sin6_addr = ipi6->ipi6_addr;
        dst->sin6_scope_id = ipi6->ipi6_ifindex;
        return l;
      }
#ifdef IP_REVCDSTADDR
    else if (h->cmsg_level == IPPROTO_IP
             && h->cmsg_type == IP_RECVDSTADDR)
      {
        struct in_addr *a = (struct in_addr *)CMSG_DATA(h);
        IN_ADDR_TO_MAPPED_IN6_ADDR(a, &dst->sin6_addr);
        dst->sin6_family = AF_INET6;
        return l;
      }
#endif /* IP_REVCDSTADDR */
#ifdef IP_PKTINFO
    else if (h->cmsg_level == IPPROTO_IP
             && h->cmsg_type == IP_PKTINFO)
      {
        struct in_pktinfo *ipi = (struct in_pktinfo *) CMSG_DATA(h);
        IN_ADDR_TO_MAPPED_IN6_ADDR(&ipi->ipi_addr, &dst->sin6_addr);
        dst->sin6_family = AF_INET6;
        return l;
      }
#endif /* IP_PKTINFO */
  /* By default, nothing happens if the option is AWOL. */
  DEBUG("unknown destination");
  return -1;
}

int udp46_send_iovec(udp46 s,
                     const struct sockaddr_in6 *src,
                     const struct sockaddr_in6 *dst,
                     struct iovec *iov, int iov_len)
{
  if (src && src->sin6_family != AF_INET6)
    {
      DEBUG("src wrong: %s", SOCKADDR_IN6_REPR(src));
      return -1;
    }
  if (!dst || dst->sin6_family != AF_INET6)
    {
      DEBUG("dst wrong: %s", SOCKADDR_IN6_REPR(dst));
      return -1;
    }
  if (src && !IN6_IS_ADDR_V4MAPPED(&src->sin6_addr)
      != !IN6_IS_ADDR_V4MAPPED(&dst->sin6_addr))
    {
      DEBUG("IPv4 <> IPv6 traffic not allowed");
      return -1;
    }
  uint8_t c[1000];
  struct msghdr msg = {
    .msg_iov = iov,
    .msg_iovlen = iov_len,
    .msg_flags = 0,
    .msg_control = c,
    .msg_controllen = sizeof(c)
  };
  struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
  struct sockaddr_in sin;
  int sock = -1;

  if (IN6_IS_ADDR_V4MAPPED(&dst->sin6_addr))
    {
      /* Convert the destination address */
      memset(&sin, 0, sizeof(sin));
      MAPPED_IN6_ADDR_TO_IN_ADDR(dst, &sin.sin_addr);
      sin.sin_family = AF_INET;
      sin.sin_port = dst->sin6_port;
      sock = s->s4;
      msg.msg_name = (void *)&sin;
      msg.msg_namelen = sizeof(sin);
    }
  else
    {
      /* Use destination address as-is */
      msg.msg_name = (void *)dst;
      msg.msg_namelen = sizeof(*dst);
      sock = s->s6;
    }
  /* Deal with source address */
  cmsg->cmsg_len = 0;
  if (src)
    {
      if (IN6_IS_ADDR_V4MAPPED(&dst->sin6_addr))
        {
          cmsg->cmsg_level = IPPROTO_IP;
#ifdef IP_PKTINFO
          struct in_pktinfo *ipi = (struct in_pktinfo *)CMSG_DATA(cmsg);
          memset(ipi, 0, sizeof(*ipi));
          MAPPED_IN6_ADDR_TO_IN_ADDR(src, &ipi->ipi_spec_dst);
          cmsg->cmsg_type = IP_PKTINFO;
          cmsg->cmsg_len = CMSG_LEN(sizeof(*ipi));
#else
#ifdef IP_SENDSRCADDR
          struct in_addr *in = (struct in_addr *)CMSG_DATA(cmsg);
          MAPPED_IN6_ADDR_TO_IN_ADDR(src, in);
          cmsg->cmsg_type = IP_SENDSRCADDR;
          cmsg->cmsg_len = CMSG_LEN(sizeof(*in));
#else
          cmsg = NULL; /* uh oh. definitely in best effort category now.. */
#endif /* IP_SENDSRCADDR */
#endif /* IP_PKTINFO */
        }
      else
        {
          /* Add source address header */
          struct in6_pktinfo *ipi6 = (struct in6_pktinfo *)CMSG_DATA(cmsg);
          memset(ipi6, 0, sizeof(*ipi6));
          ipi6->ipi6_addr = src->sin6_addr;
          ipi6->ipi6_ifindex = src->sin6_scope_id;
          cmsg->cmsg_level = IPPROTO_IPV6;
          cmsg->cmsg_type = IPV6_PKTINFO;
          cmsg->cmsg_len = CMSG_LEN(sizeof(*ipi6));
        }
    }
  msg.msg_controllen = cmsg->cmsg_len;
  return sendmsg(sock, &msg, 0);
}


void udp46_destroy(udp46 s)
{
  close(s->s4);
  close(s->s6);
  free(s);
}
