/*
 * $Id: main.c $
 *
 * Author: Markus Stenberg <markus stenberg@iki.fi>
 *
 * Copyright (c) 2014 cisco Systems, Inc.
 *
 * Created:       Mon May  5 16:53:27 2014 mstenber
 * Last modified: Thu May 15 10:24:16 2014 mstenber
 * Edit time:     120 min
 *
 */


#ifdef __APPLE__

/* Haha. Got to love advanced IPv6 socket API being disabled by
 * default. */
#define __APPLE_USE_RFC_3542

/* ARGH. It seems that udp46 socket on Apple CANNOT get destination
 * address for v4 packets, which kind of sucks. TBD: Figure how to do
 * this without second socket. */
#undef USE_IP_REVCDSTADDR
#endif /* __APPLE__ */

#ifdef __linux__
#define USE_IP_PKTINFO
#endif /* __linux__ */

#include <stdio.h>
#include <libubox/uloop.h>
#include <libubox/usock.h>
#include <unistd.h>
#include <sys/socket.h>
#include <string.h>
#include <arpa/inet.h>
#include <stdlib.h>

#include "shared.h"
#include "pcp.h"
#include "pcpproxy.h"

int client_socket, server_socket;


int init_listening_socket(int port)
{
  int on = 1, off = 0;
  int s = socket(PF_INET6, SOCK_DGRAM, 0);
  struct sockaddr_in6 sin6;

  if (s < 0)
    perror("socket");
#ifdef USE_IP_PKTINFO
  else if (setsockopt(s, IPPROTO_IP, IP_PKTINFO, &on, sizeof(on)) < 0)
    perror("setsockopt IP_PKTINFO");
#endif /* USE_IP_PKTINFO */
#ifdef USE_IP_REVCDSTADDR
  else if (setsockopt(s, IPPROTO_IP, IP_RECVDSTADDR, &on, sizeof(on)) < 0)
    perror("setsockopt IP_RECVDSTADDR");
#endif /* USE_IP_REVCDSTADDR */
  else if (setsockopt(s, IPPROTO_IPV6, IPV6_RECVPKTINFO, &on, sizeof(on)) < 0)
    perror("setsockopt IPV6_RECVPKTINFO");
  else if (setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof(off)) < 0)
    perror("setsockopt IPV6_V6ONLY");
  else if (setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) < 0)
      perror("setsockopt SO_REUSEADDR");
  else
    {
      memset(&sin6, 0, sizeof(sin6));
      sin6.sin6_family = AF_INET6;
      sin6.sin6_port = htons(port);
      if (bind(s, (struct sockaddr *)&sin6, sizeof(sin6)) < 0)
        perror("bind");
      else
        return s;
    }
  abort();
  return -1; /* Not reached */
}

#define IN_ADDR_TO_MAPPED_IN6_ADDR(a, a6)       \
do {                                            \
  memset(a6, 0, sizeof(*(a6)));                 \
  (a6)->s6_addr[10] = 0xff;                     \
  (a6)->s6_addr[11] = 0xff;                     \
  ((uint32_t *)a6)[3] = *((uint32_t *)a);       \
 } while(0)

struct uloop_fd client_ufd;
struct uloop_fd server_ufd;

void fd_callback(struct uloop_fd *u, unsigned int events)
{
  if (!events & ULOOP_READ)
    return;
  struct sockaddr_in6 srcsa;
  uint8_t data[PCP_PAYLOAD_LENGTH+1];
  struct iovec iov[1] = {
    {.iov_base = data,
     .iov_len = sizeof(data) },
  };
  uint8_t c[1000];
  struct msghdr msg = {
    .msg_iov = iov,
    .msg_iovlen = sizeof(iov) / sizeof(*iov),
    .msg_name = &srcsa,
    .msg_namelen = sizeof(srcsa),
    .msg_flags = 0,
    .msg_control = c,
    .msg_controllen = sizeof(c)
  };
  ssize_t l;
  if ((l = recvmsg(u->fd, &msg, 0)) < 0)
    {
      perror("recvmsg");
      return;
    }
  if (srcsa.sin6_family != AF_INET6)
    {
      DEBUG("got non-AF_INET6 packet");
      return;
    }
  struct cmsghdr *h;
  struct in6_addr *dst = NULL;
#ifdef USE_IP_REVCDSTADDR
  struct in6_addr dst_buf;
#endif /* USE_IP_REVCDSTADDR */

  for (h = CMSG_FIRSTHDR(&msg); h;
       h = CMSG_NXTHDR(&msg, h))
    if (h->cmsg_level == IPPROTO_IPV6
        && h->cmsg_type == IPV6_PKTINFO)
      {
        struct in6_pktinfo *ipi6 = (struct in6_pktinfo *)CMSG_DATA(h);
        dst = &ipi6->ipi6_addr;
      }
#ifdef USE_IP_REVCDSTADDR
    else if (h->cmsg_level == IPPROTO_IP
             && h->cmsg_type == IP_RECVDSTADDR)
      {
        struct in_addr *a = (struct in_addr *)CMSG_DATA(h);
        IN_ADDR_TO_MAPPED_IN6_ADDR(a, &dst_buf);
        dst = &dst_buf;
      }
#endif /* USE_IP_REVCDSTADDR */
  if (!dst)
    {
      /* By default, nothing happens if the option is AWOL. */
      DEBUG("unknown destination");
      return;
    }
  if (u == &client_ufd)
    {
      DEBUG("calling proxy_handle_from_server");
      proxy_handle_from_server(&srcsa.sin6_addr, dst, data, l);
    }
  else if (u == &server_ufd)
    {
      DEBUG("calling proxy_handle_from_client");
      proxy_handle_from_client(&srcsa.sin6_addr, dst, data, l);
    }
  else
    {
      DEBUG("invalid callback?!?");
    }

}

