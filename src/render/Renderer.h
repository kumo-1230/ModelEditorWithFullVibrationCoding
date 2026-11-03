#pragma once
// Direct3D 11 レンダラー。GPU が無い/ドライバが古い環境でも WARP (ソフトウェア) で動作する。
#include "Camera.h"
#include "core/Scene.h"
#include <d3d11.h>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

enum RenderMode {
    RM_Lit = 0,
    RM_BaseColor,
    RM_Normals,
    RM_UV,
    RM_Checker,
    RM_Weights,
    RM_VertexColor,
    RM_MetalRough,
    RM_Occlusion,
    RM_Tangents,
    RM_Backface,     // 表面は灰色、裏面は赤 (面の向きの確認。カリングしない)
    RM_NormalCheck,  // 頂点法線が面の向きと逆なら桃色 (法線の裏返りの確認)
    RM_Influences,   // 1 頂点に影響するボーンの数 (青 = 1 本 … 赤 = 4 本)
    RM_Count
};
const char* RenderModeName(int mode);

struct RenderSettings {
    int mode = RM_Lit;
    bool wireframe = false;           // 全体をワイヤーフレーム表示
    bool wireOnShaded = false;        // シェーディング + ワイヤー
    bool highlightSelection = true;   // 選択をワイヤーで強調
    bool showGrid = true;
    // 床グリッド (Maya と同様にシーンの大きさに関係なく固定。単位はメートル)
    float gridSpacing = 0.1f;   // 1 マスの大きさ
    int gridMajorEvery = 10;    // 太線の間隔 (マス数)
    float gridExtent = 10.0f;   // 原点からの範囲 (±)
    bool showOriginAxes = true;
    bool showBones = true;
    bool bonesOnTop = true;
    bool showNormals = false;
    bool showTangents = false;
    bool debugSelectedOnly = true;    // 法線等は選択のみ
    bool showBounds = false;
    bool showNodeAxes = false;
    bool backfaceCulling = true;
    bool useTextures = true;
    bool useNormalMaps = true;
    bool useVertexColors = false;
    bool skinningEnabled = true;
    int msaa = 4;
    float normalLength = 0.05f;       // シーン半径に対する比率
    float boneSize = 1.0f;
    glm::vec3 bgColor{ 0.20f, 0.21f, 0.23f };
    glm::vec3 lightDir{ -0.4f, -0.8f, -0.45f };
    float lightIntensity = 3.0f;
    float ambient = 0.35f;
    float exposure = 1.0f;
};

struct FrameInput {
    const Scene* scene = nullptr;
    const std::vector<glm::mat4>* world = nullptr;
    const RenderSettings* settings = nullptr;
    const OrbitCamera* camera = nullptr;
    std::vector<int> selection;
    int selectedBone = -1;
    int width = 1, height = 1;
    uint64_t poseRevision = 0;   // 変化したら変形を再計算
    float sceneRadius = 1.0f;
};

struct RenderStats {
    int drawCalls = 0;
    size_t triangles = 0;
    size_t vertices = 0;
    int deformedMeshes = 0;
    double deformMs = 0.0;
    size_t gpuBufferBytes = 0;
    size_t gpuTextureBytes = 0;
    int cachedMeshes = 0;
    int cachedTextures = 0;
};

class Renderer {
public:
    bool Init(ID3D11Device* device, ID3D11DeviceContext* ctx, const std::string& adapterInfo);
    void Shutdown();

    void Render(const FrameInput& in);
    void* ColorTexture() const { return m_resolveSRV; }  // ImGui::Image 用 (ID3D11ShaderResourceView*)
    // ビューポート座標 (左上原点) でノード番号を返す
    int Pick(const FrameInput& in, int x, int y);

    void* GetTextureSRV(const Texture& tex);
    void* CheckerTexture() const { return m_checkerSRV; }
    // GPU キャッシュの破棄を予約する (描画中の ImGui が参照している可能性があるため次フレーム頭で破棄)
    void ClearCaches() { m_clearRequested = true; }
    void BeginFrame();
    const RenderStats& Stats() const { return m_stats; }
    std::string GPUInfo() const { return m_adapterInfo; }
    bool DeviceLost() const;

