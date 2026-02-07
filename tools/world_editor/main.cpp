#include <SDL.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <backends/imgui_impl_sdl2.h>
#include "ImGuizmo.h"

#include "sc_engine_render.h"
#include "editor_core/editor_core.h"
#include "editor_core/sc_asset_db.h"
#include "world_format.h"
#include "sc_paths.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
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
using sc::editor::AssetDatabase;
using sc::editor::AssetEntry;
using sc::editor::AssetFolder;
using sc::editor::AssetStatus;
using sc::editor::AssetType;
using sc::editor::requestLoadModelGLB;

static constexpr float kEditorViewportAspect = 16.0f / 9.0f;

static std::filesystem::path ResolveAssetRootPath()
{
  return sc::assetsRoot();
}

static bool TryGetEnvPath(const char* name, std::filesystem::path* out_path)
{
  if (!name || !out_path)
    return false;
#if defined(_WIN32)
  char* value = nullptr;
  size_t len = 0;
  if (_dupenv_s(&value, &len, name) != 0 || !value)
    return false;
  if (len <= 1)
  {
    free(value);
    return false;
  }
  *out_path = std::filesystem::path(value);
  free(value);
  return true;
#else
  if (const char* env = std::getenv(name))
  {
    if (*env != '\0')
    {
      *out_path = std::filesystem::path(env);
      return true;
    }
  }
  return false;
#endif
}

static std::filesystem::path ResolveWorldRootPath(const std::filesystem::path& asset_root)
{
  std::filesystem::path env_path;
  if (TryGetEnvPath("SC_WORLD_ROOT", &env_path))
    return env_path;
  return asset_root / "world";
}

struct ViewportPanelState
{
  ImVec2 pos{};
  ImVec2 size{};
  ImVec2 render_pos{};
  ImVec2 render_size{};
  ImVec2 render_max{};
  ImDrawList* draw_list = nullptr;
  bool hovered = false;
  bool clicked = false;
};

struct ProjectPanelState
{
  std::string selectedFolder;
  int filterIndex = 0;
  char search[128] = {};
};

struct AssetSelection
{
  sc_world::AssetId id = 0;
  std::string relPath;
  std::string absPath;
};

static std::string ToLower(std::string text)
{
  std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c)
  {
    return static_cast<char>(std::tolower(c));
  });
  return text;
}

static void FormatAssetTime(uint64_t unix_seconds, char* out, size_t out_size)
{
  if (!out || out_size == 0)
    return;
  if (unix_seconds == 0)
  {
    std::snprintf(out, out_size, "<unknown>");
    return;
  }

  std::time_t t = static_cast<std::time_t>(unix_seconds);
  std::tm tm{};
#if defined(_WIN32)
  localtime_s(&tm, &t);
#else
  localtime_r(&t, &tm);
#endif
  std::snprintf(out, out_size, "%04d-%02d-%02d %02d:%02d",
                tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                tm.tm_hour, tm.tm_min);
}

static bool AssetInFolder(const AssetEntry& entry, const std::string& folder_rel)
{
  std::filesystem::path rel(entry.relPath);
  std::string parent = rel.parent_path().generic_string();
  if (folder_rel.empty())
    return parent.empty();
  return parent == folder_rel;
}

static bool AssetPassesFilter(AssetType type, int filter_index)
{
  switch (filter_index)
  {
    case 1: return type == AssetType::Model;
    case 2: return type == AssetType::Texture;
    case 3: return type == AssetType::Shader;
    case 4: return type == AssetType::World;
    case 0:
    default:
      return true;
  }
}

static bool AssetHasExtension(const std::string& path, const char* ext)
{
  if (!ext)
    return false;
  std::filesystem::path p(path);
  std::string e = ToLower(p.extension().string());
  return e == ext;
}

