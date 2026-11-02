#include "Renderer.h"
#include "Shaders.h"
#include "core/Log.h"
#include <d3dcompiler.h>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtc/matrix_inverse.hpp>
#include <algorithm>
#include <chrono>
#include <cstring>

template<class T> static void SafeRelease(T*& p) {
    if (p) { p->Release(); p = nullptr; }
}

const char* RenderModeName(int mode) {
    static const char* names[] = { "ライティング", "ベースカラー", "法線", "UV", "UV チェッカー", "ボーンウェイト",
                                   "頂点カラー", "メタリック/ラフネス", "オクルージョン", "接線" };
    return mode >= 0 && mode < RM_Count ? names[mode] : "?";
}

// 定数バッファ (HLSL 側と同じレイアウト)
struct CBFrame {
    glm::mat4 viewProj;
    glm::vec4 camPos;
    glm::vec4 lightDir;
    glm::vec4 params;
};
struct CBDraw {
    glm::mat4 model;
    glm::mat4 normalMat;
    glm::vec4 baseColor;
    glm::vec4 emissive;
    glm::vec4 mat;
    glm::ivec4 flags0;
    glm::ivec4 flags1;
    glm::ivec4 hasTex;
    glm::ivec4 uvSet;
    glm::vec4 overrideColor;
    glm::vec4 pickColor;
};
static_assert(sizeof(CBFrame) % 16 == 0 && sizeof(CBDraw) % 16 == 0, "cbuffer alignment");

static CBDraw g_draw; // BindMaterial と描画で共有

static ID3DBlob* CompileShader(const char* src, const char* entry, const char* target, const char* label) {
    ID3DBlob* code = nullptr;
    ID3DBlob* err = nullptr;
    HRESULT hr = D3DCompile(src, strlen(src), label, nullptr, nullptr, entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err);
    if (FAILED(hr)) {
        LOG_ERROR("シェーダーのコンパイル失敗 (%s/%s): %s", label, entry, err ? (const char*)err->GetBufferPointer() : "unknown");
        SafeRelease(code);
    }
    SafeRelease(err);
    return code;
}

// ---------------------------------------------------------------------------
bool Renderer::Init(ID3D11Device* device, ID3D11DeviceContext* ctx, const std::string& adapterInfo) {
    m_dev = device;
    m_ctx = ctx;
    m_adapterInfo = adapterInfo;

    ID3DBlob* vs = CompileShader(kMeshHLSL, "VSMain", "vs_4_0", "mesh");
    ID3DBlob* ps = CompileShader(kMeshHLSL, "PSMain", "ps_4_0", "mesh");
    ID3DBlob* pp = CompileShader(kMeshHLSL, "PSPick", "ps_4_0", "mesh");
    ID3DBlob* lvs = CompileShader(kLineHLSL, "VSMain", "vs_4_0", "line");
    ID3DBlob* lps = CompileShader(kLineHLSL, "PSMain", "ps_4_0", "line");
    if (!vs || !ps || !pp || !lvs || !lps) {
        SafeRelease(vs); SafeRelease(ps); SafeRelease(pp); SafeRelease(lvs); SafeRelease(lps);
        return false;
    }
    m_dev->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &m_meshVS);
    m_dev->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &m_meshPS);
    m_dev->CreatePixelShader(pp->GetBufferPointer(), pp->GetBufferSize(), nullptr, &m_pickPS);
    m_dev->CreateVertexShader(lvs->GetBufferPointer(), lvs->GetBufferSize(), nullptr, &m_lineVS);
    m_dev->CreatePixelShader(lps->GetBufferPointer(), lps->GetBufferSize(), nullptr, &m_linePS);

    D3D11_INPUT_ELEMENT_DESC meshLayout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 1, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 2, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 3, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TANGENT", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 4, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT, 5, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 2, DXGI_FORMAT_R32_FLOAT, 6, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    m_dev->CreateInputLayout(meshLayout, 7, vs->GetBufferPointer(), vs->GetBufferSize(), &m_meshLayout);
    D3D11_INPUT_ELEMENT_DESC lineLayout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    m_dev->CreateInputLayout(lineLayout, 2, lvs->GetBufferPointer(), lvs->GetBufferSize(), &m_lineLayout);
    SafeRelease(vs); SafeRelease(ps); SafeRelease(pp); SafeRelease(lvs); SafeRelease(lps);

    auto makeCB = [&](UINT size) {
        D3D11_BUFFER_DESC d = {};
        d.ByteWidth = size;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        ID3D11Buffer* b = nullptr;
        m_dev->CreateBuffer(&d, nullptr, &b);
        return b;
    };
    m_cbFrame = makeCB(sizeof(CBFrame));
    m_cbDraw = makeCB(sizeof(CBDraw));

    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_BACK;
    rd.FrontCounterClockwise = TRUE; // glTF / OpenGL と同じく反時計回りが表
    rd.DepthClipEnable = TRUE;
    rd.MultisampleEnable = TRUE;
    m_dev->CreateRasterizerState(&rd, &m_rsCull);
    rd.CullMode = D3D11_CULL_NONE;
    m_dev->CreateRasterizerState(&rd, &m_rsNoCull);
    rd.FillMode = D3D11_FILL_WIREFRAME;
    rd.DepthBias = -200;
    rd.SlopeScaledDepthBias = -1.0f;
    m_dev->CreateRasterizerState(&rd, &m_rsWire);

    D3D11_DEPTH_STENCIL_DESC dd = {};
    dd.DepthEnable = TRUE;
    dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    dd.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
    m_dev->CreateDepthStencilState(&dd, &m_dsTestWrite);
    dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    m_dev->CreateDepthStencilState(&dd, &m_dsTestNoWrite);
    dd.DepthEnable = FALSE;
    m_dev->CreateDepthStencilState(&dd, &m_dsOff);

    D3D11_BLEND_DESC bd = {};
    bd.RenderTarget[0].BlendEnable = TRUE;
    bd.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    m_dev->CreateBlendState(&bd, &m_bsAlpha);
    bd.RenderTarget[0].BlendEnable = FALSE;
    m_dev->CreateBlendState(&bd, &m_bsOpaque);

    // サンプラー: フィルタ(リニア/ニアレスト) x wrapS(3) x wrapT(3)
    static const D3D11_TEXTURE_ADDRESS_MODE modes[3] = { D3D11_TEXTURE_ADDRESS_WRAP, D3D11_TEXTURE_ADDRESS_CLAMP, D3D11_TEXTURE_ADDRESS_MIRROR };
    for (int f = 0; f < 2; ++f)
        for (int s = 0; s < 3; ++s)
            for (int t = 0; t < 3; ++t) {
                D3D11_SAMPLER_DESC sd = {};
                sd.Filter = f == 0 ? D3D11_FILTER_MIN_MAG_MIP_LINEAR : D3D11_FILTER_MIN_MAG_MIP_POINT;
                sd.AddressU = modes[s];
                sd.AddressV = modes[t];
                sd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
                sd.MaxLOD = D3D11_FLOAT32_MAX;
                sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
                m_dev->CreateSamplerState(&sd, &m_samplers[f * 9 + s * 3 + t]);
            }

    // 既定の頂点属性 (stride 0 で全頂点に同じ値を使う)
    const float defaults[7][4] = { { 0, 0, 0, 0 }, { 0, 1, 0, 0 }, { 0, 0, 0, 0 }, { 1, 1, 1, 1 }, { 1, 0, 0, 1 }, { 0, 0, 0, 0 }, { 0, 0, 0, 0 } };
    for (int i = 0; i < 7; ++i) {
        D3D11_BUFFER_DESC d = {};
        d.ByteWidth = 16;
        d.Usage = D3D11_USAGE_IMMUTABLE;
        d.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        D3D11_SUBRESOURCE_DATA init = { defaults[i], 0, 0 };
        m_dev->CreateBuffer(&d, &init, &m_defaultVB[i]);
    }

    uint32_t white = 0xFFFFFFFF;
    m_whiteSRV = CreateTexture(1, 1, &white, false, nullptr);
    uint8_t flat[4] = { 128, 128, 255, 255 };
    m_normalSRV = CreateTexture(1, 1, flat, false, nullptr);
    std::vector<uint8_t> checker(64 * 64 * 4);
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x) {
            bool c = ((x / 8) + (y / 8)) & 1;
            uint8_t* p = &checker[(y * 64 + x) * 4];
            p[0] = c ? 230 : 60; p[1] = c ? 230 : 60; p[2] = c ? 230 : 70; p[3] = 255;
            if (x < 8 && y < 8) { p[0] = 230; p[1] = 60; p[2] = 60; } // 左上 (UV 0,0) を赤で示す
        }
    m_checkerSRV = CreateTexture(64, 64, checker.data(), true, nullptr);

    bool ok = m_meshVS && m_meshPS && m_pickPS && m_lineVS && m_linePS && m_meshLayout && m_lineLayout && m_cbFrame && m_cbDraw;
    LOG_INFO("レンダラー初期化%s: %s", ok ? "完了" : "失敗", m_adapterInfo.c_str());
    return ok;
}

