// muduo/net/TimerQueueInterface.h
#ifndef MUDUO_NET_TIMERQUEUEINTERFACE_H
#define MUDUO_NET_TIMERQUEUEINTERFACE_H

#include "muduo/base/Timestamp.h"
#include "muduo/net/Callbacks.h"

namespace muduo {
namespace net {

class EventLoop;
class TimerId;

class TimerQueueInterface {
 public:
  virtual ~TimerQueueInterface() = default;

  virtual TimerId addTimer(TimerCallback cb, Timestamp when, double interval) = 0;
  virtual void cancel(TimerId timerId) = 0;

  static TimerQueueInterface* create(EventLoop* loop);
};

}  // namespace net
}  // namespace muduo

#endif