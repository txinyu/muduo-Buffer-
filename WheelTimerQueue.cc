// muduo/net/WheelTimerQueue.cc
// 三级时间轮定时器实现文件
// 三层：毫秒轮(1024槽) → 秒轮(60槽) → 分轮(60槽)
#include "muduo/net/WheelTimerQueue.h"
#include "muduo/net/EventLoop.h"
#include "muduo/net/Timer.h"
#include "muduo/net/TimerId.h"
#include "muduo/base/Logging.h"
#include <sys/timerfd.h>
#include <unistd.h>
#include <string.h>

using namespace muduo;
using namespace muduo::net;

namespace
{
    // 创建 Linux 定时器文件描述符 timerfd
    // 非阻塞 + 执行关闭，用于驱动时间轮前进
    int createTimerfd()
    {
        int fd = ::timerfd_create(CLOCK_REALTIME, TFD_NONBLOCK | TFD_CLOEXEC);
        if (fd < 0)
            LOG_SYSFATAL << "Failed in timerfd_create";
        return fd;
    }
} // namespace

// 构造函数：初始化三层时间轮、timerfd、channel
WheelTimerQueue::WheelTimerQueue(EventLoop *loop)
    : loop_(loop),                        // 绑定事件循环
      firstLevel_(kFirstLevelSize),       // 第一层：毫秒轮，固定大小 1024
      secondLevel_(kSecondLevelSize),     // 第二层：秒轮，固定大小 60
      thirdLevel_(kThirdLevelSize),        // 第三层：分轮，固定大小 60
      currentTick_(getCurrentTick()),      // 当前时间 tick（唯一全局指针）
      timerfd_(createTimerfd()),           // 创建定时器 fd
      timerfdChannel_(new Channel(loop, timerfd_)) // 用 Channel 监听 timerfd 事件
{
    // 绑定超时回调：时间到了就调用 handleRead
    timerfdChannel_->setReadCallback(std::bind(&WheelTimerQueue::handleRead, this));
    timerfdChannel_->enableReading(); // 开始监听读事件
    resetTimerfd();                   // 启动定时器，设置下一次触发时间
}

// 析构函数：关闭 fd，释放所有定时器对象
WheelTimerQueue::~WheelTimerQueue()
{
    ::close(timerfd_); // 关闭 timerfd

    // 遍历三层时间轮，释放所有 new 出来的 Timer
    for (auto &bucket : firstLevel_)
        for (auto timer : bucket)
            delete timer;
    for (auto &bucket : secondLevel_)
        for (auto timer : bucket)
            delete timer;
    for (auto &bucket : thirdLevel_)
        for (auto timer : bucket)
            delete timer;
}

// 获取当前时间，单位：毫秒 tick（1ms = 1tick）
int64_t WheelTimerQueue::getCurrentTick() const
{
    return Timestamp::now().microSecondsSinceEpoch() / 1000;
}

// 计算一个时间点距离现在还有多久，返回 timespec（给 timerfd_settime 用）
struct timespec WheelTimerQueue::howMuchTimeFromNow(Timestamp when)
{
    int64_t us = when.microSecondsSinceEpoch() - Timestamp::now().microSecondsSinceEpoch();
    if (us < 100)  // 最小 100us，避免时间太短出问题
        us = 100;
    struct timespec ts;
    ts.tv_sec = static_cast<time_t>(us / Timestamp::kMicroSecondsPerSecond);
    ts.tv_nsec = static_cast<long>((us % Timestamp::kMicroSecondsPerSecond) * 1000);
    return ts;
}

// 重新设置 timerfd，让它在 kTickMs(1ms) 后触发一次
void WheelTimerQueue::resetTimerfd()
{
    Timestamp next = addTime(Timestamp::now(), kTickMs / 1000.0);
    struct itimerspec newVal;
    newVal.it_value = howMuchTimeFromNow(next);
    newVal.it_interval = {0, 0};  // 只触发一次，不是周期触发
    if (::timerfd_settime(timerfd_, 0, &newVal, NULL) < 0)
        LOG_SYSERR << "timerfd_settime";
}

