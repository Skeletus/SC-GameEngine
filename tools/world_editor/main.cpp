#include <SDL.h>
#include <imgui.h>
#include <backends/imgui_impl_sdl2.h>

#include "sc_engine_render.h"
#include "editor_core/editor_core.h"
#include "world_format.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <vector>

using sc::editor::EditorAssetRegistry;
using sc::editor::EditorCamera;
using sc::editor::EditorDocument;
using sc::editor::EditorEntity;
using sc::editor::EditorTransform;
using sc::editor::CommandStack;
using sc::editor::CmdDeleteEntity;
using sc::editor::CmdPlaceEntity;
using sc::editor::CmdTransformEntity;
using sc::editor::GizmoState;

static std::string ResolveBasePath()
{
  char* sdl_base = SDL_GetBasePath();
  if (sdl_base)
  {
    std::string out = sdl_base;
    SDL_free(sdl_base);
    return out;
  }
  return std::filesystem::current_path().string();
}

static std::string ResolveAssetRoot()
{
  if (const char* env = std::getenv("SC_ASSET_ROOT"))
  {
    if (*env != '\0')
      return env;
  }
  std::filesystem::path p = ResolveBasePath();
  p /= "assets";
  return p.string();
}

static std::string ResolveWorldRoot()
{
  if (const char* env = std::getenv("SC_WORLD_ROOT"))
  {
    if (*env != '\0')
      return env;
  }
  std::filesystem::path p = ResolveAssetRoot();
  p /= "world";
  return p.string();
}

