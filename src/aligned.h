#pragma once
#include <cstdlib>
#include <new>
#ifdef _MSC_VER
#include <malloc.h>
#endif

// 512-byte aligned simple vector. Taken from https://github.com/kpu/intgemm/blob/master/intgemm/aligned.h

template <class T> class AlignedVector {
  public:
    explicit AlignedVector(std::size_t size)
      : size_(size) {
#ifdef _MSC_VER
      mem_ = static_cast<T*>(_aligned_malloc(size * sizeof(T), 512));
      if (!mem_) throw std::bad_alloc();
#else      
      if (posix_memalign(reinterpret_cast<void **>(&mem_), 512, size * sizeof(T))) {
        throw std::bad_alloc();
      }
#endif
    }

    AlignedVector(const AlignedVector&) = delete;
    AlignedVector& operator=(const AlignedVector&) = delete;

    ~AlignedVector() {
#ifdef _MSC_VER
      _aligned_free(mem_);
#else
      std::free(mem_);
#endif
    }

    std::size_t size() const { return size_; }

    T &operator[](std::size_t offset) { return mem_[offset]; }
    const T &operator[](std::size_t offset) const { return mem_[offset]; }

    T *begin() { return mem_; }
    const T *begin() const { return mem_; }
    T *end() { return mem_ + size_; }
    const T *end() const { return mem_ + size_; }

    template <typename ReturnType>
    ReturnType *as() { return reinterpret_cast<ReturnType*>(mem_); }

  private:
    T *mem_;
    std::size_t size_;
};
