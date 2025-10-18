#define _CRTDBG_MAP_ALLOC
#include <iostream>
#include <thread>
#include <vector>
#include <queue>
#include <array>
#include <cassert>
#include <atomic>
#include "mpmc_queue.h" // data限制在64位以内，先cas data_entry再 cas tail
#include "MPMCQueue.h"  // 这个和我的lock_free_queue_version一样，支持各种data类型，然后是先cas tail，然后再处理data，最后再设置flag
#include <crtdbg.h>

template <typename T>
class lock_free_queue_spsc
{
private:
    std::vector<T> m_buffer;
    std::atomic<size_t> m_head{0};
    std::atomic<size_t> m_tail{0};

public:
    lock_free_queue_spsc(size_t size) : m_buffer(size), m_head(0), m_tail(0) {}

    bool push(T const &data)
    {
        if (full())
            return false;

        m_buffer[m_head] = data;
        m_head = (m_head + 1) % m_buffer.size();
        return true;
    }

    bool pop(T &data)
    {
        if (empty())
            return false;
        data = m_buffer[m_tail];
        m_tail = (m_tail + 1) % m_buffer.size();
        return true;
    }

    bool empty() const
    {
        return m_head == m_tail;
    }

    bool full() const
    {
        return m_head == (m_tail + 1) % m_buffer.size();
    }
};

template <typename T>
class lock_free_queue
{
private:
    struct Item
    {
        T data;
        std::atomic<bool> ready{false};
    };

    std::vector<Item> m_buffer;
    std::atomic<size_t> m_head{0};
    std::atomic<size_t> m_tail{0};

public:
    lock_free_queue(size_t size) : m_buffer(size), m_head(0), m_tail(0) {}
    ~lock_free_queue() {}

    bool try_push(T const &data)
    {
        while (true)
        {
            size_t tail = m_tail.load(std::memory_order_acquire);
            size_t head = m_head.load(std::memory_order_acquire);

            size_t next_tail = (tail + 1) % m_buffer.size();
            if (next_tail == head)
            {
                return false;
            }

            // 先挪后while ready没能解决aba问题，即使buffer的size很大1024ll * 1024ll * 1024ll * 2ll，也会出现问题
            // 先判断了ready，然后再挪tail，这样1024 * 1024的buffer下面，20个线程也是可以的，1024buffer就不行了
            if (!m_buffer[tail].ready && m_tail.compare_exchange_weak(tail, next_tail, std::memory_order_acq_rel))
            {
                // while (m_buffer[tail].ready)
                // {
                //     // std::this_thread::yield();
                // }
                m_buffer[tail].data = data;
                m_buffer[tail].ready.store(true, std::memory_order_release);
                return true;
            }
        }
        return true;
    }

    bool try_pop(T &data)
    {
        while (true)
        {
            size_t tail = m_tail.load(std::memory_order_acquire);
            size_t head = m_head.load(std::memory_order_acquire);

            if (tail == head)
            {
                return false;
            }

            size_t next_head = (head + 1) % m_buffer.size();
            if (m_buffer[head].ready && m_head.compare_exchange_weak(head, next_head, std::memory_order_acq_rel))
            {
                // while (!m_buffer[head].ready)
                // {
                //     // std::this_thread::yield();
                // }
                data = m_buffer[head].data;
                m_buffer[head].ready.store(false, std::memory_order_release);
                return true;
            }
        }
        return true;
    }

    bool empty() const { return m_head.load(std::memory_order_acquire) == m_tail.load(std::memory_order_acquire); }

    friend int main();
};

template <typename T>
class lock_free_queue_node
{
private:
    struct Node
    {
        std::atomic<Node *> next;
        T data;
    };

    std::atomic<Node *> m_head;
    std::atomic<Node *> m_tail;

public:
    lock_free_queue_node(size_t size = 0)
    {
        Node *dummy = new Node;
        m_head.store(dummy, std::memory_order_relaxed);
        m_tail.store(dummy, std::memory_order_relaxed);
    }
    ~lock_free_queue_node()
    {
        while (Node *old_head = m_head.load(std::memory_order_acquire))
        {
            if (m_head.compare_exchange_weak(old_head, old_head->next.load(std::memory_order_acquire)))
            {
                delete old_head;
            }
        }
    }

    bool push(T const &data)
    {
        Node *new_node = new Node;
        new_node->data = data;
        new_node->next.store(nullptr, std::memory_order_relaxed);
        while (true)
        {
            Node *tail = m_tail.load(std::memory_order_acquire);
            Node *next = tail->next.load(std::memory_order_acquire);
            if (next == nullptr && tail->next.compare_exchange_weak(next, new_node, std::memory_order_acq_rel))
            {
                if (m_tail.compare_exchange_weak(tail, new_node, std::memory_order_acq_rel))
                {
                    return true;
                }
                else
                {
                    delete new_node;
                    throw std::runtime_error("Failed to update tail");
                }
            }
        }
        return true;
    }

