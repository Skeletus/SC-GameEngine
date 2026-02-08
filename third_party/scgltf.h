#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// Minimal GLB (glTF 2.0) reader for editor-side import.
// This is intentionally small and focused: GLB-only, static meshes,
// positions/normals/UV0/indices, nodes, scenes, and basic materials.

namespace scgltf
{
  struct BufferView
  {
    int buffer = -1;
    size_t byteOffset = 0;
    size_t byteLength = 0;
    size_t byteStride = 0;
  };

  struct Accessor
  {
    int bufferView = -1;
    size_t byteOffset = 0;
    size_t count = 0;
    int componentType = 0;
    int components = 0;
    bool normalized = false;
  };

  struct Primitive
  {
    int indices = -1;
    int position = -1;
    int normal = -1;
    int texcoord0 = -1;
    int material = -1;
  };

  struct Mesh
  {
    std::string name;
    std::vector<Primitive> primitives;
  };

  struct Node
  {
    std::string name;
    int mesh = -1;
    std::vector<int> children;
    bool hasMatrix = false;
    float matrix[16]{};
    float translation[3]{ 0.0f, 0.0f, 0.0f };
    float rotation[4]{ 0.0f, 0.0f, 0.0f, 1.0f };
    float scale[3]{ 1.0f, 1.0f, 1.0f };
  };

  struct Scene
  {
    std::vector<int> nodes;
  };

  struct Image
  {
    std::string uri;
    int bufferView = -1;
    std::string mimeType;
  };

  struct Texture
  {
    int source = -1;
  };

  struct Material
  {
    std::string name;
    int baseColorTexture = -1;
  };

  struct Document
  {
    std::vector<std::vector<uint8_t>> buffers;
    std::vector<BufferView> bufferViews;
    std::vector<Accessor> accessors;
    std::vector<Mesh> meshes;
    std::vector<Node> nodes;
    std::vector<Scene> scenes;
    int defaultScene = -1;
    std::vector<Image> images;
    std::vector<Texture> textures;
    std::vector<Material> materials;
  };

  bool parseGlb(const uint8_t* data, size_t size, Document* out_doc, std::string* out_error);
}

#ifdef SCGLTF_IMPLEMENTATION

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>

namespace scgltf
{
  namespace
  {
    struct JsonValue;

    struct JsonValue
    {
      enum class Type
      {
        Null,
        Bool,
        Number,
        String,
        Array,
        Object
      };

      Type type = Type::Null;
      bool boolean = false;
      double number = 0.0;
      std::string string;
      std::vector<JsonValue> array;
      std::vector<std::pair<std::string, JsonValue>> object;
    };

    class JsonParser
    {
    public:
      JsonParser(const char* begin, const char* end)
        : m_ptr(begin), m_end(end) {}

      bool parse(JsonValue& out, std::string* err)
      {
        skipWs();
        if (!parseValue(out, err))
          return false;
        skipWs();
        if (m_ptr != m_end)
        {
          if (err) *err = "Trailing data in JSON.";
          return false;
        }
        return true;
      }

    private:
      const char* m_ptr = nullptr;
      const char* m_end = nullptr;

      void skipWs()
      {
        while (m_ptr < m_end && std::isspace(static_cast<unsigned char>(*m_ptr)))
          ++m_ptr;
      }

      bool match(const char* lit)
      {
        const size_t len = std::strlen(lit);
        if (static_cast<size_t>(m_end - m_ptr) < len)
          return false;
        if (std::strncmp(m_ptr, lit, len) != 0)
          return false;
        m_ptr += len;
        return true;
      }

      static bool parseHex4(const char* s, uint32_t& out)
      {
        out = 0;
        for (int i = 0; i < 4; ++i)
        {
          const char c = s[i];
          uint32_t v = 0;
          if (c >= '0' && c <= '9') v = static_cast<uint32_t>(c - '0');
          else if (c >= 'a' && c <= 'f') v = static_cast<uint32_t>(c - 'a' + 10);
          else if (c >= 'A' && c <= 'F') v = static_cast<uint32_t>(c - 'A' + 10);
          else return false;
          out = (out << 4) | v;
        }
        return true;
      }

