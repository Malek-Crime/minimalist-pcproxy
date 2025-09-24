/*
 * $Id: main.c $
 *
 * Author: Markus Stenberg <markus stenberg@iki.fi>
 *
 * Copyright (c) 2014 cisco Systems, Inc.
 *
 * Created:       Mon May  5 16:53:27 2014 mstenber
 * Last modified: Mon Jun  2 19:15:43 2014 mstenber
 * Edit time:     165 min
 *
 */

#include <stdio.h>
#include <libubox/uloop.h>
#include <libubox/usock.h>
#include <unistd.h>
#include <sys/socket.h>
#include <string.h>
#include <arpa/inet.h>
#include <stdlib.h>
#include <assert.h>
#include <signal.h>
#include <errno.h>

#include "shared.h"
#include "pcp.h"
#include "pcpproxy.h"
#include "udp46.h"

int client_socket, server_socket;

struct uloop_fd ufds[4];
udp46 clients, servers;
static int running = 1;

void signal_handler(int sig)
{
  if (sig == SIGINT || sig == SIGTERM) {
    DEBUG("Received signal %d, shutting down", sig);
    running = 0;
    uloop_end();
  }
}

void fd_callback(struct uloop_fd *u, unsigned int events)
{
  if (!(events & ULOOP_READ))
    return;
    
  struct sockaddr_in6 srcsa, dstsa;
  uint8_t data[PCP_PAYLOAD_LENGTH + 1];

  int i;
  for (i = 0; i < 4; i++) {
    if (u == &ufds[i]) {
      DEBUG("fd callback for socket #%d", i);
      if (i < 2) {
        /* Client sockets (ufds[0] and ufds[1]) */
        ssize_t l = udp46_recv(clients, &srcsa, &dstsa, data, sizeof(data));
        if (l < 0) {
          if (errno != EAGAIN && errno != EWOULDBLOCK)
            perror("recvmsg");
        } else {
          DEBUG("calling pcp_proxy_handle_from_server");
          pcp_proxy_handle_from_server(&srcsa, &dstsa, data, l);
        }
      } else {
        /* Server sockets (ufds[2] and ufds[3]) */
        ssize_t l = udp46_recv(servers, &srcsa, &dstsa, data, sizeof(data));
        if (l < 0) {
          if (errno != EAGAIN && errno != EWOULDBLOCK)
            perror("recvmsg");
        } else {
          DEBUG("calling pcp_proxy_handle_from_client");
          pcp_proxy_handle_from_client(&srcsa, &dstsa, data, l);
        }
      }
      return;
    }
  }
  DEBUG("invalid callback?!?");
}

int init_sockets(int server_port)
{
  /* Any port should work for client connections */
  clients = udp46_create(0);
  if (!clients) {
    fprintf(stderr, "Failed to create client sockets\n");
    return -1;
  }

  /* We insist on server port, though. */
  servers = udp46_create(server_port);
  if (!servers) {
    fprintf(stderr, "Failed to create server sockets on port %d\n", server_port);
    udp46_destroy(clients);
    return -1;
  }

  int i;
  for (i = 0; i < 4; i++) {
    memset(&ufds[i], 0, sizeof(ufds[i]));
    ufds[i].cb = fd_callback;
  }
  
  udp46_get_fds(clients, &ufds[0].fd, &ufds[1].fd);
  udp46_get_fds(servers, &ufds[2].fd, &ufds[3].fd);
  
  for (i = 0; i < 4; i++) {
    if (ufds[i].fd >= 0) {
      if (uloop_fd_add(&ufds[i], ULOOP_READ) < 0) {
        perror("uloop_fd_add");
        return -1;
      }
    }
  }
  return 0;
}

void cleanup_sockets(void)
{
  int i;
  for (i = 0; i < 4; i++) {
    if (ufds[i].fd >= 0) {
      uloop_fd_delete(&ufds[i]);
    }
  }
  
  if (clients) {
    udp46_destroy(clients);
    clients = NULL;
  }
  
  if (servers) {
    udp46_destroy(servers);
    servers = NULL;
  }
}

