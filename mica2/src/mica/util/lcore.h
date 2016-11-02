#pragma once
#ifndef MICA_UTIL_LCORE_H_
#define MICA_UTIL_LCORE_H_

#include <limits>
#include <vector>
#include <cassert>
#include "mica/common.h"

namespace mica {
namespace util {
class LCore {
 public:
  LCore();

  static constexpr size_t kUnknown = std::numeric_limits<size_t>::max();

  size_t numa_count() const { return numa_count_; }
  size_t lcore_count() const { return lcore_to_numa_id_.size(); }

  size_t numa_id(size_t lcore_id) const {
    assert(lcore_id < lcore_count());
    return lcore_to_numa_id_[lcore_id];
  }

  size_t numa_id() const { return numa_id(lcore_id()); }

  size_t lcore_id() const { return this_lcore_id_; }

  void pin_thread(size_t lcore_id) const;

 private:
  std::vector<size_t> lcore_to_numa_id_;
  size_t numa_count_;
  static thread_local size_t this_lcore_id_;
};

extern const LCore lcore;
}
}

#endif