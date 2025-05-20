#pragma once

#include <any>
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
using StdHandle     = std::coroutine_handle<>;

struct AwaiterBase
{
    virtual void OnWaitComplete(std::coroutine_handle<>) noexcept = 0;
    virtual ~AwaiterBase()                                        = default;
};

template <typename MyCoroHandle>
class CoroAwaiter : public AwaiterBase
{
private:
    MyCoroHandle mMyHandle;
    StdHandle    mParentHandle;

public:
    CoroAwaiter(MyCoroHandle handle)
        : mMyHandle(handle)
    {
    }

    bool await_ready() const noexcept
    {
        return false;
    }

    void await_suspend(StdHandle handle) noexcept
    {
        mParentHandle               = handle;
        mMyHandle.promise().awaiter = this;
        mMyHandle.resume();
    }

    auto await_resume() const noexcept
        requires requires { std::declval<decltype(mMyHandle.promise())>().value; } // todo Try use MyCoroHandle::promise
    {
        return mMyHandle.promise().value;
    }

    void await_resume() const noexcept
        requires(!requires { std::declval<decltype(mMyHandle.promise())>().value; })
    {
    }

    void OnWaitComplete(StdHandle /*unused*/) noexcept override
    {
        mParentHandle.resume();
    }
};

struct PromiseBase
{
    AwaiterBase* awaiter{nullptr};
};

// —— Coro<T> 定义 ——

class CoroBase
{
public:
    CoroBase(StdHandle h)
        : mHandle(h)
    {
    }

    CoroBase(CoroBase&& o)
        : mHandle(o.mHandle)
    {
        o.mHandle = nullptr;
    }

    virtual ~CoroBase()
    {
        if (mHandle)
            mHandle.destroy();
    }

    void Resume()
    {
        mHandle.resume();
    }

    StdHandle Handle() const
    {
        return mHandle;
    }

private:
    StdHandle mHandle;
};

template <typename T>
struct Coro : CoroBase
{
    struct promise_type : PromiseBase
    {
        T        value;
        uint64_t mId = 0;

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
            void await_suspend(std::coroutine_handle<promise_type> h) const noexcept;

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

    using value_type  = T;
    using handle_type = std::coroutine_handle<promise_type>;
    handle_type mHandle;

    Coro(handle_type h)
        : CoroBase(h), mHandle(h)
    {
    }

    Coro(const Coro&) = delete;

    Coro(Coro&& o)
        : CoroBase(std::move(o)), mHandle(o.mHandle)
    {
        o.mHandle = nullptr;
    }

    auto operator co_await() noexcept
    {
        return CoroAwaiter(std::coroutine_handle<promise_type>::from_address(mHandle.address()));
    }
};

// void 专门化

template <>
struct Coro<void> : CoroBase
{
    struct promise_type : PromiseBase
    {
        uint64_t mId = 0;

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
            void await_suspend(std::coroutine_handle<promise_type> h) const noexcept;

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
    using value_type  = void;

    handle_type mHandle;

    Coro(handle_type h)
        : CoroBase(h), mHandle(h)
    {
    }
    Coro(const Coro&) = delete;
    Coro(Coro&& o)
        : CoroBase(std::move(o)), mHandle(o.mHandle)
    {
    }

    auto operator co_await() noexcept
    {
        return CoroAwaiter(std::coroutine_handle<promise_type>::from_address(mHandle.address()));
    }
};

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

    template <typename Task, typename... Args>
    auto Start(Task&& task, Args&&... args)
    {
        uint64_t id = mNextId++;

        using RawCoroType = std::invoke_result_t<Task, Args...>;
        using CoroType    = std::decay_t<RawCoroType>;

        static_assert(std::is_base_of_v<CoroBase, CoroType>,
                      "First parameter must be a callable object (function, lambda ect..) returns Coro<T>");

        using T = typename CoroType::value_type;

        Entry e;
        e.coro = std::make_unique<CoroType>(
            std::forward<Task>(task)(std::forward<Args>(args)...));
        e.finished = false;
        e.coro->Resume(); // Kick off Coroutine

        mCoroutines.emplace(id, std::move(e));

        return TaskHandle<T>{id};
    }

