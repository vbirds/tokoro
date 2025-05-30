#pragma once

#include "defines.h"
#include "promise.h"
#include "singleawaiter.h"
#include "timequeue.h"
#include "tmplany.h"

#include <any>
#include <assert.h>
#include <atomic>
#include <cassert>
#include <coroutine>
#include <functional>
#include <memory>

namespace tokoro
{

using namespace internal;

class TimeAwaiter
{
public:
    TimeAwaiter(double sec);

    TimeAwaiter();

    virtual ~TimeAwaiter();

    bool await_ready() const noexcept;

    template <typename T>
    void await_suspend(std::coroutine_handle<Promise<T>> handle) noexcept;

    void await_resume() const noexcept;

    void Resume();

private:
    std::optional<TimeQueue<TimeAwaiter*>::Iterator> mExeIter;
    TimePoint                                        mWhen;
    std::coroutine_handle<PromiseBase>               mHandle = nullptr;
};

template <typename T>
class Handle
{
public:
    Handle(Handle&& other);
    Handle(const Handle& other) = delete;
    ~Handle();

    bool IsDown() const noexcept;
    void Stop() const noexcept;

    std::optional<T> GetReturn() const noexcept
        requires(!std::is_void_v<T>);

private:
    friend Scheduler;

    Handle(uint64_t id, Scheduler* scheduler, const std::weak_ptr<std::monostate>& liveSignal)
        : mId(id), mScheduler(scheduler), mSchedulerLiveSignal(liveSignal)
    {
    }

    uint64_t                      mId        = 0;
    Scheduler*                    mScheduler = nullptr;
    std::weak_ptr<std::monostate> mSchedulerLiveSignal;
};

template <typename... Ts>
struct AnyAwaiter;

template <typename... Ts>
struct AllAwaiter;

template <typename T>
class Async
{
public:
    using promise_type = Promise<T>;
    using value_type   = T;
    using handle_type  = std::coroutine_handle<promise_type>;

    Async(handle_type h)
        : mHandle(h)
    {
    }

    Async(Async&& o)
        : mHandle(o.mHandle)
    {
        o.mHandle = nullptr;
    }

    ~Async()
    {
        if (mHandle)
            mHandle.destroy();
    }

    auto operator co_await() noexcept
    {
        return SingleCoroAwaiter(GetHandle());
    }

private:
    template <typename U>
    friend class Handle;
    friend class Scheduler;
    template <typename... Ts>
    friend struct AllAwaiter;
    template <typename... Ts>
    friend struct AnyAwaiter;

    void SetId(uint64_t id)
    {
        GetHandle().promise().SetId(id);
    }

    void SetScheduler(Scheduler* scheduler)
    {
        GetHandle().promise().SetScheduler(scheduler);
    }

    std::coroutine_handle<promise_type> GetHandle()
    {
        return std::coroutine_handle<promise_type>::from_address(mHandle.address());
    }

    void Resume()
    {
        mHandle.resume();
    }

    std::coroutine_handle<> mHandle;
};

class Scheduler
{
public:
    Scheduler()
    {
        mLiveSignal = std::make_shared<std::monostate>();
    }

    ~Scheduler()
    {
        mCoroutines.clear();
        mExecuteQueue.Clear();
    }

    template <typename Task, typename... Args, typename RetType = typename std::decay_t<std::invoke_result_t<Task, Args...>>::value_type>
    Handle<RetType> Start(Task&& task, Args&&... args)
    {
        uint64_t id          = mNextId++;
        auto [iter, succeed] = mCoroutines.emplace(id, Entry());

        Entry& newEntry = iter->second;

        // Cache the input function and parameters into a lambda to avoid the famous C++ coroutine pitfall.
        // https://devblogs.microsoft.com/oldnewthing/20211103-00/?p=105870
        // <A capturing lambda can be a coroutine, but you have to save your captures while you still can>
        newEntry.lambda = [task = std::forward<Task>(task), tup = std::make_tuple(std::forward<Args>(args)...)]() mutable {
            return std::apply(task, tup);
        };

        // Create the Coro<T>
        newEntry.coro = newEntry.lambda();

        auto& newCoro = newEntry.coro.WithTmplArg<RetType>();
        newCoro.SetId(id);
        newCoro.SetScheduler(this);

        // Kick off the coroutine.
        newCoro.Resume();

        return Handle<RetType>{id, this, mLiveSignal};
    }

    void Update()
    {
        mExecuteQueue.SetupUpdate(Clock::now());

        while (!mExecuteQueue.UpdateEnded())
        {
            mExecuteQueue.Pop().value()->Resume();
        }
    }

private:
    template <typename T>
    friend class Handle;
    template <typename T>
    friend class Async;
    friend PromiseBase;
    friend TimeAwaiter;

