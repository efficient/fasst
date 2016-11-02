#pragma once
#ifndef MICA_TABLE_LTABLE_H_
#define MICA_TABLE_LTABLE_H_

#include <cstdio>
#include "mica/table/table.h"
#include "mica/pool/circular_log.h"
#include "mica/pool/segregated_fit.h"
#include "mica/util/config.h"
#include "mica/util/memcpy.h"
#include "mica/table/ltable_impl/specialization.h"

// Configuration file entries for LTable:
//
//  * item_count (integer): The approximate number of items to store in the
//    table.
//  * extra_collision_avoidance (float): The amount of additional memory to
//    resolve excessive hash collisions as a fraction of the main hash table.
//    Default = 0. if kEviction = true, 0.1 otherwise.
//  * concurrent_read (bool): If true, enable concurrent reads by multiple
//    threads.
//  * concurrent_write (bool): If true, enable concurrent writes by multiple
//    threads.
//  * numa_node (integer): The ID of the NUMA node to store the data.
//  * mth_threshold (double): The move-to-head threshold.  0.0 for full LRU, 1.0
//    for FIFO, and some value between 0.0 and 1.0 (exclusive) for approximate
//    LRU.  Ignored when kEviction = false.  Default = 0.5

namespace mica {
namespace table {
struct BasicLTableConfig {
  // Support concurrent access.  The actual concurrent access is enabled by
  // concurrent_read and concurrent_write in the configuration.
  static constexpr bool kConcurrent = true;

  // Be verbose.
  static constexpr bool kVerbose = false;

  // Collect fine-grained statistics accessible via print_stats() and
  // reset_stats().
  static constexpr bool kCollectStats = false;
};

struct BasicLossyLTableConfig : public BasicLTableConfig {
  // Enable automatic eviction.  This disables the use of the extra buckets to
  // resolve excessive collisions.
  static constexpr bool kEviction = true;

  // The number of elements in each bucket (associativity).
  // Supported: 7, 15, 31.
  static constexpr size_t kBucketSize = 15;

  // The item pool type.
  typedef ::mica::pool::CircularLog<> Pool;

  // The memory allocator type.
  typedef Pool::Alloc Alloc;
};

struct BasicLosslessLTableConfig : public BasicLTableConfig {
  // Enable automatic eviction.  This disables the use of the extra buckets to
  // resolve excessive collisions.
  static constexpr bool kEviction = false;

  // The number of elements in each bucket (associativity).
  // Supported: 7, 15, 31.
  static constexpr size_t kBucketSize = 7;

  // The item pool type.
  typedef ::mica::pool::SegregatedFit<> Pool;

  // The memory allocator type.
  typedef Pool::Alloc Alloc;
};

template <class StaticConfig = BasicLossyLTableConfig>
class LTable : public TableInterface {
 public:
  std::string name;	// Name of the table
  typedef typename StaticConfig::Alloc Alloc;
  typedef typename StaticConfig::Pool Pool;

  // static constexpr size_t kBucketSize = StaticConfig::kBucketSize;
  static constexpr size_t kMaxKeyLength = 255;
  static constexpr size_t kMaxValueLength = 1048575;

  // ltable_impl/init.h
  LTable(const ::mica::util::Config& config,
         int bkt_shm_key, Alloc* alloc, Pool* pool);
  ~LTable();

  void free_pool();
  void reset();

  // ltable_impl/del.h
  Result del(uint64_t key_hash, const char* key, size_t key_length);

  // ltable_impl/get.h
  Result get(uint64_t key_hash, const char* key, size_t key_length,
             char* out_value, size_t in_value_length, size_t* out_value_length,
             bool allow_mutation) const;

  // ltable_impl/increment.h
  Result increment(uint64_t key_hash, const char* key, size_t key_length,
                   uint64_t increment, uint64_t* out_value);

  // ltable_impl/set.h
  Result set(uint64_t key_hash, const char* key, size_t key_length,
             const char* value, size_t value_length, bool overwrite);

  // ltable_impl/test.h
  Result test(uint64_t key_hash, const char* key, size_t key_length) const;

  // ltable_impl/prefetch.h
  void prefetch_table(uint64_t key_hash) const;
  void prefetch_pool(uint64_t key_hash) const;

  // ltable_impl/info.h
  void print_buckets() const;
  void print_stats() const;
  void reset_stats(bool reset_count);

 private:
  typedef LTablePoolSpecialization<typename Pool::Tag> Specialization;

  struct Item {
    uint32_t kv_length_vec;  // key_length: 8, value_length: 24; kv_length_vec
                             // == 0: empty item

    // the rest is meaningful only when kv_length_vec != 0
    // uint32_t expire_time;
    uint32_t reserved0;
    uint64_t key_hash;
    char data[0];

    static constexpr uint32_t kKeyMask = (uint32_t(1) << 8) - 1;
    static constexpr uint32_t kValueMask = (uint32_t(1) << 24) - 1;
  };
  static_assert(sizeof(Item) == 16, "Invalid size for type Item");

  struct Bucket {
    uint32_t version;                  // XXX: is uint32_t wide enough?
    uint32_t next_extra_bucket_index;  // 1-base; 0 = no extra bucket
    uint64_t item_vec[StaticConfig::kBucketSize];

    // 16: tag (1-base)
    // 48: item offset
    // item == 0: empty item

