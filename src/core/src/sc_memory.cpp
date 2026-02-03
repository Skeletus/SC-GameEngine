#include "sc_memory.h"

#include <malloc.h>
#include <cstring>

namespace sc
{
  void* MallocAllocator::allocate(size_t size, size_t align, MemTag tag, const char* file, uint32_t line)
  {
    if (size == 0) return nullptr;
    void* p = _aligned_malloc(size, align);
    if (p) memtrack_alloc(tag, (uint64_t)size, file, line);
    return p;
  }

  void MallocAllocator::deallocate(void* p, size_t size, MemTag tag)
  {
    if (!p) return;
    _aligned_free(p);
    memtrack_free(tag, (uint64_t)size);
  }

  bool ArenaAllocator::init(size_t size, MemTag tag)
  {
    if (size == 0) return false;
    m_base = (uint8_t*)_aligned_malloc(size, 64);
    if (!m_base) return false;
    m_size = size;
    m_offset = 0;
    m_owns = true;
    m_tag = tag;
    return true;
  }

  void ArenaAllocator::init(void* memory, size_t size, MemTag tag)
  {
    m_base = (uint8_t*)memory;
    m_size = size;
    m_offset = 0;
    m_owns = false;
    m_tag = tag;
  }

  void ArenaAllocator::shutdown()
  {
    if (m_base && m_owns)
    {
      _aligned_free(m_base);
    }
    m_base = nullptr;
    m_size = 0;
    m_offset = 0;
    m_owns = false;
  }

  void* ArenaAllocator::allocate(size_t size, size_t align, MemTag tag, const char* file, uint32_t line)
  {
    (void)tag;
    const size_t aligned = detail::alignUp(m_offset, align);
    if (aligned + size > m_size) return nullptr;
    void* p = m_base + aligned;
    m_offset = aligned + size;
    memtrack_alloc(m_tag, (uint64_t)size, file, line);
    return p;
  }

  void ArenaAllocator::reset()
  {
    if (m_offset)
    {
      memtrack_free(m_tag, (uint64_t)m_offset);
      m_offset = 0;
    }
  }

  bool LinearFrameAllocator::init(size_t size, MemTag tag)
  {
    if (size == 0) return false;
    m_base = (uint8_t*)_aligned_malloc(size, 64);
    if (!m_base) return false;
    m_size = size;
    m_offset = 0;
    m_tag = tag;
    return true;
  }

  void LinearFrameAllocator::shutdown()
  {
    if (m_base)
    {
      _aligned_free(m_base);
      m_base = nullptr;
    }
    m_size = 0;
    m_offset = 0;
  }

  void* LinearFrameAllocator::allocate(size_t size, size_t align, MemTag tag, const char* file, uint32_t line)
  {
    (void)tag;
    const size_t aligned = detail::alignUp(m_offset, align);
    if (aligned + size > m_size) return nullptr;
    void* p = m_base + aligned;
    m_offset = aligned + size;
    memtrack_alloc(m_tag, (uint64_t)size, file, line);
    return p;
  }

  void LinearFrameAllocator::reset()
  {
    if (m_offset)
    {
      memtrack_free(m_tag, (uint64_t)m_offset);
      m_offset = 0;
    }
  }
}
