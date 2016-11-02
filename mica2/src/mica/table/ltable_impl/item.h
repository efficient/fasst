#pragma once
#ifndef MICA_TABLE_LTABLE_IMPL_ITEM_H_
#define MICA_TABLE_LTABLE_IMPL_ITEM_H_

namespace mica {
namespace table {
template <class StaticConfig>
uint32_t LTable<StaticConfig>::get_key_length(uint32_t kv_length_vec) {
  return kv_length_vec >> 24;
}

template <class StaticConfig>
uint32_t LTable<StaticConfig>::get_value_length(uint32_t kv_length_vec) {
  return kv_length_vec & Item::kValueMask;
}

template <class StaticConfig>
uint32_t LTable<StaticConfig>::make_kv_length_vec(uint32_t key_length,
                                                  uint32_t value_length) {
  return (key_length << 24) | value_length;
}

template <class StaticConfig>
uint16_t LTable<StaticConfig>::calc_tag(uint64_t key_hash) {
  uint16_t tag = (uint16_t)(key_hash & Bucket::kTagMask);
  if (tag == 0)
    return 1;
  else
    return tag;
}

template <class StaticConfig>
void LTable<StaticConfig>::set_item(Item* item, uint64_t key_hash,
                                    const char* key, uint32_t key_length,
                                    const char* value, uint32_t value_length) {
  assert(key_length <= Item::kKeyMask);
  assert(value_length <= Item::kValueMask);

  item->kv_length_vec = make_kv_length_vec(key_length, value_length);
  item->key_hash = key_hash;
  ::mica::util::memcpy<8>(item->data, key, key_length);
  ::mica::util::memcpy<8>(item->data + ::mica::util::roundup<8>(key_length),
                          value, value_length);
}

template <class StaticConfig>
void LTable<StaticConfig>::set_item_value(Item* item, const char* value,
                                          uint32_t value_length) {
  assert(value_length <= Item::kValueMask);

  uint32_t key_length = get_key_length(item->kv_length_vec);
  item->kv_length_vec = make_kv_length_vec(key_length, value_length);
  ::mica::util::memcpy<8>(item->data + ::mica::util::roundup<8>(key_length),
                          value, value_length);
}

template <class StaticConfig>
bool LTable<StaticConfig>::compare_keys(const char* key1, size_t key1_len,
                                        const char* key2, size_t key2_len) {
  return key1_len == key2_len &&
         ::mica::util::memcmp_equal<8>(key1, key2,
                                       ::mica::util::roundup<8>(key1_len));
}
}
}

#endif