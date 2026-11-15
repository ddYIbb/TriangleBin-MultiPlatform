#include "renderer_dx11.h"
#include "compiled_shaders.h"

#include <algorithm>
#include <cstring>
#include <cwchar>

namespace {

const char* kVtxShader = R"hlsl(
cbuffer VColorBuffer : register(b0) {
    float4 vcolor[7];
};

struct VSIn {
    float4 pos : POSITION;
};

struct VSOut {
    float4 pos : SV_Position;
    float4 color : COLOR0;
};

VSOut VSMain(VSIn input, uint vertexId : SV_VertexID) {
    VSOut output;
    output.pos = input.pos;
    output.color = vcolor[vertexId % 7];
    return output;
}
)hlsl";

const char* kFragShader = R"hlsl(
RWByteAddressBuffer counter : register(u1);

cbuffer FrameParams : register(b1) {
    float4 uniformcolor;
    uint frags_to_shade;
    uint3 pad;
};

struct PSIn {
    float4 pos : SV_Position;
    float4 color : COLOR0;
};

float4 PSMain(PSIn input) : SV_Target {
    uint cur = counter.Load(0);
    if (cur > frags_to_shade) {
        return float4(0.0, 0.0, 0.0, 0.0);
    }
    uint prev;
    counter.InterlockedAdd(0, 1, prev);
    if (prev > frags_to_shade) {
        return float4(0.0, 0.0, 0.0, 0.0);
    }
    return input.color;
}
)hlsl";

struct FrameCB {
    float color[4];
    uint32_t frags_to_shade;
    uint32_t pad[3];
};

} // namespace

RendererD3D11::RendererD3D11() {}

