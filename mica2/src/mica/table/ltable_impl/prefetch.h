#pragma once
#ifndef MICA_TABLE_LTABLE_IMPL_PREFETCH_H_
#define MICA_TABLE_LTABLE_IMPL_PREFETCH_H_

namespace mica {
namespace table {
template <class StaticConfig>
void LTable<StaticConfig>::prefetch_table(uint64_t key_hash) const {
  uint32_t bucket_index = calc_bucket_index(key_hash);
  const Bucket* bucket = buckets_ + bucket_index;

  // bucket address is already 64-byte aligned
  __builtin_prefetch(bucket, 0, 0);

  if (StaticConfig::kBucketSize > 7)
    __builtin_prefetch(reinterpret_cast<const char*>(bucket) + 64, 0, 0);

  // XXX: prefetch extra buckets, too?
}

template <class StaticConfig>
void LTable<StaticConfig>::prefetch_pool(uint64_t key_hash) const {
  uint32_t bucket_index = calc_bucket_index(key_hash);
  const Bucket* bucket = buckets_ + bucket_index;

  uint16_t tag = calc_tag(key_hash);

  size_t item_index;
  for (item_index = 0; item_index < StaticConfig::kBucketSize; item_index++) {
    uint64_t item_vec = bucket->item_vec[item_index];
    if (get_tag(item_vec) != tag) continue;

    pool_->prefetch_item(get_item_offset(item_vec));
  }
}
}
}

#endif