void pcp_proxy_send_to_client(struct sockaddr_in6 *src,
                              struct sockaddr_in6 *dst,
                              void *data, int data_len)
{
  DEBUG("pcp_proxy_send_to_client %s->%s %d bytes",
        SOCKADDR_IN6_REPR(src), SOCKADDR_IN6_REPR(dst),
        data_len);
        
  if (!servers) {
    DEBUG("servers socket not initialized");
    return;
  }
  
  struct iovec iov = {.iov_base = data,
                      .iov_len = data_len };
  if (udp46_send_iovec(servers, src, dst, &iov, 1) < 0)
    perror("sendmsg to client");
}

void pcp_proxy_send_to_server(struct sockaddr_in6 *src,
                              struct sockaddr_in6 *dst,
                              void *data, int data_len,
                              void *data2, int data_len2)
{
  if (!clients) {
    DEBUG("clients socket not initialized");
    return;
  }
  
  struct iovec iov[2] = {
    {.iov_base = data,
     .iov_len = data_len },
    {.iov_base = data2,
     .iov_len = data_len2 }
  };
  
  DEBUG("pcp_proxy_send_to_server %s->%s %d+%d bytes",
        SOCKADDR_IN6_REPR(src), SOCKADDR_IN6_REPR(dst),
        data_len, data_len2);
        
  if (udp46_send_iovec(clients, src, dst, iov, 2) < 0)
    perror("sendmsg to server");
}

static void help_and_exit(const char *p, const char *reason)
{
  if (reason)
    printf("%s.\n\n", reason);
  printf("Usage: %s [-h] [-p server-port] S [S [S ..]]\n", p);
  printf(" Where S = <source prefix>/<source prefix length>=<server>\n");
  printf(" Example: %s 2000:db8::/32=2000:db8::1 ::ffff:0:0/96=2000:db8::2\n", p);
  printf("\nOptions:\n");
  printf(" -h              Show this help message\n");
  printf(" -p server-port  Port to listen on for PCP requests (default: %d)\n", PCP_SERVER_PORT);
  exit(reason ? 1 : 0);
}

int main(int argc, char **argv)
{
  int c;
  int server_port = PCP_SERVER_PORT;
  int ret = 0;

  /* Set up signal handlers */
  signal(SIGINT, signal_handler);
  signal(SIGTERM, signal_handler);
  signal(SIGPIPE, SIG_IGN);

  if (uloop_init() < 0) {
    perror("uloop_init");
    return 1;
  }
  
  /* XXX - add support for parsing interfaces too and use them for
   * announces on reset_epoch */
  while ((c = getopt(argc, argv, "hp:")) > 0) {
    switch (c) {
    case 'h':
      help_and_exit(argv[0], NULL);
      break;
      
    case 'p':
      server_port = atoi(optarg);
      if (server_port <= 0 || server_port > 65535) {
        help_and_exit(argv[0], "Invalid server port");
      }
      break;
      
    default:
      help_and_exit(argv[0], "Unknown option");
      break;
    }
  }
  
  /* Parse command leftover command line arguments. Assume they're of
   * type <source prefix>/<source prefix length>=server, all in IPv6
   * format. */
  if (optind == argc)
    help_and_exit(argv[0], "At least one server is required");
    
  /* Initialize PCP proxy before socket creation */
  pcp_proxy_init();
  
  /* Initialize sockets */
  if (init_sockets(server_port) < 0) {
    fprintf(stderr, "Failed to initialize sockets\n");
    ret = 1;
    goto cleanup;
  }
  
  /* Add servers from command line */
  int i;
  for (i = optind; i < argc; i++) {
    char err[1024];
    if (!pcp_proxy_add_server_string(argv[i], err, sizeof(err))) {
      fprintf(stderr, "Error parsing server '%s': %s\n", argv[i], err);
      ret = 1;
      goto cleanup;
    }
  }
  
  printf("PCP proxy started on port %d\n", server_port);
  printf("Press Ctrl+C to stop\n");
  
  /* Main event loop */
  while (running) {
    uloop_run();
  }
  
  printf("\nShutting down...\n");

cleanup:
  cleanup_sockets();
  uloop_done();
  return ret;
}