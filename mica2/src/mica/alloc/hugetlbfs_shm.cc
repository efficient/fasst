// #pragma once
#ifndef MICA_ALLOC_HUGETLB_SHM_CC_
#define MICA_ALLOC_HUGETLB_SHM_CC_

#include <algorithm>
#include <cstdio>
#include <cstring>

#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <linux/limits.h>

#include "mica/alloc/hugetlbfs_shm.h"
#include "mica/util/roundup.h"
#include "mica/util/barrier.h"
#include "mica/util/lcore.h"
#include "mica/util/safe_cast.h"

namespace mica {
namespace alloc {
void HugeTLBFS_SHM::clean_files() {
  char cmd[PATH_MAX];
  snprintf(cmd, PATH_MAX, "rm %s/%s* > /dev/null 2>&1", hugetlbfs_path_.c_str(),
           filename_prefix_.c_str());
  // printf("> %s\n", cmd);
  int ret = system(cmd);
  (void)ret;
}

void HugeTLBFS_SHM::clean_other_files() {
  DIR* d = opendir(hugetlbfs_path_.c_str());
  if (d != nullptr) {
    long name_max = pathconf(hugetlbfs_path_.c_str(), _PC_NAME_MAX);
    if (name_max == -1) name_max = 255;
    dirent* de = reinterpret_cast<dirent*>(
        malloc(offsetof(dirent, d_name) +
               static_cast<long unsigned int>(name_max) + 1));

    while (true) {
      dirent* rde;
      if (readdir_r(d, de, &rde) != 0) break;
      if (rde == nullptr) break;

      if (strcmp(rde->d_name, ".") == 0 || strcmp(rde->d_name, "..") == 0)
        continue;
      if (strncmp(rde->d_name, filename_prefix_.c_str(),
                  std::min(strlen(rde->d_name), filename_prefix_.size())) == 0)
        continue;

      char path[PATH_MAX];
      snprintf(path, PATH_MAX, "%s/%s", hugetlbfs_path_.c_str(), rde->d_name);

      // char cmd[PATH_MAX];
      // snprintf(cmd, PATH_MAX, "rm %s > /dev/null 2>&1", path);
      // // printf("> %s\n", cmd);
      // int ret = system(cmd);
      // (void)ret;
      unlink(path);
    }
    closedir(d);

    free(de);
  }
}

void HugeTLBFS_SHM::make_path(size_t page_id, char* out_path) {
  snprintf(out_path, PATH_MAX, "%s/%s%zu", hugetlbfs_path_.c_str(),
           filename_prefix_.c_str(), page_id);
}

void HugeTLBFS_SHM::lock() {
  // while (true) {
  //   if (__sync_bool_compare_and_swap((volatile uint64_t*)&state_lock_, 0UL,
  //                                    1UL))
  //     break;
  // }
  while (__sync_lock_test_and_set((volatile uint64_t*)&state_lock_, 1UL) == 1UL)
    ::mica::util::pause();
}

void HugeTLBFS_SHM::unlock() {
  // ::mica::util::memory_barrier();
  // *(volatile uint64_t*)&state_lock_ = 0UL;
  __sync_lock_release((volatile uint64_t*)&state_lock_);
}

void HugeTLBFS_SHM::dump_page_info() {
  lock();
  for (size_t page_id = 0; page_id < pages_.size(); page_id++) {
    if (pages_[page_id].addr == nullptr) continue;

    printf("page %zu: addr=%p numa_node=%zu in_use=%s\n", page_id,
           pages_[page_id].addr, pages_[page_id].numa_node,
           pages_[page_id].in_use ? "yes" : "no");
  }
  unlock();
}

HugeTLBFS_SHM::HugeTLBFS_SHM(const ::mica::util::Config& config)
    : config_(config) {

  // Don't use HugeTLBFS_SHM in HoTS
  printf("Error: HugeTLBFS_SHM not supported in HoTS.\n");
  exit(-1);

  // Parse the configuration.
  hugetlbfs_path_ = config.get("hugetlbfs_path").get_str("/mnt/huge");
  filename_prefix_ = config.get("filename_prefix").get_str("mica_shm_");

  num_pages_to_init_ = config.get("num_pages_to_init").get_uint64(1048576);

  {
    auto c = config.get("num_pages_to_free");
    if (!c.exists()) {
      ;
    } else {
      for (size_t i = 0; i < c.size(); i++) {
        size_t page_count = c.get(i).get_uint64();
        num_pages_to_free_.push_back(page_count);
      }
    }
    for (size_t i = num_pages_to_free_.size();
         i < ::mica::util::lcore.numa_count(); i++)
      num_pages_to_free_.push_back(0);
    if (num_pages_to_free_.size() != ::mica::util::lcore.numa_count())
      printf(
          "warning: num_pages_to_free has more entries than the total NUMA "
          "domain count in the system\n");
  }

  {
    auto c = config.get("num_pages_to_reserve");
    if (!c.exists()) {
      ;
    } else {
      for (size_t i = 0; i < c.size(); i++) {
        size_t page_count = c.get(i).get_uint64();
        num_pages_to_reserve_.push_back(page_count);
      }
    }
    for (size_t i = num_pages_to_reserve_.size();
         i < ::mica::util::lcore.numa_count(); i++)
      num_pages_to_reserve_.push_back(0);
    if (num_pages_to_reserve_.size() != ::mica::util::lcore.numa_count())
      printf(
          "warning: num_pages_to_reserve has more entries than the total NUMA "
          "domain count in the system\n");
  }

  clean_files_on_init_ = config.get("clean_files_on_init").get_bool(false);
  clean_other_files_on_init_ =
      config.get("clean_other_files_on_init").get_bool(true);

  verbose_ = config.get("verbose").get_bool(false);

  initialize();
}

void HugeTLBFS_SHM::initialize() {
  size_t page_id;
  size_t numa_node;

  size_t numa_count = ::mica::util::lcore.numa_count();

  if (clean_files_on_init_) {
    if (verbose_) printf("cleaning up existing files\n");
    clean_files();
  }

  if (clean_other_files_on_init_) {
    if (verbose_) printf("cleaning up other existing files\n");
    clean_other_files();
  }

  state_lock_ = 0;
  used_memory_ = 0;

  // initialize pages
  if (verbose_) printf("initializing pages\n");
  size_t num_allocated_pages;
  for (page_id = 0; page_id < num_pages_to_init_; page_id++) {
    if (verbose_ && (page_id % 128 == 0)) {
      printf("\r%zu / %zu", page_id + 1, num_pages_to_init_);
      fflush(stdout);
    }

    size_t file_id = page_id;
    char path[PATH_MAX];
    make_path(file_id, path);

    int fd = open(path, O_CREAT | O_RDWR, 0755);
    if (fd == -1) {
      perror("");
      fprintf(stderr, "error: could not open %s\n", path);
      assert(false);
      return;
    }

    void* p =
        mmap(nullptr, kPageSize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);

    close(fd);

    if (p == MAP_FAILED) {
      unlink(path);
      break;
    }

    // printf("got page %zu (address %p)\n", page_id, p);

    // this is required to cause a page fault and invoke actual memory
    // allocation
    *(size_t*)p = 0;

    assert(pages_.size() == page_id);
    pages_.push_back(Page{0, nullptr, nullptr, 0, false});
    pages_[page_id].file_id = file_id;
    pages_[page_id].addr = p;
    // printf("initial allocation of %zu on %p\n", kPageSize, p);
  }
  num_allocated_pages = page_id;

  if (verbose_)
    printf("\rinitial allocation of %zu pages\n", num_allocated_pages);

  // sort by virtual address
  if (verbose_) printf("sorting by virtual address\n");
  std::sort(pages_.begin(), pages_.end(),
            [](Page& a, Page& b) { return a.addr < b.addr; });

  // detect numa socket
  if (verbose_) printf("detecting NUMA mapping\n");
  FILE* f = fopen("/proc/self/numa_maps", "r");
  if (f == nullptr) {
    perror("");
    fprintf(stderr, "error: could not open /proc/self/numa_maps\n");
    assert(false);
    return;
  }

  page_id = 0;
  char buf[BUFSIZ];
  while (true) {
    if (fgets(buf, sizeof(buf), f) == nullptr) break;

    size_t addr = strtoull(buf, nullptr, 16);
    // for (page_id = 0; page_id < num_allocated_pages; page_id++)
    if (page_id < num_allocated_pages) {
      if (pages_[page_id].addr == (void*)addr) {
        char* p = strstr(buf, " N");
        if (p == nullptr) {
          assert(false);
          fprintf(stderr, "error: could not parse /proc/self/numa_maps\n");
          return;
        }

        size_t numa_node;
        size_t page_count;
        if (sscanf(p, " N%zu=%zu", &numa_node, &page_count) != 2) {
          assert(false);
          return;
        }
        if (page_count != 1)
          fprintf(stderr, "warning: page count for %p is expected to be 1\n",
                  (void*)addr);

        pages_[page_id].numa_node = numa_node;
        // printf("%p is on numa node %zu\n", p, numa_node);
        page_id++;
      }
    }
  }
  fclose(f);
  if (page_id != num_allocated_pages) {
    fprintf(stderr,
            "error: unable to get NUMA mapping information for all pages "
            "(/proc/self/numa_maps may be not sorted by virtual address)\n");
    assert(false);
    return;
  }

  // get physical address (pagemap.txt)
  if (verbose_) printf("detecting physical address of pages\n");
  size_t normal_page_size = (size_t)getpagesize();
  int fd = open("/proc/self/pagemap", O_RDONLY);
  if (fd == -1) {
    perror("");
    fprintf(stderr, "error: could not open /proc/self/pagemap\n");
    assert(false);
    return;
  }

  for (page_id = 0; page_id < num_allocated_pages; page_id++) {
    size_t pfn = (size_t)pages_[page_id].addr / normal_page_size;
    off_t offset = (off_t)(sizeof(uint64_t) * pfn);

    if (lseek(fd, offset, SEEK_SET) != offset) {
      perror("");
      close(fd);
      fprintf(stderr, "error: could not seek in /proc/self/pagemap\n");
      assert(false);
      return;
    }

    uint64_t entry;
    if (read(fd, &entry, sizeof(uint64_t)) == -1) {
      perror("");
      close(fd);
      fprintf(stderr, "error: could not read /proc/self/pagemap\n");
      assert(false);
      return;
    }

    pages_[page_id].paddr =
        (void*)((entry & 0x7fffffffffffffULL) * normal_page_size);
    // printf("virtual addr %p = physical addr %p\n", pages_[page_id].addr,
    // pages_[page_id].paddr);
  }

  // sort by physical address
  if (verbose_) printf("sorting by physical address\n");
  std::sort(pages_.begin(), pages_.end(),
            [](Page& a, Page& b) { return a.paddr < b.paddr; });

  // Throw away surplus pages on each numa node.  Release largest regions
  // (contiguous in physical memory) first so that the freed memory can be used
  // for DMA.
  if (verbose_) printf("releasing unnecessary pages\n");
  for (numa_node = 0; numa_node < numa_count; numa_node++) {
    size_t num_pages_to_free = num_pages_to_free_[numa_node];

    while (num_pages_to_free > 0) {
      size_t largest_region_start = 0;
      size_t largest_region_size = 0;

      size_t region_start = (size_t)-1;
      size_t region_size = (size_t)-1;
      void* last_page_paddr = nullptr;

      for (page_id = 0; page_id < num_allocated_pages; page_id++) {
        if (pages_[page_id].paddr == nullptr ||
            pages_[page_id].numa_node != numa_node) {
          if (region_size != (size_t)-1) {
            if (largest_region_size < region_size) {
              largest_region_start = region_start;
              largest_region_size = region_size;
            }
          }
          region_start = (size_t)-1;
          region_size = (size_t)-1;
          continue;
        }

        void* paddr = pages_[page_id].paddr;
        if (reinterpret_cast<char*>(last_page_paddr) + kPageSize != paddr) {
          if (region_size != (size_t)-1) {
            if (largest_region_size < region_size) {
              largest_region_start = region_start;
              largest_region_size = region_size;
            }
          }
          region_start = page_id;
          region_size = 1;
        } else
          region_size++;
        last_page_paddr = paddr;
      }
      if (region_size != (size_t)-1) {
        if (largest_region_size < region_size) {
          largest_region_start = region_start;
          largest_region_size = region_size;
        }
      }

      // no more pages left for this numa node
      if (largest_region_size == 0) break;

      if (verbose_)
        printf("node %zu: found largest region %zu (%zu pages)\n", numa_node,
               largest_region_start, largest_region_size);

      for (page_id = largest_region_start;
           page_id < largest_region_start + largest_region_size &&
           num_pages_to_free > 0;
           page_id++) {
        void* addr = pages_[page_id].addr;
        assert(pages_[page_id].numa_node == numa_node);

        // printf("deallocating surplus page %p on numa node %zu\n", addr,
        // numa_node);

        msync(addr, kPageSize, MS_SYNC);

        int ret = munmap(addr, kPageSize);
        assert(ret == 0);
        (void)ret;

        // wait until the page is fully freed.
        while (true) {
          // printf("checking if page %zu (address %p) has not been
          // unmapped.\n",
          //        page_id, addr);
          void* p = mmap(addr, kPageSize, PROT_READ,
                         MAP_ANONYMOUS | MAP_PRIVATE | MAP_FIXED, -1, 0);
          if (p != MAP_FAILED) {
            munmap(p, kPageSize);
            break;
          }
          printf("page %zu (address %p) has not been unmapped. waiting...\n",
                 page_id, addr);
          fflush(stdout);
          // break;
          // sync();
          // sleep(1);
        }

        char path[PATH_MAX];
        make_path(pages_[page_id].file_id, path);
        unlink(path);

        // memset(&pages_[page_id], 0, sizeof(pages_[page_id]));
        pages_[page_id].addr = nullptr;
        pages_[page_id].paddr = nullptr;
        pages_[page_id].numa_node = 0;
        pages_[page_id].in_use = false;

        num_pages_to_free--;
      }
    }

    if (num_pages_to_free == 0) {
      if (verbose_)
        printf("freed %zu pages (requested: %zu) on numa node %zu\n",
               num_pages_to_free_[numa_node], num_pages_to_free_[numa_node],
               numa_node);
    } else {
      printf(
          "warning: could free only %zu pages (requested: %zu) on numa node "
          "%zu\n",
          num_pages_to_free_[numa_node] - num_pages_to_free,
          num_pages_to_free_[numa_node], numa_node);
    }

    size_t num_reserved_pages = 0;
    for (page_id = 0; page_id < num_allocated_pages; page_id++) {
      if (pages_[page_id].paddr == nullptr) continue;
      if (pages_[page_id].numa_node != numa_node) continue;

      num_reserved_pages++;
    }

    if (num_reserved_pages >= num_pages_to_reserve_[numa_node]) {
      if (verbose_)
        printf("reserved %zu pages (requested: %zu) on numa node %zu\n",
               num_reserved_pages, num_pages_to_reserve_[numa_node], numa_node);
    } else {
      printf(
          "warning: could reserve only %zu pages (requested: %zu) on numa node "
          "%zu\n",
          num_reserved_pages, num_pages_to_reserve_[numa_node], numa_node);
    }
  }

  printf("HugeTLBFS_SHM: syncing and sleeping for 1 second\n");
  sync();
  sleep(1);

  // {
  //   FILE* f = fopen("/proc/self/numa_maps", "r");
  //   char buf[BUFSIZ];
  //   while (true) {
  //     if (fgets(buf, sizeof(buf), f) == nullptr) break;
  //     printf("%s", buf);
  //   }
  //   fclose(f);
  // }

  // dump_page_info();
}

HugeTLBFS_SHM::~HugeTLBFS_SHM() {
  // TODO: Implement.
}

void* HugeTLBFS_SHM::find_free_address(size_t size) {
  size_t alignment = kPageSize;

  if (alignment == 0) alignment = 1;

  if (::mica::util::next_power_of_two(alignment) != alignment) {
    fprintf(stderr, "error: invalid alignment\n");
    return nullptr;
  }

  int fd = open("/dev/zero", O_RDONLY);
  if (fd == -1) {
    perror("");
    fprintf(stderr, "error: could not open /dev/zero\n");
    assert(false);
    return nullptr;
  }

  void* p = mmap(nullptr, size + alignment, PROT_READ, MAP_PRIVATE, fd, 0);

  close(fd);

  if (p == MAP_FAILED) {
    perror("");
    fprintf(stderr,
            "error: failed to map /dev/zero to find a free memory region of "
            "size %zu\n",
            size);
    assert(false);
    return nullptr;
  }

  munmap(p, size);

  p = (void*)(((size_t)p + (alignment - 1)) & ~(alignment - 1));
  return p;
}

size_t HugeTLBFS_SHM::alloc(size_t length, size_t numa_node) {
  if (numa_node == (size_t)-1) {
    // using rte_socket_id() is unreliable on some systems (physical id of
    // processor 0 is 1 (0 expected))
    numa_node = ::mica::util::lcore.numa_id();
    if (numa_node == ::mica::util::LCore::kUnknown) {
      fprintf(stderr, "warning: failed to detect numa node for current cpu\n");
      return kInvalidId;
    }
  }
  lock();

  size_t entry_id;
  for (entry_id = 0; entry_id < entries_.size(); entry_id++) {
    if (entries_[entry_id].page_ids.empty()) break;
  }

  // Create a new Entry if there is no unused Entry.
  if (entry_id == entries_.size())
    entries_.push_back(Entry{0, false, 0, 0, std::vector<size_t>()});

  size_t num_pages = (length + (kPageSize - 1)) / kPageSize;
  entries_[entry_id].length = length;
  entries_[entry_id].num_pages = num_pages;
  size_t num_allocated_pages = 0;

  size_t page_id;
  for (page_id = 0; page_id < pages_.size(); page_id++) {
    if (num_allocated_pages == num_pages) break;

    if (pages_[page_id].addr == nullptr) continue;
    if (pages_[page_id].in_use || pages_[page_id].numa_node != numa_node)
      continue;

    assert(entries_[entry_id].page_ids.size() == num_allocated_pages);
    entries_[entry_id].page_ids.push_back(page_id);
    num_allocated_pages++;
  }
  if (num_pages != num_allocated_pages) {
    fprintf(
        stderr,
        "warning: insufficient memory on numa node %zu to allocate %zu bytes\n",
        numa_node, length);
    // memset(&entries_[entry_id], 0, sizeof(entries_[entry_id]));
    entries_[entry_id].refcount = 0;
    entries_[entry_id].to_remove = false;
    entries_[entry_id].length = 0;
    entries_[entry_id].num_pages = 0;
    entries_[entry_id].page_ids.clear();
    unlock();
    return static_cast<size_t>(-1);
  }

  size_t page_index;
  for (page_index = 0; page_index < num_pages; page_index++)
    pages_[entries_[entry_id].page_ids[page_index]].in_use = true;

  unlock();

  if (verbose_)
    printf(
        "allocated shm entry %zu (length=%zu, num_pages=%zu) on numa node "
        "%zu\n",
        entry_id, length, num_pages, numa_node);

  // dump_page_info();

  return entry_id;
}

void HugeTLBFS_SHM::check_release(size_t entry_id) {
  // lock assumed

  // remove entry if no one uses and scheduled to be removed
  if (entries_[entry_id].refcount == 0 && entries_[entry_id].to_remove != 0) {
    size_t page_index;
    for (page_index = 0; page_index < entries_[entry_id].num_pages;
         page_index++)
      pages_[entries_[entry_id].page_ids[page_index]].in_use = false;
    // memset(&entries_[entry_id], 0, sizeof(entries_[entry_id]));
    entries_[entry_id].refcount = 0;
    entries_[entry_id].to_remove = false;
    entries_[entry_id].length = 0;
    entries_[entry_id].num_pages = 0;
    entries_[entry_id].page_ids.clear();

    used_memory_ -= entries_[entry_id].num_pages * kPageSize;
    if (verbose_) printf("deallocated shm entry %zu\n", entry_id);
  }
}

bool HugeTLBFS_SHM::schedule_release(size_t entry_id) {
  lock();

  if (entries_[entry_id].page_ids.empty()) {
    unlock();
    fprintf(stderr, "error: invalid entry\n");
    assert(false);
    return false;
  }

  entries_[entry_id].to_remove = 1;
  HugeTLBFS_SHM::check_release(entry_id);

  unlock();
  return true;
}

bool HugeTLBFS_SHM::map(size_t entry_id, void* ptr, size_t offset,
                        size_t length) {
  if (((size_t)ptr & ~(kPageSize - 1)) != (size_t)ptr) {
    fprintf(stderr, "error: invalid ptr alignment\n");
    assert(false);
    return false;
  }

  if ((offset & ~(kPageSize - 1)) != offset) {
    fprintf(stderr, "error: invalid offset alignment\n");
    assert(false);
    return false;
  }

  lock();

  // check entry
  if (entries_[entry_id].page_ids.empty()) {
    unlock();
    fprintf(stderr, "error: invalid entry\n");
    assert(false);
    return false;
  }

  if (offset > entries_[entry_id].length) {
    unlock();
    fprintf(stderr, "error: invalid offset\n");
    assert(false);
    return false;
  }

  if (offset + length > entries_[entry_id].length) {
    unlock();
    fprintf(stderr, "error: invalid length\n");
    assert(false);
    return false;
  }

  // find empty mapping
  size_t mapping_id;
  for (mapping_id = 0; mapping_id < mappings_.size(); mapping_id++) {
    if (mappings_[mapping_id].addr == nullptr) break;
  }

  // Create a new Mapping if there is no unused Mapping.
  if (mapping_id == mappings_.size())
    mappings_.push_back(Mapping{0, nullptr, 0, 0, 0});

  size_t page_offset = offset / kPageSize;
  size_t num_pages = (length + (kPageSize - 1)) / kPageSize;

  // map
  void* p = ptr;
  size_t page_index = page_offset;
  size_t page_index_end = page_offset + num_pages;
  int error = 0;
  while (page_index < page_index_end) {
    char path[PATH_MAX];
    make_path(pages_[entries_[entry_id].page_ids[page_index]].file_id, path);
    int fd = open(path, O_RDWR);
    if (fd == -1) {
      error = 1;
      perror("");
      fprintf(stderr, "error: could not open %s\n", path);
      assert(false);
      break;
    }

    void* ret_p = mmap(p, kPageSize, PROT_READ | PROT_WRITE,
                       MAP_SHARED | MAP_FIXED, fd, 0);
    // void* ret_p = mmap(p, kPageSize, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
    // 0);

    close(fd);

    if (ret_p == MAP_FAILED) {
      error = 1;
      fprintf(stderr, "error: mmap failed at %p (error)\n", p);
      // we can retry with a new free memory region.
      // assert(false);
      break;
    } else if (ret_p != p) {
      error = 1;
      fprintf(stderr, "error: mmap failed at %p (mismatch; mapped at %p)\n", p,
              ret_p);
      // we can retry with a new free memory region.
      // assert(false);
      break;
    }

    page_index++;
    p = (void*)((size_t)p + kPageSize);
  }
  if (error) {
    // clean partially mapped memory
    p = ptr;
    size_t page_index_clean = page_offset;
    while (page_index_clean < page_index) {
      munmap(p, kPageSize);
      page_index_clean++;
      p = (void*)((size_t)p + kPageSize);
    }
    unlock();
    return false;
  }

  // register mapping
  if (entries_[entry_id].refcount == 0)
    used_memory_ += entries_[entry_id].num_pages * kPageSize;
  entries_[entry_id].refcount++;

  mappings_[mapping_id].entry_id = entry_id;
  mappings_[mapping_id].addr = ptr;
  mappings_[mapping_id].length = length;
  mappings_[mapping_id].page_offset = page_offset;
  mappings_[mapping_id].num_pages = num_pages;

  unlock();

  if (verbose_)
    printf(
        "created new mapping %zu (shm entry %zu, page_offset=%zu, "
        "num_pages=%zu) at %p\n",
        mapping_id, entry_id, page_offset, num_pages, ptr);

  return true;
}

bool HugeTLBFS_SHM::unmap(void* ptr) {
  lock();

  // find mapping
  size_t mapping_id;
  for (mapping_id = 0; mapping_id < mappings_.size(); mapping_id++) {
    if (mappings_[mapping_id].addr == ptr) break;
  }

  if (mapping_id == mappings_.size()) {
    fprintf(stderr, "error: invalid unmap\n");
    unlock();
    return false;
  }

  // unmap pages
  size_t page_index;
  for (page_index = 0; page_index < mappings_[mapping_id].num_pages;
       page_index++) {
    munmap(ptr, kPageSize);
    ptr = (void*)((size_t)ptr + kPageSize);
  }

  // remove reference to entry
  --entries_[mappings_[mapping_id].entry_id].refcount;
  HugeTLBFS_SHM::check_release(mappings_[mapping_id].entry_id);

  // remove mapping
  // memset(&mappings_[mapping_id], 0, sizeof(mappings_[mapping_id]));
  mappings_[mapping_id].entry_id = 0;
  mappings_[mapping_id].addr = nullptr;
  mappings_[mapping_id].length = 0;
  mappings_[mapping_id].page_offset = 0;
  mappings_[mapping_id].num_pages = 0;

  unlock();

  if (verbose_) printf("removed mapping %zu at %p\n", mapping_id, ptr);

  return true;
}

void* HugeTLBFS_SHM::malloc_contiguous(size_t size, size_t lcore) {
  size = HugeTLBFS_SHM::roundup(size);
  // size_t entry_id = mehcached_shm_alloc(size, (size_t)-1);
  // size_t entry_id = mehcached_shm_alloc(size, numa_node);
  size_t numa_node = ::mica::util::lcore.numa_id(lcore);
  if (numa_node == ::mica::util::lcore.kUnknown) {
    fprintf(stderr, "error: invalid lcore\n");
    return nullptr;
  }

  size_t entry_id = alloc(size, numa_node);
  // fprintf(stderr, "entry_id=%zu, size=%zu\n", entry_id, size);
  if (entry_id == kInvalidId) return nullptr;
  while (true) {
    void* p = HugeTLBFS_SHM::find_free_address(size);
    if (p == nullptr) {
      schedule_release(entry_id);
      return nullptr;
    }
    if (map(entry_id, p, 0, size)) {
      schedule_release(entry_id);
      return p;
    }
  }
}

void* HugeTLBFS_SHM::malloc_contiguous_local(size_t size) {
  size_t lcore = ::mica::util::lcore.lcore_id();

  return malloc_contiguous(size, lcore);
}

void* HugeTLBFS_SHM::malloc_contiguous_any(size_t size) {
  // size_t lcore = ::mica::util::lcore.lcore_id();
  size_t lcore = 0;

  // TODO: Use numa id instead of lcore id.
  for (size_t trial = 0; trial < ::mica::util::lcore.lcore_count(); trial++) {
    void* p;
    p = malloc_contiguous(size, lcore);
    if (!p) return p;
    if (++lcore == ::mica::util::lcore.lcore_count()) lcore = 0;
  }
  return nullptr;
}

void HugeTLBFS_SHM::free_contiguous(void* ptr) { unmap(ptr); }

void* HugeTLBFS_SHM::malloc_striped(size_t size) {
  // size_t numa_node = ::mica::util::lcore.numa_id();
  // if (numa_node == ::mica::util::lcore.kUnknown) {
  //   fprintf(stderr, "error: unable to detect current lcore\n");
  //   return nullptr;
  // }
  size_t numa_node = 0;

  size += 2 * kPageSize;  // need to store metadata

  size_t size_2 = (size + 1) / 2;
  size_2 = roundup(size_2);

  // TODO: allocate 1 fewer page (i.e., only 1 page in total) when the total
  // number of pages required is an odd numbered

  size_t entry_id[2];
  entry_id[0] = alloc(size_2, numa_node);
  if (entry_id[0] == kInvalidId) return nullptr;

  entry_id[1] = alloc(size_2, 1 - numa_node);
  if (entry_id[1] == kInvalidId) {
    schedule_release(entry_id[0]);
    return nullptr;
  }

  while (true) {
    void* base = find_free_address(size);
    if (base == nullptr) {
      schedule_release(entry_id[0]);
      schedule_release(entry_id[1]);
      return nullptr;
    }

    void* p = base;
    size_t numa_node = 0;
    size_t offset_in_stripe = 0;
    size_t mapped;
    for (mapped = 0; mapped < size; mapped += kPageSize) {
      if (!map(entry_id[numa_node], p, offset_in_stripe, kPageSize)) {
        // error
        break;
      }
      if (++numa_node == ::mica::util::lcore.numa_count()) numa_node = 0;
      if (numa_node == 0) offset_in_stripe += kPageSize;
      p = (void*)((size_t)p + kPageSize);
    }

    if (mapped < size) {
      // error
      p = base;
      size_t clean;
      for (clean = 0; clean < mapped; clean += kPageSize) {
        unmap(p);
        p = (void*)((size_t)p + kPageSize);
      }
    } else {
      // success
      schedule_release(entry_id[0]);
      schedule_release(entry_id[1]);

      *(uint64_t*)base = size;
      void* ptr = (void*)((size_t)base + 2 * kPageSize);
      return ptr;
    }
  }
}

void HugeTLBFS_SHM::free_striped(void* ptr) {
  void* base = (void*)((size_t)ptr - 2 * kPageSize);
  uint64_t size = *(uint64_t*)base;

  void* p = base;
  size_t mapped;
  for (mapped = 0; mapped < size; mapped += kPageSize) {
    unmap(p);
    p = (void*)((size_t)p + kPageSize);
  }
}
}
}

#endif
