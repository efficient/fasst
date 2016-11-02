#pragma once
#ifndef MICA_TABLE_LTABLE_IMPL_SPECIALIZATION_H_
#define MICA_TABLE_LTABLE_IMPL_SPECIALIZATION_H_

namespace mica {
namespace table {
template <class PoolTag>
class LTablePoolSpecialization {
 public:
  template <class Pool>
  static uint64_t get_tail(const Pool* pool);

  template <class Pool>
  static uint64_t get_mask(const Pool* pool);

  template <class Pool>
  static uint64_t get_size(const Pool* pool);

  template <class Pool>
  static bool is_valid(const Pool* pool, typename Pool::Offset offset);
};

template <class PoolTag>
template <class Pool>
uint64_t LTablePoolSpecialization<PoolTag>::get_tail(const Pool* pool) {
  (void)pool;
  return 0;
}

template <>
template <class Pool>
uint64_t LTablePoolSpecialization<::mica::pool::CircularLogTag>::get_tail(
    const Pool* pool) {
  return pool->get_tail();
}

template <class PoolTag>
template <class Pool>
uint64_t LTablePoolSpecialization<PoolTag>::get_mask(const Pool* pool) {
  (void)pool;
  return 0;
}

template <>
template <class Pool>
uint64_t LTablePoolSpecialization<::mica::pool::CircularLogTag>::get_mask(
    const Pool* pool) {
  return pool->get_mask();
}

template <class PoolTag>
template <class Pool>
uint64_t LTablePoolSpecialization<PoolTag>::get_size(const Pool* pool) {
  (void)pool;
  return 0;
}

template <>
template <class Pool>
uint64_t LTablePoolSpecialization<::mica::pool::CircularLogTag>::get_size(
    const Pool* pool) {
  return pool->get_size();
}

template <class PoolTag>
template <class Pool>
bool LTablePoolSpecialization<PoolTag>::is_valid(const Pool* pool,
                                                 typename Pool::Offset offset) {
  (void)pool;
  (void)offset;
  return true;
}

template <>
template <class Pool>
bool LTablePoolSpecialization<::mica::pool::CircularLogTag>::is_valid(
    const Pool* pool, typename Pool::Offset offset) {
  return pool->is_valid(offset);
}
}
}

#endif
