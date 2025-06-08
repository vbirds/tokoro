#pragma once

#include "defines.h"
#include "promise.h"
#include "singleawaiter.h"
#include "timequeue.h"
#include "tmplany.h"

#include <any>
#include <array>
#include <cassert>
#include <chrono>
#include <coroutine>
#include <functional>
#include <memory>
#include <optional>

namespace tokoro
{

template <internal::CountEnum UpdateEnum, internal::CountEnum TimeEnum>
class SchedulerBP;

template <internal::CountEnum UpdateEnum, internal::CountEnum TimeEnum>
class WaitBP
{
public:
    WaitBP(double sec, UpdateEnum updateType = internal::GetEnumDefault<UpdateEnum>(), TimeEnum timeType = internal::GetEnumDefault<TimeEnum>());
    WaitBP(UpdateEnum updateType = internal::GetEnumDefault<UpdateEnum>(), TimeEnum timeType = internal::GetEnumDefault<TimeEnum>());
    ~WaitBP();

    // Functions for C++ coroutine callbacks
    //
    bool await_ready() const noexcept;
    template <typename T>
    void await_suspend(std::coroutine_handle<internal::Promise<T>> handle) noexcept;
    void await_resume() const noexcept;

    void Resume();

private:
    friend class SchedulerBP<UpdateEnum, TimeEnum>;

    std::optional<typename internal::TimeQueue<WaitBP*>::Iterator> mExeIter;
    double                                                         mDelay;
    std::coroutine_handle<internal::PromiseBase>                   mHandle = nullptr;
    UpdateEnum                                                     mUpdateType;
    TimeEnum                                                       mTimeType;
};

template <typename T, typename UpdateEnum, typename TimeEnum>
class HandleBP
{
public:
    HandleBP(HandleBP&& other);
    HandleBP(const HandleBP& other) = delete;
    ~HandleBP();

    bool IsDown() const noexcept;
    void Stop() const noexcept;

    std::optional<T> TakeResult() const
        requires(!std::is_void_v<T>);

    void TakeResult() const
        requires(std::is_void_v<T>);

private:
    friend class SchedulerBP<UpdateEnum, TimeEnum>;

    HandleBP(uint64_t id, SchedulerBP<UpdateEnum, TimeEnum>* scheduler, const std::weak_ptr<std::monostate>& liveSignal)
        : mId(id), mScheduler(scheduler), mSchedulerLiveSignal(liveSignal)
    {
    }

    uint64_t                           mId        = 0;
    SchedulerBP<UpdateEnum, TimeEnum>* mScheduler = nullptr;
    std::weak_ptr<std::monostate>      mSchedulerLiveSignal;
};

template <typename... Ts>
class Any;

template <typename... Ts>
class All;

template <typename T>
class Async
{
public:
    using promise_type = internal::Promise<T>;
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
        return internal::SingleCoroAwaiter(GetHandle());
    }

private:
    template <typename U, typename UpdateEnum, typename TimeEnum>
    friend class HandleBP;
    template <internal::CountEnum UpdateEnum, internal::CountEnum TimeEnum>
    friend class SchedulerBP;
    template <typename... Ts>
    friend class All;
    template <typename... Ts>
    friend class Any;

    void SetId(uint64_t id)
    {
        GetHandle().promise().SetId(id);
    }

    template <typename UpdateEnum, typename TimeEnum>
    void SetScheduler(SchedulerBP<UpdateEnum, TimeEnum>* scheduler)
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

namespace internal
{
// Helper template for Scheduler
//
template <typename Func, typename... Args>
using AsyncReturnT = std::invoke_result_t<Func, Args...>;

template <typename Func, typename... Args>
using AsyncValueT = typename AsyncReturnT<Func, Args...>::value_type;

template <typename Func, typename... Args>
concept ReturnsAsync = std::invocable<Func, Args...> &&
                       std::same_as<AsyncReturnT<Func, Args...>, Async<AsyncValueT<Func, Args...>>>;
} // namespace internal

template <internal::CountEnum UpdateEnum, internal::CountEnum TimeEnum>
class SchedulerBP
{
public:
    template <typename T>
    using Handle = HandleBP<T, UpdateEnum, TimeEnum>;

    SchedulerBP()
    {
        mLiveSignal = std::make_shared<std::monostate>();
    }

