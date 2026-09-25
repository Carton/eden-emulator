// (local-only) Scalar implementations of the MSVC STL vectorized-algorithm
// helpers referenced by CPM's prebuilt static Qt 6.11.1 (built by eden CI with
// a newer MSVC STL). The system VS2022 14.44 STL/CRuntime does not provide
// them, causing LNK2019. Signatures mirror microsoft/STL
// stl/src/vector_algorithms.cpp (Apache-2.0 with LLVM-exception).
// Performance is irrelevant here: only Qt GUI code paths call these.
#include <cstdint>
#include <cstdlib>
#include <cstring>

extern "C" {

__declspec(noalias) void __stdcall __std_rotate(void* first, void* const mid, void* last) noexcept {
    unsigned char* f = static_cast<unsigned char*>(first);
    unsigned char* const m = static_cast<unsigned char*>(mid);
    unsigned char* const l = static_cast<unsigned char*>(last);
    const size_t left = static_cast<size_t>(m - f);
    const size_t right = static_cast<size_t>(l - m);
    if (left == 0 || right == 0) {
        return;
    }
    const size_t total = left + right;
    unsigned char stack_buf[2048];
    unsigned char* const buf = total <= sizeof(stack_buf) ? stack_buf : static_cast<unsigned char*>(malloc(total));
    if (buf == nullptr) {
        return;
    }
    memcpy(buf, f, total);
    memcpy(f, buf + left, right);
    memcpy(f + right, buf, left);
    if (buf != stack_buf) {
        free(buf);
    }
}

__declspec(noalias) void __stdcall __std_replace_copy_1(const void* const first, const void* const last,
                                                        void* const dest, const std::uint8_t old_val,
                                                        const std::uint8_t new_val) noexcept {
    const std::uint8_t* f = static_cast<const std::uint8_t*>(first);
    const std::uint8_t* const l = static_cast<const std::uint8_t*>(last);
    std::uint8_t* d = static_cast<std::uint8_t*>(dest);
    for (; f != l; ++f, ++d) {
        *d = *f == old_val ? new_val : *f;
    }
}

__declspec(noalias) void __stdcall __std_replace_copy_2(const void* const first, const void* const last,
                                                        void* const dest, const std::uint16_t old_val,
                                                        const std::uint16_t new_val) noexcept {
    const std::uint16_t* f = static_cast<const std::uint16_t*>(first);
    const std::uint16_t* const l = static_cast<const std::uint16_t*>(last);
    std::uint16_t* d = static_cast<std::uint16_t*>(dest);
    for (; f != l; ++f, ++d) {
        *d = *f == old_val ? new_val : *f;
    }
}

void* __stdcall __std_unique_4(void* first, void* const last) noexcept {
    std::uint32_t* const l = static_cast<std::uint32_t*>(last);
    std::uint32_t* w = static_cast<std::uint32_t*>(first);
    if (w == l) {
        return l;
    }
    for (std::uint32_t* r = w + 1; r != l; ++r) {
        if (*r != *w) {
            *++w = *r;
        }
    }
    return w + 1;
}

void* __stdcall __std_unique_8(void* first, void* const last) noexcept {
    std::uint64_t* const l = static_cast<std::uint64_t*>(last);
    std::uint64_t* w = static_cast<std::uint64_t*>(first);
    if (w == l) {
        return l;
    }
    for (std::uint64_t* r = w + 1; r != l; ++r) {
        if (*r != *w) {
            *++w = *r;
        }
    }
    return w + 1;
}

struct ShimMinmaxElement {
    const void* lo;
    const void* hi;
};

ShimMinmaxElement __stdcall __std_minmax_element_2u(const void* const first, const void* const last) noexcept {
    const std::uint16_t* const f = static_cast<const std::uint16_t*>(first);
    const std::uint16_t* const l = static_cast<const std::uint16_t*>(last);
    if (f == l) {
        return {first, first};
    }
    const std::uint16_t* lo = f;
    const std::uint16_t* hi = f;
    for (const std::uint16_t* r = f + 1; r != l; ++r) {
        if (*r < *lo) {
            lo = r;
        } else if (!(*r < *hi)) {
            hi = r;
        }
    }
    return {lo, hi};
}

} // extern "C"