      bool parseString(std::string& out, std::string* err)
      {
        if (*m_ptr != '"')
          return false;
        ++m_ptr;
        std::string result;
        while (m_ptr < m_end)
        {
          char c = *m_ptr++;
          if (c == '"')
          {
            out = std::move(result);
            return true;
          }
          if (c == '\\')
          {
            if (m_ptr >= m_end)
              break;
            char esc = *m_ptr++;
            switch (esc)
            {
              case '"': result.push_back('"'); break;
              case '\\': result.push_back('\\'); break;
              case '/': result.push_back('/'); break;
              case 'b': result.push_back('\b'); break;
              case 'f': result.push_back('\f'); break;
              case 'n': result.push_back('\n'); break;
              case 'r': result.push_back('\r'); break;
              case 't': result.push_back('\t'); break;
              case 'u':
              {
                if (m_end - m_ptr < 4)
                  break;
                uint32_t code = 0;
                if (parseHex4(m_ptr, code))
                {
                  if (code <= 0x7Fu)
                    result.push_back(static_cast<char>(code));
                  else
                    result.push_back('?');
                }
                m_ptr += 4;
                break;
              }
              default:
                result.push_back(esc);
                break;
            }
          }
          else
          {
            result.push_back(c);
          }
        }
        if (err) *err = "Unterminated string.";
        return false;
      }

      bool parseNumber(double& out)
      {
        const char* start = m_ptr;
        if (m_ptr < m_end && (*m_ptr == '-' || *m_ptr == '+'))
          ++m_ptr;
        while (m_ptr < m_end && std::isdigit(static_cast<unsigned char>(*m_ptr)))
          ++m_ptr;
        if (m_ptr < m_end && *m_ptr == '.')
        {
          ++m_ptr;
          while (m_ptr < m_end && std::isdigit(static_cast<unsigned char>(*m_ptr)))
            ++m_ptr;
        }
        if (m_ptr < m_end && (*m_ptr == 'e' || *m_ptr == 'E'))
        {
          ++m_ptr;
          if (m_ptr < m_end && (*m_ptr == '-' || *m_ptr == '+'))
            ++m_ptr;
          while (m_ptr < m_end && std::isdigit(static_cast<unsigned char>(*m_ptr)))
            ++m_ptr;
        }
        if (start == m_ptr)
          return false;

        char buf[128]{};
        const size_t len = static_cast<size_t>(m_ptr - start);
        if (len >= sizeof(buf))
          return false;
        std::memcpy(buf, start, len);
        buf[len] = '\0';
        out = std::strtod(buf, nullptr);
        return true;
      }

      bool parseValue(JsonValue& out, std::string* err)
      {
        skipWs();
        if (m_ptr >= m_end)
          return false;
        const char c = *m_ptr;
        if (c == '{')
          return parseObject(out, err);
        if (c == '[')
          return parseArray(out, err);
        if (c == '"')
        {
          out.type = JsonValue::Type::String;
          return parseString(out.string, err);
        }
        if (c == 't')
        {
          if (match("true"))
          {
            out.type = JsonValue::Type::Bool;
            out.boolean = true;
            return true;
          }
          return false;
        }
        if (c == 'f')
        {
          if (match("false"))
          {
            out.type = JsonValue::Type::Bool;
            out.boolean = false;
            return true;
          }
          return false;
        }
        if (c == 'n')
        {
          if (match("null"))
          {
            out.type = JsonValue::Type::Null;
            return true;
          }
          return false;
        }
        out.type = JsonValue::Type::Number;
        if (!parseNumber(out.number))
        {
          if (err) *err = "Invalid number.";
          return false;
        }
        return true;
      }

      bool parseArray(JsonValue& out, std::string* err)
      {
        if (*m_ptr != '[')
          return false;
        ++m_ptr;
        out.type = JsonValue::Type::Array;
        skipWs();
        if (m_ptr < m_end && *m_ptr == ']')
        {
          ++m_ptr;
          return true;
        }
        while (m_ptr < m_end)
        {
          JsonValue item;
          if (!parseValue(item, err))
            return false;
          out.array.push_back(std::move(item));
          skipWs();
          if (m_ptr < m_end && *m_ptr == ',')
          {
            ++m_ptr;
            continue;
          }
          if (m_ptr < m_end && *m_ptr == ']')
          {
            ++m_ptr;
            return true;
          }
          break;
        }
        if (err) *err = "Unterminated array.";
        return false;
      }

      bool parseObject(JsonValue& out, std::string* err)
      {
        if (*m_ptr != '{')
          return false;
        ++m_ptr;
        out.type = JsonValue::Type::Object;
        skipWs();
        if (m_ptr < m_end && *m_ptr == '}')
        {
          ++m_ptr;
          return true;
        }
        while (m_ptr < m_end)
        {
          skipWs();
          if (m_ptr >= m_end || *m_ptr != '"')
            break;
          JsonValue key;
          if (!parseString(key.string, err))
            return false;
          skipWs();
          if (m_ptr >= m_end || *m_ptr != ':')
            break;
          ++m_ptr;
          JsonValue value;
          if (!parseValue(value, err))
            return false;
          out.object.push_back({ std::move(key.string), std::move(value) });
          skipWs();
          if (m_ptr < m_end && *m_ptr == ',')
          {
            ++m_ptr;
            continue;
          }
          if (m_ptr < m_end && *m_ptr == '}')
          {
            ++m_ptr;
            return true;
          }
          break;
        }
        if (err) *err = "Unterminated object.";
        return false;
      }
    };

