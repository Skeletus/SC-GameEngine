#include "editor_core.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdio>

namespace sc
{
namespace editor
{
  namespace
  {
    struct Vec3
    {
      float x = 0.0f;
      float y = 0.0f;
      float z = 0.0f;
    };

    static Vec3 v3(float x, float y, float z) { return Vec3{ x, y, z }; }
    static Vec3 add(const Vec3& a, const Vec3& b) { return v3(a.x + b.x, a.y + b.y, a.z + b.z); }
    static Vec3 sub(const Vec3& a, const Vec3& b) { return v3(a.x - b.x, a.y - b.y, a.z - b.z); }
    static Vec3 mul(const Vec3& a, float s) { return v3(a.x * s, a.y * s, a.z * s); }
    static float dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
    static Vec3 cross(const Vec3& a, const Vec3& b)
    {
      return v3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
    }
    static float len(const Vec3& v) { return std::sqrt(dot(v, v)); }
    static Vec3 normalize(const Vec3& v)
    {
      const float l = len(v);
      if (l <= 1e-6f)
        return v3(0, 0, 0);
      return mul(v, 1.0f / l);
    }
    static void mulMat4Vec4(const sc::Mat4& m, const float v[4], float out[4])
    {
      for (int row = 0; row < 4; ++row)
      {
        out[row] =
          m.m[0 * 4 + row] * v[0] +
          m.m[1 * 4 + row] * v[1] +
          m.m[2 * 4 + row] * v[2] +
          m.m[3 * 4 + row] * v[3];
      }
    }

    static bool meshBoundsValid(const ScRenderMeshInfo& info)
    {
      const float dx = std::fabs(info.bounds_max[0] - info.bounds_min[0]);
      const float dy = std::fabs(info.bounds_max[1] - info.bounds_min[1]);
      const float dz = std::fabs(info.bounds_max[2] - info.bounds_min[2]);
      return (dx > 1e-4f) || (dy > 1e-4f) || (dz > 1e-4f);
    }

    static void getEntityWorldAabb(const EditorEntity& e, float out_min[3], float out_max[3])
    {
      float local_min[3] = { -0.5f, -0.5f, -0.5f };
      float local_max[3] = { 0.5f, 0.5f, 0.5f };

      if (e.meshHandle != 0 && meshBoundsValid(e.meshInfo))
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
  }

  bool EditorAssetRegistry::loadFromFile(const char* path)
  {
    entries.clear();
    return sc_world::LoadAssetRegistry(path, entries);
  }

  const sc_world::AssetRegistryEntry* EditorAssetRegistry::findByIds(sc_world::AssetId meshId,
                                                                     sc_world::AssetId materialId) const
  {
    return sc_world::FindByIds(entries, meshId, materialId);
  }

  void InitDocument(EditorDocument* doc)
  {
    if (!doc)
      return;
    doc->entities.clear();
    doc->nextId = 1;
    doc->selectedId = 0;
    doc->sector = {};
    doc->sectorSize = 64.0f;
    doc->gridSize = 1.0f;
    doc->snapToGrid = true;
  }

  EditorEntity* AddEntity(EditorDocument* doc, const sc_world::AssetRegistryEntry& asset, const EditorTransform& t)
  {
    if (!doc)
      return nullptr;
    EditorEntity e{};
    e.id = doc->nextId++;
    std::snprintf(e.name, sizeof(e.name), "%s", asset.label.c_str());
    e.transform = t;
    e.meshAssetId = asset.mesh_id;
    e.materialAssetId = asset.material_id;
    doc->entities.push_back(e);
    return &doc->entities.back();
  }

  bool RemoveEntity(EditorDocument* doc, uint64_t id, EditorEntity* out_removed)
  {
    if (!doc)
      return false;
    for (size_t i = 0; i < doc->entities.size(); ++i)
    {
      if (doc->entities[i].id == id)
      {
        if (out_removed)
          *out_removed = doc->entities[i];
        doc->entities.erase(doc->entities.begin() + i);
        if (doc->selectedId == id)
          doc->selectedId = 0;
        return true;
      }
    }
    return false;
  }

