// muduo/net/WheelTimerQueue.h
#ifndef MUDUO_NET_WHEELTIMERQUEUE_H
#define MUDUO_NET_WHEELTIMERQUEUE_H

#include "muduo/net/TimerQueueInterface.h"
#include "muduo/net/Channel.h"
#include <vector>
#include <list>
#include <unordered_map>
#include <memory>

namespace muduo {
namespace net {

class EventLoop;
class Timer;
class TimerId;

class WheelTimerQueue : public TimerQueueInterface {
 public:
  explicit WheelTimerQueue(EventLoop* loop);
  ~WheelTimerQueue() override;

  TimerId addTimer(TimerCallback cb, Timestamp when, double interval) override;
  void cancel(TimerId timerId) override;

  // 内部使用，但不需要在接口中声明，因为 EventLoop 不会调用
  void handleRead();  // 供 Channel 回调

 private:
  static const int kTickMs = 1;
  static const int kFirstLevelBits = 10;
  static const int kFirstLevelSize = 1 << kFirstLevelBits;
  static const int kSecondLevelSize = 60;
  static const int kThirdLevelSize = 60;

  struct TimerLocation {
    int level;
    int slot;
    std::list<Timer*>::iterator it;
  };

  EventLoop* loop_;
  std::vector<std::list<Timer*>> firstLevel_;
  std::vector<std::list<Timer*>> secondLevel_;
  std::vector<std::list<Timer*>> thirdLevel_;
  int64_t currentTick_;
  std::unordered_map<Timer*, TimerLocation> timerLocation_;

  int timerfd_;
  std::unique_ptr<Channel> timerfdChannel_;

  int64_t getCurrentTick() const;
  void resetTimerfd();
  void addTimerInLoop(Timer* timer);
  void cascade(std::vector<std::list<Timer*>>& wheel, int slot, int unitTicks);
  void readTimerfd();
  struct timespec howMuchTimeFromNow(Timestamp when);
};

}  // namespace net
}  // namespace muduo

#endif