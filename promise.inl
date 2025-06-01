#pragma once

// All function definitions of Promise related classes
// Put them in the .inl file to avoid compiling order issue.(Depend on CoroAwaiterBase and Scheduler)

#include "promise.h"

#include <cassert>

namespace tokoro::internal
{

// PromiseBase::FinalAwaiter functions
//
inline bool PromiseBase::FinalAwaiter::await_ready() const noexcept
{
    return false;
}

// LCOV_EXCL_START This function never used, but compiler need it.
inline void PromiseBase::FinalAwaiter::await_resume() const noexcept
{
}
// LCOV_EXCL_STOP

inline void PromiseBase::FinalAwaiter::await_suspend(std::coroutine_handle<> h) const noexcept
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

// PromiseBase functions
//
inline std::suspend_always PromiseBase::initial_suspend() noexcept
{
    return {};
}

inline auto PromiseBase::final_suspend() noexcept
{
    return FinalAwaiter{};
}

// LCOV_EXCL_START TODO wait until exception implement.
inline void PromiseBase::unhandled_exception()
{
    std::terminate();
}
// LCOV_EXCL_STOP

inline void PromiseBase::SetId(uint64_t id)
{
    mId = id;
}

inline void PromiseBase::SetScheduler(Scheduler* scheduler)
{
    mScheduler = scheduler;
}

inline Scheduler* PromiseBase::GetScheduler() const
{
    return mScheduler;
}

inline void PromiseBase::SetParentAwaiter(CoroAwaiterBase* awaiter)
{
    mParentAwaiter = awaiter;
}

// Promise<T> functions
//
template <typename T>
auto Promise<T>::get_return_object() noexcept
{
    return Handle::from_promise(*this);
}

template <typename T>
void Promise<T>::return_value(T&& val)
{
    this->mReturnValue = std::forward<T>(val);
}

template <typename T>
void Promise<T>::return_value(const T& val)
{
    this->mReturnValue = val;
}

template <typename T>
T& Promise<T>::GetReturnValue()
{
    return *std::any_cast<T>(&this->mReturnValue);
}

// Promise<void> functions
//
inline auto Promise<void>::get_return_object() noexcept
{
    return Handle::from_promise(*this);
}

inline void Promise<void>::return_void()
{
}

} // namespace tokoro::internal
