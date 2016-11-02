#pragma once
#ifndef MICA_UTIL_STOPWATCH_H_
#define MICA_UTIL_STOPWATCH_H_

#include <sys/time.h>
#include "mica/common.h"
#include "mica/util/tsc.h"

namespace mica {
namespace util {
class Stopwatch {
 public:
  void init_start();
  void init_end();

  uint64_t now() const { return rdtsc(); }

  uint64_t c_1_sec() const { return c_1_sec_; }
  uint64_t c_1_msec() const { return c_1_msec_; }
  uint64_t c_1_usec() const { return c_1_usec_; }
  uint64_t c_1_nsec() const { return c_1_nsec_; }  // Usually not very accurate.

  uint64_t diff_in_cycles(uint64_t new_t, uint64_t old_t) const {
    return new_t - old_t;
  }

  uint64_t diff_in_ms(uint64_t new_t, uint64_t old_t) const {
    return (new_t - old_t) * 1000UL / c_1_sec_;
  }

  uint64_t diff_in_us(uint64_t new_t, uint64_t old_t) const {
    return (new_t - old_t) * 1000000UL / c_1_sec_;
  }

  uint64_t diff_in_ns(uint64_t new_t, uint64_t old_t) const {
    // XXX: This may overflow!
    return (new_t - old_t) * 1000000000UL / c_1_sec_;
  }

  double diff(uint64_t new_t, uint64_t old_t) const {
    return static_cast<double>(diff_in_cycles(new_t, old_t)) /
           static_cast<double>(c_1_sec_);
  }

 private:
  uint64_t c_1_sec_;
  uint64_t c_1_msec_;
  uint64_t c_1_usec_;
  uint64_t c_1_nsec_;

  struct timeval init_tv_;
  uint64_t init_t_;
};
}
}

#endif