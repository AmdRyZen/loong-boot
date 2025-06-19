//
// Created by 神圣•凯莎 on 25-6-18.
//

#ifndef TBBUTILS_H
#define TBBUTILS_H

#include <vector>
#include <tbb/parallel_for.h>
#include <unordered_map>

// 通用并行 transform 工具，根据数据量自动选择串行或并行
template<typename SizeType, typename Func, typename T = std::invoke_result_t<Func, SizeType>>
static std::vector<T> smart_parallel_transform(SizeType size, Func&& func, SizeType threshold = 1000)
{
    std::vector<T> result;
    if (size >= threshold)
    {
        const size_t concurrency = tbb::this_task_arena::max_concurrency();
        std::vector<std::vector<T>> local(concurrency);
        tbb::parallel_for(SizeType(0), size, [&](SizeType i) {
            size_t tid = tbb::this_task_arena::current_thread_index();
            if (tid == static_cast<size_t>(-1)) tid = i % concurrency;
            local[tid].emplace_back(std::move(func(i)));
        });
        for (auto& v : local)
        {
            result.insert(result.end(),
                          std::make_move_iterator(v.begin()),
                          std::make_move_iterator(v.end()));
        }
    }
    else
    {
        result.reserve(size);
        for (SizeType i = 0; i < size; ++i)
        {
            result.push_back(std::move(func(i)));
        }
    }
    return result;
}

// 并发构建 unordered_map<Key, Value>
template<
    typename SizeType,
    typename Key,
    typename Value,
    typename Func>
static std::unordered_map<Key, Value> smart_parallel_transform_to_unordered_map(SizeType size, Func&& func, SizeType threshold = 1000)
{
    std::unordered_map<Key, Value> result;
    if (size >= threshold)
    {
        const size_t concurrency = tbb::this_task_arena::max_concurrency();
        std::vector<std::unordered_map<Key, Value>> local(concurrency);
        tbb::parallel_for(SizeType(0), size, [&](SizeType i) {
            size_t tid = tbb::this_task_arena::current_thread_index();
            if (tid == static_cast<size_t>(-1)) tid = i % concurrency;
            auto&& [k, v] = func(i); // 要求 func(i) 返回 std::pair<Key, Value>
            local[tid].emplace(std::move(k), std::move(v));
        });
        for (auto& map : local)
        {
            result.insert(std::make_move_iterator(map.begin()), std::make_move_iterator(map.end()));
        }
    }
    else
    {
        for (SizeType i = 0; i < size; ++i)
        {
            auto&& [k, v] = func(i);
            result.emplace(std::move(k), std::move(v));
        }
    }
    return result;
}

// 泛型版本，支持任意容器类型（默认 std::vector）
template<
    typename SizeType,
    typename Container,
    typename Func,
    typename ValueType = typename Container::value_type>
static Container smart_parallel_transform_to_container(SizeType size, Func&& func, SizeType threshold = 1000)
{
    Container result;
    if (size >= threshold)
    {
        const size_t concurrency = tbb::this_task_arena::max_concurrency();
        std::vector<std::vector<ValueType>> local(concurrency);
        tbb::parallel_for(SizeType(0), size, [&](SizeType i) {
            size_t tid = tbb::this_task_arena::current_thread_index();
            if (tid == static_cast<size_t>(-1)) tid = i % concurrency;
            local[tid].emplace_back(std::move(func(i)));
        });
        for (auto& part : local)
        {
            result.insert(result.end(), std::make_move_iterator(part.begin()), std::make_move_iterator(part.end()));
        }
    }
    else
    {
        for (SizeType i = 0; i < size; ++i)
        {
            result.insert(result.end(), std::move(func(i)));
        }
    }
    return result;
}

#endif //TBBUTILS_H
