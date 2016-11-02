#pragma once
#ifndef MICA_TABLE_LTABLE_IMPL_TEST_H_
#define MICA_TABLE_LTABLE_IMPL_TEST_H_

namespace mica {
namespace table {
template <class StaticConfig>
Result LTable<StaticConfig>::test(uint64_t key_hash, const char* key,
                                  size_t key_length) const {
  assert(key_length <= kMaxKeyLength);

  uint32_t bucket_index = calc_bucket_index(key_hash);
  uint16_t tag = calc_tag(key_hash);

  const Bucket* bucket = buckets_ + bucket_index;

  while (true) {
    uint32_t version_start = read_version_begin(bucket);

    const Bucket* located_bucket;
    size_t item_index = find_item_index(bucket, key_hash, tag, key, key_length,
                                        &located_bucket);

    if (version_start != read_version_end(bucket)) continue;

    if (item_index != StaticConfig::kBucketSize) {
      stat_inc(&Stats::test_found);
      return Result::kSuccess;
    } else {
      stat_inc(&Stats::test_notfound);
      return Result::kNotFound;
    }
  }
  // Not reachable.
}
}
}

#endif