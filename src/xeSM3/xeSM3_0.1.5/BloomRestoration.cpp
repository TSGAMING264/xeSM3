#include <Windows.h>

#include "PostFXResearch.hpp"
#include "BloomRestoration.hpp"
#include "NativeBloomKernelConstants.hpp"
#include "NativeBloomAccumulationTokens.hpp"
#include "NativeBloomBrightnessTokens.hpp"
#include "BloomSceneAlphaRoute.hpp"
#include "RowMeanSquare.hpp"
#include "AdaptiveGain.hpp"

#include <d3d9.h>
#include <detours.h>
#include <intrin.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <mutex>
#include <vector>

namespace {
static_assert(sizeof(void*) == 4, "Spider-Man 3 requires x86");
using namespace BloomRestoration;

template<class T> struct Com {
    T* p = nullptr;
    Com() = default;
    Com(const Com&) = delete;
    Com& operator=(const Com&) = delete;
    ~Com() { reset(); }
    void reset() { if (p) { p->Release(); p = nullptr; } }
    T** put() { reset(); return &p; }
    T* operator->() const { return p; }
};

bool Range(const void* p, size_t bytes, bool writable = false) {
    auto cursor = reinterpret_cast<uintptr_t>(p);
    if (!cursor || bytes > UINTPTR_MAX - cursor) return false;
    const auto end = cursor + bytes;
    while (cursor < end) {
        MEMORY_BASIC_INFORMATION info{};
        if (!VirtualQuery(reinterpret_cast<void*>(cursor), &info, sizeof(info)) ||
            info.State != MEM_COMMIT || (info.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
        const DWORD permission = info.Protect & 0xff;
        if (writable && permission != PAGE_READWRITE && permission != PAGE_EXECUTE_READWRITE &&
            permission != PAGE_WRITECOPY && permission != PAGE_EXECUTE_WRITECOPY) return false;
        const auto next = reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize;
        if (next <= cursor) return false;
        cursor = next;
    }
    return true;
}
template<class T> T Read(uintptr_t address) {
    T value{};
    if (Range(reinterpret_cast<void*>(address), sizeof(T))) memcpy(&value, reinterpret_cast<void*>(address), sizeof(T));
    return value;
}
bool Same(uintptr_t address, const void* bytes, size_t count) {
    return Range(reinterpret_cast<void*>(address), count) && memcmp(reinterpret_cast<void*>(address), bytes, count) == 0;
}

struct CodePatch {
    uintptr_t address = 0;
    std::vector<unsigned char> before, after;
    bool applied = false;
    bool protectionPending = false;
    DWORD originalProtection = 0;
    ~CodePatch() { restore(); }
    bool restoreProtection() {
        if (!protectionPending) return true;
        DWORD ignored{};
        if (!VirtualProtect(reinterpret_cast<void*>(address), before.size(), originalProtection, &ignored)) return false;
        protectionPending = false;
        return true;
    }
    bool publish(const std::vector<unsigned char>& from, const std::vector<unsigned char>& to, bool& written) {
        written = false;
        if (!restoreProtection()) return false;
        if (!Same(address, from.data(), from.size())) return false;
        if (!VirtualProtect(reinterpret_cast<void*>(address), to.size(), PAGE_EXECUTE_READWRITE, &originalProtection)) return false;
        protectionPending = true;
        memcpy(reinterpret_cast<void*>(address), to.data(), to.size());
        written = true;
        FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(address), to.size());
        return restoreProtection() && Same(address, to.data(), to.size());
    }
    bool apply() {
        if (applied) return restoreProtection() && Same(address, after.data(), after.size());
        bool written = false;
        const bool ok = publish(before, after, written);
        applied = written;
        return ok;
    }
    void restore() {
        if (applied) {
            bool written = false;
            publish(after, before, written);
            if (written) applied = false;
        }
        restoreProtection();
    }
    void call(uintptr_t site, const std::vector<unsigned char>& expected, const void* function, bool pushDevice = false) {
        address = site; before = expected; after.assign(expected.size(), 0x90);
        size_t i = 0;
        if (pushDevice) after[i++] = 0x50;
        after[i] = 0xe8;
        const auto displacement = static_cast<int32_t>(reinterpret_cast<uintptr_t>(function) - (site + i + 5));
        memcpy(after.data() + i + 1, &displacement, sizeof(displacement));
    }
};

struct Target {
    Com<IDirect3DTexture9> texture;
    Com<IDirect3DSurface9> surface;
    UINT width = 0, height = 0;
    void reset() { surface.reset(); texture.reset(); width = height = 0; }
    bool create(IDirect3DDevice9* d, UINT w, UINT h, D3DFORMAT format) {
        reset(); width = w; height = h;
        return SUCCEEDED(d->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, format, D3DPOOL_DEFAULT, texture.put(), nullptr)) &&
            SUCCEEDED(texture->GetSurfaceLevel(0, surface.put()));
    }
};

struct Resources {
    IDirect3DDevice9* device = nullptr;
    UINT width = 0, height = 0;
    Target a, b, reduced8, reduced64, rows, gain;
    Com<IDirect3DVertexShader9> vertex;
    Com<IDirect3DPixelShader9> weighted, copy, blur, gaussian, reduction, row, exposure;
    Com<IDirect3DStateBlock9> state;
    bool ready = false;
    void reset() {
        ready = false;
        state.reset(); a.reset(); b.reset(); reduced8.reset(); reduced64.reset(); rows.reset(); gain.reset();
        vertex.reset(); weighted.reset(); copy.reset(); blur.reset(); gaussian.reset(); reduction.reset(); row.reset(); exposure.reset();
        device = nullptr; width = height = 0;
    }
} resources;

bool enabled = false, prepared = false, fatal = false;
struct Recovery {
    bool resetting = false, awaitingReset = false;
    ULONGLONG nextAttempt = 0;
} recovery;

std::recursive_mutex& ResourceMutex() {
    static std::recursive_mutex mutex;
    return mutex;
}
struct StateAddresses {
    uintptr_t device, initialized, gate, presentParameters;
};
#ifdef BLOOM_SHADER_FIXTURES
StateAddresses stateAddresses{};
#else
constexpr StateAddresses stateAddresses{0x10FC9F4, 0xE8FCA0, 0xDE00E8, 0x10FC888};
#endif
std::array<std::vector<unsigned char>, 6> materialCode;
std::array<CodePatch, 6> materialPatches;
CodePatch finalCall, resetCall;
using Native = int(__cdecl*)();
Native outer = reinterpret_cast<Native>(0x795D50);
using Init = void(__cdecl*)();
Init initializeTargets = reinterpret_cast<Init>(0x7A4CF0);
Native destroyTargets = reinterpret_cast<Native>(0x795AB0);
using CreateTarget = int(__cdecl*)(uint32_t, int, int, int, int, int);
CreateTarget createTarget = reinterpret_cast<CreateTarget>(0x8D1EB0);
bool hooksQueued = false;
enum class Route { Pending, Original, Restored };
Route selectedRoute = Route::Pending;

uint32_t Fnv(const void* data, size_t bytes) {
    uint32_t hash = 2166136261u;
    const auto* p = static_cast<const unsigned char*>(data);
    for (size_t i = 0; i < bytes; ++i) hash = (hash ^ p[i]) * 16777619u;
    return hash;
}

#ifdef BLOOM_SHADER_FIXTURES
uintptr_t shaderFixtureBase = 0;
#endif
const DWORD* ShaderCode(uintptr_t address) {
#ifdef BLOOM_SHADER_FIXTURES
    return reinterpret_cast<const DWORD*>(shaderFixtureBase + address - 0xA00000);
#else
    return reinterpret_cast<const DWORD*>(address);
#endif
}

bool ShaderIdentity(uintptr_t address, size_t size, uint32_t hash) {
    return Range(ShaderCode(address), size) && Fnv(ShaderCode(address), size) == hash;
}

std::vector<DWORD> WeightedGpuGain() {
    if (!ShaderIdentity(0xABCEC0, 1604, 0x26716EBA)) return {};
    const auto* begin = ShaderCode(0xABCEC0);
    std::vector<DWORD> result(begin, begin + 401);
    const DWORD declaration[] = {0x0200001f, 0x90000000, 0xA00F0800};
    const auto it = std::search(result.begin(), result.end(), std::begin(declaration), std::end(declaration));
    if (it == result.end() || result[395] != 0x04000004 || result[396] != 0x800F0800 || result.back() != 0xffff) return {};
    result[396] = 0x800F0002;
    result.pop_back();
    const DWORD tail[] = {0x03000042, 0x800F0000, 0xA0550007, 0xA0E40801,
                         0x03000005, 0x800F0800, 0x80E40002, 0x80000000, 0xffff};
    const size_t insert = static_cast<size_t>(it - result.begin()) + 3;
    result.insert(result.end(), std::begin(tail), std::end(tail));
    const DWORD sampler[] = {0x0200001f, 0x90000000, 0xA00F0801};
    result.insert(result.begin() + insert, std::begin(sampler), std::end(sampler));
    return result;
}

bool Prepare(IDirect3DDevice9* d, UINT w, UINT h) {
    if (resources.ready && resources.device == d && resources.width == w && resources.height == h) return true;
    resources.reset();
    if (!d || w < 64 || h < 64 || w > 4096 || h > 4096 || FAILED(d->TestCooperativeLevel())) return false;
    resources.device = d; resources.width = w; resources.height = h;
    auto code = WeightedGpuGain();
    bool ok = !code.empty() && ShaderIdentity(0xAC20F8, 272, 0x8E6825EC) &&
        ShaderIdentity(0xAB7328, 196, 0xFC510D2F) && ShaderIdentity(0xAC2718, 452, 0x6C261A23);
    if (ok) ok = SUCCEEDED(d->CreateVertexShader(ShaderCode(0xAC20F8), resources.vertex.put())) &&
        SUCCEEDED(d->CreatePixelShader(code.data(), resources.weighted.put())) &&
        SUCCEEDED(d->CreatePixelShader(ShaderCode(0xAB7328), resources.copy.put())) &&
        SUCCEEDED(d->CreatePixelShader(ShaderCode(0xAC2718), resources.blur.put())) &&
        SUCCEEDED(d->CreatePixelShader(reinterpret_cast<const DWORD*>(NativeBloomAccumulationTokens::kWords), resources.gaussian.put())) &&
        SUCCEEDED(d->CreatePixelShader(reinterpret_cast<const DWORD*>(NativeBloomBrightnessTokens::kWords), resources.reduction.put())) &&
        SUCCEEDED(d->CreatePixelShader(reinterpret_cast<const DWORD*>(kRowMeanSquare), resources.row.put())) &&
        SUCCEEDED(d->CreatePixelShader(reinterpret_cast<const DWORD*>(kAdaptiveGain), resources.exposure.put()));
    if (ok) ok = resources.a.create(d, w / 4, h / 4, D3DFMT_A16B16G16R16F) &&
        resources.b.create(d, w / 4, h / 4, D3DFMT_A16B16G16R16F) &&
        resources.reduced8.create(d, w / 8, h / 8, D3DFMT_A8R8G8B8) &&
        resources.reduced64.create(d, w / 64, h / 64, D3DFMT_A8R8G8B8) &&
        resources.rows.create(d, 1, h / 64, D3DFMT_A32B32G32R32F) &&
        resources.gain.create(d, 1, 1, D3DFMT_A16B16G16R16F) &&
        SUCCEEDED(d->CreateStateBlock(D3DSBT_ALL, resources.state.put()));
    resources.ready = ok;
    if (!ok) resources.reset();
    return ok;
}

bool RecoverResources(IDirect3DDevice9* d) {
    if (recovery.resetting || recovery.awaitingReset) return false;
    if (!fatal && resources.ready && resources.device == d) return true;
    const auto now = GetTickCount64();
    if (now < recovery.nextAttempt) return false;
    recovery.nextAttempt = now + 250;
    resources.reset();
    if (!d || !Range(d, sizeof(void*)) || FAILED(d->TestCooperativeLevel())) return false;
    Com<IDirect3DSurface9> backbuffer;
    D3DSURFACE_DESC desc{};
    if (FAILED(d->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, backbuffer.put())) ||
        !backbuffer.p || FAILED(backbuffer->GetDesc(&desc))) return false;
    try {
        if (!Prepare(d, desc.Width, desc.Height)) return false;
    } catch (...) {
        resources.reset();
        return false;
    }
    fatal = false;
    recovery.nextAttempt = 0;
    return true;
}