bool Renderer::DeviceLost() const {
    return m_dev && m_dev->GetDeviceRemovedReason() != S_OK;
}

void Renderer::Shutdown() {
    ClearCachesNow();
    ReleaseTargets();
    SafeRelease(m_meshVS); SafeRelease(m_meshPS); SafeRelease(m_meshLayout);
    SafeRelease(m_lineVS); SafeRelease(m_linePS); SafeRelease(m_lineLayout); SafeRelease(m_pickPS);
    SafeRelease(m_cbFrame); SafeRelease(m_cbDraw);
    SafeRelease(m_rsCull); SafeRelease(m_rsNoCull); SafeRelease(m_rsWire);
    SafeRelease(m_dsTestWrite); SafeRelease(m_dsTestNoWrite); SafeRelease(m_dsOff);
    SafeRelease(m_bsAlpha); SafeRelease(m_bsOpaque);
    for (auto& s : m_samplers) SafeRelease(s);
    for (auto& b : m_defaultVB) SafeRelease(b);
    SafeRelease(m_lineVB);
    m_lineVBCapacity = 0;
    SafeRelease(m_whiteSRV); SafeRelease(m_normalSRV); SafeRelease(m_checkerSRV);
    m_dev = nullptr;
    m_ctx = nullptr;
}

void Renderer::BeginFrame() {
    if (m_clearRequested) {
        ClearCachesNow();
        m_clearRequested = false;
    }
}

void Renderer::ClearCachesNow() {
    for (auto& [k, m] : m_meshes) DestroyMesh(m);
    m_meshes.clear();
    for (auto& [k, t] : m_textures) SafeRelease(t.srv);
    m_textures.clear();
}

ID3D11ShaderResourceView* Renderer::CreateTexture(int w, int h, const void* rgba, bool mips, size_t* bytesOut) {
    if (w <= 0 || h <= 0 || !m_dev) return nullptr;
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = w;
    td.Height = h;
    td.MipLevels = mips ? 0 : 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | (mips ? D3D11_BIND_RENDER_TARGET : 0);
    td.MiscFlags = mips ? D3D11_RESOURCE_MISC_GENERATE_MIPS : 0;
    ID3D11Texture2D* tex = nullptr;
    HRESULT hr;
    if (mips) {
        hr = m_dev->CreateTexture2D(&td, nullptr, &tex);
        if (SUCCEEDED(hr)) m_ctx->UpdateSubresource(tex, 0, nullptr, rgba, w * 4, 0);
    } else {
        D3D11_SUBRESOURCE_DATA init = { rgba, (UINT)(w * 4), 0 };
        hr = m_dev->CreateTexture2D(&td, &init, &tex);
    }
    if (FAILED(hr)) {
        LOG_WARN("テクスチャ (%dx%d) を GPU に作成できません (hr=0x%08x)。GPU の最大サイズを超えている可能性があります", w, h, (unsigned)hr);
        return nullptr;
    }
    ID3D11ShaderResourceView* srv = nullptr;
    m_dev->CreateShaderResourceView(tex, nullptr, &srv);
    if (mips && srv) m_ctx->GenerateMips(srv);
    tex->Release();
    if (bytesOut) *bytesOut = (size_t)w * h * 4 * (mips ? 4 : 3) / 3;
    return srv;
}

int Renderer::SamplerIndex(const Texture* tex) const {
    if (!tex) return 0;
    auto w = [](WrapMode m) { return m == WrapMode::ClampToEdge ? 1 : m == WrapMode::MirroredRepeat ? 2 : 0; };
    return (tex->linearFilter ? 0 : 9) + w(tex->wrapS) * 3 + w(tex->wrapT);
}

