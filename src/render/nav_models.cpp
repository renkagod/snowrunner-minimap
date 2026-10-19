#include "render/nav_models.h"

#include "core/log.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>

namespace render {

namespace {

constexpr float kCellSize = 64.0f; // metres per placement grid cell
constexpr int kMaxCells = 256;     // per axis
constexpr unsigned kVertexStride = 28;

// Mesh data inside a CombineXMesh file: a u32-prefixed XML material header, then the node and
// mesh records. Each mesh ends its counts with a D3DVERTEXELEMENT9 declaration closed by
// D3DDECL_END, right after which its vertex and index data follow, exactly as the GPU takes them.
struct MeshData {
    std::string xml;
    const unsigned char* vertices = nullptr;
    unsigned vertexCount = 0;
    const unsigned char* indices = nullptr;
    unsigned indexCount = 0;
    bool wideIndices = false;
};

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

// Takes the file's first mesh; landmarks with an objective keep their completed look in a second one.
bool ParseMesh(const std::vector<unsigned char>& d, MeshData& out)
{
    if (d.size() < 4 || 4 + size_t(U32(d, 0)) > d.size())
        return false;
    const size_t xmlEnd = 4 + U32(d, 0);
    out.xml.assign(reinterpret_cast<const char*>(d.data()) + 4, xmlEnd - 4);

    static const unsigned char kDeclEnd[8] = {0xFF, 0, 0, 0, 0x11, 0, 0, 0};
    const auto end = std::search(d.begin() + static_cast<ptrdiff_t>(xmlEnd), d.end(), kDeclEnd, kDeclEnd + 8);
    if (end == d.end())
        return false;
    const size_t declEnd = static_cast<size_t>(end - d.begin());
    // The declaration is preceded by its element count, the vertex count and the triangle count.
    size_t decl = 0;
    unsigned elements = 0;
    for (unsigned k = 1; k <= 16 && declEnd >= xmlEnd + 16 + 8 * k; ++k) {
        if (U32(d, declEnd - 8 * k - 4) == k) {
            decl = declEnd - 8 * k;
            elements = k;
            break;
        }
    }
    // Expected layout: POSITION float3 @0, TEXCOORD0 float2 @12, TEXCOORD1 (normal) @20, TEXCOORD2 @24.
    if (!decl || elements != 4 || U16(d, decl + 2) != 0 || d[decl + 4] != 2 || U16(d, decl + 8 + 2) != 12 ||
        U16(d, decl + 16 + 2) != 20 || U16(d, decl + 24 + 2) != 24)
        return false;
    out.vertexCount = U32(d, decl - 8);
    out.indexCount = U32(d, decl - 16) * 3;
    out.wideIndices = out.vertexCount > 0xFFFF;
    const size_t vb = declEnd + 8;
    const size_t ib = vb + size_t(out.vertexCount) * kVertexStride;
    const size_t ibEnd = ib + size_t(out.indexCount) * (out.wideIndices ? 4 : 2);
    if (!out.vertexCount || !out.indexCount || ibEnd > d.size())
        return false;
    out.vertices = d.data() + vb;
    out.indices = d.data() + ib;
    return true;
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
        return mesh.vb ? &mesh : nullptr;
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

    mesh.vb = CreateBuffer(device, data.vertices, size_t(data.vertexCount) * kVertexStride, D3D11_BIND_VERTEX_BUFFER);
    mesh.ib = CreateBuffer(device, data.indices, size_t(data.indexCount) * (data.wideIndices ? 4 : 2),
                           D3D11_BIND_INDEX_BUFFER);
    mesh.indexFormat = data.wideIndices ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT;
    mesh.indexCount = data.indexCount;
    if (!mesh.vb || !mesh.ib) {
        SafeRelease(mesh.vb);
        SafeRelease(mesh.ib);
        return nullptr;
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

    m_status = std::format("{} models, {} instances on {}x{} cells; {} instances without a mesh", m_groups.size(),
                           total, m_cellsX, m_cellsZ, missing);
    logger::Info("Map models of '{}': {}", data.level, m_status);
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

const std::vector<NavModelDraw>& NavModels::Collect(ID3D11DeviceContext* context, float x, float z, float radius)
{
    m_draws.clear();
    m_visible.clear();
    if (!Ready())
        return m_draws;
    const int x0 = std::max(0, static_cast<int>(std::floor((x - radius - m_originX) / kCellSize)));
    const int x1 = std::min(m_cellsX - 1, static_cast<int>(std::floor((x + radius - m_originX) / kCellSize)));
    const int z0 = std::max(0, static_cast<int>(std::floor((z - radius - m_originZ) / kCellSize)));
    const int z1 = std::min(m_cellsZ - 1, static_cast<int>(std::floor((z + radius - m_originZ) / kCellSize)));
    if (x0 > x1 || z0 > z1)
        return m_draws;

    for (const Group& group : m_groups) {
        const size_t start = m_visible.size();
        for (int cz = z0; cz <= z1; ++cz) {
            const size_t row = static_cast<size_t>(cz) * m_cellsX;
            const unsigned first = group.cellStart[row + x0], last = group.cellStart[row + x1 + 1];
            m_visible.insert(m_visible.end(), group.instances.begin() + first, group.instances.begin() + last);
        }
        if (m_visible.size() == start)
            continue;
        NavModelDraw draw;
        draw.vb = group.mesh->vb;
        draw.ib = group.mesh->ib;
        draw.indexFormat = group.mesh->indexFormat;
        draw.indexCount = group.mesh->indexCount;
        draw.albedo = group.mesh->albedo;
        std::memcpy(draw.colour, group.mesh->colour, sizeof(draw.colour));
        draw.startInstance = static_cast<unsigned>(start);
        draw.instanceCount = static_cast<unsigned>(m_visible.size() - start);
        m_draws.push_back(draw);
    }

    D3D11_MAPPED_SUBRESOURCE mapped;
    if (m_visible.empty() || m_visible.size() > m_capacity ||
        FAILED(context->Map(m_instances, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        m_draws.clear();
        return m_draws;
    }
    std::memcpy(mapped.pData, m_visible.data(), m_visible.size() * sizeof(game::PackedInstance));
    context->Unmap(m_instances, 0);
    return m_draws;
}

void NavModels::Release()
{
    for (auto& [name, mesh] : m_meshes) {
        SafeRelease(mesh.vb);
        SafeRelease(mesh.ib);
    }
    m_meshes.clear();
    for (auto& [name, srv] : m_albedos)
        SafeRelease(srv);
    m_albedos.clear();
    m_groups.clear();
    SafeRelease(m_instances);
    m_data.reset();
}

} // namespace render