bool RecoverNativeResources() {
    auto* d = Read<IDirect3DDevice9*>(stateAddresses.device);
    if ((fatal || !resources.ready || resources.device != d) && !Read<unsigned char>(stateAddresses.initialized)) return false;
    return RecoverResources(d);
}

struct SavedState {
    IDirect3DDevice9* device;
    Com<IDirect3DSurface9> target, depth;
    D3DVIEWPORT9 viewport{};
    bool captured = false;
    explicit SavedState(IDirect3DDevice9* d) : device(d) {
        captured = resources.state.p && SUCCEEDED(resources.state->Capture()) &&
            SUCCEEDED(d->GetRenderTarget(0, target.put())) && SUCCEEDED(d->GetViewport(&viewport));
        const auto hr = d->GetDepthStencilSurface(depth.put());
        captured = captured && (SUCCEEDED(hr) || hr == D3DERR_NOTFOUND);
        for (DWORD index = 1; index < 4; ++index) {
            Com<IDirect3DSurface9> extra;
            const HRESULT extraResult = d->GetRenderTarget(index,extra.put());
            if (extra.p || (FAILED(extraResult) && extraResult != D3DERR_NOTFOUND && extraResult != D3DERR_INVALIDCALL)) captured = false;
        }
    }
    bool restore() {
        if (!captured) return false;
        HRESULT result = S_OK;
        auto check = [&](HRESULT hr) { if (FAILED(hr)) result = hr; };
        check(device->SetTexture(0, nullptr)); check(device->SetTexture(1, nullptr));
        check(device->SetDepthStencilSurface(nullptr));
        check(device->SetRenderTarget(0, target.p));
        check(device->SetDepthStencilSurface(depth.p));
        check(resources.state->Apply()); check(device->SetViewport(&viewport));
        captured = false;
        if (FAILED(result)) fatal = true;
        return SUCCEEDED(result);
    }
    ~SavedState() { if (captured) restore(); }
};

