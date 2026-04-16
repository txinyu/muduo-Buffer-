// muduo/net/LegacyTimerQueueAdapter.cc
#include "muduo/net/LegacyTimerQueueAdapter.h"
#include "muduo/net/TimerQueue.h"
#include "muduo/net/EventLoop.h"

using namespace muduo::net;

LegacyTimerQueueAdapter::LegacyTimerQueueAdapter(EventLoop* loop)
  : timerQueue_(new TimerQueue(loop)) {
}

LegacyTimerQueueAdapter::~LegacyTimerQueueAdapter() = default;

TimerId LegacyTimerQueueAdapter::addTimer(TimerCallback cb, Timestamp when, double interval) {
  return timerQueue_->addTimer(std::move(cb), when, interval);
}

void LegacyTimerQueueAdapter::cancel(TimerId timerId) {
  timerQueue_->cancel(timerId);
}