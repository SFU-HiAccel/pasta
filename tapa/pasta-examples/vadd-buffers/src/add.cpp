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

    for (int j = 0; j < TILE; j++) {
#pragma HLS pipeline II=1
      buf_rf_c[j] = buf_rf_a[j] + buf_rf_b[j];
      buf_rf_c[j] = buf_rf_a[j] + buf_rf_b[j];
      buf_rf_c[j] = buf_rf_a[j] + buf_rf_b[j];
      buf_rf_c[j] = buf_rf_a[j] + buf_rf_b[j];
    }
  }
}

void store(tapa::mmap<float> vector,
           tapa::ibuffers<float[TILE], 4, 2>& buffers_c,
           int n_tiles) {
  for (int tile_id = 0; tile_id < n_tiles; tile_id++) {
#pragma HLS pipeline off
    auto buf_provider_1 = buffers_c[0].acquire();
    auto buf_provider_2 = buffers_c[1].acquire();
    auto buf_provider_3 = buffers_c[2].acquire();
    auto buf_provider_4 = buffers_c[3].acquire();
    auto& buf_rf_1 = buf_provider_1();
    auto& buf_rf_2 = buf_provider_2();
    auto& buf_rf_3 = buf_provider_3();
    auto& buf_rf_4 = buf_provider_4();
    for (int j = 0; j < TILE; j++) {
#pragma HLS pipeline II=1
      vector[PTS_PER_PE*0 + tile_id*TILE + j] = buf_rf_1[j];
    }
    for (int j = 0; j < TILE; j++) {
#pragma HLS pipeline II=1
      vector[PTS_PER_PE*1 + tile_id*TILE + j] = buf_rf_2[j];
    }
    for (int j = 0; j < TILE; j++) {
#pragma HLS pipeline II=1
      vector[PTS_PER_PE*2 + tile_id*TILE + j] = buf_rf_3[j];
    }
    for (int j = 0; j < TILE; j++) {
#pragma HLS pipeline II=1
      vector[PTS_PER_PE*3 + tile_id*TILE + j] = buf_rf_4[j];
    }
  }
}

void VecAdd(tapa::mmaps<const float, 4> vectors_a,
            tapa::mmaps<const float, 4> vectors_b,
            tapa::mmap<float> vector_c, uint64_t n_tiles) {
  tapa::buffers<float[TILE], 4, 2> buffers_a;
  tapa::buffers<float[TILE], 4, 2> buffers_b;
  tapa::buffers<float[TILE], 4, 2> buffers_c;
  tapa::task()
    .invoke<tapa::join, 4>(load, vectors_a, buffers_a, n_tiles)
    .invoke<tapa::join, 4>(load, vectors_b, buffers_b, n_tiles)
    .invoke<tapa::join, 4>(vadd, buffers_a, buffers_b, buffers_c, n_tiles)
    .invoke(store, vector_c, buffers_c, n_tiles);
}
