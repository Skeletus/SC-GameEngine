#pragma once
#include "sc_time.h"
#include "sc_memory.h"

#include <atomic>
#include <cstdint>
#include <condition_variable>
#include <mutex>
#include <type_traits>
#include <utility>
#include <new>

namespace sc
{
  struct JobContext
  {
    uint32_t start = 0;
    uint32_t end = 0;
    uint32_t groupIndex = 0;
    uint32_t groupCount = 0;
    uint32_t workerIndex = 0;
  };

  struct JobsTelemetrySnapshot
  {
    uint32_t workerThreads = 0;
    uint64_t jobsEnqueued = 0;
    uint64_t jobsCompleted = 0;
    uint64_t jobsPending = 0;
    double totalJobMs = 0.0;
    ScopeTop topScopes{};
  };

  struct JobFence
  {
    std::atomic<int32_t> count{ 0 };
    std::mutex m;
    std::condition_variable cv;
    std::atomic<uint32_t> inUse{ 0 };
  };

  struct JobHandle
  {
    JobFence* fence = nullptr;
  };

  struct JobItem
  {
    JobContext ctx{};
    void (*fn)(const JobContext&, void*) = nullptr;
    void (*destroy)(void*) = nullptr;
    void* user = nullptr;
    JobFence* fence = nullptr;
    uint32_t scopeId = 0xFFFFFFFFu;
  };

  class JobSystem
  {
  public:
    bool init(uint32_t numThreads);
    void shutdown();

    void beginFrame();
    void publishFrameTelemetry();
    JobsTelemetrySnapshot getTelemetrySnapshot() const { return m_lastSnapshot; }

    void Kick(JobHandle handle);
    void Wait(JobHandle handle);

    template<typename F>
    JobHandle Dispatch(uint32_t count, uint32_t groupSize, F&& f)
    {
      if (count == 0 || groupSize == 0)
        return JobHandle{};

      const uint32_t groupCount = (count + groupSize - 1u) / groupSize;
      JobHandle handle = allocFence(groupCount);
      if (!handle.fence)
        return JobHandle{};

      using FnType = typename std::decay<F>::type;
      struct Payload
      {
        FnType fn;
      };

      const uint32_t scope = m_scopeJobsExecute;

      uint32_t enqueued = 0;
      for (uint32_t groupIndex = 0; groupIndex < groupCount; ++groupIndex)
      {
        const uint32_t start = groupIndex * groupSize;
        const uint32_t end = (start + groupSize > count) ? count : (start + groupSize);

        void* mem = allocPayload(sizeof(Payload), alignof(Payload));
        if (!mem)
        {
          handle.fence->count.store((int32_t)enqueued, std::memory_order_release);
          if (enqueued == 0)
          {
            releaseFence(handle);
            return JobHandle{};
          }
          break;
        }

        Payload* payload = new (mem) Payload{ static_cast<F&&>(f) };

        JobItem job{};
        job.ctx.start = start;
        job.ctx.end = end;
        job.ctx.groupIndex = groupIndex;
        job.ctx.groupCount = groupCount;
        job.fn = [](const JobContext& ctx, void* user)
        {
          auto* p = static_cast<Payload*>(user);
          p->fn(ctx);
        };
        job.destroy = [](void* user)
        {
          auto* p = static_cast<Payload*>(user);
          p->~Payload();
        };
        job.user = payload;
        job.fence = handle.fence;
        job.scopeId = scope;

        enqueue(job);
        enqueued++;
      }

      Kick(handle);
      return handle;
    }

    template<typename F>
    void DispatchAsync(F&& f, uint32_t scopeId = 0xFFFFFFFFu)
    {
      using FnType = typename std::decay<F>::type;
      struct Payload
      {
        FnType fn;
        explicit Payload(FnType&& in) : fn(std::move(in)) {}
      };

      MallocAllocator alloc;
      void* mem = alloc.allocate(sizeof(Payload), alignof(Payload), MemTag::Jobs, __FILE__, __LINE__);
      if (!mem)
      {
        JobContext ctx{};
        ctx.workerIndex = m_numWorkers;
        const uint32_t scope = (scopeId == 0xFFFFFFFFu) ? m_scopeJobsExecute : scopeId;
        { ScopedTimer frameTimer(&m_frameJobTicks); ScopedTimer scopeTimer(scope); f(ctx); }
        m_jobsCompleted.fetch_add(1, std::memory_order_relaxed);
        m_frameJobsCompleted.fetch_add(1, std::memory_order_relaxed);
        return;
      }

      Payload* payload = new (mem) Payload(static_cast<F&&>(f));
      const uint32_t scope = (scopeId == 0xFFFFFFFFu) ? m_scopeJobsExecute : scopeId;

      JobItem job{};
      job.ctx.start = 0;
      job.ctx.end = 1;
      job.ctx.groupIndex = 0;
      job.ctx.groupCount = 1;
      job.fn = [](const JobContext& ctx, void* user)
      {
        auto* p = static_cast<Payload*>(user);
        p->fn(ctx);
      };
      job.destroy = [](void* user)
      {
        auto* p = static_cast<Payload*>(user);
        p->~Payload();
        MallocAllocator a;
        a.deallocate(p, sizeof(Payload), MemTag::Jobs);
      };
      job.user = payload;
      job.scopeId = scope;

      enqueue(job);
    }

  private:
    void* allocPayload(size_t size, size_t align);
    JobHandle allocFence(uint32_t count);
    void releaseFence(JobHandle handle);

    void enqueue(const JobItem& job);
    bool runOne(uint32_t workerIndex);
    void workerMain(uint32_t workerIndex);

  private:
    JobsTelemetrySnapshot m_lastSnapshot{};
    std::atomic<uint64_t> m_jobsQueued{ 0 };
    std::atomic<uint64_t> m_jobsEnqueued{ 0 };
    std::atomic<uint64_t> m_jobsCompleted{ 0 };
    std::atomic<uint64_t> m_frameJobsEnqueued{ 0 };
    std::atomic<uint64_t> m_frameJobsCompleted{ 0 };
    std::atomic<uint64_t> m_frameJobTicks{ 0 };

    uint32_t m_scopeJobsExecute = 0xFFFFFFFFu;

    std::atomic<bool> m_shutdown{ false };
    uint32_t m_numWorkers = 0;
    std::atomic<uint32_t> m_rr{ 0 };
    std::condition_variable m_wakeCv;
    std::mutex m_wakeMutex;

    struct Worker;
    Worker* m_workers = nullptr;
    LinearFrameAllocator m_payloadAlloc{};

    // Fences
    static constexpr uint32_t kMaxFences = 256;
    JobFence m_fences[kMaxFences]{};
    std::atomic<uint32_t> m_fenceHead{ 0 };
  };

  JobSystem& jobs();
}
