#pragma once

#include <iostream>
#include <cstdlib>

#define ASSERT_MPI(a) do { int _status = a; if (_status != MPI_SUCCESS) mpi_fail(_status, __FILE__, __LINE__); } while (0)

void mpi_fail(int a, const char *file, int line)
{
  char string[MPI_MAX_ERROR_STRING] = {};
  int len = 0;
  MPI_Error_string(a, string, &len);
  std::cerr << "MPI: " << string << " (" << file << ":" << line << ")" << std::endl;
  abort();
}