// 读取 timerfd，必须读，否则会一直触发事件
void WheelTimerQueue::readTimerfd()
{
    uint64_t howmany;
    ssize_t n = ::read(timerfd_, &howmany, sizeof howmany);
    if (n != sizeof howmany)
        LOG_ERROR << "readTimerfd reads " << n << " bytes instead of 8";
}

// 外部接口：添加一个定时器
TimerId WheelTimerQueue::addTimer(TimerCallback cb, Timestamp when, double interval)
{
    Timer *timer = new Timer(std::move(cb), when, interval);
    // 跨线程安全：必须在 IO 线程执行添加操作
    loop_->runInLoop(std::bind(&WheelTimerQueue::addTimerInLoop, this, timer));
    return TimerId(timer, timer->sequence());
}

// 【核心】在 IO 线程中真正添加定时器
// 根据延迟时间，把任务放到 毫秒轮/秒轮/分轮
void WheelTimerQueue::addTimerInLoop(Timer *timer)
{
    loop_->assertInLoopThread();
    int64_t nowTicks = getCurrentTick();
    int64_t expireTicks = timer->expiration().microSecondsSinceEpoch() / 1000;
    int64_t delay = expireTicks - nowTicks; // 计算还有多少毫秒到期
    if (delay < 0)
        delay = 0;

    // ==============================
    // 三层时间轮分配逻辑
    // ==============================

    // 1. 延迟 < 1024ms → 放到第一层（毫秒轮）
    if (delay < kFirstLevelSize)
    {
        // 计算槽位：当前时间 + 延迟 → 取模循环（不扩容）
        int slot = static_cast<int>((currentTick_ + delay) % kFirstLevelSize);
        firstLevel_[slot].push_back(timer);
        // 记录位置，用于快速删除 O(1)
        timerLocation_[timer] = {1, slot, --firstLevel_[slot].end()};
    }
    // 2. 延迟 < 60s → 放到第二层（秒轮）
    else if (delay < kSecondLevelSize * kFirstLevelSize)
    {
        int64_t sec = delay / kFirstLevelSize;
        int slot = static_cast<int>((currentTick_ / kFirstLevelSize + sec) % kSecondLevelSize);
        secondLevel_[slot].push_back(timer);
        timerLocation_[timer] = {2, slot, --secondLevel_[slot].end()};
    }
    // 3. 延迟 < 60min → 放到第三层（分轮）
    else if (delay < kThirdLevelSize * kSecondLevelSize * kFirstLevelSize)
    {
        int64_t min = delay / (kFirstLevelSize * kSecondLevelSize);
        int slot = static_cast<int>((currentTick_ / (kFirstLevelSize * kSecondLevelSize) + min) % kThirdLevelSize);
        thirdLevel_[slot].push_back(timer);
        timerLocation_[timer] = {3, slot, --thirdLevel_[slot].end()};
    }
    // 4. 超过 60min，直接放到分轮最后一个槽
    else
    {
        int slot = static_cast<int>((currentTick_ / (kFirstLevelSize * kSecondLevelSize) + kThirdLevelSize - 1) % kThirdLevelSize);
        thirdLevel_[slot].push_back(timer);
        timerLocation_[timer] = {3, slot, --thirdLevel_[slot].end()};
    }

    resetTimerfd(); // 重新启动下一次 tick
}

// 取消定时器（外部接口）
void WheelTimerQueue::cancel(TimerId timerId)
{
    loop_->runInLoop([this, timerId]
                     {
    Timer* timer = timerId.timer_;
    auto it = timerLocation_.find(timer);
    if (it != timerLocation_.end()) {
      const auto& loc = it->second;
      // 根据层级 + 槽位 + 迭代器，直接删除节点 O(1)
      switch (loc.level) {
        case 1: firstLevel_[loc.slot].erase(loc.it); break;
        case 2: secondLevel_[loc.slot].erase(loc.it); break;
        case 3: thirdLevel_[loc.slot].erase(loc.it); break;
      }
      timerLocation_.erase(it); // 从哈希表删除映射
      delete timer;             // 释放定时器对象
    } });
}