    ~SchedulerBP()
    {
        mCoroutines.clear();

        for (auto& queue : mExecuteQueues)
        {
            queue.Clear();
        }
    }

    // SetCustomTimer: Set custom timer for specific time type to replace default realtime timer.
    void SetCustomTimer(TimeEnum timeType, std::function<double()> getTimeFunc)
    {
        mCustomTimers[static_cast<int>(timeType)] = std::move(getTimeFunc);
    }

    /// Start: start a coroutine and return its handle.
    /// func: Callable object that returns Async<T>. Could be a lambda or function.
    /// funcArgs: parameters of AsyncFunc£¬Start will forward them to construct the coroutine.
    template <typename AsyncFunc, typename... Args>
        requires internal::ReturnsAsync<AsyncFunc, Args...> // Constrain that need function to return Async<T>
    Handle<internal::AsyncValueT<AsyncFunc, Args...>> Start(AsyncFunc&& func, Args&&... funcArgs)
    {
        using RetType = internal::AsyncValueT<AsyncFunc, Args...>;

        uint64_t id          = mNextId++;
        auto [iter, succeed] = mCoroutines.emplace(id, Entry());

        Entry& newEntry = iter->second;

        // Cache the input function and parameters into a lambda to avoid the famous C++ coroutine pitfall.
        // https://devblogs.microsoft.com/oldnewthing/20211103-00/?p=105870
        // <A capturing lambda can be a coroutine, but you have to save your captures while you still can>
        newEntry.lambda = [task = std::forward<AsyncFunc>(func), tup = std::make_tuple(std::forward<Args>(funcArgs)...)]() mutable {
            return std::apply(task, tup);
        };

        // Create the Coro<T>
        newEntry.coro = newEntry.lambda();

        auto& newCoro = newEntry.coro.template WithTmplArg<RetType>();
        newCoro.SetId(id);
        newCoro.SetScheduler(this);

        // Kick off the coroutine.
        newCoro.Resume();

        return Handle<RetType>{id, this, mLiveSignal};
    }

    void Update(UpdateEnum updateType = UpdateEnum::Update,
                TimeEnum   timeType   = TimeEnum::Realtime)
    {
        auto& timeQueue = GetUpdateQueue(updateType, timeType);
        timeQueue.SetupUpdate(GetCurrentTime(timeType));

        while (timeQueue.CheckUpdate())
        {
            timeQueue.Pop()->Resume();

            StopNewFinishedCoro();
        }
    }

private:
    using MyWait = WaitBP<UpdateEnum, TimeEnum>;

    template <typename T, typename U, typename X>
    friend class HandleBP;
    template <typename T>
    friend class Async;
    friend internal::PromiseBase;
    friend MyWait;

    void Release(uint64_t id)
    {
        auto it = mCoroutines.find(id);
        assert(it != mCoroutines.end() && !it->second.released);

        it->second.released = true;
        if (!it->second.running)
            mCoroutines.erase(it);
    }

    bool IsDown(uint64_t id)
    {
        const auto it = mCoroutines.find(id);
        assert(it != mCoroutines.end());
        return !it->second.running;
    }

    void Stop(uint64_t id)
    {
        const auto it = mCoroutines.find(id);
        assert(it != mCoroutines.end());
        assert(!it->second.released && "Coroutines should not be released, if their handle is trying to stop (Handle still alive).");

        if (it->second.running)
        {
            it->second.running = false;
            it->second.coro.Reset();
            it->second.lambda = {};
        }
    }

    template <typename T>
        requires(!std::is_void_v<T>)
    std::optional<T> GetReturn(uint64_t id)
    {
        // todo coro should be reset in this method. This method is once only.
        auto&     coro   = mCoroutines[id].coro;
        Async<T>& asyncT = coro.template WithTmplArg<T>();
        return asyncT.GetHandle().promise().GetReturnValue();
    }

    template <typename T>
        requires(std::is_void_v<T>)
    void GetReturn(uint64_t id)
    {
        auto&        coro   = mCoroutines[id].coro;
        Async<void>& asyncT = coro.template WithTmplArg<void>();
        asyncT.GetHandle().promise().GetReturnValue();
    }