bool RendererD3D11::init(HWND hwnd, int w, int h, int adapterIndex) {
    static const D3D_FEATURE_LEVEL featureLevels[] = {
        D3D_FEATURE_LEVEL_12_1,
        D3D_FEATURE_LEVEL_12_0,
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0
    };
    const UINT numLevels = (UINT)(sizeof(featureLevels) / sizeof(featureLevels[0]));

        // Enumerate every DXGI adapter and pick the best hardware one: highest
        // supported feature level first, then largest dedicated video memory.
        ComPtr<IDXGIFactory1> factory1;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory1)))) {
            Log(LOG_ERROR) << "CreateDXGIFactory1 (D3D11) failed";
            return false;
        }
        ComPtr<IDXGIFactory4> factory4;
        factory1.As(&factory4);

        ComPtr<ID3D11Device> bestDevice;
        ComPtr<ID3D11DeviceContext> bestCtx;
        D3D_FEATURE_LEVEL bestFL = (D3D_FEATURE_LEVEL)0;
        SIZE_T bestVRAM = 0;
        bool bestSoftware = false;
        int bestIndex = -1;
        std::vector<DXGI_ADAPTER_DESC1> seenAdapters;

        for (UINT i = 0; ; ++i) {
            ComPtr<IDXGIAdapter1> adapter1;
            HRESULT hr = factory4
                ? factory4->EnumAdapters1(i, &adapter1)
                : factory1->EnumAdapters(i, (IDXGIAdapter**)adapter1.GetAddressOf());
            if (hr == DXGI_ERROR_NOT_FOUND) break;
            if (FAILED(hr)) continue;

            // When an adapter was explicitly chosen, only consider that one.
            if (adapterIndex >= 0 && (int)i != adapterIndex) continue;

            DXGI_ADAPTER_DESC1 desc1 = {};
            bool software = false;
            bool haveDesc = SUCCEEDED(adapter1->GetDesc1(&desc1));
            if (haveDesc) {
                software = (desc1.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
                // Skip duplicate virtual adapters before creating a D3D11 device
                // for each of them (same physical GPU, different DXGI LUID).
                const auto duplicate = std::find_if(
                    seenAdapters.begin(), seenAdapters.end(),
                    [&](const DXGI_ADAPTER_DESC1& seen) {
                        return seen.VendorId == desc1.VendorId &&
                               seen.DeviceId == desc1.DeviceId &&
                               seen.SubSysId == desc1.SubSysId &&
                               seen.Revision == desc1.Revision &&
                               wcscmp(seen.Description, desc1.Description) == 0;
                    });
                if (duplicate != seenAdapters.end()) continue;
                seenAdapters.push_back(desc1);
            } else {
                ComPtr<IDXGIAdapter> baseAdapter;
                adapter1.As(&baseAdapter);
                DXGI_ADAPTER_DESC desc0 = {};
                if (SUCCEEDED(baseAdapter->GetDesc(&desc0))) {
                    desc1.DedicatedVideoMemory = desc0.DedicatedVideoMemory;
                }
            }
            // Hardware rendering only: skip software (WARP) adapters.
            if (software) continue;

            ComPtr<ID3D11Device> dev;
            ComPtr<ID3D11DeviceContext> ctx;
            D3D_FEATURE_LEVEL fl = (D3D_FEATURE_LEVEL)0;
            hr = D3D11CreateDevice(adapter1.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                                   D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                   featureLevels, numLevels, D3D11_SDK_VERSION,
                                   &dev, &fl, &ctx);
            if (hr == E_INVALIDARG) {
                // Windows 7's D3D11 runtime does not know feature levels above
                // 11_0. Retry with the legacy-compatible list; Windows 10+
                // still takes the normal path above.
                static const D3D_FEATURE_LEVEL legacyLevels[] = {
                    D3D_FEATURE_LEVEL_11_0
                };
                hr = D3D11CreateDevice(adapter1.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                                       D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                       legacyLevels, 1, D3D11_SDK_VERSION,
                                       &dev, &fl, &ctx);
            }
            if (FAILED(hr)) continue;

            const bool haveHardware = bestDevice && !bestSoftware;
            bool better = false;
            if (!haveHardware) {
                if (!software) {
                    better = true; // first hardware candidate always preferred
                } else if (!bestDevice) {
                    better = true;
                } else if (fl > bestFL) {
                    better = true;
                } else if (fl == bestFL && desc1.DedicatedVideoMemory > bestVRAM) {
                    better = true;
                }
            } else if (!software) {
                if (fl > bestFL) {
                    better = true;
                } else if (fl == bestFL && desc1.DedicatedVideoMemory > bestVRAM) {
                    better = true;
                }
            }

            if (better) {
                bestDevice = std::move(dev);
                bestCtx = std::move(ctx);
                bestFL = fl;
                bestVRAM = desc1.DedicatedVideoMemory;
                bestSoftware = software;
                bestIndex = (int)i;
            }
        }

        if (!bestDevice) {
            Log(LOG_ERROR) << "D3D11CreateDevice failed for every adapter";
            return false;
        }
        m_device = std::move(bestDevice);
        m_ctx = std::move(bestCtx);
        m_fl = bestFL;
        m_info.adapterIndex = bestIndex;
        Log(LOG_INFO) << "Selected Direct3D 11 feature level " << featureLevelName(m_fl);

    if (!createSwapChain(hwnd, w, h)) return false;
    if (!createRenderTargets(w, h)) return false;
    if (!createPipeline()) return false;
    if (!createBuffers()) return false;

    m_info.width = w;
    m_info.height = h;
    // Truncate at the first NUL (the description is a wide string).
    size_t len = 0;
    while (len < 128 && m_adapterDesc.Description[len] != L'\0') ++len;
    std::wstring wname(m_adapterDesc.Description, len);
    int req = WideCharToMultiByte(CP_UTF8, 0, wname.c_str(), (int)wname.size(),
                                  nullptr, 0, nullptr, nullptr);
    m_info.adapterName.resize(req);
    WideCharToMultiByte(CP_UTF8, 0, wname.c_str(), (int)wname.size(),
                        &m_info.adapterName[0], req, nullptr, nullptr);

    m_info.apiName = "Direct3D 11";

    return true;
}

