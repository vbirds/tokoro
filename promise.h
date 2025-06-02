#pragma once

#include "defines.h"

#include <any>
#include <coroutine>

namespace tokoro
{

template <typename UpdateEnum, typename TimeEnum>
class SchedulerBP;

namespace internal
{

class CoroAwaiterBase;

class PromiseBase
{
public:
    struct FinalAwaiter
    {
        bool await_ready() const noexcept;
        void await_suspend(std::coroutine_handle<> h) const noexcept;
        void await_resume() const noexcept;
    };

    std::suspend_always initial_suspend() noexcept;
    auto                final_suspend() noexcept;
    void                unhandled_exception();
    void                SetId(uint64_t id);

    template <typename UpdateEnum = PresetUpdateType, typename TimeEnum = PresetTimeType>
    void SetScheduler(SchedulerBP<UpdateEnum, TimeEnum>* scheduler);

    template <typename UpdateEnum = PresetUpdateType, typename TimeEnum = PresetTimeType>
    SchedulerBP<UpdateEnum, TimeEnum>* GetScheduler() const;

    void SetParentAwaiter(CoroAwaiterBase* awaiter);

protected:
    std::any         mReturnValue;
    uint64_t         mId            = 0;
    CoroAwaiterBase* mParentAwaiter = nullptr;
    void*            mScheduler     = nullptr;
};

template <typename T>
class Promise : public PromiseBase
{
public:
    using Handle = std::coroutine_handle<Promise<T>>;

    auto get_return_object() noexcept;
    void return_value(T&& val);
    void return_value(const T& val);
    T&   GetReturnValue();
};

template <>
class Promise<void> : public PromiseBase
{
public:
    using Handle = std::coroutine_handle<Promise<void>>;

    auto get_return_object() noexcept;
    void return_void();
};

} // namespace internal

} // namespace tokoro