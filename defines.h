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
    Default = Update,
};

enum class PresetTimeType : int
{
    Realtime = 0,
    Count,
    Default = Realtime,
};

} // namespace tokoro::internal