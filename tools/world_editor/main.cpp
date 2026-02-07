#include <SDL.h>
#include <imgui.h>
#include <imgui_internal.h>
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
#include <cstdint>

using sc::editor::EditorAssetRegistry;
using sc::editor::EditorCamera;
using sc::editor::EditorDocument;
using sc::editor::EditorEntity;
using sc::editor::EditorTransform;
using sc::editor::CommandStack;
using sc::editor::CmdDeleteEntity;
using sc::editor::CmdPlaceEntity;
using sc::editor::CmdTransformEntity;
using sc::editor::CmdSetProperty;
using sc::editor::GizmoState;

static constexpr float kEditorViewportAspect = 16.0f / 9.0f;

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

struct ViewportPanelState
{
  ImVec2 pos{};
  ImVec2 size{};
  ImVec2 render_pos{};
  ImVec2 render_size{};
  bool hovered = false;
  bool clicked = false;
};

static void FormatEntityLabel(const EditorEntity& e, char* out, size_t out_size)
{
  if (!out || out_size == 0)
    return;
  if (e.name[0] != '\0')
    std::snprintf(out, out_size, "%s", e.name);
  else
    std::snprintf(out, out_size, "Entity %llu", (unsigned long long)e.id);
}

static float DegToRad(float deg)
{
  return deg * 0.0174532925f;
}

static float RadToDeg(float rad)
{
  return rad * 57.2957795f;
}

static bool MeshBoundsValid(const ScRenderMeshInfo& info)
{
  const float dx = std::fabs(info.bounds_max[0] - info.bounds_min[0]);
  const float dy = std::fabs(info.bounds_max[1] - info.bounds_min[1]);
  const float dz = std::fabs(info.bounds_max[2] - info.bounds_min[2]);
  return (dx > 1e-4f) || (dy > 1e-4f) || (dz > 1e-4f);
}

static void GetEntityWorldAabb(const EditorEntity& e, float out_min[3], float out_max[3])
{
  float local_min[3] = { -0.5f, -0.5f, -0.5f };
  float local_max[3] = { 0.5f, 0.5f, 0.5f };

  if (e.meshHandle != 0 && MeshBoundsValid(e.meshInfo))
  {
    local_min[0] = e.meshInfo.bounds_min[0];
    local_min[1] = e.meshInfo.bounds_min[1];
    local_min[2] = e.meshInfo.bounds_min[2];
    local_max[0] = e.meshInfo.bounds_max[0];
    local_max[1] = e.meshInfo.bounds_max[1];
    local_max[2] = e.meshInfo.bounds_max[2];
  }

  float minx = local_min[0] * e.transform.scale[0];
  float miny = local_min[1] * e.transform.scale[1];
  float minz = local_min[2] * e.transform.scale[2];
  float maxx = local_max[0] * e.transform.scale[0];
  float maxy = local_max[1] * e.transform.scale[1];
  float maxz = local_max[2] * e.transform.scale[2];

  if (minx > maxx) std::swap(minx, maxx);
  if (miny > maxy) std::swap(miny, maxy);
  if (minz > maxz) std::swap(minz, maxz);

  out_min[0] = minx + e.transform.position[0];
  out_min[1] = miny + e.transform.position[1];
  out_min[2] = minz + e.transform.position[2];
  out_max[0] = maxx + e.transform.position[0];
  out_max[1] = maxy + e.transform.position[1];
  out_max[2] = maxz + e.transform.position[2];
}

static bool TransformDifferent(const EditorTransform& a, const EditorTransform& b)
{
  auto diff = [](float x, float y) { return std::fabs(x - y) > 1e-4f; };
  for (int i = 0; i < 3; ++i)
  {
    if (diff(a.position[i], b.position[i]) ||
        diff(a.rotation[i], b.rotation[i]) ||
        diff(a.scale[i], b.scale[i]))
      return true;
  }
  return false;
}

static void FocusCameraOnEntity(EditorCamera* cam, const EditorEntity& e)
{
  if (!cam)
    return;
  float bmin[3]{};
  float bmax[3]{};
  GetEntityWorldAabb(e, bmin, bmax);
  float center[3] = {
    (bmin[0] + bmax[0]) * 0.5f,
    (bmin[1] + bmax[1]) * 0.5f,
    (bmin[2] + bmax[2]) * 0.5f
  };
  float ext[3] = {
    (bmax[0] - bmin[0]) * 0.5f,
    (bmax[1] - bmin[1]) * 0.5f,
    (bmax[2] - bmin[2]) * 0.5f
  };
  float radius = std::sqrt(ext[0] * ext[0] + ext[1] * ext[1] + ext[2] * ext[2]);
  radius = std::max(radius, 0.25f);

  float forward[3]{};
  sc::editor::CameraForward(cam, forward);
  const float dist = radius * 2.5f + 2.0f;
  cam->position[0] = center[0] - forward[0] * dist;
  cam->position[1] = center[1] - forward[1] * dist;
  cam->position[2] = center[2] - forward[2] * dist;
}