void init_sockets()
{
  client_socket = init_listening_socket(PCP_CLIENT_PORT);
  memset(&client_ufd, 0, sizeof(client_ufd));
  client_ufd.cb = fd_callback;
  client_ufd.fd = client_socket;
  if (uloop_fd_add(&client_ufd, ULOOP_READ) < 0)
    {
      perror("uloop_fd_add(client)");
      abort();
    }

  server_socket = init_listening_socket(PCP_SERVER_PORT);
  memset(&server_ufd, 0, sizeof(server_ufd));
  server_ufd.cb = fd_callback;
  server_ufd.fd = server_socket;
  if (uloop_fd_add(&server_ufd, ULOOP_READ) < 0)
    {
      perror("uloop_fd_add(client)");
      abort();
    }

}

static void
socket_send_from_to(int socket,
                    struct in6_addr *src,
                    struct sockaddr_in6 *dst,
                    void *data, int data_len,
                    void *data2, int data_len2)
{
  struct iovec iov[2] = {
    {.iov_base = data,
     .iov_len = data_len },
    {.iov_base = data2,
     .iov_len = data_len2 }
  };
  struct in6_pktinfo ipi6 = { .ipi6_addr = *src,
                              .ipi6_ifindex = 0 };
  uint8_t c[CMSG_SPACE(sizeof(ipi6))];
  struct msghdr msg = {
    .msg_iov = iov,
    .msg_iovlen = sizeof(iov) / sizeof(*iov),
    .msg_name = dst,
    .msg_namelen = sizeof(*dst),
    .msg_flags = 0,
    .msg_control = c,
    .msg_controllen = sizeof(c)
  };
  struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
  cmsg->cmsg_level = IPPROTO_IPV6;
  cmsg->cmsg_type = IPV6_PKTINFO;
  cmsg->cmsg_len = CMSG_LEN(sizeof(ipi6));
  *((struct in6_pktinfo *)CMSG_DATA(cmsg)) = ipi6;
  if (sendmsg(socket, &msg, 0) < 0)
    perror("sendmsg");
}

void proxy_send_to_client(struct in6_addr *src,
                          struct in6_addr *dst,
                          void *data, int data_len)
{
  struct sockaddr_in6 dstsa = { .sin6_family = AF_INET6,
                                .sin6_port = htons(PCP_CLIENT_PORT),
                                .sin6_addr = *dst };
  DEBUG("proxy_send_to_client");
  socket_send_from_to(client_socket, src, &dstsa, data, data_len, NULL, 0);
}

void proxy_send_to_server(struct in6_addr *src,
                          struct in6_addr *dst,
                          void *data, int data_len,
                          void *data2, int data_len2)
{
  struct sockaddr_in6 dstsa = { .sin6_family = AF_INET6,
                                .sin6_port = htons(PCP_SERVER_PORT),
                                .sin6_addr = *dst };
  DEBUG("proxy_send_to_server");
  socket_send_from_to(server_socket, src, &dstsa,
                      data, data_len,
                      data2, data_len2);
}

static void do_help(const char *p, const char *reason)
{
  if (reason)
    printf("%s.\n\n", reason);
  printf("Usage: %s S [S [S ..]]\n", p);
  printf(" Where S = <source prefix>/<source prefix length>=<server>\n");
  exit(1);
}

int main(int argc, char **argv)
{
  int c;

  if (uloop_init() < 0)
    {
      perror("uloop_init");
      abort();
    }
  proxy_init();
  init_sockets();
  /* XXX - add support for parsing interfaces too and use them for
   * announces on reset_epoch */
  while ((c = getopt(argc, argv, "h"))>0)
    {
      switch (c)
        {
        case 'h':
          do_help(argv[0], NULL);
        }
    }
  /* Parse command leftover command line arguments. Assume they're of
   * type <source prefix>/<source prefix length>=server, all in IPv6
   * format. */
  int i;
  if (optind == argc)
    do_help(argv[0], "One server is required");
  for (i = optind ; i < argc ; i++)
    {
      char *prefix = argv[i];
      char *c = strchr(prefix, '=');
      char *d = strchr(prefix, '/');
      if (!c || !d || d > c || c <= prefix || d <= prefix)
        do_help(argv[0], "Invalid server format (no X/Y=Z)");
      *d = 0;
      d++;
      *c = 0;
      c++;
      struct in6_addr p, s;
      DEBUG("converting to IPv6: %s / %s", prefix, c);
      if (inet_pton(AF_INET6, prefix, &p) < 1
          || inet_pton(AF_INET6, c, &s) < 1)
        do_help(argv[0], "Unable to parse the addresses");
      int plen;
      if (!(plen = atoi(d)) || plen < 1 || plen > 128)
        do_help(argv[0], "Invalid prefix length");
      proxy_add_server(&p, plen, &s);
    }
  uloop_run();
  uloop_done();
  return 0;
}