    void OnCoroutineFinished(uint64_t id)
    {
        // Because delete root coroutine inside FinalAwaiter::await_suspend() will delete
        // the return value receiver of await_suspend() too. Which will lead to use after free
        // issue for 'return std::noop_coroutine();'. So add a delay release mechanic for scheduler
        // managed coroutines.

        assert(id != 0 && "id parameter should never be invalid in this method.");
        assert(mNewFinishedCoro == 0 && "There's already a coro need to be finished. Only one coro at max should be finished in one awaiter resume.");
        mNewFinishedCoro = id;
    }

    void StopNewFinishedCoro()
    {
        if (mNewFinishedCoro == 0)
            return;

        const auto it    = mCoroutines.find(mNewFinishedCoro);
        mNewFinishedCoro = 0;
        Entry& e         = it->second;
        assert(it != mCoroutines.end() && e.running);

        e.running = false;
        e.lambda  = {};

        if (e.released)
            mCoroutines.erase(it);
    }

    int TypesToIndex(UpdateEnum updateType, TimeEnum timeType)
    {
        const int updateIndex = static_cast<int>(updateType);
        const int timeIndex   = static_cast<int>(timeType);
        return updateIndex * static_cast<int>(TimeEnum::Count) + timeIndex;
    }

    internal::TimeQueue<MyWait*>& GetUpdateQueue(UpdateEnum updateType, TimeEnum timeType)
    {
        int queueIndex = TypesToIndex(updateType, timeType);
        return mExecuteQueues[queueIndex];
    }

    std::function<double()>& GetCustomTimer(TimeEnum timeType)
    {
        return mCustomTimers[static_cast<int>(timeType)];
    }

    static double defaultTimer()
    {
        using Clock     = std::chrono::steady_clock;
        using TimePoint = Clock::time_point;

        static TimePoint                    startTime = Clock::now();
        const std::chrono::duration<double> diff      = Clock::now() - startTime;
        return diff.count();
    }

    double GetCurrentTime(TimeEnum timeType)
    {
        auto& customTimer = GetCustomTimer(timeType);
        if (customTimer)
        {
            return customTimer();
        }
        else
        {
            return defaultTimer();
        }
    }

    using WaitIter = typename internal::TimeQueue<MyWait*>::Iterator;
    WaitIter AddWait(MyWait* wait, UpdateEnum updateType, TimeEnum timeType)
    {
        auto& timeQueue = GetUpdateQueue(updateType, timeType);

        double executeTime = 0;
        if (wait->mDelay != 0)
            executeTime = GetCurrentTime(timeType) + wait->mDelay;
        return timeQueue.AddTimed(executeTime, wait);
    }

    void RemoveWait(WaitIter waitHandle, UpdateEnum updateType, TimeEnum timeType)
    {
        auto& timeQueue = GetUpdateQueue(updateType, timeType);
        timeQueue.Remove(waitHandle);
    }

    struct Entry
    {
        internal::TmplAny<Async>                  coro;
        std::function<internal::TmplAny<Async>()> lambda;
        bool                                      running  = true;
        bool                                      released = false;
    };

    static constexpr int UpdateQueueCount = static_cast<int>(UpdateEnum::Count) * static_cast<int>(TimeEnum::Count);

