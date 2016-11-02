#pragma once
#ifndef MICA_UTIL_RAND_PHILOX_H_
#define MICA_UTIL_RAND_PHILOX_H_

#include "mica/common.h"
#include "mica/util/philox/philox_random.h"

namespace mica {
namespace util {
class RandPhilox {
 public:
  explicit RandPhilox() : state_(0), result_(), i_(0) {}
  explicit RandPhilox(uint64_t seed) : state_(seed), result_(), i_(0) {}
  RandPhilox(const RandPhilox& o)
      : state_(o.state_), result_(o.result_), i_(o.i_) {}
  RandPhilox& operator=(const RandPhilox& o) {
    state_ = o.state_;
    result_ = o.result_;
    i_ = o.i_;
    return *this;
  }

  uint32_t next_u32() {
    if (!i_) {
      result_ = state_();
      i_ = 4;
    }
    return result_[--i_];
  }

  double next_f64() { return next_u32() / (double)((1UL << 32) - 1); }

 private:
  ::tensorflow::random::PhiloxRandom state_;
  ::tensorflow::random::PhiloxRandom::ResultType result_;
  int i_;
};
}
}

#endif
