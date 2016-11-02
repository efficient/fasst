#pragma once
#ifndef MICA_TABLE_LTABLE_IMPL_DEBUG_H_
#define MICA_TABLE_LTABLE_IMPL_DEBUG_H_

namespace mica {
namespace table {
template <class StaticConfig>
void LTable<StaticConfig>::print_bucket(const Bucket* bucket) const {
  printf("<bucket %zx>\n", (size_t)bucket);
  for (size_t item_index = 0; item_index < StaticConfig::kBucketSize;
       item_index++)
    printf("  item_vec[%zu]: tag=%hu, item_offset=%lu\n", item_index,
           get_tag(bucket->item_vec[item_index]),
           get_item_offset(bucket->item_vec[item_index]));
}

template <class StaticConfig>
void LTable<StaticConfig>::print_buckets() const {
  for (size_t bucket_index = 0; bucket_index < num_buckets_; bucket_index++) {
    Bucket* bucket = buckets_ + bucket_index;
    print_bucket(bucket);
  }
  printf("\n");
}

template <class StaticConfig>
void LTable<StaticConfig>::print_stats() const {
  if (StaticConfig::kCollectStats) {
    printf("count:                  %10zu\n", stats_.count);
    printf("set_nooverwrite:        %10zu | ", stats_.set_nooverwrite);
    printf("set_new:                %10zu | ", stats_.set_new);
    printf("set_inplace:            %10zu | ", stats_.set_inplace);
    printf("set_evicted:            %10zu\n", stats_.set_evicted);
    printf("get_found:              %10zu | ", stats_.get_found);
    printf("get_notfound:           %10zu\n", stats_.get_notfound);
    printf("test_found:             %10zu | ", stats_.test_found);
    printf("test_notfound:          %10zu\n", stats_.test_notfound);
    printf("delete_found:           %10zu | ", stats_.delete_found);
    printf("delete_notfound:        %10zu\n", stats_.delete_notfound);
    printf("cleanup:                %10zu\n", stats_.cleanup);
    printf("move_to_head_performed: %10zu | ", stats_.move_to_head_performed);
    printf("move_to_head_skipped:   %10zu | ", stats_.move_to_head_skipped);
    printf("move_to_head_failed:    %10zu\n", stats_.move_to_head_failed);
  }
}

template <class StaticConfig>
void LTable<StaticConfig>::reset_stats(bool reset_count) {
  if (StaticConfig::kCollectStats) {
    size_t count = stats_.count;
    ::mica::util::memset(&stats_, 0, sizeof(stats_));
    if (!reset_count) stats_.count = count;
  }
}

template <class StaticConfig>
void LTable<StaticConfig>::stat_inc(size_t Stats::*counter) const {
  if (StaticConfig::kCollectStats) __sync_add_and_fetch(&(stats_.*counter), 1);
}

template <class StaticConfig>
void LTable<StaticConfig>::stat_dec(size_t Stats::*counter) const {
  if (StaticConfig::kCollectStats) __sync_sub_and_fetch(&(stats_.*counter), 1);
}
}
}

#endif