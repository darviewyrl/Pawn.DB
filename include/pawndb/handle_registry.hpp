#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <typeindex>
#include <utility>
#include <vector>

namespace pawndb {

class HandleRegistry {
 public:
  using Handle = std::uint32_t;
  using Warning = std::function<void(Handle)>;

  explicit HandleRegistry(Warning warning = {}) : warning_(std::move(warning)) {}

  template <class T>
  Handle insert(std::shared_ptr<T> value) {
    if (!value) return 0;
    std::lock_guard lock(mutex_);
    std::uint32_t index;
    if (free_.empty()) {
      if (slots_.size() == kIndexMask) return 0;
      index = static_cast<std::uint32_t>(slots_.size());
      slots_.emplace_back();
    } else {
      index = free_.back();
      free_.pop_back();
    }
    auto& slot = slots_[index];
    slot.value = std::move(value);
    slot.type = typeid(T);
    return (slot.generation << kIndexBits) | (index + 1);
  }

  template <class T>
  std::shared_ptr<T> get(Handle handle) const {
    return get<T>(handle, true);
  }

  template <class T>
  std::shared_ptr<T> get(Handle handle, bool warn_invalid) const {
    std::shared_ptr<T> result;
    {
      std::lock_guard lock(mutex_);
      if (const auto* slot = valid(handle); slot && slot->type == typeid(T))
        result = std::static_pointer_cast<T>(slot->value);
    }
    if (!result && warn_invalid) warn(handle);
    return result;
  }

  template <class T>
  bool erase(Handle handle) {
    std::shared_ptr<void> removed;
    {
      std::lock_guard lock(mutex_);
      auto* slot = valid(handle);
      if (slot && slot->type == typeid(T)) {
        removed = std::move(slot->value);
        slot->type = typeid(void);
        const auto index = (handle & kIndexMask) - 1;
        // Retire a slot before generation wraps, so an old handle can never revive.
        if (++slot->generation < kGenerationLimit) free_.push_back(index);
      }
    }
    if (!removed) warn(handle);
    return static_cast<bool>(removed);
  }

 private:
  static constexpr std::uint32_t kIndexBits = 20;
  static constexpr std::uint32_t kIndexMask = (1u << kIndexBits) - 1;
  static constexpr std::uint32_t kGenerationLimit = 1u << (32 - kIndexBits);

  struct Slot {
    std::shared_ptr<void> value;
    std::type_index type = typeid(void);
    std::uint32_t generation = 1;
  };

  Slot* valid(Handle handle) const {
    const auto one_based = handle & kIndexMask;
    if (!one_based || one_based > slots_.size()) return nullptr;
    auto* slot = &slots_[one_based - 1];
    return slot->value && slot->generation == (handle >> kIndexBits) ? slot : nullptr;
  }

  void warn(Handle handle) const {
    if (warning_) warning_(handle);
  }

  mutable std::mutex mutex_;
  mutable std::vector<Slot> slots_;
  std::vector<std::uint32_t> free_;
  Warning warning_;
};

}  // namespace pawndb