  EditorEntity* FindEntity(EditorDocument* doc, uint64_t id)
  {
    if (!doc)
      return nullptr;
    for (auto& e : doc->entities)
    {
      if (e.id == id)
        return &e;
    }
    return nullptr;
  }

  void SetSelected(EditorDocument* doc, uint64_t id)
  {
    if (!doc)
      return;
    if (id == 0)
    {
      doc->selectedId = 0;
      return;
    }
    doc->selectedId = FindEntity(doc, id) ? id : 0;
  }

  void ValidateSelection(EditorDocument* doc)
  {
    if (!doc)
      return;
    if (doc->selectedId != 0 && !FindEntity(doc, doc->selectedId))
      doc->selectedId = 0;
  }

  void ResolveEntityAssets(EditorEntity* e, ScRenderContext* render, const EditorAssetRegistry& assets)
  {
    if (!e || !render)
      return;
    const sc_world::AssetRegistryEntry* entry = assets.findByIds(e->meshAssetId, e->materialAssetId);
    if (!entry)
      return;
    e->meshHandle = scRenderLoadMesh(render, entry->mesh_path.c_str());
    e->materialHandle = scRenderLoadMaterial(render, entry->material_path.c_str());
    scRenderGetMeshInfo(render, e->meshHandle, &e->meshInfo);
  }

  void BuildDrawItems(const EditorDocument* doc, std::vector<ScRenderDrawItem>* out_items)
  {
    if (!doc || !out_items)
      return;
    out_items->clear();
    out_items->reserve(doc->entities.size());

    for (const EditorEntity& e : doc->entities)
    {
      if (e.meshHandle == 0 || e.materialHandle == 0)
        continue;

      ScRenderDrawItem di{};
      di.mesh = e.meshHandle;
      di.material = e.materialHandle;
      di.flags = 0;

      const sc::Mat4 model = sc::mat4_trs(e.transform.position, e.transform.rotation, e.transform.scale);
      std::memcpy(di.model, model.m, sizeof(di.model));
      out_items->push_back(di);
    }
  }

  void BuildDebugLines(const EditorDocument* doc, std::vector<ScRenderLine>* out_lines)
  {
    if (!doc || !out_lines)
      return;
    out_lines->clear();

    const float extent = doc->sectorSize * 0.5f;
    const float y = 0.0f;
    const float grid = std::max(0.001f, doc->gridSize);
    const int grid_count = static_cast<int>(std::floor((extent * 2.0f) / grid)) + 1;
    out_lines->reserve(static_cast<size_t>(grid_count) * 2u + 4u + (doc->selectedId != 0 ? 12u : 0u));

    for (float x = -extent; x <= extent; x += grid)
    {
      ScRenderLine l{};
      l.a[0] = x; l.a[1] = y; l.a[2] = -extent;
      l.b[0] = x; l.b[1] = y; l.b[2] = extent;
      l.rgba = 0x404040FF;
      out_lines->push_back(l);
    }
    for (float z = -extent; z <= extent; z += grid)
    {
      ScRenderLine l{};
      l.a[0] = -extent; l.a[1] = y; l.a[2] = z;
      l.b[0] = extent; l.b[1] = y; l.b[2] = z;
      l.rgba = 0x404040FF;
      out_lines->push_back(l);
    }

    ScRenderLine b0{{-extent,y,-extent},{extent,y,-extent},0x00FF00FF};
    ScRenderLine b1{{extent,y,-extent},{extent,y,extent},0x00FF00FF};
    ScRenderLine b2{{extent,y,extent},{-extent,y,extent},0x00FF00FF};
    ScRenderLine b3{{-extent,y,extent},{-extent,y,-extent},0x00FF00FF};
    out_lines->push_back(b0);
    out_lines->push_back(b1);
    out_lines->push_back(b2);
    out_lines->push_back(b3);

    if (doc->selectedId != 0)
    {
      const EditorEntity* selected = nullptr;
      for (const EditorEntity& e : doc->entities)
      {
        if (e.id == doc->selectedId)
        {
          selected = &e;
          break;
        }
      }

      if (selected)
      {
        float bmin[3]{};
        float bmax[3]{};
        getEntityWorldAabb(*selected, bmin, bmax);
        const float minx = bmin[0];
        const float miny = bmin[1];
        const float minz = bmin[2];
        const float maxx = bmax[0];
        const float maxy = bmax[1];
        const float maxz = bmax[2];

        const uint32_t color = 0xFFD200FF;
        auto push_line = [&](float ax, float ay, float az, float bx, float by, float bz)
        {
          ScRenderLine l{};
          l.a[0] = ax; l.a[1] = ay; l.a[2] = az;
          l.b[0] = bx; l.b[1] = by; l.b[2] = bz;
          l.rgba = color;
          out_lines->push_back(l);
        };

        push_line(minx, miny, minz, maxx, miny, minz);
        push_line(maxx, miny, minz, maxx, miny, maxz);
        push_line(maxx, miny, maxz, minx, miny, maxz);
        push_line(minx, miny, maxz, minx, miny, minz);

        push_line(minx, maxy, minz, maxx, maxy, minz);
        push_line(maxx, maxy, minz, maxx, maxy, maxz);
        push_line(maxx, maxy, maxz, minx, maxy, maxz);
        push_line(minx, maxy, maxz, minx, maxy, minz);

        push_line(minx, miny, minz, minx, maxy, minz);
        push_line(maxx, miny, minz, maxx, maxy, minz);
        push_line(maxx, miny, maxz, maxx, maxy, maxz);
        push_line(minx, miny, maxz, minx, maxy, maxz);
      }
    }
  }

