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

template <typename UpdateEnum, typename TimeEnum>
class SchedulerBP;

template <typename UpdateEnum = internal::PresetUpdateType, typename TimeEnum = internal::PresetTimeType>
class WaitBP
{
public:
    WaitBP(double sec, UpdateEnum updateType = UpdateEnum::Default, TimeEnum timeType = TimeEnum::Default);
    WaitBP(UpdateEnum updateType = UpdateEnum::Default, TimeEnum timeType = TimeEnum::Default);
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

template <typename T, typename UpdateEnum = internal::PresetUpdateType, typename TimeEnum = internal::PresetTimeType>
class HandleBP
{
public:
    HandleBP(HandleBP&& other);
    HandleBP(const HandleBP& other) = delete;
    ~HandleBP();

    bool IsDown() const noexcept;
    void Stop() const noexcept;

    std::optional<T> GetReturn() const noexcept
        requires(!std::is_void_v<T>);

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
    template <typename UpdateEnum, typename TimeEnum>
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

template <typename UpdateEnum = internal::PresetUpdateType, typename TimeEnum = internal::PresetTimeType>
class SchedulerBP
{
private:
    static constexpr int UpdateQueueCount = static_cast<int>(UpdateEnum::Count) * static_cast<int>(TimeEnum::Count);

public:
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

    void SetCustomTimer(TimeEnum timeType, std::function<double()> getTimeFunc)
    {
        mCustomTimers[static_cast<int>(timeType)] = std::move(getTimeFunc);
    }

    template <typename Task, typename... Args, typename RetType = typename std::decay_t<std::invoke_result_t<Task, Args...>>::value_type>
    HandleBP<RetType, UpdateEnum, TimeEnum> Start(Task&& task, Args&&... args)
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

        auto& newCoro = newEntry.coro.template WithTmplArg<RetType>();
        newCoro.SetId(id);
        newCoro.SetScheduler(this);

        // Kick off the coroutine.
        newCoro.Resume();

        return HandleBP<RetType, UpdateEnum, TimeEnum>{id, this, mLiveSignal};
    }

    void Update(UpdateEnum updateType = UpdateEnum::Update,
                TimeEnum   timeType   = TimeEnum::Realtime)
    {
        auto& timeQueue = GetUpdateQueue(updateType, timeType);
        timeQueue.SetupUpdate(GetCurrentTime(timeType));

        while (timeQueue.CheckUpdate())
        {
            timeQueue.Pop()->Resume();
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
        bool                                      finished = false;
        bool                                      released = false;
        std::any                                  returnValue;
    };

    std::unordered_map<uint64_t, Entry>                                    mCoroutines;
    uint64_t                                                               mNextId{1};
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
std::optional<T> HandleBP<T, UpdateEnum, TimeEnum>::GetReturn() const noexcept
    requires(!std::is_void_v<T>)
{
    if (mSchedulerLiveSignal.expired())
        return std::nullopt;
    return mScheduler->template GetReturn<T>(mId);
}

// TimeAwaiter functions
//
template <typename UpdateEnum, typename TimeEnum>
WaitBP<UpdateEnum, TimeEnum>::WaitBP(double sec, UpdateEnum updateType, TimeEnum timeType)
    : mDelay(sec),
      mUpdateType(updateType), mTimeType(timeType)
{
}

template <typename UpdateEnum, typename TimeEnum>
WaitBP<UpdateEnum, TimeEnum>::WaitBP(UpdateEnum updateType, TimeEnum timeType)
    : mDelay(0), mUpdateType(updateType), mTimeType(timeType)
{
}

template <typename UpdateEnum, typename TimeEnum>
WaitBP<UpdateEnum, TimeEnum>::~WaitBP()
{
    if (mExeIter.has_value())
        mHandle.promise().GetScheduler<UpdateEnum, TimeEnum>()->RemoveWait(*mExeIter, mUpdateType, mTimeType);
}

template <typename UpdateEnum, typename TimeEnum>
bool WaitBP<UpdateEnum, TimeEnum>::await_ready() const noexcept
{
    return false;
}

template <typename UpdateEnum, typename TimeEnum>
template <typename T>
void WaitBP<UpdateEnum, TimeEnum>::await_suspend(std::coroutine_handle<internal::Promise<T>> handle) noexcept
{
    mHandle  = std::coroutine_handle<internal::PromiseBase>::from_address(handle.address());
    mExeIter = mHandle.promise().GetScheduler<UpdateEnum, TimeEnum>()->AddWait(this, mUpdateType, mTimeType);
}

template <typename UpdateEnum, typename TimeEnum>
void WaitBP<UpdateEnum, TimeEnum>::await_resume() const noexcept
{
}

template <typename UpdateEnum, typename TimeEnum>
void WaitBP<UpdateEnum, TimeEnum>::Resume()
{
    assert(mHandle && !mHandle.done() && mExeIter.has_value());
    // mExeIter has been removed from mExecuteQueue before enter Resume().
    mExeIter.reset();
    mHandle.resume();
}

template <typename UpdateEnum, typename TimeEnum>
template <typename T>
std::optional<T> SchedulerBP<UpdateEnum, TimeEnum>::GetReturn(uint64_t id)
{
    return std::any_cast<T>(mCoroutines[id].returnValue);
}

//  Awaiter for All: waits all, returns tuple<T1, T2, T3 ...>
//
template <typename... Ts>
class All : public internal::CoroAwaiterBase
{
private:
    std::tuple<Async<Ts>...>                     mWaitedCoros;
    std::tuple<internal::RetConvert<Ts>...>      mResults;
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
class Any : public internal::CoroAwaiterBase
{
private:
    std::tuple<Async<Ts>...>                               mWaitedCoros;
    std::tuple<std::optional<internal::RetConvert<Ts>>...> mResults;
    bool                                                   mTriggered{false};
    std::coroutine_handle<internal::PromiseBase>           mParentHandle;

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
    void await_suspend(std::coroutine_handle<internal::Promise<T>> h) noexcept
    {
        mParentHandle = std::coroutine_handle<internal::PromiseBase>::from_address(h.address());

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

using Scheduler = SchedulerBP<internal::PresetUpdateType, internal::PresetTimeType>;
using Wait      = WaitBP<internal::PresetUpdateType, internal::PresetTimeType>;
template <typename T>
using Handle = HandleBP<T, internal::PresetUpdateType, internal::PresetTimeType>;

static Scheduler& GlobalScheduler()
{
    static Scheduler s;
    return s;
}

} // namespace tokoro

#include "promise.inl"