bool BaseState(IDirect3DDevice9* d) {
    HRESULT result = S_OK;
    auto check = [&](HRESULT hr) { if (FAILED(hr)) result = hr; };
    check(d->SetDepthStencilSurface(nullptr));
    const std::pair<D3DRENDERSTATETYPE, DWORD> states[] = {
        {D3DRS_ZENABLE,FALSE},{D3DRS_ZWRITEENABLE,FALSE},{D3DRS_ALPHATESTENABLE,FALSE},
        {D3DRS_CULLMODE,D3DCULL_NONE},{D3DRS_ALPHABLENDENABLE,FALSE},{D3DRS_FOGENABLE,FALSE},
        {D3DRS_STENCILENABLE,FALSE},{D3DRS_COLORWRITEENABLE,15},{D3DRS_SRGBWRITEENABLE,FALSE},
        {D3DRS_SEPARATEALPHABLENDENABLE,FALSE},{D3DRS_CLIPPLANEENABLE,0},{D3DRS_SCISSORTESTENABLE,FALSE},
        {D3DRS_FILLMODE,D3DFILL_SOLID},{D3DRS_BLENDOP,D3DBLENDOP_ADD},
        {D3DRS_SRCBLEND,D3DBLEND_ONE},{D3DRS_DESTBLEND,D3DBLEND_ONE}};
    for (const auto& state : states) check(d->SetRenderState(state.first, state.second));
    for (DWORD s = 0; s < 2; ++s) {
        check(d->SetSamplerState(s, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP));
        check(d->SetSamplerState(s, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP));
        check(d->SetSamplerState(s, D3DSAMP_MINFILTER, s ? D3DTEXF_POINT : D3DTEXF_LINEAR));
        check(d->SetSamplerState(s, D3DSAMP_MAGFILTER, s ? D3DTEXF_POINT : D3DTEXF_LINEAR));
        check(d->SetSamplerState(s, D3DSAMP_MIPFILTER, D3DTEXF_NONE));
        check(d->SetSamplerState(s, D3DSAMP_SRGBTEXTURE, FALSE));
    }
    check(d->SetTexture(1, nullptr));
    check(d->SetVertexShader(resources.vertex.p));
    return SUCCEEDED(result);
}