  sc::Mat4 CameraView(const EditorCamera* cam)
  {
    float rot[3] = { cam->pitch, cam->yaw, 0.0f };
    float scale[3] = { 1.0f, 1.0f, 1.0f };
    const sc::Mat4 world = sc::mat4_trs(cam->position, rot, scale);
    return sc::mat4_inverse(world);
  }

  sc::Mat4 CameraProj(const EditorCamera* cam, float aspect)
  {
    const float fovRad = cam->fovDeg * 3.1415926535f / 180.0f;
    return sc::mat4_perspective_rh_zo(fovRad, aspect, cam->nearZ, cam->farZ, true);
  }

  void CameraForward(const EditorCamera* cam, float out_forward[3])
  {
    const float cy = std::cos(cam->yaw);
    const float sy = std::sin(cam->yaw);
    const float cp = std::cos(cam->pitch);
    const float sp = std::sin(cam->pitch);
    out_forward[0] = sy * cp;
    out_forward[1] = -sp;
    out_forward[2] = cy * cp;
    const float inv = 1.0f / std::max(0.0001f, std::sqrt(out_forward[0] * out_forward[0] +
                                                       out_forward[1] * out_forward[1] +
                                                       out_forward[2] * out_forward[2]));
    out_forward[0] *= inv;
    out_forward[1] *= inv;
    out_forward[2] *= inv;
  }

  Ray computePickRay(const EditorCamera* cam,
                     float mouse_x,
                     float mouse_y,
                     float viewport_x,
                     float viewport_y,
                     float viewport_w,
                     float viewport_h)
  {
    Ray ray{};
    ray.origin[0] = cam->position[0];
    ray.origin[1] = cam->position[1];
    ray.origin[2] = cam->position[2];

    const float safe_w = std::max(1.0f, viewport_w);
    const float safe_h = std::max(1.0f, viewport_h);
    const float ndc_x = ((mouse_x - viewport_x) / safe_w) * 2.0f - 1.0f;
    const float ndc_y = 1.0f - ((mouse_y - viewport_y) / safe_h) * 2.0f;

    const float aspect = safe_w / safe_h;
    const sc::Mat4 view = CameraView(cam);
    const sc::Mat4 proj = CameraProj(cam, aspect);
    const sc::Mat4 inv_view_proj = sc::mat4_inverse(sc::mat4_mul(proj, view));

    const float near_clip[4] = { ndc_x, ndc_y, 0.0f, 1.0f };
    const float far_clip[4] = { ndc_x, ndc_y, 1.0f, 1.0f };
    float near_world[4]{};
    float far_world[4]{};
    mulMat4Vec4(inv_view_proj, near_clip, near_world);
    mulMat4Vec4(inv_view_proj, far_clip, far_world);

    if (std::fabs(near_world[3]) > 1e-6f)
    {
      near_world[0] /= near_world[3];
      near_world[1] /= near_world[3];
      near_world[2] /= near_world[3];
    }
    if (std::fabs(far_world[3]) > 1e-6f)
    {
      far_world[0] /= far_world[3];
      far_world[1] /= far_world[3];
      far_world[2] /= far_world[3];
    }

    Vec3 cam_pos = v3(cam->position[0], cam->position[1], cam->position[2]);
    Vec3 far_pos = v3(far_world[0], far_world[1], far_world[2]);
    Vec3 dir = normalize(sub(far_pos, cam_pos));
    ray.dir[0] = dir.x;
    ray.dir[1] = dir.y;
    ray.dir[2] = dir.z;
    return ray;
  }

