#pragma once

#include "tmplany.h"

#include <any>
#include <assert.h>
#include <atomic>
#include <cassert>
#include <chrono>
#include <coroutine>
#include <functional>
#include <memory>
#include <set>

namespace tokoro
{

using Clock         = std::chrono::steady_clock;
using TimePoint     = Clock::time_point;
using ClockDuration = Clock::duration;

struct CoroAwaiterBase
{
    virtual void OnWaitComplete(std::coroutine_handle<>) noexcept = 0;
    virtual ~CoroAwaiterBase()                                    = default;
};

class Scheduler;

class PromiseBase
{
public:
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
        void await_suspend(std::coroutine_handle<> h) const noexcept;

        void await_resume() const noexcept
        {
        }
    };

    auto final_suspend() noexcept
    {
        return FinalAwaiter{};
    }

    void unhandled_exception()
    {
        std::terminate();
    }

    void SetId(uint64_t id)
    {
        mId = id;
    }

    void SetScheduler(Scheduler* scheduler)
    {
        mScheduler = scheduler;
    }

    Scheduler* GetScheduler() const
    {
        return mScheduler;
    }

    void SetParentAwaiter(CoroAwaiterBase* awaiter)
    {
        mParentAwaiter = awaiter;
    }

protected:
    std::any         mReturnValue;
    uint64_t         mId            = 0;
    CoroAwaiterBase* mParentAwaiter = nullptr;
    Scheduler*       mScheduler     = nullptr;
};

template <typename T>
class Promise : public PromiseBase
{
public:
    using Handle = std::coroutine_handle<Promise<T>>;

    auto get_return_object() noexcept
    {
        return Handle::from_promise(*this);
    }

    void return_value(T&& val)
    {
        this->mReturnValue = std::forward<T>(val);
    }

    void return_value(const T& val)
    {
        this->mReturnValue = val;
    }

    T& GetReturnValue()
    {
        return *std::any_cast<T>(&this->mReturnValue);
    }
};

template <>
class Promise<void> : public PromiseBase
{
public:
    using Handle = std::coroutine_handle<Promise<void>>;

    auto get_return_object() noexcept
    {
        return Handle::from_promise(*this);
    }

    void return_void()
    {
    }
};

template <typename T>
class Coro
{
public:
    using promise_type = Promise<T>;
    using value_type   = T;
    using handle_type  = std::coroutine_handle<promise_type>;

    Coro(handle_type h)
        : mHandle(h)
    {
    }

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

    auto operator co_await() noexcept;

private:
    std::coroutine_handle<> mHandle;
};

template <typename T>
class SingleCoroAwaiter : public CoroAwaiterBase
{
public:
    SingleCoroAwaiter(std::coroutine_handle<Promise<T>> handle)
        : mWaitedHandle(handle)
    {
    }

    bool await_ready() const noexcept
    {
        return false;
    }

    template <typename U>
    void await_suspend(std::coroutine_handle<Promise<U>> handle) noexcept
    {
        mParentHandle = std::coroutine_handle<PromiseBase>::from_address(handle.address());

        auto& promise = mWaitedHandle.promise();
        promise.SetScheduler(mParentHandle.promise().GetScheduler());
        promise.SetParentAwaiter(this);

        mWaitedHandle.resume(); // Kick off child Coro<T>
    }

    auto await_resume() const noexcept
        requires(!std::is_void_v<T>)
    {
        return mWaitedHandle.promise().GetReturnValue();
    }

    void await_resume() const noexcept
        requires(std::is_void_v<T>)
    {
    }

    void OnWaitComplete(std::coroutine_handle<> /*unused*/) noexcept override
    {
        mParentHandle.resume();
    }

private:
    std::coroutine_handle<Promise<T>>  mWaitedHandle;
    std::coroutine_handle<PromiseBase> mParentHandle;
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

    TimeQueue()
    {
        mUpdatePtr = mSet.end();
    }

    void Clear()
    {
        mSet.clear();
        mAddOrder   = 0;
        mAddFrame   = 0;
        mUpdatePtr  = mSet.end();
        mCurExeTime = TimePoint::min();
    }

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
class CoroHandle
{
public:
    CoroHandle(CoroHandle&& other);
    CoroHandle(const CoroHandle& other) = delete;
    ~CoroHandle();

    bool IsDown() const noexcept;
    void Stop() const noexcept;

    std::optional<T> GetReturn() const noexcept
        requires(!std::is_void_v<T>);

private:
    friend Scheduler;

    CoroHandle(uint64_t id, Scheduler* scheduler, const std::weak_ptr<std::monostate>& liveSignal)
        : mId(id), mScheduler(scheduler), mSchedulerLiveSignal(liveSignal)
    {
    }

    uint64_t                      mId        = 0;
    Scheduler*                    mScheduler = nullptr;
    std::weak_ptr<std::monostate> mSchedulerLiveSignal;
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
    CoroHandle<RetType> Start(Task&& task, Args&&... args)
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

        return CoroHandle<RetType>{id, this, mLiveSignal};
    }

    void Update();

private:
    template <typename T>
    friend class CoroHandle;
    template <typename T>
    friend class Coro;
    friend PromiseBase;
    friend TimeAwaiter;

