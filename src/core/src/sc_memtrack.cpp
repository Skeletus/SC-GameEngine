#include "sc_memtrack.h"

#include <atomic>
#include <cstring>

namespace sc
{
  namespace
  {
    static std::atomic<uint64_t> g_alloc[(uint8_t)MemTag::Count];
    static std::atomic<uint64_t> g_free[(uint8_t)MemTag::Count];

#if defined(SC_DEBUG)
    static constexpr uint32_t kMaxRecords = 1024;
    static std::atomic<uint32_t> g_recordHead{ 0 };
    static MemRecord g_records[kMaxRecords]{};
#endif
  }

  const char* memTagName(MemTag tag)
  {
    switch (tag)
    {
      case MemTag::Core:      return "Core";
      case MemTag::Renderer:  return "Renderer";
      case MemTag::Physics:   return "Physics";
      case MemTag::Streaming: return "Streaming";
      case MemTag::Jobs:      return "Jobs";
      case MemTag::ImGui:     return "ImGui";
      default:                return "Unknown";
    }
  }

  void memtrack_alloc(MemTag tag, uint64_t size, const char* file, uint32_t line)
  {
    if (size == 0) return;
    const uint8_t idx = (uint8_t)tag;
    g_alloc[idx].fetch_add(size, std::memory_order_relaxed);

#if defined(SC_DEBUG)
    if (file)
    {
      const uint32_t slot = g_recordHead.fetch_add(1, std::memory_order_relaxed) % kMaxRecords;
      g_records[slot] = MemRecord{ file, line, tag, (uint32_t)size };
    }
#else
    (void)file; (void)line;
#endif
  }

  void memtrack_free(MemTag tag, uint64_t size)
  {
    if (size == 0) return;
    const uint8_t idx = (uint8_t)tag;
    g_free[idx].fetch_add(size, std::memory_order_relaxed);
  }

  MemStats memtrack_snapshot()
  {
    MemStats s{};
    for (uint8_t i = 0; i < (uint8_t)MemTag::Count; ++i)
    {
      const uint64_t a = g_alloc[i].load(std::memory_order_relaxed);
      const uint64_t f = g_free[i].load(std::memory_order_relaxed);
      s.bytesAllocated[i] = a;
      s.bytesFreed[i] = f;
      s.bytesLive[i] = (a >= f) ? (a - f) : 0;
      s.totalLive += s.bytesLive[i];
    }
    return s;
  }

#if defined(SC_DEBUG)
  uint32_t memtrack_recentAllocCount()
  {
    const uint32_t n = g_recordHead.load(std::memory_order_relaxed);
    return n > kMaxRecords ? kMaxRecords : n;
  }

  const MemRecord* memtrack_recentAllocs()
  {
    return g_records;
  }
#endif
}
