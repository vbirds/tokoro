#pragma once
#include <assert.h>
#include <cstddef>
#include <new>
#include <type_traits>
#include <typeindex>
#include <utility>

// Detect if a template template parameter is instantiable
template <template <typename> class Tmpl, typename U, typename = void>
struct is_instantiable : std::false_type
{
};
template <template <typename> class Tmpl, typename U>
struct is_instantiable<Tmpl, U, std::void_t<Tmpl<U>>> : std::true_type
{
};

// TmplAny: type-erased wrapper. Only fails at compile time.
// Supports copy-only, move-only, and mixed types.
// Allows specifying a placeholder type PlaceholderT, defaulting to void.
// Usage: using AnyFoo = TmplAny<FooTemplate, int>;

template <template <typename> class Tmpl, typename PlaceholderT = void>
class TmplAny
{
    static_assert(is_instantiable<Tmpl, PlaceholderT>::value,
                  "PlaceholderT cannot be used to instantiate Tmpl<PlaceholderT>");

    using Placeholder = Tmpl<PlaceholderT>;

    static constexpr std::size_t StorageSize  = sizeof(Placeholder);
    static constexpr std::size_t StorageAlign = alignof(Placeholder);

    struct VTable
    {
        void (*destroy)(void*) noexcept;
        void (*copy)(const void*, void*);
        void (*move)(void*, void*);
        std::type_index type_index;
    };
    alignas(StorageAlign) unsigned char storage_[StorageSize];
    const VTable* vtable_ = nullptr;

    // Construct vtable for the Actual type
    template <typename Actual>
    static const VTable* make_vtable()
    {
        static VTable vt{.type_index = typeid(Actual)};
        vt.destroy = +[](void* p) noexcept { reinterpret_cast<Actual*>(p)->~Actual(); };
        if constexpr (std::is_copy_constructible_v<Actual>)
        {
            vt.copy = +[](const void* src, void* dest) {
                new (dest) Actual(*reinterpret_cast<const Actual*>(src));
            };
        }
        else
        {
            vt.copy = nullptr;
        }
        if constexpr (std::is_move_constructible_v<Actual>)
        {
            vt.move = +[](void* src, void* dest) {
                new (dest) Actual(std::move(*reinterpret_cast<Actual*>(src)));
            };
        }
        else
        {
            vt.move = nullptr;
        }
        return &vt;
    }

public:
    TmplAny() noexcept = default;

    // Constructor: prefer move, otherwise copy. Both are checked statically at compile time.
    template <typename U>
    TmplAny(Tmpl<U> obj)
    {
        using Actual = Tmpl<U>;
        static_assert(sizeof(Actual) == StorageSize, "Tmpl<U> size mismatch");
        if constexpr (std::is_move_constructible_v<Actual>)
        {
            new (storage_) Actual(std::move(obj));
        }
        else
        {
            static_assert(std::is_copy_constructible_v<Actual>, "Tmpl<U> must be at least copyable or movable");
            new (storage_) Actual(obj);
        }
        vtable_ = make_vtable<Actual>();
    }

    // Copy constructor: only available if Placeholder is copyable
    TmplAny(const TmplAny& other)
        requires std::is_copy_constructible_v<Placeholder>
    {
        if (other.vtable_)
        {
            other.vtable_->copy(other.storage_, storage_);
            vtable_ = other.vtable_;
        }
    }
    TmplAny(const TmplAny&) = delete;

    // Move constructor: only available if Placeholder is movable
    TmplAny(TmplAny&& other) noexcept
        requires std::is_move_constructible_v<Placeholder>
    {
        if (other.vtable_)
        {
            other.vtable_->move(other.storage_, storage_);
            vtable_ = other.vtable_;
            other.vtable_->destroy(other.storage_);
            other.vtable_ = nullptr;
        }
    }
    TmplAny(TmplAny&&) = delete;

    // Copy assignment
    TmplAny& operator=(const TmplAny& other)
        requires std::is_copy_constructible_v<Placeholder>
    {
        if (this != &other)
        {
            Reset();
            if (other.vtable_)
            {
                other.vtable_->copy(other.storage_, storage_);
                vtable_ = other.vtable_;
            }
        }
        return *this;
    }
    TmplAny& operator=(const TmplAny&) = delete;

    // Move assignment
    TmplAny& operator=(TmplAny&& other) noexcept
        requires std::is_move_constructible_v<Placeholder>
    {
        if (this != &other)
        {
            Reset();
            if (other.vtable_)
            {
                other.vtable_->move(other.storage_, storage_);
                vtable_ = other.vtable_;
                other.vtable_->destroy(other.storage_);
                other.vtable_ = nullptr;
            }
        }
        return *this;
    }
    TmplAny& operator=(TmplAny&&) = delete;

    ~TmplAny() noexcept
    {
        Reset();
    }
    void Reset() noexcept
    {
        if (vtable_)
        {
            vtable_->destroy(storage_);
            vtable_ = nullptr;
        }
    }

    explicit operator bool() const noexcept
    {
        return vtable_ != nullptr;
    }

    // Access underlying object. Requires U to have matching size with placeholder.
    template <typename U>
    Tmpl<U>& WithTmplArg()
    {
        assert(vtable_ && vtable_->type_index == typeid(Tmpl<U>));
        return *reinterpret_cast<Tmpl<U>*>(storage_);
    }

    template <typename U>
    const Tmpl<U>& WithTmplArg() const
    {
        assert(vtable_ && vtable_->type_index == typeid(Tmpl<U>));
        return *reinterpret_cast<const Tmpl<U>*>(storage_);
    }
};