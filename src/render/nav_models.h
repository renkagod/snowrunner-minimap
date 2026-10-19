#pragma once

#include "game/game_state.h"
#include "render/pak.h"

#include <d3d11.h>

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace render {

// Placement of one single object as VSObject takes it: the three rows of its world transform in
// column-vector form (world = row . (position, 1)).
struct NavObjectInstance {
    float rows[3][4];
};
static_assert(sizeof(NavObjectInstance) == 48);

// One instanced draw of a landmark model part. Instances come from NavModels::InstanceBuffer()
// (PackedInstance) or, for single objects, NavModels::ObjectBuffer() (NavObjectInstance).
struct NavModelDraw {
    ID3D11Buffer* vb = nullptr; // 28-byte vertices: position, uv, normal (RGBA8), tangent (RGBA8)
    ID3D11Buffer* ib = nullptr;
    DXGI_FORMAT indexFormat = DXGI_FORMAT_R16_UINT;
    unsigned indexCount = 0;
    ID3D11ShaderResourceView* albedo = nullptr; // null: drawn in a flat grey
    float colour[4] = {}; // linear diffuse multiplier; w = 1 when albedo is set
    unsigned startInstance = 0;
    unsigned instanceCount = 0;
    bool object = false; // instances are NavObjectInstance
};

// The map screen's landmark models (trees, rocks, buildings, ...): placement from the level data the
// engine keeps in memory, meshes from shared.pak ([meshes]\landmarks_<name>), their colour textures
// (mostly the shared palette models/landmark__d) from main_menu_textures.pak. Most models are packed
// instances; towers, service stations, bridges and the like are single objects with a full transform.
// Render thread only.
class NavModels {
public:
    // Rebuilds when the level's placement changes; meshes are cached across levels.
    void Update(ID3D11Device* device, const std::shared_ptr<const game::LevelNavData>& data);
    // Copies the instances and objects within radius metres of (x, z) into the instance buffers and
    // returns the draws that show them, packed instances first.
    const std::vector<NavModelDraw>& Collect(ID3D11DeviceContext* context, float x, float z, float radius);
    ID3D11Buffer* InstanceBuffer() const { return m_instances; }
    ID3D11Buffer* ObjectBuffer() const { return m_objectBuffer; }
    bool Ready() const { return !m_groups.empty() || !m_objects.empty(); }
    const std::string& Status() const { return m_status; }
    void Release();

private:
    // A node of the mesh file that has geometry, baked into model space.
    struct Part {
        std::string node;
        ID3D11Buffer* vb = nullptr;
        ID3D11Buffer* ib = nullptr;
        DXGI_FORMAT indexFormat = DXGI_FORMAT_R16_UINT;
        unsigned indexCount = 0;
    };
    struct Mesh {
        bool loaded = false;
        std::vector<Part> parts;
        ID3D11ShaderResourceView* albedo = nullptr; // owned by m_textures
        float colour[4] = {1, 1, 1, 1};
    };
    struct Group {
        const Mesh* mesh = nullptr;
        std::vector<game::PackedInstance> instances; // sorted by grid cell, row by row
        std::vector<unsigned> cellStart;             // first instance of each cell, plus the end
    };
    struct ObjectGroup {
        const Mesh* mesh = nullptr;
        std::vector<const Part*> parts; // the parts the map screen shows
        std::vector<NavObjectInstance> objects;
    };

    const Mesh* LoadMesh(ID3D11Device* device, const std::string& model);
    ID3D11ShaderResourceView* LoadAlbedo(ID3D11Device* device, const std::string& albedoMap);
    void Build(ID3D11Device* device, const game::LevelNavData& data);
    void BuildObjects(ID3D11Device* device, const game::LevelNavData& data);
    static void AddDraw(std::vector<NavModelDraw>& draws, const Mesh& mesh, const Part& part, size_t start,
                        size_t count, bool object);

    std::unique_ptr<Pak> m_pak;      // shared.pak
    std::unique_ptr<Pak> m_textures; // main_menu_textures.pak
    std::map<std::string, Mesh> m_meshes;
    std::map<std::string, ID3D11ShaderResourceView*> m_albedos; // by AlbedoMap, null when unreadable
    std::shared_ptr<const game::LevelNavData> m_data;
    std::vector<Group> m_groups;
    float m_originX = 0, m_originZ = 0;
    int m_cellsX = 0, m_cellsZ = 0;
    ID3D11Buffer* m_instances = nullptr;
    size_t m_capacity = 0;
    std::vector<game::PackedInstance> m_visible;
    std::vector<ObjectGroup> m_objects;
    ID3D11Buffer* m_objectBuffer = nullptr;
    size_t m_objectCapacity = 0;
    std::vector<NavObjectInstance> m_visibleObjects;
    std::vector<NavModelDraw> m_draws;
    std::string m_status = "no level data";
};

} // namespace render