int main(int argc, char** argv)
{
  (void)argc;
  (void)argv;

  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER | SDL_INIT_EVENTS) != 0)
  {
    std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
    return 1;
  }

  SDL_Window* window = SDL_CreateWindow(
    "SC World Editor",
    SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
    1280, 720,
    SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE
  );
  if (!window)
  {
    std::fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
    return 1;
  }

  ScRenderInitParams init{};
  const std::string asset_root = ResolveAssetRoot();
  init.asset_root = asset_root.c_str();
  init.enable_validation = 0;
  scRenderInitialize(&init);

  int w = 0;
  int h = 0;
  SDL_GetWindowSize(window, &w, &h);

  ScRenderContextDesc ctx_desc{};
  ctx_desc.width = static_cast<uint32_t>(w);
  ctx_desc.height = static_cast<uint32_t>(h);
  ctx_desc.vsync = 1;

  ScRenderContext* render_ctx = scRenderCreateContext(window, &ctx_desc);
  if (!render_ctx)
  {
    std::fprintf(stderr, "scRenderCreateContext failed. Vulkan init likely failed.\n");
    return 1;
  }

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGui::StyleColorsDark();
  ImGui_ImplSDL2_InitForVulkan(window);
  scRenderImGuiInit(render_ctx);

  EditorDocument doc;
  sc::editor::InitDocument(&doc);

  EditorAssetRegistry registry;
  std::filesystem::path registry_path = ResolveWorldRoot();
  registry_path /= "asset_registry.txt";
  registry.loadFromFile(registry_path.string().c_str());

  EditorCamera camera;
  CommandStack cmd_stack;
  GizmoState gizmo;

  const int32_t initial_sector_x = static_cast<int32_t>(std::floor(camera.position[0] / doc.sectorSize));
  const int32_t initial_sector_z = static_cast<int32_t>(std::floor(camera.position[2] / doc.sectorSize));
  doc.sector.x = initial_sector_x;
  doc.sector.z = initial_sector_z;

  const std::string world_root = ResolveWorldRoot();
  const std::string initial_path = sc_world::BuildSectorPath(world_root.c_str(), doc.sector);
  sc_world::SectorFile initial_file{};
  if (sc_world::ReadSectorFile(initial_path.c_str(), &initial_file))
  {
    sc::editor::DocumentFromSectorFile(&doc, initial_file, render_ctx, registry);
  }
  else if (!registry.entries.empty())
  {
    const int count = std::min<int>(3, static_cast<int>(registry.entries.size()));
    for (int i = 0; i < count; ++i)
    {
      EditorTransform t{};
      t.position[0] = static_cast<float>(i) * 2.0f;
      t.position[1] = 0.5f;
      t.position[2] = 0.0f;
      EditorEntity* e = sc::editor::AddEntity(&doc, registry.entries[i], t);
      if (e)
        sc::editor::ResolveEntityAssets(e, render_ctx, registry);
    }
  }

  bool running = true;
  uint64_t perf_freq = SDL_GetPerformanceFrequency();
  uint64_t last_counter = SDL_GetPerformanceCounter();

  bool left_mouse_down = false;
  bool left_mouse_released = false;

  while (running)
  {
    SDL_Event e;
    left_mouse_released = false;
    while (SDL_PollEvent(&e))
    {
      ImGui_ImplSDL2_ProcessEvent(&e);
      if (e.type == SDL_QUIT)
        running = false;
      if (e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED)
        scRenderResize(render_ctx, static_cast<uint32_t>(e.window.data1), static_cast<uint32_t>(e.window.data2));
      if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT)
        left_mouse_down = true;
      if (e.type == SDL_MOUSEBUTTONUP && e.button.button == SDL_BUTTON_LEFT)
      {
        left_mouse_down = false;
        left_mouse_released = true;
      }
    }

    uint64_t now = SDL_GetPerformanceCounter();
    float dt = static_cast<float>(static_cast<double>(now - last_counter) / static_cast<double>(perf_freq));
    last_counter = now;

    const Uint8* ks = SDL_GetKeyboardState(nullptr);

    int mx = 0;
    int my = 0;
    Uint32 mouse_buttons = SDL_GetMouseState(&mx, &my);
    const bool right_down = (mouse_buttons & SDL_BUTTON(SDL_BUTTON_RIGHT)) != 0;
    SDL_SetRelativeMouseMode(right_down ? SDL_TRUE : SDL_FALSE);
    if (right_down)
    {
      int dx = 0;
      int dy = 0;
      SDL_GetRelativeMouseState(&dx, &dy);
      camera.yaw += dx * camera.lookSpeed;
      camera.pitch += -dy * camera.lookSpeed;
      camera.pitch = std::max(-1.5f, std::min(1.5f, camera.pitch));
    }
    else
    {
      SDL_GetRelativeMouseState(nullptr, nullptr);
    }

    float forward[3]{};
    sc::editor::CameraForward(&camera, forward);
    const float right[3] = { forward[2], 0.0f, -forward[0] };
    if (ks[SDL_SCANCODE_W])
    {
      camera.position[0] += forward[0] * camera.moveSpeed * dt;
      camera.position[1] += forward[1] * camera.moveSpeed * dt;
      camera.position[2] += forward[2] * camera.moveSpeed * dt;
    }
    if (ks[SDL_SCANCODE_S])
    {
      camera.position[0] -= forward[0] * camera.moveSpeed * dt;
      camera.position[1] -= forward[1] * camera.moveSpeed * dt;
      camera.position[2] -= forward[2] * camera.moveSpeed * dt;
    }
    if (ks[SDL_SCANCODE_A])
    {
      camera.position[0] -= right[0] * camera.moveSpeed * dt;
      camera.position[2] -= right[2] * camera.moveSpeed * dt;
    }
    if (ks[SDL_SCANCODE_D])
    {
      camera.position[0] += right[0] * camera.moveSpeed * dt;
      camera.position[2] += right[2] * camera.moveSpeed * dt;
    }
    if (ks[SDL_SCANCODE_Q])
      camera.position[1] -= camera.moveSpeed * dt;
    if (ks[SDL_SCANCODE_E])
      camera.position[1] += camera.moveSpeed * dt;

    ImGui_ImplSDL2_NewFrame();
    scRenderImGuiNewFrame(render_ctx);
    ImGui::NewFrame();

    const int32_t cam_sector_x = static_cast<int32_t>(std::floor(camera.position[0] / doc.sectorSize));
    const int32_t cam_sector_z = static_cast<int32_t>(std::floor(camera.position[2] / doc.sectorSize));

    ImGui::Begin("World");
    ImGui::Text("Camera Sector: %d, %d", cam_sector_x, cam_sector_z);
    ImGui::Checkbox("Snap To Grid", &doc.snapToGrid);
    ImGui::DragFloat("Grid Size", &doc.gridSize, 0.1f, 0.1f, 10.0f);
    if (ImGui::Button("Save Sector"))
    {
      doc.sector.x = cam_sector_x;
      doc.sector.z = cam_sector_z;
      sc_world::SectorFile file{};
      sc::editor::SectorFileFromDocument(&doc, &file);

      std::filesystem::create_directories(std::filesystem::path(world_root) / "sectors");
      const std::string path = sc_world::BuildSectorPath(world_root.c_str(), doc.sector);
      sc_world::WriteSectorFile(path.c_str(), file);

      sc_world::WorldManifest manifest{};
      const std::string manifest_path = sc_world::BuildWorldManifestPath(world_root.c_str());
      sc_world::ReadWorldManifest(manifest_path.c_str(), &manifest);
      bool exists = false;
      for (const auto& s : manifest.sectors)
      {
        if (s.x == doc.sector.x && s.z == doc.sector.z)
        {
          exists = true;
          break;
        }
      }
      if (!exists)
        manifest.sectors.push_back(doc.sector);
      sc_world::WriteWorldManifest(manifest_path.c_str(), manifest);
    }
    if (ImGui::Button("Load Sector"))
    {
      doc.sector.x = cam_sector_x;
      doc.sector.z = cam_sector_z;
      const std::string path = sc_world::BuildSectorPath(world_root.c_str(), doc.sector);
      sc_world::SectorFile file{};
      if (sc_world::ReadSectorFile(path.c_str(), &file))
        sc::editor::DocumentFromSectorFile(&doc, file, render_ctx, registry);
    }
    ImGui::End();

    ImGui::Begin("Palette");
    for (const auto& entry : registry.entries)
    {
      if (ImGui::Button(entry.label.c_str()))
      {
        EditorEntity e{};
        e.id = doc.nextId++;
        std::snprintf(e.name, sizeof(e.name), "%s", entry.label.c_str());
        e.meshAssetId = entry.mesh_id;
        e.materialAssetId = entry.material_id;
        e.transform.position[0] = camera.position[0] + forward[0] * 5.0f;
        e.transform.position[1] = camera.position[1] + forward[1] * 5.0f;
        e.transform.position[2] = camera.position[2] + forward[2] * 5.0f;
        if (doc.snapToGrid)
          sc::editor::SnapTransform(&e.transform, doc.gridSize);
        sc::editor::ResolveEntityAssets(&e, render_ctx, registry);

        auto cmd = std::make_unique<CmdPlaceEntity>();
        cmd->entity = e;
        cmd_stack.execute(std::move(cmd), &doc);
      }
    }
    ImGui::End();

    ImGui::Begin("Inspector");
    if (doc.selectedId != 0)
    {
      EditorEntity* selected = sc::editor::FindEntity(&doc, doc.selectedId);
      if (selected)
      {
        ImGui::Text("Entity %llu", (unsigned long long)selected->id);
        ImGui::DragFloat3("Position", selected->transform.position, 0.1f);
      }
    }
    ImGui::End();

    ImGui::Begin("Viewport", nullptr, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImVec2 vp_pos = ImGui::GetWindowPos();
    ImVec2 vp_size = ImGui::GetContentRegionAvail();
    if (vp_size.x < 1.0f) vp_size.x = 1.0f;
    if (vp_size.y < 1.0f) vp_size.y = 1.0f;
    bool viewport_hovered = ImGui::IsWindowHovered();
    ImGui::InvisibleButton("viewport_capture", vp_size);
    ImGui::End();

    const bool ctrl = ImGui::GetIO().KeyCtrl;
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Z))
      cmd_stack.undoLast(&doc);
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Y))
      cmd_stack.redoLast(&doc);

    if (viewport_hovered && left_mouse_down && !ImGui::GetIO().WantCaptureMouse)
    {
      sc::editor::Ray ray = sc::editor::BuildPickRay(&camera,
                                                     static_cast<float>(mx),
                                                     static_cast<float>(my),
                                                     vp_pos.x, vp_pos.y,
                                                     vp_size.x, vp_size.y);
      doc.selectedId = sc::editor::PickEntity(&doc, ray);
    }

    if (doc.selectedId != 0 && viewport_hovered && !ImGui::GetIO().WantCaptureMouse)
    {
      EditorEntity* sel = sc::editor::FindEntity(&doc, doc.selectedId);
      if (sel)
      {
        const bool was_active = gizmo.active;
        bool dragging = (mouse_buttons & SDL_BUTTON(SDL_BUTTON_LEFT)) != 0;
        sc::editor::GizmoTranslate(&gizmo, sel, &camera,
          static_cast<float>(mx), static_cast<float>(my),
          vp_pos.x, vp_pos.y, vp_size.x, vp_size.y,
          left_mouse_down, dragging, left_mouse_released,
          doc.snapToGrid, doc.gridSize);

        if (was_active && left_mouse_released)
        {
          auto diff = [](float a, float b) { return std::fabs(a - b) > 1e-4f; };
          const bool moved =
            diff(gizmo.startTransform.position[0], sel->transform.position[0]) ||
            diff(gizmo.startTransform.position[1], sel->transform.position[1]) ||
            diff(gizmo.startTransform.position[2], sel->transform.position[2]);

          if (moved)
          {
            auto cmd = std::make_unique<CmdTransformEntity>();
            cmd->entityId = sel->id;
            cmd->before = gizmo.startTransform;
            cmd->after = sel->transform;
            cmd_stack.execute(std::move(cmd), &doc);
          }
        }
      }
    }

    if (ImGui::IsKeyPressed(ImGuiKey_Delete) && doc.selectedId != 0)
    {
      EditorEntity removed{};
      if (sc::editor::RemoveEntity(&doc, doc.selectedId, &removed))
      {
        auto cmd = std::make_unique<CmdDeleteEntity>();
        cmd->entity = removed;
        cmd_stack.execute(std::move(cmd), &doc);
      }
    }

    std::vector<ScRenderDrawItem> draw_items;
    sc::editor::BuildDrawItems(&doc, &draw_items);

    std::vector<ScRenderLine> debug_lines;
    sc::editor::BuildDebugLines(&doc, &debug_lines);

    const float vp_w = std::max(1.0f, vp_size.x);
    const float vp_h = std::max(1.0f, vp_size.y);
    const sc::Mat4 view = sc::editor::CameraView(&camera);
    const sc::Mat4 proj = sc::editor::CameraProj(&camera, vp_w / vp_h);

    ScRenderFrameDesc frame{};
    std::memcpy(frame.view, view.m, sizeof(frame.view));
    std::memcpy(frame.proj, proj.m, sizeof(frame.proj));
    frame.camera_pos[0] = camera.position[0];
    frame.camera_pos[1] = camera.position[1];
    frame.camera_pos[2] = camera.position[2];
    frame.time_sec = static_cast<float>(SDL_GetTicks()) / 1000.0f;

    scRenderBeginFrame(render_ctx, &frame);

    if (!draw_items.empty())
    {
      ScRenderDrawList list{};
      list.items = draw_items.data();
      list.count = static_cast<uint32_t>(draw_items.size());
      scRenderSubmit(render_ctx, &list);
    }

    if (!debug_lines.empty())
    {
      ScRenderDebugDraw dbg{};
      dbg.lines = debug_lines.data();
      dbg.count = static_cast<uint32_t>(debug_lines.size());
      scRenderSubmitDebug(render_ctx, &dbg);
    }

    ImGui::Render();
    scRenderRenderImGui(render_ctx, ImGui::GetDrawData());
    scRenderEndFrame(render_ctx);
  }

  scRenderImGuiShutdown(render_ctx);
  ImGui_ImplSDL2_Shutdown();
  ImGui::DestroyContext();

  scRenderDestroyContext(render_ctx);
  scRenderShutdown();

  SDL_DestroyWindow(window);
  SDL_Quit();
  return 0;
}