  bool intersectRayAABB(const Ray& ray, const float bmin[3], const float bmax[3], float* out_t)
  {
    float tmin = 0.0f;
    float tmax = 1e30f;
    for (int i = 0; i < 3; ++i)
    {
      const float origin = ray.origin[i];
      const float dir = ray.dir[i];
      float minv = bmin[i];
      float maxv = bmax[i];
      if (std::fabs(dir) < 1e-6f)
      {
        if (origin < minv || origin > maxv)
          return false;
      }
      else
      {
        const float ood = 1.0f / dir;
        float t1 = (minv - origin) * ood;
        float t2 = (maxv - origin) * ood;
        if (t1 > t2)
          std::swap(t1, t2);
        tmin = std::max(tmin, t1);
        tmax = std::min(tmax, t2);
        if (tmin > tmax)
          return false;
      }
    }
    if (out_t)
      *out_t = tmin;
    return true;
  }

  Ray BuildPickRay(const EditorCamera* cam,
                   float mouse_x,
                   float mouse_y,
                   float viewport_x,
                   float viewport_y,
                   float viewport_w,
                   float viewport_h)
  {
    return computePickRay(cam, mouse_x, mouse_y, viewport_x, viewport_y, viewport_w, viewport_h);
  }

  uint64_t PickEntity(const EditorDocument* doc, const Ray& ray)
  {
    if (!doc)
      return 0;
    float best_t = 1e30f;
    uint64_t best_id = 0;

    for (const EditorEntity& e : doc->entities)
    {
      float t = 0.0f;
      float bmin[3]{};
      float bmax[3]{};
      getEntityWorldAabb(e, bmin, bmax);
      if (intersectRayAABB(ray, bmin, bmax, &t))
      {
        if (t < best_t)
        {
          best_t = t;
          best_id = e.id;
        }
      }
    }
    return best_id;
  }