static void drawHierarchyPanel(EditorDocument* doc,
                               const EditorCamera* camera,
                               const std::string& world_root,
                               ScRenderContext* render_ctx,
                               const EditorAssetRegistry& registry)
{
  if (!doc || !camera)
    return;

  const int32_t cam_sector_x = static_cast<int32_t>(std::floor(camera->position[0] / doc->sectorSize));
  const int32_t cam_sector_z = static_cast<int32_t>(std::floor(camera->position[2] / doc->sectorSize));

  ImGui::Begin("Hierarchy");
  ImGui::Text("Camera Sector: %d, %d", cam_sector_x, cam_sector_z);
  ImGui::Checkbox("Snap To Grid", &doc->snapToGrid);
  ImGui::DragFloat("Grid Size", &doc->gridSize, 0.1f, 0.1f, 10.0f);
  if (ImGui::Button("Save Sector"))
  {
    doc->sector.x = cam_sector_x;
    doc->sector.z = cam_sector_z;
    sc_world::SectorFile file{};
    sc::editor::SectorFileFromDocument(doc, &file);

    std::filesystem::create_directories(std::filesystem::path(world_root) / "sectors");
    const std::string path = sc_world::BuildSectorPath(world_root.c_str(), doc->sector);
    sc_world::WriteSectorFile(path.c_str(), file);

    sc_world::WorldManifest manifest{};
    const std::string manifest_path = sc_world::BuildWorldManifestPath(world_root.c_str());
    sc_world::ReadWorldManifest(manifest_path.c_str(), &manifest);
    bool exists = false;
    for (const auto& s : manifest.sectors)
    {
      if (s.x == doc->sector.x && s.z == doc->sector.z)
      {
        exists = true;
        break;
      }
    }
    if (!exists)
      manifest.sectors.push_back(doc->sector);
    sc_world::WriteWorldManifest(manifest_path.c_str(), manifest);
  }
  if (ImGui::Button("Load Sector"))
  {
    doc->sector.x = cam_sector_x;
    doc->sector.z = cam_sector_z;
    const std::string path = sc_world::BuildSectorPath(world_root.c_str(), doc->sector);
    sc_world::SectorFile file{};
    if (sc_world::ReadSectorFile(path.c_str(), &file))
      sc::editor::DocumentFromSectorFile(doc, file, render_ctx, registry);
  }

  ImGui::Separator();
  ImGui::TextUnformatted("Entities");
  ImGui::Separator();
  ImGui::BeginChild("HierarchyList", ImVec2(0.0f, 0.0f), true);
  for (const auto& e : doc->entities)
  {
    char label[128]{};
    FormatEntityLabel(e, label, sizeof(label));
    ImGui::PushID(reinterpret_cast<void*>(static_cast<uintptr_t>(e.id)));
    const bool selected = (doc->selectedId == e.id);
    if (ImGui::Selectable(label, selected))
      sc::editor::SetSelected(doc, e.id);
    ImGui::PopID();
  }
  ImGui::EndChild();
  ImGui::End();
}