    // 外部から追加するデバッグ線 (フレームごとにクリア)
    void AddLine(const glm::vec3& a, const glm::vec3& b, uint32_t color, bool onTop = false);

private:
    struct GpuPrim {
        ID3D11Buffer* vb[7] = {};
        ID3D11Buffer* ib = nullptr;
        int indexCount = 0;
        int vertexCount = 0;
        bool hasNormals = false, hasTangents = false, hasUV0 = false, hasUV1 = false, hasColors = false;
        int material = -1;
        size_t bytes = 0;
    };
    struct GpuMesh {
        std::vector<GpuPrim> prims;
        uint64_t uid = 0;
        uint64_t poseRevision = ~0ull;
        int weightKey = -2;
        bool used = false;
        bool deformed = false;
        std::vector<std::vector<glm::vec3>> pos, nrm;   // 変形後 (デバッグ描画用)
        std::vector<std::vector<glm::vec4>> tan;
    };
    struct LineVert {
        glm::vec3 p;
        uint32_t c;
    };
    struct TexEntry {
        ID3D11ShaderResourceView* srv = nullptr;
        bool used = false;
        size_t bytes = 0;
        int sampler = 0;
    };

    void ClearCachesNow();
    bool m_clearRequested = false;
    bool EnsureTargets(int w, int h, int msaa);
    void ReleaseTargets();
    GpuMesh& GetMesh(const Scene& scene, int node, bool deform);
    void BuildMesh(GpuMesh& gm, const Mesh& mesh);
    void Deform(GpuMesh& gm, const Scene& scene, int nodeIdx, const std::vector<glm::mat4>& world, bool skinning);
    void UpdateWeightVis(GpuMesh& gm, const Scene& scene, int nodeIdx, int selectedBone);
    void DestroyMesh(GpuMesh& gm);
    void DrawLines(const std::vector<LineVert>& lines, bool depthTest);
    void BuildOverlays(const FrameInput& in);
    void BindMaterial(const Scene& scene, int matIdx, const RenderSettings& rs, const GpuPrim& g);
    void BindPrim(const GpuPrim& g);
    ID3D11ShaderResourceView* CreateTexture(int w, int h, const void* rgba, bool mips, size_t* bytesOut);
    int SamplerIndex(const Texture* tex) const;

    ID3D11Device* m_dev = nullptr;
    ID3D11DeviceContext* m_ctx = nullptr;
    std::string m_adapterInfo;

    ID3D11VertexShader* m_meshVS = nullptr;
    ID3D11PixelShader* m_meshPS = nullptr;
    ID3D11InputLayout* m_meshLayout = nullptr;
    ID3D11VertexShader* m_lineVS = nullptr;
    ID3D11PixelShader* m_linePS = nullptr;
    ID3D11InputLayout* m_lineLayout = nullptr;
    ID3D11PixelShader* m_pickPS = nullptr;
    ID3D11Buffer* m_cbFrame = nullptr;
    ID3D11Buffer* m_cbDraw = nullptr;

    ID3D11RasterizerState* m_rsCull = nullptr;
    ID3D11RasterizerState* m_rsNoCull = nullptr;
    ID3D11RasterizerState* m_rsWire = nullptr;
    ID3D11DepthStencilState* m_dsTestWrite = nullptr;
    ID3D11DepthStencilState* m_dsTestNoWrite = nullptr;
    ID3D11DepthStencilState* m_dsOff = nullptr;
    ID3D11BlendState* m_bsAlpha = nullptr;
    ID3D11BlendState* m_bsOpaque = nullptr;
    ID3D11SamplerState* m_samplers[18] = {}; // フィルタ(2) x wrapS(3) x wrapT(3)

    // 頂点属性が無いとき用 (stride 0 で全頂点に同じ値)
    ID3D11Buffer* m_defaultVB[7] = {};

    ID3D11Texture2D* m_msColor = nullptr;
    ID3D11RenderTargetView* m_msRTV = nullptr;
    ID3D11Texture2D* m_msDepth = nullptr;
    ID3D11DepthStencilView* m_msDSV = nullptr;
    ID3D11Texture2D* m_resolveTex = nullptr;
    ID3D11ShaderResourceView* m_resolveSRV = nullptr;
    ID3D11Texture2D* m_pickTex = nullptr;
    ID3D11RenderTargetView* m_pickRTV = nullptr;
    ID3D11Texture2D* m_pickDepth = nullptr;
    ID3D11DepthStencilView* m_pickDSV = nullptr;
    ID3D11Texture2D* m_pickStaging = nullptr;
    int m_w = 0, m_h = 0, m_samples = 0;

    ID3D11Buffer* m_lineVB = nullptr;
    size_t m_lineVBCapacity = 0;
    ID3D11ShaderResourceView* m_whiteSRV = nullptr;
    ID3D11ShaderResourceView* m_normalSRV = nullptr;
    ID3D11ShaderResourceView* m_checkerSRV = nullptr;

    std::map<std::pair<uint64_t, int>, GpuMesh> m_meshes; // (mesh uid, node or -1)
    std::unordered_map<uint64_t, TexEntry> m_textures;
    std::vector<LineVert> m_lines, m_linesTop, m_extraLines, m_extraLinesTop;
    RenderStats m_stats;
    glm::mat4 m_viewProj{ 1.0f };
};