    void Release(uint64_t id)
    {
        auto it = mCoroutines.find(id);
        assert(it != mCoroutines.end() && !it->second.released);

        it->second.released = true;
        if (it->second.released && it->second.finished)
            mCoroutines.erase(it);
    }

    bool IsDown(uint64_t id)
    {
        const auto it = mCoroutines.find(id);
        assert(it != mCoroutines.end());
        return it->second.finished;
    }

    void Stop(uint64_t id)
    {
        const auto it = mCoroutines.find(id);
        assert(it != mCoroutines.end());

        if (!it->second.finished)
        {
            it->second.finished = true;
            it->second.coro.Reset();
            it->second.lambda = {};

            if (it->second.released && it->second.finished)
                mCoroutines.erase(it);
        }
    }

    template <typename T>
    std::optional<T> GetReturn(uint64_t id);

    void OnCoroutineFinished(uint64_t id, std::any&& result)
    {
        auto& e       = mCoroutines[id];
        e.returnValue = std::move(result);
        Stop(id);
    }

    struct Entry
    {
        TmplAny<Async>                  coro;
        std::function<TmplAny<Async>()> lambda;
        bool                            finished = false;
        bool                            released = false;
        std::any                        returnValue;
    };

    std::unordered_map<uint64_t, Entry> mCoroutines;
    std::atomic<uint64_t>               mNextId{1};
    TimeQueue<TimeAwaiter*>             mExecuteQueue;
    std::shared_ptr<std::monostate>     mLiveSignal;
};

// Handle functions
//
template <typename T>
Handle<T>::Handle(Handle&& other)
    : mId(other.mId), mScheduler(other.mScheduler), mSchedulerLiveSignal(other.mSchedulerLiveSignal)
{
    other.mId        = 0;
    other.mScheduler = nullptr;
    other.mSchedulerLiveSignal.reset();
}

template <typename T>
Handle<T>::~Handle()
{
    if (mId != 0 && !mSchedulerLiveSignal.expired())
    {
        mScheduler->Release(mId);
    }
}

template <typename T>
bool Handle<T>::IsDown() const noexcept
{
    return mSchedulerLiveSignal.expired() || mScheduler->IsDown(mId);
}

template <typename T>
void Handle<T>::Stop() const noexcept
{
    if (!mSchedulerLiveSignal.expired())
        mScheduler->Stop(mId);
}

template <typename T>
std::optional<T> Handle<T>::GetReturn() const noexcept
    requires(!std::is_void_v<T>)
{
    if (mSchedulerLiveSignal.expired())
        return std::nullopt;
    return mScheduler->GetReturn<T>(mId);
}

// TimeAwaiter functions
//
template <typename T>
void TimeAwaiter::await_suspend(std::coroutine_handle<Promise<T>> handle) noexcept
{
    mHandle  = std::coroutine_handle<PromiseBase>::from_address(handle.address());
    mExeIter = mHandle.promise().GetScheduler()->mExecuteQueue.AddTimed(mWhen, this);
}

inline TimeAwaiter::TimeAwaiter(double sec)
    : mWhen(Clock::now() + std::chrono::duration_cast<ClockDuration>(std::chrono::duration<double>(sec)))
{
}

inline TimeAwaiter::TimeAwaiter() : mWhen(TimePoint::min())
{
}

inline TimeAwaiter::~TimeAwaiter()
{
    if (mExeIter.has_value())
        mHandle.promise().GetScheduler()->mExecuteQueue.Remove(*mExeIter);
}

inline bool TimeAwaiter::await_ready() const noexcept
{
    return false;
}

inline void TimeAwaiter::await_resume() const noexcept
{
}

inline void TimeAwaiter::Resume()
{
    assert(mHandle && !mHandle.done() && mExeIter.has_value());
    // mExeIter has been removed from mExecuteQueue before enter Resume().
    mExeIter.reset();
    mHandle.resume();
}

template <typename T>
std::optional<T> Scheduler::GetReturn(uint64_t id)
{
    return std::any_cast<T>(mCoroutines[id].returnValue);
}

//  Awaiter for All: waits all, returns tuple<T1, T2, T3 ...>
//
template <typename... Ts>
class AllAwaiter : CoroAwaiterBase
{
private:
    std::tuple<Async<Ts>...>           mWaitedCoros;
    std::tuple<RetConvert<Ts>...>      mResults;
    std::size_t                        mRemainingCount;
    std::coroutine_handle<PromiseBase> mParentHandle;

public:
    AllAwaiter(Async<Ts>&&... cs)
        : mWaitedCoros(std::move(cs)...), mRemainingCount(sizeof...(Ts))
    {
    }