static ViewportPanelState drawViewportPanel(float target_aspect)
{
  ViewportPanelState state{};
  ImGui::Begin("Viewport", nullptr,
    ImGuiWindowFlags_NoScrollbar |
    ImGuiWindowFlags_NoScrollWithMouse |
    ImGuiWindowFlags_NoBackground);
  state.pos = ImGui::GetCursorScreenPos();
  state.size = ImGui::GetContentRegionAvail();
  if (state.size.x < 1.0f) state.size.x = 1.0f;
  if (state.size.y < 1.0f) state.size.y = 1.0f;
  ImGui::InvisibleButton("viewport_capture", state.size);

  const ImVec2 panel_min = state.pos;
  const ImVec2 panel_max = ImVec2(state.pos.x + state.size.x, state.pos.y + state.size.y);
  const float panel_aspect = state.size.x / state.size.y;
  state.render_size = state.size;
  if (panel_aspect > target_aspect)
  {
    state.render_size.x = state.size.y * target_aspect;
  }
  else
  {
    state.render_size.y = state.size.x / target_aspect;
  }
  if (state.render_size.x < 1.0f) state.render_size.x = 1.0f;
  if (state.render_size.y < 1.0f) state.render_size.y = 1.0f;
  state.render_pos = ImVec2(
    state.pos.x + (state.size.x - state.render_size.x) * 0.5f,
    state.pos.y + (state.size.y - state.render_size.y) * 0.5f);
  const ImVec2 render_max = ImVec2(state.render_pos.x + state.render_size.x,
                                   state.render_pos.y + state.render_size.y);

  ImDrawList* draw_list = ImGui::GetWindowDrawList();
  const ImU32 bar_color = ImGui::GetColorU32(ImVec4(0.02f, 0.02f, 0.05f, 1.0f));
  if (state.render_pos.y > panel_min.y)
    draw_list->AddRectFilled(panel_min, ImVec2(panel_max.x, state.render_pos.y), bar_color);
  if (render_max.y < panel_max.y)
    draw_list->AddRectFilled(ImVec2(panel_min.x, render_max.y), panel_max, bar_color);
  if (state.render_pos.x > panel_min.x)
    draw_list->AddRectFilled(panel_min, ImVec2(state.render_pos.x, panel_max.y), bar_color);
  if (render_max.x < panel_max.x)
    draw_list->AddRectFilled(ImVec2(render_max.x, panel_min.y), panel_max, bar_color);

  const bool panel_hovered = ImGui::IsItemHovered();
  const bool render_hovered = panel_hovered &&
                              ImGui::IsMouseHoveringRect(state.render_pos, render_max, true);
  state.hovered = render_hovered;
  state.clicked = render_hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
  ImGui::End();
  return state;
}

