// Round minimap drawn as one screen-space quad (VSMain/PSMain).
// With the map-screen look, the map is first rendered in linear colour into an offscreen target the
// size of the quad: the terrain (VSScene/PSTerrain), then the landmark models (VSModel/PSModel).
// PSMain then tone maps it and adds the rim and the truck arrow.
// World axes: X = east, Z = north. Headings are radians clockwise from north.

cbuffer Minimap : register(b0)
{
    float4 g_rect;        // left, top, width, height in pixels
    float2 g_viewport;    // render target size in pixels
    float2 g_player;      // truck world X, Z
    float  g_heading;     // truck heading
    float  g_mapRotation; // heading the map is turned to (0 = north up)
    float  g_metersAcross;
    float  g_opacity;
    float4 g_mapRect;     // world X at texture left, world Z at texture top, signed X span, signed Z span
    float4 g_reliefRect;  // same, for the relief layer
    float  g_hasMap;
    float  g_hasTruck;
    float  g_hasRelief;   // set when the colour layer is unshaded (the game's own terrain bake)
    float  g_pad;
    // Map-screen look, rebuilt from the game's MinimapNavigation shader (see NavColor).
    float4 g_navTerrain;  // terrain min X, min Z, size in metres, height of the full height range
    float4 g_navTexel;    // 1 / fog-of-war size, 1 / height map size, unused, map-screen look (scene rendered)
    float4 g_navTone;     // exposure, white scale, detail tiling, detail power
};

cbuffer Model : register(b1)
{
    float4 g_modelColour; // linear diffuse multiplier; w = 1 when g_albedo is bound
};

Texture2D g_map : register(t0);
Texture2D g_relief : register(t1);
Texture2D g_tint : register(t2);      // level colour map, alpha marks land; row 0 = south
Texture2D g_cloaking : register(t3);  // fog of war: 1 revealed, 0.5 explored, 0 hidden; row 0 = south
Texture2D g_height : register(t4);    // R16 heights, 0.5 m per texel, centred on the origin; row 0 = north
Texture2D g_detail : register(t5);
Texture3D g_lut : register(t6);
Texture2D g_scene : register(t7);     // offscreen map in linear colour, alpha = on terrain
Texture2D g_albedo : register(t8);    // landmark model colour (mostly the shared palette), sRGB view
SamplerState g_sampler : register(s0);
SamplerState g_wrap : register(s1);
SamplerState g_linear : register(s2);

struct VSOut
{
    float4 pos : SV_Position;
    float2 local : TEXCOORD0; // -1..1 across the quad, +y down
};

VSOut VSMain(uint id : SV_VertexID)
{
    float2 corner = float2(id & 1, id >> 1);
    float2 px = g_rect.xy + corner * g_rect.zw;
    VSOut o;
    o.pos = float4(px.x / g_viewport.x * 2 - 1, 1 - px.y / g_viewport.y * 2, 0, 1);
    o.local = corner * 2 - 1;
    return o;
}

// Turns a screen vector (x right, y up) into the world direction it shows when the map is rotated by a.
float2 Rotate(float2 v, float a)
{
    float s = sin(a), c = cos(a);
    return float2(c * v.x + s * v.y, -s * v.x + c * v.y);
}

float SdPolygon4(float2 p, float2 v[4])
{
    float d = dot(p - v[0], p - v[0]);
    float s = 1;
    for (int i = 0, j = 3; i < 4; j = i, i++)
    {
        float2 e = v[j] - v[i];
        float2 w = p - v[i];
        float2 b = w - e * saturate(dot(w, e) / dot(e, e));
        d = min(d, dot(b, b));
        bool3 c = bool3(p.y >= v[i].y, p.y < v[j].y, e.x * w.y > e.y * w.x);
        if (all(c) || all(!c))
            s = -s;
    }
    return s * sqrt(d);
}

float HeightAt(float2 world)
{
    float size = 1.0 / g_navTexel.y;
    float2 texel = float2(world.x + (size - 1) * 0.25, (size - 1) * 0.25 - world.y) * 2.0 + 0.5;
    return g_height.SampleLevel(g_linear, texel * g_navTexel.y, 0).r * g_navTerrain.w;
}