    static constexpr uint64_t kTagMask = (uint64_t(1) << 16) - 1;
    static constexpr uint64_t kItemOffsetMask = (uint64_t(1) << 48) - 1;
  };
  static_assert((StaticConfig::kBucketSize == 7 && sizeof(Bucket) == 64) ||
                    (StaticConfig::kBucketSize == 15 &&
                     sizeof(Bucket) == 128) ||
                    (StaticConfig::kBucketSize == 31 && sizeof(Bucket) == 256),
                "Invalid size for type Bucket");

  struct ExtraBucketFreeList {
    uint8_t lock;
    uint32_t head;  // 1-base; 0 = no extra bucket
  };

  struct Stats {
    size_t count;
    size_t set_nooverwrite;
    size_t set_new;
    size_t set_inplace;
    size_t set_evicted;
    size_t get_found;
    size_t get_notfound;
    size_t test_found;
    size_t test_notfound;
    size_t delete_found;
    size_t delete_notfound;
    size_t cleanup;
    size_t move_to_head_performed;
    size_t move_to_head_skipped;
    size_t move_to_head_failed;
  };

  // ltable_impl/bucket.h
  static uint16_t get_tag(uint64_t item_vec);
  static uint64_t get_item_offset(uint64_t item_vec);
  static uint64_t make_item_vec(uint16_t tag, uint64_t item_offset);
  uint32_t calc_bucket_index(uint64_t key_hash) const;
  static bool has_extra_bucket(const Bucket* bucket);
  const Bucket* get_extra_bucket(uint32_t extra_bucket_index) const;
  Bucket* get_extra_bucket(uint32_t extra_bucket_index);
  bool alloc_extra_bucket(Bucket* bucket);
  void free_extra_bucket(Bucket* bucket);
  void fill_hole(Bucket* bucket, size_t unused_item_index);
  size_t get_empty(Bucket* bucket, Bucket** located_bucket);
  size_t get_empty_or_oldest(Bucket* bucket, Bucket** located_bucket);
  size_t find_item_index(const Bucket* bucket, uint64_t key_hash, uint16_t tag,
                         const char* key, size_t key_length,
                         const Bucket** located_bucket) const;
  size_t find_item_index(Bucket* bucket, uint64_t key_hash, uint16_t tag,
                         const char* key, size_t key_length,
                         Bucket** located_bucket);
  size_t find_same_tag(const Bucket* bucket, uint16_t tag,
                       const Bucket** located_bucket) const;
  size_t find_same_tag(Bucket* bucket, uint16_t tag, Bucket** located_bucket);
  void cleanup_bucket(uint64_t old_tail, uint64_t new_tail);
  void cleanup_all();

  // ltable_impl/info.h
  void print_bucket(const Bucket* bucket) const;
  void stat_inc(size_t Stats::*counter) const;
  void stat_dec(size_t Stats::*counter) const;

  // ltable_impl/item.h
  static uint32_t get_key_length(uint32_t kv_length_vec);
  static uint32_t get_value_length(uint32_t kv_length_vec);
  static uint32_t make_kv_length_vec(uint32_t key_length,
                                     uint32_t value_length);
  static uint16_t calc_tag(uint64_t key_hash);
  static void set_item(Item* item, uint64_t key_hash, const char* key,
                       uint32_t key_length, const char* value,
                       uint32_t value_length);
  static void set_item_value(Item* item, const char* value,
                             uint32_t value_length);
  static bool compare_keys(const char* key1, size_t key1_len, const char* key2,
                           size_t key2_len);

  // ltable_impl/move_to_head.h
  void move_to_head(Bucket* bucket, Bucket* located_bucket, const Item* item,
                    size_t key_length, size_t value_length, size_t item_index,
                    uint64_t item_vec, uint64_t item_offset);

  // ltable_impl/lock.h
  void lock_bucket(Bucket* bucket);
  void unlock_bucket(Bucket* bucket);
  void lock_extra_bucket_free_list();
  void unlock_extra_bucket_free_list();
  uint32_t read_version_begin(const Bucket* bucket) const;
  uint32_t read_version_end(const Bucket* bucket) const;

  ::mica::util::Config config_;
  int bkt_shm_key;	// User-defined SHM key used for bucket memory
  Alloc* alloc_;
  Pool* pool_;

  Bucket* buckets_;
  Bucket* extra_buckets_;  // = (buckets + num_buckets); extra_buckets[0] is
                           // not used because index 0 indicates "no more
                           // extra bucket"

  uint8_t concurrent_access_mode_;
  uint8_t rshift_;

  uint32_t num_buckets_;
  uint32_t num_buckets_mask_;
  uint32_t num_extra_buckets_;

  uint64_t mth_threshold_;

  // Padding to separate static and dynamic fields.
  char padding0[128];

  ExtraBucketFreeList extra_bucket_free_list_;

  mutable Stats stats_;
} __attribute__((aligned(128)));  // To prevent false sharing caused by
                                  // adjacent cacheline prefetching.
}
}

#include "mica/table/ltable_impl/bucket.h"
#include "mica/table/ltable_impl/del.h"
#include "mica/table/ltable_impl/get.h"
#include "mica/table/ltable_impl/increment.h"
#include "mica/table/ltable_impl/info.h"
#include "mica/table/ltable_impl/init.h"
#include "mica/table/ltable_impl/item.h"
#include "mica/table/ltable_impl/lock.h"
#include "mica/table/ltable_impl/move_to_head.h"
#include "mica/table/ltable_impl/prefetch.h"
#include "mica/table/ltable_impl/set.h"
#include "mica/table/ltable_impl/test.h"

#endif
