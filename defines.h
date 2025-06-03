#pragma once

#include <coroutine>
#include <cstdint>
#include <variant>

namespace tokoro::internal
{

class CoroAwaiterBase
{
public:
    virtual void OnWaitComplete(std::coroutine_handle<>) noexcept = 0;
    virtual ~CoroAwaiterBase()                                    = default;
};

// map void to std::monostate
template <typename T>
using RetConvert = std::conditional_t<std::is_void_v<T>, std::monostate, T>;

enum class PresetUpdateType : int
{
    Update = 0,
    Count,
};

enum class PresetTimeType : int
{
    Realtime = 0,
    Count,
};

template <typename E>
constexpr E GetEnumDefault()
{
    return static_cast<E>(0);
}

} // namespace tokoro::internal