float3 Uncharted2(float3 x)
{
    const float A = 0.1, B = 0.5, C = 0.1, D = 0.3, E = 0.02, F = 0.3;
    return (x * (A * x + C * B) + D * E) / (x * (A * x + B) + D * F) - E / F;
}

// Fog of war as both map-screen shaders apply it: hidden ground goes black, explored ground grey.
float3 ApplyFog(float3 color, float2 uv)
{
    float lum = dot(color, float3(0.2125, 0.7154, 0.0721)) * 0.4;
    float t = g_navTexel.x;
    float fog = g_cloaking.SampleLevel(g_linear, uv + float2(-0.5, 0.5) * t, 0).r +
                g_cloaking.SampleLevel(g_linear, uv + float2(-0.5, -0.5) * t, 0).r +
                g_cloaking.SampleLevel(g_linear, uv + float2(0.5, -0.5) * t, 0).r +
                g_cloaking.SampleLevel(g_linear, uv + float2(0.5, 0.5) * t, 0).r +
                g_cloaking.SampleLevel(g_linear, uv, 0).r;
    float revealed = saturate(fog - 2.5);
    float visible = saturate((fog * 0.2 - 0.25) * 4);
    return visible * lerp(lum.xxx, color, revealed);
}

// The map screen's post-processing chain: filmic tone map, sRGB, minimap colour LUT.
float3 ToneMap(float3 color)
{
    float3 x = saturate(Uncharted2(color * g_navTone.x) * g_navTone.y);
    float3 srgb = x < 0.003131 ? x * 12.92 : 1.055 * pow(x, 1 / 2.4) - 0.055;
    return g_lut.SampleLevel(g_linear, srgb * (15.0 / 16.0) + 0.5 / 16.0, 0).rgb;
}

// World direction the top of the minimap shows.
float2 MapForward()
{
    return Rotate(float2(0, 1), g_mapRotation);
}

// Direction toward the map screen's camera, which looks along MapForward() tilted 45 degrees down.
float3 ToEye()
{
    float2 forward = MapForward();
    return float3(-forward.x * 0.707107, 0.707107, -forward.y * 0.707107);
}

// The game's map-screen terrain colour (linear, before ToneMap) for a world position, shaded for
// the map camera's view (see ToEye). Follows the disassembly of MinimapNavigation.fxs (pixel
// shader); constants are the captured ones.
float3 NavLinear(float2 world, out bool onTerrain)
{
    float2 uv = (world - g_navTerrain.xy) / g_navTerrain.z;
    onTerrain = all(uv >= 0) && all(uv <= 1);

    // Normal from the height map; the game decodes it per vertex instead.
    const float e = 1.0;
    float hx = HeightAt(world + float2(e, 0)) - HeightAt(world - float2(e, 0));
    float hz = HeightAt(world + float2(0, e)) - HeightAt(world - float2(0, e));
    float3 n = normalize(float3(-hx, 2 * e, -hz));
    float3 v = ToEye(); // the game's Fresnel terms assume its tilted map camera

    float detail = g_detail.Sample(g_wrap, uv * g_navTone.z).r;
    float4 tint = g_tint.Sample(g_sampler, uv);
    float land = saturate((tint.a - 0.8) * 5);
    float3 color = tint.rgb * n.y * pow(saturate(land + detail), g_navTone.w);
    float ndv = dot(n, v);
    color *= 1 + min(pow(ndv, 4), 1);
    // Sky reflection: the game samples its environment cube along the reflected view ray.
    const float3 sky = float3(0.811969, 1.21528, 1.80069) * 0.45;
    color += land * (sky - color) * 0.1;
    return ApplyFog(color, uv);
}

// World position shown at a point of the minimap (-1..1 across it, y up).
float2 WorldAt(float2 screen)
{
    return g_player + Rotate(screen * (g_metersAcross * 0.5), g_mapRotation);
}

// --- Offscreen map: terrain -------------------------------------------------------------------

struct SceneOut
{
    float4 pos : SV_Position;
    float2 screen : TEXCOORD0; // -1..1, y up
};

