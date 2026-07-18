// Pure-logic unit test, no external framework — runs on any host CI without
// devices/toolchains (ARCHITECTURE.md §5). Each check prints PASS/FAIL and the
// process exits non-zero on any failure, which is all CTest needs.

#include <cstdio>
#include <cstdlib>

#include "util/RingBuffer.h"

using lumacore::util::RingBuffer;

namespace {

int g_failures = 0;

void check(bool condition, const char* description) {
  if (condition) {
    std::printf("PASS: %s\n", description);
  } else {
    std::printf("FAIL: %s\n", description);
    ++g_failures;
  }
}

void testPushPopInOrder() {
  RingBuffer<int> buffer(3);
  buffer.pushDropOldest(1);
  buffer.pushDropOldest(2);
  check(buffer.size() == 2, "size reflects pushed items below capacity");
  check(buffer.tryPop().value() == 1, "pop returns items in FIFO order (1)");
  check(buffer.tryPop().value() == 2, "pop returns items in FIFO order (2)");
  check(!buffer.tryPop().has_value(), "pop on empty buffer returns nullopt");
}

void testDropOldestWhenFull() {
  RingBuffer<int> buffer(2);
  check(!buffer.pushDropOldest(1), "push below capacity does not drop");
  check(!buffer.pushDropOldest(2), "push filling capacity does not drop");
  check(buffer.pushDropOldest(3), "push over capacity reports a drop");
  check(buffer.size() == 2, "size stays capped at capacity");
  check(buffer.tryPop().value() == 2, "oldest item (1) was dropped, 2 survives");
  check(buffer.tryPop().value() == 3, "newest item (3) survives");
  check(buffer.droppedCount() == 1, "droppedCount tracks total drops");
}

}  // namespace

int main() {
  testPushPopInOrder();
  testDropOldestWhenFull();

  if (g_failures == 0) {
    std::printf("All tests passed.\n");
    return EXIT_SUCCESS;
  }
  std::printf("%d test(s) failed.\n", g_failures);
  return EXIT_FAILURE;
}
