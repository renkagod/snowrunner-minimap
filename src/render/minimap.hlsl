// Round minimap drawn as one screen-space quad.
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
    float4 g_navTexel;    // 1 / fog-of-war size, 1 / height map size, unused, has nav inputs
    float4 g_navTone;     // exposure, white scale, detail tiling, detail power
};

Texture2D g_map : register(t0);
Texture2D g_relief : register(t1);
Texture2D g_tint : register(t2);      // level colour map, alpha marks land; row 0 = south
Texture2D g_cloaking : register(t3);  // fog of war: 1 revealed, 0.5 explored, 0 hidden; row 0 = south
Texture2D g_height : register(t4);    // R16 heights, 0.5 m per texel, centred on the origin; row 0 = north
Texture2D g_detail : register(t5);
Texture3D g_lut : register(t6);
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

// The game's map-screen terrain colour for a world position, seen from straight above.
// Follows the disassembly of MinimapNavigation.fxs (pixel shader) with the post-processing chain
// of the map screen (filmic tone map, sRGB, minimap colour LUT); constants are the captured ones.
float3 NavColor(float2 world, out bool onTerrain)
{
    float2 uv = (world - g_navTerrain.xy) / g_navTerrain.z;
    onTerrain = all(uv >= 0) && all(uv <= 1);

    // Normal from the height map; the game decodes it per vertex instead.
    const float e = 1.0;
    float hx = HeightAt(world + float2(e, 0)) - HeightAt(world - float2(e, 0));
    float hz = HeightAt(world + float2(0, e)) - HeightAt(world - float2(0, e));
    float3 n = normalize(float3(-hx, 2 * e, -hz));
    float3 v = float3(0, 1, 0);

    float detail = g_detail.Sample(g_wrap, uv * g_navTone.z).r;
    float4 tint = g_tint.Sample(g_sampler, uv);
    float land = saturate((tint.a - 0.8) * 5);
    float3 color = tint.rgb * n.y * pow(saturate(land + detail), g_navTone.w);
    float ndv = dot(n, v);
    color *= 1 + min(pow(ndv, 4), 1);
    // Sky reflection: the game samples its environment cube along the reflected view ray.
    const float3 sky = float3(0.811969, 1.21528, 1.80069) * 0.45;
    color += land * (sky - color) * 0.1;
    float lum = dot(color, float3(0.2125, 0.7154, 0.0721)) * 0.4;

    float t = g_navTexel.x;
    float fog = g_cloaking.SampleLevel(g_linear, uv + float2(-0.5, 0.5) * t, 0).r +
                g_cloaking.SampleLevel(g_linear, uv + float2(-0.5, -0.5) * t, 0).r +
                g_cloaking.SampleLevel(g_linear, uv + float2(0.5, -0.5) * t, 0).r +
                g_cloaking.SampleLevel(g_linear, uv + float2(0.5, 0.5) * t, 0).r +
                g_cloaking.SampleLevel(g_linear, uv, 0).r;
    float revealed = saturate(fog - 2.5);
    float visible = saturate((fog * 0.2 - 0.25) * 4);
    color = visible * lerp(lum.xxx, color, revealed);

    float3 x = saturate(Uncharted2(color * g_navTone.x) * g_navTone.y);
    float3 srgb = x < 0.003131 ? x * 12.92 : 1.055 * pow(x, 1 / 2.4) - 0.055;
    return g_lut.SampleLevel(g_linear, srgb * (15.0 / 16.0) + 0.5 / 16.0, 0).rgb;
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
    float2 world = g_player + Rotate(screen * (g_metersAcross * 0.5), g_mapRotation);

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
        bool onTerrain;
        color = NavColor(world, onTerrain);
        if (!onTerrain)
            color = 0;
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