    bool pop(T &data)
    {
        while (true)
        {
            Node *head = m_head.load(std::memory_order_acquire);
            Node *next = head->next.load(std::memory_order_acquire);
            if (next == nullptr)
            {
                return false;
            }
            if (m_head.compare_exchange_weak(head, next, std::memory_order_acq_rel))
            {
                data = next->data;
                delete head;
                return true;
            }
        }
    }
};

template <typename T>
class lock_free_queue_version // 非常正确的，缺点不会return false
{
private:
    using index_type = unsigned short;

    struct Item
    {
        T data;
        std::atomic<index_type> meta;
    };

    std::vector<Item> m_buffer;
    std::atomic<index_type> m_head{0};
    std::atomic<index_type> m_tail{0};

public:
    lock_free_queue_version(size_t size) : m_buffer(size), m_head(0), m_tail(0)
    {
        for (size_t i = 0; i < size; ++i)
        {
            m_buffer[i].meta.store(i << 1 | 0, std::memory_order_relaxed);
        }
    }
    ~lock_free_queue_version() {}

    bool try_push(T const &data)
    {
        while (true)
        {
            index_type tail = m_tail.load(std::memory_order_acquire);
            size_t index = tail % m_buffer.size();
            if (m_buffer[index].meta.load(std::memory_order_acquire) == (index_type(tail << 1) | 0))
            {
                if (m_tail.compare_exchange_weak(tail, tail + 1, std::memory_order_acq_rel))
                {
                    m_buffer[index].data = data;
                    m_buffer[index].meta.store((m_buffer[index].meta.load(std::memory_order_acquire)) | 1, std::memory_order_release);
                    return true;
                }
            }
            // 要么m_tail不等于meta的version，要么flag是1
            else if (tail != m_tail.load(std::memory_order_acquire))
            {
                return false;
            }
        }
        return true;
    }

    bool try_pop(T &data)
    {
        while (true)
        {
            index_type head = m_head.load(std::memory_order_acquire);
            size_t index = head % m_buffer.size();
            if (m_buffer[index].meta.load(std::memory_order_acquire) == (index_type(head << 1) | 1))
            {
                if (m_head.compare_exchange_weak(head, head + 1, std::memory_order_acq_rel))
                {
                    data = m_buffer[index].data;
                    m_buffer[index].meta.store((head + m_buffer.size()) << 1 | 0, std::memory_order_release);
                    return true;
                }
            }
            else if (head != m_head.load(std::memory_order_acquire))
            {
                return false;
            }
        }
        return true;
    }

    bool empty() const
    {
        return m_head.load(std::memory_order_acquire) == m_tail.load(std::memory_order_acquire);
    }

    friend int main();
};

// using test_queue_class = lock_free_queue_version<unsigned>;
using test_queue_class = lock_free_queue<unsigned>;
// using test_queue_class = es::lockfree::mpmc_queue<unsigned>;
// using test_queue_class = lock_free_queue_node<unsigned>;
// using test_queue_class = rigtorp::MPMCQueue<unsigned>;
// using try_push = decltype(&test_queue_class::try_push);

int main()
{
    {
        test_queue_class q{1024 * 1024};
        constexpr unsigned N{100000};
        constexpr unsigned P{10};
        constexpr unsigned C{10};
        std::atomic<uint64_t> prod_sum{0};
        std::atomic<uint64_t> cons_sum{0};

        auto producer = [&]()
        {
            for (unsigned x = 0; x < N; ++x)
            {
                while (!q.try_push(x))
                    ;
                prod_sum += x;
            }
        };
        std::vector<std::thread> producers;
        producers.resize(P);
        for (auto &p : producers)
            p = std::thread{producer};

        auto consumer = [&]()
        {
            unsigned v{0};
            for (unsigned x = 0; x < N; ++x)
            {
                while (!q.try_pop(v))
                    ;
                cons_sum += v;
            }
        };
        std::vector<std::thread> consumers;
        consumers.resize(C);
        for (auto &c : consumers)
            c = std::thread{consumer};

        for (auto &p : producers)
            p.join();
        for (auto &c : consumers)
            c.join();
        std::cout << (cons_sum && cons_sum == prod_sum ? "OK" : "ERROR") << " " << cons_sum << '\n';
        assert(q.empty());
        // for (unsigned x = 0; x < q.m_buffer.size(); ++x)
        // {
        //     assert((q.m_buffer[x].meta.load(std::memory_order_acquire) & 1) == 0);
        // }
        for (unsigned x = 0; x < q.m_buffer.size(); ++x)
        {
            assert(q.m_buffer[x].ready.load(std::memory_order_acquire) == 0);
        }
    }

    _CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_DEBUG);
    _CrtDumpMemoryLeaks();
    return 0;
}