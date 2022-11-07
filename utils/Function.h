#pragma once

#include <assert.h>
#include <cstddef>
#include <tuple>
#include <type_traits>

namespace utils
{

class Function;

namespace detail
{

constexpr size_t calculateStorageSize()
{
    constexpr size_t minSize = 20 * sizeof(uint64_t);
    // Ensure EngineFunction has a size multiple of alignof(std::max_align_t) to not waste space with paddings
    // when we have a contiguous containers with EngineFunction
    constexpr size_t alignedSpace =
        (minSize + sizeof(void*) + (alignof(std::max_align_t) - 1)) & ~(alignof(std::max_align_t) - 1);

    return alignedSpace - sizeof(void*);
}

using FunctionStorage = std::aligned_storage_t<calculateStorageSize(), alignof(std::max_align_t)>;

class Invokable
{
public:
    virtual ~Invokable() = default;

private:
    friend class ::utils::Function;
    virtual void invoke() const = 0;
    virtual Invokable* moveTo(FunctionStorage& newStorage) = 0;
    virtual Invokable* copyTo(FunctionStorage& newStorage) const = 0;
};

template <class Func, class... Args>
class FunctionBinder final : public Invokable
{
public:
    template <class UFunc, class... UArgs>
    FunctionBinder(UFunc&& func, UArgs&&... args)
        : _function(std::forward<UFunc>(func)),
          _functionArguments(std::forward<UArgs>(args)...)
    {
    }

    FunctionBinder(const FunctionBinder& rhs) : _function(rhs._function), _functionArguments(rhs._functionArguments) {}

    FunctionBinder(FunctionBinder&& rhs)
        : _function(std::move(rhs._function)),
          _functionArguments(std::move(rhs._functionArguments))
    {
    }

    FunctionBinder& operator=(const FunctionBinder& rhs)
    {
        _function = rhs._function;
        _functionArguments = rhs._functionArguments;
        return *this;
    }

    FunctionBinder& operator=(FunctionBinder&& rhs)
    {
        _function = std::move(rhs._function);
        _functionArguments = std::move(rhs._functionArguments);
        return *this;
    }

    void operator()() const { invoke(); }

private:
    using TThis = FunctionBinder<Func, Args...>;
    friend class ::utils::Function;

    template <class T, class... UArgs>
    void callMemberFunction(T* instance, UArgs&&... args) const
    {
        (instance->*_function)(std::forward<UArgs>(args)...);
    }

    template <size_t... I>
    void call(std::true_type, std::index_sequence<I...>) const
    {
        callMemberFunction(std::get<I>(_functionArguments)...);
    }

    template <size_t... I>
    void call(std::false_type, std::index_sequence<I...>) const
    {
        _function(std::get<I>(_functionArguments)...);
    }

    void invoke() const final
    {
        call(std::is_member_function_pointer<Func>(),
            std::make_index_sequence<std::tuple_size<std::tuple<Args...>>::value>{});
    }

    TThis* moveTo(FunctionStorage& newStorage) final { return new (&newStorage) TThis(std::move(*this)); }

    TThis* copyTo(FunctionStorage& newStorage) const final { return new (&newStorage) TThis(*this); }

private:
    Func _function;
    std::tuple<Args...> _functionArguments;
};

} // namespace detail

class Function
{
public:
    Function() : _invokable(nullptr) {}
    Function(std::nullptr_t) : Function() {}

    template <class Func, class... Args>
    Function(const detail::FunctionBinder<Func, Args...>& binder)
    {
        copyBinder(binder);
    }

    template <class Func, class... Args>
    Function(detail::FunctionBinder<Func, Args...>&& binder)
    {
        moveBinder(std::move(binder));
    }

    Function(const Function& rhs) : _invokable(nullptr)
    {
        if (rhs._invokable)
        {
            _invokable = rhs._invokable->copyTo(_storage);
        }
    }

    Function(Function&& rhs) : _invokable(nullptr)
    {
        if (rhs._invokable)
        {
            _invokable = rhs._invokable->moveTo(_storage);
            rhs.release();
        }
    }

    ~Function() { release(); }

