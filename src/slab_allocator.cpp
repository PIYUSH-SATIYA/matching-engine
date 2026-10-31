#include "slab_allocator.hpp"

#include <cstring>

namespace slab_allocator_detail {
namespace {

bool is_slot_address(const void* candidate, const void* pool,
                     std::size_t slot_size,
                     std::size_t capacity) noexcept {
  if (candidate == nullptr || pool == nullptr || slot_size == 0 ||
      capacity == 0) {
    return false;
  }

  const auto address = reinterpret_cast<std::uintptr_t>(candidate);
  const auto start = reinterpret_cast<std::uintptr_t>(pool);
  if (address < start) {
    return false;
  }

  const std::uintptr_t offset = address - start;
  return offset % slot_size == 0 && offset / slot_size < capacity;
}

} // namespace

void initialize_free_list(void* pool, std::size_t slot_size,
                          std::size_t capacity) noexcept {
  auto* bytes = static_cast<std::byte*>(pool);
  for (std::size_t index = 0; index < capacity; ++index) {
    void* next = index + 1 < capacity
                     ? static_cast<void*>(bytes + slot_size * (index + 1))
                     : nullptr;
    std::memcpy(bytes + slot_size * index, &next, sizeof(next));
  }
}

bool pop_free_slot(void*& head, const void* pool, std::size_t slot_size,
                   std::size_t capacity, void*& slot) noexcept {
  slot = nullptr;
  if (!is_slot_address(head, pool, slot_size, capacity)) {
    return false;
  }

  void* next = nullptr;
  std::memcpy(&next, head, sizeof(next));
  if (next != nullptr && !is_slot_address(next, pool, slot_size, capacity)) {
    return false;
  }

  slot = head;
  head = next;
  return true;
}

void push_free_slot(void*& head, void* slot) noexcept {
  if (slot == nullptr) {
    return;
  }

  std::memcpy(slot, &head, sizeof(head));
  head = slot;
}

} // namespace slab_allocator_detail
