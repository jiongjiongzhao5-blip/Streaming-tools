#include "core/common/BoundedQueue.h"
#include "core/common/Common.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

namespace {

int g_passed = 0;
int g_failed = 0;

void section(const char* name)
{
    std::printf("\n--- %s ---\n", name);
}

void check(bool condition, const char* what)
{
    if (condition) {
        ++g_passed;
        std::printf("  [通过] %s\n", what);
    } else {
        ++g_failed;
        std::printf("  [失败] %s\n", what);
    }
}

using Clock = std::chrono::steady_clock;

long long elapsedMs(Clock::time_point from)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - from)
        .count();
}

void testFifo()
{
    section("1. 基本行为：先进先出");

    BoundedQueue<int> queue(4);
    queue.push(1);
    queue.push(2);
    queue.push(3);

    check(queue.tryPop(10).value_or(-1) == 1, "第一个出来的是最先放进去的（FIFO）");
    check(queue.tryPop(10).value_or(-1) == 2, "顺序保持");
    check(queue.tryPop(10).value_or(-1) == 3, "顺序保持");
    check(!queue.tryPop(10).has_value(), "空队列返回 nullopt");
    check(queue.empty(), "empty() 为真");
}

void testCapacityDropsOldest()
{
    section("2. 容量上限：满了丢最旧的（核心语义）");

    BoundedQueue<int> queue(3);
    queue.push(10);
    queue.push(20);
    queue.push(30);
    queue.push(40);
    queue.push(50);

    check(queue.tryPop(10).value_or(-1) == 30, "最旧的 10 已被丢弃");
    check(queue.tryPop(10).value_or(-1) == 40, "最旧的 20 已被丢弃");
    check(queue.tryPop(10).value_or(-1) == 50, "最新的 50 还在");
    check(!queue.tryPop(10).has_value(), "队列里只剩容量允许的 3 个元素");

    BoundedQueue<int> queue2(2);
    for (int i = 0; i < 100; ++i)
        queue2.push(i);
    check(queue2.tryPop(10).value_or(-1) == 98, "连续压入 100 个后，留下的是最新的一批");
    check(queue2.tryPop(10).value_or(-1) == 99, "一直到最新的那个");
}

void testTimeout()
{
    section("3. tryPop 的超时语义");

    BoundedQueue<int> queue(4);

    auto start = Clock::now();
    auto value = queue.tryPop(120);
    const long long waited = elapsedMs(start);

    check(!value.has_value(), "空队列上超时返回 nullopt");
    check(waited >= 100, "确实等待了大约 120 毫秒（没有忙等）");
    check(waited < 400, "也没有无限等下去");

    queue.push(42);
    start = Clock::now();
    value = queue.tryPop(500);
    check(value.has_value() && *value == 42, "有数据时取到正确值");
    check(elapsedMs(start) < 100, "有数据时立即返回，不会等满超时时间");
}

void testClear()
{
    section("4. clear 清空积压");

    BoundedQueue<int> queue(10);
    for (int i = 0; i < 5; ++i)
        queue.push(i);

    check(!queue.empty(), "清空前有数据");
    queue.clear();
    check(queue.empty(), "清空后为空");
    check(!queue.tryPop(10).has_value(), "清空后取不到任何东西");

    queue.push(7);
    check(queue.tryPop(10).value_or(-1) == 7, "清空后仍可正常使用");
}

void testSharedFrameIsCheap()
{
    section("5. 传一帧的代价：只拷贝指针，不拷贝像素");

    auto frame = std::make_shared<VideoFrame>();
    frame->width = 1920;
    frame->height = 1080;
    frame->bgra.resize(static_cast<size_t>(1920) * 1080 * 4);

    std::printf("  这一帧的像素数据 = %.1f MB\n",
                frame->bgra.size() / 1024.0 / 1024.0);

    check(frame.use_count() == 1, "初始引用计数为 1");

    BoundedQueue<VideoFramePtr> queue(4);
    queue.push(frame);
    check(frame.use_count() == 2, "push 之后引用计数为 2（队列持有了一个引用，没拷贝像素）");

    auto taken = queue.tryPop(10);
    check(taken.has_value() && taken->get() == frame.get(),
          "取出的还是同一个对象，全程零像素拷贝");

    frame.reset();
    check(taken->use_count() == 1, "原持有者释放后，数据仍由队列的引用维持存活");
}

