#pragma once
#include <algorithm>
#include <vector>

namespace me {
// Large loaded arrays should not double in size after the first file event.
template<class T>
void reserveForAppend(std::vector<T>& values, size_t count = 1) {
    const size_t required = values.size() + count;
    if (required <= values.capacity()) return;
    const size_t capacity = values.capacity();
    const size_t growth = capacity < 1024 ? std::max<size_t>(capacity, 16) : capacity / 8;
    values.reserve(std::max(required, capacity + growth));
}
}