bool Bind(IDirect3DDevice9* d, IDirect3DSurface9* target, UINT w, UINT h,
          IDirect3DBaseTexture9* source, IDirect3DPixelShader9* shader) {
    const D3DVIEWPORT9 vp{0,0,w,h,0.0f,1.0f};
    const float phase[] = {0.5f / w, 0.5f / h, 0.5f / w, 0.5f / h};
    return SUCCEEDED(d->SetTexture(0,nullptr)) && SUCCEEDED(d->SetRenderTarget(0,target)) &&
        SUCCEEDED(d->SetViewport(&vp)) && SUCCEEDED(d->SetTexture(0,source)) &&
        SUCCEEDED(d->SetPixelShader(shader)) && SUCCEEDED(d->SetVertexShaderConstantF(0,phase,1));
}
bool Draw(IDirect3DDevice9* d) { return SUCCEEDED(d->DrawPrimitive(D3DPT_TRIANGLESTRIP,0,2)); }
bool Pass(IDirect3DDevice9* d, Target& out, IDirect3DBaseTexture9* source, IDirect3DPixelShader9* shader) {
    return Bind(d,out.surface.p,out.width,out.height,source,shader) && Draw(d);
}

bool Render(IDirect3DDevice9* d, IDirect3DTexture9* scene, IDirect3DSurface9* destination, UINT w, UINT h) {
    if (!BaseState(d)) return false;
    auto reduce = [&](Target& output, IDirect3DTexture9* input, UINT iw, UINT ih) {
        const float offsets[] = {-3.0f/iw,-1.0f/iw,1.0f/iw,3.0f/iw,-3.0f/ih,-1.0f/ih,1.0f/ih,3.0f/ih};
        return SUCCEEDED(d->SetPixelShaderConstantF(0,offsets,2)) && Pass(d,output,input,resources.reduction.p);
    };
    if (!reduce(resources.reduced8,scene,w,h) ||
        !reduce(resources.reduced64,resources.reduced8.texture.p,resources.reduced8.width,resources.reduced8.height)) return false;
    const float dimensions[] = {static_cast<float>(resources.reduced64.width),static_cast<float>(resources.reduced64.height),0,0};
    if (FAILED(d->SetSamplerState(0,D3DSAMP_MINFILTER,D3DTEXF_POINT)) ||
        FAILED(d->SetSamplerState(0,D3DSAMP_MAGFILTER,D3DTEXF_POINT)) ||
        FAILED(d->SetPixelShaderConstantF(0,dimensions,1)) ||
        !Pass(d,resources.rows,resources.reduced64.texture.p,resources.row.p) ||
        !Pass(d,resources.gain,resources.rows.texture.p,resources.exposure.p)) return false;
    if (FAILED(d->SetSamplerState(0,D3DSAMP_MINFILTER,D3DTEXF_LINEAR)) ||
        FAILED(d->SetSamplerState(0,D3DSAMP_MAGFILTER,D3DTEXF_LINEAR))) return false;
    NativeBloomKernel::Constants kernel;
    if (!NativeBloomKernel::Build(w,h,kernel) ||
        !Bind(d,resources.a.surface.p,resources.a.width,resources.a.height,scene,resources.weighted.p)) return false;
    kernel.vs[0] += .5f/resources.a.width; kernel.vs[2] += .5f/resources.a.width;
    kernel.vs[1] += .5f/resources.a.height; kernel.vs[3] += .5f/resources.a.height;
    if (FAILED(d->SetTexture(1,resources.gain.texture.p)) ||
        FAILED(d->SetVertexShaderConstantF(0,kernel.vs,1)) ||
        FAILED(d->SetPixelShaderConstantF(0,&kernel.ps[0][0],7)) || !Draw(d) ||
        FAILED(d->SetTexture(1,nullptr))) return false;
    if (!Pass(d,resources.b,resources.a.texture.p,resources.copy.p)) return false;
    const float gaussian[] = {0,-4.8f/h,0,4.8f/h};
    if (FAILED(d->SetRenderState(D3DRS_ALPHABLENDENABLE,TRUE)) ||
        FAILED(d->SetRenderState(D3DRS_DESTBLEND,D3DBLEND_BLENDFACTOR)) ||
        FAILED(d->SetRenderState(D3DRS_BLENDFACTOR,0x60606060)) ||
        FAILED(d->SetPixelShaderConstantF(0,gaussian,1)) ||
        !Pass(d,resources.b,resources.a.texture.p,resources.gaussian.p) ||
        FAILED(d->SetRenderState(D3DRS_ALPHABLENDENABLE,FALSE))) return false;
    const float blur1[] = {-6.0f/w,-6.0f/h,6.0f/w,-6.0f/h};
    const float blur2[] = {-10.0f/w,-10.0f/h,10.0f/w,-10.0f/h};
    if (FAILED(d->SetPixelShaderConstantF(0,blur1,1)) || !Pass(d,resources.a,resources.b.texture.p,resources.blur.p) ||
        FAILED(d->SetPixelShaderConstantF(0,blur2,1)) || !Pass(d,resources.b,resources.a.texture.p,resources.blur.p)) return false;
    if (!Bind(d,destination,w,h,resources.b.texture.p,resources.copy.p) ||
        FAILED(d->SetRenderState(D3DRS_ALPHABLENDENABLE,TRUE)) ||
        FAILED(d->SetRenderState(D3DRS_SRCBLEND,D3DBLEND_ONE)) ||
        FAILED(d->SetRenderState(D3DRS_DESTBLEND,D3DBLEND_ONE)) ||
        FAILED(d->SetRenderState(D3DRS_COLORWRITEENABLE,7))) return false;
    return Draw(d);
}

