// #pragma once
#ifndef MICA_UTIL_STOPWATCH_CC_
#define MICA_UTIL_STOPWATCH_CC_

#include "mica/util/stopwatch.h"
#include "mica/util/barrier.h"

namespace mica {
namespace util {
void Stopwatch::init_start() {
  init_t_ = now();
  gettimeofday(&init_tv_, nullptr);
}

void Stopwatch::init_end() {
  struct timeval tv;

  const uint64_t min_time = 100000UL;  // 100,000 us = 0.1 sec

  while (true) {
    gettimeofday(&tv, nullptr);

    uint64_t diff =
        static_cast<uint64_t>(tv.tv_sec - init_tv_.tv_sec) * 1000000UL +
        static_cast<uint64_t>(tv.tv_usec - init_tv_.tv_usec);

    if (diff >= min_time) {
      uint64_t t = now();
      c_1_sec_ = (t - init_t_) * min_time * 10 / diff;
      c_1_msec_ = c_1_sec_ / 1000;
      c_1_usec_ = c_1_msec_ / 1000;
      c_1_nsec_ = c_1_usec_ / 1000;
      break;
    }

    pause();
  }
}
}
}

#endif