void Renderer::ReleaseTargets() {
    SafeRelease(m_msColor); SafeRelease(m_msRTV); SafeRelease(m_msDepth); SafeRelease(m_msDSV);
    SafeRelease(m_resolveTex); SafeRelease(m_resolveSRV);
    SafeRelease(m_pickTex); SafeRelease(m_pickRTV); SafeRelease(m_pickDepth); SafeRelease(m_pickDSV); SafeRelease(m_pickStaging);
    m_w = m_h = 0;
}

bool Renderer::EnsureTargets(int w, int h, int msaa) {
    if (w == m_w && h == m_h && msaa == m_samples && m_msRTV) return true;
    ReleaseTargets();
    m_samples = msaa;
    if (w <= 0 || h <= 0) return false;
    m_w = w;
    m_h = h;

    // 対応する MSAA サンプル数を探す (非対応なら 1)
    UINT samples = 1;
    for (UINT s = (UINT)std::clamp(msaa, 1, 8); s > 1; s /= 2) {
        UINT q1 = 0, q2 = 0;
        if (SUCCEEDED(m_dev->CheckMultisampleQualityLevels(DXGI_FORMAT_R8G8B8A8_UNORM, s, &q1)) &&
            SUCCEEDED(m_dev->CheckMultisampleQualityLevels(DXGI_FORMAT_D24_UNORM_S8_UINT, s, &q2)) && q1 > 0 && q2 > 0) {
            samples = s;
            break;
        }
    }

    D3D11_TEXTURE2D_DESC td = {};
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = samples;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    HRESULT hr = m_dev->CreateTexture2D(&td, nullptr, &m_msColor);
    if (SUCCEEDED(hr)) hr = m_dev->CreateRenderTargetView(m_msColor, nullptr, &m_msRTV);
    td.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    if (SUCCEEDED(hr)) hr = m_dev->CreateTexture2D(&td, nullptr, &m_msDepth);
    if (SUCCEEDED(hr)) hr = m_dev->CreateDepthStencilView(m_msDepth, nullptr, &m_msDSV);

    td.SampleDesc.Count = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (SUCCEEDED(hr)) hr = m_dev->CreateTexture2D(&td, nullptr, &m_resolveTex);
    if (SUCCEEDED(hr)) hr = m_dev->CreateShaderResourceView(m_resolveTex, nullptr, &m_resolveSRV);

    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    if (SUCCEEDED(hr)) hr = m_dev->CreateTexture2D(&td, nullptr, &m_pickTex);
    if (SUCCEEDED(hr)) hr = m_dev->CreateRenderTargetView(m_pickTex, nullptr, &m_pickRTV);
    td.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    if (SUCCEEDED(hr)) hr = m_dev->CreateTexture2D(&td, nullptr, &m_pickDepth);
    if (SUCCEEDED(hr)) hr = m_dev->CreateDepthStencilView(m_pickDepth, nullptr, &m_pickDSV);

    D3D11_TEXTURE2D_DESC sd = {};
    sd.Width = 1;
    sd.Height = 1;
    sd.MipLevels = 1;
    sd.ArraySize = 1;
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (SUCCEEDED(hr)) hr = m_dev->CreateTexture2D(&sd, nullptr, &m_pickStaging);
    if (FAILED(hr)) {
        LOG_ERROR("ビューポート用のレンダーターゲットを作成できません (hr=0x%08x, %dx%d)", (unsigned)hr, w, h);
        ReleaseTargets();
        return false;
    }
    return true;
}

void* Renderer::GetTextureSRV(const Texture& tex) {
    auto it = m_textures.find(tex.uid);
    if (it != m_textures.end()) { it->second.used = true; return it->second.srv; }
    if (!tex.image || tex.image->width <= 0 || !m_dev) return nullptr;
    TexEntry e;
    e.srv = CreateTexture(tex.image->width, tex.image->height, tex.image->rgba.data(), tex.generateMips, &e.bytes);
    e.sampler = SamplerIndex(&tex);
    e.used = true;
    m_textures[tex.uid] = e; // 失敗しても記録して毎フレーム再試行しない
    return e.srv;
}

// ---------------------------------------------------------------------------
// メッシュ
// ---------------------------------------------------------------------------
void Renderer::DestroyMesh(GpuMesh& gm) {
    for (auto& p : gm.prims) {
        for (auto& b : p.vb) SafeRelease(b);
        SafeRelease(p.ib);
    }
    gm.prims.clear();
}

void Renderer::BuildMesh(GpuMesh& gm, const Mesh& mesh) {
    DestroyMesh(gm);
    gm.uid = mesh.uid;
    gm.pos.assign(mesh.primitives.size(), {});
    gm.nrm.assign(mesh.primitives.size(), {});
    gm.tan.assign(mesh.primitives.size(), {});
    for (auto& p : mesh.primitives) {
        GpuPrim g;
        g.vertexCount = (int)p.positions.size();
        g.indexCount = (int)p.indices.size();
        g.material = p.material;
        g.hasNormals = p.HasNormals();
        g.hasTangents = p.HasTangents();
        g.hasUV0 = p.HasUV0();
        g.hasUV1 = p.HasUV1();
        g.hasColors = p.HasColors();
        auto make = [&](int slot, const void* data, size_t bytes, UINT bind) {
            if (!data || !bytes) return;
            D3D11_BUFFER_DESC d = {};
            d.ByteWidth = (UINT)bytes;
            d.Usage = D3D11_USAGE_DEFAULT; // スキニングで UpdateSubresource する
            d.BindFlags = bind;
            D3D11_SUBRESOURCE_DATA init = { data, 0, 0 };
            ID3D11Buffer* b = nullptr;
            if (SUCCEEDED(m_dev->CreateBuffer(&d, &init, &b))) {
                if (slot >= 0) g.vb[slot] = b;
                else g.ib = b;
                g.bytes += bytes;
            }
        };
        if (p.positions.empty() || p.indices.empty()) {
            g.indexCount = 0;
            gm.prims.push_back(g);
            continue;
        }
        make(0, p.positions.data(), p.positions.size() * 12, D3D11_BIND_VERTEX_BUFFER);
        if (g.hasNormals) make(1, p.normals.data(), p.normals.size() * 12, D3D11_BIND_VERTEX_BUFFER);
        if (g.hasUV0) make(2, p.uv0.data(), p.uv0.size() * 8, D3D11_BIND_VERTEX_BUFFER);
        if (g.hasColors) make(3, p.colors.data(), p.colors.size() * 16, D3D11_BIND_VERTEX_BUFFER);
        if (g.hasTangents) make(4, p.tangents.data(), p.tangents.size() * 16, D3D11_BIND_VERTEX_BUFFER);
        if (g.hasUV1) make(5, p.uv1.data(), p.uv1.size() * 8, D3D11_BIND_VERTEX_BUFFER);
        std::vector<float> zeros(p.positions.size(), 0.0f);
        make(6, zeros.data(), zeros.size() * 4, D3D11_BIND_VERTEX_BUFFER);
        make(-1, p.indices.data(), p.indices.size() * 4, D3D11_BIND_INDEX_BUFFER);
        if (!g.vb[0] || !g.ib) {
            g.indexCount = 0;
            LOG_WARN("メッシュ '%s' の GPU バッファを作成できません", mesh.name.c_str());
        }
        gm.prims.push_back(g);
    }
    gm.poseRevision = ~0ull;
    gm.weightKey = -2;
}