    void Release(uint64_t id);
    bool IsDown(uint64_t id);
    void Stop(uint64_t id);

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
        TmplAny<Coro>                  coro;
        std::function<TmplAny<Coro>()> lambda;
        bool                           finished = false;
        bool                           released = false;
        std::any                       returnValue;
    };

    std::unordered_map<uint64_t, Entry> mCoroutines;
    std::atomic<uint64_t>               mNextId{1};
    TimeQueue<TimeAwaiter*>             mExecuteQueue;
    std::shared_ptr<std::monostate>     mLiveSignal;
};

static Scheduler& GlobalScheduler()
{
    static Scheduler s;
    return s;
}

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
            mHandle.promise().GetScheduler()->mExecuteQueue.Remove(*mExeIter);
    }

    bool await_ready() const noexcept
    {
        return false;
    }

    template <typename T>
    void await_suspend(std::coroutine_handle<Promise<T>> handle) noexcept
    {
        mHandle  = std::coroutine_handle<PromiseBase>::from_address(handle.address());
        mExeIter = mHandle.promise().GetScheduler()->mExecuteQueue.AddTimed(mWhen, this);
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
auto Coro<T>::operator co_await() noexcept
{
    return SingleCoroAwaiter(GetHandle());
}

template <typename T>
CoroHandle<T>::CoroHandle(CoroHandle&& other)
    : mId(other.mId), mScheduler(other.mScheduler), mSchedulerLiveSignal(other.mSchedulerLiveSignal)
{
    other.mId        = 0;
    other.mScheduler = nullptr;
    other.mSchedulerLiveSignal.reset();
}

template <typename T>
CoroHandle<T>::~CoroHandle()
{
    if (mId != 0 && !mSchedulerLiveSignal.expired())
    {
        mScheduler->Release(mId);
    }
}

template <typename T>
bool CoroHandle<T>::IsDown() const noexcept
{
    return mSchedulerLiveSignal.expired() || mScheduler->IsDown(mId);
}

template <typename T>
void CoroHandle<T>::Stop() const noexcept
{
    if (!mSchedulerLiveSignal.expired())
        mScheduler->Stop(mId);
}

template <typename T>
std::optional<T> CoroHandle<T>::GetReturn() const noexcept
    requires(!std::is_void_v<T>)
{
    if (mSchedulerLiveSignal.expired())
        return std::nullopt;
    return mScheduler->GetReturn<T>(mId);
}

TimeAwaiter NextFrame() noexcept
{
    return TimeAwaiter();
}

TimeAwaiter Wait(double sec) noexcept
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
    assert(it != mCoroutines.end() && !it->second.released);

    it->second.released = true;
    if (it->second.released && it->second.finished)
        mCoroutines.erase(it);
}

inline bool Scheduler::IsDown(uint64_t id)
{
    const auto it = mCoroutines.find(id);
    assert(it != mCoroutines.end());
    return it->second.finished;
}

inline void Scheduler::Stop(uint64_t id)
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
std::optional<T> Scheduler::GetReturn(uint64_t id)
{
    return std::any_cast<T>(mCoroutines[id].returnValue);
}

void PromiseBase::FinalAwaiter::await_suspend(std::coroutine_handle<> h) const noexcept
{
    auto             handle        = std::coroutine_handle<PromiseBase>::from_address(h.address());
    auto&            promise       = handle.promise();
    const uint64_t   coroId        = promise.mId;
    CoroAwaiterBase* parentAwaiter = promise.mParentAwaiter;

    // Can't have awaiter and coroId both.
    assert(parentAwaiter == nullptr || coroId == 0);

    if (parentAwaiter != nullptr)
    {
        parentAwaiter->OnWaitComplete(h);
    }
    else if (coroId != 0)
    {
        promise.GetScheduler()->OnCoroutineFinished(coroId, std::move(promise.mReturnValue));
    }
}

// ¡ª¡ª All & Any ¡ª¡ª

namespace detail
{

// map void to monostate
template <typename T>
using Ret = std::conditional_t<std::is_void_v<T>, std::monostate, T>;

// ©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤
//  Awaiter for All: waits all, returns tuple<Ret<Ts>...>
// ©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤
template <typename... Ts>
struct AllAwaiter : CoroAwaiterBase
{
    std::tuple<Coro<Ts>...>            mWaitedCoros;
    std::tuple<Ret<Ts>...>             mResults;
    std::size_t                        mRemainingCount;
    std::coroutine_handle<PromiseBase> mParentHandle;

    AllAwaiter(Coro<Ts>&&... cs)
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
        using HandleT = typename Coro<T>::handle_type;
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

// ©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤
//  Awaiter for Any: waits first, returns tuple<optional<Ret<Ts>>...>
// ©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤
template <typename... Ts>
struct AnyAwaiter : CoroAwaiterBase
{
    std::tuple<Coro<Ts>...>               mWaitedCoros;
    std::tuple<std::optional<Ret<Ts>>...> mResults;
    bool                                  mTriggered{false};
    std::coroutine_handle<PromiseBase>    mParentHandle;

    AnyAwaiter(Coro<Ts>&&... cs)
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
        using HandleT = typename Coro<T>::handle_type;
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

} // namespace detail

// ©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤
//  Public API
// ©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤©¤

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

} // namespace tokoro