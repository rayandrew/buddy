#pragma once

#include <cstddef>

namespace buddy {

int tcp_connect(const char *server_name, int port);
int tcp_listen(int port);
void full_read(int fd, char *buf, size_t n);
void full_write(int fd, const char *buf, size_t n);

} // namespace buddy