    Function& operator=(const Function& rhs)
    {
        release();
        if (rhs._invokable)
        {
            _invokable = rhs._invokable->copyTo(_storage);
        }

        return *this;
    }

    Function& operator=(Function&& rhs)
    {
        release();
        if (rhs._invokable)
        {
            _invokable = rhs._invokable->moveTo(_storage);
            rhs.release();
        }

        return *this;
    }

    template <class Func, class... Args>
    Function& operator=(const detail::FunctionBinder<Func, Args...>& binder)
    {
        release();
        copyBinder(binder);
        return *this;
    }

    template <class Func, class... Args>
    Function& operator=(detail::FunctionBinder<Func, Args...>&& binder)
    {
        release();
        moveBinder(std::move(binder));
        return *this;
    }

    Function& operator=(std::nullptr_t)
    {
        release();
        return *this;
    }

    operator bool() const { return !!_invokable; }

    void operator()() const
    {
        assert(_invokable);
        _invokable->invoke();
    }

private:
    void release()
    {
        if (_invokable)
        {
            _invokable->~Invokable();
            _invokable = nullptr;
        }
    }

    template <class Func, class... Args>
    void copyBinder(const detail::FunctionBinder<Func, Args...>& binder)
    {
        static_assert(sizeof(_storage) >= sizeof(std::decay_t<decltype(binder)>),
            "EngineFunctionStorage has insufficient space");
        _invokable = binder.copyTo(_storage);
    }

    template <class Func, class... Args>
    void moveBinder(detail::FunctionBinder<Func, Args...>&& binder)
    {
        static_assert(sizeof(_storage) >= sizeof(std::decay_t<decltype(binder)>),
            "EngineFunctionStorage has insufficient space");
        _invokable = binder.moveTo(_storage);
    }

private:
    detail::FunctionStorage _storage;
    detail::Invokable* _invokable;
};

inline bool operator==(const Function& f, std::nullptr_t) noexcept
{
    return !f;
}

inline bool operator==(std::nullptr_t, const Function& f) noexcept
{
    return !f;
}

inline bool operator!=(const Function& f, std::nullptr_t) noexcept
{
    return !!f;
}

inline bool operator!=(std::nullptr_t, const Function& f) noexcept
{
    return !!f;
}

/**
 * This is for give pass rvalues using utils::bind function for types that can't be copied
 * like std::unique_ptr.
 *
 * WARNING: This object moves internal object on copies. it must be used only
 * for functions that will be called only one time
 */
template <class T>
class RValueWrapper
{
public:
    RValueWrapper(T&& rValueRef) : _rValue(std::move(rValueRef)) {}
    RValueWrapper(const RValueWrapper& rhs) : _rValue(std::move(rhs._rValue)) {}
    RValueWrapper(RValueWrapper&& rhs) : _rValue(std::move(rhs._rValue)) {}

    RValueWrapper& operator=(const RValueWrapper& rhs)
    {
        _rValue(std::move(rhs._rValue));
        return *this;
    }

    RValueWrapper& operator=(RValueWrapper&& rhs)
    {
        _rValue(std::move(rhs._rValue));
        return *this;
    }

    operator T() const { return std::move(_rValue); }

private:
    mutable T _rValue;
};

template <class T>
auto moveParam(T&& value)
{
    return RValueWrapper<std::remove_reference_t<T>>(std::move(value));
}

template <class Func, class... Args>
std::enable_if_t<std::is_function<std::remove_pointer_t<Func>>::value,
    detail::FunctionBinder<Func, std::decay_t<Args>...>>
bind(Func&& func, Args&&... args)
{
    return detail::FunctionBinder<Func, std::decay_t<Args>...>(std::forward<Func>(func), std::forward<Args>(args)...);
}

template <class T, class U, class K, class... Args>
auto bind(U T::*memberFunction, K* instance, Args&&... args)
{
    static_assert(std::is_base_of<T, K>::value, "instance is not same or derived type of T");
    return detail::FunctionBinder<U T::*, T*, std::decay_t<Args>...>(memberFunction,
        instance,
        std::forward<Args>(args)...);
}

} // namespace utils