Renderer::GpuMesh& Renderer::GetMesh(const Scene& scene, int node, bool deform) {
    const Mesh& mesh = scene.meshes[scene.nodes[node].mesh];
    auto key = std::make_pair(mesh.uid, deform ? node : -1);
    auto& gm = m_meshes[key];
    if (gm.prims.empty() && !mesh.primitives.empty()) {
        BuildMesh(gm, mesh);
        gm.deformed = deform;
    }
    gm.used = true;
    return gm;
}

void Renderer::Deform(GpuMesh& gm, const Scene& scene, int nodeIdx, const std::vector<glm::mat4>& world, bool skinning) {
    const Node& node = scene.nodes[nodeIdx];
    const Mesh& mesh = scene.meshes[node.mesh];
    const Skin* skin = (skinning && node.skin >= 0 && node.skin < (int)scene.skins.size()) ? &scene.skins[node.skin] : nullptr;
    std::vector<glm::mat4> jm;
    if (skin) {
        jm.resize(skin->joints.size(), glm::mat4(1));
        for (size_t j = 0; j < skin->joints.size(); ++j) {
            int jn = skin->joints[j];
            glm::mat4 ibm = j < skin->inverseBind.size() ? skin->inverseBind[j] : glm::mat4(1);
            jm[j] = (jn >= 0 && jn < (int)world.size()) ? world[jn] * ibm : glm::mat4(1);
        }
    }
    for (size_t pi = 0; pi < mesh.primitives.size() && pi < gm.prims.size(); ++pi) {
        const Primitive& p = mesh.primitives[pi];
        GpuPrim& g = gm.prims[pi];
        if (!g.vb[0]) continue;
        auto& pos = gm.pos[pi];
        auto& nrm = gm.nrm[pi];
        auto& tan = gm.tan[pi];
        pos = p.positions;
        nrm = p.HasNormals() ? p.normals : std::vector<glm::vec3>();
        tan = p.HasTangents() ? p.tangents : std::vector<glm::vec4>();
        for (size_t t = 0; t < p.targets.size() && t < mesh.morphWeights.size(); ++t) {
            float w = mesh.morphWeights[t];
            if (std::abs(w) < 1e-6f) continue;
            const auto& tg = p.targets[t];
            for (size_t v = 0; v < pos.size() && v < tg.dPositions.size(); ++v) pos[v] += tg.dPositions[v] * w;
            if (!nrm.empty()) for (size_t v = 0; v < nrm.size() && v < tg.dNormals.size(); ++v) nrm[v] += tg.dNormals[v] * w;
        }
        if (skin && p.HasSkin()) {
            for (size_t v = 0; v < pos.size(); ++v) {
                const glm::u16vec4& j = p.joints[v];
                const glm::vec4& w = p.weights[v];
                glm::mat4 m(0.0f);
                float sum = 0;
                for (int k = 0; k < 4; ++k) {
                    if (w[k] <= 0) continue;
                    int ji = j[k] < jm.size() ? j[k] : 0;
                    m += (jm.empty() ? glm::mat4(1) : jm[ji]) * w[k];
                    sum += w[k];
                }
                if (sum <= 0) m = world[nodeIdx];
                pos[v] = glm::vec3(m * glm::vec4(pos[v], 1.0f));
                glm::mat3 m3(m);
                if (!nrm.empty()) { glm::vec3 n = m3 * nrm[v]; float l = glm::length(n); nrm[v] = l > 0 ? n / l : n; }
                if (!tan.empty()) { glm::vec3 t = m3 * glm::vec3(tan[v]); float l = glm::length(t); tan[v] = glm::vec4(l > 0 ? t / l : t, tan[v].w); }
            }
        } else if (!nrm.empty()) {
            for (auto& n : nrm) { float l = glm::length(n); if (l > 0) n /= l; }
        }
        m_ctx->UpdateSubresource(g.vb[0], 0, nullptr, pos.data(), 0, 0);
        if (g.hasNormals && g.vb[1] && nrm.size() == pos.size()) m_ctx->UpdateSubresource(g.vb[1], 0, nullptr, nrm.data(), 0, 0);
        if (g.hasTangents && g.vb[4] && tan.size() == pos.size()) m_ctx->UpdateSubresource(g.vb[4], 0, nullptr, tan.data(), 0, 0);
    }
}

void Renderer::UpdateWeightVis(GpuMesh& gm, const Scene& scene, int nodeIdx, int selectedBone) {
    if (gm.weightKey == selectedBone) return;
    gm.weightKey = selectedBone;
    const Node& node = scene.nodes[nodeIdx];
    const Mesh& mesh = scene.meshes[node.mesh];
    int jointIdx = -1;
    if (node.skin >= 0 && node.skin < (int)scene.skins.size()) {
        auto& joints = scene.skins[node.skin].joints;
        for (size_t j = 0; j < joints.size(); ++j) if (joints[j] == selectedBone) { jointIdx = (int)j; break; }
    }
    for (size_t pi = 0; pi < mesh.primitives.size() && pi < gm.prims.size(); ++pi) {
        const Primitive& p = mesh.primitives[pi];
        if (!gm.prims[pi].vb[6]) continue;
        std::vector<float> w(p.positions.size(), 0.0f);
        if (p.HasSkin() && jointIdx >= 0)
            for (size_t v = 0; v < w.size(); ++v)
                for (int k = 0; k < 4; ++k)
                    if (p.joints[v][k] == jointIdx) w[v] += p.weights[v][k];
        m_ctx->UpdateSubresource(gm.prims[pi].vb[6], 0, nullptr, w.data(), 0, 0);
    }
}

