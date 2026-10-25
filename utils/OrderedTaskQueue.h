#pragma once

// 按 key 保序的后台任务队列。
//
// 用途：Kafka 落库原先每条消息都 submit 到 TBB，多个 worker 并发调用
// rd_kafka_produce。librdkafka 只保证「调用进入 producer 之后」的顺序，
// 而 TBB 不保证任务开始顺序 == 提交顺序，于是同一房间/会话的归档可能乱序。
//
// 这里用固定数量的 stripe，每个 stripe 一个线程、一个 FIFO 队列：
//   · 同一个 key 永远落到同一个 stripe，提交顺序即执行顺序；
//   · 不同 key 可并行，不会把 Kafka 生产压成单线程；
//   · 队列有界，满了返回 false，由调用方计入持久化丢弃指标。
//
// 为什么不继续用 TBB + future 链：等待前一条会阻塞 TBB worker，Kafka 抖动时
// 会把整个 TBB 池拖死；而 TBB 池同时承载其他任务，不能为归档顺序牺牲它。
//
// 生命周期：本类析构时先停止接收、排空已入队任务，再 join。调用方必须保证
// 它比 KafkaManager 先析构（本工程用函数内静态量，首次投递时构造，晚于
// KafkaManager::initialize，因此静态析构顺序天然满足）。

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

class OrderedTaskQueue
{
  public:
    using Task = std::function<void()>;

    explicit OrderedTaskQueue(size_t stripes = 8, size_t maxPending = 1 << 16)
        : maxPending_(std::max<size_t>(maxPending, 1))
    {
        stripes = std::max<size_t>(stripes, 1);
        stripes_.reserve(stripes);
        for (size_t i = 0; i < stripes; ++i)
        {
            stripes_.push_back(std::make_unique<Stripe>());
        }
        threads_.reserve(stripes);
        for (size_t i = 0; i < stripes; ++i)
        {
            threads_.emplace_back([this, i] { workerLoop(*stripes_[i]); });
        }
    }

    ~OrderedTaskQueue()
    {
        for (auto& s : stripes_)
        {
            {
                std::lock_guard lock(s->mtx);
                s->stop = true;
            }
            s->cv.notify_all();
        }
        for (auto& t : threads_)
        {
            if (t.joinable())
            {
                t.join();
            }
        }
    }

    OrderedTaskQueue(const OrderedTaskQueue&) = delete;
    OrderedTaskQueue& operator=(const OrderedTaskQueue&) = delete;

    // 提交一个任务。返回 false 表示队列已满（任务未入队）。
    bool submit(std::string_view key, Task task)
    {
        if (!task)
        {
            return true;
        }
        const size_t pending = pending_.load(std::memory_order_relaxed);
        if (pending >= maxPending_)
        {
            return false;
        }

        Stripe& s = *stripes_[stripeIndex(key)];
        {
            std::lock_guard lock(s.mtx);
            if (s.tasks.size() >= maxPending_)
            {
                return false;
            }
            s.tasks.push_back(std::move(task));
            pending_.fetch_add(1, std::memory_order_relaxed);
        }
        s.cv.notify_one();
        return true;
    }

    size_t pending() const noexcept
    {
        return pending_.load(std::memory_order_relaxed);
    }

    size_t stripeCount() const noexcept
    {
        return stripes_.size();
    }

  private:
    struct Stripe
    {
        std::mutex mtx;
        std::condition_variable cv;
        std::deque<Task> tasks;
        bool stop = false;
    };

    static uint64_t hashKey(std::string_view key) noexcept
    {
        // FNV-1a：稳定、无分配，不依赖 std::hash 的实现差异。
        uint64_t h = 1469598103934665603ull;
        for (const unsigned char c : key)
        {
            h ^= c;
            h *= 1099511628211ull;
        }
        return h;
    }

    size_t stripeIndex(std::string_view key) const noexcept
    {
        return static_cast<size_t>(hashKey(key) % stripes_.size());
    }

    void workerLoop(Stripe& s)
    {
        for (;;)
        {
            Task task;
            {
                std::unique_lock lock(s.mtx);
                s.cv.wait(lock, [&s] { return s.stop || !s.tasks.empty(); });
                if (s.tasks.empty())
                {
                    if (s.stop)
                    {
                        return;
                    }
                    continue;
                }
                task = std::move(s.tasks.front());
                s.tasks.pop_front();
            }
            pending_.fetch_sub(1, std::memory_order_relaxed);
            try
            {
                task();
            }
            catch (...)
            {
                // 任务自身必须负责计数与日志。这里吞掉异常是最后防线：
                // 让一个归档任务失败绝不能终止后台线程或整个进程。
            }
        }
    }

    size_t maxPending_;
    // unique_ptr：线程里持有 Stripe& 的引用，vector 一旦扩容搬移就会悬空。
    std::vector<std::unique_ptr<Stripe>> stripes_;
    std::vector<std::thread> threads_;
    std::atomic<size_t> pending_{0};
};
