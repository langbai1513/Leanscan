#pragma once
// ════════════════════════════════════════════════════════════════════════
//  thread_pool.hpp —— 两件互相独立的小东西
//
//    Pool       固定线程池：线程只建一次、只销毁一次，任务反复投递。
//    BufferPool 无锁缓冲池：搜索前一次性把 N 块缓冲分配好，
//               任务用原子栈（CAS）借/还，借还全程无锁、无共享写。
//
//  都不认识 maps / 搜索：谁都能用。
// ════════════════════════════════════════════════════════════════════════

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <utility>
#include <vector>

namespace tp {

// ────────────────────────────────────────────────────────────────────────
//  无锁缓冲池
// ────────────────────────────────────────────────────────────────────────
class BufferPool {
public:
    struct Lease {                    // 借到的一块缓冲
        uint8_t*    data  = nullptr;
        std::size_t bytes = 0;
        bool ok() const noexcept { return data != nullptr; }
    };

    BufferPool(std::size_t count, std::size_t bytes) : bytes_(bytes) {
        if (count == 0) count = 1;
        nodes_.reserve(count);
        store_.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            store_.emplace_back(new uint8_t[bytes]);
            nodes_.push_back(Node{nullptr, store_.back().get()});   // reserve 过，地址稳定
        }
        // 全部压栈（此时无并发，直接链）
        for (std::size_t i = count; i-- > 0;) {
            nodes_[i].next = head_.load(std::memory_order_relaxed);
            head_.store(&nodes_[i], std::memory_order_relaxed);
        }
    }

    BufferPool(const BufferPool&)            = delete;
    BufferPool& operator=(const BufferPool&) = delete;

    Lease acquire() noexcept {
        Node* n = pop();
        return n ? Lease{n->data, bytes_} : Lease{};
    }

    void release(Lease l) noexcept {
        if (!l.data) return;
        for (Node& n : nodes_) {          // 反查节点（只在归还时走，节点数=线程数，很短）
            if (n.data == l.data) { push(&n); return; }
        }
    }

    std::size_t bytes() const noexcept { return bytes_; }

private:
    struct Node { Node* next; uint8_t* data; };

    Node* pop() noexcept {
        Node* h = head_.load(std::memory_order_acquire);
        while (h && !head_.compare_exchange_weak(h, h->next,
                                                 std::memory_order_acquire,
                                                 std::memory_order_relaxed)) {}
        return h;
    }
    void push(Node* n) noexcept {
        Node* h = head_.load(std::memory_order_relaxed);
        do { n->next = h; }
        while (!head_.compare_exchange_weak(h, n,
                                            std::memory_order_release,
                                            std::memory_order_relaxed));
    }

    std::vector<std::unique_ptr<uint8_t[]>> store_;
    std::vector<Node>                       nodes_;
    std::atomic<Node*>                      head_{nullptr};
    std::size_t                             bytes_ = 0;
};

// ────────────────────────────────────────────────────────────────────────
//  固定线程池
// ────────────────────────────────────────────────────────────────────────
class Pool {
public:
    /// n = 工作线程数；on_worker 在每个工作线程启动时调用一次（可用来做该线程的一次性准备）
    explicit Pool(std::size_t n, std::function<void()> on_worker = {})
        : on_worker_(std::move(on_worker)) {
        if (n == 0) n = 1;
        threads_.reserve(n);
        for (std::size_t i = 0; i < n; ++i)
            threads_.emplace_back([this] { worker(); });
    }

    ~Pool() {
        {
            std::lock_guard<std::mutex> lk(m_);
            stop_ = true;
        }
        cv_job_.notify_all();
        for (std::thread& t : threads_)
            if (t.joinable()) t.join();
    }

    Pool(const Pool&)            = delete;
    Pool& operator=(const Pool&) = delete;

    std::size_t workers() const noexcept { return threads_.size(); }

    void submit(std::function<void()> job) {
        {
            std::lock_guard<std::mutex> lk(m_);
            queue_.push(std::move(job));
            ++pending_;
        }
        cv_job_.notify_one();
    }

    /// 阻塞到「已投递的任务」全部跑完
    void wait() {
        std::unique_lock<std::mutex> lk(m_);
        cv_done_.wait(lk, [this] { return pending_ == 0; });
    }

private:
    void worker() {
        if (on_worker_) on_worker_();
        for (;;) {
            std::function<void()> job;
            {
                std::unique_lock<std::mutex> lk(m_);
                cv_job_.wait(lk, [this] { return stop_ || !queue_.empty(); });
                if (queue_.empty()) { if (stop_) return; continue; }
                job = std::move(queue_.front());
                queue_.pop();
            }
            job();
            {
                std::lock_guard<std::mutex> lk(m_);
                if (--pending_ == 0) cv_done_.notify_all();
            }
        }
    }

    std::vector<std::thread>          threads_;
    std::queue<std::function<void()>> queue_;
    std::mutex                        m_;
    std::condition_variable           cv_job_;
    std::condition_variable           cv_done_;
    std::size_t                       pending_ = 0;   // 排队 + 执行中
    bool                              stop_    = false;
    std::function<void()>             on_worker_;
};

} // namespace tp