    static const JsonValue* findMember(const JsonValue& obj, const char* key)
    {
      if (obj.type != JsonValue::Type::Object)
        return nullptr;
      for (const auto& m : obj.object)
      {
        if (m.first == key)
          return &m.second;
      }
      return nullptr;
    }

    static int readInt(const JsonValue& v, int defaultValue = 0)
    {
      if (v.type == JsonValue::Type::Number)
        return static_cast<int>(v.number);
      if (v.type == JsonValue::Type::Bool)
        return v.boolean ? 1 : 0;
      return defaultValue;
    }

    static float readFloat(const JsonValue& v, float defaultValue = 0.0f)
    {
      if (v.type == JsonValue::Type::Number)
        return static_cast<float>(v.number);
      if (v.type == JsonValue::Type::Bool)
        return v.boolean ? 1.0f : 0.0f;
      return defaultValue;
    }

    static std::string readString(const JsonValue& v)
    {
      if (v.type == JsonValue::Type::String)
        return v.string;
      return {};
    }

    static bool isArray(const JsonValue* v)
    {
      return v && v->type == JsonValue::Type::Array;
    }

    static bool isObject(const JsonValue* v)
    {
      return v && v->type == JsonValue::Type::Object;
    }

    static int componentsFromType(const std::string& type)
    {
      if (type == "SCALAR") return 1;
      if (type == "VEC2") return 2;
      if (type == "VEC3") return 3;
      if (type == "VEC4") return 4;
      if (type == "MAT4") return 16;
      return 0;
    }

    static bool parseGlbChunks(const uint8_t* data,
                               size_t size,
                               const char** out_json,
                               size_t* out_json_size,
                               const uint8_t** out_bin,
                               size_t* out_bin_size,
                               std::string* out_error)
    {
      if (size < 12)
      {
        if (out_error) *out_error = "GLB data too small.";
        return false;
      }

      auto readU32 = [&](size_t offset) -> uint32_t
      {
        uint32_t v = 0;
        std::memcpy(&v, data + offset, sizeof(uint32_t));
        return v;
      };

      const uint32_t magic = readU32(0);
      const uint32_t version = readU32(4);
      const uint32_t length = readU32(8);
      if (magic != 0x46546C67u)
      {
        if (out_error) *out_error = "Invalid GLB magic.";
        return false;
      }
      if (version != 2u)
      {
        if (out_error) *out_error = "Unsupported GLB version.";
        return false;
      }
      if (length > size)
      {
        if (out_error) *out_error = "GLB length exceeds buffer.";
        return false;
      }

      size_t offset = 12;
      const char* json = nullptr;
      size_t jsonSize = 0;
      const uint8_t* bin = nullptr;
      size_t binSize = 0;

      while (offset + 8 <= size)
      {
        uint32_t chunkLen = readU32(offset);
        uint32_t chunkType = readU32(offset + 4);
        offset += 8;
        if (offset + chunkLen > size)
          break;

        if (chunkType == 0x4E4F534Au) // JSON
        {
          json = reinterpret_cast<const char*>(data + offset);
          jsonSize = chunkLen;
        }
        else if (chunkType == 0x004E4942u) // BIN
        {
          bin = data + offset;
          binSize = chunkLen;
        }
        offset += chunkLen;
      }

      if (!json || jsonSize == 0)
      {
        if (out_error) *out_error = "GLB missing JSON chunk.";
        return false;
      }

      *out_json = json;
      *out_json_size = jsonSize;
      *out_bin = bin;
      *out_bin_size = binSize;
      return true;
    }
  }

