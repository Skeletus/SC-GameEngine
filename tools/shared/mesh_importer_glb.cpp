#define SCGLTF_IMPLEMENTATION
#include "scgltf.h"

#include "mesh_importer.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace sc_import
{
  namespace
  {
    struct AccessorView
    {
      const uint8_t* data = nullptr;
      size_t stride = 0;
      size_t count = 0;
      int componentType = 0;
      int components = 0;
      bool normalized = false;
    };

    static size_t componentSize(int componentType)
    {
      switch (componentType)
      {
        case 5120: // BYTE
        case 5121: // UNSIGNED_BYTE
          return 1;
        case 5122: // SHORT
        case 5123: // UNSIGNED_SHORT
          return 2;
        case 5125: // UNSIGNED_INT
        case 5126: // FLOAT
          return 4;
        default:
          return 0;
      }
    }

    static bool resolveAccessor(const scgltf::Document& doc,
                                int accessorIndex,
                                AccessorView* out,
                                std::string* out_error)
    {
      if (!out)
        return false;
      if (accessorIndex < 0 || accessorIndex >= static_cast<int>(doc.accessors.size()))
      {
        if (out_error) *out_error = "Accessor index out of range.";
        return false;
      }

      const scgltf::Accessor& acc = doc.accessors[accessorIndex];
      if (acc.bufferView < 0 || acc.bufferView >= static_cast<int>(doc.bufferViews.size()))
      {
        if (out_error) *out_error = "Accessor missing bufferView.";
        return false;
      }

      const scgltf::BufferView& bv = doc.bufferViews[acc.bufferView];
      if (bv.buffer < 0 || bv.buffer >= static_cast<int>(doc.buffers.size()))
      {
        if (out_error) *out_error = "BufferView buffer index out of range.";
        return false;
      }

      const std::vector<uint8_t>& buffer = doc.buffers[bv.buffer];
      const size_t compSize = componentSize(acc.componentType);
      if (compSize == 0 || acc.components == 0)
      {
        if (out_error) *out_error = "Unsupported accessor component type.";
        return false;
      }

      const size_t stride = (bv.byteStride != 0) ? bv.byteStride : compSize * static_cast<size_t>(acc.components);
      const size_t start = bv.byteOffset + acc.byteOffset;
      const size_t required = start + stride * (acc.count > 0 ? (acc.count - 1) : 0) + compSize * acc.components;
      if (required > buffer.size())
      {
        if (out_error) *out_error = "Accessor data out of buffer bounds.";
        return false;
      }

      out->data = buffer.data() + start;
      out->stride = stride;
      out->count = acc.count;
      out->componentType = acc.componentType;
      out->components = acc.components;
      out->normalized = acc.normalized;
      return true;
    }

    static float readComponentAsFloat(const uint8_t* ptr, int componentType, bool normalized)
    {
      switch (componentType)
      {
        case 5126: // FLOAT
        {
          float v = 0.0f;
          std::memcpy(&v, ptr, sizeof(float));
          return v;
        }
        case 5121: // UNSIGNED_BYTE
        {
          const uint8_t v = *ptr;
          return normalized ? (static_cast<float>(v) / 255.0f) : static_cast<float>(v);
        }
        case 5123: // UNSIGNED_SHORT
        {
          uint16_t v = 0;
          std::memcpy(&v, ptr, sizeof(uint16_t));
          return normalized ? (static_cast<float>(v) / 65535.0f) : static_cast<float>(v);
        }
        case 5125: // UNSIGNED_INT
        {
          uint32_t v = 0;
          std::memcpy(&v, ptr, sizeof(uint32_t));
          return normalized ? (static_cast<float>(v) / 4294967295.0f) : static_cast<float>(v);
        }
        case 5120: // BYTE
        {
          int8_t v = 0;
          std::memcpy(&v, ptr, sizeof(int8_t));
          return normalized ? (std::max(-1.0f, static_cast<float>(v) / 127.0f)) : static_cast<float>(v);
        }
        case 5122: // SHORT
        {
          int16_t v = 0;
          std::memcpy(&v, ptr, sizeof(int16_t));
          return normalized ? (std::max(-1.0f, static_cast<float>(v) / 32767.0f)) : static_cast<float>(v);
        }
        default:
          return 0.0f;
      }
    }

    static uint32_t readIndex(const uint8_t* ptr, int componentType)
    {
      switch (componentType)
      {
        case 5121: return *ptr;
        case 5123:
        {
          uint16_t v = 0;
          std::memcpy(&v, ptr, sizeof(uint16_t));
          return static_cast<uint32_t>(v);
        }
        case 5125:
        {
          uint32_t v = 0;
          std::memcpy(&v, ptr, sizeof(uint32_t));
          return v;
        }
        default:
          return 0;
      }
    }

    static bool buildMeshData(const scgltf::Document& doc,
                              const scgltf::Mesh& src,
                              MeshData* out_mesh,
                              std::string* out_error)
    {
      if (!out_mesh)
        return false;

      out_mesh->vertices.clear();
      out_mesh->indices.clear();
      out_mesh->submeshes.clear();
      out_mesh->vertexLayoutFlags = VertexLayout_Position;

      for (const scgltf::Primitive& prim : src.primitives)
      {
        if (prim.position < 0)
          continue;

        AccessorView pos{};
        if (!resolveAccessor(doc, prim.position, &pos, out_error))
          return false;
        AccessorView norm{};
        AccessorView uv{};
        const bool hasNorm = (prim.normal >= 0) && resolveAccessor(doc, prim.normal, &norm, nullptr);
        const bool hasUv = (prim.texcoord0 >= 0) && resolveAccessor(doc, prim.texcoord0, &uv, nullptr);

        const size_t baseVertex = out_mesh->vertices.size();
        out_mesh->vertices.reserve(out_mesh->vertices.size() + pos.count);
        for (size_t i = 0; i < pos.count; ++i)
        {
          const uint8_t* p = pos.data + pos.stride * i;
          MeshVertex v{};
          for (int c = 0; c < 3; ++c)
            v.pos[c] = readComponentAsFloat(p + componentSize(pos.componentType) * c,
                                            pos.componentType, pos.normalized);

          if (hasNorm && norm.count > 0)
          {
            const uint8_t* n = norm.data + norm.stride * i;
            for (int c = 0; c < 3; ++c)
              v.normal[c] = readComponentAsFloat(n + componentSize(norm.componentType) * c,
                                                 norm.componentType, norm.normalized);
            out_mesh->vertexLayoutFlags |= VertexLayout_Normal;
          }

          if (hasUv && uv.count > 0)
          {
            const uint8_t* t = uv.data + uv.stride * i;
            for (int c = 0; c < 2; ++c)
              v.uv0[c] = readComponentAsFloat(t + componentSize(uv.componentType) * c,
                                              uv.componentType, uv.normalized);
            out_mesh->vertexLayoutFlags |= VertexLayout_UV0;
          }
          out_mesh->vertices.push_back(v);
        }

        const uint32_t indexOffset = static_cast<uint32_t>(out_mesh->indices.size());
        if (prim.indices >= 0)
        {
          AccessorView idx{};
          if (!resolveAccessor(doc, prim.indices, &idx, out_error))
            return false;
          out_mesh->indices.reserve(out_mesh->indices.size() + idx.count);
          for (size_t i = 0; i < idx.count; ++i)
          {
            const uint8_t* ip = idx.data + idx.stride * i;
            const uint32_t index = readIndex(ip, idx.componentType);
            out_mesh->indices.push_back(static_cast<uint32_t>(baseVertex) + index);
          }
        }
        else
        {
          out_mesh->indices.reserve(out_mesh->indices.size() + pos.count);
          for (uint32_t i = 0; i < pos.count; ++i)
            out_mesh->indices.push_back(static_cast<uint32_t>(baseVertex) + i);
        }

        Submesh sm{};
        sm.indexOffset = indexOffset;
        sm.indexCount = static_cast<uint32_t>(out_mesh->indices.size() - indexOffset);
        sm.materialIndex = prim.material;
        out_mesh->submeshes.push_back(sm);
      }

      if (!out_mesh->vertices.empty())
        ComputeMeshBounds(out_mesh);
      return true;
    }

    static void mat4_identity(float m[16])
    {
      std::memset(m, 0, sizeof(float) * 16);
      m[0] = 1.0f;
      m[5] = 1.0f;
      m[10] = 1.0f;
      m[15] = 1.0f;
    }

    static void mat4_mul(const float a[16], const float b[16], float out[16])
    {
      for (int row = 0; row < 4; ++row)
      {
        for (int col = 0; col < 4; ++col)
        {
          out[col * 4 + row] =
            a[0 * 4 + row] * b[col * 4 + 0] +
            a[1 * 4 + row] * b[col * 4 + 1] +
            a[2 * 4 + row] * b[col * 4 + 2] +
            a[3 * 4 + row] * b[col * 4 + 3];
        }
      }
    }

    static void mat4_from_trs(const float t[3], const float r[4], const float s[3], float out[16])
    {
      const float x = r[0], y = r[1], z = r[2], w = r[3];
      const float xx = x * x;
      const float yy = y * y;
      const float zz = z * z;
      const float xy = x * y;
      const float xz = x * z;
      const float yz = y * z;
      const float wx = w * x;
      const float wy = w * y;
      const float wz = w * z;

      float rot[16]{};
      rot[0] = 1.0f - 2.0f * (yy + zz);
      rot[1] = 2.0f * (xy + wz);
      rot[2] = 2.0f * (xz - wy);
      rot[3] = 0.0f;

      rot[4] = 2.0f * (xy - wz);
      rot[5] = 1.0f - 2.0f * (xx + zz);
      rot[6] = 2.0f * (yz + wx);
      rot[7] = 0.0f;

      rot[8] = 2.0f * (xz + wy);
      rot[9] = 2.0f * (yz - wx);
      rot[10] = 1.0f - 2.0f * (xx + yy);
      rot[11] = 0.0f;

      rot[12] = 0.0f;
      rot[13] = 0.0f;
      rot[14] = 0.0f;
      rot[15] = 1.0f;

      float scale[16]{};
      mat4_identity(scale);
      scale[0] = s[0];
      scale[5] = s[1];
      scale[10] = s[2];

      float rs[16]{};
      mat4_mul(rot, scale, rs);

      float trans[16]{};
      mat4_identity(trans);
      trans[12] = t[0];
      trans[13] = t[1];
      trans[14] = t[2];

      mat4_mul(trans, rs, out);
    }

    static void buildLocalMatrix(const scgltf::Node& node, float out[16])
    {
      if (node.hasMatrix)
      {
        std::memcpy(out, node.matrix, sizeof(float) * 16);
        return;
      }
      mat4_from_trs(node.translation, node.rotation, node.scale, out);
    }

    static bool readFileBytes(const char* path, std::vector<uint8_t>& out)
    {
      std::ifstream file(path, std::ios::binary | std::ios::ate);
      if (!file.is_open())
        return false;
      const std::streamsize size = file.tellg();
      if (size <= 0)
        return false;
      out.resize(static_cast<size_t>(size));
      file.seekg(0, std::ios::beg);
      if (!file.read(reinterpret_cast<char*>(out.data()), size))
        return false;
      return true;
    }
  }

  class GlbImporter final : public IMeshImporter
  {
  public:
    bool canImportExtension(const char* extension) const override
    {
      if (!extension)
        return false;
      return std::strcmp(extension, ".glb") == 0;
    }

    bool importFile(const char* absPath,
                    const MeshImportOptions& options,
                    ImportedModel* out_model,
                    std::string* out_error) override
    {
      if (!absPath || !out_model)
        return false;
      (void)options;

      std::vector<uint8_t> bytes;
      if (!readFileBytes(absPath, bytes))
      {
        if (out_error) *out_error = "Failed to read GLB file.";
        return false;
      }

      scgltf::Document doc{};
      if (!scgltf::parseGlb(bytes.data(), bytes.size(), &doc, out_error))
        return false;

      ImportedModel model{};
      model.meshes.reserve(doc.meshes.size());
      for (const scgltf::Mesh& m : doc.meshes)
      {
        ImportedMesh mesh{};
        mesh.name = m.name;
        if (!buildMeshData(doc, m, &mesh.mesh, out_error))
          return false;
        model.meshes.push_back(std::move(mesh));
      }

      model.materials.reserve(doc.materials.size());
      for (const scgltf::Material& mat : doc.materials)
      {
        ImportedMaterial outMat{};
        outMat.name = mat.name;
        if (mat.baseColorTexture >= 0 && mat.baseColorTexture < static_cast<int>(doc.textures.size()))
        {
          const scgltf::Texture& tex = doc.textures[mat.baseColorTexture];
          if (tex.source >= 0 && tex.source < static_cast<int>(doc.images.size()))
          {
            const scgltf::Image& img = doc.images[tex.source];
            outMat.baseColorTexture = img.uri;
            outMat.baseColorTextureEmbedded = img.uri.empty() && img.bufferView >= 0;
          }
        }
        model.materials.push_back(std::move(outMat));
      }

      model.nodes.reserve(doc.nodes.size());
      for (const scgltf::Node& n : doc.nodes)
      {
        ImportedNode node{};
        node.name = n.name;
        node.meshIndex = n.mesh;
        node.children = n.children;
        buildLocalMatrix(n, node.localMatrix);
        model.nodes.push_back(std::move(node));
      }

      for (size_t i = 0; i < model.nodes.size(); ++i)
      {
        for (int child : model.nodes[i].children)
        {
          if (child >= 0 && child < static_cast<int>(model.nodes.size()))
            model.nodes[child].parent = static_cast<int>(i);
        }
      }

      if (!doc.scenes.empty())
      {
        const int sceneIndex = (doc.defaultScene >= 0 && doc.defaultScene < static_cast<int>(doc.scenes.size()))
          ? doc.defaultScene
          : 0;
        for (int root : doc.scenes[sceneIndex].nodes)
          model.sceneRoots.push_back(root);
      }

      *out_model = std::move(model);
      return true;
    }
  };
}

namespace sc_import
{
  void RegisterGlbImporter(ImporterRegistry* registry)
  {
    if (!registry)
      return;
    registry->registerImporter(std::make_unique<GlbImporter>());
  }
}
