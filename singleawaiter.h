#pragma once

#include "defines.h"
#include "promise.h"
#include <coroutine>

namespace tokoro::internal
{

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

        mWaitedHandle.resume(); // Kick off child Async<T>
    }

    T await_resume() const noexcept
        requires(!std::is_void_v<T>)
    {
        return mWaitedHandle.promise().GetReturnValue();
    }

    void await_resume() const
        requires(std::is_void_v<T>)
    {
        mWaitedHandle.promise().GetReturnValue();
    }

    std::coroutine_handle<> OnWaitComplete(std::coroutine_handle<> /*unused*/) noexcept override
    {
        return mParentHandle;
    }

private:
    std::coroutine_handle<Promise<T>>  mWaitedHandle;
    std::coroutine_handle<PromiseBase> mParentHandle;
};

} // namespace tokoro::internal