void Renderer::BindPrim(const GpuPrim& g) {
    static const UINT strides[7] = { 12, 12, 8, 16, 16, 8, 4 };
    ID3D11Buffer* bufs[7];
    UINT st[7], offs[7] = {};
    for (int i = 0; i < 7; ++i) {
        bufs[i] = g.vb[i] ? g.vb[i] : m_defaultVB[i];
        st[i] = g.vb[i] ? strides[i] : 0;
    }
    m_ctx->IASetVertexBuffers(0, 7, bufs, st, offs);
    m_ctx->IASetIndexBuffer(g.ib, DXGI_FORMAT_R32_UINT, 0);
}

void Renderer::BindMaterial(const Scene& scene, int matIdx, const RenderSettings& rs, const GpuPrim& g) {
    static const Material kDefault = [] { Material m; m.baseColor = glm::vec4(0.8f, 0.8f, 0.8f, 1.0f); m.roughness = 0.6f; return m; }();
    const Material& m = (matIdx >= 0 && matIdx < (int)scene.materials.size()) ? scene.materials[matIdx] : kDefault;
    g_draw.baseColor = m.baseColor;
    g_draw.emissive = glm::vec4(m.emissive * m.emissiveStrength, m.normalScale);
    g_draw.mat = glm::vec4(m.metallic, m.roughness, m.occlusionStrength, m.alphaCutoff);
    g_draw.flags0 = glm::ivec4((int)m.alphaMode, m.unlit ? 1 : 0, g.hasNormals ? 1 : 0, g.hasTangents ? 1 : 0);
    ID3D11ShaderResourceView* srvs[6] = {};
    ID3D11SamplerState* samps[6] = {};
    int has[5] = {}, uv[5] = {};
    for (int s = 0; s < Slot_Count; ++s) {
        const TextureSlot& slot = m.slots[s];
        bool enabled = rs.useTextures && (s != Slot_Normal || rs.useNormalMaps);
        ID3D11ShaderResourceView* srv = nullptr;
        int samp = 0;
        if (enabled && slot.texture >= 0 && slot.texture < (int)scene.textures.size()) {
            srv = (ID3D11ShaderResourceView*)GetTextureSRV(scene.textures[slot.texture]);
            samp = SamplerIndex(&scene.textures[slot.texture]);
        }
        has[s] = srv ? 1 : 0;
        uv[s] = slot.uvSet;
        srvs[s] = srv ? srv : (s == Slot_Normal ? m_normalSRV : m_whiteSRV);
        samps[s] = m_samplers[samp];
    }
    srvs[5] = m_checkerSRV;
    samps[5] = m_samplers[0];
    g_draw.hasTex = glm::ivec4(has[0], has[1], has[2], has[3]);
    g_draw.uvSet = glm::ivec4(uv[0], uv[1], uv[2], uv[3]);
    g_draw.flags1 = glm::ivec4((rs.useVertexColors && g.hasColors) ? 1 : 0, has[4], uv[4], 0);
    m_ctx->PSSetShaderResources(0, 6, srvs);
    m_ctx->PSSetSamplers(0, 6, samps);
    m_ctx->RSSetState((m.doubleSided || !rs.backfaceCulling) ? m_rsNoCull : m_rsCull);
}

// ---------------------------------------------------------------------------
void Renderer::AddLine(const glm::vec3& a, const glm::vec3& b, uint32_t color, bool onTop) {
    auto& v = onTop ? m_extraLinesTop : m_extraLines;
    v.push_back({ a, color });
    v.push_back({ b, color });
}

