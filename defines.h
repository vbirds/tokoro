#pragma once

#include <chrono>
#include <coroutine>

namespace tokoro::internal
{

using Clock         = std::chrono::steady_clock;
using TimePoint     = Clock::time_point;
using ClockDuration = Clock::duration;

class CoroAwaiterBase
{
public:
    virtual void OnWaitComplete(std::coroutine_handle<>) noexcept = 0;
    virtual ~CoroAwaiterBase()                                    = default;
};

// map void to std::monostate
template <typename T>
using RetConvert = std::conditional_t<std::is_void_v<T>, std::monostate, T>;

} // namespace tokoro::internal