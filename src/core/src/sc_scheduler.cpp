#include "sc_scheduler.h"
#include "sc_log.h"

#include <cstring>

namespace sc
{
  void Scheduler::addSystem(const char* name,
                            SystemPhase phase,
                            SystemFn fn,
                            void* user,
                            std::initializer_list<const char*> deps)
  {
    SystemRecord rec{};
    rec.name = name;
    rec.phase = phase;
    rec.fn = fn;
    rec.user = user;
    rec.depNames.assign(deps.begin(), deps.end());
    rec.scopeId = registerScope(name);
    m_systems.push_back(rec);
  }

  void Scheduler::finalize()
  {
    for (auto& list : m_phaseLists)
      list.clear();

    for (uint32_t i = 0; i < (uint32_t)m_systems.size(); ++i)
    {
      auto& sys = m_systems[i];
      sys.deps.clear();
      for (const char* depName : sys.depNames)
      {
        const uint32_t depIndex = findSystemIndex(depName);
        if (depIndex == 0xFFFFFFFFu)
        {
          sc::log(sc::LogLevel::Warn, "Scheduler: dependency not found: %s (system=%s)", depName, sys.name);
          continue;
        }
        sys.deps.push_back(depIndex);
      }
      m_phaseLists[(uint32_t)sys.phase].push_back(i);
    }

    m_completed.resize(m_systems.size());
    m_ready.reserve(m_systems.size());
  }

  void Scheduler::tick(World& world, float dt)
  {
    if (m_systems.empty())
      return;

    for (auto& sys : m_systems)
      sys.frameTicks = 0;

    std::memset(m_completed.data(), 0, m_completed.size());

    runPhase(SystemPhase::Input, world, dt);
    runPhase(SystemPhase::Simulation, world, dt);
    runPhase(SystemPhase::RenderPrep, world, dt);
    runPhase(SystemPhase::Render, world, dt);

    publishStats();
  }

  void Scheduler::runPhase(SystemPhase phase, World& world, float dt)
  {
    const auto& list = m_phaseLists[(uint32_t)phase];
    if (list.empty())
      return;

    uint32_t remaining = (uint32_t)list.size();
    while (remaining > 0)
    {
      m_ready.clear();
      for (uint32_t idx : list)
      {
        if (m_completed[idx])
          continue;
        if (depsReady(m_systems[idx]))
          m_ready.push_back(idx);
      }

      if (m_ready.empty())
      {
        sc::log(sc::LogLevel::Warn, "Scheduler: phase %u had unsatisfied deps; running sequential fallback.", (uint32_t)phase);
        for (uint32_t idx : list)
        {
          if (m_completed[idx])
            continue;
          executeSystem(idx, world, dt);
          m_completed[idx] = 1;
          remaining--;
        }
        break;
      }

      if (m_ready.size() == 1)
      {
        const uint32_t idx = m_ready[0];
        executeSystem(idx, world, dt);
      }
      else
      {
        auto& jobs = sc::jobs();
        auto handle = jobs.Dispatch((uint32_t)m_ready.size(), 1, [&](const JobContext& ctx)
        {
          const uint32_t sysIndex = m_ready[ctx.start];
          executeSystem(sysIndex, world, dt);
        });
        jobs.Wait(handle);
      }

      for (uint32_t idx : m_ready)
      {
        m_completed[idx] = 1;
        remaining--;
      }
    }
  }

  void Scheduler::executeSystem(uint32_t index, World& world, float dt)
  {
    SystemRecord& sys = m_systems[index];
    if (!sys.fn)
      return;
    const Tick start = nowTicks();
    ScopedTimer scopeTimer(sys.scopeId);
    sys.fn(world, dt, sys.user);
    const Tick end = nowTicks();
    sys.frameTicks = end - start;
  }

  bool Scheduler::depsReady(const SystemRecord& sys) const
  {
    for (uint32_t dep : sys.deps)
    {
      if (dep >= m_completed.size())
        return false;
      if (!m_completed[dep])
        return false;
    }
    return true;
  }

  uint32_t Scheduler::findSystemIndex(const char* name) const
  {
    if (!name)
      return 0xFFFFFFFFu;
    for (uint32_t i = 0; i < (uint32_t)m_systems.size(); ++i)
    {
      const char* n = m_systems[i].name;
      if (n && std::strcmp(n, name) == 0)
        return i;
    }
    return 0xFFFFFFFFu;
  }

  void Scheduler::publishStats()
  {
    SchedulerStatsSnapshot snap{};
    snap.count = 0;

    for (const auto& sys : m_systems)
    {
      if (snap.count >= 32)
        break;
      const uint64_t ticks = sys.frameTicks;
      const double ms = ticksToSeconds(ticks) * 1000.0;
      snap.entries[snap.count].name = sys.name;
      snap.entries[snap.count].phase = sys.phase;
      snap.entries[snap.count].ms = ms;
      snap.count++;
    }

    const uint32_t back = 1u - m_statsIndex.load(std::memory_order_relaxed);
    m_stats[back] = snap;
    m_statsIndex.store(back, std::memory_order_release);
  }

  SchedulerStatsSnapshot Scheduler::statsSnapshot() const
  {
    const uint32_t idx = m_statsIndex.load(std::memory_order_acquire);
    return m_stats[idx];
  }
}