    static TimeAwaiter NextFrame() noexcept;

    static TimeAwaiter Wait(double sec) noexcept;

    void Update();

private:
    friend TimeAwaiter;
    friend TaskHandle;
    friend Coro;

    void Release(uint64_t id);
    bool IsValid(uint64_t id);
    bool IsDown(uint64_t id);
    void Stop(uint64_t id);

    template <typename T>
    std::optional<T> GetReturn(uint64_t id);

    // Called from FinalAwaiter
    template <typename R>
    void OnCoroutineFinished(uint64_t id, R&& result)
    {
        auto& e       = mCoroutines[id];
        e.finished    = true;
        e.returnValue = std::any(std::forward<R>(result));
    }
    void OnCoroutineFinished(uint64_t id)
    {
        mCoroutines[id].finished = true;
    }

    struct Entry
    {
        std::unique_ptr<CoroBase> coro;
        bool                      finished;
        std::any                  returnValue;
    };

    std::unordered_map<uint64_t, Entry> mCoroutines;
    std::atomic<uint64_t>               mNextId{1};
    TimeQueue<TimeAwaiter*>             mExecuteQueue;
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
    auto it = mCoroutines.find(id);
    if (it != mCoroutines.end() && it->second.finished)
    {
        mCoroutines.erase(it);
    }
}

inline bool Scheduler::IsValid(uint64_t id)
{
    auto it = mCoroutines.find(id);
    return it != mCoroutines.end() && !it->second.finished;
}

inline bool Scheduler::IsDown(uint64_t id)
{
    auto it = mCoroutines.find(id);
    return it != mCoroutines.end() && it->second.finished;
}

inline void Scheduler::Stop(uint64_t id)
{
    auto it = mCoroutines.find(id);
    if (it != mCoroutines.end())
    {
        it->second.coro.reset();
        mCoroutines.erase(it);
    }
}

template <typename T>
void Coro<T>::promise_type::FinalAwaiter::await_suspend(std::coroutine_handle<promise_type> h) const noexcept
{
    const uint64_t coroId = h.promise().mId;

    // Can't have awaiter and coroId both.
    assert(h.promise().awaiter == nullptr || coroId == 0);

    if (h.promise().awaiter)
    {
        h.promise().awaiter->OnWaitComplete(h);
    }
    else if (coroId != 0)
    {
        Scheduler::Instance().OnCoroutineFinished(coroId, std::move(h.promise().value));
        Scheduler::Instance().Release(coroId);
    }
}

inline void Coro<void>::promise_type::FinalAwaiter::await_suspend(std::coroutine_handle<promise_type> h) const noexcept
{
    const uint64_t coroId = h.promise().mId;

    // Can't have awaiter and coroId both.
    assert(h.promise().awaiter == nullptr || coroId == 0);

    if (h.promise().awaiter)
    {
        h.promise().awaiter->OnWaitComplete(h);
    }
    else if (coroId != 0)
    {
        Scheduler::Instance().OnCoroutineFinished(h.promise().mId);
        Scheduler::Instance().Release(h.promise().mId);
    }
}

// —— All & Any ——

// map void to monostate
template <typename T>
using Ret = std::conditional_t<std::is_void_v<T>, std::monostate, T>;

namespace detail
{

// ───────────────────────────────────────────────────────────────────────────
//  Awaiter for All: waits all, returns tuple<Ret<Ts>...>
// ───────────────────────────────────────────────────────────────────────────
template <typename... Ts>
struct AllAwaiter : AwaiterBase
{
    std::tuple<Coro<Ts>...> coros;
    std::tuple<Ret<Ts>...>  results;
    std::size_t             remaining;
    std::coroutine_handle<> continuation;

    AllAwaiter(Coro<Ts>&&... cs)
        : coros(std::move(cs)...), remaining(sizeof...(Ts))
    {
    }

    bool await_ready() const noexcept
    {
        return remaining == 0;
    }

    void await_suspend(std::coroutine_handle<> h) noexcept
    {
        continuation = h;
        resume_all(std::index_sequence_for<Ts...>{});
    }

    auto await_resume() noexcept
    {
        return results;
    }

