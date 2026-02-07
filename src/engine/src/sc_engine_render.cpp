#include "sc_engine_render.h"

#include "sc_vk.h"
#include "sc_debug_draw.h"
#include "sc_assets.h"
#include "sc_ecs.h"
#include "sc_math.h"
#include "sc_paths.h"

#include <cstring>

namespace
{
  static const uint32_t kRenderApiVersion = 1;

  enum HandleType : uint8_t
  {
    HandleMesh = 1,
    HandleTexture = 2,
    HandleMaterial = 3
  };

  static ScRenderHandle make_handle(HandleType type, uint32_t id)
  {
    return (static_cast<uint64_t>(type) << 56) | static_cast<uint64_t>(id);
  }

  static HandleType handle_type(ScRenderHandle h)
  {
    return static_cast<HandleType>((h >> 56) & 0xFFu);
  }

  static uint32_t handle_id(ScRenderHandle h)
  {
    return static_cast<uint32_t>(h & 0xFFFFFFFFu);
  }

  static sc::Mat4 mat4_from_array(const float m[16])
  {
    sc::Mat4 out{};
    std::memcpy(out.m, m, sizeof(out.m));
    return out;
  }

  static void unpack_rgba(uint32_t rgba, float out_rgb[3])
  {
    const float inv = 1.0f / 255.0f;
    out_rgb[0] = static_cast<float>((rgba >> 24) & 0xFFu) * inv;
    out_rgb[1] = static_cast<float>((rgba >> 16) & 0xFFu) * inv;
    out_rgb[2] = static_cast<float>((rgba >> 8) & 0xFFu) * inv;
  }

  struct RenderGlobals
  {
    ScRenderInitParams params{};
    bool initialized = false;
  };

  static RenderGlobals g_globals{};
}

struct ScRenderContext
{
  sc::VkRenderer renderer{};
  sc::RenderFrameData frame{};
  sc::DebugDraw debugDraw{};
  ScRenderFrameDesc frameDesc{};
  bool hasFrameDesc = false;
  const ImDrawData* imguiDrawData = nullptr;
};

uint32_t scRenderGetApiVersion()
{
  return kRenderApiVersion;
}

int scRenderInitialize(const ScRenderInitParams* params)
{
  if (params)
    g_globals.params = *params;
  g_globals.initialized = true;

  if (params && params->asset_root && params->asset_root[0])
    sc::setAssetsRootOverride(params->asset_root);
  return 1;
}

void scRenderShutdown()
{
  sc::clearAssetsRootOverride();
  g_globals = RenderGlobals{};
}

ScRenderContext* scRenderCreateContext(SDL_Window* window, const ScRenderContextDesc* desc)
{
  if (!window || !desc)
    return nullptr;

  ScRenderContext* ctx = new ScRenderContext();
  ctx->frame.reserve(8192);
  ctx->debugDraw.reserve(65536);

  sc::VkConfig cfg{};
  cfg.enableValidation = g_globals.params.enable_validation != 0;
  cfg.enableDebugUI = false;

  if (!ctx->renderer.init(window, cfg))
  {
    delete ctx;
    return nullptr;
  }

  ctx->renderer.setDebugDraw(&ctx->debugDraw);
  return ctx;
}

void scRenderDestroyContext(ScRenderContext* ctx)
{
  if (!ctx)
    return;
  ctx->renderer.shutdown();
  delete ctx;
}

void scRenderResize(ScRenderContext* ctx, uint32_t /*w*/, uint32_t /*h*/)
{
  if (!ctx)
    return;
  ctx->renderer.onResizeRequest();
}

void scRenderBeginFrame(ScRenderContext* ctx, const ScRenderFrameDesc* frame)
{
  if (!ctx || !frame)
    return;

  ctx->frame.clear();
  ctx->debugDraw.clear();
  ctx->frameDesc = *frame;
  ctx->hasFrameDesc = true;
}

void scRenderSubmit(ScRenderContext* ctx, const ScRenderDrawList* list)
{
  if (!ctx || !list || !list->items || list->count == 0)
    return;

  ctx->frame.draws.reserve(ctx->frame.draws.size() + list->count);
  for (uint32_t i = 0; i < list->count; ++i)
  {
    const ScRenderDrawItem& src = list->items[i];
    if (handle_type(src.mesh) != HandleMesh || handle_type(src.material) != HandleMaterial)
      continue;

    sc::DrawItem di{};
    di.entity = sc::kInvalidEntity;
    di.meshId = handle_id(src.mesh);
    di.materialId = handle_id(src.material);
    di.model = mat4_from_array(src.model);
    ctx->frame.draws.push_back(di);
  }
}

void scRenderSubmitDebug(ScRenderContext* ctx, const ScRenderDebugDraw* debug)
{
  if (!ctx || !debug || !debug->lines || debug->count == 0)
    return;

  ctx->debugDraw.reserve(debug->count * 2u);
  for (uint32_t i = 0; i < debug->count; ++i)
  {
    const ScRenderLine& line = debug->lines[i];
    float color[3]{};
    unpack_rgba(line.rgba, color);
    ctx->debugDraw.addLine(line.a, line.b, color);
  }
}