  bool parseGlb(const uint8_t* data, size_t size, Document* out_doc, std::string* out_error)
  {
    if (!data || size == 0 || !out_doc)
    {
      if (out_error) *out_error = "Invalid arguments.";
      return false;
    }

    const char* json = nullptr;
    size_t jsonSize = 0;
    const uint8_t* bin = nullptr;
    size_t binSize = 0;
    if (!parseGlbChunks(data, size, &json, &jsonSize, &bin, &binSize, out_error))
      return false;

    JsonValue root;
    JsonParser parser(json, json + jsonSize);
    std::string parseErr;
    if (!parser.parse(root, &parseErr))
    {
      if (out_error) *out_error = parseErr;
      return false;
    }
    if (root.type != JsonValue::Type::Object)
    {
      if (out_error) *out_error = "Root JSON is not an object.";
      return false;
    }

    Document doc{};
    if (bin && binSize > 0)
      doc.buffers.push_back(std::vector<uint8_t>(bin, bin + binSize));

    if (const JsonValue* buffers = findMember(root, "buffers"); isArray(buffers))
    {
      for (const JsonValue& buf : buffers->array)
      {
        if (!isObject(&buf))
          continue;
        if (const JsonValue* uri = findMember(buf, "uri"))
        {
          (void)uri;
          // External buffers are not supported in GLB-first reader.
        }
        if (doc.buffers.empty())
          doc.buffers.emplace_back();
      }
    }

    if (const JsonValue* views = findMember(root, "bufferViews"); isArray(views))
    {
      doc.bufferViews.reserve(views->array.size());
      for (const JsonValue& v : views->array)
      {
        if (!isObject(&v))
          continue;
        BufferView view{};
        if (const JsonValue* buffer = findMember(v, "buffer"))
          view.buffer = readInt(*buffer, -1);
        if (const JsonValue* off = findMember(v, "byteOffset"))
          view.byteOffset = static_cast<size_t>(readInt(*off, 0));
        if (const JsonValue* len = findMember(v, "byteLength"))
          view.byteLength = static_cast<size_t>(readInt(*len, 0));
        if (const JsonValue* stride = findMember(v, "byteStride"))
          view.byteStride = static_cast<size_t>(readInt(*stride, 0));
        doc.bufferViews.push_back(view);
      }
    }

    if (const JsonValue* accs = findMember(root, "accessors"); isArray(accs))
    {
      doc.accessors.reserve(accs->array.size());
      for (const JsonValue& a : accs->array)
      {
        if (!isObject(&a))
          continue;
        Accessor acc{};
        if (const JsonValue* bv = findMember(a, "bufferView"))
          acc.bufferView = readInt(*bv, -1);
        if (const JsonValue* off = findMember(a, "byteOffset"))
          acc.byteOffset = static_cast<size_t>(readInt(*off, 0));
        if (const JsonValue* cnt = findMember(a, "count"))
          acc.count = static_cast<size_t>(readInt(*cnt, 0));
        if (const JsonValue* comp = findMember(a, "componentType"))
          acc.componentType = readInt(*comp, 0);
        if (const JsonValue* norm = findMember(a, "normalized"))
          acc.normalized = norm->type == JsonValue::Type::Bool ? norm->boolean : false;
        if (const JsonValue* type = findMember(a, "type"))
          acc.components = componentsFromType(readString(*type));
        doc.accessors.push_back(acc);
      }
    }

    if (const JsonValue* images = findMember(root, "images"); isArray(images))
    {
      doc.images.reserve(images->array.size());
      for (const JsonValue& img : images->array)
      {
        if (!isObject(&img))
          continue;
        Image out{};
        if (const JsonValue* uri = findMember(img, "uri"))
          out.uri = readString(*uri);
        if (const JsonValue* bv = findMember(img, "bufferView"))
          out.bufferView = readInt(*bv, -1);
        if (const JsonValue* mime = findMember(img, "mimeType"))
          out.mimeType = readString(*mime);
        doc.images.push_back(std::move(out));
      }
    }

    if (const JsonValue* textures = findMember(root, "textures"); isArray(textures))
    {
      doc.textures.reserve(textures->array.size());
      for (const JsonValue& tex : textures->array)
      {
        if (!isObject(&tex))
          continue;
        Texture t{};
        if (const JsonValue* src = findMember(tex, "source"))
          t.source = readInt(*src, -1);
        doc.textures.push_back(t);
      }
    }

    if (const JsonValue* materials = findMember(root, "materials"); isArray(materials))
    {
      doc.materials.reserve(materials->array.size());
      for (const JsonValue& mat : materials->array)
      {
        if (!isObject(&mat))
          continue;
        Material m{};
        if (const JsonValue* name = findMember(mat, "name"))
          m.name = readString(*name);
        if (const JsonValue* pbr = findMember(mat, "pbrMetallicRoughness"); isObject(pbr))
        {
          if (const JsonValue* baseTex = findMember(*pbr, "baseColorTexture"); isObject(baseTex))
          {
            if (const JsonValue* idx = findMember(*baseTex, "index"))
              m.baseColorTexture = readInt(*idx, -1);
          }
        }
        doc.materials.push_back(std::move(m));
      }
    }

    if (const JsonValue* meshes = findMember(root, "meshes"); isArray(meshes))
    {
      doc.meshes.reserve(meshes->array.size());
      for (const JsonValue& mesh : meshes->array)
      {
        if (!isObject(&mesh))
          continue;
        Mesh m{};
        if (const JsonValue* name = findMember(mesh, "name"))
          m.name = readString(*name);
        if (const JsonValue* prims = findMember(mesh, "primitives"); isArray(prims))
        {
          for (const JsonValue& p : prims->array)
          {
            if (!isObject(&p))
              continue;
            Primitive prim{};
            if (const JsonValue* idx = findMember(p, "indices"))
              prim.indices = readInt(*idx, -1);
            if (const JsonValue* matIdx = findMember(p, "material"))
              prim.material = readInt(*matIdx, -1);
            if (const JsonValue* attrs = findMember(p, "attributes"); isObject(attrs))
            {
              if (const JsonValue* pos = findMember(*attrs, "POSITION"))
                prim.position = readInt(*pos, -1);
              if (const JsonValue* norm = findMember(*attrs, "NORMAL"))
                prim.normal = readInt(*norm, -1);
              if (const JsonValue* uv0 = findMember(*attrs, "TEXCOORD_0"))
                prim.texcoord0 = readInt(*uv0, -1);
            }
            m.primitives.push_back(prim);
          }
        }
        doc.meshes.push_back(std::move(m));
      }
    }

    if (const JsonValue* nodes = findMember(root, "nodes"); isArray(nodes))
    {
      doc.nodes.reserve(nodes->array.size());
      for (const JsonValue& n : nodes->array)
      {
        if (!isObject(&n))
          continue;
        Node node{};
        if (const JsonValue* name = findMember(n, "name"))
          node.name = readString(*name);
        if (const JsonValue* mesh = findMember(n, "mesh"))
          node.mesh = readInt(*mesh, -1);
        if (const JsonValue* children = findMember(n, "children"); isArray(children))
        {
          node.children.reserve(children->array.size());
          for (const JsonValue& c : children->array)
          {
            if (c.type == JsonValue::Type::Number)
              node.children.push_back(readInt(c, -1));
          }
        }
        if (const JsonValue* mat = findMember(n, "matrix"); isArray(mat))
        {
          if (mat->array.size() == 16)
          {
            node.hasMatrix = true;
            for (size_t i = 0; i < 16; ++i)
              node.matrix[i] = readFloat(mat->array[i], 0.0f);
          }
        }
        if (const JsonValue* t = findMember(n, "translation"); isArray(t) && t->array.size() == 3)
        {
          node.translation[0] = readFloat(t->array[0], 0.0f);
          node.translation[1] = readFloat(t->array[1], 0.0f);
          node.translation[2] = readFloat(t->array[2], 0.0f);
        }
        if (const JsonValue* r = findMember(n, "rotation"); isArray(r) && r->array.size() == 4)
        {
          node.rotation[0] = readFloat(r->array[0], 0.0f);
          node.rotation[1] = readFloat(r->array[1], 0.0f);
          node.rotation[2] = readFloat(r->array[2], 0.0f);
          node.rotation[3] = readFloat(r->array[3], 1.0f);
        }
        if (const JsonValue* s = findMember(n, "scale"); isArray(s) && s->array.size() == 3)
        {
          node.scale[0] = readFloat(s->array[0], 1.0f);
          node.scale[1] = readFloat(s->array[1], 1.0f);
          node.scale[2] = readFloat(s->array[2], 1.0f);
        }
        doc.nodes.push_back(std::move(node));
      }
    }

    if (const JsonValue* scenes = findMember(root, "scenes"); isArray(scenes))
    {
      doc.scenes.reserve(scenes->array.size());
      for (const JsonValue& s : scenes->array)
      {
        if (!isObject(&s))
          continue;
        Scene scene{};
        if (const JsonValue* nodes = findMember(s, "nodes"); isArray(nodes))
        {
          scene.nodes.reserve(nodes->array.size());
          for (const JsonValue& n : nodes->array)
          {
            if (n.type == JsonValue::Type::Number)
              scene.nodes.push_back(readInt(n, -1));
          }
        }
        doc.scenes.push_back(std::move(scene));
      }
    }

    if (const JsonValue* scene = findMember(root, "scene"))
      doc.defaultScene = readInt(*scene, -1);

    *out_doc = std::move(doc);
    return true;
  }
}

#endif // SCGLTF_IMPLEMENTATION
