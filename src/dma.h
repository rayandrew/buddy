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

class Engine {
  public:
    Engine(unsigned num_clients, int *socks);
    void transfer(unsigned client, size_t len);
    bool poll(unsigned *client);

  private:
    unsigned num_clients;
    size_t buflen;
    char *local_buf;

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