    void OnWaitComplete(std::coroutine_handle<> h) noexcept override
    {
        store_result(h, std::index_sequence_for<Ts...>{});
        if (--remaining == 0)
            continuation.resume();
    }

private:
    template <std::size_t... Is>
    void resume_all(std::index_sequence<Is...>)
    {
        (resume_one<Is>(), ...);
    }

    template <std::size_t I>
    void resume_one()
    {
        auto& c                  = std::get<I>(coros);
        auto  handle             = c.mHandle;
        handle.promise().awaiter = this;
        handle.resume();
    }

    template <std::size_t... Is>
    void store_result(std::coroutine_handle<> h, std::index_sequence<Is...>) noexcept
    {
        (store_one<Is>(h), ...);
    }

    template <std::size_t I>
    void store_one(std::coroutine_handle<> h) noexcept
    {
        using T       = std::tuple_element_t<I, std::tuple<Ts...>>;
        using U       = Ret<T>;
        using HandleT = typename Coro<T>::handle_type;
        auto  done    = HandleT::from_address(h.address());
        auto& c       = std::get<I>(coros);
        if (done.address() == c.mHandle.address())
        {
            if constexpr (std::is_void_v<T>)
            {
                std::get<I>(results) = std::monostate{};
            }
            else
            {
                std::get<I>(results) = std::move(done.promise().value);
            }
        }
    }
};

// ───────────────────────────────────────────────────────────────────────────
//  Awaiter for Any: waits first, returns tuple<optional<Ret<Ts>>...>
// ───────────────────────────────────────────────────────────────────────────
template <typename... Ts>
struct AnyAwaiter : AwaiterBase
{
    std::tuple<Coro<Ts>...>               coros;
    std::tuple<std::optional<Ret<Ts>>...> results;
    std::atomic<bool>                     triggered{false};
    std::coroutine_handle<>               continuation;

    AnyAwaiter(Coro<Ts>&&... cs)
        : coros(std::move(cs)...), results()
    {
    }

    bool await_ready() const noexcept
    {
        return false;
    }

    void await_suspend(std::coroutine_handle<> h) noexcept
    {
        continuation = h;
        resume_all(std::index_sequence_for<Ts...>{});
    }

    auto await_resume() noexcept
    {
        return results;
    }

    void OnWaitComplete(std::coroutine_handle<> h) noexcept override
    {
        if (!triggered.exchange(true))
        {
            store_result(h, std::index_sequence_for<Ts...>{});
            continuation.resume();
        }
    }

private:
    template <std::size_t... Is>
    void resume_all(std::index_sequence<Is...>)
    {
        (resume_one<Is>(), ...);
    }

    template <std::size_t I>
    void resume_one()
    {
        auto& c                  = std::get<I>(coros);
        auto  handle             = c.mHandle;
        handle.promise().awaiter = this;
        handle.resume();
    }

    template <std::size_t... Is>
    void store_result(std::coroutine_handle<> h, std::index_sequence<Is...>) noexcept
    {
        (store_one<Is>(h), ...);
    }

    template <std::size_t I>
    void store_one(std::coroutine_handle<> h) noexcept
    {
        using T       = std::tuple_element_t<I, std::tuple<Ts...>>;
        using U       = Ret<T>;
        using HandleT = typename Coro<T>::handle_type;
        auto  done    = HandleT::from_address(h.address());
        auto& c       = std::get<I>(coros);
        if (done.address() == c.mHandle.address())
        {
            if constexpr (std::is_void_v<T>)
            {
                std::get<I>(results) = std::monostate{};
            }
            else
            {
                std::get<I>(results) = std::move(done.promise().value);
            }
        }
    }
};

} // namespace detail

// ───────────────────────────────────────────────────────────────────────────
//  Public API
// ───────────────────────────────────────────────────────────────────────────

// All: returns tuple<Ret<Ts>...>
template <typename... Ts>
auto All(Coro<Ts>... coros)
{
    return detail::AllAwaiter<Ts...>(std::move(coros)...);
}

// Any: returns tuple<optional<Ret<Ts>>...>
template <typename... Ts>
auto Any(Coro<Ts>... coros)
{
    return detail::AnyAwaiter<Ts...>(std::move(coros)...);
}