#pragma once

#include "defines.h"
#include "promise.h"
#include "singleawaiter.h"
#include "timequeue.h"
#include "tmplany.h"

#include <any>
#include <atomic>
#include <cassert>
#include <coroutine>
#include <functional>
#include <memory>

namespace tokoro
{

using namespace internal;

class Wait
{
public:
    Wait(double sec);

    Wait();

    ~Wait();

    bool await_ready() const noexcept;

    template <typename T>
    void await_suspend(std::coroutine_handle<Promise<T>> handle) noexcept;

    void await_resume() const noexcept;

    void Resume();

private:
    std::optional<TimeQueue<Wait*>::Iterator> mExeIter;
    TimePoint                                 mWhen;
    std::coroutine_handle<PromiseBase>        mHandle = nullptr;
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
class Any;

template <typename... Ts>
class All;

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
    friend class All;
    template <typename... Ts>
    friend class Any;

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

        while (mExecuteQueue.CheckUpdate())
        {
            mExecuteQueue.Pop()->Resume();
        }
    }

private:
    template <typename T>
    friend class Handle;
    template <typename T>
    friend class Async;
    friend PromiseBase;
    friend Wait;

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
    TimeQueue<Wait*>                    mExecuteQueue;
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
void Wait::await_suspend(std::coroutine_handle<Promise<T>> handle) noexcept
{
    mHandle  = std::coroutine_handle<PromiseBase>::from_address(handle.address());
    mExeIter = mHandle.promise().GetScheduler()->mExecuteQueue.AddTimed(mWhen, this);
}

inline Wait::Wait(double sec)
    : mWhen(Clock::now() + std::chrono::duration_cast<ClockDuration>(std::chrono::duration<double>(sec)))
{
}

inline Wait::Wait() : mWhen(TimePoint::min())
{
}

inline Wait::~Wait()
{
    if (mExeIter.has_value())
        mHandle.promise().GetScheduler()->mExecuteQueue.Remove(*mExeIter);
}

inline bool Wait::await_ready() const noexcept
{
    return false;
}

inline void Wait::await_resume() const noexcept
{
}

inline void Wait::Resume()
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
class All : public CoroAwaiterBase
{
private:
    std::tuple<Async<Ts>...>           mWaitedCoros;
    std::tuple<RetConvert<Ts>...>      mResults;
    std::size_t                        mRemainingCount;
    std::coroutine_handle<PromiseBase> mParentHandle;

public:
    All(Async<Ts>&&... cs)
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

        auto resumeWithIndexes = [this]<std::size_t... Is>(std::index_sequence<Is...>) {
            (
                [this] {
                    auto& coro    = std::get<Is>(mWaitedCoros);
                    auto  handle  = coro.GetHandle();
                    auto& promise = handle.promise();
                    promise.SetScheduler(mParentHandle.promise().GetScheduler());
                    promise.SetParentAwaiter(this);
                    handle.resume();
                }(),
                ...);
        };

        resumeWithIndexes(std::index_sequence_for<Ts...>{});
    }

    auto await_resume() noexcept
    {
        return mResults;
    }

    void OnWaitComplete(std::coroutine_handle<> h) noexcept override
    {
        auto findAndStoreWithIndexes = [this, h]<std::size_t... Is>(std::index_sequence<Is...>) {
            ([this, h] {
                auto& coro = std::get<Is>(mWaitedCoros);
                if (coro.GetHandle().address() == h.address())
                {
                    using T = std::tuple_element_t<Is, std::tuple<Ts...>>;
                    if constexpr (std::is_void_v<T>)
                    {
                        std::get<Is>(mResults) = std::monostate{};
                    }
                    else
                    {
                        using HandleT          = typename Async<T>::handle_type;
                        auto done              = HandleT::from_address(h.address());
                        std::get<Is>(mResults) = std::move(done.promise().GetReturnValue());
                    }
                }
            }(),
             ...);
        };

        findAndStoreWithIndexes(std::index_sequence_for<Ts...>{});

        if (--mRemainingCount == 0)
            mParentHandle.resume();
    }
};

//  Awaiter for Any: waits first, returns tuple<optional<T1>, optional<T2>, optional<T2>...>
//
template <typename... Ts>
class Any : public CoroAwaiterBase
{
private:
    std::tuple<Async<Ts>...>                     mWaitedCoros;
    std::tuple<std::optional<RetConvert<Ts>>...> mResults;
    bool                                         mTriggered{false};
    std::coroutine_handle<PromiseBase>           mParentHandle;

public:
    Any(Async<Ts>&&... cs)
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

        auto resumeWithIndexes = [this]<std::size_t... Is>(std::index_sequence<Is...>) {
            ([this] {
                auto& coro    = std::get<Is>(mWaitedCoros);
                auto  handle  = coro.GetHandle();
                auto& promise = handle.promise();
                promise.SetScheduler(mParentHandle.promise().GetScheduler());
                promise.SetParentAwaiter(this);
                handle.resume();
            }(),
             ...);
        };
        resumeWithIndexes(std::index_sequence_for<Ts...>{});
    }

    auto await_resume() noexcept
    {
        return mResults;
    }

    void OnWaitComplete(std::coroutine_handle<> h) noexcept override
    {
        if (!mTriggered)
        {
            auto checkStoreWithIndexes = [this, h]<std::size_t... Is>(std::index_sequence<Is...>) {
                ([this, h] {
                    auto& coro = std::get<Is>(mWaitedCoros);
                    if (coro.GetHandle().address() != h.address())
                        return;

                    using T = std::tuple_element_t<Is, std::tuple<Ts...>>;
                    if constexpr (std::is_void_v<T>)
                    {
                        std::get<Is>(mResults) = std::monostate{};
                    }
                    else
                    {
                        using HandleT          = typename Async<T>::handle_type;
                        auto done              = HandleT::from_address(h.address());
                        std::get<Is>(mResults) = std::move(done.promise().GetReturnValue());
                    }
                }(),
                 ...);
            };
            checkStoreWithIndexes(std::index_sequence_for<Ts...>{});

            mTriggered = true;
            mParentHandle.resume();
        }
    }
};

static Scheduler& GlobalScheduler()
{
    static Scheduler s;
    return s;
}

#include "promise.inl"

} // namespace tokoro