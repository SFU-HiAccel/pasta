#include <iostream>
#include <vector>
#include <cstdlib>

#include <gflags/gflags.h>
#include <tapa.h>

#include "add.h"

void VecAdd(tapa::mmap<const float> vector_a,
            tapa::mmap<const float> vector_b,
            tapa::mmap<float> vector_c, uint64_t n_tiles);

DEFINE_string(bitstream, "", "path to bitstream file, run csim if empty");

int main(int argc, char *argv[]) {
  gflags::ParseCommandLineFlags(&argc, &argv, false);

  std::array<float, N> array_a;
  std::array<float, N> array_b;
  std::array<float, N> array_c_fpga;
  std::array<float, N> array_c_cpu;

  srand(0);
  for (int i = 0; i < N; i++) {
    array_a[i] = rand() % 100;
    array_b[i] = rand() % 100;
    array_c_cpu[i] = array_a[i] + array_b[i];
  }

  const int n_tiles_per_pe = N / TILE;

  int64_t kernel_time_us = tapa::invoke(VecAdd, FLAGS_bitstream,
    tapa::read_only_mmap<const float>(array_a),
    tapa::read_only_mmap<const float>(array_b),
    tapa::write_only_mmap<float>(array_c_fpga), n_tiles_per_pe);

  for (int i = 0; i < N; i++) {
    if (array_c_cpu[i] != array_c_fpga[i]) {
      std::cout << "Mismatch found at i = " << i << std::endl;
      return -1;
    }
  }
  std::cout << "Successfully processed!" << std::endl;
  return 0;
}
