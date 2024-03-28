#pragma once

#include <cstdint>

namespace buddy {

const int LOCAL_PORT = 22838;

struct local_init {
  int32_t world_rank;
  int32_t world_size;
};

} // namespace buddy
