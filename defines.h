#pragma once

#include <coroutine>
#include <variant>

namespace tokoro
{

enum class PresetUpdateType
{
    Update = 0,
    Count
};

enum class PresetTimeType
{
    Realtime = 0,
    Count
};

} // namespace tokoro

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

} // namespace tokoro::internal