void Renderer::DrawLines(const std::vector<LineVert>& lines, bool depthTest) {
    if (lines.empty()) return;
    size_t bytes = lines.size() * sizeof(LineVert);
    if (bytes > m_lineVBCapacity) {
        SafeRelease(m_lineVB);
        m_lineVBCapacity = std::max<size_t>(bytes * 2, 64 * 1024);
        D3D11_BUFFER_DESC d = {};
        d.ByteWidth = (UINT)m_lineVBCapacity;
        d.Usage = D3D11_USAGE_DYNAMIC;
        d.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(m_dev->CreateBuffer(&d, nullptr, &m_lineVB))) { m_lineVBCapacity = 0; return; }
    }
    D3D11_MAPPED_SUBRESOURCE ms;
    if (FAILED(m_ctx->Map(m_lineVB, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) return;
    memcpy(ms.pData, lines.data(), bytes);
    m_ctx->Unmap(m_lineVB, 0);
    UINT stride = sizeof(LineVert), offset = 0;
    m_ctx->IASetInputLayout(m_lineLayout);
    m_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
    m_ctx->IASetVertexBuffers(0, 1, &m_lineVB, &stride, &offset);
    m_ctx->VSSetShader(m_lineVS, nullptr, 0);
    m_ctx->PSSetShader(m_linePS, nullptr, 0);
    m_ctx->VSSetConstantBuffers(0, 1, &m_cbFrame);
    m_ctx->RSSetState(m_rsNoCull);
    m_ctx->OMSetDepthStencilState(depthTest ? m_dsTestNoWrite : m_dsOff, 0);
    m_ctx->Draw((UINT)lines.size(), 0);
    m_stats.drawCalls++;
}

static uint32_t RGBA(int r, int g, int b, int a = 255) { return (uint32_t)r | ((uint32_t)g << 8) | ((uint32_t)b << 16) | ((uint32_t)a << 24); }

void Renderer::BuildOverlays(const FrameInput& in) {
    const Scene& scene = *in.scene;
    const auto& world = *in.world;
    const RenderSettings& rs = *in.settings;
    m_lines.clear();
    m_linesTop.clear();
    auto line = [&](std::vector<LineVert>& v, const glm::vec3& a, const glm::vec3& b, uint32_t c) {
        v.push_back({ a, c });
        v.push_back({ b, c });
    };
    const float R = std::max(in.sceneRadius, 1e-4f);

    if (rs.showGrid) {
        // 固定サイズのグリッド (モデルの大きさで変化しない基準)
        const float step = std::max(rs.gridSpacing, 1e-5f);
        const int major = std::max(rs.gridMajorEvery, 1);
        const int n = std::clamp((int)std::round(rs.gridExtent / step), 1, 2000);
        const float ext = step * n;
        for (int i = -n; i <= n; ++i) {
            uint32_t c = (i == 0) ? RGBA(150, 150, 150, 230) : (i % major == 0 ? RGBA(105, 105, 105, 200) : RGBA(70, 70, 70, 130));
            line(m_lines, { i * step, 0, -ext }, { i * step, 0, ext }, c);
            line(m_lines, { -ext, 0, i * step }, { ext, 0, i * step }, c);
        }
    }
    if (rs.showOriginAxes) {
        float a = std::max(rs.gridSpacing * rs.gridMajorEvery, 1e-4f); // 太線 1 マス分 (既定 1 m)
        line(m_lines, { 0, 0, 0 }, { a, 0, 0 }, RGBA(230, 60, 60));
        line(m_lines, { 0, 0, 0 }, { 0, a, 0 }, RGBA(60, 230, 60));
        line(m_lines, { 0, 0, 0 }, { 0, 0, a }, RGBA(70, 110, 240));
    }
    if (scene.nodes.empty()) return;

    std::vector<bool> selected(scene.nodes.size(), false);
    for (int s : in.selection) if (s >= 0 && s < (int)selected.size()) selected[s] = true;

    if (rs.showBones && !scene.skins.empty()) {
        auto joints = scene.JointMask();
        auto& dst = rs.bonesOnTop ? m_linesTop : m_lines;
        float js = R * 0.01f * rs.boneSize;
        for (int i = 0; i < (int)scene.nodes.size(); ++i) {
            if (!joints[i]) continue;
            glm::vec3 p(world[i][3]);
            bool sel = i == in.selectedBone || selected[i];
            uint32_t c = sel ? RGBA(255, 220, 40) : RGBA(80, 200, 255);
            int parent = scene.nodes[i].parent;
            if (parent >= 0 && joints[parent]) {
                glm::vec3 pp(world[parent][3]);
                glm::vec3 d = p - pp;
                float len = glm::length(d);
                if (len > 1e-6f) {
                    glm::vec3 dir = d / len;
                    glm::vec3 up = std::abs(dir.y) < 0.99f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0);
                    glm::vec3 s1 = glm::normalize(glm::cross(dir, up)) * len * 0.08f;
                    glm::vec3 s2 = glm::normalize(glm::cross(dir, s1)) * len * 0.08f;
                    glm::vec3 mid = pp + d * 0.2f;
                    bool parentSel = parent == in.selectedBone || selected[parent];
                    uint32_t bc = parentSel ? RGBA(255, 220, 40) : RGBA(80, 200, 255, 220);
                    glm::vec3 q[4] = { mid + s1, mid + s2, mid - s1, mid - s2 };
                    for (int k = 0; k < 4; ++k) {
                        line(dst, pp, q[k], bc);
                        line(dst, q[k], p, bc);
                        line(dst, q[k], q[(k + 1) % 4], bc);
                    }
                }
            }
            line(dst, p - glm::vec3(js, 0, 0), p + glm::vec3(js, 0, 0), c);
            line(dst, p - glm::vec3(0, js, 0), p + glm::vec3(0, js, 0), c);
            line(dst, p - glm::vec3(0, 0, js), p + glm::vec3(0, 0, js), c);
        }
    }

    if (rs.showNodeAxes) {
        float a = R * 0.03f;
        for (size_t i = 0; i < scene.nodes.size(); ++i) {
            glm::vec3 p(world[i][3]);
            glm::mat3 m(world[i]);
            line(m_linesTop, p, p + glm::normalize(m[0]) * a, RGBA(230, 60, 60));
            line(m_linesTop, p, p + glm::normalize(m[1]) * a, RGBA(60, 230, 60));
            line(m_linesTop, p, p + glm::normalize(m[2]) * a, RGBA(70, 110, 240));
        }
    }

    bool needVerts = rs.showNormals || rs.showTangents || rs.showBounds;
    if (!needVerts) return;
    const float nl = R * rs.normalLength;
    for (int i = 0; i < (int)scene.nodes.size(); ++i) {
        const Node& n = scene.nodes[i];
        if (n.mesh < 0 || n.mesh >= (int)scene.meshes.size() || !n.visible) continue;
        if (rs.debugSelectedOnly && !selected[i]) continue;
        const Mesh& mesh = scene.meshes[n.mesh];
        bool skinned = rs.skinningEnabled && n.skin >= 0 && mesh.HasSkin();
        bool morphed = false;
        for (float w : mesh.morphWeights) if (std::abs(w) > 1e-6f) morphed = true;
        GpuMesh* gm = nullptr;
        auto it = m_meshes.find({ mesh.uid, (skinned || morphed) ? i : -1 });
        if (it != m_meshes.end()) gm = &it->second;
        glm::mat4 M = skinned ? glm::mat4(1) : world[i];
        glm::mat3 NM = glm::inverseTranspose(glm::mat3(M));
        for (size_t pi = 0; pi < mesh.primitives.size(); ++pi) {
            const Primitive& p = mesh.primitives[pi];
            const std::vector<glm::vec3>& pos = (gm && gm->deformed && pi < gm->pos.size() && !gm->pos[pi].empty()) ? gm->pos[pi] : p.positions;
            const std::vector<glm::vec3>& nrm = (gm && gm->deformed && pi < gm->nrm.size() && !gm->nrm[pi].empty()) ? gm->nrm[pi] : p.normals;
            const std::vector<glm::vec4>& tan = (gm && gm->deformed && pi < gm->tan.size() && !gm->tan[pi].empty()) ? gm->tan[pi] : p.tangents;
            size_t stride = std::max<size_t>(1, pos.size() / 20000);
            if (rs.showNormals && nrm.size() == pos.size())
                for (size_t v = 0; v < pos.size(); v += stride) {
                    glm::vec3 a = glm::vec3(M * glm::vec4(pos[v], 1));
                    line(m_lines, a, a + glm::normalize(NM * nrm[v]) * nl, RGBA(80, 160, 255));
                }
            if (rs.showTangents && tan.size() == pos.size())
                for (size_t v = 0; v < pos.size(); v += stride) {
                    glm::vec3 a = glm::vec3(M * glm::vec4(pos[v], 1));
                    line(m_lines, a, a + glm::normalize(glm::mat3(M) * glm::vec3(tan[v])) * nl, RGBA(255, 90, 90));
                    if (nrm.size() == pos.size()) {
                        glm::vec3 b = glm::cross(nrm[v], glm::vec3(tan[v])) * tan[v].w;
                        line(m_lines, a, a + glm::normalize(glm::mat3(M) * b) * nl, RGBA(90, 255, 90));
                    }
                }
            if (rs.showBounds && !pos.empty()) {
                glm::vec3 mn(1e30f), mx(-1e30f);
                for (auto& v : pos) { mn = glm::min(mn, v); mx = glm::max(mx, v); }
                glm::vec3 c[8];
                for (int k = 0; k < 8; ++k)
                    c[k] = glm::vec3(M * glm::vec4((k & 1) ? mx.x : mn.x, (k & 2) ? mx.y : mn.y, (k & 4) ? mx.z : mn.z, 1));
                static const int e[12][2] = { {0,1},{2,3},{4,5},{6,7},{0,2},{1,3},{4,6},{5,7},{0,4},{1,5},{2,6},{3,7} };
                for (auto& ed : e) line(m_lines, c[ed[0]], c[ed[1]], RGBA(255, 200, 60));
            }
        }
    }
}

