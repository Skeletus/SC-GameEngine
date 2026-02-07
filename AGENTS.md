# SandboxCityEngine — AGENTS.md

## 1. Project Purpose

**SandboxCityEngine** is a **custom C++ game engine** built **exclusively** for **GTA-like open-world games** (urban sandbox, persistent world, aggressive streaming, traffic, NPCs, and vehicles).

This engine is **NOT** intended to be:

* a generic engine (Unity / Unreal alternative)
* an academic framework
* a multi-platform engine at this stage

This engine **IS** intended to be:

* performance-driven and predictable
* architecturally clear and data-oriented
* scalable to large worlds
* supported by serious tooling (external editor)
* used to build a real, shippable game

---

## 2. Architectural Philosophy (Non-Negotiable)

### 2.1 Strict Layer Separation

```
Core        → math, jobs, IO, serialization, utilities
Engine      → runtime (ECS, renderer, streaming, physics, traffic)
Tools       → external editor, authoring pipelines
Sandbox     → test / debug application
```

Rules:

* ❌ Runtime must NOT depend on editor code
* ❌ Editor must NOT execute gameplay logic
* ✅ Shared data & formats live in `shared/` modules

---

### 2.2 Data-Oriented First

* Flat ECS, no deep hierarchies
* Explicit systems, explicit execution order
* No gameplay logic in constructors
* No magic singletons
* Everything must be measurable (timers, counters, budgets)

---

### 2.3 Open-World as the Core Constraint

All decisions must scale to:

* sector-based streaming
* strict CPU / GPU budgets
* zero-stutter traversal
* graceful degradation (LOD, simulation tiers)

If a system does not scale to **thousands of entities**, it does not belong.

---

## 3. Current Engine State (Implemented Systems)

### 3.1 Core

* Multithreaded job system
* Persistent multi-frame jobs
* Scoped profiling
* Custom math library (`Mat4`, Vulkan RH ZO projection)
* Binary serialization utilities

### 3.2 Rendering

* Vulkan (Windows)
* Robust swapchain (resize / minimize safe)
* Pipelines: unlit, textured, debug
* Descriptor sets + push constants
* Asset residency (meshes, textures)
* DebugDraw (lines, bounds)
* Renderer extracted as reusable library: **`engine_render_Debug.dll`**

### 3.3 ECS

* Entity / component system
* Phase-based scheduler
* Transform, Camera, RenderMesh, Name components
* Decoupled systems

### 3.4 Open World

* World grid partitioned into **sectors**
* True async streaming (I/O + Jobs)
* Sector activation / eviction
* Budgets (sectors, entities, draws)
* Sector pinning for critical gameplay
* Culling and visibility filtering

### 3.5 Physics

* Bullet Physics integration
* Fixed timestep simulation
* Static colliders per sector
* Dynamic & kinematic rigid bodies
* Raycasts and sweeps
* Physics debug visualization

### 3.6 Vehicles

* Raycast vehicle model
* Tunable handling parameters
* Chase camera with occlusion
* Integrated with streaming & pinning

### 3.7 Traffic

* Lane graph per sector
* Sector-aware spawners
* Lane-following AI
* Minimal avoidance
* Simulation LOD:

  * Tier A: full physics
  * Tier B: kinematic
  * Tier C: on-rails

### 3.8 Tooling (External Editor)

* Separate application: `tools/world_editor`
* Uses the **same renderer as the runtime**
* ImGui with Docking (Unity-like layout)
* Central Viewport
* Prefab Palette
* Sector save/load
* Selection groundwork in progress

---

## 4. External Editor — Vision & Rules

### 4.1 What the Editor IS

* A **world authoring tool**
* Data-driven
* Non-gameplay
* Uses engine rendering for visual parity

### 4.2 What the Editor IS NOT

* Not a runtime
* Not a demo
* Not a duplicate engine

### 4.3 Editor Development Phases

#### E1 — Layout (COMPLETED)

* Docking layout (Unity-style)
* Viewport / Hierarchy / Inspector / Project / Toolbar

#### E2 — Selection & Hierarchy (COMPLETED)

* Viewport picking
* Hierarchy selection
* Visual highlight

#### E3 — Inspector (COMPLETED)

* Transform editing
* Mesh / material assignment
* Delete / duplicate / focus

#### E4 — Gizmos (COMPLETED)

* Move / Rotate / Scale
* Undo / Redo

#### E5 — Asset Pipeline (IN PROGRESS)

* Project Browser + Asset Database
* Texture Import + Preview + Assignment
* Mesh Import (glTF recomendado) + Preview + Placement
* Prefabs (player, casa, farola) y auto-setup

---

## 5. World Data Formats (Data-Driven)

### 5.1 WorldManifest

* Sector size
* World seed
* Existing sector list

### 5.2 SectorFile (`sector_x_z.scsector`)

Contains:

* Instances (transform + asset IDs)
* Simplified colliders
* Lanes
* Spawners
* Metadata

Format requirements:

* Binary
* Versioned
* Chunk-based
* Shared by editor and runtime

---

## 6. Technical Roadmap (High Level)

### Phase 4.x

* Complete editor (E2–E5)
* Fully data-driven world

### Phase 5.0

* NPCs / pedestrians
* Navigation per sector
* Crowd simulation

### Phase 6.0

* Missions / events
* Triggers and lightweight scripting

### Phase 7.0

* Final optimization
* HLOD
* Aggressive instancing
* Frame pacing polish

---

## 7. Rules for Automated Agents (Codex / AI)

If you are an automated agent:

* ❌ Do not invent new architecture
* ❌ Do not add unnecessary dependencies
* ✅ Respect ECS, streaming, and budgets
* ✅ Implement in clear, incremental phases
* ✅ Prefer clarity over cleverness
* ✅ Always leave the project compiling

If a task is large:
➡️ **Split it into explicit phases**

---

## 8. Definition of “Engine Ready”

SandboxCityEngine is considered **production-ready** when:

* The entire world can be authored from the editor
* Runtime streams sectors without stutter
* Vehicles, traffic, and NPCs coexist
* Content creation does not require code changes

---

## 9. Final Vision

This engine is not a toy.
It is the foundation for a **real urban sandbox game**.

Every system must justify its cost.
Every line of code must scale.
