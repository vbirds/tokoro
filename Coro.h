#pragma once

#include <assert.h>
#include <atomic>
#include <cassert>
#include <chrono>
#include <coroutine>
#include <functional>
#include <iostream>
#include <memory>
#include <set>
#include <vector>

using Clock         = std::chrono::steady_clock;
using TimePoint     = Clock::time_point;
using ClockDuration = Clock::duration;

struct AwaiterBase
{
    virtual void OnWaitComplete(std::coroutine_handle<>) noexcept = 0;
    virtual ~AwaiterBase()                                        = default;
};

template <typename MyCoroHandle>
class CoroAwaiter : public AwaiterBase
{
private:
    MyCoroHandle            mMyHandle;
    std::coroutine_handle<> mParentHandle;

public:
    CoroAwaiter(MyCoroHandle handle)
        : mMyHandle(handle)
    {
    }

    bool await_ready() const noexcept
    {
        return false;
    }

    void await_suspend(std::coroutine_handle<> handle) noexcept
    {
        mParentHandle               = handle;
        mMyHandle.promise().awaiter = this;
        mMyHandle.resume();
    }

    auto await_resume() const noexcept
        requires requires { std::declval<decltype(mMyHandle.promise())>().value; }
    {
        return mMyHandle.promise().value;
    }

    void await_resume() const noexcept
        requires(!requires { std::declval<decltype(mMyHandle.promise())>().value; })
    {
    }

    void OnWaitComplete(std::coroutine_handle<> /*unused*/) noexcept override
    {
        mParentHandle.resume();
    }
};

struct PromiseBase
{
    AwaiterBase* awaiter{nullptr};
};

// —— Coro<T> 定义 ——

template <typename T>
struct Coro
{
    struct promise_type : PromiseBase
    {
        T value;
        // continuation 不再直接存在于 promise；resume 由 Awaiter 接管
        auto get_return_object() noexcept
        {
            return Coro{std::coroutine_handle<promise_type>::from_promise(*this)};
        }
        std::suspend_always initial_suspend() noexcept
        {
            return {};
        }
        struct FinalAwaiter
        {
            bool await_ready() const noexcept
            {
                return false;
            }
            void await_suspend(std::coroutine_handle<promise_type> h) const noexcept
            {
                if (h.promise().awaiter)
                    h.promise().awaiter->OnWaitComplete(h);
            }
            void await_resume() const noexcept
            {
            }
        };
        auto final_suspend() noexcept
        {
            return FinalAwaiter{};
        }
        void return_value(T v) noexcept
        {
            value = std::move(v);
        }
        void unhandled_exception()
        {
            std::terminate();
        }
    };

    using handle_type = std::coroutine_handle<promise_type>;
    handle_type mHandle;
    Coro(handle_type h)
        : mHandle(h)
    {
    }
    Coro(const Coro&) = delete;
    Coro(Coro&& o)
        : mHandle(o.mHandle)
    {
        o.mHandle = nullptr;
    }
    ~Coro()
    {
        if (mHandle)
            mHandle.destroy();
    }

    auto operator co_await() noexcept
    {
        return CoroAwaiter(mHandle);
    }
};

// void 专门化

template <>
struct Coro<void>
{
    struct promise_type : PromiseBase
    {
        auto get_return_object() noexcept
        {
            return Coro{std::coroutine_handle<promise_type>::from_promise(*this)};
        }
        std::suspend_always initial_suspend() noexcept
        {
            return {};
        }
        struct FinalAwaiter
        {
            bool await_ready() const noexcept
            {
                return false;
            }
            void await_suspend(std::coroutine_handle<promise_type> h) const noexcept
            {
                if (h.promise().awaiter)
                    h.promise().awaiter->OnWaitComplete(h);
            }
            void await_resume() const noexcept
            {
            }
        };
        auto final_suspend() noexcept
        {
            return FinalAwaiter{};
        }
        void return_void() noexcept
        {
        }
        void unhandled_exception()
        {
            std::terminate();
        }
    };

    using handle_type = std::coroutine_handle<promise_type>;
    handle_type mHandle;
    Coro(handle_type h)
        : mHandle(h)
    {
    }
    Coro(const Coro&) = delete;
    Coro(Coro&& o)
        : mHandle(o.mHandle)
    {
        o.mHandle = nullptr;
    }
    ~Coro()
    {
        if (mHandle)
            mHandle.destroy();
    }

    auto operator co_await() noexcept
    {
        return CoroAwaiter(mHandle);
    }
};

// —— 调度器 ——

template <typename T>
class TimeQueue
{
private:
    struct Node
    {
        TimePoint time;
        uint32_t  seq;
        uint32_t  frame;
        T         value;
    };

