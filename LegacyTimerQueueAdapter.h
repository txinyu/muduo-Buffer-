// muduo/net/LegacyTimerQueueAdapter.h
#ifndef MUDUO_NET_LEGACYTIMERQUEUEADAPTER_H
#define MUDUO_NET_LEGACYTIMERQUEUEADAPTER_H

#include "muduo/net/TimerQueueInterface.h"
#include <memory>

namespace muduo {
namespace net {

class TimerQueue;

class LegacyTimerQueueAdapter : public TimerQueueInterface {
 public:
  explicit LegacyTimerQueueAdapter(EventLoop* loop);
  ~LegacyTimerQueueAdapter() override;

  TimerId addTimer(TimerCallback cb, Timestamp when, double interval) override;
  void cancel(TimerId timerId) override;

 private:
  std::unique_ptr<TimerQueue> timerQueue_;
};

}  // namespace net
}  // namespace muduo

#endif