  bool GizmoTranslate(GizmoState* state,
                      EditorEntity* entity,
                      const EditorCamera* cam,
                      float mouse_x,
                      float mouse_y,
                      float viewport_x,
                      float viewport_y,
                      float viewport_w,
                      float viewport_h,
                      bool mouse_down,
                      bool mouse_dragging,
                      bool mouse_released,
                      bool snap_to_grid,
                      float grid_size)
  {
    if (!state || !entity || !cam)
      return false;

    Vec3 axes[3] = { v3(1,0,0), v3(0,1,0), v3(0,0,1) };
    const float axis_len = 2.0f;

    Ray ray = computePickRay(cam, mouse_x, mouse_y, viewport_x, viewport_y, viewport_w, viewport_h);

    if (!state->active && mouse_down)
    {
      float best = 1e30f;
      int best_axis = -1;
      for (int i = 0; i < 3; ++i)
      {
        Vec3 p0 = v3(entity->transform.position[0], entity->transform.position[1], entity->transform.position[2]);
        Vec3 p1 = add(p0, mul(axes[i], axis_len));
        Vec3 v = sub(p1, p0);
        Vec3 w0 = sub(v3(ray.origin[0], ray.origin[1], ray.origin[2]), p0);
        float a = dot(v, v);
        float b = dot(v, v3(ray.dir[0], ray.dir[1], ray.dir[2]));
        float c = dot(v3(ray.dir[0], ray.dir[1], ray.dir[2]), v3(ray.dir[0], ray.dir[1], ray.dir[2]));
        float d = dot(v, w0);
        float e = dot(v3(ray.dir[0], ray.dir[1], ray.dir[2]), w0);
        float denom = a * c - b * b;
        if (std::fabs(denom) < 1e-6f)
          continue;
        float s = (b * e - c * d) / denom;
        float t = (a * e - b * d) / denom;
        Vec3 c1 = add(p0, mul(v, s));
        Vec3 c2 = add(v3(ray.origin[0], ray.origin[1], ray.origin[2]), mul(v3(ray.dir[0], ray.dir[1], ray.dir[2]), t));
        float dist = len(sub(c1, c2));
        if (dist < best && dist < 0.2f)
        {
          best = dist;
          best_axis = i;
          state->startAxisT = s;
        }
      }

      if (best_axis >= 0)
      {
        state->active = true;
        state->axis = best_axis;
        state->startTransform = entity->transform;
        return true;
      }
    }

    if (state->active && mouse_dragging)
    {
      Vec3 axis = axes[state->axis];
      Vec3 p0 = v3(state->startTransform.position[0], state->startTransform.position[1], state->startTransform.position[2]);
      Vec3 w0 = sub(v3(ray.origin[0], ray.origin[1], ray.origin[2]), p0);
      float a = dot(axis, axis);
      float b = dot(axis, v3(ray.dir[0], ray.dir[1], ray.dir[2]));
      float c = dot(v3(ray.dir[0], ray.dir[1], ray.dir[2]), v3(ray.dir[0], ray.dir[1], ray.dir[2]));
      float d = dot(axis, w0);
      float e = dot(v3(ray.dir[0], ray.dir[1], ray.dir[2]), w0);
      float denom = a * c - b * b;
      if (std::fabs(denom) > 1e-6f)
      {
        float s = (b * e - c * d) / denom;
        float delta = s - state->startAxisT;
        entity->transform.position[0] = state->startTransform.position[0] + axis.x * delta;
        entity->transform.position[1] = state->startTransform.position[1] + axis.y * delta;
        entity->transform.position[2] = state->startTransform.position[2] + axis.z * delta;
        if (snap_to_grid)
          SnapTransform(&entity->transform, grid_size);
      }
      return true;
    }

    if (state->active && mouse_released)
    {
      state->active = false;
      state->axis = -1;
    }

    return false;
  }

  void SnapTransform(EditorTransform* t, float grid)
  {
    if (!t || grid <= 0.0f)
      return;
    t->position[0] = std::round(t->position[0] / grid) * grid;
    t->position[1] = std::round(t->position[1] / grid) * grid;
    t->position[2] = std::round(t->position[2] / grid) * grid;
  }

  void DocumentFromSectorFile(EditorDocument* doc,
                              const sc_world::SectorFile& file,
                              ScRenderContext* render,
                              const EditorAssetRegistry& assets)
  {
    if (!doc)
      return;
    InitDocument(doc);
    doc->sector = file.sector;

    for (const auto& inst : file.instances)
    {
      EditorEntity e{};
      e.id = inst.id;
      e.meshAssetId = inst.mesh_id;
      e.materialAssetId = inst.material_id;
      if (inst.name[0] != '\0')
        std::snprintf(e.name, sizeof(e.name), "%s", inst.name);
      else
        std::snprintf(e.name, sizeof(e.name), "Inst_%llu", (unsigned long long)inst.id);
      e.transform.position[0] = inst.transform.position[0];
      e.transform.position[1] = inst.transform.position[1];
      e.transform.position[2] = inst.transform.position[2];
      e.transform.rotation[0] = inst.transform.rotation[0];
      e.transform.rotation[1] = inst.transform.rotation[1];
      e.transform.rotation[2] = inst.transform.rotation[2];
      e.transform.scale[0] = inst.transform.scale[0];
      e.transform.scale[1] = inst.transform.scale[1];
      e.transform.scale[2] = inst.transform.scale[2];

      ResolveEntityAssets(&e, render, assets);
      doc->entities.push_back(e);
      doc->nextId = std::max(doc->nextId, e.id + 1);
    }
  }