void Renderer::Render(const FrameInput& in) {
    m_stats = RenderStats();
    if (!m_dev) return;
    const RenderSettings& rs = *in.settings;
    if (!EnsureTargets(in.width, in.height, rs.msaa)) return;
    for (auto& [k, m] : m_meshes) m.used = false;
    for (auto& [k, t] : m_textures) t.used = false;

    const float aspect = (float)in.width / std::max(in.height, 1);
    glm::mat4 view = in.camera->View();
    glm::mat4 proj = in.camera->Projection(aspect);
    m_viewProj = proj * view;
    glm::vec3 camPos = in.camera->Position();

    CBFrame cf;
    cf.viewProj = m_viewProj;
    cf.camPos = glm::vec4(camPos, 1);
    cf.lightDir = glm::vec4(glm::normalize(rs.lightDir), 0);
    cf.params = glm::vec4(rs.lightIntensity, rs.ambient, rs.exposure, (float)rs.mode);
    m_ctx->UpdateSubresource(m_cbFrame, 0, nullptr, &cf, 0, 0);

    D3D11_VIEWPORT vp = { 0, 0, (float)in.width, (float)in.height, 0, 1 };
    m_ctx->RSSetViewports(1, &vp);
    m_ctx->OMSetRenderTargets(1, &m_msRTV, m_msDSV);
    float clear[4] = { rs.bgColor.r, rs.bgColor.g, rs.bgColor.b, 1.0f };
    m_ctx->ClearRenderTargetView(m_msRTV, clear);
    m_ctx->ClearDepthStencilView(m_msDSV, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
    float bf[4] = {};
    m_ctx->OMSetBlendState(m_bsAlpha, bf, 0xFFFFFFFF);

    if (in.scene && !in.scene->Empty()) {
        const Scene& scene = *in.scene;
        const auto& world = *in.world;
        std::vector<bool> selected(scene.nodes.size(), false);
        for (int s : in.selection) if (s >= 0 && s < (int)selected.size()) selected[s] = true;

        std::vector<bool> visible(scene.nodes.size(), true);
        for (int i = 0; i < (int)scene.nodes.size(); ++i) {
            int p = i, guard = 0;
            while (p >= 0 && guard++ < 4096) {
                if (!scene.nodes[p].visible) { visible[i] = false; break; }
                p = scene.nodes[p].parent;
            }
        }

        struct DrawItem {
            int node;
            size_t prim;
            GpuMesh* gm;
            glm::mat4 model;
            float depth;
            AlphaMode alpha;
        };
        std::vector<DrawItem> items;
        auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < (int)scene.nodes.size(); ++i) {
            const Node& n = scene.nodes[i];
            if (n.mesh < 0 || n.mesh >= (int)scene.meshes.size() || !visible[i]) continue;
            const Mesh& mesh = scene.meshes[n.mesh];
            bool skinned = rs.skinningEnabled && n.skin >= 0 && n.skin < (int)scene.skins.size() && mesh.HasSkin();
            bool morphed = false;
            for (float w : mesh.morphWeights) if (std::abs(w) > 1e-6f) morphed = true;
            bool deform = skinned || morphed;
            GpuMesh& gm = GetMesh(scene, i, deform);
            if (deform) {
                if (gm.poseRevision != in.poseRevision) {
                    Deform(gm, scene, i, world, skinned);
                    gm.poseRevision = in.poseRevision;
                }
                m_stats.deformedMeshes++;
            }
            if (rs.mode == RM_Weights) UpdateWeightVis(gm, scene, i, in.selectedBone);
            glm::mat4 model = skinned ? glm::mat4(1.0f) : world[i];
            for (size_t pi = 0; pi < gm.prims.size(); ++pi) {
                if (gm.prims[pi].indexCount == 0) continue;
                int mat = gm.prims[pi].material;
                AlphaMode am = (mat >= 0 && mat < (int)scene.materials.size()) ? scene.materials[mat].alphaMode : AlphaMode::Opaque;
                items.push_back({ i, pi, &gm, model, glm::length(glm::vec3(model[3]) - camPos), am });
            }
        }
        m_stats.deformMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        std::stable_sort(items.begin(), items.end(), [](const DrawItem& a, const DrawItem& b) {
            if (a.alpha != b.alpha) return (int)a.alpha < (int)b.alpha;
            if (a.alpha == AlphaMode::Blend) return a.depth > b.depth;
            return false;
        });

        m_ctx->IASetInputLayout(m_meshLayout);
        m_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_ctx->VSSetShader(m_meshVS, nullptr, 0);
        m_ctx->PSSetShader(m_meshPS, nullptr, 0);
        ID3D11Buffer* cbs[2] = { m_cbFrame, m_cbDraw };
        m_ctx->VSSetConstantBuffers(0, 2, cbs);
        m_ctx->PSSetConstantBuffers(0, 2, cbs);

        auto drawItem = [&](const DrawItem& it, const glm::vec4& overrideColor) {
            const GpuPrim& g = it.gm->prims[it.prim];
            g_draw.model = it.model;
            g_draw.normalMat = glm::mat4(glm::inverseTranspose(glm::mat3(it.model)));
            g_draw.overrideColor = overrideColor;
            m_ctx->UpdateSubresource(m_cbDraw, 0, nullptr, &g_draw, 0, 0);
            BindPrim(g);
            m_ctx->DrawIndexed(g.indexCount, 0, 0);
            m_stats.drawCalls++;
            m_stats.triangles += g.indexCount / 3;
            m_stats.vertices += g.vertexCount;
        };

        if (!rs.wireframe) {
            for (auto& it : items) {
                const GpuPrim& g = it.gm->prims[it.prim];
                BindMaterial(scene, g.material, rs, g);
                m_ctx->OMSetDepthStencilState(it.alpha == AlphaMode::Blend ? m_dsTestNoWrite : m_dsTestWrite, 0);
                drawItem(it, glm::vec4(0));
            }
        }
        if (rs.wireframe || rs.wireOnShaded || rs.highlightSelection) {
            m_ctx->OMSetDepthStencilState(m_dsTestWrite, 0);
            for (auto& it : items) {
                bool sel = selected[it.node];
                if (!(rs.wireframe || rs.wireOnShaded || sel)) continue;
                const GpuPrim& g = it.gm->prims[it.prim];
                BindMaterial(scene, g.material, rs, g);
                m_ctx->RSSetState(m_rsWire);
                glm::vec4 c = (rs.wireframe || rs.wireOnShaded)
                                  ? (sel ? glm::vec4(1.0f, 0.6f, 0.1f, 1.0f) : glm::vec4(0.75f, 0.78f, 0.82f, rs.wireframe ? 0.9f : 0.35f))
                                  : glm::vec4(1.0f, 0.6f, 0.1f, 0.55f);
                drawItem(it, c);
            }
        }
        BuildOverlays(in);
    } else {
        static Scene emptyScene;
        static std::vector<glm::mat4> emptyWorld;
        FrameInput overlayIn = in;
        overlayIn.scene = &emptyScene;
        overlayIn.world = &emptyWorld;
        BuildOverlays(overlayIn);
    }
    DrawLines(m_lines, true);
    DrawLines(m_extraLines, true);
    DrawLines(m_linesTop, false);
    DrawLines(m_extraLinesTop, false);
    m_extraLines.clear();
    m_extraLinesTop.clear();

    // MSAA を解決して ImGui に渡す。パイプラインからは外しておく
    m_ctx->OMSetRenderTargets(0, nullptr, nullptr);
    ID3D11ShaderResourceView* nullSRV[6] = {};
    m_ctx->PSSetShaderResources(0, 6, nullSRV);
    m_ctx->ResolveSubresource(m_resolveTex, 0, m_msColor, 0, DXGI_FORMAT_R8G8B8A8_UNORM);

    for (auto it = m_meshes.begin(); it != m_meshes.end();) {
        if (!it->second.used) {
            DestroyMesh(it->second);
            it = m_meshes.erase(it);
        } else {
            for (auto& p : it->second.prims) m_stats.gpuBufferBytes += p.bytes;
            ++it;
        }
    }
    m_stats.cachedMeshes = (int)m_meshes.size();
    m_stats.cachedTextures = (int)m_textures.size();
    for (auto& [k, t] : m_textures) m_stats.gpuTextureBytes += t.bytes;
}

