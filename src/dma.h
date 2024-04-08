#pragma once

struct doca_dev;
struct doca_mmap;
struct doca_ctx;
struct doca_buf_inventory;
struct doca_workq;
struct doca_buf;


namespace buddy::dma {

class Buffer {
  public:
    Buffer(size_t len);
    void send(int sockfd);

    char * const buf;

  private:
    size_t len;

    doca_dev *dev;
    doca_mmap *dmap;
};

enum direction { H2D, D2H };

struct jobspec {
  unsigned client;
  uint32_t offset;
  uint32_t len;
  direction dir;
};

class Engine {
  public:
    Engine(unsigned num_clients, int *socks);
    void transfer(jobspec job);
    bool poll(jobspec *job);

    char *client_buf(unsigned client)
    {
      return local_buf + client*buflen;
    }

  private:
    unsigned num_clients;
    size_t buflen;
    char *local_buf;
    char **remote_addr;

    doca_dev *dev;
    doca_ctx *ctx;
    doca_mmap *local_map;
    doca_buf_inventory *buf_inv;
    doca_workq *workq;
    doca_mmap **remote_map;
    doca_buf **doca_buf_local;
    doca_buf **doca_buf_remote;
};

} // namespace buddy