int __cdecl Composite() {
    const auto native = reinterpret_cast<Native>(0x797A70);
    if (!enabled || !prepared) return native();
    std::lock_guard<std::recursive_mutex> guard(ResourceMutex());
    if (recovery.resetting || recovery.awaitingReset) return 0;
    if (fatal || !resources.ready) return 0;
    const auto settings = Read<uintptr_t>(Read<uintptr_t>(0xE879AC) + 0x60);
    const auto owner = Read<uintptr_t>(0x110687C);
    const auto mainTarget = Read<uintptr_t>(owner + 0x2fc);
    const auto quarter = Read<uintptr_t>(0xE8FCA4);
    auto* d = Read<IDirect3DDevice9*>(0x10FC9F4);
    if (!settings || !Range(reinterpret_cast<void*>(settings + 0x8840),2) ||
        Read<unsigned char>(settings + 0x8840) || Read<unsigned char>(settings + 0x8841) ||
        !Read<uint32_t>(0xD1668C) || !Range(reinterpret_cast<void*>(mainTarget),104) ||
        !Range(reinterpret_cast<void*>(quarter),104) || !d || resources.device != d) return 0;
    using TextureBind = int(__cdecl*)(int,uintptr_t);
    using TargetBind = int(__cdecl*)(uintptr_t);
    using Quad = int(__cdecl*)(uintptr_t,uintptr_t,int,int);
    auto textureBind = reinterpret_cast<TextureBind>(0x8C6BF0);
    textureBind(0,mainTarget + 20);
    Com<IDirect3DBaseTexture9> source;
    if (FAILED(d->GetTexture(0,source.put())) || !source.p || source->GetType() != D3DRTYPE_TEXTURE) return 0;
    auto* scene = static_cast<IDirect3DTexture9*>(source.p);
    D3DSURFACE_DESC desc{};
    if (FAILED(scene->GetLevelDesc(0,&desc)) || desc.Width != resources.width || desc.Height != resources.height) return 0;
    textureBind(0,quarter + 20);
    reinterpret_cast<TargetBind>(0x795A30)(mainTarget);
    reinterpret_cast<Quad>(0x470900)(0xE8FB70,0xE8FBEC,0,0);
    SavedState state(d);
    if (!state.captured) return 0;
    const bool rendered = Render(d,scene,state.target.p,desc.Width,desc.Height);
    const bool restored = state.restore();
    //the original composite must never run after a successful additive draw.
    if (!rendered || !restored) fatal = true;
    return 0;
}

