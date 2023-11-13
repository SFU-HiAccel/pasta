#include <tapa.h>

#include "add.h"

void load(tapa::mmap<const float> vector,
          tapa::obuffer<float[TILE], 2>& buffer,
          int n_tiles) {
  for (int tile_id = 0; tile_id < n_tiles; tile_id++) {
#pragma HLS pipeline off
    auto section = buffer.acquire();
    auto& buf_ref = section();
    for (int j = 0; j < TILE; j++) {
#pragma HLS pipeline II=1
      buf_ref[j] = vector[tile_id * TILE + j];
    }
  }
}

void vadd(tapa::ibuffer<float[TILE], 2>& buffer_a,
          tapa::ibuffer<float[TILE], 2>& buffer_b,
          tapa::obuffer<float[TILE], 2>& buffer_c,
          int n_tiles) {
  for (int tile_id = 0; tile_id < n_tiles; tile_id++) {
#pragma HLS pipeline off

    auto section_a = buffer_a.acquire();
    auto section_b = buffer_b.acquire();
    auto section_c = buffer_c.acquire();

    auto& buf_rf_a = section_a();
    auto& buf_rf_b = section_b();
    auto& buf_rf_c = section_c();

COMPUTE_LOOP:
    for (int j = 0; j < TILE; j++) {
#pragma HLS pipeline II=1
      buf_rf_c[j] = buf_rf_a[j] + buf_rf_b[j];
    }
  }
}

void store(tapa::mmap<float> vector,
           tapa::ibuffer<float[TILE], 2>& buffer_c,
           int n_tiles) {
  for (int tile_id = 0; tile_id < n_tiles; tile_id++) {
#pragma HLS pipeline off
    auto section = buffer_c.acquire();
    auto& buf_rf = section();
    for (int j = 0; j < TILE; j++) {
#pragma HLS pipeline II=1
      vector[tile_id*TILE + j] = buf_rf[j];
    }
  }
}

void VecAdd(tapa::mmap<const float> vector_a,
            tapa::mmap<const float> vector_b,
            tapa::mmap<float> vector_c, uint64_t n_tiles) {
  tapa::buffer<float[TILE], 2> buffer_a;
  tapa::buffer<float[TILE], 2> buffer_b;
  tapa::buffer<float[TILE], 2> buffer_c;
  tapa::task()
    .invoke(load, vector_a, buffer_a, n_tiles)
    .invoke(load, vector_b, buffer_b, n_tiles)
    .invoke(vadd, buffer_a, buffer_b, buffer_c, n_tiles)
    .invoke(store, vector_c, buffer_c, n_tiles);
}
