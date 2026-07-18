#pragma once

#ifdef _WIN32

namespace lumacore::win32 {

// Dedicated Media Foundation work queue for IMFSourceReaderCallback::OnReadSample,
// requested explicitly via MFAllocateWorkQueue(MF_STANDARD_WORKQUEUE, ...) rather
// than the default queue. See ARCHITECTURE.md §1. Stub — implemented in Этап 10.
class MfWorkQueue {
 public:
  bool initialize();
  unsigned long queueId() const;
  void shutdown();

 private:
  unsigned long queueId_ = 0;
};

}  // namespace lumacore::win32

#endif  // _WIN32
