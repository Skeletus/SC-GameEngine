#include "mesh_importer.h"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstring>

namespace sc_import
{
  namespace
  {
    struct Mat4
    {
      float m[16]{};
    };

    static Mat4 mat4_identity()
    {
      Mat4 r{};
      r.m[0] = 1.0f;
      r.m[5] = 1.0f;
      r.m[10] = 1.0f;
      r.m[15] = 1.0f;
      return r;
    }

    static Mat4 mat4_mul(const Mat4& a, const Mat4& b)
    {
      Mat4 r{};
      for (int row = 0; row < 4; ++row)
      {
        for (int col = 0; col < 4; ++col)
        {
          r.m[col * 4 + row] =
            a.m[0 * 4 + row] * b.m[col * 4 + 0] +
            a.m[1 * 4 + row] * b.m[col * 4 + 1] +
            a.m[2 * 4 + row] * b.m[col * 4 + 2] +
            a.m[3 * 4 + row] * b.m[col * 4 + 3];
        }
      }
      return r;
    }

    static void mat4_transform_point(const Mat4& m, const float in[3], float out[3])
    {
      out[0] = m.m[0] * in[0] + m.m[4] * in[1] + m.m[8] * in[2] + m.m[12];
      out[1] = m.m[1] * in[0] + m.m[5] * in[1] + m.m[9] * in[2] + m.m[13];
      out[2] = m.m[2] * in[0] + m.m[6] * in[1] + m.m[10] * in[2] + m.m[14];
    }

    static void mat4_transform_dir(const Mat4& m, const float in[3], float out[3])
    {
      out[0] = m.m[0] * in[0] + m.m[4] * in[1] + m.m[8] * in[2];
      out[1] = m.m[1] * in[0] + m.m[5] * in[1] + m.m[9] * in[2];
      out[2] = m.m[2] * in[0] + m.m[6] * in[1] + m.m[10] * in[2];
      const float len = std::sqrt(out[0] * out[0] + out[1] * out[1] + out[2] * out[2]);
      if (len > 1e-6f)
      {
        out[0] /= len;
        out[1] /= len;
        out[2] /= len;
      }
    }

    static void appendMeshTransformed(const MeshData& src,
                                      const Mat4& world,
                                      MeshData* out)
    {
      if (!out)
        return;

      const uint32_t baseVertex = static_cast<uint32_t>(out->vertices.size());
      out->vertices.reserve(out->vertices.size() + src.vertices.size());
      for (const MeshVertex& v : src.vertices)
      {
        MeshVertex nv = v;
        mat4_transform_point(world, v.pos, nv.pos);
        if ((src.vertexLayoutFlags & VertexLayout_Normal) != 0)
          mat4_transform_dir(world, v.normal, nv.normal);
        out->vertices.push_back(nv);
      }

      const uint32_t baseIndex = static_cast<uint32_t>(out->indices.size());
      out->indices.reserve(out->indices.size() + src.indices.size());
      for (uint32_t idx : src.indices)
        out->indices.push_back(baseVertex + idx);

      for (const Submesh& sm : src.submeshes)
      {
        Submesh outSm = sm;
        outSm.indexOffset = baseIndex + sm.indexOffset;
        out->submeshes.push_back(outSm);
      }

      out->vertexLayoutFlags |= src.vertexLayoutFlags;
    }

    static void buildWorldMatrices(const ImportedModel& model,
                                   int nodeIndex,
                                   const Mat4& parent,
                                   std::vector<Mat4>& outWorld)
    {
      if (nodeIndex < 0 || nodeIndex >= static_cast<int>(model.nodes.size()))
        return;
      const ImportedNode& node = model.nodes[nodeIndex];
      Mat4 local{};
      std::memcpy(local.m, node.localMatrix, sizeof(local.m));
      const Mat4 world = mat4_mul(parent, local);
      outWorld[nodeIndex] = world;
      for (int child : node.children)
        buildWorldMatrices(model, child, world, outWorld);
    }
  }