bool RendererD3D11::createSwapChain(HWND hwnd, int w, int h) {
    ComPtr<IDXGIDevice> dxgiDevice;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<IDXGIFactory1> factory;

    if (FAILED(m_device.As(&dxgiDevice))) {
        Log(LOG_ERROR) << "D3D11: device does not expose IDXGIDevice";
        return false;
    }
    if (FAILED(dxgiDevice->GetAdapter(&adapter))) {
        Log(LOG_ERROR) << "D3D11: could not get adapter from device";
        return false;
    }
    adapter->GetDesc(&m_adapterDesc);
    LARGE_INTEGER umdVersion = {};
    if (SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umdVersion))) {
        formatDriverVersion((ULONGLONG)umdVersion.QuadPart, m_info.driverVersion);
    } else {
        m_info.driverVersion = "unknown";
    }
    if (FAILED(adapter->GetParent(IID_PPV_ARGS(&factory)))) {
        Log(LOG_ERROR) << "D3D11: could not get DXGI factory from adapter";
        return false;
    }

    // IDXGIFactory2/CreateSwapChainForHwnd exists only on Windows 8+. Try it
    // first (flip model), then fall back to the Windows 7 IDXGIFactory path.
    ComPtr<IDXGIFactory2> factory2;
    factory.As(&factory2);
    HRESULT hr = E_FAIL;
    if (factory2) {
        DXGI_SWAP_CHAIN_DESC1 sd = {};
        sd.Width = (UINT)w;
        sd.Height = (UINT)h;
        sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = 3;
        sd.Scaling = DXGI_SCALING_STRETCH;
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;

        ComPtr<IDXGISwapChain1> swapChain1;
        hr = factory2->CreateSwapChainForHwnd(m_device.Get(), hwnd, &sd,
                                              nullptr, nullptr, &swapChain1);
        if (SUCCEEDED(hr)) {
            m_swapChain = swapChain1;
            m_bufferCount = 3;
            m_flipModel = true;
        }
    }

    if (!m_swapChain) {
        // Legacy path used by Windows 7: IDXGISwapChain with the discard
        // model. This path also covers Windows 8+ when flip-model creation
        // fails for driver-specific reasons.
        DXGI_SWAP_CHAIN_DESC sd = {};
        sd.BufferDesc.Width = (UINT)w;
        sd.BufferDesc.Height = (UINT)h;
        sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.BufferCount = 2;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.OutputWindow = hwnd;
        sd.SampleDesc.Count = 1;
        sd.Windowed = TRUE;
        sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        hr = factory->CreateSwapChain(m_device.Get(), &sd, &m_swapChain);
        if (FAILED(hr)) {
            Log(LOG_ERROR) << "CreateSwapChain (D3D11) failed: 0x"
                           << std::hex << (unsigned)hr;
            return false;
        }
        m_bufferCount = 2;
        m_flipModel = false;
        Log(LOG_WARN) << "Using legacy DXGI_SWAP_EFFECT_DISCARD swap chain";
    }

    DXGI_SWAP_CHAIN_DESC dbg = {};
    m_swapChain->GetDesc(&dbg);
    Log(LOG_INFO) << "D3D11 swap chain: BufferCount=" << dbg.BufferCount
                  << " SwapEffect=" << (int)dbg.SwapEffect
                  << " Width=" << dbg.BufferDesc.Width
                  << " Height=" << dbg.BufferDesc.Height;
    factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
    if (m_flipModel) m_swapChain.As(&m_swapChain3);
    return true;
}

