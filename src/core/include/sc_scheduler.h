#pragma once
#include "sc_ecs.h"
#include "sc_time.h"
#include "sc_jobs.h"

#include <vector>
#include <atomic>
#include <initializer_list>

namespace sc
{
  enum class SystemPhase : uint8_t
  {
    Input = 0,
    Simulation,
    FixedUpdate,
    RenderPrep,
    Render,
    Count
  };

  struct SchedulerStatEntry
  {
    const char* name = nullptr;
    SystemPhase phase = SystemPhase::Simulation;
    double ms = 0.0;
  };

  struct SchedulerStatsSnapshot
  {
    uint32_t count = 0;
    SchedulerStatEntry entries[32]{};
  };

  class Scheduler
  {
  public:
    using SystemFn = void(*)(World&, float, void*);

    void addSystem(const char* name,
                   SystemPhase phase,
                   SystemFn fn,
                   void* user = nullptr,
                   std::initializer_list<const char*> deps = {});

    void finalize();
    void tick(World& world, float dt, uint32_t fixedSteps = 0, float fixedDt = 0.0f);

    SchedulerStatsSnapshot statsSnapshot() const;

  private:
    struct SystemRecord
    {
      const char* name = nullptr;
      SystemPhase phase = SystemPhase::Simulation;
      SystemFn fn = nullptr;
      void* user = nullptr;
      std::vector<const char*> depNames;
      std::vector<uint32_t> deps;
      uint32_t scopeId = 0xFFFFFFFFu;
      Tick frameTicks = 0;
    };

    void runPhase(SystemPhase phase, World& world, float dt);
    void executeSystem(uint32_t index, World& world, float dt);
    bool depsReady(const SystemRecord& sys) const;

    uint32_t findSystemIndex(const char* name) const;
    void publishStats();

  private:
    std::vector<SystemRecord> m_systems;
    std::vector<uint32_t> m_phaseLists[(uint32_t)SystemPhase::Count];
    std::vector<uint8_t> m_completed;
    std::vector<uint32_t> m_ready;

    SchedulerStatsSnapshot m_stats[2]{};
    std::atomic<uint32_t> m_statsIndex{ 0 };
  };
}
