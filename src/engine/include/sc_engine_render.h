#pragma once
#include <stdint.h>
#include <stddef.h>

#ifdef _WIN32
  #ifdef SC_RENDER_EXPORTS
    #define SC_RENDER_API __declspec(dllexport)
  #else
    #define SC_RENDER_API __declspec(dllimport)
  #endif
#else
  #define SC_RENDER_API
#endif

struct SDL_Window;
struct ImDrawData;

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ScRenderContext ScRenderContext;

typedef uint64_t ScRenderHandle;

typedef struct ScRenderInitParams
{
  const char* asset_root;
  uint32_t enable_validation;
} ScRenderInitParams;

typedef struct ScRenderContextDesc
{
  uint32_t width;
  uint32_t height;
  uint32_t vsync;
} ScRenderContextDesc;

typedef struct ScRenderFrameDesc
{
  float view[16];
  float proj[16];
  float camera_pos[3];
  float time_sec;
} ScRenderFrameDesc;

typedef struct ScRenderDrawItem
{
  ScRenderHandle mesh;
  ScRenderHandle material;
  float model[16];
  uint32_t flags;
} ScRenderDrawItem;

typedef struct ScRenderDrawList
{
  const ScRenderDrawItem* items;
  uint32_t count;
} ScRenderDrawList;

typedef struct ScRenderMeshInfo
{
  float bounds_min[3];
  float bounds_max[3];
} ScRenderMeshInfo;

typedef struct ScRenderLine
{
  float a[3];
  float b[3];
  uint32_t rgba;
} ScRenderLine;

typedef struct ScRenderDebugDraw
{
  const ScRenderLine* lines;
  uint32_t count;
} ScRenderDebugDraw;

typedef struct ScRenderStats
{
  uint32_t draw_calls;
  uint32_t triangle_count;
  uint32_t mesh_count;
  uint32_t texture_count;
  float gpu_ms;
  float cpu_ms;
} ScRenderStats;

SC_RENDER_API uint32_t scRenderGetApiVersion();

SC_RENDER_API int scRenderInitialize(const ScRenderInitParams* params);
SC_RENDER_API void scRenderShutdown();

SC_RENDER_API ScRenderContext* scRenderCreateContext(struct SDL_Window* window,
                                                     const ScRenderContextDesc* desc);
SC_RENDER_API void scRenderDestroyContext(ScRenderContext* ctx);
SC_RENDER_API void scRenderResize(ScRenderContext* ctx, uint32_t w, uint32_t h);

SC_RENDER_API void scRenderBeginFrame(ScRenderContext* ctx, const ScRenderFrameDesc* frame);
SC_RENDER_API void scRenderSubmit(ScRenderContext* ctx, const ScRenderDrawList* list);
SC_RENDER_API void scRenderSubmitDebug(ScRenderContext* ctx, const ScRenderDebugDraw* debug);
SC_RENDER_API void scRenderEndFrame(ScRenderContext* ctx);

SC_RENDER_API ScRenderHandle scRenderLoadMesh(ScRenderContext* ctx, const char* asset_path_or_id);
SC_RENDER_API void scRenderUnloadMesh(ScRenderContext* ctx, ScRenderHandle mesh);
SC_RENDER_API ScRenderHandle scRenderLoadTexture(ScRenderContext* ctx, const char* asset_path_or_id);
SC_RENDER_API void scRenderUnloadTexture(ScRenderContext* ctx, ScRenderHandle texture);
SC_RENDER_API ScRenderHandle scRenderLoadMaterial(ScRenderContext* ctx, const char* asset_path_or_id);
SC_RENDER_API void scRenderUnloadMaterial(ScRenderContext* ctx, ScRenderHandle material);

SC_RENDER_API int scRenderGetMeshInfo(ScRenderContext* ctx, ScRenderHandle mesh, ScRenderMeshInfo* out_info);
SC_RENDER_API void scRenderGetStats(ScRenderContext* ctx, ScRenderStats* out_stats);

SC_RENDER_API int scRenderImGuiInit(ScRenderContext* ctx);
SC_RENDER_API void scRenderImGuiShutdown(ScRenderContext* ctx);
SC_RENDER_API void scRenderImGuiNewFrame(ScRenderContext* ctx);
SC_RENDER_API void scRenderRenderImGui(ScRenderContext* ctx, const struct ImDrawData* draw_data);

#ifdef __cplusplus
}
#endif