bool RendererD3D11::createRenderTargets(int w, int h) {
    // Cache one RTV per back buffer. Creating GetBuffer/CreateRenderTargetView
    // every frame was a measurable COM/allocation cost in the hot path. The
    // discard-model chain only has one reachable back buffer (index 0).
    for (auto& rtv : m_rtv) rtv.Reset();
    const UINT rtvCount = m_flipModel ? m_bufferCount : 1;
    for (UINT i = 0; i < rtvCount; ++i) {
        ComPtr<ID3D11Texture2D> back;
        HRESULT hr = m_swapChain->GetBuffer(i, IID_PPV_ARGS(&back));
        if (FAILED(hr)) {
            // Some drivers only expose non-current flip-model back buffers
            // after the first Present. Defer those RTVs to draw(), where the
            // current buffer is guaranteed to be valid.
            if (i == 0) {
                Log(LOG_ERROR) << "D3D11: GetBuffer(" << i << ") failed: 0x"
                               << std::hex << (unsigned)hr;
                return false;
            }
            Log(LOG_DEBUG) << "D3D11: GetBuffer(" << i << ") deferred: 0x"
                           << std::hex << (unsigned)hr;
            break;
        }
        hr = m_device->CreateRenderTargetView(back.Get(), nullptr, &m_rtv[i]);
        if (FAILED(hr)) {
            Log(LOG_ERROR) << "D3D11: CreateRenderTargetView failed for buffer "
                           << i << ": 0x" << std::hex << (unsigned)hr;
            return false;
        }
    }

    D3D11_TEXTURE2D_DESC dd = {};
    dd.Width = (UINT)w;
    dd.Height = (UINT)h;
    dd.MipLevels = 1;
    dd.ArraySize = 1;
    dd.Format = kDepthFormat;
    dd.SampleDesc.Count = 1;
    dd.Usage = D3D11_USAGE_DEFAULT;
    dd.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    if (FAILED(m_device->CreateTexture2D(&dd, nullptr, &m_depthTex))) {
        Log(LOG_ERROR) << "D3D11: depth texture creation failed";
        return false;
    }
    if (FAILED(m_device->CreateDepthStencilView(m_depthTex.Get(), nullptr, &m_dsv))) {
        Log(LOG_ERROR) << "D3D11: depth stencil view creation failed";
        return false;
    }
    return true;
}

bool RendererD3D11::createPipeline() {
    if (FAILED(m_device->CreateVertexShader(kQuadVS, kQuadVSSize, nullptr, &m_vs))) return false;
    if (FAILED(m_device->CreatePixelShader(kQuadPS, kQuadPSSize, nullptr, &m_ps))) return false;

    D3D11_INPUT_ELEMENT_DESC layoutDesc[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 }
    };
    if (FAILED(m_device->CreateInputLayout(layoutDesc, 1,
                                           kQuadVS, kQuadVSSize, &m_il))) return false;

    D3D11_BLEND_DESC bd = {};
    bd.RenderTarget[0].BlendEnable = TRUE;
    bd.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_SRC_ALPHA;
    bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(m_device->CreateBlendState(&bd, &m_blend))) return false;

    D3D11_DEPTH_STENCIL_DESC dd = {};
    dd.DepthEnable = TRUE;
    dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    dd.DepthFunc = D3D11_COMPARISON_LESS;
    if (FAILED(m_device->CreateDepthStencilState(&dd, &m_dsState))) return false;

    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    if (FAILED(m_device->CreateRasterizerState(&rd, &m_rs))) return false;

    return true;
}

bool RendererD3D11::createBuffers() {
    // Quad vertices (GL z=0 remapped to D3D z=0.5).
    std::vector<vtxData> quad = vbData;
    for (auto& vert : quad) {
        vert.pos.z = vert.pos.z * 0.5f + 0.5f;
    }

    D3D11_BUFFER_DESC bd = {};
    D3D11_SUBRESOURCE_DATA init = {};

    bd.ByteWidth = (UINT)(quad.size() * sizeof(vtxData));
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    init.pSysMem = quad.data();
    if (FAILED(m_device->CreateBuffer(&bd, &init, &m_vb))) return false;

    bd.ByteWidth = (UINT)(ibData.size() * sizeof(uint16_t));
    bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
    init.pSysMem = ibData.data();
    if (FAILED(m_device->CreateBuffer(&bd, &init, &m_ib))) return false;

    float vcolor[7][4] = {
        {1.f, 0.f, 0.f, 1.f}, {0.f, 1.f, 0.f, 1.f}, {0.f, 0.f, 1.f, 1.f},
        {1.f, 1.f, 0.f, 1.f}, {1.f, 0.f, 1.f, 1.f}, {0.f, 1.f, 1.f, 1.f},
        {1.f, 1.f, 1.f, 1.f}
    };
    bd.ByteWidth = sizeof(vcolor);
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    init.pSysMem = vcolor;
    if (FAILED(m_device->CreateBuffer(&bd, &init, &m_cbVS))) return false;

    // Per-frame constants are rewritten every frame, so keep the buffer
    // dynamic and update it with Map(DISCARD) instead of a full-subresource
    // copy (same 32 bytes, same values, cheaper per frame).
    bd.ByteWidth = sizeof(FrameCB);
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    init.pSysMem = nullptr;
    if (FAILED(m_device->CreateBuffer(&bd, nullptr, &m_cbPS))) return false;
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.CPUAccessFlags = 0;

    bd.ByteWidth = 16;
    bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
    uint32_t zero = 0;
    init.pSysMem = &zero;
    if (FAILED(m_device->CreateBuffer(&bd, &init, &m_counter))) return false;

    D3D11_UNORDERED_ACCESS_VIEW_DESC uav = {};
    uav.Format = DXGI_FORMAT_R32_TYPELESS;
    uav.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    uav.Buffer.FirstElement = 0;
    uav.Buffer.NumElements = 1;
    uav.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
    if (FAILED(m_device->CreateUnorderedAccessView(m_counter.Get(), &uav, &m_counterUAV))) return false;

    return true;
}

