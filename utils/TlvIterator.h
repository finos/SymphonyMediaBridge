#pragma once

namespace utils
{
// Type Length Value iterator. Assumes block as a size method.
template <typename T>
class TlvIterator
{
public:
    TlvIterator() = delete;
    explicit TlvIterator(T* start) : _item(start) {}

    TlvIterator& operator++()
    {
        _item = reinterpret_cast<T*>(const_cast<char*>(reinterpret_cast<const char*>(_item) + _item->size()));
        return *this;
    }

    T* operator->() { return _item; }
    T& operator*() { return *_item; }

    bool operator==(const TlvIterator& it) const { return _item == it._item; }

    bool operator!=(const TlvIterator& it) const { return !(*this == it); }

private:
    T* _item;
};

template <typename T>
class TlvCollectionConst
{
public:
    typedef utils::TlvIterator<const T> const_iterator;

    TlvCollectionConst(const void* beginPtr, const void* endPtr)
        : _begin(reinterpret_cast<const T*>(beginPtr)),
          _end(reinterpret_cast<const T*>(endPtr))
    {
    }

    const_iterator cbegin() const { return const_iterator(_begin); }
    const_iterator begin() const { return cbegin(); }
    const_iterator cend() const { return const_iterator(_end); }
    const_iterator end() const { return cend(); }

protected:
    const T* _begin;
    const T* _end;
};

template <typename T>
class TlvCollection
{
public:
    typedef utils::TlvIterator<T> iterator;
    typedef utils::TlvIterator<const T> const_iterator;

    TlvCollection(void* beginPtr, void* endPtr)
        : _begin(reinterpret_cast<T*>(beginPtr)),
          _end(reinterpret_cast<T*>(endPtr))
    {
    }

    const_iterator cbegin() const { return const_iterator(_begin); }
    const_iterator begin() const { return cbegin(); }
    const_iterator cend() const { return const_iterator(_end); }
    const_iterator end() const { return cend(); }
    iterator begin() { return iterator(const_cast<T*>(_begin)); }
    iterator end() { return iterator(const_cast<T*>(_end)); }

protected:
    T* _begin;
    T* _end;
};

} // namespace utils