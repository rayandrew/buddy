#pragma once

#include <infiniband/verbs.h>

namespace buddy::rdma {

struct IBDest {
  uint16_t lid;
  uint16_t _padding;
  uint32_t qpn;
  uint32_t psn;
  union ibv_gid gid;
};

struct server_cqs {
  server_cqs();

  ibv_srq *srq;
  ibv_cq *send;
  ibv_cq *recv;
};

class Context {
  public:
      ~Context();
      void init();
      ibv_context* get_ctx(){return ctx;}
      ibv_pd* get_pd(){return pd;}
      const IBDest *get_local_dest_template() {return &local_dest_template;}

      static Context& get();

  private:
      IBDest local_dest_template = {};
      ibv_context *ctx = {};
      ibv_pd *pd = {};
      ibv_port_attr port_info = {};
};

class QP {
  public:
    QP() = default;
    QP(int connfd);
    QP(server_cqs cqs, int connfd);
    ~QP();

    void send(ibv_mr *mr, void *buf, unsigned len, uint64_t wr_id);
    void send_imm(uint32_t tag, ibv_mr *mr, unsigned len);
    void send_imm_inline(uint32_t tag, void *buf, unsigned len);
    void write_imm(uint32_t tag, void *buf, ibv_mr *mr, unsigned len, uint64_t remote_addr, uint32_t rkey, uint64_t wr_id);
    void write_imm(uint32_t tag)
    {
      write_imm(tag, NULL, NULL, 0, 0, 0, 0);
    }
    void write(void *buf, ibv_mr *mr, unsigned len, uint64_t remote_addr, uint32_t rkey);
    void read(void *buf, ibv_mr *mr, unsigned len, uint64_t remote_addr, uint32_t rkey);
    void recv(ibv_mr *mr, unsigned len);

    void post_rdma_recv();
    void wait_send(ibv_wc *wc);
    void wait_recv(ibv_wc *wc);

    ibv_qp *get_qp() { return qp; }
    ibv_cq *get_recv_cq() { return recv_cq; }
    ibv_cq *get_send_cq() { return send_cq; }

  private:
    ibv_qp *qp = NULL;
    ibv_cq *send_cq = NULL;
    ibv_cq *recv_cq = NULL;
    uint32_t max_inline_data = 0;
    bool own_cqs = false;

    void setup_common(ibv_srq *srq, int connfd);
    void wait_op(ibv_wc *wc, ibv_cq *cq);
};

void init();

} // namespace buddy::rdma