static void DrawFolderTreeNode(const AssetDatabase& db, int index, std::string* selected_folder)
{
  const auto& folders = db.getFolders();
  if (index < 0 || index >= static_cast<int>(folders.size()))
    return;

  const AssetFolder& folder = folders[index];
  ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanFullWidth;
  if (folder.children.empty())
    flags |= ImGuiTreeNodeFlags_Leaf;
  if (index == 0)
    flags |= ImGuiTreeNodeFlags_DefaultOpen;
  if (selected_folder && folder.relPath == *selected_folder)
    flags |= ImGuiTreeNodeFlags_Selected;

  ImGui::PushID(folder.relPath.c_str());
  const bool open = ImGui::TreeNodeEx(folder.name.c_str(), flags);
  if (ImGui::IsItemClicked() && selected_folder)
    *selected_folder = folder.relPath;
  if (open)
  {
    for (int child : folder.children)
      DrawFolderTreeNode(db, child, selected_folder);
    ImGui::TreePop();
  }
  ImGui::PopID();
}

static void DrawProjectPanel(AssetDatabase* db, ProjectPanelState* state, AssetSelection* selection)
{
  if (!db || !state)
    return;

  ImGui::Begin("Project");

  if (db->findFolderIndex(state->selectedFolder) < 0)
    state->selectedFolder.clear();

  const std::filesystem::path& root = db->root();
  if (!db->hasValidRoot())
  {
    ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f),
                       "Asset root missing: %s",
                       root.string().c_str());
  }

  const char* filter_labels[] = { "All", "Models", "Textures", "Shaders", "World" };
  ImGui::SetNextItemWidth(140.0f);
  ImGui::Combo("##asset_filter", &state->filterIndex, filter_labels, IM_ARRAYSIZE(filter_labels));
  ImGui::SameLine();
  ImGui::SetNextItemWidth(220.0f);
  ImGui::InputTextWithHint("##asset_search", "Search...", state->search, sizeof(state->search));
  ImGui::SameLine();
  if (ImGui::Button("Rescan"))
    db->scanAll();

  ImGui::Separator();

  if (ImGui::BeginTable("ProjectSplit", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV))
  {
    ImGui::TableSetupColumn("Folders", ImGuiTableColumnFlags_WidthFixed, 220.0f);
    ImGui::TableSetupColumn("Assets", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::BeginChild("ProjectFolders", ImVec2(0.0f, 0.0f), true);
    const auto& folders = db->getFolders();
    if (!folders.empty())
      DrawFolderTreeNode(*db, 0, &state->selectedFolder);
    else
      ImGui::TextDisabled("No folders indexed.");
    ImGui::EndChild();

    ImGui::TableSetColumnIndex(1);
    ImGui::BeginChild("ProjectFiles", ImVec2(0.0f, 0.0f), true);

    std::vector<const AssetEntry*> filtered;
    filtered.reserve(db->getAll().size());

    const std::string search_text = ToLower(state->search);
    for (const auto& entry : db->getAll())
    {
      if (!AssetInFolder(entry, state->selectedFolder))
        continue;
      if (!AssetPassesFilter(entry.type, state->filterIndex))
        continue;
      if (!search_text.empty())
      {
        const std::string hay = ToLower(entry.relPath);
        if (hay.find(search_text) == std::string::npos)
          continue;
      }
      filtered.push_back(&entry);
    }

    std::sort(filtered.begin(), filtered.end(), [](const AssetEntry* a, const AssetEntry* b)
    {
      const std::string an = ToLower(std::filesystem::path(a->relPath).filename().string());
      const std::string bn = ToLower(std::filesystem::path(b->relPath).filename().string());
      return an < bn;
    });

    ImGuiTableFlags table_flags = ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_ScrollY |
                                  ImGuiTableFlags_Resizable |
                                  ImGuiTableFlags_BordersInnerV;
    if (ImGui::BeginTable("ProjectAssetsTable", 4, table_flags))
    {
      ImGui::TableSetupScrollFreeze(0, 1);
      ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
      ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 90.0f);
      ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 80.0f);
      ImGui::TableSetupColumn("Modified", ImGuiTableColumnFlags_WidthFixed, 130.0f);
      ImGui::TableHeadersRow();

      if (filtered.empty())
      {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextDisabled("No assets in folder.");
      }
      else
      {
        for (const AssetEntry* entry : filtered)
        {
          const std::string name = std::filesystem::path(entry->relPath).filename().string();
          const bool is_selected = selection && selection->id == entry->id;

          ImGui::TableNextRow();
          ImGui::TableSetColumnIndex(0);
          ImGui::PushID(entry->relPath.c_str());
          if (ImGui::Selectable(name.c_str(), is_selected,
                                ImGuiSelectableFlags_SpanAllColumns |
                                ImGuiSelectableFlags_AllowDoubleClick))
          {
            if (selection)
            {
              selection->id = entry->id;
              selection->relPath = entry->relPath;
              selection->absPath = entry->absPath;
            }
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
            {
              if (entry->type == AssetType::Model && AssetHasExtension(entry->relPath, ".glb"))
                requestLoadModelGLB(entry->id);
            }
          }

          ImGui::TableSetColumnIndex(1);
          ImGui::TextUnformatted(AssetTypeLabel(entry->type));

          ImGui::TableSetColumnIndex(2);
          const uint64_t kb = (entry->fileSize + 1023u) / 1024u;
          ImGui::Text("%llu KB", static_cast<unsigned long long>(kb));

          ImGui::TableSetColumnIndex(3);
          char time_buf[32]{};
          FormatAssetTime(entry->lastWriteTime, time_buf, sizeof(time_buf));
          ImGui::TextUnformatted(time_buf);

          ImGui::PopID();
        }
      }

      ImGui::EndTable();
    }

    ImGui::EndChild();
    ImGui::EndTable();
  }

  ImGui::End();
}