SceneOut VSScene(uint id : SV_VertexID)
{
    float2 corner = float2((id << 1) & 2, id & 2);
    SceneOut o;
    o.screen = corner * float2(2, -2) + float2(-1, 1);
    o.pos = float4(o.screen, 1, 1);
    return o;
}

float4 PSTerrain(SceneOut input) : SV_Target
{
    bool onTerrain;
    float3 color = NavLinear(WorldAt(input.screen), onTerrain);
    return onTerrain ? float4(color, 1) : 0;
}

// --- Offscreen map: landmark models -----------------------------------------------------------
// Follows the disassembly of ModelNavigation.fxs. Instances are the engine's packed transforms,
// single objects carry a world matrix (VSObject).

struct ModelIn
{
    float3 pos : POSITION;
    float2 uv : TEXCOORD0;
    float4 normal : TEXCOORD1;  // RGBA8, xyz biased
    float4 place : INSTANCE0;   // world position, w = instance index
    float4 rotation : INSTANCE1; // RGBA8 quaternion, biased
    float4 scale : INSTANCE2;   // RGBA8, scale = z * 255 + w
};

struct ModelOut
{
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float3 world : TEXCOORD2;
    float shade : TEXCOORD3;
};

float3 RotateByQuaternion(float4 q, float3 v)
{
    float3 t = 2 * cross(q.xyz, v);
    return v + q.w * t + cross(q.xyz, t);
}

ModelOut ModelVertex(float3 world, float3 normal, float2 uv, float shade)
{
    // Like the game, pull anything beyond the terrain's edge back onto it.
    float2 lo = g_navTerrain.xy;
    world.xz -= 0.999 * (min(world.xz - lo - 0.5, 0) + max(world.xz + lo + 0.5, 0));

    ModelOut o;
    float2 screen = Rotate(world.xz - g_player, -g_mapRotation) / (g_metersAcross * 0.5);
    o.pos = float4(screen, saturate(0.5 - world.y / 4000), 1);
    o.uv = uv;
    o.normal = normal;
    o.world = world;
    o.shade = shade;
    return o;
}

ModelOut VSModel(ModelIn v)
{
    float4 q = normalize((v.rotation - 0.501961) * 2.007874);
    float scale = dot(v.scale.zw, float2(255, 0.996094));
    float3 world = RotateByQuaternion(q, v.pos * scale) + v.place.xyz;
    return ModelVertex(world, RotateByQuaternion(q, (v.normal.xyz - 0.501960) * 2), v.uv, sin(v.place.w));
}

// Single objects (towers, service stations, bridges, ...): the engine draws each with its own world
// matrix (g_tmWorld) and no per-instance shade.
struct ObjectIn
{
    float3 pos : POSITION;
    float2 uv : TEXCOORD0;
    float4 normal : TEXCOORD1;
    float4 row0 : INSTANCE0; // world transform rows: world = row . (pos, 1)
    float4 row1 : INSTANCE1;
    float4 row2 : INSTANCE2;
};

ModelOut VSObject(ObjectIn v)
{
    float4 p = float4(v.pos, 1);
    float3 world = float3(dot(v.row0, p), dot(v.row1, p), dot(v.row2, p));
    float3 n = (v.normal.xyz - 0.501960) * 2;
    n = normalize(float3(dot(v.row0.xyz, n), dot(v.row1.xyz, n), dot(v.row2.xyz, n)));
    return ModelVertex(world, n, v.uv, 1);
}

float4 PSModel(ModelOut input) : SV_Target
{
    float3 albedo = g_modelColour.w > 0.5 ? g_albedo.Sample(g_sampler, input.uv).rgb : 0.35;
    float3 color = albedo * g_modelColour.rgb * (input.shade * 0.2 + 0.8);

    // The game lights models from the side of its map camera's view and adds a Fresnel rim for its
    // 45 degree tilt; "forward" is the direction the minimap's top shows.
    float2 forward = MapForward();
    float3 toLight = float3(-0.447214 * forward.y, 0.894427, 0.447214 * forward.x);
    float3 n = input.normal;
    color *= min(max(dot(toLight, n), 0) + 0.2, 1);
    // Faces turned away from that camera never show on the map screen but do from straight above;
    // they get the Fresnel of a camera facing the other way instead of a full-strength rim.
    float3 e = ToEye();
    float ndv = max(dot(n, e), dot(n, float3(-e.x, e.y, -e.z)));
    color *= 1 + pow(saturate(1 - ndv), 2) * 4; // g_fFresnelPower 2, g_fFresnelScale 4

    float2 uv = (input.world.xz - g_navTerrain.xy) / g_navTerrain.z;
    return float4(ApplyFog(color, uv), 1);
}

