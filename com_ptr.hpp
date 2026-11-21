#pragma once

#include <utility>

template<class T>
class ComPtr {
public:
    ComPtr() = default;
    ~ComPtr() { Reset(); }
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;
    T* Get() const { return object; }
    T** Put() { Reset(); return &object; }
    void Reset() { if (object) object->Release(); object = nullptr; }
    void Swap(ComPtr& other) { std::swap(object, other.object); }
private:
    T* object = nullptr;
};
