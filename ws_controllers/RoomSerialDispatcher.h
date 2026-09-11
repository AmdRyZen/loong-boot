//
// Created by 神圣•凯莎 on 26-9-11.
//
// 房间级串行派发器
// ---------------------------------------------------------------------------
// 解决问题：房间广播原先直接跑在多线程 TBB 池里，同一房间的两条消息会被两个
// worker 并发执行 chatRooms_.publish()，客户端看到的顺序不保证（消息乱序）。
//
// 设计要点：
//  1. 每个房间维护一个待执行队列 + running 标志；抢到 running 的线程以
//     run-to-completion 的方式把队列抽干，因此不额外占用线程池槽位，也不会
//     出现任务嵌套提交。
//  2. 只在入队/出队时短暂持锁，真正的扇出（publish）在锁外执行。
//  3. 单房间队列有上限，过载时按房间限流丢弃并计数，避免无界内存增长。
//  4. 队列排空后立刻回收 Lane，空闲房间不占内存。
//
#ifndef LOONG_BOOT_ROOM_SERIAL_DISPATCHER_H
#define LOONG_BOOT_ROOM_SERIAL_DISPATCHER_H

#pragma once

#include <atomic>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>

class RoomSerialDispatcher
{
public:
    // 单房间最大积压任务数，超出即丢弃（配合 TBB 池背压，双重限流）
    static constexpr size_t kMaxRoomBacklog = 8192;

    using Task = std::function<void()>;

    /**
     * @brief 提交一个「在 room 内必须串行执行」的任务
     * @return false 表示该房间积压已达上限，任务被丢弃
     */
    bool dispatch(const std::string& room, Task&& task)
    {
        std::unique_lock lock(mutex_);

        Lane& lane = lanes_[room];
        if (lane.queue.size() >= kMaxRoomBacklog)
        {
            dropped_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        lane.queue.push_back(std::move(task));

        // 已经有人在抽这个房间的队列，交给他顺序执行即可
        if (lane.running)
        {
            return true;
        }
        lane.running = true;

        // 当前线程直接接管抽干，省掉一次额外的线程池调度
        drain(lane, room, lock);
        return true;
    }

    size_t droppedCount() const noexcept
    {
        return dropped_.load(std::memory_order_relaxed);
    }

    size_t activeRooms() const
    {
        std::unique_lock lock(mutex_);
        return lanes_.size();
    }

private:
    struct Lane
    {
        std::deque<Task> queue;
        bool running = false;
    };

    void drain(Lane& lane, const std::string& room, std::unique_lock<std::mutex>& lock)
    {
        for (;;)
        {
            if (lane.queue.empty())
            {
                lane.running = false;
                // 队列空且无人接管时才回收，避免与并发的 dispatch 抢同一个房间
                lanes_.erase(room);
                return;
            }

            Task task = std::move(lane.queue.front());
            lane.queue.pop_front();

            lock.unlock(); // 真正的扇出在锁外执行，不阻塞其他房间
            try
            {
                task();
            }
            catch (...)
            {
                // 单个任务异常不影响本房间后续消息的顺序投递
            }
            lock.lock();
            // 期间本房间的 Lane 不可能被 erase：running 始终为 true
        }
    }

    // 必须使用节点式容器：drain 会在锁外持有 Lane& 引用，
    // 若换成 phmap::flat_hash_map，其他房间的插入触发 rehash 会让该引用失效（UB）。
    // std::unordered_map 的 rehash 只失效迭代器，不失效元素引用。
    std::unordered_map<std::string, Lane> lanes_;
    mutable std::mutex mutex_;
    std::atomic<size_t> dropped_{0};
};

#endif // LOONG_BOOT_ROOM_SERIAL_DISPATCHER_H