void RendererD3D11::setClearColor(float r, float g, float b, float a) {
    clearColor[0] = r;
    clearColor[1] = g;
    clearColor[2] = b;
    clearColor[3] = a;
}

void RendererD3D11::setFragCount(uint32_t v) {
    frag_count = v;
}

void RendererD3D11::draw() {
    ensureQuadIndexData();

    UINT bb = m_swapChain3 ? m_swapChain3->GetCurrentBackBufferIndex() : 0;
    const UINT target = m_flipModel ? bb : 0;
    if (!m_rtv[target]) {
        ComPtr<ID3D11Texture2D> back;
        HRESULT hr = m_swapChain->GetBuffer(target, IID_PPV_ARGS(&back));
        if (FAILED(hr)) {
            Log(LOG_ERROR) << "D3D11: GetBuffer(" << target << ") failed: 0x"
                           << std::hex << (unsigned)hr;
            return;
        }
        hr = m_device->CreateRenderTargetView(back.Get(), nullptr, &m_rtv[target]);
        if (FAILED(hr)) {
            Log(LOG_ERROR) << "D3D11: CreateRenderTargetView failed for buffer "
                           << target << ": 0x" << std::hex << (unsigned)hr;
            return;
        }
    }
    ID3D11RenderTargetView* rtv = m_rtv[target].Get();

    D3D11_VIEWPORT vp = {0.f, 0.f, (float)m_info.width, (float)m_info.height, 0.f, 1.f};
    m_ctx->RSSetViewports(1, &vp);
    m_ctx->RSSetState(m_rs.Get());
    m_ctx->OMSetBlendState(m_blend.Get(), nullptr, 0xFFFFFFFF);
    m_ctx->OMSetDepthStencilState(m_dsState.Get(), 0);

    m_ctx->ClearRenderTargetView(rtv, clearColor);
    m_ctx->ClearDepthStencilView(m_dsv.Get(), D3D11_CLEAR_DEPTH, 1.f, 0);

    // Per-frame parameters.
    FrameCB cb = {};
    cb.color[0] = 0.5f; cb.color[1] = 0.5f; cb.color[2] = 0.f; cb.color[3] = 1.f;
    cb.frags_to_shade = frag_count;
    D3D11_MAPPED_SUBRESOURCE mappedCB = {};
    if (SUCCEEDED(m_ctx->Map(m_cbPS.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedCB))) {
        memcpy(mappedCB.pData, &cb, sizeof(cb));
        m_ctx->Unmap(m_cbPS.Get(), 0);
    }

    // Bind the counter UAV through the output-merger stage (this SDK's headers
    // only expose OMSetRenderTargetsAndUnorderedAccessViews, and OM UAV slots
    // are the same slots the pixel shader sees). The fragment counter is reset
    // with a GPU-side UAV clear while the view stays bound, which avoids the
    // unbind / CPU update / rebind round trip the old code did every frame.
    ID3D11UnorderedAccessView* uav = m_counterUAV.Get();
    m_ctx->OMSetRenderTargetsAndUnorderedAccessViews(
        1, &rtv, m_dsv.Get(), 1, 1, &uav, nullptr);
    const UINT clearCounter[4] = {0, 0, 0, 0};
    m_ctx->ClearUnorderedAccessViewUint(m_counterUAV.Get(), clearCounter);

    m_ctx->IASetInputLayout(m_il.Get());
    m_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_ctx->VSSetShader(m_vs.Get(), nullptr, 0);
    m_ctx->PSSetShader(m_ps.Get(), nullptr, 0);
    ID3D11Buffer* cbv[1] = { m_cbVS.Get() };
    m_ctx->VSSetConstantBuffers(0, 1, cbv);
    ID3D11Buffer* cbp[1] = { m_cbPS.Get() };
    m_ctx->PSSetConstantBuffers(1, 1, cbp);

    UINT stride = (UINT)sizeof(vtxData);
    UINT offset = 0;
    m_ctx->IASetVertexBuffers(0, 1, m_vb.GetAddressOf(), &stride, &offset);
    m_ctx->IASetIndexBuffer(m_ib.Get(), DXGI_FORMAT_R16_UINT, 0);
    m_ctx->DrawIndexed((UINT)ibData.size(), 0, 0);
}