  void ImporterRegistry::registerImporter(std::unique_ptr<IMeshImporter> importer)
  {
    if (importer)
      m_importers.push_back(std::move(importer));
  }

  bool ImporterRegistry::importModel(const char* absPath,
                                     const MeshImportOptions& options,
                                     ImportedModel* out_model,
                                     std::string* out_error) const
  {
    if (!absPath || !out_model)
      return false;

    std::string ext;
    if (const char* dot = std::strrchr(absPath, '.'))
      ext = dot;
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c)
    {
      return static_cast<char>(std::tolower(c));
    });

    for (const auto& importer : m_importers)
    {
      if (importer && importer->canImportExtension(ext.c_str()))
        return importer->importFile(absPath, options, out_model, out_error);
    }

    if (out_error)
      *out_error = "No importer for extension.";
    return false;
  }

  void ComputeMeshBounds(MeshData* mesh)
  {
    if (!mesh || mesh->vertices.empty())
      return;

    float minv[3] = { mesh->vertices[0].pos[0], mesh->vertices[0].pos[1], mesh->vertices[0].pos[2] };
    float maxv[3] = { minv[0], minv[1], minv[2] };
    for (const MeshVertex& v : mesh->vertices)
    {
      for (int i = 0; i < 3; ++i)
      {
        minv[i] = std::min(minv[i], v.pos[i]);
        maxv[i] = std::max(maxv[i], v.pos[i]);
      }
    }

    mesh->bounds.min[0] = minv[0];
    mesh->bounds.min[1] = minv[1];
    mesh->bounds.min[2] = minv[2];
    mesh->bounds.max[0] = maxv[0];
    mesh->bounds.max[1] = maxv[1];
    mesh->bounds.max[2] = maxv[2];
    mesh->bounds.center[0] = (minv[0] + maxv[0]) * 0.5f;
    mesh->bounds.center[1] = (minv[1] + maxv[1]) * 0.5f;
    mesh->bounds.center[2] = (minv[2] + maxv[2]) * 0.5f;

    float radius = 0.0f;
    for (const MeshVertex& v : mesh->vertices)
    {
      const float dx = v.pos[0] - mesh->bounds.center[0];
      const float dy = v.pos[1] - mesh->bounds.center[1];
      const float dz = v.pos[2] - mesh->bounds.center[2];
      const float d2 = dx * dx + dy * dy + dz * dz;
      if (d2 > radius)
        radius = d2;
    }
    mesh->bounds.radius = std::sqrt(radius);
  }

  bool FlattenModelToMesh(const ImportedModel& model, MeshData* out_mesh, std::string* out_error)
  {
    if (!out_mesh)
      return false;
    out_mesh->vertices.clear();
    out_mesh->indices.clear();
    out_mesh->submeshes.clear();
    out_mesh->vertexLayoutFlags = 0;

    if (model.meshes.empty() || model.nodes.empty())
    {
      if (out_error) *out_error = "Model has no meshes or nodes.";
      return false;
    }

    std::vector<Mat4> world(model.nodes.size(), mat4_identity());
    std::vector<int> roots = model.sceneRoots;
    if (roots.empty())
    {
      roots.reserve(model.nodes.size());
      for (size_t i = 0; i < model.nodes.size(); ++i)
      {
        if (model.nodes[i].parent < 0)
          roots.push_back(static_cast<int>(i));
      }
    }

    for (int root : roots)
      buildWorldMatrices(model, root, mat4_identity(), world);

    for (size_t i = 0; i < model.nodes.size(); ++i)
    {
      const ImportedNode& node = model.nodes[i];
      if (node.meshIndex < 0 || node.meshIndex >= static_cast<int>(model.meshes.size()))
        continue;
      const ImportedMesh& mesh = model.meshes[node.meshIndex];
      appendMeshTransformed(mesh.mesh, world[i], out_mesh);
    }

    if (out_mesh->vertices.empty())
    {
      if (out_error) *out_error = "No geometry found after flatten.";
      return false;
    }

    out_mesh->vertexLayoutFlags |= VertexLayout_Position;
    ComputeMeshBounds(out_mesh);
    return true;
  }
}
