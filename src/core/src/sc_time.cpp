#include "sc_time.h"

#include <atomic>
#include <mutex>
#include <cstring>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace sc
{
  namespace
  {
    static std::atomic<uint64_t> g_freq{ 0 };

    static uint64_t queryFreq()
    {
      uint64_t f = g_freq.load(std::memory_order_relaxed);
      if (f != 0) return f;
      LARGE_INTEGER li{};
      QueryPerformanceFrequency(&li);
      f = (uint64_t)li.QuadPart;
      g_freq.store(f, std::memory_order_relaxed);
      return f;
    }

    static constexpr uint32_t kMaxScopes = 64;
    struct ScopeRecord
    {
      const char* name = nullptr;
      std::atomic<uint64_t> ticks{ 0 };
    };

    static std::atomic<uint32_t> g_scopeCount{ 0 };
    static ScopeRecord g_scopes[kMaxScopes]{};
    static std::mutex g_scopeMutex;
  }

  Tick nowTicks()
  {
    LARGE_INTEGER li{};
    QueryPerformanceCounter(&li);
    return (Tick)li.QuadPart;
  }

  double ticksToSeconds(Tick ticks)
  {
    const double f = (double)queryFreq();
    return f > 0.0 ? (double)ticks / f : 0.0;
  }

  uint32_t registerScope(const char* name)
  {
    if (!name) return 0xFFFFFFFFu;

    {
      std::lock_guard<std::mutex> lock(g_scopeMutex);
      const uint32_t count = g_scopeCount.load(std::memory_order_relaxed);
      for (uint32_t i = 0; i < count; ++i)
      {
        if (g_scopes[i].name && std::strcmp(g_scopes[i].name, name) == 0)
          return i;
      }

      if (count < kMaxScopes)
      {
        g_scopes[count].name = name;
        g_scopes[count].ticks.store(0, std::memory_order_relaxed);
        g_scopeCount.store(count + 1, std::memory_order_relaxed);
        return count;
      }
    }

    return 0xFFFFFFFFu;
  }

  void addScopeTicks(uint32_t scopeId, Tick ticks)
  {
    if (scopeId == 0xFFFFFFFFu) return;
    if (scopeId >= kMaxScopes) return;
    g_scopes[scopeId].ticks.fetch_add(ticks, std::memory_order_relaxed);
  }

  ScopeTop snapshotTopScopes(uint32_t maxEntries)
  {
    ScopeTop top{};
    if (maxEntries > 5) maxEntries = 5;

    const uint32_t count = g_scopeCount.load(std::memory_order_relaxed);
    for (uint32_t i = 0; i < count; ++i)
    {
      const uint64_t ticks = g_scopes[i].ticks.exchange(0, std::memory_order_relaxed);
      if (ticks == 0) continue;

      const double ms = ticksToSeconds(ticks) * 1000.0;
      uint32_t insert = top.count;
      if (insert >= maxEntries)
      {
        double minMs = top.entries[0].ms;
        insert = 0;
        for (uint32_t j = 1; j < maxEntries; ++j)
        {
          if (top.entries[j].ms < minMs)
          {
            minMs = top.entries[j].ms;
            insert = j;
          }
        }
        if (ms <= minMs)
          continue;
      }
      else
      {
        top.count++;
      }

      top.entries[insert].name = g_scopes[i].name;
      top.entries[insert].ms = ms;
    }

    return top;
  }

  ScopedTimer::ScopedTimer(std::atomic<Tick>* counter)
    : m_start(nowTicks()), m_counter(counter)
  {
  }

  ScopedTimer::ScopedTimer(uint32_t scopeId)
    : m_start(nowTicks()), m_scopeId(scopeId)
  {
  }

  ScopedTimer::~ScopedTimer()
  {
    const Tick end = nowTicks();
    const Tick dt = end - m_start;
    if (m_counter)
      m_counter->fetch_add(dt, std::memory_order_relaxed);
    else if (m_scopeId != 0xFFFFFFFFu)
      addScopeTicks(m_scopeId, dt);
  }
}