    uint64_t                                                               mNextId{1};
    std::unordered_map<uint64_t, Entry>                                    mCoroutines;
    uint64_t                                                               mNewFinishedCoro = 0;
    std::array<internal::TimeQueue<MyWait*>, UpdateQueueCount>             mExecuteQueues;
    std::array<std::function<double()>, static_cast<int>(TimeEnum::Count)> mCustomTimers;
    std::shared_ptr<std::monostate>                                        mLiveSignal;
};

// Handle functions
//
template <typename T, typename UpdateEnum, typename TimeEnum>
HandleBP<T, UpdateEnum, TimeEnum>::HandleBP(HandleBP&& other)
    : mId(other.mId), mScheduler(other.mScheduler), mSchedulerLiveSignal(other.mSchedulerLiveSignal)
{
    other.mId        = 0;
    other.mScheduler = nullptr;
    other.mSchedulerLiveSignal.reset();
}

template <typename T, typename UpdateEnum, typename TimeEnum>
HandleBP<T, UpdateEnum, TimeEnum>::~HandleBP()
{
    if (mId != 0 && !mSchedulerLiveSignal.expired())
    {
        mScheduler->Release(mId);
    }
}

template <typename T, typename UpdateEnum, typename TimeEnum>
bool HandleBP<T, UpdateEnum, TimeEnum>::IsDown() const noexcept
{
    return mSchedulerLiveSignal.expired() || mScheduler->IsDown(mId);
}

template <typename T, typename UpdateEnum, typename TimeEnum>
void HandleBP<T, UpdateEnum, TimeEnum>::Stop() const noexcept
{
    if (!mSchedulerLiveSignal.expired())
        mScheduler->Stop(mId);
}

template <typename T, typename UpdateEnum, typename TimeEnum>
std::optional<T> HandleBP<T, UpdateEnum, TimeEnum>::TakeResult() const
    requires(!std::is_void_v<T>)
{
    if (mSchedulerLiveSignal.expired())
        return std::nullopt;
    return mScheduler->template GetReturn<T>(mId);
}

template <typename T, typename UpdateEnum, typename TimeEnum>
void HandleBP<T, UpdateEnum, TimeEnum>::TakeResult() const
    requires(std::is_void_v<T>)
{
    if (mSchedulerLiveSignal.expired())
        return;
    mScheduler->template GetReturn<T>(mId);
}

// TimeAwaiter functions
//
template <internal::CountEnum UpdateEnum, internal::CountEnum TimeEnum>
WaitBP<UpdateEnum, TimeEnum>::WaitBP(double sec, UpdateEnum updateType, TimeEnum timeType)
    : mDelay(sec),
      mUpdateType(updateType), mTimeType(timeType)
{
}

template <internal::CountEnum UpdateEnum, internal::CountEnum TimeEnum>
WaitBP<UpdateEnum, TimeEnum>::WaitBP(UpdateEnum updateType, TimeEnum timeType)
    : mDelay(0), mUpdateType(updateType), mTimeType(timeType)
{
}

template <internal::CountEnum UpdateEnum, internal::CountEnum TimeEnum>
WaitBP<UpdateEnum, TimeEnum>::~WaitBP()
{
    if (mExeIter.has_value())
        mHandle.promise().GetScheduler<UpdateEnum, TimeEnum>()->RemoveWait(*mExeIter, mUpdateType, mTimeType);
}

template <internal::CountEnum UpdateEnum, internal::CountEnum TimeEnum>
bool WaitBP<UpdateEnum, TimeEnum>::await_ready() const noexcept
{
    return false;
}

template <internal::CountEnum UpdateEnum, internal::CountEnum TimeEnum>
template <typename T>
void WaitBP<UpdateEnum, TimeEnum>::await_suspend(std::coroutine_handle<internal::Promise<T>> handle) noexcept
{
    mHandle  = std::coroutine_handle<internal::PromiseBase>::from_address(handle.address());
    mExeIter = mHandle.promise().GetScheduler<UpdateEnum, TimeEnum>()->AddWait(this, mUpdateType, mTimeType);
}

template <internal::CountEnum UpdateEnum, internal::CountEnum TimeEnum>
void WaitBP<UpdateEnum, TimeEnum>::await_resume() const noexcept
{
}

template <internal::CountEnum UpdateEnum, internal::CountEnum TimeEnum>
void WaitBP<UpdateEnum, TimeEnum>::Resume()
{
    assert(mHandle && !mHandle.done() && mExeIter.has_value());
    // mExeIter has been removed from mExecuteQueue before enter Resume().
    mExeIter.reset();
    mHandle.resume();
}

//  Awaiter for All: waits all, returns tuple<T1, T2, T3 ...>
//
template <typename... Ts>
class All : public internal::CoroAwaiterBase
{
private:
    std::tuple<Async<Ts>...>                     mWaitedCoros;
    std::size_t                                  mRemainingCount;
    std::coroutine_handle<internal::PromiseBase> mParentHandle;

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
    void await_suspend(std::coroutine_handle<internal::Promise<T>> h) noexcept
    {
        mParentHandle = std::coroutine_handle<internal::PromiseBase>::from_address(h.address());

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

    auto await_resume()
    {
        std::tuple<internal::RetConvert<Ts>...> results;

        auto storeResults = [this, &results]<std::size_t... Is>(std::index_sequence<Is...>) {
            ([this, &results] {
                auto& coro = std::get<Is>(mWaitedCoros);
                using T    = std::tuple_element_t<Is, std::tuple<Ts...>>;
                if constexpr (std::is_void_v<T>)
                {
                    coro.GetHandle().promise().GetReturnValue();
                    std::get<Is>(results) = std::monostate{};
                }
                else
                {
                    std::get<Is>(results) = std::move(coro.GetHandle().promise().GetReturnValue());
                }
            }(),
             ...);
        };

        storeResults(std::index_sequence_for<Ts...>{});
        return std::move(results);
    }

    std::coroutine_handle<> OnWaitComplete(std::coroutine_handle<> h) noexcept override
    {
        if (--mRemainingCount == 0)
            return mParentHandle;
        else
            return std::noop_coroutine();
    }
};

//  Awaiter for Any: waits first, returns tuple<optional<T1>, optional<T2>, optional<T2>...>
//
template <typename... Ts>
class Any : public internal::CoroAwaiterBase
{
private:
    std::optional<std::tuple<Async<Ts>...>>                mWaitedCoros;
    std::coroutine_handle<>                                mFirstFinish;
    std::tuple<std::optional<internal::RetConvert<Ts>>...> mResults;
    std::coroutine_handle<internal::PromiseBase>           mParentHandle;

public:
    Any(Async<Ts>&&... cs)
        : mWaitedCoros(std::tuple<Async<Ts>...>(std::move(cs)...)), mResults()
    {
    }