void testConcurrent()
{
    section("6. 多线程生产者 / 消费者");

    constexpr int kCapacity = 8;
    constexpr int kTotal = 100000;

    BoundedQueue<int> queue(kCapacity);
    std::atomic<bool> producerDone{false};
    std::atomic<int> received{0};
    std::atomic<int> lastReceived{-1};
    std::atomic<bool> orderBroken{false};

    std::thread producer([&] {
        for (int i = 0; i < kTotal; ++i)
            queue.push(i);
        producerDone = true;
    });

    std::thread consumer([&] {
        int previous = -1;
        while (true) {
            auto value = queue.tryPop(50);
            if (!value) {
                if (producerDone && queue.empty())
                    break;
                continue;
            }
            if (*value <= previous)
                orderBroken = true;
            previous = *value;
            lastReceived = *value;
            ++received;
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    });

    producer.join();
    consumer.join();

    std::printf("  压入 %d 个，实际取到 %d 个\n", kTotal, received.load());

    check(true, "两个线程都正常结束（没有死锁）");
    check(!orderBroken, "取到的值严格递增（数据没有被破坏、没有重复）");
    check(lastReceived.load() == kTotal - 1,
          "最后一个取到的是最新的那个值（保留了最新数据）");
    check(received.load() > 0, "确实取到了数据");
    check(received.load() < kTotal / 10,
          "取到的数量远少于压入的数量，说明队列确实有界并丢弃了旧数据");
}

void testMemoryFootprint()
{
    section("7. 内存占用估算（为什么队列容量这么小）");

    const size_t frameBytes = static_cast<size_t>(1920) * 1080 * 4;

    std::printf("  sizeof(StreamConfig)  = %zu 字节（纯配置，很小）\n",
                sizeof(StreamConfig));
    std::printf("  sizeof(VideoFrame)    = %zu 字节（只有几个字段，"
                "像素在堆上）\n",
                sizeof(VideoFrame));
    std::printf("  sizeof(AudioChunk)    = %zu 字节（同理）\n",
                sizeof(AudioChunk));
    std::printf("\n");
    std::printf("  但一帧 1080p 的像素数据 = %zu 字节 = %.1f MB\n", frameBytes,
                frameBytes / 1024.0 / 1024.0);
    std::printf("  视频队列容量 5  ->  最多缓存的像素数据 = %.1f MB\n",
                frameBytes * 5 / 1024.0 / 1024.0);
    std::printf("  如果容量写成 1000 ->  %.1f GB（显然不可接受）\n",
                frameBytes * 1000.0 / 1024 / 1024 / 1024);
    std::printf("\n");
    std::printf("  所以队列容量的取值依据是：\n");
    std::printf("    - 装的是不可控大小的大对象（如视频帧）-> 容量取个位到十位数\n");
    std::printf("    - 装的是小块数据（如音频块）        -> 可以取几十\n");
    std::printf("    - 需要抗网络抖动的编码包队列        -> 可以取几百\n");

    check(true, "内存估算完成（见上面输出）");
}

}

int main()
{
#if defined(_WIN32)
    std::system("chcp 65001 > nul");
#endif

    std::printf("================================================\n");
    std::printf("  模块 3：公共契约层 —— 测试\n");
    std::printf("================================================\n");

    testFifo();
    testCapacityDropsOldest();
    testTimeout();
    testClear();
    testSharedFrameIsCheap();
    testConcurrent();
    testMemoryFootprint();

    std::printf("\n================================================\n");
    std::printf("  通过 %d 项，失败 %d 项\n", g_passed, g_failed);
    std::printf("================================================\n");
    std::fflush(stdout);

    return g_failed == 0 ? 0 : 1;
}
