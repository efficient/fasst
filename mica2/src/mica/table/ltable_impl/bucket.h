#pragma once
#ifndef MICA_TABLE_LTABLE_IMPL_BUCKET_H_
#define MICA_TABLE_LTABLE_IMPL_BUCKET_H_

namespace mica {
namespace table {
template <class StaticConfig>
uint16_t LTable<StaticConfig>::get_tag(uint64_t item_vec) {
  return static_cast<uint16_t>(item_vec >> 48);
}

template <class StaticConfig>
uint64_t LTable<StaticConfig>::get_item_offset(uint64_t item_vec) {
  return item_vec & Bucket::kItemOffsetMask;
}

template <class StaticConfig>
uint64_t LTable<StaticConfig>::make_item_vec(uint16_t tag,
                                             uint64_t item_offset) {
  return ((static_cast<uint64_t>(tag) << 48) | item_offset);
}

template <class StaticConfig>
uint32_t LTable<StaticConfig>::calc_bucket_index(uint64_t key_hash) const {
  // 16 is from Bucket::kTagMask's log length
  return static_cast<uint32_t>(key_hash >> 16) & num_buckets_mask_;
}

template <class StaticConfig>
bool LTable<StaticConfig>::has_extra_bucket(const Bucket* bucket) {
  if (StaticConfig::kEviction)
    return false;
  else
    return bucket->next_extra_bucket_index != 0;
}

template <class StaticConfig>
const typename LTable<StaticConfig>::Bucket*
LTable<StaticConfig>::get_extra_bucket(uint32_t extra_bucket_index) const {
  // extra_bucket_index is 1-base
  assert(extra_bucket_index != 0);
  assert(extra_bucket_index < 1 + num_extra_buckets_);
  return extra_buckets_ +
         extra_bucket_index;  // extra_buckets[1] is the actual start
}

template <class StaticConfig>
typename LTable<StaticConfig>::Bucket* LTable<StaticConfig>::get_extra_bucket(
    uint32_t extra_bucket_index) {
  assert(extra_bucket_index != 0);
  assert(extra_bucket_index < 1 + num_extra_buckets_);
  return extra_buckets_ +
         extra_bucket_index;  // extra_buckets[1] is the actual start
}

template <class StaticConfig>
bool LTable<StaticConfig>::alloc_extra_bucket(Bucket* bucket) {
  assert(!has_extra_bucket(bucket));

  lock_extra_bucket_free_list();

  if (extra_bucket_free_list_.head == 0) {
    unlock_extra_bucket_free_list();
    return false;
  }

  // take the first free extra bucket
  uint32_t extra_bucket_index = extra_bucket_free_list_.head;
  extra_bucket_free_list_.head =
      get_extra_bucket(extra_bucket_index)->next_extra_bucket_index;
  get_extra_bucket(extra_bucket_index)->next_extra_bucket_index = 0;

  // add it to the given bucket
  // concurrent readers may see the new extra_bucket from this point
  bucket->next_extra_bucket_index = extra_bucket_index;

  unlock_extra_bucket_free_list();
  return true;
}

template <class StaticConfig>
void LTable<StaticConfig>::free_extra_bucket(Bucket* bucket) {
  assert(has_extra_bucket(bucket));

  uint32_t extra_bucket_index = bucket->next_extra_bucket_index;

  Bucket* extra_bucket = get_extra_bucket(extra_bucket_index);
  assert(extra_bucket->next_extra_bucket_index ==
         0);  // only allows freeing the tail of the extra bucket chain

  // verify if the extra bucket is empty (debug only)
  for (size_t item_index = 0; item_index < StaticConfig::kBucketSize;
       item_index++)
    assert(extra_bucket->item_vec[item_index] == 0);

  // detach
  bucket->next_extra_bucket_index = 0;

  // add freed extra bucket to the free list
  lock_extra_bucket_free_list();

  extra_bucket->next_extra_bucket_index = extra_bucket_free_list_.head;
  extra_bucket_free_list_.head = extra_bucket_index;

  unlock_extra_bucket_free_list();
}

template <class StaticConfig>
void LTable<StaticConfig>::fill_hole(Bucket* bucket, size_t unused_item_index) {
  while (true) {
    // there is no extra bucket; do not try to fill a hole within the same
    // bucket
    if (!has_extra_bucket(bucket)) return;

    Bucket* prev_extra_bucket = nullptr;
    Bucket* current_extra_bucket = bucket;
    do {
      prev_extra_bucket = current_extra_bucket;
      current_extra_bucket =
          get_extra_bucket(current_extra_bucket->next_extra_bucket_index);
    } while (has_extra_bucket(current_extra_bucket) != 0);

    bool last_item = true;
    size_t moved_item_index = StaticConfig::kBucketSize;

    for (size_t item_index = 0; item_index < StaticConfig::kBucketSize;
         item_index++)
      if (current_extra_bucket->item_vec[item_index] != 0) {
        if (moved_item_index == StaticConfig::kBucketSize)
          moved_item_index = item_index;
        else {
          last_item = false;
          break;
        }
      }

    if (moved_item_index == StaticConfig::kBucketSize) {
      // The last extra bucket in the chain was already empty.  This happens if
      // one of the previous delete operation has removed the last item from the
      // last bucket; fill_hole() does not remove such an empty extra bucket
      // because it does not know the previous bucket (which could be not even
      // an extra bucket) in the chain.  We release this extra bucket from the
      // chain and restart.
      free_extra_bucket(prev_extra_bucket);
      continue;
    }

    // move the entry
    bucket->item_vec[unused_item_index] =
        current_extra_bucket->item_vec[moved_item_index];
    current_extra_bucket->item_vec[moved_item_index] = 0;

    // if it was the last entry of current_extra_bucket, free
    // current_extra_bucket
    if (last_item) free_extra_bucket(prev_extra_bucket);
    break;
  }
}

template <class StaticConfig>
size_t LTable<StaticConfig>::get_empty(Bucket* bucket,
                                       Bucket** located_bucket) {
  Bucket* current_bucket = bucket;
  while (true) {
    for (size_t item_index = 0; item_index < StaticConfig::kBucketSize;
         item_index++) {
      if (current_bucket->item_vec[item_index] == 0) {
        *located_bucket = current_bucket;
        return item_index;
      }
    }
    if (!has_extra_bucket(current_bucket)) break;
    current_bucket = get_extra_bucket(current_bucket->next_extra_bucket_index);
  }

  // no space; alloc new extra_bucket
  if (alloc_extra_bucket(current_bucket)) {
    *located_bucket = get_extra_bucket(current_bucket->next_extra_bucket_index);
    return 0;  // use the first slot (it should be empty)
  } else {
    // no free extra_bucket
    *located_bucket = nullptr;
    return StaticConfig::kBucketSize;
  }
}

template <class StaticConfig>
size_t LTable<StaticConfig>::get_empty_or_oldest(Bucket* bucket,
                                                 Bucket** located_bucket) {
  // We do not know the age of items except with CircularLog.
  if (!std::is_base_of<::mica::pool::CircularLogTag,
                       typename Pool::Tag>::value) {
    assert(false);
    return get_empty(bucket, located_bucket);
  }

  size_t oldest_item_index = 0;
  uint64_t oldest_item_distance = (uint64_t)-1;
  Bucket* oldest_item_bucket = nullptr;

  Bucket* current_bucket = bucket;
  while (true) {
    for (size_t item_index = 0; item_index < StaticConfig::kBucketSize;
         item_index++) {
      uint64_t item_vec = current_bucket->item_vec[item_index];
      if (item_vec == 0) {
        *located_bucket = current_bucket;
        return item_index;
      }
      uint64_t distance =
          (Specialization::get_tail(pool_) - get_item_offset(item_vec)) &
          Specialization::get_mask(pool_);
      if (oldest_item_distance > distance) {
        oldest_item_distance = distance;
        oldest_item_index = item_index;
        oldest_item_bucket = current_bucket;
      }
    }
    if (!has_extra_bucket(current_bucket)) break;
    current_bucket = get_extra_bucket(current_bucket->next_extra_bucket_index);
  }

  *located_bucket = oldest_item_bucket;
  return oldest_item_index;
}

template <class StaticConfig>
size_t LTable<StaticConfig>::find_item_index(
    const Bucket* bucket, uint64_t key_hash, uint16_t tag, const char* key,
    size_t key_length, const Bucket** located_bucket) const {
  const Bucket* current_bucket = bucket;

  while (true) {
    for (size_t item_index = 0; item_index < StaticConfig::kBucketSize;
         item_index++) {
      uint64_t item_vec = current_bucket->item_vec[item_index];
      if (get_tag(item_vec) != tag) continue;

      // we may read garbage values, which do not cause any fatal issue
      const Item* item = reinterpret_cast<const Item*>(
          pool_->get_item(get_item_offset(item_vec)));

      // printf("item->key_hash %lu\n", item->key_hash);
      // printf("key_hash %lu\n", key_hash);

      if (item->key_hash != key_hash) continue;

      // printf("item->key_length %u\n", get_key_length(item->kv_length_vec));
      // printf("key_length %u\n", key_length);

      // printf("item->data %lu\n",
      //        *reinterpret_cast<const uint64_t*>(item->data));
      // printf("key %lu\n", *reinterpret_cast<const uint64_t*>(key));

      // a key comparison reads up to min(source key length and destination
      // key length), which is always safe to do
      if (!compare_keys(item->data, get_key_length(item->kv_length_vec), key,
                        key_length))
        continue;

      // we skip any validity check because it will be done by callers who are
      // doing
      // more jobs with this result

      if (StaticConfig::kVerbose) printf("find item index: %zu\n", item_index);
      *located_bucket = current_bucket;
      return item_index;
    }

    if (!has_extra_bucket(current_bucket)) break;
    current_bucket = get_extra_bucket(current_bucket->next_extra_bucket_index);
  }

  if (StaticConfig::kVerbose) printf("could not find item index\n");

  *located_bucket = nullptr;
  return StaticConfig::kBucketSize;
}

template <class StaticConfig>
size_t LTable<StaticConfig>::find_item_index(Bucket* bucket, uint64_t key_hash,
                                             uint16_t tag, const char* key,
                                             size_t key_length,
                                             Bucket** located_bucket) {
  return find_item_index(const_cast<const Bucket*>(bucket), key_hash, tag, key,
                         key_length,
                         const_cast<const Bucket**>(located_bucket));
}

template <class StaticConfig>
size_t LTable<StaticConfig>::find_same_tag(
    const Bucket* bucket, uint16_t tag, const Bucket** located_bucket) const {
  const Bucket* current_bucket = bucket;

  while (true) {
    for (size_t item_index = 0; item_index < StaticConfig::kBucketSize;
         item_index++) {
      uint64_t item_vec = current_bucket->item_vec[item_index];
      if (get_tag(item_vec) != tag) continue;

      *located_bucket = current_bucket;
      return item_index;
    }

    if (!has_extra_bucket(current_bucket)) break;
    current_bucket = get_extra_bucket(current_bucket->next_extra_bucket_index);
  }

  *located_bucket = nullptr;
  return StaticConfig::kBucketSize;
}

template <class StaticConfig>
size_t LTable<StaticConfig>::find_same_tag(Bucket* bucket, uint16_t tag,
                                           Bucket** located_bucket) {
  return find_same_tag(const_cast<const Bucket*>(bucket), tag,
                       const_cast<const Bucket**>(located_bucket));
}

template <class StaticConfig>
void LTable<StaticConfig>::cleanup_bucket(uint64_t old_tail,
                                          uint64_t new_tail) {
  if (!std::is_base_of<::mica::pool::CircularLogTag,
                       typename Pool::Tag>::value) {
    assert(false);
    return;
  }

  // XXX: in principal, we should use old/new head for cleanup instead of
  // tails because head changes are what invalidates locations.
  // however, using tails does the same job as using heads by large because
  // tail change >= head change.

  // tails are using 64-bit numbers, but it is OK because we apply
  // table->num_buckets_mask mask
  uint64_t bucket_index = (old_tail >> rshift_) & num_buckets_mask_;
  uint64_t bucket_index_end = (new_tail >> rshift_) & num_buckets_mask_;

  while (bucket_index != bucket_index_end) {
    if (StaticConfig::kVerbose) printf("cleanup bucket: %lu\n", bucket_index);
    Bucket* bucket = buckets_ + bucket_index;

    lock_bucket(bucket);

    Bucket* current_bucket = bucket;
    while (true) {
      size_t item_index;
      for (item_index = 0; item_index < StaticConfig::kBucketSize;
           item_index++) {
        uint64_t* item_vec_p = &current_bucket->item_vec[item_index];
        if (*item_vec_p == 0) continue;

        if (!Specialization::is_valid(pool_, *item_vec_p)) {
          *item_vec_p = 0;
          stat_inc(&Stats::cleanup);
          stat_dec(&Stats::count);
        }
      }

      if (!has_extra_bucket(current_bucket)) break;
      current_bucket =
          get_extra_bucket(current_bucket->next_extra_bucket_index);
    }

    unlock_bucket(bucket);
    bucket_index = (bucket_index + 1UL) & num_buckets_mask_;
  }
}

template <class StaticConfig>
void LTable<StaticConfig>::cleanup_all() {
  if (std::is_base_of<::mica::pool::CircularLogTag,
                      typename Pool::Tag>::value) {
    cleanup_bucket(0, (uint64_t)1 << rshift_);
    cleanup_bucket((uint64_t)1 << rshift_, 0);
  }
}
}
}

#endif