    bool await_ready() const noexcept
    {
        return false;
    }

    template <typename T>
    void await_suspend(std::coroutine_handle<internal::Promise<T>> h) noexcept
    {
        mParentHandle = std::coroutine_handle<internal::PromiseBase>::from_address(h.address());

        auto resumeWithIndexes = [this]<std::size_t... Is>(std::index_sequence<Is...>) {
            ([this] {
                auto& coro    = std::get<Is>(mWaitedCoros.value());
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

    auto await_resume()
    {
        auto checkStoreWithIndexes = [this]<std::size_t... Is>(std::index_sequence<Is...>) {
            ([this] {
                auto& coro = std::get<Is>(mWaitedCoros.value());
                if (coro.GetHandle().address() != mFirstFinish.address())
                    return;

                using T = std::tuple_element_t<Is, std::tuple<Ts...>>;
                if constexpr (std::is_void_v<T>)
                {
                    // To trigger the exception if any
                    coro.GetHandle().promise().GetReturnValue();
                    std::get<Is>(mResults) = std::monostate{};
                }
                else
                {
                    std::get<Is>(mResults) = std::move(coro.GetHandle().promise().GetReturnValue());
                }
            }(),
             ...);
        };
        checkStoreWithIndexes(std::index_sequence_for<Ts...>{});

        mWaitedCoros.reset();
        return mResults;
    }

    std::coroutine_handle<> OnWaitComplete(std::coroutine_handle<> h) noexcept override
    {
        mFirstFinish = h;
        return mParentHandle;
    }
};

} // namespace tokoro

#include "promise.inl"

namespace tokoro
{

template <internal::CountEnum UpdateEnum, internal::CountEnum TimeEnum>
Async<void> WaitUntilBP(std::function<bool()>&& checkFunc)
{
    while (!checkFunc())
    {
        co_await WaitBP<UpdateEnum, TimeEnum>(internal::GetEnumDefault<UpdateEnum>());
    }
}

template <internal::CountEnum UpdateEnum, internal::CountEnum TimeEnum>
Async<void> WaitWhileBP(std::function<bool()>&& checkFunc)
{
    while (checkFunc())
    {
        co_await WaitBP<UpdateEnum, TimeEnum>(internal::GetEnumDefault<UpdateEnum>());
    }
}

// Define preset types for quick setup.
//
using Scheduler = SchedulerBP<internal::PresetUpdateType, internal::PresetTimeType>;
using Wait      = WaitBP<internal::PresetUpdateType, internal::PresetTimeType>;
template <typename T>
using Handle          = HandleBP<T, internal::PresetUpdateType, internal::PresetTimeType>;
inline auto WaitUntil = WaitUntilBP<internal::PresetUpdateType, internal::PresetTimeType>;
inline auto WaitWhile = WaitWhileBP<internal::PresetUpdateType, internal::PresetTimeType>;

} // namespace tokoro
