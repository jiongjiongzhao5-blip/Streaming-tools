#pragma once

#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>

template <typename T>
class BoundedQueue {
public:
    explicit BoundedQueue(size_t maxSize = 64)
        : maxSize_(maxSize)
    {
    }

    void push(T item)
    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (queue_.size() >= maxSize_)
            queue_.pop_front();

        queue_.push_back(std::move(item));

        cv_.notify_all();
    }

    std::optional<T> tryPop(int timeoutMs)
    {
        std::unique_lock<std::mutex> lock(mutex_);

        cv_.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                     [this] { return !queue_.empty(); });

        if (queue_.empty())
            return std::nullopt;

        T item = std::move(queue_.front());
        queue_.pop_front();
        return item;
    }

    void clear()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.clear();
    }

    bool empty() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.empty();
    }

private:
    size_t maxSize_;

    std::deque<T> queue_;

    mutable std::mutex mutex_;

    std::condition_variable cv_;
};