void scRenderEndFrame(ScRenderContext* ctx)
{
  if (!ctx || !ctx->hasFrameDesc)
    return;

  const sc::Mat4 view = mat4_from_array(ctx->frameDesc.view);
  const sc::Mat4 proj = mat4_from_array(ctx->frameDesc.proj);
  ctx->frame.viewProj = sc::mat4_mul(proj, view);

  ctx->renderer.setRenderFrame(&ctx->frame);
  ctx->renderer.setDebugDraw(&ctx->debugDraw);
  ctx->renderer.setExternalImGuiDrawData(ctx->imguiDrawData);

  if (ctx->renderer.beginFrame())
    ctx->renderer.endFrame();

  ctx->imguiDrawData = nullptr;
}

ScRenderHandle scRenderLoadMesh(ScRenderContext* ctx, const char* asset_path_or_id)
{
  if (!ctx || !asset_path_or_id)
    return 0;
  const sc::MeshHandle handle = ctx->renderer.assets().loadMesh(asset_path_or_id);
  if (handle == sc::kInvalidMeshHandle)
    return 0;
  return make_handle(HandleMesh, handle);
}

void scRenderUnloadMesh(ScRenderContext* /*ctx*/, ScRenderHandle /*mesh*/)
{
}

ScRenderHandle scRenderLoadTexture(ScRenderContext* ctx, const char* asset_path_or_id)
{
  if (!ctx || !asset_path_or_id)
    return 0;
  const sc::TextureHandle handle = ctx->renderer.assets().loadTexture2D(asset_path_or_id);
  if (handle == sc::kInvalidTextureHandle)
    return 0;
  return make_handle(HandleTexture, handle);
}

void scRenderUnloadTexture(ScRenderContext* /*ctx*/, ScRenderHandle /*texture*/)
{
}

static bool is_unlit_material(const char* path)
{
  if (!path)
    return false;
  return std::strstr(path, "unlit") != nullptr;
}

static const char* resolve_material_texture_path(const char* path)
{
  if (!path)
    return nullptr;
  if (std::strncmp(path, "materials/", 10) == 0)
  {
    const char* name = path + 10;
    if (std::strcmp(name, "checker") == 0)
      return "textures/checker.ppm";
    if (std::strcmp(name, "test") == 0)
      return "textures/test.ppm";
  }
  return path;
}

ScRenderHandle scRenderLoadMaterial(ScRenderContext* ctx, const char* asset_path_or_id)
{
  if (!ctx || !asset_path_or_id)
    return 0;

  sc::MaterialDesc desc{};
  if (is_unlit_material(asset_path_or_id))
  {
    desc.unlit = true;
  }
  else
  {
    const char* texturePath = resolve_material_texture_path(asset_path_or_id);
    desc.albedo = ctx->renderer.assets().loadTexture2D(texturePath);
    desc.unlit = false;
  }

  const sc::MaterialHandle handle = ctx->renderer.assets().createMaterial(desc);
  if (handle == sc::kInvalidMaterialHandle)
    return 0;
  return make_handle(HandleMaterial, handle);
}

void scRenderUnloadMaterial(ScRenderContext* /*ctx*/, ScRenderHandle /*material*/)
{
}

int scRenderGetMeshInfo(ScRenderContext* ctx, ScRenderHandle mesh, ScRenderMeshInfo* out_info)
{
  if (!ctx || !out_info || handle_type(mesh) != HandleMesh)
    return 0;

  const uint32_t id = handle_id(mesh);
  if (id == 0u)
  {
    out_info->bounds_min[0] = -0.5f;
    out_info->bounds_min[1] = -0.5f;
    out_info->bounds_min[2] = 0.0f;
    out_info->bounds_max[0] = 0.5f;
    out_info->bounds_max[1] = 0.5f;
    out_info->bounds_max[2] = 0.0f;
    return 1;
  }
  if (id == 1u)
  {
    out_info->bounds_min[0] = -0.5f;
    out_info->bounds_min[1] = -0.5f;
    out_info->bounds_min[2] = -0.5f;
    out_info->bounds_max[0] = 0.5f;
    out_info->bounds_max[1] = 0.5f;
    out_info->bounds_max[2] = 0.5f;
    return 1;
  }
  return 0;
}

void scRenderGetStats(ScRenderContext* ctx, ScRenderStats* out_stats)
{
  if (!ctx || !out_stats)
    return;

  const sc::AssetStatsSnapshot stats = ctx->renderer.assets().stats();
  out_stats->draw_calls = static_cast<uint32_t>(ctx->frame.draws.size());
  out_stats->triangle_count = 0;
  out_stats->mesh_count = stats.meshCount;
  out_stats->texture_count = stats.textureCount;
  out_stats->gpu_ms = 0.0f;
  out_stats->cpu_ms = 0.0f;
}

int scRenderImGuiInit(ScRenderContext* ctx)
{
  if (!ctx)
    return 0;
  return ctx->renderer.initExternalImGui() ? 1 : 0;
}

void scRenderImGuiShutdown(ScRenderContext* ctx)
{
  if (!ctx)
    return;
  ctx->renderer.shutdownExternalImGui();
}

void scRenderImGuiNewFrame(ScRenderContext* ctx)
{
  if (!ctx)
    return;
  ctx->renderer.newExternalImGuiFrame();
}

void scRenderRenderImGui(ScRenderContext* ctx, const ImDrawData* draw_data)
{
  if (!ctx)
    return;
  ctx->imguiDrawData = draw_data;
}
