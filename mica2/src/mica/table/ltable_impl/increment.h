#pragma once
#ifndef MICA_TABLE_LTABLE_IMPL_INCREMENT_H_
#define MICA_TABLE_LTABLE_IMPL_INCREMENT_H_

namespace mica {
namespace table {
template <class StaticConfig>
Result LTable<StaticConfig>::increment(uint64_t key_hash, const char* key,
                                       size_t key_length, uint64_t increment,
                                       uint64_t* out_value) {
  assert(key_length <= kMaxKeyLength);

  uint32_t bucket_index = calc_bucket_index(key_hash);
  uint16_t tag = calc_tag(key_hash);

  Bucket* bucket = buckets_ + bucket_index;

  // TODO: add stats

  lock_bucket(bucket);

  Bucket* located_bucket;
  size_t item_index =
      find_item_index(bucket, key_hash, tag, key, key_length, &located_bucket);

  if (item_index == StaticConfig::kBucketSize) {
    unlock_bucket(bucket);
    // TODO: support seeding a new item with the given default value?
    return Result::kNotFound;  // does not exist
  }

  uint64_t item_offset = get_item_offset(located_bucket->item_vec[item_index]);

  if (std::is_base_of<::mica::pool::CircularLogTag,
                      typename Pool::Tag>::value) {
    // ensure that the item is still valid
    pool_->lock();

    if (!Specialization::is_valid(pool_, item_offset)) {
      pool_->unlock();
      unlock_bucket(bucket);
      // TODO: support seeding a new item with the given default value?
      return Result::kError;  // exists in the table but not valid in the pool
    }
  }

  Item* item = reinterpret_cast<Item*>(pool_->get_item(item_offset));

  /* Commenting this out makes increment() work for non-8-byte values.
  size_t value_length = get_value_length(item->kv_length_vec);
  if (value_length != sizeof(uint64_t)) {
    if (std::is_base_of<::mica::pool::CircularLogTag,
                        typename Pool::Tag>::value)
      pool_->unlock();
    unlock_bucket(bucket);
    return Result::kError;  // invalid value size
  }
  */

  uint64_t old_value;
  ::mica::util::memcpy<8>(reinterpret_cast<uint8_t*>(&old_value),
                          item->data + ::mica::util::roundup<8>(key_length),
                          sizeof(old_value));

  uint64_t new_value = old_value + increment;
  ::mica::util::memcpy<8>(item->data + ::mica::util::roundup<8>(key_length),
                          reinterpret_cast<const uint8_t*>(&new_value),
                          sizeof(new_value));

  *out_value = new_value;
  // va(SimpleValueReader(reinterpret_cast<const char*>(&new_value),
  // sizeof(new_value));

  if (std::is_base_of<::mica::pool::CircularLogTag, typename Pool::Tag>::value)
    pool_->unlock();
  unlock_bucket(bucket);

  return Result::kSuccess;
}
}
}

#endif
