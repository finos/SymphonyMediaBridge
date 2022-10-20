#include <cassert>
#include <tuple>

namespace bridge
{

class EngineFunction;

namespace detail
{

constexpr size_t calculateStorageSize()
{
    constexpr size_t minSize = 128;
    // EngineFunction will have EngineFunctionStorage + a pointer
    // Most likely std::max_align_t will return 16. Then it would create
    // padding of 8 bytes for a 128 size storage. Here we will ensure that we
    // take advantage of all allocated memory without wasting memory with paddings
    // when we have a contiguous collection of EngineFunction
    constexpr size_t alignedSpace =
        (minSize + sizeof(void*) + (alignof(std::max_align_t) - 1)) & ~(alignof(std::max_align_t) - 1);

    return alignedSpace - sizeof(void*);
}

using EngineFunctionStorage = std::aligned_storage_t<calculateStorageSize(), alignof(std::max_align_t)>;

class Invokable
{
public:
    virtual ~Invokable() = default;

private:
    friend class ::bridge::EngineFunction;
    virtual void invoke() const = 0;
    virtual Invokable* moveTo(EngineFunctionStorage& newStorage) = 0;
    virtual Invokable* copyTo(EngineFunctionStorage& newStorage) const = 0;
};

template <class Func, class... Args>
class EngineFunctionBinder final : public Invokable
{
public:
    template <class UFunc, class... UArgs>
    EngineFunctionBinder(UFunc&& func, UArgs&&... args)
        : _engineFunction(std::forward<UFunc>(func)),
          _functionArguments(std::forward<UArgs>(args)...)
    {
    }

    EngineFunctionBinder(const EngineFunctionBinder& rhs)
        : _engineFunction(rhs._engineFunction),
          _functionArguments(rhs._functionArguments)
    {
    }

    EngineFunctionBinder(EngineFunctionBinder&& rhs)
        : _engineFunction(std::move(rhs._engineFunction)),
          _functionArguments(std::move(rhs._functionArguments))
    {
    }

    EngineFunctionBinder& operator=(const EngineFunctionBinder& rhs)
    {
        _engineFunction = rhs._engineFunction;
        _functionArguments = rhs._functionArguments;
        return *this;
    }

    EngineFunctionBinder& operator=(EngineFunctionBinder&& rhs)
    {
        _engineFunction = std::move(rhs._engineFunction);
        _functionArguments = std::move(rhs._functionArguments);
        return *this;
    }

    void operator()() const { invoke(); }

private:
    using TThis = EngineFunctionBinder<Func, Args...>;
    friend class ::bridge::EngineFunction;

    template <class T, class... UArgs>
    void callMemberFunction(T* instance, UArgs&&... args) const
    {
        (instance->*_engineFunction)(std::forward<UArgs>(args)...);
    }

    template <size_t... I>
    void call(std::true_type, std::index_sequence<I...>) const
    {
        callMemberFunction(std::get<I>(_functionArguments)...);
    }

    template <size_t... I>
    void call(std::false_type, std::index_sequence<I...>) const
    {
        _engineFunction(std::get<I>(_functionArguments)...);
    }

    void invoke() const final
    {
        call(std::is_member_function_pointer<Func>(),
            std::make_index_sequence<std::tuple_size<std::tuple<Args...>>::value>{});
    }

    TThis* moveTo(EngineFunctionStorage& newStorage) final { return new (&newStorage) TThis(std::move(*this)); }

    TThis* copyTo(EngineFunctionStorage& newStorage) const final { return new (&newStorage) TThis(*this); }

private:
    Func _engineFunction;
    std::tuple<Args...> _functionArguments;
};

} // namespace detail

class EngineFunction
{
public:
    EngineFunction() : _invokable(nullptr) {}
    EngineFunction(std::nullptr_t) : EngineFunction() {}

    template <class Func, class... Args>
    EngineFunction(const detail::EngineFunctionBinder<Func, Args...>& binder)
    {
        static_assert(sizeof(_storage) >= sizeof(std::decay_t<decltype(binder)>),
            "EngineFunctionStorage has insufficient space");
        _invokable = binder.copyTo(_storage);
    }

    template <class Func, class... Args>
    EngineFunction(detail::EngineFunctionBinder<Func, Args...>&& binder)
    {
        static_assert(sizeof(_storage) >= sizeof(std::decay_t<decltype(binder)>),
            "EngineFunctionStorage has insufficient space");
        _invokable = binder.moveTo(_storage);
    }

    EngineFunction(const EngineFunction& rhs) : _invokable(nullptr)
    {
        if (rhs._invokable)
        {
            _invokable = rhs._invokable->copyTo(_storage);
        }
    }

    EngineFunction(EngineFunction&& rhs) : _invokable(nullptr)
    {
        if (rhs._invokable)
        {
            _invokable = rhs._invokable->moveTo(_storage);
            rhs.release();
        }
    }

    ~EngineFunction() { release(); }

    EngineFunction& operator=(const EngineFunction& rhs)
    {
        release();
        if (rhs._invokable)
        {
            _invokable = rhs._invokable->copyTo(_storage);
        }

        return *this;
    }

    EngineFunction& operator=(EngineFunction&& rhs)
    {
        release();
        if (rhs._invokable)
        {
            _invokable = rhs._invokable->moveTo(_storage);
            rhs.release();
        }

        return *this;
    }

    EngineFunction& operator=(std::nullptr_t)
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

private:
    detail::EngineFunctionStorage _storage;
    detail::Invokable* _invokable;
};

inline bool operator==(const EngineFunction& f, std::nullptr_t) noexcept
{
    return !f;
}

inline bool operator==(std::nullptr_t, const EngineFunction& f) noexcept
{
    return !f;
}

inline bool operator!=(const EngineFunction& f, std::nullptr_t) noexcept
{
    return !!f;
}

inline bool operator!=(std::nullptr_t, const EngineFunction& f) noexcept
{
    return !!f;
}

namespace engine
{

template <class Func, class... Args>
std::enable_if_t<std::is_function<std::remove_pointer_t<Func>>::value,
    detail::EngineFunctionBinder<Func, std::decay_t<Args>...>>
bind(Func&& func, Args&&... args)
{
    return detail::EngineFunctionBinder<Func, std::decay_t<Args>...>(std::forward<Func>(func),
        std::forward<Args>(args)...);
}

template <class T, class U, class K, class... Args>
auto bind(U T::*memberFunction, K* instance, Args&&... args)
{
    static_assert(std::is_base_of<T, K>::value, "instance is not same or derived type of T");
    return detail::EngineFunctionBinder<U T::*, T*, std::decay_t<Args>...>(memberFunction,
        instance,
        std::forward<Args>(args)...);
}

} // namespace engine

}; // namespace bridge