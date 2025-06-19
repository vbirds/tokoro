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
        promise.SetCoroManager(mParentHandle.promise().GetCoroManager());
        promise.SetParentAwaiter(this);

        mWaitedHandle.resume(); // Kick off child Async<T>
    }

    T await_resume() const noexcept
        requires(!std::is_void_v<T>)
    {
        return std::move(mWaitedHandle.promise().TakeResult());
    }

    void await_resume() const
        requires(std::is_void_v<T>)
    {
        mWaitedHandle.promise().TakeResult();
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