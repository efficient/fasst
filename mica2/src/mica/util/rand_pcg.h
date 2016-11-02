#pragma once
#ifndef MICA_UTIL_RAND_PCG_H_
#define MICA_UTIL_RAND_PCG_H_

#include <cassert>
#include "mica/common.h"
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-but-set-parameter"
#include "mica/util/pcg/pcg_random.hpp"
#pragma GCC diagnostic pop

namespace mica {
namespace util {
class RandPCG {
 public:
  explicit RandPCG() : state_(0) {}
  explicit RandPCG(uint64_t seed) : state_(seed) { assert(seed < (1UL << 63)); }
  RandPCG(const RandPCG& o) : state_(o.state_) {}
  RandPCG& operator=(const RandPCG& o) {
    state_ = o.state_;
    return *this;
  }

  uint32_t next_u32() { return state_(); }

  double next_f64() { return next_u32() / (double)((1UL << 32) - 1); }

 private:
  // pcg32 state_;
  pcg32_oneseq state_;
  // pcg32_fast state_;
};
}
}

#endif