int __cdecl Outer() {
    if (!enabled || !prepared) return outer();
    std::lock_guard<std::recursive_mutex> guard(ResourceMutex());
    if (!RecoverNativeResources() || !Range(reinterpret_cast<void*>(stateAddresses.gate),1,true)) return outer();
    auto* gate = reinterpret_cast<unsigned char*>(stateAddresses.gate);
    const auto original = *gate;
    *gate = 1;
    const int result = outer();
    if (*gate == 1) *gate = original;
    return result;
}

void __cdecl InitializeTargets() {
    if (!enabled || !prepared) { initializeTargets(); return; }
    std::lock_guard<std::recursive_mutex> guard(ResourceMutex());
    const bool rebuilding = Read<unsigned char>(stateAddresses.initialized) == 0;
    if (rebuilding) { resources.reset(); recovery.nextAttempt = 0; }
    initializeTargets();
    if (rebuilding && !recovery.resetting && Read<unsigned char>(stateAddresses.initialized)) recovery.awaitingReset = false;
    RecoverNativeResources();
}

int __cdecl DestroyTargets() {
    if (!enabled || !prepared) return destroyTargets();
    std::lock_guard<std::recursive_mutex> guard(ResourceMutex());
    resources.reset();
    recovery.nextAttempt = 0;
    return destroyTargets();
}

HRESULT WINAPI Reset(IDirect3DDevice9* d) {
    {
        std::lock_guard<std::recursive_mutex> guard(ResourceMutex());
        if (recovery.resetting) return D3DERR_INVALIDCALL;
        recovery.resetting = recovery.awaitingReset = true;
        recovery.nextAttempt = 0;
        resources.reset();
        fatal = false;
    }
    const auto result = d->Reset(reinterpret_cast<D3DPRESENT_PARAMETERS*>(stateAddresses.presentParameters));
    {
        std::lock_guard<std::recursive_mutex> guard(ResourceMutex());
        recovery.resetting = false;
        recovery.awaitingReset = FAILED(result);
    }
    return result;
}