static void DrawAssetInspector(const AssetDatabase& db, const AssetSelection& selection)
{
  if (ImGui::CollapsingHeader("Asset Inspector", ImGuiTreeNodeFlags_DefaultOpen))
  {
    if (selection.id == 0)
    {
      ImGui::TextUnformatted("No asset selected.");
      return;
    }

    const AssetEntry* entry = db.findById(selection.id);
    if (!entry)
    {
      ImGui::TextUnformatted("Selected asset not found in database.");
      ImGui::Text("AssetId: 0x%016llX", static_cast<unsigned long long>(selection.id));
      if (!selection.relPath.empty())
        ImGui::Text("Path: %s", selection.relPath.c_str());
      return;
    }

    ImGui::Text("Type: %s", AssetTypeLabel(entry->type));
    ImGui::Text("Path: %s", entry->relPath.c_str());
    ImGui::Text("AssetId: 0x%016llX", static_cast<unsigned long long>(entry->id));
    ImGui::Text("Size: %llu KB", static_cast<unsigned long long>((entry->fileSize + 1023u) / 1024u));
    char time_buf[32]{};
    FormatAssetTime(entry->lastWriteTime, time_buf, sizeof(time_buf));
    ImGui::Text("Modified: %s", time_buf);
    ImGui::Text("Status: %s", AssetStatusLabel(entry->status));
  }
}

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

static bool IsFiniteFloat(float v)
{
  return std::isfinite(v);
}

static bool IsFiniteVec3(const float v[3])
{
  return IsFiniteFloat(v[0]) && IsFiniteFloat(v[1]) && IsFiniteFloat(v[2]);
}

enum class GizmoMode
{
  None,
  Translate,
  Rotate,
  Scale
};

enum class GizmoSpace
{
  Local,
  World
};

struct GizmoSettings
{
  bool show = true;
  GizmoMode mode = GizmoMode::Translate;
  GizmoSpace space = GizmoSpace::Local;
  bool snapTranslate = false;
  bool snapRotate = false;
  bool snapScale = false;
  float translateSnap = 0.5f;
  float rotateSnap = 15.0f;
  float scaleSnap = 0.1f;
};

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

static const char* GizmoModeLabel(GizmoMode mode)
{
  switch (mode)
  {
    case GizmoMode::Translate: return "Translate";
    case GizmoMode::Rotate: return "Rotate";
    case GizmoMode::Scale: return "Scale";
    case GizmoMode::None: default: return "None";
  }
}