  void SectorFileFromDocument(const EditorDocument* doc, sc_world::SectorFile* out_file)
  {
    if (!doc || !out_file)
      return;
    out_file->version = sc_world::kSectorVersion;
    out_file->sector = doc->sector;
    out_file->instances.clear();
    out_file->lanes.clear();
    out_file->spawners.clear();
    out_file->colliders.clear();

    for (const EditorEntity& e : doc->entities)
    {
      sc_world::Instance inst{};
      inst.id = e.id;
      inst.mesh_id = e.meshAssetId;
      inst.material_id = e.materialAssetId;
      std::snprintf(inst.name, sizeof(inst.name), "%s", e.name);
      inst.tags = e.tags;
      inst.transform.position[0] = e.transform.position[0];
      inst.transform.position[1] = e.transform.position[1];
      inst.transform.position[2] = e.transform.position[2];
      inst.transform.rotation[0] = e.transform.rotation[0];
      inst.transform.rotation[1] = e.transform.rotation[1];
      inst.transform.rotation[2] = e.transform.rotation[2];
      inst.transform.scale[0] = e.transform.scale[0];
      inst.transform.scale[1] = e.transform.scale[1];
      inst.transform.scale[2] = e.transform.scale[2];
      out_file->instances.push_back(inst);
    }
  }

  void CommandStack::execute(std::unique_ptr<Command> cmd, EditorDocument* doc)
  {
    if (!cmd)
      return;
    cmd->apply(doc);
    undo.push_back(std::move(cmd));
    redo.clear();
  }

  void CommandStack::undoLast(EditorDocument* doc)
  {
    if (undo.empty())
      return;
    auto cmd = std::move(undo.back());
    undo.pop_back();
    cmd->undo(doc);
    redo.push_back(std::move(cmd));
  }

  void CommandStack::redoLast(EditorDocument* doc)
  {
    if (redo.empty())
      return;
    auto cmd = std::move(redo.back());
    redo.pop_back();
    cmd->apply(doc);
    undo.push_back(std::move(cmd));
  }

  void CmdPlaceEntity::apply(EditorDocument* doc)
  {
    if (!doc)
      return;
    doc->entities.push_back(entity);
    doc->nextId = std::max(doc->nextId, entity.id + 1);
  }

  void CmdPlaceEntity::undo(EditorDocument* doc)
  {
    if (!doc)
      return;
    RemoveEntity(doc, entity.id, nullptr);
  }

  void CmdDeleteEntity::apply(EditorDocument* doc)
  {
    if (!doc)
      return;
    RemoveEntity(doc, entity.id, nullptr);
  }

  void CmdDeleteEntity::undo(EditorDocument* doc)
  {
    if (!doc)
      return;
    doc->entities.push_back(entity);
    doc->nextId = std::max(doc->nextId, entity.id + 1);
  }

  void CmdTransformEntity::apply(EditorDocument* doc)
  {
    if (EditorEntity* e = FindEntity(doc, entityId))
      e->transform = after;
  }

  void CmdTransformEntity::undo(EditorDocument* doc)
  {
    if (EditorEntity* e = FindEntity(doc, entityId))
      e->transform = before;
  }

  void CmdSetProperty::apply(EditorDocument* doc)
  {
    EditorEntity* e = FindEntity(doc, entityId);
    if (!e)
      return;
    if (type == MeshId) e->meshAssetId = newId;
    if (type == MaterialId) e->materialAssetId = newId;
    if (type == Name) std::snprintf(e->name, sizeof(e->name), "%s", newName);
    if (type == Tags) e->tags = newTags;
  }

  void CmdSetProperty::undo(EditorDocument* doc)
  {
    EditorEntity* e = FindEntity(doc, entityId);
    if (!e)
      return;
    if (type == MeshId) e->meshAssetId = oldId;
    if (type == MaterialId) e->materialAssetId = oldId;
    if (type == Name) std::snprintf(e->name, sizeof(e->name), "%s", oldName);
    if (type == Tags) e->tags = oldTags;
  }
}
}
