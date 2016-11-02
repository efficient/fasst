#pragma once
#ifndef MICA_ALLOC_HUGETLB_SHM_H_
#define MICA_ALLOC_HUGETLB_SHM_H_

#include <limits>
#include <vector>
#include "mica/common.h"
#include "mica/util/config.h"
#include "mica/util/roundup.h"

// Configuration file entries for HugeTLBFS_SHM:
//
//  * hugetlbfs_path (string): The HugeTLBFS directory. Default = "/mnt/huge"
//  * filename_prefix (string): The prefix of the filenames on HugeTLBFS.
//    Default = "mica_shm_"
//  * num_pages_to_init (integer): The maximum number of pages to initialize
//    across all NUMA domains.   This is recommended to be set high because it
//    may not possible to find a sufficient number of free pages on each NUMA
//    domain if this number is too small.  Default = 1048576 (virtually all
//    available pages)
//  * num_pages_to_free (array of integers): The number of pages to free for
//    other applications in each NUMA domain (e.g., Intel DPDK).  Default = [0,
//    ..., 0]
//  * num_pages_to_reserve (array of integers): The number of pages to reserve
//    for use by HugeTLBFS_SHM.  This actually does nothing in the
//    initialization process, but it shows a warning if HugeTLBFS_SHM is not
//    given that number of pages after initialization.  Default = [0, ..., 0]
//  * clean_files_on_init (bool): If true, delete all files whose filename
//    starts with filename_prefix.  Creating the files again takes some time, so
//    enable this only when the old memory state must be discarded.  Default =
//    false
//  * clean_other_files_on_init (bool): Similar to clean_files_on_init, but
//    delete all files whose filename does not starts with filename_prefix.
//    This is required to make num_pages_to_free work.  Default = true
//  * verbose (bool): Print verbose messages.  Default = false

namespace mica {
namespace alloc {
class HugeTLBFS_SHM {
 public:
  HugeTLBFS_SHM(const ::mica::util::Config& config);
  ~HugeTLBFS_SHM();

  static constexpr size_t kInvalidId = std::numeric_limits<size_t>::max();

  static size_t roundup(size_t size) {
    return ::mica::util::roundup<2 * 1048576>(size);
  }

  void* find_free_address(size_t size);

  size_t alloc(size_t length, size_t numa_node);
  bool schedule_release(size_t entry_id);

  bool map(size_t entry_id, void* ptr, size_t offset, size_t length);
  bool unmap(void* ptr);

  size_t get_memuse() const { return used_memory_; }
  void dump_page_info();

  void* malloc_contiguous(size_t size, size_t numa_node);
  void* malloc_contiguous_local(size_t size);
  void free_contiguous(void* ptr);

  void* malloc_striped(size_t size);
  void free_striped(void* ptr);

 private:
  void initialize();

  void clean_files();
  void clean_other_files();
  void make_path(size_t page_id, char* out_path);

  void lock();
  void unlock();

  void check_release(size_t entry_id);

  void* malloc_contiguous_any(size_t size);

  static constexpr size_t kPageSize = 2 * 1048576;

  struct Page {
    size_t file_id;  // ID used for path generation.
    void* addr;
    void* paddr;
    size_t numa_node;
    bool in_use;
  };

  struct Entry {
    size_t refcount;  // reference by mapping
    bool to_remove;   // remove entry when refcount == 0
    size_t length;
    size_t num_pages;
    std::vector<size_t> page_ids;
  };

  struct Mapping {
    size_t entry_id;
    void* addr;
    size_t length;
    size_t page_offset;
    size_t num_pages;
  };

  ::mica::util::Config config_;

  std::string hugetlbfs_path_;
  std::string filename_prefix_;
  size_t num_numa_nodes_;
  size_t num_pages_to_init_;
  std::vector<size_t> num_pages_to_free_;
  std::vector<size_t> num_pages_to_reserve_;
  bool clean_files_on_init_;
  bool clean_other_files_on_init_;
  bool verbose_;

  uint64_t state_lock_;
  std::vector<Page> pages_;
  std::vector<Entry> entries_;
  std::vector<Mapping> mappings_;
  size_t used_memory_;
};
}
}

#endif
