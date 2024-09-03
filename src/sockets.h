#pragma once

#include <cstddef>
#include <stdint.h>

namespace buddy {

int tcp_connect(const char *server_name, int port);
int tcp_connect_ip(uint32_t ip, int port);
int tcp_listen(int port);
void full_read(int fd, char *buf, size_t n);
void full_write(int fd, const char *buf, size_t n);

} // namespace buddy