int __cdecl CreateRT(uint32_t flags, int format, int w, int h, int p5, int p6) {
    const auto caller = reinterpret_cast<uintptr_t>(_ReturnAddress());
    const bool full = (caller == 0x7A4D9E || caller == 0x7A56CE || caller == 0x7A56E2) && flags == 64;
    const bool quarter = (caller == 0x7A4DBD || caller == 0x7A4DD4) && flags == 576;
    const bool reduction = (caller == 0x7A4DF0 && w == 160 && h == 120) ||
        ((caller == 0x7A4E06 || caller == 0x7A4E1C) && w == 16 && h == 12);
    if (enabled && prepared && format == D3DFMT_A8R8G8B8 && w > 0 && h > 0 &&
        (full || quarter || (reduction && flags == 64))) format = D3DFMT_A16B16G16R16F;
    return createTarget(flags,format,w,h,p5,p6);
}

bool PreparePatches() {
    const auto descriptors = Sm3BloomSceneAlphaDescriptors();
    constexpr uintptr_t pushes[] = {0x501149,0x501118,0x50117A,0x515CC8,0x4FD22A,0x502DEA};
    for (size_t i = 0; i < descriptors.size(); ++i) {
        const auto& desc = descriptors[i];
        const auto blob = 0x400000u + desc.bytecodeRva;
        if (!Range(reinterpret_cast<void*>(blob),desc.expectedSize) ||
            !Range(reinterpret_cast<void*>(0x400000u + desc.handleSlotRva),sizeof(uintptr_t)) ||
            Read<uintptr_t>(0x400000u + desc.handleSlotRva) != 0 ||
            !RewriteSm3BloomSceneAlphaShader(desc.shader,{reinterpret_cast<const unsigned char*>(blob),desc.expectedSize},materialCode[i])) return false;
        auto& patch = materialPatches[i];
        patch.address = pushes[i]; patch.before.resize(5); patch.after.resize(5);
        patch.before[0] = patch.after[0] = 0x68;
        memcpy(patch.before.data()+1,&blob,4);
        const auto replacement = reinterpret_cast<uintptr_t>(materialCode[i].data());
        memcpy(patch.after.data()+1,&replacement,4);
        const unsigned char call[] = {0x50,0xff,0x91,0xa8,1,0,0};
        if (!Same(pushes[i],patch.before.data(),5) || !Same(pushes[i]+5,call,sizeof(call))) return false;
    }
    finalCall.call(0x79618A,{0xe8,0xe1,0x18,0,0},reinterpret_cast<void*>(&Composite));
    resetCall.call(0x8C1BC4,{0x68,0x88,0xc8,0x0f,1,0x50,0xff,0x52,0x40},reinterpret_cast<void*>(&Reset),true);
    return Same(finalCall.address,finalCall.before.data(),finalCall.before.size()) &&
        Same(resetCall.address,resetCall.before.data(),resetCall.before.size());
}

bool ValidateRoutes() {
    if (reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)) != 0x400000) return false;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(0x400000);
    if (!Range(dos,sizeof(*dos)) || dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0 || dos->e_lfanew > 4096) return false;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(0x400000 + dos->e_lfanew);
    if (!Range(nt,sizeof(*nt)) || nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_I386 ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC) return false;
    const unsigned char outerBytes[] = {0x55,0x8b,0xec,0x83,0xe4,0xf0,0x83,0xec,0x2c,0x53,0x33,0xdb,0x38,0x1d,0xe8,0,0xde,0};
    const unsigned char initBytes[] = {0x55,0x8b,0xec,0x83,0xe4,0xf0,0x81,0xec,0xb4,0,0,0,0x80,0x3d,0xa0,0xfc,0xe8,0};
    const unsigned char targetBytes[] = {0x64,0xa1,0,0,0,0,0x6a,0xff,0x68,0x8d,0x9e,0x9e,0};
    const unsigned char destroyBytes[] = {0x53,0x33,0xdb,0x38,0x1d,0xa0,0xfc,0xe8,0};
    const unsigned char textureBytes[] = {0x51,0x53,0x8b,0x5c,0x24,0x0c,0x83,0xfb,0xff};
    const unsigned char bindBytes[] = {0x51,0x56,0x8b,0x74,0x24,0x0c,0x85,0xf6};
    const unsigned char quadBytes[] = {0x8b,0x44,0x24,4,0x8b,8,0x83,0x39,0};
    return Same(0x795D50,outerBytes,sizeof(outerBytes)) && Same(0x7A4CF0,initBytes,sizeof(initBytes)) &&
        Same(0x8D1EB0,targetBytes,sizeof(targetBytes)) && Same(0x8C6BF0,textureBytes,sizeof(textureBytes)) &&
        Same(0x795A30,bindBytes,sizeof(bindBytes)) && Same(0x470900,quadBytes,sizeof(quadBytes)) &&
        Same(0x795AB0,destroyBytes,sizeof(destroyBytes));
}
}

