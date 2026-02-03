#pragma once
#include "sc_memtrack.h"

#include <cstddef>
#include <cstdint>

namespace sc
{
  namespace detail
  {
    inline size_t alignUp(size_t v, size_t a)
    {
      const size_t mask = a - 1;
      return (v + mask) & ~mask;
    }
  }

  struct MallocAllocator
  {
    void* allocate(size_t size, size_t align, MemTag tag, const char* file = nullptr, uint32_t line = 0);
    void deallocate(void* p, size_t size, MemTag tag);
  };

  class ArenaAllocator
  {
  public:
    bool init(size_t size, MemTag tag);
    void init(void* memory, size_t size, MemTag tag);
    void shutdown();

    void* allocate(size_t size, size_t align, MemTag tag, const char* file = nullptr, uint32_t line = 0);
    void deallocate(void* /*p*/, size_t /*size*/, MemTag /*tag*/) {}
    void reset();

    size_t capacity() const { return m_size; }
    size_t used() const { return m_offset; }

  private:
    uint8_t* m_base = nullptr;
    size_t m_size = 0;
    size_t m_offset = 0;
    bool m_owns = false;
    MemTag m_tag = MemTag::Core;
  };

  class LinearFrameAllocator
  {
  public:
    bool init(size_t size, MemTag tag);
    void shutdown();

    void* allocate(size_t size, size_t align, MemTag tag, const char* file = nullptr, uint32_t line = 0);
    void deallocate(void* /*p*/, size_t /*size*/, MemTag /*tag*/) {}
    void reset();

    size_t capacity() const { return m_size; }
    size_t used() const { return m_offset; }

  private:
    uint8_t* m_base = nullptr;
    size_t m_size = 0;
    size_t m_offset = 0;
    MemTag m_tag = MemTag::Core;
  };

  template<typename T, typename Alloc, typename... Args>
  T* allocNew(Alloc& a, MemTag tag, Args&&... args)
  {
    void* mem = a.allocate(sizeof(T), alignof(T), tag, __FILE__, __LINE__);
    if (!mem) return nullptr;
    return new (mem) T(static_cast<Args&&>(args)...);
  }

  template<typename T, typename Alloc>
  void allocDelete(Alloc& a, T* p, MemTag tag)
  {
    if (!p) return;
    p->~T();
    a.deallocate(p, sizeof(T), tag);
  }

#if defined(SC_DEBUG)
  #define SC_ALLOC(alloc, size, align, tag) (alloc).allocate((size), (align), (tag), __FILE__, __LINE__)
#else
  #define SC_ALLOC(alloc, size, align, tag) (alloc).allocate((size), (align), (tag))
#endif

#define SC_NEW(alloc, T, tag, ...) sc::allocNew<T>(alloc, tag, __VA_ARGS__)
#define SC_DELETE(alloc, ptr, tag) sc::allocDelete(alloc, ptr, tag)
}
