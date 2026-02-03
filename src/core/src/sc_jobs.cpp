#include "sc_jobs.h"
#include "sc_log.h"

#include <functional>
#include <thread>
#include <chrono>

namespace sc
{
  namespace
  {
    static constexpr uint32_t kQueueSize = 1024; // power of two

    struct MPMCQueue
    {
      struct Cell
      {
        std::atomic<uint32_t> seq;
        JobItem job;
      };

      Cell* buffer = nullptr;
      uint32_t mask = 0;
      std::atomic<uint32_t> enqueuePos{ 0 };
      std::atomic<uint32_t> dequeuePos{ 0 };

      bool init(uint32_t size)
      {
        buffer = new Cell[size];
        if (!buffer) return false;
        mask = size - 1;
        for (uint32_t i = 0; i < size; ++i)
          buffer[i].seq.store(i, std::memory_order_relaxed);
        enqueuePos.store(0, std::memory_order_relaxed);
        dequeuePos.store(0, std::memory_order_relaxed);
        return true;
      }

      void shutdown()
      {
        delete[] buffer;
        buffer = nullptr;
        mask = 0;
      }

      bool enqueue(const JobItem& job)
      {
        Cell* cell = nullptr;
        uint32_t pos = enqueuePos.load(std::memory_order_relaxed);
        for (;;)
        {
          cell = &buffer[pos & mask];
          const uint32_t seq = cell->seq.load(std::memory_order_acquire);
          const int32_t diff = (int32_t)seq - (int32_t)pos;
          if (diff == 0)
          {
            if (enqueuePos.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed))
              break;
          }
          else if (diff < 0)
          {
            return false; // full
          }
          else
          {
            pos = enqueuePos.load(std::memory_order_relaxed);
          }
        }

        cell->job = job;
        cell->seq.store(pos + 1, std::memory_order_release);
        return true;
      }

      bool dequeue(JobItem& out)
      {
        Cell* cell = nullptr;
        uint32_t pos = dequeuePos.load(std::memory_order_relaxed);
        for (;;)
        {
          cell = &buffer[pos & mask];
          const uint32_t seq = cell->seq.load(std::memory_order_acquire);
          const int32_t diff = (int32_t)seq - (int32_t)(pos + 1);
          if (diff == 0)
          {
            if (dequeuePos.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed))
              break;
          }
          else if (diff < 0)
          {
            return false; // empty
          }
          else
          {
            pos = dequeuePos.load(std::memory_order_relaxed);
          }
        }

