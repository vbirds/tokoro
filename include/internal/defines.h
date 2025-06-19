#pragma once

#include <coroutine>
#include <cstdint>
#include <variant>

namespace tokoro::internal
{

class CoroAwaiterBase
{
public:
    virtual std::coroutine_handle<> OnWaitComplete(std::coroutine_handle<>) noexcept = 0;

    virtual ~CoroAwaiterBase() = default;
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

// Update enums used with tokoro must be
// 1. Start from 0. The first enum value is also the default value of its kind.
// 2. Each enum value increment by 1
// 3. End with enum: Count
template <typename Enum>
concept CountEnum = std::is_enum_v<Enum> && requires { Enum::Count; };

template <typename E>
constexpr E GetEnumDefault()
{
    return static_cast<E>(0);
}

} // namespace tokoro::internal