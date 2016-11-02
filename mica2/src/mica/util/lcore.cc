// #pragma once
#ifndef MICA_UTIL_LCORE_CC_
#define MICA_UTIL_LCORE_CC_

#include "mica/common.h"
#include <cstdio>
#include <numa.h>
#include <pthread.h>
#ifdef USE_DPDK
#include <rte_common.h>
#include <rte_lcore.h>
#endif
#include "mica/util/lcore.h"

namespace mica {
namespace util {
thread_local size_t LCore::this_lcore_id_ = LCore::kUnknown;

LCore::LCore() {
  numa_count_ = static_cast<size_t>(numa_num_configured_nodes());

  for (auto i = 0; i < numa_num_configured_cpus(); i++) {
    lcore_to_numa_id_.push_back(static_cast<size_t>(numa_node_of_cpu(i)));
#ifndef NDEBUG
// printf("LCore %d -> NUMA %zu\n", i, lcore_to_numa_id_[i]);
#endif
  }
}

void LCore::pin_thread(size_t lcore_id) const {
  // #ifndef USE_DPDK
  cpu_set_t cpuset;

  CPU_ZERO(&cpuset);
  CPU_SET(lcore_id, &cpuset);

  auto s = pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
  if (s != 0) {
    fprintf(stderr, "error: could not set affinity\n");
    assert(false);
    return;
  }
  // #else
  //   if (rte_lcore_id() != lcore_id) {
  //     fprintf(
  //         stderr,
  //         "error: cannot pin the thread to a different lcore while using
  //         DPDK\n");
  //     assert(false);
  //     return;
  //   }
  //   if (lcore_to_numa_id_[lcore_id] !=
  //       rte_lcore_to_socket_id(static_cast<unsigned int>(lcore_id))) {
  //     fprintf(stderr,
  //             "error: numactl and DPDK have inconsistent lcore-to-socket "
  //             "mappings\n");
  //   }
  // #endif

  this_lcore_id_ = lcore_id;
}

const LCore lcore;
}
}

#endif