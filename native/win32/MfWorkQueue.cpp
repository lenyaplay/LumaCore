#ifdef _WIN32

#include "MfWorkQueue.h"

namespace lumacore::win32 {

bool MfWorkQueue::initialize() {
  // TODO(Этап 10): MFAllocateWorkQueue(MF_STANDARD_WORKQUEUE, &queueId_).
  return false;
}

unsigned long MfWorkQueue::queueId() const { return queueId_; }

void MfWorkQueue::shutdown() {}

}  // namespace lumacore::win32

#endif  // _WIN32