    struct Comp
    {
        bool operator()(const Node& a, const Node& b) const noexcept
        {
            if (a.time != b.time)
                return a.time < b.time;
            return a.seq < b.seq;
        }
    };

    using SetType = std::multiset<Node, Comp>;

public:
    using Iterator = typename SetType::const_iterator;

    Iterator Add(const T& e)
    {
        return AddImpl(TimePoint::min(), e);
    }

    Iterator AddTimed(const TimePoint& time, const T& e)
    {
        return AddImpl(time, e);
    }

    void Remove(Iterator iter)
    {
        if (iter == mUpdatePtr)
        {
            mUpdatePtr = mSet.erase(mUpdatePtr);
            MoveToNext();
        }
        else
        {
            mSet.erase(iter);
        }
    }

    std::optional<T> Pop()
    {
        if (mUpdatePtr == mSet.end())
            return std::nullopt;

        T ret = std::move(mUpdatePtr->value);

        mUpdatePtr = mSet.erase(mUpdatePtr);
        MoveToNext();

        return ret;
    }

    bool UpdateEnded() const noexcept
    {
        return mSet.empty() || mSet.end() == mUpdatePtr;
    }

    void SetupUpdate(TimePoint exeTime)
    {
        mAddFrame++;
        mAddOrder   = 0;
        mUpdatePtr  = mSet.begin();
        mCurExeTime = exeTime;

        MoveToNext();
    }

private:
    void MoveToNext()
    {
        while (mUpdatePtr != mSet.end())
        {
            const Node& node = *mUpdatePtr;

            if (node.time > mCurExeTime)
            {
                mUpdatePtr = mSet.end();
                break;
            }

            if (node.frame == mAddFrame)
            {
                ++mUpdatePtr;
            }
            else
            {
                break;
            }
        }
    }

    Iterator AddImpl(const TimePoint& time, const T& e)
    {
        Node node{time, mAddOrder++, mAddFrame, e};
        return mSet.insert(std::move(node));
    }

    SetType   mSet;
    uint32_t  mAddOrder = 0;
    uint32_t  mAddFrame = 0;
    Iterator  mUpdatePtr;
    TimePoint mCurExeTime;
};

class TimeAwaiter;
class Scheduler;

template <typename T>
class TaskHandle
{
public:
    ~TaskHandle();
    bool             IsValid() const noexcept;
    bool             IsDown() const noexcept;
    void             Stop() const noexcept;
    std::optional<T> GetReturn() const noexcept;

private:
    friend Scheduler;

    TaskHandle(uint64_t id)
        : mId(id)
    {
    }

    uint64_t mId = 0;
};

class Scheduler
{
public:
    static Scheduler& Instance()
    {
        static Scheduler s;
        return s;
    }

    template <typename T>
    static TaskHandle<T> Start(Coro<T>&& c) noexcept
    {
        auto h    = c.mHandle;
        c.mHandle = nullptr;
        h.resume();
        return TaskHandle<T>{0}; // todo
    }

    static TimeAwaiter NextFrame() noexcept;

    static TimeAwaiter Wait(double sec) noexcept;

    void Update();

private:
    friend TimeAwaiter;
    friend TaskHandle;

    void Release(uint64_t id);
    void IsValid(uint64_t id);
    void IsDown(uint64_t id);
    void Stop(uint64_t id);

    template <typename T>
    std::optional<T> GetReturn(uint64_t id);

    // std::unordered_map<uint64_t, std::unique_ptr<CoroBase>> mCoroutines;
    TimeQueue<TimeAwaiter*> mExecuteQueue;
};

class TimeAwaiter
{
public:
    TimeAwaiter(double sec)
        : mWhen(Clock::now() + std::chrono::duration_cast<ClockDuration>(std::chrono::duration<double>(sec)))
    {
    }

    TimeAwaiter()
        : mWhen(TimePoint::min())
    {
    }

    virtual ~TimeAwaiter()
    {
        if (mExeIter.has_value())
            Scheduler::Instance().mExecuteQueue.Remove(*mExeIter);
    }

    bool await_ready() const noexcept
    {
        return false;
    }

    void await_suspend(std::coroutine_handle<> handle) noexcept
    {
        mHandle  = std::coroutine_handle<PromiseBase>::from_address(handle.address());
        mExeIter = Scheduler::Instance().mExecuteQueue.AddTimed(mWhen, this);
    }

    void await_resume() const noexcept
    {
    }

    void Resume()
    {
        assert(mHandle && !mHandle.done() && mExeIter.has_value());
        // mExeIter has been removed from mExecuteQueue before enter Resume().
        mExeIter.reset();
        mHandle.resume();
    }

private:
    std::optional<TimeQueue<TimeAwaiter*>::Iterator> mExeIter;
    TimePoint                                        mWhen;
    std::coroutine_handle<PromiseBase>               mHandle = nullptr;
};