        out = cell->job;
        cell->seq.store(pos + mask + 1, std::memory_order_release);
        return true;
      }
    };
  }

  struct JobSystem::Worker
  {
    std::thread thread;
    MPMCQueue queue;
    uint32_t index = 0;
  };

  static JobSystem g_jobs;

  JobSystem& jobs()
  {
    return g_jobs;
  }

  bool JobSystem::init(uint32_t numThreads)
  {
    if (numThreads == 0) numThreads = 1;
    m_numWorkers = numThreads;
    m_shutdown.store(false, std::memory_order_relaxed);

    if (!m_payloadAlloc.init(2 * 1024 * 1024, MemTag::Jobs))
      return false;

    m_scopeJobsExecute = registerScope("Jobs/Execute");

    m_workers = new Worker[m_numWorkers];
    if (!m_workers) return false;

    for (uint32_t i = 0; i < m_numWorkers; ++i)
    {
      m_workers[i].index = i;
      if (!m_workers[i].queue.init(kQueueSize))
        return false;
    }

    for (uint32_t i = 0; i < m_numWorkers; ++i)
    {
      m_workers[i].thread = std::thread([this, i]() { workerMain(i); });
#if defined(SC_DEBUG)
      sc::log(sc::LogLevel::Debug, "Job worker started: %u", i);
#endif
    }

    return true;
  }

  void JobSystem::shutdown()
  {
    m_shutdown.store(true, std::memory_order_relaxed);
    m_wakeCv.notify_all();

    for (uint32_t i = 0; i < m_numWorkers; ++i)
    {
      if (m_workers[i].thread.joinable())
        m_workers[i].thread.join();
    }

    for (uint32_t i = 0; i < m_numWorkers; ++i)
      m_workers[i].queue.shutdown();

    delete[] m_workers;
    m_workers = nullptr;
    m_numWorkers = 0;

    m_payloadAlloc.shutdown();
  }

  void JobSystem::beginFrame()
  {
    m_frameJobsEnqueued.store(0, std::memory_order_relaxed);
    m_frameJobsCompleted.store(0, std::memory_order_relaxed);
    m_frameJobTicks.store(0, std::memory_order_relaxed);
  }

  void JobSystem::publishFrameTelemetry()
  {
    JobsTelemetrySnapshot snap{};
    snap.workerThreads = m_numWorkers;
    snap.jobsEnqueued = m_frameJobsEnqueued.load(std::memory_order_relaxed);
    snap.jobsCompleted = m_frameJobsCompleted.load(std::memory_order_relaxed);
    snap.jobsPending = m_jobsQueued.load(std::memory_order_relaxed);
    const uint64_t ticks = m_frameJobTicks.load(std::memory_order_relaxed);
    snap.totalJobMs = ticksToSeconds(ticks) * 1000.0;
    snap.topScopes = snapshotTopScopes(5);

    m_lastSnapshot = snap;

    m_payloadAlloc.reset();
  }

  void JobSystem::Kick(JobHandle handle)
  {
    (void)handle;
    // No-op: enqueue already wakes workers; keep for API completeness.
  }

  void JobSystem::Wait(JobHandle handle)
  {
    if (!handle.fence) return;
    while (handle.fence->count.load(std::memory_order_acquire) > 0)
    {
      if (runOne(m_numWorkers))
        continue;

      std::unique_lock<std::mutex> lk(handle.fence->m);
      handle.fence->cv.wait_for(lk, std::chrono::microseconds(200), [&]()
      {
        return handle.fence->count.load(std::memory_order_acquire) == 0;
      });
    }

    releaseFence(handle);
  }

  void* JobSystem::allocPayload(size_t size, size_t align)
  {
    return m_payloadAlloc.allocate(size, align, MemTag::Jobs, __FILE__, __LINE__);
  }

  JobHandle JobSystem::allocFence(uint32_t count)
  {
    for (uint32_t i = 0; i < kMaxFences; ++i)
    {
      const uint32_t idx = m_fenceHead.fetch_add(1, std::memory_order_relaxed) % kMaxFences;
      uint32_t expected = 0;
      if (m_fences[idx].inUse.compare_exchange_strong(expected, 1, std::memory_order_acquire))
      {
        m_fences[idx].count.store((int32_t)count, std::memory_order_release);
        return JobHandle{ &m_fences[idx] };
      }
    }
    return JobHandle{};
  }

  void JobSystem::releaseFence(JobHandle handle)
  {
    if (!handle.fence) return;
    handle.fence->count.store(0, std::memory_order_release);
    handle.fence->inUse.store(0, std::memory_order_release);
  }

  void JobSystem::enqueue(const JobItem& job)
  {
    const uint32_t idx = m_rr.fetch_add(1, std::memory_order_relaxed) % m_numWorkers;
    if (m_workers[idx].queue.enqueue(job))
    {
      m_jobsQueued.fetch_add(1, std::memory_order_relaxed);
      m_jobsEnqueued.fetch_add(1, std::memory_order_relaxed);
      m_frameJobsEnqueued.fetch_add(1, std::memory_order_relaxed);
      m_wakeCv.notify_one();
      return;
    }

    // Queue was full, fall back to linear search
    for (uint32_t i = 0; i < m_numWorkers; ++i)
    {
      if (m_workers[i].queue.enqueue(job))
      {
        m_jobsQueued.fetch_add(1, std::memory_order_relaxed);
        m_jobsEnqueued.fetch_add(1, std::memory_order_relaxed);
        m_frameJobsEnqueued.fetch_add(1, std::memory_order_relaxed);
        m_wakeCv.notify_one();
        return;
      }
    }

    // If all queues full, execute on caller thread to avoid loss
    JobItem local = job;
    local.ctx.workerIndex = m_numWorkers;
    { ScopedTimer t(&m_frameJobTicks); local.fn(local.ctx, local.user); }
    if (local.destroy) local.destroy(local.user);
    if (local.fence)
    {
      const int32_t remaining = local.fence->count.fetch_sub(1, std::memory_order_acq_rel) - 1;
      if (remaining == 0)
      {
        std::lock_guard<std::mutex> lk(local.fence->m);
        local.fence->cv.notify_all();
      }
    }
    m_jobsCompleted.fetch_add(1, std::memory_order_relaxed);
    m_frameJobsCompleted.fetch_add(1, std::memory_order_relaxed);
  }

  bool JobSystem::runOne(uint32_t workerIndex)
  {
    if (m_numWorkers == 0) return false;

    JobItem job{};
    if (workerIndex < m_numWorkers)
    {
      if (m_workers[workerIndex].queue.dequeue(job))
      {
        m_jobsQueued.fetch_sub(1, std::memory_order_relaxed);
      }
      else
      {
        // steal
        for (uint32_t i = 0; i < m_numWorkers; ++i)
        {
          if (i == workerIndex) continue;
          if (m_workers[i].queue.dequeue(job))
          {
            m_jobsQueued.fetch_sub(1, std::memory_order_relaxed);
            break;
          }
        }
        if (!job.fn)
          return false;
      }
    }
    else
    {
      // main thread help: steal from any queue
      bool found = false;
      for (uint32_t i = 0; i < m_numWorkers; ++i)
      {
        if (m_workers[i].queue.dequeue(job))
        {
          m_jobsQueued.fetch_sub(1, std::memory_order_relaxed);
          found = true;
          break;
        }
      }
      if (!found)
        return false;
    }

    job.ctx.workerIndex = workerIndex;
    {
      ScopedTimer frameTimer(&m_frameJobTicks);
      ScopedTimer scopeTimer(job.scopeId);
      job.fn(job.ctx, job.user);
    }

    if (job.destroy) job.destroy(job.user);

    if (job.fence)
    {
      const int32_t remaining = job.fence->count.fetch_sub(1, std::memory_order_acq_rel) - 1;
      if (remaining == 0)
      {
        std::lock_guard<std::mutex> lk(job.fence->m);
        job.fence->cv.notify_all();
      }
    }

    m_jobsCompleted.fetch_add(1, std::memory_order_relaxed);
    m_frameJobsCompleted.fetch_add(1, std::memory_order_relaxed);
    return true;
  }

  void JobSystem::workerMain(uint32_t workerIndex)
  {
#if defined(SC_DEBUG)
    const auto tid = std::hash<std::thread::id>{}(std::this_thread::get_id());
    sc::log(sc::LogLevel::Debug, "Job worker %u thread id=%llu", workerIndex, (unsigned long long)tid);
#endif
    while (!m_shutdown.load(std::memory_order_relaxed))
    {
      if (runOne(workerIndex))
        continue;

      std::unique_lock<std::mutex> lk(m_wakeMutex);
      m_wakeCv.wait(lk, [&]()
      {
        return m_shutdown.load(std::memory_order_relaxed) || m_jobsQueued.load(std::memory_order_relaxed) > 0;
      });
    }
  }
}
