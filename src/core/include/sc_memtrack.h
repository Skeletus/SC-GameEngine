#pragma once
#include <cstdint>

namespace sc
{
  enum class MemTag : uint8_t
  {
    Core = 0,
    Renderer,
    Physics,
    Streaming,
    Jobs,
    ImGui,
    Count
  };

  const char* memTagName(MemTag tag);

  struct MemStats
  {
    uint64_t bytesAllocated[(uint8_t)MemTag::Count]{};
    uint64_t bytesFreed[(uint8_t)MemTag::Count]{};
    uint64_t bytesLive[(uint8_t)MemTag::Count]{};
    uint64_t totalLive = 0;
  };

  void memtrack_alloc(MemTag tag, uint64_t size, const char* file = nullptr, uint32_t line = 0);
  void memtrack_free(MemTag tag, uint64_t size);
  MemStats memtrack_snapshot();

#if defined(SC_DEBUG)
  struct MemRecord
  {
    const char* file = nullptr;
    uint32_t line = 0;
    MemTag tag = MemTag::Core;
    uint32_t size = 0;
  };

  uint32_t memtrack_recentAllocCount();
  const MemRecord* memtrack_recentAllocs();
#endif
}