void RendererD3D11::drawRandomTris(int count) {
    ensureRandomTris(count);
    if (vbDataRandom.empty()) return;

    const size_t bytes = vbDataRandom.size() * sizeof(vtxData);
    const size_t maxBytes = 200 * 3 * sizeof(vtxData); // "Tris" slider max
    if (!m_vbRandom || m_vbRandomBytes != maxBytes) {
        m_vbRandom.Reset();
        D3D11_BUFFER_DESC bd = {};
        bd.ByteWidth = (UINT)maxBytes;
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        if (FAILED(m_device->CreateBuffer(&bd, nullptr, &m_vbRandom))) return;
        m_vbRandomBytes = maxBytes;
        m_randomUploadedVersion = -1;
    }

    // Random triangles are regenerated only when the Tris count changes, so
    // upload them only when the generator has advanced instead of copying every
    // frame (count -> 0 -> count also regenerates, which a plain count check
    // would miss).
    if (m_randomUploadedVersion != m_randomVersion) {
        D3D11_BOX box = {};
        box.left = 0;
        box.right = (UINT)bytes;
        box.bottom = 1;
        box.back = 1;
        m_ctx->UpdateSubresource(m_vbRandom.Get(), 0, &box, vbDataRandom.data(), 0, 0);
        m_randomUploadedVersion = m_randomVersion;
    }

    UINT stride = (UINT)sizeof(vtxData);
    UINT offset = 0;
    m_ctx->IASetVertexBuffers(0, 1, m_vbRandom.GetAddressOf(), &stride, &offset);
    m_ctx->Draw((UINT)vbDataRandom.size(), 0);
}

void RendererD3D11::present() {
    // Present without blocking on vsync; frame pacing to the display refresh
    // rate is done on the CPU (main.cpp). This avoids the flip-model TDR that
    // can happen if a vsync present is pending when the swapchain is released.
    m_swapChain->Present(0, 0);
}

bool RendererD3D11::resize(int w, int h) {
    if (w <= 0 || h <= 0) return false;

    m_ctx->OMSetRenderTargets(0, nullptr, nullptr);
    for (auto& rtv : m_rtv) rtv.Reset();
    m_dsv.Reset();
    m_depthTex.Reset();

    HRESULT hr = m_swapChain->ResizeBuffers(m_bufferCount, (UINT)w, (UINT)h,
                                            DXGI_FORMAT_R8G8B8A8_UNORM, 0);
    if (FAILED(hr)) {
        Log(LOG_ERROR) << "ResizeBuffers (D3D11) failed: 0x" << std::hex << (unsigned)hr;
        return false;
    }
    if (!createRenderTargets(w, h)) return false;

    m_info.width = w;
    m_info.height = h;
    return true;
}
