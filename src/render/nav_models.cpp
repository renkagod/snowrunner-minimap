#include "render/nav_models.h"

#include "core/log.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <format>

namespace render {

namespace {

constexpr float kCellSize = 64.0f; // metres per placement grid cell
constexpr int kMaxCells = 256;     // per axis
constexpr unsigned kVertexStride = 28;

// A CombineXMesh file: a u32-prefixed XML material header, then a header (magic, node count, bounds,
// mesh count) and the nodes in order. A node is u16 parent (0xfffe for none), u16 index, u32 "rendered",
// a u32-prefixed name and its row-vector 4x4 transform relative to the parent. A mesh follows its
// node: vertex and triangle counts, names, its subsets (material ranges), a D3DVERTEXELEMENT9
// declaration closed by D3DDECL_END, then the vertex and index data exactly as the GPU takes them
// (indices count from the mesh's first vertex across all subsets).
constexpr uint32_t kMeshMagic = 0x0133EF63;
constexpr size_t kNodeSearch = 128; // a node starts at most this far after the previous one's data

// One node with geometry, its vertices baked into model space.
struct PartData {
    std::string node;
    std::vector<unsigned char> vertices;
    const unsigned char* indices = nullptr;
    unsigned vertexCount = 0;
    unsigned indexCount = 0;
    bool wideIndices = false;
};

struct MeshData {
    std::string xml;
    std::vector<PartData> parts;
};

using Matrix = std::array<float, 16>; // row vectors: point * matrix, translation in row 3

uint32_t U32(const std::vector<unsigned char>& d, size_t at)
{
    uint32_t v;
    std::memcpy(&v, d.data() + at, 4);
    return v;
}

uint16_t U16(const std::vector<unsigned char>& d, size_t at)
{
    uint16_t v;
    std::memcpy(&v, d.data() + at, 2);
    return v;
}

Matrix Multiply(const Matrix& a, const Matrix& b)
{
    Matrix r{};
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            for (int k = 0; k < 4; ++k)
                r[i * 4 + j] += a[i * 4 + k] * b[k * 4 + j];
    return r;
}

bool IsIdentity(const Matrix& m)
{
    for (int i = 0; i < 16; ++i) {
        if (std::fabs(m[i] - (i % 5 == 0 ? 1.0f : 0.0f)) > 1e-6f)
            return false;
    }
    return true;
}

// Rotates an RGBA8 direction (xyz biased by 0.502 and halved) by the matrix's upper 3x3.
void RotateDirection(unsigned char* rgba, const Matrix& m)
{
    float d[3], r[3];
    for (int i = 0; i < 3; ++i)
        d[i] = (rgba[i] / 255.0f - 0.50196f) * 2.0f;
    for (int j = 0; j < 3; ++j)
        r[j] = d[0] * m[j] + d[1] * m[4 + j] + d[2] * m[8 + j];
    const float len = std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
    if (len < 1e-6f)
        return;
    for (int i = 0; i < 3; ++i)
        rgba[i] = static_cast<unsigned char>(std::clamp(std::lround((r[i] / len * 0.5f + 0.50196f) * 255.0f), 0L, 255L));
}

void BakeVertices(const unsigned char* src, unsigned count, const Matrix& m, std::vector<unsigned char>& out)
{
    out.assign(src, src + size_t(count) * kVertexStride);
    if (IsIdentity(m))
        return;
    for (size_t v = 0; v < count; ++v) {
        unsigned char* vertex = out.data() + v * kVertexStride;
        float p[3], q[3];
        std::memcpy(p, vertex, sizeof(p));
        for (int j = 0; j < 3; ++j)
            q[j] = p[0] * m[j] + p[1] * m[4 + j] + p[2] * m[8 + j] + m[12 + j];
        std::memcpy(vertex, q, sizeof(q));
        RotateDirection(vertex + 20, m);
    }
}

// Node header with the given index at `at`, or false.
bool NodeAt(const std::vector<unsigned char>& d, size_t at, unsigned index, std::string& name)
{
    if (at + 12 > d.size() || U16(d, at + 2) != index || U32(d, at + 4) > 1)
        return false;
    const uint32_t length = U32(d, at + 8);
    if (length < 2 || length > 256 || at + 12 + length + 64 > d.size() || d[at + 12 + length - 1] != 0)
        return false;
    const auto* chars = reinterpret_cast<const char*>(d.data() + at + 12);
    if (!std::all_of(chars, chars + length - 1, [](char c) { return c >= 32 && c < 127; }))
        return false;
    name.assign(chars, length - 1);
    return true;
}

// Bytes of a D3DDECLTYPE, 0 for unknown ones.
unsigned DeclTypeSize(unsigned char type)
{
    static const unsigned char kSizes[] = {4, 8, 12, 16, 4, 4, 4, 8, 4, 4, 8, 4, 4, 4, 4, 4, 8};
    return type < sizeof(kSizes) ? kSizes[type] : 0;
}

// The mesh after a node's transform at `pos`; on success pos moves past its index data. Meshes in
// another vertex layout (collision shapes, positions only) leave vertices null.
bool ParseNodeMesh(const std::vector<unsigned char>& d, size_t& pos, PartData& part, const unsigned char*& vertices)
{
    if (pos + 8 > d.size())
        return false;
    const uint32_t vertexCount = U32(d, pos), triangleCount = U32(d, pos + 4);
    static const unsigned char kDeclEnd[8] = {0xFF, 0, 0, 0, 0x11, 0, 0, 0};
    const auto end = std::search(d.begin() + static_cast<ptrdiff_t>(pos), d.end(), kDeclEnd, kDeclEnd + 8);
    if (end == d.end())
        return false;
    const size_t declEnd = static_cast<size_t>(end - d.begin());
    // The declaration is preceded by its element count, and that by the subsets: a u32 count, then
    // per subset material, first triangle, triangle count, first vertex and vertex count.
    size_t decl = 0;
    unsigned elements = 0;
    for (unsigned k = 1; k <= 16 && declEnd >= pos + 8 * k + 4; ++k) {
        if (U32(d, declEnd - 8 * k - 4) == k) {
            decl = declEnd - 8 * k;
            elements = k;
            break;
        }
    }
    // Collision meshes have no subsets.
    bool subsetsMatch = false;
    for (unsigned count = 0; decl && count <= 64 && decl >= pos + 8 + 20 * count; ++count) {
        const size_t at = decl - 8 - 20 * count;
        if (U32(d, at) != count)
            continue;
        if (!count) {
            subsetsMatch = true;
            break;
        }
        uint64_t vertexSum = 0, triangleSum = 0;
        for (unsigned s = 0; s < count; ++s) {
            triangleSum += U32(d, at + 4 + 20 * s + 8);
            vertexSum += U32(d, at + 4 + 20 * s + 16);
        }
        if (vertexSum == vertexCount && triangleSum == triangleCount) {
            subsetsMatch = true;
            break;
        }
    }
    if (!subsetsMatch)
        return false;
    unsigned stride = 0;
    for (unsigned e = 0; e < elements; ++e) {
        const unsigned size = DeclTypeSize(d[decl + 8 * e + 4]);
        if (!size || U16(d, decl + 8 * e) != 0)
            return false;
        stride = std::max(stride, U16(d, decl + 8 * e + 2) + size);
    }
    // Drawn layout: POSITION float3 @0, TEXCOORD0 float2 @12, TEXCOORD1 (normal) @20, TEXCOORD2 @24.
    const bool drawn = elements == 4 && stride == kVertexStride && d[decl + 4] == 2 && U16(d, decl + 2) == 0 &&
                       U16(d, decl + 8 + 2) == 12 && U16(d, decl + 16 + 2) == 20 && U16(d, decl + 24 + 2) == 24;
    part.vertexCount = vertexCount;
    part.indexCount = triangleCount * 3;
    part.wideIndices = vertexCount > 0xFFFF;
    const size_t vb = declEnd + 8;
    const size_t ib = vb + size_t(vertexCount) * stride;
    const size_t ibEnd = ib + size_t(part.indexCount) * (part.wideIndices ? 4 : 2);
    if (!vertexCount || !triangleCount || ibEnd > d.size())
        return false;
    vertices = drawn ? d.data() + vb : nullptr;
    part.indices = d.data() + ib;
    pos = ibEnd;
    return true;
}

bool ParseMesh(const std::vector<unsigned char>& d, MeshData& out)
{
    if (d.size() < 4 || 4 + size_t(U32(d, 0)) + 36 > d.size())
        return false;
    const size_t xmlEnd = 4 + U32(d, 0);
    out.xml.assign(reinterpret_cast<const char*>(d.data()) + 4, xmlEnd - 4);
    if (U32(d, xmlEnd) != kMeshMagic)
        return false;
    const uint32_t nodeCount = U32(d, xmlEnd + 4);
    if (nodeCount == 0 || nodeCount > 4096)
        return false;

    std::vector<Matrix> world(nodeCount);
    size_t pos = xmlEnd + 36;
    for (unsigned i = 0; i < nodeCount; ++i) {
        std::string name;
        size_t at = pos;
        while (at <= pos + kNodeSearch && !NodeAt(d, at, i, name))
            ++at;
        if (at > pos + kNodeSearch)
            return false;
        const uint16_t parent = U16(d, at);
        const bool rendered = U32(d, at + 4) != 0;
        const size_t matrixAt = at + 12 + U32(d, at + 8);
        Matrix local;
        std::memcpy(local.data(), d.data() + matrixAt, sizeof(float) * 16);
        world[i] = parent < i ? Multiply(local, world[parent]) : local;
        pos = matrixAt + 64;
        // Collision nodes are not flagged as rendered but still carry a mesh; a node without one is
        // followed within a few bytes by the next node.
        if (!rendered) {
            std::string next;
            bool nodeFollows = i + 1 == nodeCount;
            for (size_t gap = 0; gap <= 8 && !nodeFollows; ++gap)
                nodeFollows = NodeAt(d, pos + gap, i + 1, next);
            if (nodeFollows)
                continue;
        }
        PartData part;
        const unsigned char* vertices = nullptr;
        if (!ParseNodeMesh(d, pos, part, vertices))
            return false;
        if (!vertices)
            continue;
        part.node = std::move(name);
        BakeVertices(vertices, part.vertexCount, world[i], part.vertices);
        out.parts.push_back(std::move(part));
    }
    return !out.parts.empty();
}

std::string XmlAttribute(const std::string& xml, const char* name)
{
    const std::string key = std::string(name) + "=\"";
    const size_t at = xml.find(key);
    if (at == std::string::npos)
        return {};
    const size_t begin = at + key.size();
    const size_t end = xml.find('"', begin);
    return end == std::string::npos ? std::string() : xml.substr(begin, end - begin);
}

ID3D11Buffer* CreateBuffer(ID3D11Device* device, const void* data, size_t size, UINT bind)
{
    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = static_cast<UINT>(size);
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = bind;
    const D3D11_SUBRESOURCE_DATA init{data, 0, 0};
    ID3D11Buffer* buffer = nullptr;
    device->CreateBuffer(&desc, &init, &buffer);
    return buffer;
}

template <class T>
void SafeRelease(T*& p)
{
    if (p) {
        p->Release();
        p = nullptr;
    }
}

} // namespace

const NavModels::Mesh* NavModels::LoadMesh(ID3D11Device* device, const std::string& model)
{
    Mesh& mesh = m_meshes[model];
    if (mesh.loaded)
        return mesh.parts.empty() ? nullptr : &mesh;
    mesh.loaded = true;

    std::string entry = "[meshes]\\" + model;
    std::replace(entry.begin() + 9, entry.end(), '/', '_');
    std::vector<unsigned char> file;
    MeshData data;
    if (!m_pak->Read(entry, file) || !ParseMesh(file, data)) {
        logger::Warn("Map model '{}': {}", model, file.empty() ? "not in shared.pak" : "unexpected mesh layout");
        return nullptr;
    }

    // DiffuseMultiplier="g(220; 220; 220; 255)" is an sRGB colour; the shader gets it linear.
    const std::string diffuse = XmlAttribute(data.xml, "DiffuseMultiplier");
    int r, g, b;
    if (diffuse.starts_with("g(") && sscanf_s(diffuse.c_str() + 2, "%d; %d; %d", &r, &g, &b) == 3) {
        const int rgb[3] = {r, g, b};
        for (int i = 0; i < 3; ++i)
            mesh.colour[i] = std::pow(std::clamp(rgb[i], 0, 255) / 255.0f, 2.2f);
    }
    // Nearly all landmarks pick their colour from the shared palette, rocks from their own texture.
    mesh.albedo = LoadAlbedo(device, XmlAttribute(data.xml, "AlbedoMap"));
    mesh.colour[3] = mesh.albedo ? 1.0f : 0.0f;

    for (const PartData& source : data.parts) {
        Part part;
        part.node = source.node;
        part.vb = CreateBuffer(device, source.vertices.data(), source.vertices.size(), D3D11_BIND_VERTEX_BUFFER);
        part.ib = CreateBuffer(device, source.indices, size_t(source.indexCount) * (source.wideIndices ? 4 : 2),
                               D3D11_BIND_INDEX_BUFFER);
        part.indexFormat = source.wideIndices ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT;
        part.indexCount = source.indexCount;
        if (!part.vb || !part.ib) {
            SafeRelease(part.vb);
            SafeRelease(part.ib);
            for (Part& created : mesh.parts) {
                SafeRelease(created.vb);
                SafeRelease(created.ib);
            }
            mesh.parts.clear();
            return nullptr;
        }
        mesh.parts.push_back(std::move(part));
    }
    return &mesh;
}

// AlbedoMap="models/rock_03_lmk__d.tga" is [textures]\pct\models_rock_03_lmk__d.pct. The game samples
// these through sRGB views.
ID3D11ShaderResourceView* NavModels::LoadAlbedo(ID3D11Device* device, const std::string& albedoMap)
{
    if (albedoMap.empty())
        return nullptr;
    const auto found = m_albedos.find(albedoMap);
    if (found != m_albedos.end())
        return found->second;
    std::string name = albedoMap.substr(0, albedoMap.rfind('.'));
    std::replace(name.begin(), name.end(), '/', '_');
    ID3D11ShaderResourceView* srv =
        m_textures && m_textures->IsOpen() ? m_textures->LoadTexture(device, "[textures]\\pct\\" + name + ".pct", true)
                                           : nullptr;
    if (!srv)
        logger::Warn("Map model texture '{}' unreadable", albedoMap);
    m_albedos[albedoMap] = srv;
    return srv;
}

void NavModels::Build(ID3D11Device* device, const game::LevelNavData& data)
{
    m_groups.clear();
    SafeRelease(m_instances);
    m_capacity = 0;
    BuildObjects(device, data);

    // Placement grid over every instance of the level.
    float minX = 1e9f, minZ = 1e9f, maxX = -1e9f, maxZ = -1e9f;
    for (const auto& group : data.groups) {
        for (const auto& i : group.instances) {
            minX = std::min(minX, i.x);
            maxX = std::max(maxX, i.x);
            minZ = std::min(minZ, i.z);
            maxZ = std::max(maxZ, i.z);
        }
    }
    if (minX > maxX)
        return;
    m_originX = std::floor(minX);
    m_originZ = std::floor(minZ);
    m_cellsX = std::clamp(static_cast<int>((maxX - m_originX) / kCellSize) + 1, 1, kMaxCells);
    m_cellsZ = std::clamp(static_cast<int>((maxZ - m_originZ) / kCellSize) + 1, 1, kMaxCells);
    const auto cellOf = [&](const game::PackedInstance& i) {
        const int cx = std::clamp(static_cast<int>((i.x - m_originX) / kCellSize), 0, m_cellsX - 1);
        const int cz = std::clamp(static_cast<int>((i.z - m_originZ) / kCellSize), 0, m_cellsZ - 1);
        return static_cast<size_t>(cz) * m_cellsX + cx;
    };

    size_t missing = 0, total = 0;
    for (const auto& source : data.groups) {
        const Mesh* mesh = LoadMesh(device, source.model);
        if (!mesh) {
            missing += source.instances.size();
            continue;
        }
        Group group;
        group.mesh = mesh;
        // Counting sort by cell. The engine replaces w with the instance index; the shader shades
        // each instance a little differently from it.
        const size_t cells = static_cast<size_t>(m_cellsX) * m_cellsZ;
        group.cellStart.assign(cells + 1, 0);
        for (const auto& i : source.instances)
            ++group.cellStart[cellOf(i) + 1];
        for (size_t c = 0; c < cells; ++c)
            group.cellStart[c + 1] += group.cellStart[c];
        std::vector<unsigned> next(group.cellStart.begin(), group.cellStart.end() - 1);
        group.instances.resize(source.instances.size());
        for (size_t n = 0; n < source.instances.size(); ++n) {
            game::PackedInstance i = source.instances[n];
            i.w = static_cast<float>(n);
            group.instances[next[cellOf(i)]++] = i;
        }
        total += group.instances.size();
        m_groups.push_back(std::move(group));
    }

    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = static_cast<UINT>(std::max<size_t>(total, 1) * sizeof(game::PackedInstance));
    desc.Usage = D3D11_USAGE_DYNAMIC;
    desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (SUCCEEDED(device->CreateBuffer(&desc, nullptr, &m_instances)))
        m_capacity = std::max<size_t>(total, 1);

    size_t objects = 0;
    for (const ObjectGroup& group : m_objects)
        objects += group.objects.size();
    m_status = std::format("{} models, {} instances on {}x{} cells; {} instances without a mesh; {} single objects of "
                           "{} models",
                           m_groups.size(), total, m_cellsX, m_cellsZ, missing, objects, m_objects.size());
    logger::Info("Map models of '{}': {}", data.level, m_status);
}

// Single objects: each placement keeps the engine's full transform. Objects tied to a task show their
// unfinished look (the engine hides the "_complete" and "_navigation" nodes until the task is done);
// the others show every node.
void NavModels::BuildObjects(ID3D11Device* device, const game::LevelNavData& data)
{
    m_objects.clear();
    SafeRelease(m_objectBuffer);
    m_objectCapacity = 0;

    size_t total = 0, missing = 0;
    for (const auto& source : data.objectGroups) {
        const Mesh* mesh = LoadMesh(device, source.model);
        if (!mesh) {
            missing += source.objects.size();
            continue;
        }
        ObjectGroup groups[2]; // free-standing, tied to a task
        for (int objective = 0; objective < 2; ++objective) {
            groups[objective].mesh = mesh;
            for (const Part& part : mesh->parts) {
                if (!objective || (!part.node.ends_with("_complete") && !part.node.ends_with("_navigation")))
                    groups[objective].parts.push_back(&part);
            }
        }
        for (const game::NavObject& o : source.objects) {
            NavObjectInstance instance;
            for (int r = 0; r < 3; ++r)
                for (int c = 0; c < 4; ++c)
                    instance.rows[r][c] = o.world[c * 4 + r];
            groups[o.objective ? 1 : 0].objects.push_back(instance);
        }
        for (ObjectGroup& group : groups) {
            if (group.objects.empty() || group.parts.empty())
                continue;
            total += group.objects.size();
            m_objects.push_back(std::move(group));
        }
    }
    if (missing)
        logger::Warn("Map objects of '{}': {} without a mesh", data.level, missing);
    if (!total)
        return;

    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = static_cast<UINT>(total * sizeof(NavObjectInstance));
    desc.Usage = D3D11_USAGE_DYNAMIC;
    desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (SUCCEEDED(device->CreateBuffer(&desc, nullptr, &m_objectBuffer)))
        m_objectCapacity = total;
}

void NavModels::Update(ID3D11Device* device, const std::shared_ptr<const game::LevelNavData>& data)
{
    if (!data || data == m_data)
        return;
    m_data = data;
    if (!m_pak) {
        m_pak = std::make_unique<Pak>();
        if (!m_pak->Open(Pak::ClientDir() / L"shared.pak")) {
            m_status = "shared.pak not found";
            logger::Error("Cannot open shared.pak for the map models");
            return;
        }
        m_textures = std::make_unique<Pak>();
        if (!m_textures->Open(Pak::ClientDir() / L"main_menu_textures.pak"))
            logger::Error("Cannot open main_menu_textures.pak; map models will be drawn flat");
    }
    if (m_pak->IsOpen())
        Build(device, *data);
}

void NavModels::AddDraw(std::vector<NavModelDraw>& draws, const Mesh& mesh, const Part& part, size_t start,
                        size_t count, bool object)
{
    NavModelDraw draw;
    draw.vb = part.vb;
    draw.ib = part.ib;
    draw.indexFormat = part.indexFormat;
    draw.indexCount = part.indexCount;
    draw.albedo = mesh.albedo;
    std::memcpy(draw.colour, mesh.colour, sizeof(draw.colour));
    draw.startInstance = static_cast<unsigned>(start);
    draw.instanceCount = static_cast<unsigned>(count);
    draw.object = object;
    draws.push_back(draw);
}

const std::vector<NavModelDraw>& NavModels::Collect(ID3D11DeviceContext* context, float x, float z, float radius)
{
    m_draws.clear();
    m_visible.clear();
    m_visibleObjects.clear();
    if (!Ready())
        return m_draws;

    // Packed instances, by grid cell. Landmark models keep their look in their first node.
    const int x0 = std::max(0, static_cast<int>(std::floor((x - radius - m_originX) / kCellSize)));
    const int x1 = std::min(m_cellsX - 1, static_cast<int>(std::floor((x + radius - m_originX) / kCellSize)));
    const int z0 = std::max(0, static_cast<int>(std::floor((z - radius - m_originZ) / kCellSize)));
    const int z1 = std::min(m_cellsZ - 1, static_cast<int>(std::floor((z + radius - m_originZ) / kCellSize)));
    for (const Group& group : m_groups) {
        if (x0 > x1 || z0 > z1)
            break;
        const size_t start = m_visible.size();
        for (int cz = z0; cz <= z1; ++cz) {
            const size_t row = static_cast<size_t>(cz) * m_cellsX;
            const unsigned first = group.cellStart[row + x0], last = group.cellStart[row + x1 + 1];
            m_visible.insert(m_visible.end(), group.instances.begin() + first, group.instances.begin() + last);
        }
        if (m_visible.size() != start)
            AddDraw(m_draws, *group.mesh, group.mesh->parts.front(), start, m_visible.size() - start, false);
    }
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (!m_visible.empty()) {
        if (m_visible.size() > m_capacity || FAILED(context->Map(m_instances, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            m_draws.clear();
        } else {
            std::memcpy(mapped.pData, m_visible.data(), m_visible.size() * sizeof(game::PackedInstance));
            context->Unmap(m_instances, 0);
        }
    }

    // Single objects: few enough to test one by one.
    const size_t packedDraws = m_draws.size();
    for (const ObjectGroup& group : m_objects) {
        const size_t start = m_visibleObjects.size();
        for (const NavObjectInstance& o : group.objects) {
            if (std::fabs(o.rows[0][3] - x) <= radius && std::fabs(o.rows[2][3] - z) <= radius)
                m_visibleObjects.push_back(o);
        }
        if (m_visibleObjects.size() == start)
            continue;
        for (const Part* part : group.parts)
            AddDraw(m_draws, *group.mesh, *part, start, m_visibleObjects.size() - start, true);
    }
    if (!m_visibleObjects.empty()) {
        if (m_visibleObjects.size() > m_objectCapacity ||
            FAILED(context->Map(m_objectBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            m_draws.resize(packedDraws);
        } else {
            std::memcpy(mapped.pData, m_visibleObjects.data(), m_visibleObjects.size() * sizeof(NavObjectInstance));
            context->Unmap(m_objectBuffer, 0);
        }
    }
    return m_draws;
}

void NavModels::Release()
{
    for (auto& [name, mesh] : m_meshes) {
        for (Part& part : mesh.parts) {
            SafeRelease(part.vb);
            SafeRelease(part.ib);
        }
    }
    m_meshes.clear();
    for (auto& [name, srv] : m_albedos)
        SafeRelease(srv);
    m_albedos.clear();
    m_groups.clear();
    m_objects.clear();
    SafeRelease(m_instances);
    SafeRelease(m_objectBuffer);
    m_data.reset();
}

} // namespace render