static bool ValidateRestoredHost() {
    return ValidateRoutes() && ShaderIdentity(0xABCEC0,1604,0x26716EBA) &&
        ShaderIdentity(0xAC20F8,272,0x8E6825EC) && ShaderIdentity(0xAB7328,196,0xFC510D2F);
}

static void AttachForOptions(BloomRestoration::Options options) {
    if (selectedRoute != Route::Pending) return;
    if (!BloomRestoration::Selected(options)) {
        selectedRoute = Route::Original;
        AttachPostFXResearchDetours();
        return;
    }
    selectedRoute = Route::Restored;
    enabled = true;
    try {
        if (!ValidateRestoredHost() || !PreparePatches()) { enabled = false; return; }
    } catch (...) { enabled = false; return; }
    //the host opens the detours transaction before calling this function.
    const struct { PVOID* original; PVOID hook; } hooks[] = {
        {reinterpret_cast<PVOID*>(&outer),reinterpret_cast<PVOID>(&Outer)},
        {reinterpret_cast<PVOID*>(&initializeTargets),reinterpret_cast<PVOID>(&InitializeTargets)},
        {reinterpret_cast<PVOID*>(&createTarget),reinterpret_cast<PVOID>(&CreateRT)},
        {reinterpret_cast<PVOID*>(&destroyTargets),reinterpret_cast<PVOID>(&DestroyTargets)}};
    size_t queued = 0;
    for (const auto& hook : hooks) {
        if (DetourAttach(hook.original,hook.hook) != NO_ERROR) {
            while (queued) { --queued; DetourDetach(hooks[queued].original,hooks[queued].hook); }
            enabled = false;
            return;
        }
        ++queued;
    }
    hooksQueued = true;
}

static bool InstallRestoredPatches() {
    if (!enabled || !hooksQueued) return false;
    if (!resetCall.apply() || !finalCall.apply()) {
        finalCall.restore(); resetCall.restore(); return false;
    }
    for (auto& patch : materialPatches) {
        if (!patch.apply()) {
            for (auto& prior : materialPatches) prior.restore();
            finalCall.restore(); resetCall.restore(); return false;
        }
    }
    //native shader objects consume these buffers during initialization and device recreation.
    HMODULE module{};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
        reinterpret_cast<LPCWSTR>(&InstallRestoredPatches),&module)) {
        for (auto& patch : materialPatches) patch.restore();
        finalCall.restore(); resetCall.restore(); return false;
    }
    prepared = true;
    return true;
}

void BloomRestoration::Attach() {
    if (selectedRoute != Route::Pending) return;
    HMODULE module{};
    wchar_t path[MAX_PATH]{};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&BloomRestoration::Attach),&module)) return;
    const DWORD count = GetModuleFileNameW(module,path,MAX_PATH);
    if (!count || count >= MAX_PATH) return;
    auto* slash = wcsrchr(path,L'\\');
    if (!slash) return;
    wcscpy_s(slash+1,MAX_PATH-(slash+1-path),L"xeSM3.ini");
    const Options options = PublicOptions(
        GetPrivateProfileIntW(L"PostProcessing",L"PostProcessFix",100,path),
        GetPrivateProfileIntW(L"PostProcessing",L"PostProcessRetailXbox",0,path),
        GetPrivateProfileIntW(L"PostProcessing",L"PostProcessDebugXbox",0,path),
        GetPrivateProfileIntW(L"PostProcessing",L"RestoredBloom",100,path));
    AttachForOptions(options);
}

bool BloomRestoration::Finish(long transactionResult) {
    if (transactionResult != NO_ERROR) { enabled = false; return false; }
    if (selectedRoute == Route::Original) return InstallPostFXResearchCallsitePatches();
    return selectedRoute == Route::Restored && InstallRestoredPatches();
}

void BloomRestoration::OnPresent() {
    if (selectedRoute == Route::Original) PostFXResearch_OnPresent();
}