static uint64_t pickEntityFromMouse(const EditorDocument* doc,
                                    const EditorCamera* cam,
                                    float mouse_x,
                                    float mouse_y,
                                    const ViewportPanelState& viewport)
{
  if (!doc || !cam)
    return 0;
  if (!viewport.hovered)
    return 0;
  sc::editor::Ray ray = sc::editor::computePickRay(cam,
                                                   mouse_x,
                                                   mouse_y,
                                                   viewport.render_pos.x,
                                                   viewport.render_pos.y,
                                                   viewport.render_size.x,
                                                   viewport.render_size.y);
  return sc::editor::PickEntity(doc, ray);
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
  ImGuiIO& io = ImGui::GetIO();
  io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
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

  std::vector<ScRenderDrawItem> draw_items;
  std::vector<ScRenderLine> debug_lines;
  draw_items.reserve(256);
  debug_lines.reserve(512);
  uint64_t last_logged_selection = 0;

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

    ImGuiViewport* main_viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(main_viewport->Pos);
    ImGui::SetNextWindowSize(main_viewport->Size);
    ImGui::SetNextWindowViewport(main_viewport->ID);
    ImGuiWindowFlags host_window_flags =
      ImGuiWindowFlags_NoDocking |
      ImGuiWindowFlags_NoTitleBar |
      ImGuiWindowFlags_NoCollapse |
      ImGuiWindowFlags_NoResize |
      ImGuiWindowFlags_NoMove |
      ImGuiWindowFlags_NoBringToFrontOnFocus |
      ImGuiWindowFlags_NoNavFocus |
      ImGuiWindowFlags_NoBackground |
      ImGuiWindowFlags_NoScrollbar |
      ImGuiWindowFlags_NoScrollWithMouse;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::Begin("DockSpaceHost", nullptr, host_window_flags);
    ImGui::PopStyleVar(3);

    ImGuiDockNodeFlags dockspace_flags = ImGuiDockNodeFlags_PassthruCentralNode;
    ImGuiID dockspace_id = ImGui::GetID("DockSpace");
    ImGui::DockSpace(dockspace_id, ImVec2(0.0f, 0.0f), dockspace_flags);

    static bool dockspace_built = false;
    if (!dockspace_built)
    {
      ImGui::DockBuilderRemoveNode(dockspace_id);
      ImGui::DockBuilderAddNode(dockspace_id, dockspace_flags | ImGuiDockNodeFlags_DockSpace);
      ImGui::DockBuilderSetNodeSize(dockspace_id, main_viewport->Size);

      ImGuiID dock_main_id = dockspace_id;
      const float left_ratio = 0.20f;
      const float right_ratio = 0.25f;
      const float bottom_ratio = 0.25f;
      const float top_ratio = 0.08f;
      const float remaining_after_sides = 1.0f - left_ratio - right_ratio;
      const float remaining_after_bottom = remaining_after_sides - bottom_ratio;

      ImGuiID dock_id_left = ImGui::DockBuilderSplitNode(
        dock_main_id, ImGuiDir_Left, left_ratio, nullptr, &dock_main_id);
      ImGuiID dock_id_right = ImGui::DockBuilderSplitNode(
        dock_main_id, ImGuiDir_Right, right_ratio / (1.0f - left_ratio), nullptr, &dock_main_id);
      ImGuiID dock_id_down = ImGui::DockBuilderSplitNode(
        dock_main_id, ImGuiDir_Down, bottom_ratio / remaining_after_sides, nullptr, &dock_main_id);
      ImGuiID dock_id_up = ImGui::DockBuilderSplitNode(
        dock_main_id, ImGuiDir_Up, top_ratio / remaining_after_bottom, nullptr, &dock_main_id);

      ImGui::DockBuilderDockWindow("Hierarchy", dock_id_left);
      ImGui::DockBuilderDockWindow("Inspector", dock_id_right);
      ImGui::DockBuilderDockWindow("Palette", dock_id_down);
      ImGui::DockBuilderDockWindow("Toolbar", dock_id_up);
      ImGui::DockBuilderDockWindow("Viewport", dock_main_id);
      ImGui::DockBuilderFinish(dockspace_id);
      dockspace_built = true;
    }
    ImGui::End();

    sc::editor::ValidateSelection(&doc);

    ImGui::Begin("Toolbar");
    ImGui::TextUnformatted("Toolbar (placeholder)");
    ImGui::End();

    drawHierarchyPanel(&doc, &camera, world_root, render_ctx, registry);

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

    static uint64_t name_buffer_id = 0;
    static uint64_t name_edit_id = 0;
    static char name_buf[64] = {};
    static char name_before[64] = {};
    static bool name_editing = false;

    static uint64_t transform_edit_id = 0;
    static EditorTransform transform_before{};
    static bool transform_editing = false;

    static bool asset_cache_ready = false;
    static std::vector<const sc_world::AssetRegistryEntry*> asset_entries;
    static std::vector<const char*> asset_labels;

    auto commit_name = [&](uint64_t entity_id)
    {
      if (entity_id == 0)
        return;
      if (std::strcmp(name_before, name_buf) == 0)
        return;
      if (!sc::editor::FindEntity(&doc, entity_id))
        return;
      auto cmd = std::make_unique<CmdSetProperty>();
      cmd->type = CmdSetProperty::Name;
      cmd->entityId = entity_id;
      std::snprintf(cmd->oldName, sizeof(cmd->oldName), "%s", name_before);
      std::snprintf(cmd->newName, sizeof(cmd->newName), "%s", name_buf);
      cmd_stack.execute(std::move(cmd), &doc);
    };

    auto commit_transform = [&](uint64_t entity_id)
    {
      if (entity_id == 0)
        return;
      EditorEntity* e = sc::editor::FindEntity(&doc, entity_id);
      if (!e)
        return;
      if (!TransformDifferent(transform_before, e->transform))
        return;
      auto cmd = std::make_unique<CmdTransformEntity>();
      cmd->entityId = entity_id;
      cmd->before = transform_before;
      cmd->after = e->transform;
      cmd_stack.execute(std::move(cmd), &doc);
    };

    EditorEntity* selected = (doc.selectedId != 0) ? sc::editor::FindEntity(&doc, doc.selectedId) : nullptr;

    if (name_editing && (!selected || selected->id != name_edit_id))
    {
      commit_name(name_edit_id);
      name_editing = false;
    }

    if (transform_editing && (!selected || selected->id != transform_edit_id))
    {
      commit_transform(transform_edit_id);
      transform_editing = false;
    }

    if (!selected)
    {
      ImGui::TextUnformatted("No selection");
    }
    else
    {
      if (ImGui::CollapsingHeader("General", ImGuiTreeNodeFlags_DefaultOpen))
      {
        ImGui::Text("ID: %llu", (unsigned long long)selected->id);

        if (!name_editing)
        {
          if (name_buffer_id != selected->id)
          {
            name_buffer_id = selected->id;
            std::snprintf(name_buf, sizeof(name_buf), "%s", selected->name);
          }
          else if (std::strcmp(name_buf, selected->name) != 0)
          {
            std::snprintf(name_buf, sizeof(name_buf), "%s", selected->name);
          }
        }

        ImGui::InputText("Name", name_buf, sizeof(name_buf));
        const bool name_active = ImGui::IsItemActive();
        if (name_active && !name_editing)
        {
          name_editing = true;
          name_edit_id = name_buffer_id;
          if (EditorEntity* e = sc::editor::FindEntity(&doc, name_edit_id))
            std::snprintf(name_before, sizeof(name_before), "%s", e->name);
          else
            name_before[0] = '\0';
        }

        if (name_editing)
        {
          if (EditorEntity* e = sc::editor::FindEntity(&doc, name_edit_id))
          {
            if (std::strcmp(e->name, name_buf) != 0)
              std::snprintf(e->name, sizeof(e->name), "%s", name_buf);
          }
        }

        if (!name_active && name_editing)
        {
          commit_name(name_edit_id);
          name_editing = false;
          name_buffer_id = name_edit_id;
        }
      }

      if (ImGui::CollapsingHeader("Transform", ImGuiTreeNodeFlags_DefaultOpen))
      {
        EditorTransform pre = selected->transform;
        ImGui::DragFloat3("Position", selected->transform.position, 0.1f);
        if (ImGui::IsItemActivated())
        {
          transform_editing = true;
          transform_edit_id = selected->id;
          transform_before = pre;
        }
        if (ImGui::IsItemDeactivatedAfterEdit())
        {
          commit_transform(selected->id);
          transform_editing = false;
        }

        pre = selected->transform;
        float rot_deg[3] = {
          RadToDeg(pre.rotation[0]),
          RadToDeg(pre.rotation[1]),
          RadToDeg(pre.rotation[2])
        };
        if (ImGui::DragFloat3("Rotation (deg)", rot_deg, 1.0f))
        {
          selected->transform.rotation[0] = DegToRad(rot_deg[0]);
          selected->transform.rotation[1] = DegToRad(rot_deg[1]);
          selected->transform.rotation[2] = DegToRad(rot_deg[2]);
        }
        if (ImGui::IsItemActivated())
        {
          transform_editing = true;
          transform_edit_id = selected->id;
          transform_before = pre;
        }
        if (ImGui::IsItemDeactivatedAfterEdit())
        {
          commit_transform(selected->id);
          transform_editing = false;
        }

        pre = selected->transform;
        ImGui::DragFloat3("Scale", selected->transform.scale, 0.05f);
        if (ImGui::IsItemActivated())
        {
          transform_editing = true;
          transform_edit_id = selected->id;
          transform_before = pre;
        }
        if (ImGui::IsItemDeactivatedAfterEdit())
        {
          commit_transform(selected->id);
          transform_editing = false;
        }
      }

      if (ImGui::CollapsingHeader("Render", ImGuiTreeNodeFlags_DefaultOpen))
      {
        const sc_world::AssetRegistryEntry* current =
          registry.findByIds(selected->meshAssetId, selected->materialAssetId);
        ImGui::Text("Mesh: %s", current ? current->mesh_path.c_str() : "<missing>");
        ImGui::Text("Material: %s", current ? current->material_path.c_str() : "<missing>");

        if (!asset_cache_ready || asset_entries.size() != registry.entries.size())
        {
          asset_entries.clear();
          asset_labels.clear();
          asset_entries.reserve(registry.entries.size());
          asset_labels.reserve(registry.entries.size());
          for (const auto& entry : registry.entries)
          {
            asset_entries.push_back(&entry);
            asset_labels.push_back(entry.label.c_str());
          }
          asset_cache_ready = true;
        }

        if (!asset_entries.empty())
        {
          const char* preview = current ? current->label.c_str() : "<missing>";
          if (ImGui::BeginCombo("Asset", preview))
          {
            for (size_t i = 0; i < asset_entries.size(); ++i)
            {
              const bool is_selected = (current == asset_entries[i]);
              if (ImGui::Selectable(asset_labels[i], is_selected))
              {
                const auto* entry = asset_entries[i];
                if (entry && (selected->meshAssetId != entry->mesh_id ||
                              selected->materialAssetId != entry->material_id))
                {
                  selected->meshAssetId = entry->mesh_id;
                  selected->materialAssetId = entry->material_id;
                  sc::editor::ResolveEntityAssets(selected, render_ctx, registry);
                }
              }
              if (is_selected)
                ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
          }
        }
        else
        {
          ImGui::TextDisabled("No render assets loaded.");
        }
      }

      if (ImGui::CollapsingHeader("Actions", ImGuiTreeNodeFlags_DefaultOpen))
      {
        if (ImGui::Button("Delete"))
        {
          EditorEntity removed{};
          if (sc::editor::RemoveEntity(&doc, selected->id, &removed))
          {
            auto cmd = std::make_unique<CmdDeleteEntity>();
            cmd->entity = removed;
            cmd_stack.execute(std::move(cmd), &doc);
          }
        }
        ImGui::SameLine();
        if (ImGui::Button("Duplicate"))
        {
          EditorEntity copy = *selected;
          copy.id = doc.nextId++;
          const float offset = (doc.snapToGrid && doc.gridSize > 0.0f) ? doc.gridSize : 1.0f;
          copy.transform.position[0] += offset;
          copy.transform.position[2] += offset;
          if (doc.snapToGrid)
            sc::editor::SnapTransform(&copy.transform, doc.gridSize);
          if (copy.name[0] != '\0')
            std::snprintf(copy.name, sizeof(copy.name), "%s Copy", selected->name);
          else
            std::snprintf(copy.name, sizeof(copy.name), "Entity %llu Copy", (unsigned long long)selected->id);

          auto cmd = std::make_unique<CmdPlaceEntity>();
          cmd->entity = copy;
          cmd_stack.execute(std::move(cmd), &doc);
        }
        ImGui::SameLine();
        if (ImGui::Button("Focus"))
        {
          FocusCameraOnEntity(&camera, *selected);
        }
      }
    }
    ImGui::End();

    ViewportPanelState viewport = drawViewportPanel(kEditorViewportAspect);

    const bool ctrl = ImGui::GetIO().KeyCtrl;
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Z))
      cmd_stack.undoLast(&doc);
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Y))
      cmd_stack.redoLast(&doc);

    if (viewport.clicked && viewport.hovered)
    {
      uint64_t picked = pickEntityFromMouse(&doc, &camera,
                                            static_cast<float>(mx),
                                            static_cast<float>(my),
                                            viewport);
      sc::editor::SetSelected(&doc, picked);
    }

    if (doc.selectedId != 0 && viewport.hovered)
    {
      EditorEntity* sel = sc::editor::FindEntity(&doc, doc.selectedId);
      if (sel)
      {
        const bool was_active = gizmo.active;
        bool dragging = (mouse_buttons & SDL_BUTTON(SDL_BUTTON_LEFT)) != 0;
        sc::editor::GizmoTranslate(&gizmo, sel, &camera,
          static_cast<float>(mx), static_cast<float>(my),
          viewport.render_pos.x, viewport.render_pos.y,
          viewport.render_size.x, viewport.render_size.y,
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

    if (doc.selectedId != last_logged_selection)
    {
      if (doc.selectedId != 0)
        std::printf("Selected entity %llu\n", (unsigned long long)doc.selectedId);
      else if (last_logged_selection != 0)
        std::printf("Selection cleared\n");
      last_logged_selection = doc.selectedId;
    }

    sc::editor::BuildDrawItems(&doc, &draw_items);
    sc::editor::BuildDebugLines(&doc, &debug_lines);

    const float vp_w = std::max(1.0f, viewport.render_size.x);
    const float vp_h = std::max(1.0f, viewport.render_size.y);
    const sc::Mat4 view = sc::editor::CameraView(&camera);
    const sc::Mat4 proj = sc::editor::CameraProj(&camera, vp_w / vp_h);

    ScRenderFrameDesc frame{};
    std::memcpy(frame.view, view.m, sizeof(frame.view));
    std::memcpy(frame.proj, proj.m, sizeof(frame.proj));
    frame.camera_pos[0] = camera.position[0];
    frame.camera_pos[1] = camera.position[1];
    frame.camera_pos[2] = camera.position[2];
    frame.time_sec = static_cast<float>(SDL_GetTicks()) / 1000.0f;
    const ImVec2 fb_scale = ImGui::GetIO().DisplayFramebufferScale;
    auto to_u32 = [](float v)
    {
      const float clamped = (v < 0.0f) ? 0.0f : v;
      return static_cast<uint32_t>(std::lround(clamped));
    };
    frame.viewport_x = to_u32(viewport.render_pos.x * fb_scale.x);
    frame.viewport_y = to_u32(viewport.render_pos.y * fb_scale.y);
    frame.viewport_w = std::max(1u, to_u32(viewport.render_size.x * fb_scale.x));
    frame.viewport_h = std::max(1u, to_u32(viewport.render_size.y * fb_scale.y));

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