int Renderer::Pick(const FrameInput& in, int x, int y) {
    if (!in.scene || in.scene->Empty() || !m_pickRTV) return -1;
    if (x < 0 || y < 0 || x >= m_w || y >= m_h) return -1;
    const Scene& scene = *in.scene;
    const auto& world = *in.world;
    const RenderSettings& rs = *in.settings;
    D3D11_VIEWPORT vp = { 0, 0, (float)m_w, (float)m_h, 0, 1 };
    m_ctx->RSSetViewports(1, &vp);
    m_ctx->OMSetRenderTargets(1, &m_pickRTV, m_pickDSV);
    float zero[4] = { 0, 0, 0, 0 };
    m_ctx->ClearRenderTargetView(m_pickRTV, zero);
    m_ctx->ClearDepthStencilView(m_pickDSV, D3D11_CLEAR_DEPTH, 1.0f, 0);
    float bf[4] = {};
    m_ctx->OMSetBlendState(m_bsOpaque, bf, 0xFFFFFFFF);
    m_ctx->OMSetDepthStencilState(m_dsTestWrite, 0);
    m_ctx->RSSetState(m_rsNoCull);
    m_ctx->IASetInputLayout(m_meshLayout);
    m_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_ctx->VSSetShader(m_meshVS, nullptr, 0);
    m_ctx->PSSetShader(m_pickPS, nullptr, 0);
    ID3D11Buffer* cbs[2] = { m_cbFrame, m_cbDraw };
    m_ctx->VSSetConstantBuffers(0, 2, cbs);
    m_ctx->PSSetConstantBuffers(0, 2, cbs);
    for (int i = 0; i < (int)scene.nodes.size(); ++i) {
        const Node& n = scene.nodes[i];
        if (n.mesh < 0 || n.mesh >= (int)scene.meshes.size() || !n.visible) continue;
        const Mesh& mesh = scene.meshes[n.mesh];
        bool skinned = rs.skinningEnabled && n.skin >= 0 && mesh.HasSkin();
        bool morphed = false;
        for (float w : mesh.morphWeights) if (std::abs(w) > 1e-6f) morphed = true;
        auto it = m_meshes.find({ mesh.uid, (skinned || morphed) ? i : -1 });
        if (it == m_meshes.end()) continue;
        g_draw.model = skinned ? glm::mat4(1) : world[i];
        uint32_t id = (uint32_t)i + 1;
        g_draw.pickColor = glm::vec4((id & 0xFF) / 255.0f, ((id >> 8) & 0xFF) / 255.0f, ((id >> 16) & 0xFF) / 255.0f, 1.0f);
        m_ctx->UpdateSubresource(m_cbDraw, 0, nullptr, &g_draw, 0, 0);
        for (auto& g : it->second.prims) {
            if (!g.indexCount) continue;
            BindPrim(g);
            m_ctx->DrawIndexed(g.indexCount, 0, 0);
        }
    }
    m_ctx->OMSetRenderTargets(0, nullptr, nullptr);
    D3D11_BOX box = { (UINT)x, (UINT)y, 0, (UINT)x + 1, (UINT)y + 1, 1 };
    m_ctx->CopySubresourceRegion(m_pickStaging, 0, 0, 0, 0, m_pickTex, 0, &box);
    D3D11_MAPPED_SUBRESOURCE ms;
    uint32_t idv = 0;
    if (SUCCEEDED(m_ctx->Map(m_pickStaging, 0, D3D11_MAP_READ, 0, &ms))) {
        const uint8_t* px = (const uint8_t*)ms.pData;
        idv = px[0] | (px[1] << 8) | (px[2] << 16);
        m_ctx->Unmap(m_pickStaging, 0);
    }
    return idv == 0 ? -1 : (int)idv - 1;
}