float4 PSMain(VSOut input) : SV_Target
{
    float radiusPx = g_rect.z * 0.5;
    float r = length(input.local);
    float edgePx = (1 - r) * radiusPx; // distance to the rim, in pixels
    if (edgePx < -1.5)
        discard;

    float2 screen = float2(input.local.x, -input.local.y);   // unit circle, y up
    float metersPerPx = g_metersAcross / g_rect.z;
    float2 world = WorldAt(screen);

    // Terrain: the game's map-screen look when its inputs are captured, else a map texture where
    // we have one, otherwise a 100 m grid so movement is still readable.
    float3 color = float3(0.13, 0.15, 0.16);
    float2 uv = (world - g_mapRect.xy) / g_mapRect.zw;
    float2 reliefUv = (world - g_reliefRect.xy) / g_reliefRect.zw;
    bool onMap = g_hasMap > 0.5 && all(uv >= 0) && all(uv <= 1);
    // The game's bake is larger than the level; its unused part is black, so clip to the terrain.
    if (g_hasRelief > 0.5)
        onMap = onMap && all(reliefUv >= 0) && all(reliefUv <= 1);
    if (g_navTexel.w > 0.5)
    {
        float4 scene = g_scene.SampleLevel(g_linear, input.local * 0.5 + 0.5, 0);
        color = ToneMap(scene.rgb) * scene.a;
    }
    else if (onMap)
    {
        color = g_map.Sample(g_sampler, uv).rgb;
        if (g_hasRelief > 0.5)
            color *= 0.45 + 0.75 * g_relief.Sample(g_sampler, reliefUv).r;
    }
    else
    {
        float2 cell = abs(frac(world / 100 + 0.5) - 0.5) * 100;
        float gridLine = saturate(1.2 - min(cell.x, cell.y) / metersPerPx);
        color = lerp(color, float3(0.30, 0.34, 0.36), gridLine);
    }

    // Soft darkening toward the rim keeps the edge readable over bright terrain.
    color *= lerp(1.0, 0.72, smoothstep(0.6, 1.0, r));

    // Truck arrow, pointing along the heading as seen on this map.
    if (g_hasTruck > 0.5)
    {
        float2 p = Rotate(screen * radiusPx, -(g_heading - g_mapRotation)); // pixels, y = forward
        float size = clamp(radiusPx * 0.09, 7, 22);
        float2 shape[4] = { float2(0, 1.0) * size, float2(0.62, -0.75) * size, float2(0, -0.38) * size,
                            float2(-0.62, -0.75) * size };
        float d = SdPolygon4(p, shape);
        float outline = 1 - smoothstep(1.5, 3.0, d);
        float fill = 1 - smoothstep(-0.5, 0.5, d);
        color = lerp(color, float3(0.05, 0.05, 0.05), outline * 0.85);
        color = lerp(color, float3(1.0, 0.78, 0.18), fill);
    }

    // North tick on the rim.
    float2 northDir = Rotate(float2(0, 1), -g_mapRotation);
    float2 northPx = northDir * (radiusPx - 9);
    float north = 1 - smoothstep(4.0, 5.5, length(screen * radiusPx - northPx));
    color = lerp(color, float3(0.86, 0.2, 0.16), north);

    // Rim.
    float rim = 1 - smoothstep(2.0, 3.0, edgePx);
    color = lerp(color, float3(0.92, 0.92, 0.88), rim);

    float alpha = saturate(edgePx + 0.5) * g_opacity;
    return float4(color, alpha);
}