static const char* GizmoSpaceLabel(GizmoSpace space)
{
  return (space == GizmoSpace::World) ? "World" : "Local";
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
  state.render_max = render_max;
  state.draw_list = ImGui::GetWindowDrawList();

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
  const std::filesystem::path asset_root_path = ResolveAssetRootPath();
  const std::string asset_root = asset_root_path.string();
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
  const std::filesystem::path world_root_path = ResolveWorldRootPath(asset_root_path);
  std::filesystem::path registry_path = world_root_path;
  registry_path /= "asset_registry.txt";
  registry.loadFromFile(registry_path.string().c_str());

  AssetDatabase asset_db;
  asset_db.setRoot(asset_root_path);
  asset_db.scanAll();
  ProjectPanelState project_state;
  project_state.selectedFolder = "";
  AssetSelection asset_selection;
  uint64_t last_asset_scan_ms = static_cast<uint64_t>(SDL_GetTicks());
  const uint64_t asset_scan_interval_ms = 5000;

  EditorCamera camera;
  CommandStack cmd_stack;
  GizmoState gizmo;
  GizmoSettings gizmo_settings;

  const int32_t initial_sector_x = static_cast<int32_t>(std::floor(camera.position[0] / doc.sectorSize));
  const int32_t initial_sector_z = static_cast<int32_t>(std::floor(camera.position[2] / doc.sectorSize));
  doc.sector.x = initial_sector_x;
  doc.sector.z = initial_sector_z;

  const std::string world_root = world_root_path.string();
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

  std::vector<ScRenderDrawItem> draw_items;
  std::vector<ScRenderLine> debug_lines;
  draw_items.reserve(256);
  debug_lines.reserve(512);
  uint64_t last_logged_selection = 0;

  while (running)
  {
    SDL_Event e;
    while (SDL_PollEvent(&e))
    {
      ImGui_ImplSDL2_ProcessEvent(&e);
      if (e.type == SDL_QUIT)
        running = false;
      if (e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED)
        scRenderResize(render_ctx, static_cast<uint32_t>(e.window.data1), static_cast<uint32_t>(e.window.data2));
    }

    uint64_t now = SDL_GetPerformanceCounter();
    float dt = static_cast<float>(static_cast<double>(now - last_counter) / static_cast<double>(perf_freq));
    last_counter = now;

    const uint64_t scan_now = static_cast<uint64_t>(SDL_GetTicks());
    if (scan_now - last_asset_scan_ms >= asset_scan_interval_ms)
    {
      asset_db.scanIncremental();
      last_asset_scan_ms = scan_now;
    }

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
    if (right_down)
    {
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
    }

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
      ImGui::DockBuilderDockWindow("Project", dock_id_down);
      ImGui::DockBuilderDockWindow("Palette", dock_id_down);
      ImGui::DockBuilderDockWindow("Toolbar", dock_id_up);
      ImGui::DockBuilderDockWindow("Viewport", dock_main_id);
      ImGui::DockBuilderFinish(dockspace_id);
      dockspace_built = true;
    }
    ImGui::End();

    sc::editor::ValidateSelection(&doc);

    ImGui::Begin("Toolbar");
    ImGui::TextUnformatted("Gizmos");
    ImGui::SameLine();
    ImGui::Checkbox("Show Gizmo", &gizmo_settings.show);
    ImGui::Separator();

    ImGui::TextUnformatted("Operation");
    ImGui::SameLine();
    if (ImGui::RadioButton("Translate (W)", gizmo_settings.mode == GizmoMode::Translate))
      gizmo_settings.mode = GizmoMode::Translate;
    ImGui::SameLine();
    if (ImGui::RadioButton("Rotate (E)", gizmo_settings.mode == GizmoMode::Rotate))
      gizmo_settings.mode = GizmoMode::Rotate;
    ImGui::SameLine();
    if (ImGui::RadioButton("Scale (R)", gizmo_settings.mode == GizmoMode::Scale))
      gizmo_settings.mode = GizmoMode::Scale;
    ImGui::SameLine();
    if (ImGui::RadioButton("None (Q)", gizmo_settings.mode == GizmoMode::None))
      gizmo_settings.mode = GizmoMode::None;

    ImGui::TextUnformatted("Space");
    ImGui::SameLine();
    if (ImGui::RadioButton("Local", gizmo_settings.space == GizmoSpace::Local))
      gizmo_settings.space = GizmoSpace::Local;
    ImGui::SameLine();
    if (ImGui::RadioButton("World", gizmo_settings.space == GizmoSpace::World))
      gizmo_settings.space = GizmoSpace::World;

    ImGui::Separator();
    ImGui::TextUnformatted("Snapping");
    ImGui::Checkbox("Translate", &gizmo_settings.snapTranslate);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(80.0f);
    ImGui::DragFloat("##snap_translate", &gizmo_settings.translateSnap, 0.05f, 0.001f, 100.0f, "%.2f");

    ImGui::Checkbox("Rotate", &gizmo_settings.snapRotate);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(80.0f);
    ImGui::DragFloat("##snap_rotate", &gizmo_settings.rotateSnap, 1.0f, 1.0f, 90.0f, "%.0f deg");

    ImGui::Checkbox("Scale", &gizmo_settings.snapScale);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(80.0f);
    ImGui::DragFloat("##snap_scale", &gizmo_settings.scaleSnap, 0.05f, 0.001f, 10.0f, "%.2f");

    ImGui::Separator();
    ImGui::Text("Mode: %s | Space: %s", GizmoModeLabel(gizmo_settings.mode),
                GizmoSpaceLabel(gizmo_settings.space));
    ImGui::End();

    drawHierarchyPanel(&doc, &camera, world_root, render_ctx, registry);

    DrawProjectPanel(&asset_db, &project_state, &asset_selection);

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

    DrawAssetInspector(asset_db, asset_selection);
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

    ImGuiIO& frame_io = ImGui::GetIO();
    if (!frame_io.WantTextInput)
    {
      if (ImGui::IsKeyPressed(ImGuiKey_Q))
        gizmo_settings.mode = GizmoMode::None;
      if (ImGui::IsKeyPressed(ImGuiKey_W))
        gizmo_settings.mode = GizmoMode::Translate;
      if (ImGui::IsKeyPressed(ImGuiKey_E))
        gizmo_settings.mode = GizmoMode::Rotate;
      if (ImGui::IsKeyPressed(ImGuiKey_R))
        gizmo_settings.mode = GizmoMode::Scale;
    }

    const float vp_w = std::max(1.0f, viewport.render_size.x);
    const float vp_h = std::max(1.0f, viewport.render_size.y);
    const sc::Mat4 view = sc::editor::CameraView(&camera);
    const sc::Mat4 proj = sc::editor::CameraProj(&camera, vp_w / vp_h);
    const sc::Mat4 gizmo_proj =
      sc::mat4_perspective_rh_zo(DegToRad(camera.fovDeg), vp_w / vp_h,
                                 camera.nearZ, camera.farZ, false);

    bool gizmo_over = false;
    bool gizmo_using = false;
    if (gizmo_settings.show && selected && gizmo_settings.mode != GizmoMode::None)
    {
      ImGuizmo::OPERATION op = ImGuizmo::TRANSLATE;
      if (gizmo_settings.mode == GizmoMode::Rotate)
        op = ImGuizmo::ROTATE;
      else if (gizmo_settings.mode == GizmoMode::Scale)
        op = ImGuizmo::SCALE;

      ImGuizmo::MODE mode = (gizmo_settings.space == GizmoSpace::World) ? ImGuizmo::WORLD : ImGuizmo::LOCAL;

      const EditorTransform pre_gizmo = selected->transform;
      const sc::Mat4 model_mat = sc::mat4_trs(selected->transform.position,
                                              selected->transform.rotation,
                                              selected->transform.scale);
      float model[16]{};
      std::memcpy(model, model_mat.m, sizeof(model));

      float snap_values[3]{};
      float snap_angle = 0.0f;
      const float* snap = nullptr;
      if (op == ImGuizmo::TRANSLATE && gizmo_settings.snapTranslate)
      {
        snap_values[0] = gizmo_settings.translateSnap;
        snap_values[1] = gizmo_settings.translateSnap;
        snap_values[2] = gizmo_settings.translateSnap;
        snap = snap_values;
      }
      else if (op == ImGuizmo::ROTATE && gizmo_settings.snapRotate)
      {
        snap_angle = gizmo_settings.rotateSnap;
        snap = &snap_angle;
      }
      else if (op == ImGuizmo::SCALE && gizmo_settings.snapScale)
      {
        snap_values[0] = gizmo_settings.scaleSnap;
        snap_values[1] = gizmo_settings.scaleSnap;
        snap_values[2] = gizmo_settings.scaleSnap;
        snap = snap_values;
      }

      ImGuizmo::SetDrawlist(viewport.draw_list);
      ImGuizmo::SetRect(viewport.render_pos.x, viewport.render_pos.y,
                        viewport.render_size.x, viewport.render_size.y);
      if (viewport.draw_list)
        viewport.draw_list->PushClipRect(viewport.render_pos, viewport.render_max, true);
      bool changed = ImGuizmo::Manipulate(view.m, gizmo_proj.m, op, mode, model, nullptr, snap);
      if (viewport.draw_list)
        viewport.draw_list->PopClipRect();

      gizmo_over = ImGuizmo::IsOver();
      gizmo_using = ImGuizmo::IsUsing();

      if (changed)
      {
        float t[3]{};
        float r[3]{};
        float s[3]{};
        ImGuizmo::DecomposeMatrixToComponents(model, t, r, s);

        const float min_scale = 0.001f;
        if (!IsFiniteVec3(t) || !IsFiniteVec3(r) || !IsFiniteVec3(s))
          changed = false;
        else
        {
          for (int i = 0; i < 3; ++i)
          {
            if (!IsFiniteFloat(s[i]))
              s[i] = 1.0f;
            if (std::fabs(s[i]) < min_scale)
              s[i] = (s[i] < 0.0f) ? -min_scale : min_scale;
          }
        }

        if (!changed)
        {
          // Ignore invalid decomposition results to avoid propagating NaNs.
        }
        else
        {
        EditorTransform updated = selected->transform;
        updated.position[0] = t[0];
        updated.position[1] = t[1];
        updated.position[2] = t[2];
        updated.rotation[0] = DegToRad(r[0]);
        updated.rotation[1] = DegToRad(r[1]);
        updated.rotation[2] = DegToRad(r[2]);
        updated.scale[0] = s[0];
        updated.scale[1] = s[1];
        updated.scale[2] = s[2];
        sc::editor::SetTransform(&doc, selected->id, updated);
        }
      }

      if (gizmo_using && !gizmo.active)
      {
        gizmo.active = true;
        gizmo.entityId = selected->id;
        gizmo.startTransform = pre_gizmo;
      }
      if (!gizmo_using && gizmo.active)
      {
        if (EditorEntity* e = sc::editor::FindEntity(&doc, gizmo.entityId))
        {
          if (TransformDifferent(gizmo.startTransform, e->transform))
          {
            auto cmd = std::make_unique<CmdTransformEntity>();
            cmd->entityId = gizmo.entityId;
            cmd->before = gizmo.startTransform;
            cmd->after = e->transform;
            cmd_stack.execute(std::move(cmd), &doc);
          }
        }
        gizmo.active = false;
        gizmo.entityId = 0;
      }
    }
    else if (gizmo.active)
    {
      gizmo.active = false;
      gizmo.entityId = 0;
    }

    const bool ctrl = ImGui::GetIO().KeyCtrl;
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Z))
      cmd_stack.undoLast(&doc);
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Y))
      cmd_stack.redoLast(&doc);

    if (viewport.clicked && viewport.hovered && !gizmo_over && !gizmo_using)
    {
      uint64_t picked = pickEntityFromMouse(&doc, &camera,
                                            static_cast<float>(mx),
                                            static_cast<float>(my),
                                            viewport);
      sc::editor::SetSelected(&doc, picked);
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