template <typename T>
TaskHandle<T>::~TaskHandle()
{
    Scheduler::Instance().Release(mId);
}

template <typename T>
bool TaskHandle<T>::IsValid() const noexcept
{
    Scheduler::Instance().IsValid(mId);
}

template <typename T>
bool TaskHandle<T>::IsDown() const noexcept
{
    Scheduler::Instance().IsDown(mId);
}

template <typename T>
void TaskHandle<T>::Stop() const noexcept
{
    Scheduler::Instance().Stop(mId);
}

template <typename T>
std::optional<T> TaskHandle<T>::GetReturn() const noexcept
{
    return Scheduler::Instance().GetReturn<T>(mId);
}

template <typename T>
std::optional<T> Scheduler::GetReturn(uint64_t id)
{
    return std::nullopt;
}

inline TimeAwaiter Scheduler::NextFrame() noexcept
{
    return TimeAwaiter();
}

inline TimeAwaiter Scheduler::Wait(double sec) noexcept
{
    return TimeAwaiter(sec);
}

inline void Scheduler::Update()
{
    mExecuteQueue.SetupUpdate(Clock::now());

    while (!mExecuteQueue.UpdateEnded())
    {
        mExecuteQueue.Pop().value()->Resume();
    }
}

inline void Scheduler::Release(uint64_t id)
{
}

inline void Scheduler::IsValid(uint64_t id)
{
}

inline void Scheduler::IsDown(uint64_t id)
{
}

inline void Scheduler::Stop(uint64_t id)
{
}

// —— All & Any ——

// All: 并行等待所有，返回 vector<T>
template <typename T, typename... Ts>
auto All(Coro<T> first, Coro<Ts>... rest)
{
    struct Awaiter : AwaiterBase
    {
        std::vector<typename Coro<T>::handle_type> hs;
        std::vector<T>                             results;
        size_t                                     remaining;
        std::coroutine_handle<>                    cont;

        Awaiter(Coro<T>&& f, Coro<Ts>&&... rs)
            : remaining(sizeof...(Ts) + 1)
        {
            hs.reserve(remaining);
            hs.push_back(f.mHandle);
            f.mHandle = nullptr;
            ([&] { hs.push_back(rs.mHandle); rs.mHandle = nullptr; }(), ...);
            results.resize(remaining);
        }

        bool await_ready() const noexcept
        {
            return remaining == 0;
        }
        void await_suspend(std::coroutine_handle<> c) noexcept
        {
            cont = c;
            for (size_t i = 0; i < hs.size(); ++i)
            {
                hs[i].promise().awaiter = this;
                hs[i].resume();
            }
        }
        void OnWaitComplete(std::coroutine_handle<> h) noexcept override
        {
            for (size_t i = 0; i < hs.size(); ++i)
            {
                if (hs[i] == h)
                {
                    auto handle = Coro<T>::handle_type::from_address(h.address());
                    results[i]  = std::move(handle.promise().value);
                    break;
                }
            }
            if (--remaining == 0)
                cont.resume();
        }
        auto await_resume()
        {
            return results;
        }
    };
    return Awaiter{std::move(first), std::move(rest)...};
}

// Any: 并行等待第一个，返回 T
template <typename T, typename... Ts>
auto Any(Coro<T> first, Coro<Ts>... rest)
{
    struct Awaiter : AwaiterBase
    {
        std::vector<typename Coro<T>::handle_type> hs;
        std::atomic<bool>                          triggered{false};
        T                                          result;
        std::coroutine_handle<>                    cont;

        Awaiter(Coro<T>&& f, Coro<Ts>&&... rs)
        {
            hs.reserve(sizeof...(Ts) + 1);
            hs.push_back(f.mHandle);
            f.mHandle = nullptr;
            ([&] { hs.push_back(rs.mHandle); rs.mHandle = nullptr; }(), ...);
        }

        bool await_ready() const noexcept
        {
            return false;
        }
        void await_suspend(std::coroutine_handle<> c) noexcept
        {
            cont = c;
            for (auto& h : hs)
            {
                h.promise().awaiter = this;
                h.resume();
            }
        }
        void OnWaitComplete(std::coroutine_handle<> h) noexcept override
        {
            if (!triggered.exchange(true))
            {
                auto handle = Coro<T>::handle_type::from_address(h.address());
                result      = std::move(handle.promise().value);
                cont.resume();
            }
        }
        T await_resume()
        {
            return std::move(result);
        }
    };
    return Awaiter{std::move(first), std::move(rest)...};
}