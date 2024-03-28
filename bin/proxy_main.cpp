#include <sys/socket.h>
#include <cstdio>
#include "sockets.h"
#include "local_proto.h"
#include "util.h"

int main(int argc, char **argv)
{
  int lsock = buddy::tcp_listen(buddy::LOCAL_PORT);

  int world_size = 0;
  int conns = 0;

  do {
    int conn = accept(lsock, NULL, NULL);
    if(conn < 0) {
      perror("accept");
      FAIL("accept failed!");
    }

    buddy::local_init msg;
    buddy::full_read(conn, (char*)&msg, sizeof(msg));

    std::cout << "rank = " << msg.world_rank << ", size = " << msg.world_size << std::endl;
    if (!world_size)
      world_size = msg.world_size;
    else
      CHECK(world_size == msg.world_size);
  } while (conns < world_size);

}
