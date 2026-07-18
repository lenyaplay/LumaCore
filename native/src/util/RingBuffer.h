#pragma once

#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <optional>
#include <vector>

namespace lumacore::util {

// Bounded ring buffer with a drop-oldest push policy: when full, pushing a new
// item silently discards the oldest one instead of blocking the producer.
// See ARCHITECTURE.md §1 — shared by render->encode backpressure (Android/iOS)
// and the Windows IMFSourceReader single-in-flight throttle.
template <typename T>
class RingBuffer {
 public:
  explicit RingBuffer(std::size_t capacity) : capacity_(capacity) { items_.reserve(capacity); }

  // Returns true if an existing item was dropped to make room.
  bool pushDropOldest(T item) {
    std::lock_guard<std::mutex> lock(mutex_);
    bool dropped = false;
    if (items_.size() >= capacity_) {
      items_.erase(items_.begin());
      ++droppedCount_;
      dropped = true;
    }
    items_.push_back(std::move(item));
    notEmpty_.notify_one();
    return dropped;
  }

  std::optional<T> tryPop() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (items_.empty()) return std::nullopt;
    T front = std::move(items_.front());
    items_.erase(items_.begin());
    return front;
  }

  std::size_t size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return items_.size();
  }

  std::size_t capacity() const { return capacity_; }

  uint32_t droppedCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return droppedCount_;
  }

 private:
  std::size_t capacity_;
  mutable std::mutex mutex_;
  std::condition_variable notEmpty_;
  std::vector<T> items_;
  uint32_t droppedCount_ = 0;
};

}  // namespace lumacore::util