    bool await_ready() const noexcept
    {
        return mRemainingCount == 0;
    }

    template <typename T>
    void await_suspend(std::coroutine_handle<Promise<T>> h) noexcept
    {
        mParentHandle = std::coroutine_handle<PromiseBase>::from_address(h.address());
        resume_all(std::index_sequence_for<Ts...>{});
    }

    auto await_resume() noexcept
    {
        return mResults;
    }

    void OnWaitComplete(std::coroutine_handle<> h) noexcept override
    {
        store_result(h, std::index_sequence_for<Ts...>{});
        if (--mRemainingCount == 0)
            mParentHandle.resume();
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
        auto& coro   = std::get<I>(mWaitedCoros);
        auto  handle = coro.GetHandle();

        auto& promise = handle.promise();
        promise.SetScheduler(mParentHandle.promise().GetScheduler());
        promise.SetParentAwaiter(this);

        handle.resume(); // Kick off sub Coro<T>
    }

    template <std::size_t... Is>
    void store_result(std::coroutine_handle<> h, std::index_sequence<Is...>) noexcept
    {
        (store_one<Is>(h), ...);
    }

    template <std::size_t Index>
    void store_one(std::coroutine_handle<> h) noexcept
    {
        using T       = std::tuple_element_t<Index, std::tuple<Ts...>>;
        using HandleT = typename Async<T>::handle_type;
        auto  done    = HandleT::from_address(h.address());
        auto& coro    = std::get<Index>(mWaitedCoros);
        if (done.address() == coro.GetHandle().address())
        {
            if constexpr (std::is_void_v<T>)
            {
                std::get<Index>(mResults) = std::monostate{};
            }
            else
            {
                std::get<Index>(mResults) = std::move(done.promise().GetReturnValue());
            }
        }
    }
};

//  Awaiter for Any: waits first, returns tuple<optional<T1>, optional<T2>, optional<T2>...>
//
template <typename... Ts>
class AnyAwaiter : CoroAwaiterBase
{
private:
    std::tuple<Async<Ts>...>                     mWaitedCoros;
    std::tuple<std::optional<RetConvert<Ts>>...> mResults;
    bool                                         mTriggered{false};
    std::coroutine_handle<PromiseBase>           mParentHandle;

public:
    AnyAwaiter(Async<Ts>&&... cs)
        : mWaitedCoros(std::move(cs)...), mResults()
    {
    }

    bool await_ready() const noexcept
    {
        return false;
    }

    template <typename T>
    void await_suspend(std::coroutine_handle<Promise<T>> h) noexcept
    {
        mParentHandle = std::coroutine_handle<PromiseBase>::from_address(h.address());
        resume_all(std::index_sequence_for<Ts...>{});
    }

    auto await_resume() noexcept
    {
        return mResults;
    }

    void OnWaitComplete(std::coroutine_handle<> h) noexcept override
    {
        if (!mTriggered)
        {
            store_result(h, std::index_sequence_for<Ts...>{});
            mParentHandle.resume();
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
        auto& coro   = std::get<I>(mWaitedCoros);
        auto  handle = coro.GetHandle();

        auto& promise = handle.promise();
        promise.SetScheduler(mParentHandle.promise().GetScheduler());
        promise.SetParentAwaiter(this);

        handle.resume(); // Kick off sub Coro<T>
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
        using HandleT = typename Async<T>::handle_type;
        auto  done    = HandleT::from_address(h.address());
        auto& coro    = std::get<I>(mWaitedCoros);
        if (done.address() == coro.GetHandle().address())
        {
            if constexpr (std::is_void_v<T>)
            {
                std::get<I>(mResults) = std::monostate{};
            }
            else
            {
                std::get<I>(mResults) = std::move(done.promise().GetReturnValue());
            }
        }
    }
};

template <typename... Ts>
auto All(Async<Ts>... coros)
{
    return AllAwaiter<Ts...>(std::move(coros)...);
}

template <typename... Ts>
auto Any(Async<Ts>... coros)
{
    return AnyAwaiter<Ts...>(std::move(coros)...);
}

TimeAwaiter NextFrame() noexcept
{
    return TimeAwaiter();
}

TimeAwaiter Wait(double sec) noexcept
{
    return TimeAwaiter(sec);
}

static Scheduler& GlobalScheduler()
{
    static Scheduler s;
    return s;
}

#include "promise.inl"

} // namespace tokoro