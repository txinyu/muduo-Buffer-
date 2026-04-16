// muduo/net/TimerQueueInterface.cc
#include "muduo/net/TimerQueueInterface.h"
#include "muduo/net/LegacyTimerQueueAdapter.h"
#include "muduo/net/WheelTimerQueue.h"
#include <cstdlib>
#include <cstring>

using namespace muduo::net;

TimerQueueInterface* TimerQueueInterface::create(EventLoop* loop) {
  const char* type = ::getenv("MUDUO_TIMER_TYPE");
  if (type && ::strcmp(type, "wheel") == 0) {
    return new WheelTimerQueue(loop);
  } else {
    return new LegacyTimerQueueAdapter(loop);
  }
}