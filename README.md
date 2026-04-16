# muduo Buffer 优化 - TCMalloc 条件编译优化
基于 muduo 网络库原生 Buffer 实现TCMalloc 无侵入式内存分配优化，通过条件编译实现 glibc malloc 与 TCMalloc 按需切换，解决高并发场景下内存分配锁竞争、内存碎片问题，同时保持原生接口兼容。
## 优化背景
muduo 原生 Buffer 底层采用 std::vector<char> 存储，依赖系统 glibc malloc 分配器，在高并发网络场景存在核心痛点：
1. 锁竞争严重：高并发下多线程竞争内存分配全局锁，CPU 空闲占比升高、吞吐量下降
2. 内存碎片率高：频繁小内存分配 / 释放，glibc malloc 管理效率低，长期运行内存占用飙升
3. 扩展性差：无可替换分配器接口，切换分配器需修改核心源码，侵入性强
## 优化思路
遵循最小侵入性、高扩展性、按需适配原则，不修改 muduo 原生逻辑与接口：
1. 条件编译：新增 MUDUO_BUFFER_USE_TCMALLOC 宏，编译时切换分配器
2. 分配器封装：实现符合 C++ 标准的 TCMalloc 分配器模板，适配 std::vector
3. 存储封装：通过类型别名切换底层存储，上层业务代码无感知
4. 兼容保留：保留原生 Buffer 三段式结构、智能扩容、协议解析等所有核心功能
## 核心实现
1. 条件编译宏与头文件
```
// TCMalloc 条件编译宏，编译时定义启用
#ifdef MUDUO_BUFFER_USE_TCMALLOC
#include <gperftools/tcmalloc.h>
#endif
```
2. TCMalloc 标准分配器封装
```
#ifdef MUDUO_BUFFER_USE_TCMALLOC
template <typename T>
class TCMallocAllocator {
public:
    using value_type = T;

    TCMallocAllocator() = default;
    template <typename U>
    TCMallocAllocator(const TCMallocAllocator<U>&) {}

    T* allocate(std::size_t n) {
        if (n > std::size_t(-1) / sizeof(T))
            throw std::bad_alloc();
        void* p = tc_malloc(n * sizeof(T));
        if (!p) throw std::bad_alloc();
        return static_cast<T*>(p);
    }

    void deallocate(T* p, std::size_t) noexcept {
        tc_free(p);
    }

    template <typename U>
    struct rebind { using other = TCMallocAllocator<U>; };
};

template <typename T, typename U>
bool operator==(const TCMallocAllocator<T>&, const TCMallocAllocator<U>&) { return true; }
template <typename T, typename U>
bool operator!=(const TCMallocAllocator<T>&, const TCMallocAllocator<U>&) { return false; }
#endif
```
3. 底层存储类型封装
```
// 按需切换 Buffer 底层存储
#ifdef MUDUO_BUFFER_USE_TCMALLOC
using BufferStorage = std::vector<char, TCMallocAllocator<char>>;
#else
using BufferStorage = std::vector<char>;
#endif

// 原生 Buffer 类无修改
class Buffer : public muduo::copyable {
private:
    BufferStorage buffer_;
    size_t readerIndex_;
    size_t writerIndex_;
    static const char kCRLF[];
};
```
## 编译与使用
### 启用 TCMalloc 优化
编译时添加宏定义：
```
-DMUDUO_BUFFER_USE_TCMALLOC
```
链接 TCMalloc 库：
```
-ltcmalloc
```
### 默认模式（原生兼容）
不添加上述宏定义，自动使用系统 glibc malloc，与原生 muduo 完全一致。
## 压测对比
| 分配器       | Requests/sec | 平均延迟 | 99% 延迟 |
| ------------ | ------------ | -------- | -------- |
| glibc malloc | 209618.40    | 1.34ms   | 95.54ms  |
| TCMalloc     | 213576.19    | 0.96ms   | 48.91ms  |

## 测试结论
- muduo 原生 Buffer 设计优秀（预分配 + 内存复用），运行时内存分配非瓶颈
- TCMalloc 在延迟优化上效果显著，99% 延迟降低约 50%
- 高并发长稳运行场景，TCMalloc 可减少内存碎片，提升服务稳定性
## 优化亮点
1. 无侵入式：不修改原生 Buffer 核心逻辑，上层业务零改动
2. 高兼容：默认保持原生行为，支持无 TCMalloc 环境编译
3. 易扩展：支持快速接入 jemalloc 等其他分配器
4. 工业级：包含溢出检查、标准分配器接口，适配线上生产环境
## 适用场景
- 高并发网络服务（QPS 10 万 +）
- 长期运行的后端服务（降低内存碎片）
- 需要灵活切换内存分配器的工程化项目
