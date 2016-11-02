#pragma once
#ifndef MICA_TABLE_LTABLE_IMPL_DEL_H_
#define MICA_TABLE_LTABLE_IMPL_DEL_H_

namespace mica {
namespace table {
template <class StaticConfig>
Result LTable<StaticConfig>::del(uint64_t key_hash, const char* key,
                                 size_t key_length) {
  assert(key_length <= kMaxKeyLength);

  uint32_t bucket_index = calc_bucket_index(key_hash);
  uint16_t tag = calc_tag(key_hash);

  Bucket* bucket = buckets_ + bucket_index;

  lock_bucket(bucket);

  Bucket* located_bucket;
  size_t item_index =
      find_item_index(bucket, key_hash, tag, key, key_length, &located_bucket);
  if (item_index == StaticConfig::kBucketSize) {
    unlock_bucket(bucket);
    stat_inc(&Stats::delete_notfound);
    return Result::kNotFound;
  }

  pool_->release(get_item_offset(located_bucket->item_vec[item_index]));

  located_bucket->item_vec[item_index] = 0;
  stat_dec(&Stats::count);

  fill_hole(located_bucket, item_index);

  unlock_bucket(bucket);

  stat_inc(&Stats::delete_found);
  return Result::kSuccess;
}
}
}

#endif