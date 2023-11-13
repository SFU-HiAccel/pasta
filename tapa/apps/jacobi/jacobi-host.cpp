#include <cmath>

#include <iostream>
#include <vector>

#include <tapa.h>

using std::clog;
using std::endl;
using std::vector;

void Jacobi(tapa::mmap<float> bank_0_t0, tapa::mmap<const float> bank_0_t1,
            uint64_t coalesced_data_num);

DEFINE_string(bitstream, "", "path to bitstream file, run csim if empty");

int main(int argc, char* argv[]) {
  gflags::ParseCommandLineFlags(&argc, &argv, true);

  const uint64_t width = 100;
  const uint64_t height = argc > 1 ? atoll(argv[1]) : 100;
  vector<float> t1_vec(height * width);
  vector<float> t0_vec(
      height * width +
      (width * 2 + 1));  // additional space is stencil distance
  auto t1 = reinterpret_cast<float(*)[width]>(t1_vec.data());
  auto t0 = reinterpret_cast<float(*)[width]>(t0_vec.data() + width);
  for (uint64_t i = 0; i < height; ++i) {
    for (uint64_t j = 0; j < width; ++j) {
      auto shuffle = [](uint64_t x, uint64_t n) -> float {
        return static_cast<float>((n / 2 - x) * (n / 2 - x));
      };
      t1[i][j] = pow(shuffle(i, height), 1.5f) + shuffle(j, width);
      t0[i][j] = 0.f;
    }
  }

  int64_t kernel_time_ns = tapa::invoke(
      Jacobi, FLAGS_bitstream, tapa::write_only_mmap<float>(t0_vec),
      tapa::read_only_mmap<const float>(t1_vec), height * width / 2);
  clog << "kernel time: " << kernel_time_ns * 1e-9 << " s" << endl;

  uint64_t num_errors = 0;
  const uint64_t threshold = 10;  // only report up to these errors
  for (uint64_t i = 1; i < height - 1; ++i) {
    for (uint64_t j = 1; j < width - 1; ++j) {
      auto expected =
          static_cast<uint64_t>((t1[i - 1][j] + t1[i][j - 1] + t1[i][j] +
                                 t1[i + 1][j] + t1[i][j + 1]) *
                                .2f);
      auto actual = static_cast<uint64_t>(t0[i][j]);
      if (actual != expected) {
        if (num_errors < threshold) {
          clog << "expected: " << expected << ", actual: " << actual << endl;
        } else if (num_errors == threshold) {
          clog << "...";
        }
        ++num_errors;
      }
    }
  }
  if (num_errors == 0) {
    clog << "PASS!" << endl;
  } else {
    if (num_errors > threshold) {
      clog << " (+" << (num_errors - threshold) << " more errors)" << endl;
    }
    clog << "FAIL!" << endl;
  }
  return num_errors > 0 ? 1 : 0;
}
