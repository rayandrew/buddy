#pragma once

#include <mpi.h>
#include <iostream>
#include <cstdlib>

#define FAIL(a) do { std::cerr << "FAIL: " << a << " (" << __FILE__ << ":" << __LINE__ << ")" << std::endl; abort(); } while (0)
#define CHECK(a) do { if (!(a)) FAIL("check " #a); } while (0)
#define CHECK_MPI(a) do { int _status = a; if (_status != MPI_SUCCESS) ::buddy::check_mpi_fail(_status, __FILE__, __LINE__); } while (0)

namespace buddy {

inline void check_mpi_fail(int a, const char *file, int line)
{
  char string[MPI_MAX_ERROR_STRING] = {};
  int len = 0;
  MPI_Error_string(a, string, &len);
  std::cerr << "CHECK_MPI: " << string << " (" << file << ":" << line << ")" << std::endl;
  abort();
}

} // namespace buddy
