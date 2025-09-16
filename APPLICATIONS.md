# Buddy Applications

Six apps have been ported to Buddy so far.

## Bale Apps

Five apps from Bale/Conveyors are included in this repository.

### Histogram: `test/histo`

Parameters: `$bins $load $seed` (the number of elements per rank, the number of iterations, and the random seed).

Example: `mpirun -np 16 build/test/histo 10000 100000 1`

Optionally, this app also supports increasing the window size by setting the `BUDDY_SENDBUF` and `BUDDY_RECVBUF` environment variables to change the number of send and receive buffers. It can also help to prevent a deadlock in some situations. The other applications are currently using only a single send and receive buffer.

### Indexgather: `test/bale/ig`

Parameters: `-N $num_requests -T $elements_per_rank`

Example: `mpirun -np 16 build/test/bale/ig -N 80000000 -T 10000`

### Triangle Counting: `test/bale/triangle`

Parameters: `-N $vertices` or `-n $vertices_per_rank`. (The original supports different types of graphs, but only the flat model is ported.)

Example: `mpirun -np 16 build/test/bale/triangle -N 1600000`

### Transpose: `test/bale/transpose_matrix`

Parameters: `-N $rows_per_rank` or `-n $rows_per_rank`. (The original supports different types of matrices, but only the flat model is ported.)

Example: `mpirun -np 16 build/test/bale/transpose_matrix -N 2000000`

### SSSP: `test/bale/sssp`

Parameters: `-N $vertices` or `-n $vertices_per_rank`. (The original supports different types of graphs, but only the flat model is ported.)

Example: `mpirun -np 16 build/test/bale/sssp -N 1600000`

## Quicksilver

The ported Quicksilver is available in a different repository: https://github.com/SPS-Lab/quicksilver-buddy. Build using the Makefile in the src/ directory. Change the BUDDY variable in the Makefile to point to your Buddy build directory.

Warning: Currently, only the 2 rank case works.

Parameters: check the `Examples/CTS2_Benchmark/CTS2_scaling.sh` script.

Example: `mpirun -np 2 src/qs -i Examples/CTS2_Benchmark/CTS2.inp  -X 32  -Y 16  -Z 16  -x 32  -y 16  -z 16  -I 2  -J 1  -K 1  -n 81920`
