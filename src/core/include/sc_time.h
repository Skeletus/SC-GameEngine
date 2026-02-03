#pragma once
#include <cstdint>
#include <atomic>

namespace sc
{
  using Tick = uint64_t;

  Tick nowTicks();
  double ticksToSeconds(Tick ticks);

  struct ScopeTopEntry
  {
    const char* name = nullptr;
    double ms = 0.0;
  };

  struct ScopeTop
  {
    uint32_t count = 0;
    ScopeTopEntry entries[5]{};
  };

  uint32_t registerScope(const char* name);
  void addScopeTicks(uint32_t scopeId, Tick ticks);
  ScopeTop snapshotTopScopes(uint32_t maxEntries = 5);

  class ScopedTimer
  {
  public:
    explicit ScopedTimer(std::atomic<Tick>* counter);
    explicit ScopedTimer(uint32_t scopeId);
    ~ScopedTimer();

  private:
    Tick m_start = 0;
    std::atomic<Tick>* m_counter = nullptr;
    uint32_t m_scopeId = 0xFFFFFFFFu;
  };
}