// ==============================
// 【最核心函数】时间轮驱动函数
// timerfd 触发 → 时间前进 → 执行任务 → 降级
// ==============================
void WheelTimerQueue::handleRead()
{
    loop_->assertInLoopThread();
    readTimerfd(); // 必须读，清空事件

    int64_t nowTicks = getCurrentTick(); // 获取当前最新时间

    // 时间一步一步往前走，直到追上当前时间
    while (currentTick_ < nowTicks)
    {
        // 1. 取第一层（毫秒轮）当前槽
        int slot = static_cast<int>(currentTick_ % kFirstLevelSize);
        auto &bucket = firstLevel_[slot];

        // 2. 执行当前槽所有任务
        for (auto it = bucket.begin(); it != bucket.end();)
        {
            Timer *timer = *it;
            it = bucket.erase(it);       // 先从槽里移除
            timerLocation_.erase(timer); // 删除位置映射

            timer->run(); // 执行任务回调

            // 如果是重复任务，重启并重新加入时间轮
            if (timer->repeat())
            {
                timer->restart(Timestamp::now());
                addTimerInLoop(timer);
            }
            else // 一次性任务，直接释放
            {
                delete timer;
            }
        }

        // ==============================
        // 3. 级联降级（关键！）
        // ==============================

        // 毫秒轮转完一圈（1024ms）→ 秒轮转一格
        if ((currentTick_ + 1) % kFirstLevelSize == 0)
        {
            int secondSlot = static_cast<int>((currentTick_ / kFirstLevelSize) % kSecondLevelSize);
            cascade(secondLevel_, secondSlot, kFirstLevelSize);
        }

        // 秒轮转完一圈（60s）→ 分轮转一格
        if ((currentTick_ + 1) % (kFirstLevelSize * kSecondLevelSize) == 0)
        {
            int thirdSlot = static_cast<int>((currentTick_ / (kFirstLevelSize * kSecondLevelSize)) % kThirdLevelSize);
            cascade(thirdLevel_, thirdSlot, kFirstLevelSize * kSecondLevelSize);
        }

        // 时间前进 1ms
        ++currentTick_;
    }

    resetTimerfd(); // 设置下一次时间触发
}

// ==============================
// 【级联降级函数】高层任务 → 下层
// 秒轮的任务 → 降级到毫秒轮
// 分轮的任务 → 降级到秒轮/毫秒轮
// ==============================
void WheelTimerQueue::cascade(std::vector<std::list<Timer *>> &wheel, int slot, int unitTicks)
{
    // 取出当前要降级的槽里所有任务
    auto &bucket = wheel[slot];

    // 遍历每个任务，重新分配到合适的层级
    for (Timer *timer : bucket)
    {
        // 计算任务剩余时间
        int64_t expireTicks = timer->expiration().microSecondsSinceEpoch() / 1000;
        int64_t remain = expireTicks - currentTick_;

        // 任务已到期，直接执行
        if (remain <= 0)
        {
            addTimerInLoop(timer);
        }
        // 剩余时间 < 1024ms → 放入毫秒轮
        else if (remain < kFirstLevelSize)
        {
            int newSlot = static_cast<int>((currentTick_ + remain) % kFirstLevelSize);
            firstLevel_[newSlot].push_back(timer);
            timerLocation_[timer] = {1, newSlot, --firstLevel_[newSlot].end()};
        }
        // 剩余时间 < 60s → 放入秒轮
        else if (remain < kSecondLevelSize * kFirstLevelSize)
        {
            int64_t sec = remain / kFirstLevelSize;
            int newSlot = static_cast<int>((currentTick_ / kFirstLevelSize + sec) % kSecondLevelSize);
            secondLevel_[newSlot].push_back(timer);
            timerLocation_[timer] = {2, newSlot, --secondLevel_[newSlot].end()};
        }
        // 剩余时间更长 → 留在分轮
        else
        {
            int64_t min = remain / (kFirstLevelSize * kSecondLevelSize);
            int newSlot = static_cast<int>((currentTick_ / (kFirstLevelSize * kSecondLevelSize) + min) % kThirdLevelSize);
            thirdLevel_[newSlot].push_back(timer);
            timerLocation_[timer] = {3, newSlot, --thirdLevel_[newSlot].end()};
        }
    }

    // 降级完成，清空当前高层槽
    bucket.clear();
}