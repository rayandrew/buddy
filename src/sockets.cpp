#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <cstdio>
#include <cstring>
#include "sockets.h"
#include "util.h"

namespace buddy {

int tcp_connect_ip(uint32_t ip, int port)
{
  struct in_addr ip_addr = { .s_addr = ip };
  char *ip_str = inet_ntoa(ip_addr);
  
  return tcp_connect(ip_str, port);
}

int tcp_connect(const char *server_name, int port)
{
  int sockfd = -1;
  struct addrinfo hints = {}, *result = NULL, *rp = NULL;

  hints.ai_family = AF_UNSPEC; // Allow IPv4 or IPv6.
  hints.ai_socktype = SOCK_STREAM; // TCP

  char port_str[32];
  sprintf(port_str, "%d", port);

  if(getaddrinfo(server_name, port_str, &hints, &result))
  {
    perror("getaddrinfo");
    FAIL("Failed to get address");
  }

  // Start connection:
  for(rp = result; rp; rp = rp->ai_next)
  {
    sockfd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);

    if(sockfd == -1)
        continue;

    if(!connect(sockfd, rp->ai_addr, rp->ai_addrlen))
        break; // Success.

    close(sockfd);
  }

  if(!rp)
  {
    perror("connect");
    FAIL("Failed to connect");
  }

  freeaddrinfo(result);

  return sockfd;
}

int tcp_listen(int port)
{
  int sockfd;
  struct sockaddr_in server_address;

  // Start connection:
  sockfd = socket(AF_INET, SOCK_STREAM, 0);
  if (sockfd == -1)
  {
    perror("socket");
    FAIL("Socket creation failed!");
  }

  memset(&server_address, 0, sizeof(server_address));
  server_address.sin_family = AF_INET;
  server_address.sin_addr.s_addr = htonl(INADDR_ANY);
  server_address.sin_port = htons(port);

  int yes = 1;
  if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) < 0) {
    perror("setsockopt");
  }

  if (bind(sockfd, (struct sockaddr*)&server_address, sizeof(server_address)))
  {
    perror("bind");
    FAIL("Socket bind failed!");
  }

  if (listen(sockfd, 256))
  {
    perror("listen");
    FAIL("Listen failed...");
  }

  return sockfd;
}

void full_read(int fd, char *buf, size_t n)
{
  size_t off = 0;

  while (off < n) {
    ssize_t count = read(fd, buf+off, n-off);

    if (count < 0) {
      perror("read");
      FAIL("full_read failed");
    } else {
      off += count;
    }
  }
}

void full_write(int fd, const char *buf, size_t n)
{
  size_t off = 0;

  while (off < n) {
    ssize_t count = write(fd, buf+off, n-off);

    if (count < 0) {
      perror("write");
      FAIL("full_write failed");
    } else {
      off += count;
    }
  }
}

} // namespace buddy
