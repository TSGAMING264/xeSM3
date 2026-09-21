#include "PostFXResearch.hpp"

#include <Windows.h>
#include <detours.h>
#include <intrin.h>
#include <guiddef.h>
#include <cstdint>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cmath>

namespace
{
    // Retail Spider-Man 3 PC addresses.
    constexpr uintptr_t kCreateRenderTargetAddress = 0x008D1EB0;
    constexpr uintptr_t kBloomLuminanceCallbackAddress = 0x00795D50;
    constexpr uintptr_t kBloomBlurCompositeAddress = 0x00797A70;

    // V10.5.49.10.14: retain the September 19 live x32dbg proof of the exact stock
    // late-frame caller relationship:
    //   00796185  call 00797D50
    //   0079618A  call 00797A70
    //   0079618F  pop ebx
    // Rather than detouring every possible entry into 00797A70 in Mode 4,
    // patch only this proven five-byte CALL. The stock 00797A70 body remains
    // byte-for-byte untouched and still runs first; xeSM3 extends the tail only
    // for this exact retail caller.
    constexpr uintptr_t kBloomCompositeLateCallsiteAddress = 0x0079618A;
    constexpr uintptr_t kBloomCompositeLateCallsiteReturn = 0x0079618F;
    constexpr uint8_t kBloomCompositeLateCallsiteOriginalCall[5] =
        { 0xE8, 0xE1, 0x18, 0x00, 0x00 };

    constexpr uintptr_t kBindTextureCachedAddress = 0x008C6BF0;
    constexpr uintptr_t kSetRenderTargetTextureAddress = 0x00795A30;
    constexpr uintptr_t kFullscreenQuadSetupAddress = 0x00470900;

    // V10.5.16: PC NGL scene callback + native sm_depth_shadow target probe.
    // Retail PC registers PRE/POST NoOps at 008A7A11/008A7A1F but leaves
    // callback type 1 (MID) empty. Xbox schedules its main depth resolve in
    // the equivalent MID slot. This build proves that exact insertion point.
    constexpr uintptr_t kNGLRegisterSceneCallbackAddress = 0x008CA660;
    constexpr uintptr_t kNGLRenderSceneAddress = 0x008D1120; // retail nglRenderScene, IDA/source matched
    constexpr uintptr_t kNGLBeginRenderNodeAddress = 0x008D0940; // retail nglBeginRenderNode, IDA/source matched
    constexpr uintptr_t kNGLAdvanceRenderNodeAddress = 0x00471090; // retail nglAdvanceRenderNode, IDA/source matched
    constexpr uintptr_t kNGLCurRenderNodeGlobal = 0x01106884;
    constexpr uintptr_t kNGLPrevRenderNodeGlobal = 0x01106888;
    constexpr uintptr_t kNGLRenderRangeStartGlobal = 0x011068F0;
    constexpr uintptr_t kNGLRenderRangeEndGlobal = 0x011068F4;
    constexpr uintptr_t kNGLRenderIndexGlobal = 0x011068F8;
    constexpr uintptr_t kWdsPostCallbackRegisterReturn = 0x008A7A24;
    constexpr uintptr_t kNGLCurrentSceneGlobal = 0x0110687C;
    constexpr uintptr_t kNGLBackBufferGlobal = 0x01106BA0;
    constexpr uintptr_t kNGLDepthCompanionGlobal = 0x01106BA4;
    constexpr uintptr_t kNGLMainDepthSurfaceGlobal = 0x01106BA8; // cached GetDepthStencilSurface for main backbuffer
    constexpr uintptr_t kShadowDepthPixelShaderGlobal = 0x010FBF68; // ps_sm_depth_shadow runtime object
    constexpr uintptr_t kShadowDepthVertexShaderGlobal = 0x010FBD94; // vs_sm_depth_shadow runtime object
    constexpr uintptr_t kPhatExecuteAddress = 0x004F9340; // SM_PhatRenderCommand_Execute
    constexpr uintptr_t kPhatShadowExecuteAddress = 0x00472240; // SM_PhatRenderCommand_ExecuteShadowPass
    constexpr uintptr_t kPhatRenderCommandVTable = 0x00A504E0; // vtable +8 -> 004F9340
    constexpr uintptr_t kRenderListSentinelGlobal = 0x00D73B60; // -> 011068C8 sentinel
    constexpr uint32_t kScenePrimaryRenderListOffset = 0x310u;
    constexpr uint32_t kSceneOpaqueRenderListOffset = 0x310u;
    constexpr uint32_t kSceneTransRenderListOffset = 0x314u;
    constexpr uint32_t kSceneOpaqueListCountOffset = 0x318u;
    constexpr uint32_t kSceneTransListCountOffset = 0x31Cu;
    constexpr uint32_t kSceneRenderTargetOffset = 0x2FCu;
    constexpr uint32_t kSceneZTargetOffset = 0x300u;
    constexpr uint32_t kSceneTargetWidthOffset = 0x304u;
    constexpr uint32_t kSceneTargetHeightOffset = 0x308u;
    constexpr uint32_t kSceneProjTypeOffset = 0x30Cu;
    constexpr uint32_t kMode4RenderListReplayCap = 8u;
    constexpr uintptr_t kShadowRenderPassFlagGlobal = 0x00F23721;
    constexpr uintptr_t kCacheRenderState16 = 0x010FD660;
    constexpr uintptr_t kCacheRenderState1B = 0x010FD674;
    constexpr uintptr_t kCacheRenderStateAF = 0x010FD8C4;
    constexpr uintptr_t kCacheRenderStateC3 = 0x010FD914;
    constexpr uint32_t kScenePreCallbackOffset = 0x2E4u;
    constexpr uint32_t kSceneMidCallbackOffset = 0x2ECu;
    constexpr uint32_t kSceneMidUserdataOffset = 0x2F0u;
    constexpr uint32_t kScenePostCallbackOffset = 0x2F4u;
    constexpr uint32_t kNGLWrapperSurfaceOffset = 0x34u;

    // V10.4.1 startup-safe native exposure path. V10.4 detoured the generic
    // scalar setter at 008D3140 and could stall the game during startup before
    // the exposure callsite was ever reached. V10.4.1 leaves 008D3140 stock and
    // patches ONLY the proven CALL instruction at 0081F12B in sub_81F100.
    constexpr uintptr_t kShaderScalarSetterAddress = 0x008D3140;
    constexpr uintptr_t kExposureScalarCallAddress = 0x0081F12B;
    constexpr uintptr_t kExposureScalarCallReturn = 0x0081F130;
    constexpr uint8_t kExposureScalarOriginalCall[5] = { 0xE8, 0x10, 0x40, 0x0B, 0x00 };

    // Retail global IDirect3DDevice9* and vtable slots. V8 installs these
    // lazily on the first present, after the real D3D9 device exists.
    constexpr uintptr_t kD3DDeviceGlobal = 0x010FC9F4;
    constexpr uint32_t kD3DResetVtableIndex = 16;
    constexpr uint32_t kD3DPresentVtableIndex = 17;
    constexpr uint32_t kD3DGetBackBufferVtableIndex = 18;
    constexpr uint32_t kD3DCreateTextureVtableIndex = 23;
    constexpr uint32_t kD3DCreateDepthStencilSurfaceVtableIndex = 29;
    constexpr uint32_t kD3DGetRenderTargetDataVtableIndex = 32;
    constexpr uint32_t kD3DStretchRectVtableIndex = 34;
    constexpr uint32_t kD3DCreateOffscreenPlainSurfaceVtableIndex = 36;
    constexpr uint32_t kD3DSetRenderTargetVtableIndex = 37;
    constexpr uint32_t kD3DGetRenderTargetVtableIndex = 38;
    constexpr uint32_t kD3DSetDepthStencilSurfaceVtableIndex = 39;
    constexpr uint32_t kD3DGetDepthStencilSurfaceVtableIndex = 40;
    constexpr uint32_t kD3DBeginSceneVtableIndex = 41;
    constexpr uint32_t kD3DEndSceneVtableIndex = 42;
    constexpr uint32_t kD3DClearVtableIndex = 43;
    constexpr uint32_t kD3DSetViewportVtableIndex = 47;
    constexpr uint32_t kD3DGetViewportVtableIndex = 48;
    constexpr uint32_t kD3DSetRenderStateVtableIndex = 57;
    constexpr uint32_t kD3DGetRenderStateVtableIndex = 58;
    constexpr uint32_t kD3DCreateStateBlockVtableIndex = 59;
    constexpr uint32_t kD3DGetTextureVtableIndex = 64;
    constexpr uint32_t kD3DSetTextureVtableIndex = 65;
    constexpr uint32_t kD3DSetTextureStageStateVtableIndex = 67;
    constexpr uint32_t kD3DGetSamplerStateVtableIndex = 68;
    constexpr uint32_t kD3DSetSamplerStateVtableIndex = 69;
    constexpr uint32_t kD3DDrawPrimitiveVtableIndex = 81;
    constexpr uint32_t kD3DDrawPrimitiveUPVtableIndex = 83;
    constexpr uint32_t kD3DSetFVFVtableIndex = 89;
    constexpr uint32_t kD3DSetVertexShaderVtableIndex = 92;
    constexpr uint32_t kD3DGetVertexShaderVtableIndex = 93;
    constexpr uint32_t kD3DSetVertexShaderConstantFVtableIndex = 94;
    constexpr uint32_t kD3DGetVertexShaderConstantFVtableIndex = 95;
    constexpr uint32_t kD3DCreatePixelShaderVtableIndex = 106;
    constexpr uint32_t kD3DSetPixelShaderVtableIndex = 107;
    constexpr uint32_t kD3DGetPixelShaderVtableIndex = 108;
    constexpr uint32_t kD3DSetPixelShaderConstantFVtableIndex = 109;
    constexpr uint32_t kD3DGetPixelShaderConstantFVtableIndex = 110;
    constexpr uint32_t kD3DTextureGetLevelDescVtableIndex = 17;
    constexpr uint32_t kD3DTextureGetSurfaceLevelVtableIndex = 18;
    constexpr uint32_t kD3DSurfaceGetContainerVtableIndex = 11;
    constexpr uint32_t kD3DSurfaceGetDescVtableIndex = 12;
    constexpr uint32_t kD3DSurfaceLockRectVtableIndex = 13;
    constexpr uint32_t kD3DSurfaceUnlockRectVtableIndex = 14;
    constexpr uint32_t kD3DStateBlockCaptureVtableIndex = 4;
    constexpr uint32_t kD3DStateBlockApplyVtableIndex = 5;

    // Camera Motion ZBlur evidence / dormant native PC shader pair.
    constexpr uintptr_t kCameraMotionZBlurVS = 0x010FBEFC;
    constexpr uintptr_t kCameraMotionZBlurPS = 0x010FBEF8;
    constexpr float kXboxCameraZBlurDistanceForMax = 0.5f;
    constexpr float kXboxCameraZBlurMin = 1.25f;
    constexpr float kXboxCameraZBlurMax = 10.5f;
    constexpr float kXboxCameraZBlurReferenceWidth = 640.0f;
    constexpr uintptr_t kGameSingletonGlobal = 0x00DE7A1C;
    constexpr uint32_t kGameSpiderCameraOffset = 0x5Cu;
    constexpr uint32_t kEntityTransformOffset = 0x10u;
    constexpr uint32_t kEntityTransformPositionOffset = 0x30u;
    constexpr uint32_t kXboxCameraZBlurHistoryCount = 4u;
    constexpr float kCameraMotionZBlurFixedMinScale =
        1.0f + (kXboxCameraZBlurMin / kXboxCameraZBlurReferenceWidth);
    // Retail PC survivor: sub_8094B0() is already called by stock game::render,
    // and its result is written to byte_D16584 immediately before the post-FX tail.
    // Do NOT call the predicate a second time from xeSM3: sub_8094B0 reaches the
    // stock world/spatial query path (sub_809470 -> sub_8076B0).  V10.5.49.10.4
    // called it out-of-band and produced a bad run.  V10.5.49.10.4.1 reconnects
    // the surviving native gate by consuming the stock byte only.
    constexpr uintptr_t kPcCameraZBlurGateByte = 0x00D16584;
    constexpr uint32_t kIUnknownReleaseVtableIndex = 2;
    constexpr LONG kRawGenericBudgetPerSampledPresent = 256;
    constexpr LONG kRawPostFxBudgetPerSampledPresent = 512;

    // V9 is intentionally a conservative proof-of-concept. It reads the
    // already-generated 16x12 FP16 luminance target back every few presents,
    // derives a bounded temporal exposure scalar, then applies that scalar as
    // one final fixed-function full-frame multiply from a raw D3D9 Present hook.
    // Running immediately before the real device Present guarantees that the
    // game's EndScene has completed. E8FBE8 remains untouched.
    constexpr uint32_t kV9ReadbackInterval = 6;
    constexpr uint32_t kV9WarmupSamples = 8;
    constexpr float kV9MinExposure = 0.70f;
    constexpr float kV9MaxExposure = 1.30f;
    constexpr float kV9DarkenAlpha = 0.18f;
    constexpr float kV9BrightenAlpha = 0.08f;

    // V10 reconstructs the dynamic bloom control recovered from the May 30
    // Xbox 360 milestone. The console computes a weighted RMS scene brightness
    // from its tiny luminance target, maps brightness 28..96 to bloom 0..1.5,
    // then scales the bloom-sample weights. PC's retail shader register layout
    // differs, so V10 applies the equivalent uniform gain to the finished
    // quarter-resolution bloom texture immediately before native final composite.
    constexpr uint32_t kV10ReadbackInterval = 6;
    constexpr float kV10XboxDarkBrightness = 28.0f;
    constexpr float kV10XboxLightBrightness = 96.0f;
    constexpr float kV10XboxDarkBloom = 0.0f;
    constexpr float kV10XboxLightBloom = 1.5f;
    // V10.5.49.5: exact Xbox Function_82F31348 luminance weights.
    // The Xbox packed byte path is B*0.11 + G*0.59 + R*0.30. PC's
    // A16B16G16R16F surface is read here as R,G,B,A half-floats, so the
    // equivalent channel weights are R=0.30, G=0.59, B=0.11.
    constexpr float kV10WeightR = 0.30f;
    constexpr float kV10WeightG = 0.59f;
    constexpr float kV10WeightB = 0.11f;

    // V10.3 reconstructed the ORIGINAL Xbox auto-exposure STATE update recovered
    // from release/May30 Function_82F31E60. V10.4 keeps that recurrence and, in
    // mode 3 only, supplies the recovered Xbox shader input (-exposureState) to
    // PC's own native "exposure" scalar parameter at the proven retail callsite.
    // No guessed exp2/gamma/full-screen transfer function is introduced.
    constexpr uintptr_t kPCGameTimeGlobal = 0x00DE7D88;
    constexpr uint32_t kPCGameTimeWholeSecondsOffset = 220u; // sub_5420F0
    constexpr uint32_t kPCGameTimeFractionOffset = 228u;     // sub_5420F0
    constexpr float kXboxSecondsToDays = 0.000011574074f;
    constexpr float kXboxTwoPi = 6.2831855f;
    constexpr float kXboxExposureInitial = 2.0f;
    constexpr float kXboxExposureMin = 0.0f;
    constexpr float kXboxExposureMax = 20.0f;
    constexpr float kXboxExposureBrightnessHigh = 100.0f;
    constexpr float kXboxExposureBrightnessLow = 50.0f;
    constexpr float kXboxExposureSpeed = 2.5f;
    constexpr float kXboxExposureDeltaScale = 0.000001f;
    constexpr float kXboxExposureFallbackTarget = 2.0f;
    constexpr float kXboxExposureFallbackKeep = 0.99f;
    constexpr float kXboxExposureFallbackBlend = 0.01f;
    constexpr float kXboxManualExposure = 0.75f;
    constexpr uint32_t kD3DPoolDefault = 0;
    constexpr uint32_t kD3DPoolSystemMem = 2;
    constexpr uint32_t kD3DUsageRenderTarget = 0x00000001u;
    constexpr uint32_t kD3DUsageDepthStencil = 0x00000002u;
    constexpr uint32_t kFormatD24S8 = 0x0000004Bu;
    constexpr uint32_t kFormatR32F = 0x00000072u;
    constexpr uint32_t kFormatINTZ = 0x5A544E49u; // MAKEFOURCC('I','N','T','Z')
    constexpr uint32_t kFormatDF24 = 0x34324644u; // MAKEFOURCC('D','F','2','4')
    constexpr uint32_t kFormatRAWZ = 0x5A574152u; // MAKEFOURCC('R','A','W','Z')
    constexpr uint32_t kFormatDF16 = 0x36314644u; // MAKEFOURCC('D','F','1','6')
    constexpr uint32_t kD3DLockReadOnly = 0x00000010u;
    constexpr uint32_t kD3DStateBlockAll = 1;
    constexpr uint32_t kD3DClearTarget = 0x00000001u;
    constexpr uint32_t kD3DClearZBuffer = 0x00000002u;
    constexpr uint32_t kD3DClearStencil = 0x00000004u;
    constexpr uint32_t kD3DFilterNone = 0;
    constexpr HRESULT kD3DErrNotFound = static_cast<HRESULT>(0x88760866u);
    constexpr uint32_t kD3DPrimitiveTriangleStrip = 5;
    constexpr uint32_t kD3DFVFXYZRHW = 0x00000004u;
    constexpr uint32_t kD3DFVFTex1 = 0x00000100u;

    // D3D9 render/texture-stage/sampler state ids used by the V9 final pass.
    constexpr uint32_t kRSZEnable = 7;
    constexpr uint32_t kRSPointSize = 154;
    constexpr uint32_t kReszResolveMagic = 0x7FA05000u; // legacy D3D9 RESZ depth resolve trigger
    constexpr uint32_t kRSZWriteEnable = 14;
    constexpr uint32_t kRSAlphaTestEnable = 15;
    constexpr uint32_t kRSCullMode = 22;
    constexpr uint32_t kRSAlphaBlendEnable = 27;
    constexpr uint32_t kRSSrcBlend = 19;
    constexpr uint32_t kRSDestBlend = 20;

    // V10.5.49: exact retail PC blend-state helper used by 00797A70/00797D50.
    // The Xbox comparison resolved the earlier V10.5.45 result: 2/2 is the
    // first DOF mask/bloom pass, while 7/8 belongs to the later destination-alpha
    // composite.  White-out occurred when pass 1 was run without its follow-up.
    constexpr uintptr_t kNglSetBlendStateAddress = 0x00470C30;
    using NglSetBlendStateFn = void(__cdecl*)(
        uint32_t, uint32_t, uint32_t, uint32_t,
        uint32_t, uint32_t, uint32_t, uint32_t);
    NglSetBlendStateFn s_nglSetBlendState =
        reinterpret_cast<NglSetBlendStateFn>(kNglSetBlendStateAddress);
    constexpr uint32_t kRSFogEnable = 28;
    constexpr uint32_t kRSStencilEnable = 52;
    constexpr uint32_t kRSTextureFactor = 60;
    constexpr uint32_t kRSLighting = 137;
    constexpr uint32_t kRSColorWriteEnable = 168;
    constexpr uint32_t kRSScissorTestEnable = 174;
    constexpr uint32_t kRSSrgbWriteEnable = 194;

    constexpr uint32_t kTSSColorOp = 1;
    constexpr uint32_t kTSSColorArg1 = 2;
    constexpr uint32_t kTSSColorArg2 = 3;
    constexpr uint32_t kTSSAlphaOp = 4;
    constexpr uint32_t kTSSAlphaArg1 = 5;
    constexpr uint32_t kTSSTexCoordIndex = 11;
    constexpr uint32_t kTSSTextureTransformFlags = 24;
    constexpr uint32_t kTopDisable = 1;
    constexpr uint32_t kTopSelectArg1 = 2;
    constexpr uint32_t kTopModulate = 4;
    constexpr uint32_t kTopModulate2X = 5;
    constexpr uint32_t kTaTexture = 2;
    constexpr uint32_t kTaTFactor = 3;

    constexpr uint32_t kSampAddressU = 1;
    constexpr uint32_t kSampAddressV = 2;
    constexpr uint32_t kSampMagFilter = 5;
    constexpr uint32_t kSampMinFilter = 6;
    constexpr uint32_t kSampMipFilter = 7;
    constexpr uint32_t kSampSrgbTexture = 11;
    constexpr uint32_t kSampMaxAnisotropy = 10;
    constexpr uint32_t kTextureAddressClamp = 3;
    constexpr uint32_t kTextureFilterNone = 0;
    constexpr uint32_t kTextureFilterPoint = 1;
    constexpr uint32_t kTextureFilterLinear = 2;

    // Exact retail return addresses inside 00797A70.
    constexpr uintptr_t kCompositeSceneBindReturn = 0x00797AD1; // scene color bound as s0 before scene->quarter pass
    constexpr uintptr_t kCompositeInitialSetRTReturn = 0x00797B8F;
    constexpr uintptr_t kCompositeBlur1BindReturn = 0x00797BCA;
    constexpr uintptr_t kCompositeBlur1QuadReturn = 0x00797C33;
    constexpr uintptr_t kCompositeBlur2QuadReturn = 0x00797C74;
    constexpr uintptr_t kCompositeFinalBindReturn = 0x00797CD5;
    constexpr uintptr_t kCompositeFinalSetRTReturn = 0x00797D01;
    // V10.5.55 PostProcessFix: return after the native final fullscreen helper call at
    // 00797D0F. At this point BE64 should already be the active pixel shader.
    constexpr uintptr_t kCompositeFinalQuadReturn = 0x00797D14;
    // Retail PC actual final fullscreen GPU draw: immediately after sub_470900
    // prepares the quad/stream state, 0x00797D25 calls IDirect3DDevice9::DrawPrimitive
    // and returns to 0x00797D2B. V10.5.61 moves the shader override across THIS call.
    constexpr uintptr_t kCompositeFinalDrawPrimitiveReturn = 0x00797D2B;
    constexpr uint32_t kD3DPixelShaderGetFunctionVtableIndex = 4u;
    constexpr uint32_t kMode5FinalShaderCaptureMaxBytes = 16384u;
    // V10.5.55 PostProcessFix: exact retail BE64 proof + valid two-instruction clamp patch.
    // Captured V10.5.52/53 bytecode is 196 bytes / FNV1a FC510D2F.
    // The failed .54 experiment proved D3D9 rejects _sat directly on BE64's TEXLD.
    // Build a legal ps_3_0 equivalent instead:
    //     texld   r0,  v0, s0
    //     mov_sat oC0, r0
    // This changes the TEXLD destination from oC0 to r0 and inserts one MOV_SAT
    // instruction before END. The resulting shader is 208 bytes.
    constexpr uint32_t kMode5FinalRetailBytecodeBytes = 196u;
    constexpr uint32_t kMode5FinalRetailBytecodeHash = 0xFC510D2Fu;
    constexpr uint32_t kMode5FinalClampTexldOpcodeIndex = 44u;
    constexpr uint32_t kMode5FinalClampDwordIndex = 45u;
    constexpr uint32_t kMode5FinalClampEndDwordIndex = 48u;
    constexpr uint32_t kMode5FinalClampOriginalTexldOpcode = 0x03000042u;
    constexpr uint32_t kMode5FinalClampOriginalDword = 0x800F0800u; // oC0
    constexpr uint32_t kMode5FinalClampPatchedDword = 0x800F0000u;  // r0
    constexpr uint32_t kMode5FinalClampOriginalEnd = 0x0000FFFFu;
    constexpr uint32_t kMode5FinalClampMovOpcode = 0x02000001u;
    constexpr uint32_t kMode5FinalClampMovDest = 0x801F0800u;       // oC0_sat
    constexpr uint32_t kMode5FinalClampMovSource = 0x80E40000u;     // r0.xyzw
    constexpr uint32_t kMode5FinalClampPatchedBytes = 208u;
    constexpr uint32_t kMode5FinalClampExpectedHash = 0x3CCDBF13u;
    // Retail PC God Rays binds &unk_E8FB78 to s1 at 0079834A; this is the
    // return address immediately after NGL_BindTextureCached. V10.5.4 uses it
    // as an observation anchor only.
    constexpr uintptr_t kGodRaysDepthBindReturn = 0x00798356;

    // V10.5.55 PostProcessFix: retain the eight proven native
    // PostFX render-target allocations inside SM3_PostProcess_Initialize
    // (007A4CF0). Ghidra/IDA cross-check:
    //   full      CALL 007A4D99 -> return 007A4D9E
    //   quarter A CALL 007A4DB8 -> return 007A4DBD
    //   quarter B CALL 007A4DCF -> return 007A4DD4
    //   lum 160   CALL 007A4DEB -> return 007A4DF0
    //   lum 16 A  CALL 007A4E01 -> return 007A4E06
    //   lum 16 B  CALL 007A4E17 -> return 007A4E1C
    //   half A    CALL 007A56C9 -> return 007A56CE
    //   half B    CALL 007A56DD -> return 007A56E2
    constexpr uintptr_t kFullSizeReturnAddress = 0x007A4D9E;
    constexpr uintptr_t kQuarterAReturnAddress = 0x007A4DBD;
    constexpr uintptr_t kQuarterBReturnAddress = 0x007A4DD4;
    constexpr uintptr_t kLuminance160ReturnAddress = 0x007A4DF0;
    constexpr uintptr_t kTerminalLuminance16x12ReturnAddress = 0x007A4E06;
    constexpr uintptr_t kSecondLuminance16x12ReturnAddress = 0x007A4E1C;
    constexpr uintptr_t kHalfAReturnAddress = 0x007A56CE;
    constexpr uintptr_t kHalfBReturnAddress = 0x007A56E2;

    constexpr uint32_t kFormatA8R8G8B8 = 0x15;
    constexpr uint32_t kFormatA16B16G16R16F = 0x71;
    constexpr uint32_t kQuarterRTFlags = 0x240;
    constexpr uint32_t kColorRTFlags = 0x40;
    constexpr uint32_t kLuminanceRTFlags = 0x40;

    // Native postFX wrapper globals.
    constexpr uintptr_t kRtQuarterA = 0x00E8FCA4;
    constexpr uintptr_t kRtQuarterB = 0x00E8FCA8;
    constexpr uintptr_t kRt160x120 = 0x00E8FAF4;
    constexpr uintptr_t kRtLuminance16x12 = 0x00E8FB04;
    constexpr uintptr_t kRtSecond16x12 = 0x00E8FBE8;
    constexpr uintptr_t kRtHalfA = 0x00E8FB54;
    constexpr uintptr_t kRtHalfB = 0x00E8FB58;

    constexpr uintptr_t kCurrentSceneGlobal = 0x0110687C;
    constexpr uintptr_t kAutoExposureEnabled = 0x00DE00E8;

    // Shader globals proven by the V6 trace.
    constexpr uintptr_t kCompositeVS = 0x010FBF28;
    constexpr uintptr_t kCompositePSInitial = 0x010FBEE8;
    constexpr uintptr_t kCompositePSBlur = 0x010FBF14;
    constexpr uintptr_t kCompositePSBlurLevel1 = 0x010FBF04; // blob 00AC2718, CTAB: blurlevel1offset
    constexpr uintptr_t kCompositePSBlurLevel2 = 0x010FBF38; // blob 00AB61C8, CTAB: blurlevel2offset
    // V10.5.49.10.1 live retail-PC proof: these tail shaders use distinct
    // full-scene texel radii rather than the Gaussian c0.
    constexpr float kNativeBloomTailLevel1RadiusPixels = 6.0f;
    constexpr float kNativeBloomTailLevel2RadiusPixels = 10.0f;
    constexpr uintptr_t kCompositePSFinal = 0x010FBE64;
    constexpr uintptr_t kCompositePSDofFinal = 0x010FBF18;

    // V10.5.49.9: native dormant PC weighted-bloom stage, now fed from the
    // shipped retail shader set. Xbox creates the structurally matching pair
    // 821DB4F8 / 821D5F78; PC creates AC20F8 / ABCEC0 into these handles.
    constexpr uintptr_t kWeightedBloomVS = 0x010FBEB4;
    constexpr uintptr_t kWeightedBloomPS = 0x010FBE48;
    constexpr uintptr_t kWeightedBloomVsTexelScale = 0x00E8FAF8;
    constexpr uintptr_t kWeightedBloomPsC0 = 0x00EE52B0;
    constexpr uintptr_t kWeightedBloomPsC1 = 0x00EE4F90;
    constexpr uintptr_t kWeightedBloomPsC2 = 0x00EE5DA0;
    constexpr uintptr_t kWeightedBloomPsC3 = 0x00EE7280;
    constexpr uintptr_t kWeightedBloomPsC4 = 0x00EE72E0;
    constexpr uintptr_t kWeightedBloomPsC5 = 0x00E8FDE0;

    // V10.5.49: exact retail-PC constants/resources used by 00797A70.
    // The full-chain test reuses the already-working PC scene->quarter and
    // Gaussian passes after the dormant DOF shader writes the depth mask.
    constexpr uintptr_t kCompositeDownsampleOffsetX = 0x00E8FAF8;
    constexpr uintptr_t kCompositeDownsampleOffsetY = 0x00E8FB74;
    constexpr uintptr_t kCompositeBlurConstPass1 = 0x00EE5EA0;
    constexpr uintptr_t kCompositeBlurConstPass2 = 0x00EE4F00;
    constexpr uintptr_t kFullscreenQuadRectA = 0x00E8FB70;
    constexpr uintptr_t kFullscreenQuadRectB = 0x00E8FBEC;
    constexpr uintptr_t kPostFxDepthDescriptor = 0x00E8FB78;
    constexpr uintptr_t kNglTextureCacheBase = 0x010FDA30; // dword_10FDA30[16]
    constexpr uintptr_t kSamplerFilterStateAddress = 0x00470730;
    constexpr uintptr_t kSamplerAddressUVAddress = 0x00470800;
    constexpr uintptr_t kActivePSCache = 0x010FDAB4;
    constexpr uintptr_t kActiveVSCache = 0x010FDAB8;

    // V10.5.48: retail Xbox BloomFinalCombineDOF control builder bridge.
    // The PC frame loop still publishes live near/far values through FUN_00797D30:
    //   00D16694 = near plane, 00D16690 = far plane.
    // Retail Xbox FUN_826B9F50 consumes the equivalent live near/far pair and
    // four DOF tuning values to build PS c0 (bloomDepthControl). The PC equivalents
    // of the four tuning values are not yet proven, so keep the observed Xbox
    // defaults frozen and vary ONLY the proven PC near/far inputs in this build.
    // Register map is locked by the shipped PC shader CTAB:
    //   c0=bloomDepthControl, s0=colorSampler, s1=depthSampler.
    constexpr uintptr_t kPcPostFxNearPlane = 0x00D16694;
    constexpr uintptr_t kPcPostFxFarPlane = 0x00D16690;
    constexpr float kXboxDofControlDistanceDefault = 300.0f;
    constexpr float kXboxDofControlFarDefault = 500.0f;
    constexpr float kXboxDofControlScaleDefault = 1.0f;
    constexpr float kXboxDofControlZDefault = 0.8f;
    constexpr float kMode4BloomDepthControlFallback[4] =
    {
        3000.0f,
        0.9996749973f,
        0.8f,
        0.9998083317f
    };

    struct D3DSurfaceDescLite
    {
        uint32_t Format;
        uint32_t Type;
        uint32_t Usage;
        uint32_t Pool;
        uint32_t MultiSampleType;
        uint32_t MultiSampleQuality;
        uint32_t Width;
        uint32_t Height;
    };

    struct DepthTextureRoundTripLite
    {
        HRESULT createHr = E_PENDING;
        HRESULT surfaceHr = E_PENDING;
        HRESULT descHr = E_PENDING;
        HRESULT containerHr = E_PENDING;
        uint32_t texture = 0;
        uint32_t surface = 0;
        uint32_t containerTexture = 0;
        uint32_t containerMatchesTexture = 0;
        D3DSurfaceDescLite desc = {};
    };

    struct D3DLockedRectLite
    {
        int Pitch;
        void* pBits;
    };

    struct D3DViewportLite
    {
        uint32_t X;
        uint32_t Y;
        uint32_t Width;
        uint32_t Height;
        float MinZ;
        float MaxZ;
    };

    struct ExposureVertex
    {
        float x, y, z, rhw;
        float u, v;
    };

    using CreateRenderTarget_t = int(__cdecl*)(uint32_t, int, int, int, int, int);
    using BloomCallback_t = void(__cdecl*)();
    using BloomComposite_t = int(__cdecl*)();
    using BindTextureCached_t = void(__cdecl*)(int, int);
    using SetRenderTarget_t = int(__cdecl*)(int);
    using FullscreenQuadSetup_t = int(__cdecl*)(void*, void*, unsigned int, int);
    using NGLSceneCallback_t = void(__cdecl*)(void*);
    using NGLRegisterSceneCallback_t = void(__cdecl*)(int, NGLSceneCallback_t, void*);
    using NGLRenderScene_t = int(__cdecl*)();
    using NGLBeginRenderNode_t = int(__cdecl*)(int);
    using NGLAdvanceRenderNode_t = int(__cdecl*)();
    using NGLNodeRender_t = void(__thiscall*)(void*);
    using PhatRenderCommandExecute_t = void(__fastcall*)(void*);
    using RtlCaptureStackBackTrace_t = USHORT (WINAPI*)(ULONG, ULONG, PVOID*, PULONG);
    using ShaderScalarSetter_t = int(__cdecl*)(int, float);
    using SetSamplerFilterState_t = int(__cdecl*)(int, int, int, int, int);
    using SetSamplerAddressUV_t = int(__cdecl*)(int, int, int);
    using D3DReset_t = HRESULT(WINAPI*)(void*, void*);
    using D3DPresent_t = HRESULT(WINAPI*)(void*, const void*, const void*, void*, const void*);
    using D3DGetBackBuffer_t = HRESULT(WINAPI*)(void*, UINT, UINT, DWORD, void**);
    using D3DCreateTexture_t = HRESULT(WINAPI*)(void*, UINT, UINT, UINT, DWORD, DWORD, DWORD, void**, HANDLE*);
    using D3DCreateDepthStencilSurface_t = HRESULT(WINAPI*)(void*, UINT, UINT, DWORD, DWORD, DWORD, BOOL, void**, HANDLE*);
    using D3DGetRenderTargetData_t = HRESULT(WINAPI*)(void*, void*, void*);
    using D3DStretchRect_t = HRESULT(WINAPI*)(void*, void*, const void*, void*, const void*, DWORD);
    using D3DCreateOffscreenPlainSurface_t = HRESULT(WINAPI*)(void*, UINT, UINT, DWORD, DWORD, void**, HANDLE*);
    using D3DSetRenderTarget_t = HRESULT(WINAPI*)(void*, DWORD, void*);
    using D3DGetRenderTarget_t = HRESULT(WINAPI*)(void*, DWORD, void**);
    using D3DSetDepthStencilSurface_t = HRESULT(WINAPI*)(void*, void*);
    using D3DGetDepthStencilSurface_t = HRESULT(WINAPI*)(void*, void**);
    using D3DBeginScene_t = HRESULT(WINAPI*)(void*);
    using D3DEndScene_t = HRESULT(WINAPI*)(void*);
    using D3DClear_t = HRESULT(WINAPI*)(void*, DWORD, const void*, DWORD, DWORD, float, DWORD);
    using D3DSetViewport_t = HRESULT(WINAPI*)(void*, const D3DViewportLite*);
    using D3DGetViewport_t = HRESULT(WINAPI*)(void*, D3DViewportLite*);
    using D3DSetRenderState_t = HRESULT(WINAPI*)(void*, DWORD, DWORD);
    using D3DGetRenderState_t = HRESULT(WINAPI*)(void*, DWORD, DWORD*);
    using D3DCreateStateBlock_t = HRESULT(WINAPI*)(void*, DWORD, void**);
    using D3DGetTexture_t = HRESULT(WINAPI*)(void*, DWORD, void**);
    using D3DSetTexture_t = HRESULT(WINAPI*)(void*, DWORD, void*);
    using D3DSetTextureStageState_t = HRESULT(WINAPI*)(void*, DWORD, DWORD, DWORD);
    using D3DGetSamplerState_t = HRESULT(WINAPI*)(void*, DWORD, DWORD, DWORD*);
    using D3DSetSamplerState_t = HRESULT(WINAPI*)(void*, DWORD, DWORD, DWORD);
    using D3DDrawPrimitive_t = HRESULT(WINAPI*)(void*, DWORD, UINT, UINT);
    using D3DDrawPrimitiveUP_t = HRESULT(WINAPI*)(void*, DWORD, UINT, const void*, UINT);
    using D3DSetFVF_t = HRESULT(WINAPI*)(void*, DWORD);
    using D3DSetVertexShader_t = HRESULT(WINAPI*)(void*, void*);
    using D3DGetVertexShader_t = HRESULT(WINAPI*)(void*, void**);
    using D3DSetVertexShaderConstantF_t = HRESULT(WINAPI*)(void*, UINT, const float*, UINT);
    using D3DGetVertexShaderConstantF_t = HRESULT(WINAPI*)(void*, UINT, float*, UINT);
    using D3DCreatePixelShader_t = HRESULT(WINAPI*)(void*, const DWORD*, void**);
    using D3DSetPixelShader_t = HRESULT(WINAPI*)(void*, void*);
    using D3DGetPixelShader_t = HRESULT(WINAPI*)(void*, void**);
    using D3DCompile_t = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const void*, void*, LPCSTR, LPCSTR, UINT, UINT, void**, void**);
    using D3DBlobGetBufferPointer_t = void*(WINAPI*)(void*);
    using D3DBlobGetBufferSize_t = SIZE_T(WINAPI*)(void*);
    using D3DSetPixelShaderConstantF_t = HRESULT(WINAPI*)(void*, UINT, const float*, UINT);
    using D3DGetPixelShaderConstantF_t = HRESULT(WINAPI*)(void*, UINT, float*, UINT);
    using D3DTextureGetLevelDesc_t = HRESULT(WINAPI*)(void*, UINT, D3DSurfaceDescLite*);
    using D3DTextureGetSurfaceLevel_t = HRESULT(WINAPI*)(void*, UINT, void**);
    using D3DSurfaceGetContainer_t = HRESULT(WINAPI*)(void*, REFIID, void**);
    using D3DSurfaceGetDesc_t = HRESULT(WINAPI*)(void*, D3DSurfaceDescLite*);
    using D3DSurfaceLockRect_t = HRESULT(WINAPI*)(void*, D3DLockedRectLite*, const void*, DWORD);
    using D3DSurfaceUnlockRect_t = HRESULT(WINAPI*)(void*);
    using D3DStateBlockCapture_t = HRESULT(WINAPI*)(void*);
    using D3DStateBlockApply_t = HRESULT(WINAPI*)(void*);
    using IUnknownRelease_t = ULONG(WINAPI*)(void*);

    // Local copy of IID_IDirect3DTexture9 so V8.1 can query a surface's
    // owning texture without adding a d3d9.lib dependency to the hook.
    const GUID kIID_IDirect3DTexture9 =
    { 0x85c31227, 0x3de5, 0x4f00, { 0x9b, 0x3a, 0xf1, 0x1a, 0xc3, 0x8c, 0x18, 0xb5 } };

    CreateRenderTarget_t s_originalCreateRT = reinterpret_cast<CreateRenderTarget_t>(kCreateRenderTargetAddress);
    BloomCallback_t s_originalBloomCallback = reinterpret_cast<BloomCallback_t>(kBloomLuminanceCallbackAddress);
    BloomComposite_t s_originalBloomComposite = reinterpret_cast<BloomComposite_t>(kBloomBlurCompositeAddress);
    BindTextureCached_t s_originalBindTexture = reinterpret_cast<BindTextureCached_t>(kBindTextureCachedAddress);
    SetRenderTarget_t s_originalSetRT = reinterpret_cast<SetRenderTarget_t>(kSetRenderTargetTextureAddress);
    FullscreenQuadSetup_t s_originalFullscreenQuad = reinterpret_cast<FullscreenQuadSetup_t>(kFullscreenQuadSetupAddress);
    NGLRegisterSceneCallback_t s_originalRegisterSceneCallback = reinterpret_cast<NGLRegisterSceneCallback_t>(kNGLRegisterSceneCallbackAddress);
    NGLRenderScene_t s_originalNGLRenderScene = reinterpret_cast<NGLRenderScene_t>(kNGLRenderSceneAddress);
    NGLBeginRenderNode_t s_nglBeginRenderNode = reinterpret_cast<NGLBeginRenderNode_t>(kNGLBeginRenderNodeAddress);
    NGLAdvanceRenderNode_t s_nglAdvanceRenderNode = reinterpret_cast<NGLAdvanceRenderNode_t>(kNGLAdvanceRenderNodeAddress);
    PhatRenderCommandExecute_t s_originalPhatRenderCommandExecute = reinterpret_cast<PhatRenderCommandExecute_t>(kPhatExecuteAddress);
    PhatRenderCommandExecute_t s_shadowPhatRenderCommandExecute = reinterpret_cast<PhatRenderCommandExecute_t>(kPhatShadowExecuteAddress);
    ShaderScalarSetter_t s_stockShaderScalarSetter = reinterpret_cast<ShaderScalarSetter_t>(kShaderScalarSetterAddress);
    SetSamplerFilterState_t s_setSamplerFilterState = reinterpret_cast<SetSamplerFilterState_t>(kSamplerFilterStateAddress);
    SetSamplerAddressUV_t s_setSamplerAddressUV = reinterpret_cast<SetSamplerAddressUV_t>(kSamplerAddressUVAddress);

    D3DReset_t s_rawReset = nullptr;
    D3DPresent_t s_rawPresent = nullptr;
    D3DGetBackBuffer_t s_rawGetBackBuffer = nullptr;
    D3DCreateTexture_t s_rawCreateTexture = nullptr;
    D3DCreateDepthStencilSurface_t s_rawCreateDepthStencilSurface = nullptr;
    D3DGetRenderTargetData_t s_rawGetRenderTargetData = nullptr;
    D3DStretchRect_t s_rawStretchRect = nullptr;
    D3DCreateOffscreenPlainSurface_t s_rawCreateOffscreenPlainSurface = nullptr;
    D3DSetRenderTarget_t s_rawSetRenderTarget = nullptr;
    D3DGetRenderTarget_t s_rawGetRenderTarget = nullptr;
    D3DSetDepthStencilSurface_t s_rawSetDepthStencilSurface = nullptr;
    D3DGetDepthStencilSurface_t s_rawGetDepthStencilSurface = nullptr;
    D3DBeginScene_t s_rawBeginScene = nullptr;
    D3DEndScene_t s_rawEndScene = nullptr;
    D3DSetViewport_t s_rawSetViewport = nullptr;
    D3DSetRenderState_t s_rawSetRenderState = nullptr;
    D3DGetRenderState_t s_rawGetRenderState = nullptr;
    D3DCreateStateBlock_t s_rawCreateStateBlock = nullptr;
    D3DSetTexture_t s_rawSetTexture = nullptr;
    D3DSetTextureStageState_t s_rawSetTextureStageState = nullptr;
    D3DSetSamplerState_t s_rawSetSamplerState = nullptr;
    D3DDrawPrimitive_t s_rawDrawPrimitive = nullptr;
    D3DDrawPrimitiveUP_t s_rawDrawPrimitiveUP = nullptr;
    D3DSetFVF_t s_rawSetFVF = nullptr;
    D3DSetVertexShader_t s_rawSetVertexShader = nullptr;
    D3DSetVertexShaderConstantF_t s_rawSetVertexShaderConstantF = nullptr;
    D3DCreatePixelShader_t s_rawCreatePixelShader = nullptr;
    D3DSetPixelShader_t s_rawSetPixelShader = nullptr;
    D3DSetPixelShaderConstantF_t s_rawSetPixelShaderConstantF = nullptr;

    enum class EventType : uint32_t
    {
        CreateRT,
        BloomCallback,
        CompositeEnter,
        NativeSetRT,
        RouteBlur1,
        RouteBlur2,
        RouteFinal,
        RouteMismatch,
        CompositeExit,
        Summary,
        RawHookStatus,
        DeviceReset,
        RawTextureExact,
        RawRenderTargetExact,
        RawSetTexture,
        RawSetRenderTarget,
        RawDepthStencilCreate,
        RawDepthStencilBind,
        RawSetPixelShader,
        RawPixelShaderConstantF,
        RawIdentityStatus,
        RawSummary,
        ExposureConfig,
        ExposureSample,
        ExposureApply,
        ExposureSummary,
        BloomControlConfig,
        BloomControlSample,
        BloomScaleApply,
        BloomControlSummary,
        CalibrationSample,
        XboxExposureStateConfig,
        XboxExposureStateSample,
        NativeExposureApply,
        NativeExposureSummary,
        NativeExposurePatchStatus,
        BloomCompositeCallsitePatchStatus,
        DofFinalState,
        DofFinalSummary,
        DofDepthProbe,
        GodRaysDepthBindProbe,
        DepthTextureCaps,
        DepthBridgeProbe,
        DepthReplacementTest,
        DepthPackProbe,
        MidSceneCallbackInstall,
        MidSceneDepthProbe,
        MidSceneResolveProbe,
        ShadowDepthTargetProbe,
        FullResDepthTargetProbe,
        MainCameraDepthPrepass,
        MainCameraDepthReadback,
        RenderListTopologyProfile,
        ShadowGraphRTPin,
        PhatCallerProfile,
        SceneListProfile,
        SceneNodeClassProfile,
        SceneTreeProfileSummary,
        OffscreenIntzTargetProbe,
        IntzSampleReadbackProbe,
        OpaqueIntzReplayProbe,
        OpaqueIntzTraversalProof,
        OpaqueIntzDofSemanticProbe,
        ProvenIntzDofActivation,
        DofC0BridgeProbe,
        FullDofChain,
        NativeBloomTail,
        CameraMotionZBlur,
        CameraMotionZBlurProbe,
        CameraMotionZBlurCopyOnly,
        CameraMotionZBlurBindOnly,
        CameraMotionZBlurTextureBindOnly,
        CameraMotionZBlurSamplerStateOnly,
        CameraMotionZBlurRenderStateOnly,
        CameraMotionZBlurOutputTargetOnly,
        ReszDepthResolveProbe,
        NativeFinalShaderBind,
        NativeFinalShaderBytecodeHeader,
        NativeFinalShaderBytecodeChunk,
        NativeFinalShaderCloneOverride,
        RetailXboxDofContractProbe,
        RetailXboxDepthFeedProbe,
        RetailXboxDofNativeDraw,
        RetailXboxZBlurContractProbe,
        RetailXboxZBlurShaderChunk,
        RetailXboxImageZoomRouteBindProbe,
        RetailXboxImageZoomNativeDraw,
        Mode5DedicatedDrawHookStatus,
        RetailXboxActualFinalDraw,
        NativeFinalShaderActualClampDraw,
        NativeFinalShaderSummary,
        RetailXboxIntegrationHeartbeat,
        DebugXboxD2Probe,
        GodRayG1SelectorTransition,
        GodRayG1FinalContract,
        GodRayG1FinalConstants,
        GodRayG1VertexConstants,
        GodRayG1FinalStates,
        GodRayG2BindStateProbe,
        GodRayG3FirstRealDraw,
        GodRayG4OwnedDrawSuppress,
        PermanentReszDofHeartbeat
    };

    struct Event
    {
        EventType type;
        uint32_t a, b, c, d, e, f, g, h, i, j;
        uint32_t extra[32];
        char text[48];
    };

    constexpr uint32_t kEventCapacity = 8192;
    Event s_events[kEventCapacity] = {};
    volatile LONG s_eventWrite = 0;
    volatile LONG s_eventRead = 0;
    volatile LONG s_droppedEvents = 0;

    HANDLE s_logFile = INVALID_HANDLE_VALUE;
    uint32_t s_presentCount = 0;

    volatile LONG s_bloomCallbackCount = 0;
    volatile LONG s_compositeCount = 0;
    volatile LONG s_routeBlur1Count = 0;
    volatile LONG s_routeBlur2Count = 0;
    volatile LONG s_routeFinalCount = 0;
    volatile LONG s_routeMismatchCount = 0;
    // V10.5.55 integrated native final-shader clamp override.
    // Stage 5.1 captures BE64 exactly as V10.5.52 did, then rewrites only
    // the final output sequence from direct TEXLD-to-oC0 to TEXLD r0 followed
    // by MOV_SAT oC0,r0. The patched shader is bound only around the proven
    // native final fullscreen draw, then the exact original is restored.
    volatile LONG s_mode5FinalShaderBindCount = 0;
    volatile LONG s_mode5FinalShaderCaptureState = 0; // 0=waiting,1=capturing,2=captured,-1=failed
    volatile uint32_t s_mode5FinalShaderBytecodeSize = 0;
    volatile uint32_t s_mode5FinalShaderBytecodeHash = 0;
    alignas(4) uint8_t s_mode5FinalShaderBytecode[kMode5FinalShaderCaptureMaxBytes] = {};
    alignas(4) uint8_t s_mode5FinalCloneVerifyBytecode[kMode5FinalShaderCaptureMaxBytes] = {};
    alignas(4) uint8_t s_mode5FinalPatchedBytecode[kMode5FinalShaderCaptureMaxBytes] = {};
    volatile LONG s_mode5FinalClampPatchState = 0; // 0=waiting,1=patched, -1=source mismatch
    volatile uint32_t s_mode5FinalClampPatchedHash = 0u;
    volatile uint32_t s_mode5FinalClampSourceWord = 0u;
    volatile uint32_t s_mode5FinalClampPatchedWord = 0u;
    volatile uint32_t s_mode5FinalClampPatchedBytes = 0u;
    void* s_mode5FinalCloneShader = nullptr;
    volatile uint32_t s_mode5FinalCloneDevice = 0u;
    volatile LONG s_mode5FinalCloneState = 0; // 0=waiting,1=creating,2=ready,-1=failed
    volatile LONG s_mode5FinalCloneDrawCount = 0;
    volatile LONG s_mode5FinalClonePassCount = 0;
    volatile LONG s_mode5FinalCloneFailCount = 0;
    volatile uint32_t s_mode5FinalCloneBytecodeHash = 0u;

    // V10.5.56 Retail Xbox Stage R1: probe the dormant PC F18
    // BloomFinalCombineDOF contract at the proven native final draw. The probe
    // binds and readback-verifies F18, captures s0/s1/c0, then restores BE64
    // BEFORE any draw. The visible draw continues through the proven .55 clamp.
    volatile LONG s_retailXboxDofProbeAttemptCount = 0;
    volatile LONG s_retailXboxDofProbePassCount = 0;
    volatile LONG s_retailXboxDofProbeFailCount = 0;
    volatile uint32_t s_retailXboxDofShaderBytes = 0u;
    volatile uint32_t s_retailXboxDofShaderHash = 0u;

    // V10.5.58 Retail Xbox Stage R3: reuse the proven same-present
    // MID-scene RESZ -> INTZ -> packed A8R8G8B8 producer and finally allow the
    // dormant F18 BloomFinalCombineDOF shader to perform SM3's EXISTING native
    // final fullscreen draw. This is fail-closed: stale/missing depth, an
    // unverified s0/s1/c0 contract, or any pre-draw bind failure falls back to
    // the proven .55/.54.1 final clamp. There is never an xeSM3 extra draw.
    volatile LONG s_retailXboxDepthApiState = 0; // 0=waiting, 1=ready, -1=failed
    volatile LONG s_retailXboxDepthArmCount = 0;
    volatile LONG s_retailXboxDepthArmFailCount = 0;
    volatile LONG s_retailXboxDepthFeedAttemptCount = 0;
    volatile LONG s_retailXboxDepthFeedPassCount = 0;
    volatile LONG s_retailXboxDepthFeedFailCount = 0;
    volatile LONG s_retailXboxDofDrawAttemptCount = 0;
    volatile LONG s_retailXboxDofDrawCount = 0;
    volatile LONG s_retailXboxDofDrawPassCount = 0;
    volatile LONG s_retailXboxDofDrawFallbackCount = 0;
    volatile LONG s_retailXboxDofDrawFailCount = 0;
    volatile LONG s_retailXboxDofDrawDisabled = 0; // set only after a post-draw integrity failure
    uint32_t s_retailXboxDepthLastArmedPresent = 0xFFFFFFFFu;

    // V10.5.61 retained R4 camera-motion/ZBlur contract capture.
    // The retail-PC ZBlur shader pair is initialized by stock code but has no
    // retail-PC draw XREF. R4 therefore performs NO shader bind, texture bind,
    // render-target change, copy, or draw. It only captures the dormant shader
    // bytecode and samples the live stock gate/camera/scene context at the exact
    // native final fullscreen boundary while the proven R3 F18 route remains intact.
    volatile LONG s_retailXboxZBlurProbeCount = 0;
    volatile LONG s_retailXboxZBlurCaptureState = 0; // 0=waiting, 1=capturing, 2=captured, -1=failed
    volatile LONG s_retailXboxZBlurVsCaptureState = 0;
    volatile LONG s_retailXboxZBlurPsCaptureState = 0;
    volatile uint32_t s_retailXboxZBlurVsBytes = 0u;
    volatile uint32_t s_retailXboxZBlurVsHash = 0u;
    volatile uint32_t s_retailXboxZBlurVsVersion = 0u;
    volatile uint32_t s_retailXboxZBlurVsLastToken = 0u;
    volatile uint32_t s_retailXboxZBlurVsQueryHr = 0xFFFFFFFFu;
    volatile uint32_t s_retailXboxZBlurVsFetchHr = 0xFFFFFFFFu;
    volatile uint32_t s_retailXboxZBlurPsBytes = 0u;
    volatile uint32_t s_retailXboxZBlurPsHash = 0u;
    volatile uint32_t s_retailXboxZBlurPsVersion = 0u;
    volatile uint32_t s_retailXboxZBlurPsLastToken = 0u;
    volatile uint32_t s_retailXboxZBlurPsQueryHr = 0xFFFFFFFFu;
    volatile uint32_t s_retailXboxZBlurPsFetchHr = 0xFFFFFFFFu;
    alignas(4) uint8_t s_retailXboxZBlurVsBytecode[kMode5FinalShaderCaptureMaxBytes] = {};
    alignas(4) uint8_t s_retailXboxZBlurPsBytecode[kMode5FinalShaderCaptureMaxBytes] = {};

    // V10.5.61 retained R5 ImageZoom route/bind probe, now retimed after the actual GPU draw.
    // The source is a dedicated copy of the already-finished Scene RT made AFTER
    // the proven R3 F18 native final draw. The current Scene RT remains the target.
    // R5 verifies source != target, then temporarily binds the dormant PC ImageZoom
    // VS/PS, VS c0 (zoombase), source texture and sampler 0, reads everything back,
    // restores exact prior state, and issues ZERO ImageZoom draws / target writes.
    volatile LONG s_retailXboxImageZoomProbeCount = 0;
    volatile LONG s_retailXboxImageZoomEligibleCount = 0;
    volatile LONG s_retailXboxImageZoomPassCount = 0;
    volatile LONG s_retailXboxImageZoomFailCount = 0;
    volatile LONG s_retailXboxImageZoomSkipCount = 0;
    void* s_retailXboxImageZoomSourceTexture = nullptr;
    void* s_retailXboxImageZoomSourceSurface = nullptr;
    uint32_t s_retailXboxImageZoomSourceWidth = 0u;
    uint32_t s_retailXboxImageZoomSourceHeight = 0u;
    DWORD s_retailXboxImageZoomSourceFormat = 0u;

    // V10.5.63 Retail Xbox R6: first real camera-motion ImageZoom draw.
    // It runs only after a verified actual F18 final DrawPrimitive. The finished
    // target is copied to the already-proven non-aliased safe source, then the
    // dormant PC ImageZoom VS/PS + exact Retail VS c0 + linear/clamp sampler +
    // no-blend fullscreen state are verified before ONE direct original
    // DrawPrimitive(TRIANGLESTRIP,0,2). Any pre-draw failure skips ZBlur. Any
    // post-draw integrity failure disables future ImageZoom draws for the session.
    volatile LONG s_retailXboxImageZoomDrawAttemptCount = 0;
    volatile LONG s_retailXboxImageZoomDrawCount = 0;
    volatile LONG s_retailXboxImageZoomDrawPassCount = 0;
    volatile LONG s_retailXboxImageZoomDrawFailCount = 0;
    volatile LONG s_retailXboxImageZoomDrawSkipCount = 0;
    volatile LONG s_retailXboxImageZoomDrawDisabled = 0;

    // V10.5.61 R6A: actual GPU-draw boundary proof. Earlier R3/R5 experiments
    // wrapped sub_470900 (quad/stream setup), not the following D3D9 DrawPrimitive.
    // These counters cover the real 0x00797D25 -> 0x00797D2B GPU draw.
    volatile LONG s_actualFinalDrawAttemptCount = 0;
    volatile LONG s_actualFinalF18DrawCount = 0;
    volatile LONG s_actualFinalF18PassCount = 0;
    volatile LONG s_actualFinalF18FailCount = 0;
    volatile LONG s_actualFinalClampDrawCount = 0;
    volatile LONG s_actualFinalClampPassCount = 0;
    volatile LONG s_actualFinalClampFailCount = 0;
    volatile LONG s_actualFinalStockFallbackCount = 0;
    volatile LONG s_actualFinalF18Disabled = 0;

    // V10.5.72 Mode 5 retains only the narrow production D3D9 hooks: Reset slot 16
    // plus DrawPrimitive slot 81. This remains intentionally separate from
    // s_rawHookState / TryAttachRawD3DDetours(), whose broad research bundle stays
    // unreachable in Mode 5.
    volatile LONG s_mode5DrawHookState = 0; // 0=waiting,1=attaching,2=attached,-1=hard failure
    volatile LONG s_mode5DrawHookAttemptCount = 0;
    volatile LONG s_mode5DrawHookAttachCount = 0;
    volatile LONG s_mode5DrawHookFailCount = 0;
    volatile uint32_t s_mode5DrawHookDevice = 0u;
    volatile uint32_t s_mode5DrawHookTarget = 0u;
    volatile uint32_t s_mode5ResetHookTarget = 0u;
    volatile uint32_t s_mode5DrawHookLastError = 0u;
    volatile LONG s_bindLum16Count = 0;
    volatile LONG s_bindSecond16Count = 0;
    volatile LONG s_setRTLum16Count = 0;
    volatile LONG s_setRTSecond16Count = 0;

    // V8 raw D3D9 counters/state. No V7 rendering state is changed here.
    volatile LONG s_rawHookState = 0; // 0=waiting, 1=attaching, 2=attached, -1=failed
    volatile LONG s_rawGenericEventCountThisPresent = 0;
    volatile LONG s_rawPostFxEventCountThisPresent = 0;
    volatile LONG s_rawLumTextureCount = 0;
    volatile LONG s_rawSecondTextureCount = 0;
    volatile LONG s_rawLumSetRTCount = 0;
    volatile LONG s_rawSecondSetRTCount = 0;
    volatile LONG s_rawSetPixelShaderCount = 0;
    volatile LONG s_rawPSConstantCount = 0;

    volatile uint32_t s_rawCurrentPixelShader = 0;
    volatile uint32_t s_rawCurrentRT0 = 0;
    volatile uint32_t s_rawTextureStages[16] = {};
    volatile uint32_t s_rawTextureQuarterA = 0;
    volatile uint32_t s_rawTextureQuarterB = 0;
    volatile uint32_t s_rawTexture160 = 0;
    volatile uint32_t s_rawTextureLum16 = 0;
    volatile uint32_t s_rawTextureSecond16 = 0;
    volatile uint32_t s_rawTextureHalfA = 0;
    volatile uint32_t s_rawTextureHalfB = 0;
    volatile uint32_t s_rawTextureScene = 0;
    volatile uint32_t s_rawSurfaceQuarterA = 0;
    volatile uint32_t s_rawSurfaceQuarterB = 0;
    volatile uint32_t s_rawSurfaceLum16 = 0;
    volatile uint32_t s_rawSurfaceSecond16 = 0;
    volatile uint32_t s_rawSurfaceScene = 0;
    volatile uint32_t s_rawLumGetContainerHr = 0xFFFFFFFFu;
    volatile uint32_t s_rawSecondGetContainerHr = 0xFFFFFFFFu;
    volatile uint32_t s_rawLumIdentitySource = 0;
    volatile uint32_t s_rawSecondIdentitySource = 0;
    volatile uint32_t s_rawDeviceValue = 0;

    // V9 adaptive-exposure proof-of-concept state.
    bool s_v9ConfigRead = false;
    bool s_v9ExposureEnabled = true;
    bool s_v9ConfigQueued = false;
    void* s_v9ReadbackSurface = nullptr;
    void* s_v9SceneCopyTexture = nullptr;
    void* s_v9SceneCopySurface = nullptr;
    void* s_v9StateBlock = nullptr;
    uint32_t s_v9SceneResourceIdentity = 0;
    uint32_t s_v9SceneWidth = 0;
    uint32_t s_v9SceneHeight = 0;
    uint32_t s_v9SceneFormat = 0;
    uint32_t s_v9ReadbackFormat = 0;
    uint32_t s_v9ReadbackWidth = 0;
    uint32_t s_v9ReadbackHeight = 0;
    uint32_t s_v9SampleCount = 0;
    uint32_t s_v9ApplyCount = 0;
    uint32_t s_v9WarmupCount = 0;
    float s_v9WarmupSum = 0.0f;
    float s_v9ReferenceLuminance = 0.0f;
    float s_v9CurrentLuminance = 0.0f;
    float s_v9TargetExposure = 1.0f;
    float s_v9AdaptedExposure = 1.0f;
    HRESULT s_v9LastReadbackHr = E_PENDING;
    HRESULT s_v9LastLockHr = E_PENDING;
    HRESULT s_v9LastStretchHr = E_PENDING;
    HRESULT s_v9LastDrawHr = E_PENDING;
    HRESULT s_v9LastApplyHr = E_PENDING;
    volatile LONG s_v9PresentInFlight = 0;
    uint32_t s_v9LastProcessedPresent = 0;

    // xeSM3 PostFX research mode:
    //   0 = exact V7 visual baseline (V8/V8.1 diagnostics only)
    //   1 = V7 + Xbox-style dynamic bloom control
    //   2 = frozen V10.1 reference: mode 1 + V9 full-frame adaptive-exposure POC
    //   3 = V10.4.1 SpeedTree exposure research
    //   4 = V10.5.23 native shadow-graph depth prepass + shadow-PS RT0 pin
    bool s_v10ConfigRead = false;
    bool s_v10ConfigQueued = false;
    int s_v10Mode = 5;

    // V10.5.66 user-facing PostFX route. Retail Xbox stays locked on the proven .63 chain.
    // Debug Xbox D2 intentionally reuses the same shared Xbox contracts proved byte-identical
    // across Debug/Retail (F18, ImageZoom shaders, 0.5/1.25/10.5 controller) and adds only
    // Debug adaptive bloom is now applied on route 3; GodRay selector remains read-only and no GodRay pixels are added.
    bool s_postProcessFixEnabled = true;
    bool s_postProcessRetailXboxEnabled = false;
    bool s_postProcessDebugXboxEnabled = false;
    bool s_postProcessNativeRepairActive = true;
    uint32_t s_postProcessRoute = 1u; // 0=RetailPC, 1=Fix, 2=RetailXbox, 3=DebugXboxD2
    char s_postProcessIniPath[MAX_PATH] = {};
    // V10.5.63 integration logging: preserve failure proof + sparse heartbeats while
    // removing the per-frame research spam used during R1-R6 qualification.
    constexpr bool kRetailXboxIntegrationQuietLogging = true;
    constexpr bool kRetailXboxLogShaderBytecodeChunks = false;
    // V10.5.72 Alt-Tab/device-reset fix: preserve the qualified V10.5.71
    // D2 + F18 + ImageZoom/ZBlur + xeSM3-owned GodRay pixels exactly as V10.5.71.
    // Production logging keeps selector transitions, failures and the 300-present
    // health heartbeat, but disables successful research-detail events by default.
    constexpr bool kDebugXboxIntegrationLockdownQuietLogging = true;
    constexpr bool kDebugXboxProductionSuccessDetailLogging = false;
    constexpr uint32_t kDebugXboxIntegrationValidationInterval = 1800u;
    constexpr uint32_t kRetailXboxHeartbeatPresents = 300u;

    inline bool IsXboxPostProcessRoute()
    {
        return s_postProcessRoute == 2u || s_postProcessRoute == 3u;
    }

    // V10.5.66 Debug Xbox D2 + GodRay G1: source-proven defaults from Release 2 / exact May-30
    // PostFX function. D2 feeds only the adaptive bloom value into the guarded weighted bloom working pass; GodRay remains telemetry-only.
    constexpr float kDebugXboxDarkBrightness = 28.0f;
    constexpr float kDebugXboxLightBrightness = 96.0f;
    constexpr float kDebugXboxDarkBloom = 0.0f;
    constexpr float kDebugXboxLightBloom = 1.5f;
    constexpr float kRetailXboxDarkBloomCompare = 3.5f;
    constexpr float kRetailXboxLightBloomCompare = 1.5f;
    constexpr uintptr_t kPcDormantGodRaySelector = 0x00EE5E90u;

    // V10.5.66 Xbox GodRay G1: passive contract capture only. Retail PC already calls
    // 00797D50 every post-FX frame; when the stock selector is non-zero its final
    // fullscreen DrawPrimitive is the call at 0079849C -> return 007984A2. G1 never
    // writes the selector and never issues an extra GodRay draw.
    constexpr uintptr_t kGodRayFinalDrawPrimitiveReturn = 0x007984A2u;
    constexpr uintptr_t kGodRayFinalPixelShader = 0x010FBE94u;
    constexpr uintptr_t kGodRayStrengthC0 = 0x00EE5E80u;
    constexpr uintptr_t kGodRayOptionalTextureWrapper = 0x010CFF44u;
    volatile LONG s_godRayG1FinalSeenCount = 0;
    volatile LONG s_godRayG1ContractCaptureCount = 0;
    volatile LONG s_godRayG1ContractFailCount = 0;
    volatile LONG s_godRayG1SelectorTransitionCount = 0;
    uint32_t s_godRayG1LastSelectorRaw = 0u;
    bool s_godRayG1SelectorInitialized = false;
    uint32_t s_godRayG1LastTransitionPresent = 0u;
    uint32_t s_godRayG1LastCapturePresent = 0u;
    uint32_t s_godRayG1LastVsHash = 0u;
    uint32_t s_godRayG1LastPsHash = 0u;

    // V10.5.67 GodRay G2: reproduce the validated G1 bind/state contract without
    // issuing an internal G2 draw. Exact shader hashes/byte sizes come from the
    // repeated .66/.66.1 stock final-draw captures.
    constexpr uint32_t kGodRayG2ExpectedVsHash = 0xD4B3813Du;
    constexpr uint32_t kGodRayG2ExpectedPsHash = 0xDA246012u;
    constexpr uint32_t kGodRayG2ExpectedVsBytes = 208u;
    constexpr uint32_t kGodRayG2ExpectedPsBytes = 996u;
    volatile LONG s_godRayG2BindAttemptCount = 0;
    volatile LONG s_godRayG2BindPassCount = 0;
    volatile LONG s_godRayG2BindFailCount = 0;
    volatile LONG s_godRayG2DrawCount = 0; // G2 itself remains draw-free.

    // V10.5.68 GodRay G3: first real xeSM3 GodRay draw. G3 executes exactly one
    // original/trampoline DrawPrimitive per validated stock GodRay boundary, only
    // after the full G2 contract is read-back verified. Any post-draw integrity or
    // restoration failure disables subsequent G3 draws fail-closed.
    volatile LONG s_godRayG3AttemptCount = 0;
    volatile LONG s_godRayG3DrawCount = 0;
    volatile LONG s_godRayG3PassCount = 0;
    volatile LONG s_godRayG3FailCount = 0;
    volatile LONG s_godRayG3Disabled = 0;

    // V10.5.69 GodRay G4: ownership handoff. Only after G3 has drawn successfully,
    // held the exact contract, and restored the pre-probe state does the outer slot-81
    // hook suppress the duplicate stock DrawPrimitive by returning S_OK. Any G3/preflight
    // failure falls through to the original stock draw automatically.
    volatile LONG s_godRayG4BoundaryAttemptCount = 0;
    volatile LONG s_godRayG4StockSuppressCount = 0;
    volatile LONG s_godRayG4StockFallbackCount = 0;

    volatile LONG s_debugXboxD2ProbeCount = 0;
    volatile LONG s_debugXboxBloomReadbackAttemptCount = 0;
    volatile LONG s_debugXboxBloomReadbackPassCount = 0;
    volatile LONG s_debugXboxBloomReadbackFailCount = 0;
    // V10.5.66 Debug Xbox D2 + GodRay G1: apply the proven 28..96 -> 0..1.5 controller only
    // on same-present world frames. Fail closed to the locked .64.1 path.
    volatile LONG s_debugXboxD2BloomApplyAttemptCount = 0;
    volatile LONG s_debugXboxD2BloomApplyPassCount = 0;
    volatile LONG s_debugXboxD2BloomApplyFailCount = 0;
    volatile LONG s_debugXboxD2BloomApplySkipCount = 0;
    uint32_t s_debugXboxD2LastApplyPresent = 0u;
    float s_debugXboxD2LastAppliedBloom = 0.0f;
    // V10.5.23 keeps the dormant DOF final shader OFF. V10.5.18.1 proved the
    // native R32F producer works and that the previous hot per-draw collector was
    // the source of gameplay choppiness. This build removes that detour entirely.
    // At MID it walks the already-built scene+0x310 NGL linked render list once
    // and profiles each unique vtable -> virtual Execute() pair.
    // V10.5.23 keeps the same 7-wrapper replay but pins RT0 at the proven
    // sm_depth_shadow pixel-shader bind. It also queries physical RT0 with
    // IDirect3DDevice9::GetRenderTarget so the target probe is no longer cache-derived.
    uint32_t s_mode4ReplacementFormat = kFormatD24S8;
    bool s_mode4BlendDiagnostic = false;
    // 0=R, 1=G, 2=B, 3=A. Explicit env override locks one channel.
    uint32_t s_mode4PackChannel = 1u;
    bool s_mode4PackAutoCycle = true;
    uint32_t s_mode4LastPackChannel = 0xFFFFFFFFu;
    uint32_t s_mode4LastLoggedPackChannel = 0xFFFFFFFFu;
    constexpr uint32_t kMode4PackAutoBlockFrames = 300u;
    bool s_mode4PackedDepthEnabled = false; // Channel guessing remains retired.
    bool s_mode4MidProbeOnly = true;         // DOF/live-depth replacement still disabled in V10.5.18.
    volatile LONG s_mode4MidInstallCount = 0;
    volatile LONG s_mode4MidCallbackCount = 0;
    volatile LONG s_mode4MidConflictCount = 0;
    volatile LONG s_mode4MidResolveAttemptCount = 0;
    volatile LONG s_mode4MidResolveSuccessCount = 0;
    volatile LONG s_mode4MidResolveFailCount = 0;
    HRESULT s_mode4MidResolveLastHr = E_PENDING;
    volatile LONG s_shadowDepthTargetProbeCount = 0;
    // V10.5.17 full-resolution native-style float depth target setup, retained by V10.5.18.
    volatile LONG s_fullResDepthTargetCreateState = 0; // 0=none, 1=ready, -1=failed
    volatile LONG s_fullResDepthTargetBindTestState = 0; // 0=not tried, 1=done
    void* s_fullResDepthTexture = nullptr;
    void* s_fullResDepthSurface = nullptr;
    void* s_fullResDepthStencil = nullptr;
    uint32_t s_fullResDepthWidth = 0;
    uint32_t s_fullResDepthHeight = 0;
    HRESULT s_fullResCreateTextureHr = E_PENDING;
    HRESULT s_fullResGetSurfaceHr = E_PENDING;
    HRESULT s_fullResCreateDepthHr = E_PENDING;
    HRESULT s_fullResContainerHr = E_PENDING;
    uint32_t s_fullResContainerTex = 0;
    HRESULT s_fullResBindRTHr = E_PENDING;
    HRESULT s_fullResBindDSHr = E_PENDING;
    HRESULT s_fullResRestoreDSHr = E_PENDING;
    HRESULT s_fullResRestoreRTHr = E_PENDING;

    // V10.5.18: first active main-camera native depth-prepass proof.
    constexpr LONG kMode4DepthCommandCapacity = 8192;
    void* s_mode4DepthCommands[kMode4DepthCommandCapacity] = {};
    volatile LONG s_mode4DepthCommandCount = 0;
    volatile LONG s_mode4DepthCommandOverflow = 0;
    volatile LONG s_mode4DepthPrepassRunState = 0; // V10.5.28: 0=collecting in proven same-frame MID-installed window, 1=complete, -1=failed
    volatile LONG s_mode4DepthReadbackPending = 0;
    void* s_mode4DepthReadbackSurface = nullptr;
    HRESULT s_mode4PrepassStateBlockHr = E_PENDING;
    HRESULT s_mode4PrepassCaptureHr = E_PENDING;
    HRESULT s_mode4PrepassBindRTHr = E_PENDING;
    HRESULT s_mode4PrepassBindDSHr = E_PENDING;
    HRESULT s_mode4PrepassClearHr = E_PENDING;
    HRESULT s_mode4PrepassRestoreDSHr = E_PENDING;
    HRESULT s_mode4PrepassRestoreRTHr = E_PENDING;
    HRESULT s_mode4PrepassApplyHr = E_PENDING;
    uint32_t s_mode4PrepassSelectedCommand = 0u;
    uint32_t s_mode4PrepassSelectedMaterialType = 0u;
    uint32_t s_mode4PrepassReplayCount = 0u;
    uint32_t s_mode4RenderListScanned = 0u;
    uint32_t s_mode4RenderListPhatNodes = 0u;
    uint32_t s_mode4RenderListEligible = 0u;
    HRESULT s_mode4DepthReadbackCreateHr = E_PENDING;
    HRESULT s_mode4DepthReadbackHr = E_PENDING;
    HRESULT s_mode4DepthReadbackLockHr = E_PENDING;
    float s_mode4DepthReadbackMin = 0.0f;
    float s_mode4DepthReadbackMax = 0.0f;
    float s_mode4DepthReadbackMean = 0.0f;
    uint32_t s_mode4DepthReadbackFiniteCount = 0u;
    uint32_t s_mode4DepthReadbackChangedCount = 0u;

    // V10.5.23: during the one-shot native shadow-graph replay only, pin RT0
    // at the proven sm_depth_shadow pixel-shader bind. V10.5.21 and V10.5.22
    // both recorded zero replay-time RT transitions at their lower pin layers.
    // Outside this tiny window, raw/NGL RT routing remains stock.
    volatile LONG s_mode4ShadowGraphRTPinActive = 0;
    // V10.5.26 one-frame profile of all non-shadow 0x004F9340 dispatch ancestry.
    volatile LONG s_mode4PhatCallerProfileCount = 0;
    volatile LONG s_mode4PhatCallerProfileLogged = 0;
    volatile LONG s_mode4PhatCallerProfileStackFailures = 0;
    uint32_t s_mode4PhatCallerProfileScene = 0u;
    uint32_t s_mode4PhatCallerProfileArmPresent = 0u;

    // V10.5.28: one-shot retail nglRenderScene tree/list profiler.  IDA + original
    // NGL headers now prove the retail PC scene list layout.  This profiler does
    // not execute nodes, change RT/DS state, or activate DOF; it only reads the
    // already-built scene lists at nglRenderScene entry.
    volatile LONG s_sceneTreeProfileState = 0; // 0=waiting for world scene, 1=capturing subtree, 2=complete
    volatile LONG s_sceneTreeProfileSceneCount = 0;
    volatile LONG s_sceneTreeProfileNodeClassEvents = 0;
    volatile LONG s_sceneTreeProfileOpaqueNodes = 0;
    volatile LONG s_sceneTreeProfileTransNodes = 0;
    volatile LONG s_sceneTreeProfileReadFailures = 0;
    __declspec(thread) LONG s_nglRenderSceneHookDepth = 0;
    __declspec(thread) LONG s_sceneTreeProfileRootDepth = -1;
    __declspec(thread) uint32_t s_sceneTreeProfileSceneStack[64] = {};
    constexpr uint32_t kSceneTreeProfileMaxScenes = 192u;
    constexpr uint32_t kSceneTreeProfileMaxClassEvents = 2048u;
    constexpr uint32_t kSceneTreeProfileMaxNodesPerList = 8192u;

    // V10.5.29: full-resolution offscreen INTZ target proof.  This is deliberately
    // allocation/bind/clear/restore only: no render-node replay and no live game
    // depth replacement.  It validates the exact D3D9 pair we need for the next
    // opaque-list depth replay while preserving the game's stock D24S8.
    volatile LONG s_offscreenIntzProbeState = 0; // 0=waiting, 1=done, -1=failed
    volatile LONG s_opaqueIntzReplayState = 0; // 0=waiting, 1=armed, 2=running, 3=done, -1=failed
    uint32_t s_opaqueIntzReplayScene = 0u;
    uint32_t s_opaqueIntzOriginalMid = 0u;
    uint32_t s_opaqueIntzOriginalMidData = 0u;
    uint32_t s_opaqueIntzReplayDeclared = 0u;
    uint32_t s_opaqueIntzReplayCount = 0u;
    uint32_t s_opaqueIntzReplayFirstNode = 0u;
    uint32_t s_opaqueIntzReplayLastNode = 0u;
    uint32_t s_opaqueIntzReplayBadNode = 0u;
    uint32_t s_opaqueIntzTraversalStartIndex = 0u;
    uint32_t s_opaqueIntzTraversalFinalIndex = 0u;
    uint32_t s_opaqueIntzTraversalFinalNode = 0u;
    uint32_t s_opaqueIntzTraversalInternalConsumed = 0u;
    uint32_t s_opaqueIntzTraversalSentinelReached = 0u;
    HRESULT s_opaqueIntzStateBlockHr = E_PENDING;
    HRESULT s_opaqueIntzCaptureHr = E_PENDING;
    HRESULT s_opaqueIntzBindRTHr = E_PENDING;
    HRESULT s_opaqueIntzBindDSHr = E_PENDING;
    HRESULT s_opaqueIntzClearHr = E_PENDING;
    HRESULT s_opaqueIntzRestoreDSHr = E_PENDING;
    HRESULT s_opaqueIntzRestoreRTHr = E_PENDING;
    HRESULT s_opaqueIntzApplyHr = E_PENDING;
    HRESULT s_opaqueIntzPackHr = E_PENDING;
    HRESULT s_opaqueIntzCreateReadbackHr = E_PENDING;
    HRESULT s_opaqueIntzGetDataHr = E_PENDING;
    HRESULT s_opaqueIntzLockHr = E_PENDING;
    HRESULT s_opaqueIntzUnlockHr = E_PENDING;
    uint32_t s_opaqueIntzFinite = 0u;
    uint32_t s_opaqueIntzChanged = 0u;
    float s_opaqueIntzMin = 1.0f;
    float s_opaqueIntzMax = 1.0f;
    float s_opaqueIntzMean = 1.0f;
    uint32_t s_opaqueIntzDofBelowNear = 0u;
    uint32_t s_opaqueIntzDofBand = 0u;
    uint32_t s_opaqueIntzDofAboveFar = 0u;
    uint32_t s_opaqueIntzDofBelow099 = 0u;
    uint32_t s_opaqueIntzDofBelow0995 = 0u;
    uint32_t s_opaqueIntzDofBelow0999 = 0u;

    // V10.5.40: continuous final-combine permanent path using only the proven producer
    // proven by V10.5.29-33. The INTZ snapshot is converted to the exact
    // A/R/G packed representation decoded by the dormant PC DOF shader.
    // Activation is restricted to the SAME present that produced the depth.
    volatile LONG s_provenIntzDofState = 0; // 0=waiting, 1=armed, 2=active, 3=restored, -1=failed
    uint32_t s_provenIntzDofPresent = 0u;
    uint32_t s_provenIntzDofColorDescriptor = 0u;
    uint32_t s_provenIntzDofDepthTexture = 0u;
    uint32_t s_provenIntzDofShader = 0u;
    uint32_t s_provenIntzDofPrevStage1 = 0u;
    uint32_t s_provenIntzDofPrevStage1Cache = 0u;
    uint32_t s_provenIntzDofPrevPS = 0u;
    uint32_t s_provenIntzDofPrevPSCache = 0u;
    HRESULT s_provenIntzDofBindHr = E_PENDING;
    HRESULT s_provenIntzDofConstHr = E_PENDING;
    HRESULT s_provenIntzDofPsHr = E_PENDING;
    HRESULT s_provenIntzDofSrcBlendHr = E_PENDING;
    HRESULT s_provenIntzDofDstBlendHr = E_PENDING;
    HRESULT s_provenIntzDofRestoreTexHr = E_PENDING;
    HRESULT s_provenIntzDofRestorePsHr = E_PENDING;
    HRESULT s_provenIntzDofRestoreSrcBlendHr = E_PENDING;
    HRESULT s_provenIntzDofRestoreDstBlendHr = E_PENDING;

    // V10.5.49.10.6 permanent hook. Refresh the RESZ producer every world
    // frame, but never consume it in DOF until packed depth is independently
    // verified non-clear. The old 180-frame hard stop remains removed.
    constexpr bool kMode4PermanentReszDofEnabled = true;
    constexpr bool kMode4ReleaseSceneProfiler = false;
    constexpr bool kMode4ReleaseQuietLogging = true;
    constexpr uint32_t kMode4ReleaseHeartbeatPresents = 120u; // V10.5.49.10.14 isolation cadence
    constexpr uint32_t kLegacyReplayVisualWindowFrames = 180u; // unreachable while RESZ primary path is enabled
    volatile LONG s_provenIntzDofWindowState = 0; // 0=waiting, 1=continuous-active, -1=failed
    uint32_t s_provenIntzDofWindowStartPresent = 0u;
    uint32_t s_provenIntzDofWindowEndPresent = 0u;
    uint32_t s_provenIntzDofWindowLastArmedPresent = 0xFFFFFFFFu;
    uint32_t s_provenIntzDofWindowFrameOrdinal = 0u;
    uint32_t s_provenIntzDofWindowProduced = 0u;
    uint32_t s_provenIntzDofWindowApplied = 0u;
    uint32_t s_provenIntzDofWindowFailures = 0u;

    // V10.5.40: RESZ is the permanent producer. At MID, after
    // the stock opaque pass populated SM3's live D24S8 depth, resolve it into the
    // proven full-res INTZ texture. No duplicate nglRenderNode replay is used.
    constexpr bool kMode4ReszProbeEnabled = true;
    constexpr bool kMode4ReszDofVisualWindowEnabled = kMode4PermanentReszDofEnabled;
    volatile LONG s_reszDepthResolveState = 0; // 0=waiting, 1=armed, 2=running, 3=done, -1=failed
    uint32_t s_reszDepthResolveScene = 0u;
    uint32_t s_reszOriginalMid = 0u;
    uint32_t s_reszOriginalMidData = 0u;
    uint32_t s_reszLiveDepthSurface = 0u;
    uint32_t s_reszLiveDepthFormat = 0u;
    uint32_t s_reszLiveDepthWidth = 0u;
    uint32_t s_reszLiveDepthHeight = 0u;
    uint32_t s_reszStage0Before = 0u;
    uint32_t s_reszStage0CacheBefore = 0u;
    uint32_t s_reszStage0CacheAfter = 0u;
    HRESULT s_reszStateBlockHr = E_PENDING;
    HRESULT s_reszCaptureHr = E_PENDING;
    HRESULT s_reszClearBindHr = E_PENDING;
    HRESULT s_reszClearHr = E_PENDING;
    HRESULT s_reszRestoreLiveDepthHr = E_PENDING;
    HRESULT s_reszBindIntzTextureHr = E_PENDING;
    HRESULT s_reszDummySetupHr = E_PENDING;
    HRESULT s_reszDummyDrawHr = E_PENDING;
    HRESULT s_reszTriggerHr = E_PENDING;
    HRESULT s_reszApplyHr = E_PENDING;
    bool s_reszReadbackOk = false;
    bool s_opaqueIntzDiagnosticReadbackThisFrame = false;

    // V10.5.49.10.6: fail-closed depth trust. The permanent DOF path is not
    // allowed to consume a RESZ/INTZ result until a CPU verification proves the
    // packed depth is non-clear. Once proven, re-verify periodically and after
    // every device/reset resource rebuild. This avoids treating HRESULT-only
    // RESZ success as proof that usable depth actually arrived.
    bool s_mode4ReszDepthValidated = false;
    uint32_t s_mode4ReszLastValidationPresent = 0u;
    constexpr uint32_t kMode4ReszDepthRevalidatePresents = 120u;

    void* s_offscreenIntzTexture = nullptr;
    void* s_offscreenIntzSurface = nullptr;
    uint32_t s_offscreenIntzWidth = 0u;
    uint32_t s_offscreenIntzHeight = 0u;
    HRESULT s_offscreenIntzCreateHr = E_PENDING;
    HRESULT s_offscreenIntzGetSurfaceHr = E_PENDING;
    HRESULT s_offscreenIntzDescHr = E_PENDING;
    HRESULT s_offscreenIntzContainerHr = E_PENDING;
    uint32_t s_offscreenIntzContainerTex = 0u;
    HRESULT s_offscreenIntzStateBlockHr = E_PENDING;
    HRESULT s_offscreenIntzCaptureHr = E_PENDING;
    HRESULT s_offscreenIntzGetRTHr = E_PENDING;
    HRESULT s_offscreenIntzGetDSHr = E_PENDING;
    HRESULT s_offscreenIntzBindRTHr = E_PENDING;
    HRESULT s_offscreenIntzBindDSHr = E_PENDING;
    HRESULT s_offscreenIntzClearHr = E_PENDING;
    HRESULT s_offscreenIntzRestoreDSHr = E_PENDING;
    HRESULT s_offscreenIntzRestoreRTHr = E_PENDING;
    HRESULT s_offscreenIntzApplyHr = E_PENDING;
    uint32_t s_offscreenIntzActualRT = 0u;
    uint32_t s_offscreenIntzActualDS = 0u;

    // V10.5.30: end-to-end sampleability proof. Clear the offscreen INTZ to a
    // known Z, sample it through a tiny ps_2_0 diagnostic shader into A8R8G8B8,
    // read one packed center pixel back to the CPU, and reconstruct depth.
    volatile LONG s_intzSampleReadbackState = 0; // 0=waiting, 1=done, -1=failed
    HRESULT s_intzSampleClearHr = E_PENDING;
    HRESULT s_intzSamplePackHr = E_PENDING;
    HRESULT s_intzSampleCreateReadbackHr = E_PENDING;
    HRESULT s_intzSampleGetDataHr = E_PENDING;
    HRESULT s_intzSampleLockHr = E_PENDING;
    HRESULT s_intzSampleUnlockHr = E_PENDING;
    uint32_t s_intzSamplePackedBGRA = 0u;
    float s_intzSampleExpected = 0.25f;
    float s_intzSampleReconstructed = 0.0f;
    RtlCaptureStackBackTrace_t s_captureStackBackTrace = nullptr;
    volatile LONG s_mode4ShadowGraphRTPinAttempts = 0;
    volatile LONG s_mode4ShadowGraphRTPinRedirects = 0;
    volatile uint32_t s_mode4ShadowGraphWorldRT = 0u;          // captured D3D world surface
    volatile uint32_t s_mode4ShadowGraphWorldRTWrapper = 0u;   // scene+0x2FC NGL wrapper
    volatile uint32_t s_mode4ShadowGraphReplacementRT = 0u;    // xeSM3 R32F D3D surface
    volatile uint32_t s_mode4ShadowGraphLastRequestedRT = 0u;
    volatile uint32_t s_mode4ShadowGraphLastEffectiveRT = 0u;
    volatile uint32_t s_mode4ShadowGraphLastReturnAddress = 0u;
    volatile HRESULT s_mode4ShadowGraphLastRTHr = E_PENDING;
    volatile uint32_t s_mode4ShadowGraphLastRedirectReturnAddress = 0u;
    volatile HRESULT s_mode4ShadowGraphLastRedirectHr = E_PENDING;
    volatile int32_t s_mode4ShadowGraphLastNglResult = 0;
    volatile uint32_t s_mode4ShadowGraphWorldScene = 0u;
    volatile uint32_t s_mode4ShadowGraphLastActualRTBefore = 0u;
    volatile uint32_t s_mode4ShadowGraphLastActualRTAfter = 0u;
    volatile uint32_t s_mode4ShadowGraphLastShadowPS = 0u;
    uint32_t s_v10BloomSampleCount = 0;
    uint32_t s_v10BloomScaleApplyCount = 0;
    uint32_t s_v10BloomScaleFailCount = 0;
    uint32_t s_v10FinalSubstituteCount = 0;
    bool s_v10BloomSampleValid = false;
    float s_v10WeightedRms = 0.0f;
    float s_v10Brightness255 = 0.0f;
    float s_v10BloomT = 0.0f;
    float s_v10BloomStrength = 1.0f;
    void* s_v10BloomStateBlock = nullptr;
    uint32_t s_v10BloomStateBlockDeviceIdentity = 0;
    HRESULT s_v10LastScaleSetupHr = E_PENDING;
    HRESULT s_v10LastScaleDrawHr = E_PENDING;
    HRESULT s_v10LastScaleRestoreHr = E_PENDING;

    // V10.5.49 full native-style DOF chain diagnostics.  Pass 1 is the
    // dormant BloomFinalCombineDOF draw; these counters cover the follow-up
    // scene->quarter, Gaussian A->B->A, and final DstAlpha composite.
    volatile LONG s_mode4FullDofAttemptCount = 0;
    volatile LONG s_mode4FullDofSuccessCount = 0;
    volatile LONG s_mode4FullDofFailureCount = 0;
    volatile LONG s_mode4FullDofFinalDrawCount = 0;

    // V10.5.49.10.14: full Camera Motion ZBlur drawing remains hard-disabled.
    // .10.9 through .10.13 cleared the Scene copy, ZBlur VS/PS+c0, stage-0 texture,
    // sampler-0 state, and the eleven render states. This build advances exactly one
    // architectural category: the old full-path output-target transition. It captures
    // the live RT0 / depth-stencil / viewport, binds Scene as RT0, detaches depth, sets
    // the full-size Scene viewport, reads everything back, restores the exact prior
    // output state, and verifies restoration. No DrawPrimitive or fullscreen helper runs.
    bool s_mode4CameraMotionZBlurEnabled = false;
    bool s_mode4CameraMotionZBlurCopyOnlyEnabled = false;
    bool s_mode4CameraMotionZBlurBindOnlyEnabled = false;
    bool s_mode4CameraMotionZBlurTextureBindOnlyEnabled = false;
    bool s_mode4CameraMotionZBlurSamplerStateOnlyEnabled = false;
    bool s_mode4CameraMotionZBlurRenderStateOnlyEnabled = false;
    bool s_mode4CameraMotionZBlurOutputTargetOnlyEnabled = true;
    volatile LONG s_cameraMotionZBlurCopyOnlyAttemptCount = 0;
    volatile LONG s_cameraMotionZBlurCopyOnlySuccessCount = 0;
    volatile LONG s_cameraMotionZBlurCopyOnlyFailureCount = 0;
    volatile LONG s_cameraMotionZBlurBindOnlyAttemptCount = 0;
    volatile LONG s_cameraMotionZBlurBindOnlySuccessCount = 0;
    volatile LONG s_cameraMotionZBlurBindOnlyFailureCount = 0;
    volatile LONG s_cameraMotionZBlurTextureBindOnlyAttemptCount = 0;
    volatile LONG s_cameraMotionZBlurTextureBindOnlySuccessCount = 0;
    volatile LONG s_cameraMotionZBlurTextureBindOnlyFailureCount = 0;
    volatile LONG s_cameraMotionZBlurSamplerStateOnlyAttemptCount = 0;
    volatile LONG s_cameraMotionZBlurSamplerStateOnlySuccessCount = 0;
    volatile LONG s_cameraMotionZBlurSamplerStateOnlyFailureCount = 0;
    volatile LONG s_cameraMotionZBlurRenderStateOnlyAttemptCount = 0;
    volatile LONG s_cameraMotionZBlurRenderStateOnlySuccessCount = 0;
    volatile LONG s_cameraMotionZBlurRenderStateOnlyFailureCount = 0;
    volatile LONG s_cameraMotionZBlurOutputTargetOnlyAttemptCount = 0;
    volatile LONG s_cameraMotionZBlurOutputTargetOnlySuccessCount = 0;
    volatile LONG s_cameraMotionZBlurOutputTargetOnlyFailureCount = 0;
    HRESULT s_cameraMotionZBlurBindSetVsHr = E_PENDING;
    HRESULT s_cameraMotionZBlurBindSetPsHr = E_PENDING;
    HRESULT s_cameraMotionZBlurBindSetC0Hr = E_PENDING;
    HRESULT s_cameraMotionZBlurBindRestoreVsHr = E_PENDING;
    HRESULT s_cameraMotionZBlurBindRestorePsHr = E_PENDING;
    HRESULT s_cameraMotionZBlurBindRestoreC0Hr = E_PENDING;
    HRESULT s_cameraMotionZBlurTextureSetHr = E_PENDING;
    HRESULT s_cameraMotionZBlurTextureRestoreHr = E_PENDING;
    HRESULT s_cameraMotionZBlurSamplerSetHr = E_PENDING;
    HRESULT s_cameraMotionZBlurSamplerRestoreHr = E_PENDING;
    HRESULT s_cameraMotionZBlurRenderSetHr = E_PENDING;
    HRESULT s_cameraMotionZBlurRenderRestoreHr = E_PENDING;
    HRESULT s_cameraMotionZBlurOutputSetCopyRtHr = E_PENDING;
    HRESULT s_cameraMotionZBlurOutputSetCopyDsHr = E_PENDING;
    HRESULT s_cameraMotionZBlurOutputSetCopyViewportHr = E_PENDING;
    HRESULT s_cameraMotionZBlurOutputSetRtHr = E_PENDING;
    HRESULT s_cameraMotionZBlurOutputSetDsHr = E_PENDING;
    HRESULT s_cameraMotionZBlurOutputSetViewportHr = E_PENDING;
    HRESULT s_cameraMotionZBlurOutputRestoreRtHr = E_PENDING;
    HRESULT s_cameraMotionZBlurOutputRestoreDsHr = E_PENDING;
    HRESULT s_cameraMotionZBlurOutputRestoreViewportHr = E_PENDING;
    void* s_cameraMotionZBlurCopyTexture = nullptr;
    void* s_cameraMotionZBlurCopySurface = nullptr;
    uint32_t s_cameraMotionZBlurWidth = 0u;
    uint32_t s_cameraMotionZBlurHeight = 0u;
    uint32_t s_cameraMotionZBlurFormat = 0u;
    volatile LONG s_cameraMotionZBlurCreateState = 0;
    volatile LONG s_cameraMotionZBlurAttemptCount = 0;
    volatile LONG s_cameraMotionZBlurSuccessCount = 0;
    volatile LONG s_cameraMotionZBlurFailureCount = 0;
    HRESULT s_cameraMotionZBlurCreateHr = E_PENDING;
    HRESULT s_cameraMotionZBlurCaptureHr = E_PENDING;
    HRESULT s_cameraMotionZBlurCopySetupHr = E_PENDING;
    HRESULT s_cameraMotionZBlurCopyDrawHr = E_PENDING;
    HRESULT s_cameraMotionZBlurSetupHr = E_PENDING;
    HRESULT s_cameraMotionZBlurDrawHr = E_PENDING;
    HRESULT s_cameraMotionZBlurRestoreHr = E_PENDING;
    float s_cameraMotionZBlurLastC0[4] =
        { 0.5f, 0.5f, kCameraMotionZBlurFixedMinScale, kCameraMotionZBlurFixedMinScale };
    bool s_cameraMotionZBlurControllerValid = false;
    bool s_cameraMotionZBlurHistoryValid = false;
    uint32_t s_cameraMotionZBlurCameraValue = 0u;
    uint32_t s_cameraMotionZBlurTransformValue = 0u;
    uint32_t s_cameraMotionZBlurHistoryIndex = 0u;
    float s_cameraMotionZBlurPrevPosition[3] = { 0.0f, 0.0f, 0.0f };
    float s_cameraMotionZBlurMotionHistory[kXboxCameraZBlurHistoryCount] =
        { 0.0f, 0.0f, 0.0f, 0.0f };
    float s_cameraMotionZBlurLastDelta = 0.0f;
    float s_cameraMotionZBlurLastAverage = 0.0f;
    float s_cameraMotionZBlurLastMotion = 0.0f;
    float s_cameraMotionZBlurLastBlurPixels = kXboxCameraZBlurMin;
    bool s_cameraMotionZBlurGateReadable = false;
    bool s_cameraMotionZBlurGateLast = false;
    volatile LONG s_cameraMotionZBlurGateCalls = 0;
    volatile LONG s_cameraMotionZBlurGateBlocked = 0;
    volatile LONG s_cameraMotionZBlurProbeCount = 0;
    volatile LONG s_deviceResetCount = 0;
    volatile LONG s_deviceResetInFlight = 0;
    // 1 during normal rendering; cleared before Reset and restored only when the
    // game's original D3D9 Reset succeeds. Mode 5 fails closed while this is 0.
    volatile LONG s_postFxDeviceReady = 1;
    HRESULT s_deviceResetLastHr = E_PENDING;
    uint32_t s_deviceResetLastWidth = 0u;
    uint32_t s_deviceResetLastHeight = 0u;
    uint32_t s_deviceResetLastFormat = 0u;
    HRESULT s_mode4FullDofCaptureHr = E_PENDING;
    HRESULT s_mode4FullDofDownsampleHr = E_PENDING;
    HRESULT s_mode4FullDofBlur1Hr = E_PENDING;
    HRESULT s_mode4FullDofBlur2Hr = E_PENDING;
    HRESULT s_mode4FullDofRestoreHr = E_PENDING;
    HRESULT s_mode4FullDofFinalHr = E_PENDING;

    // V10.5.49.10.1: recovered native PC c0 for the Xbox-order bloom tail.
    // x32 runtime proof with the debug menu removed showed the retail PC path
    // actively binds 010FBF04 / 010FBF38 and uses two distinct full-resolution
    // offset vectors:
    //   BlurLevel1: {-6/W, -6/H, +6/W, -6/H}
    //   BlurLevel2: {-10/W, -10/H, +10/W, -10/H}
    // At 800x600 those are {-0.0075,-0.01,+0.0075,-0.01} and
    // {-0.0125,-0.0166667,+0.0125,-0.0166667}.  The .49.10 shared-Gaussian-c0
    // approximation is removed; the recovered tail order itself is unchanged.
    volatile LONG s_nativeBloomTailSuccessCount = 0;
    volatile LONG s_nativeBloomTailFailureCount = 0;
    HRESULT s_nativeBloomTailLevel1PsHr = E_PENDING;
    HRESULT s_nativeBloomTailLevel1C0Hr = E_PENDING;
    HRESULT s_nativeBloomTailLevel1RestorePsHr = E_PENDING;
    HRESULT s_nativeBloomTailLevel1RestoreC0Hr = E_PENDING;
    HRESULT s_nativeBloomTailLevel2CaptureHr = E_PENDING;
    HRESULT s_nativeBloomTailLevel2SetupHr = E_PENDING;
    HRESULT s_nativeBloomTailLevel2DrawHr = E_PENDING;
    HRESULT s_nativeBloomTailLevel2RestoreHr = E_PENDING;
    HRESULT s_nativeBloomTailFallbackGaussianHr = E_PENDING;
    float s_nativeBloomTailLevel1C0[4] = {};
    float s_nativeBloomTailLevel2C0[4] = {};

    // V10.2 is instrumentation-only over the frozen V10.1 visual path.
    // It records raw/unclamped FP16 distribution data so we can calibrate the
    // PC E8FB04 -> Xbox 0..255 brightness bridge without changing the image.
    uint32_t s_v10_2CalibrationSampleCount = 0;

    // V10.3 original-Xbox exposure state reconstruction. Mode 3 consumes it
    // only through the proven SpeedTree exposure callsite; Mode 4 does not use
    // this value for the fullscreen final-combine restoration.
    bool s_v10_3XboxExposureConfigQueued = false;
    uint32_t s_v10_3XboxExposureUpdateCount = 0;
    uint32_t s_v10_3LastLuminanceSamplePresent = 0;
    float s_v10_3XboxExposureState = kXboxExposureInitial;
    float s_v10_3LastSceneMeanSq = 0.0f;
    float s_v10_3LastTargetMeanSq = 0.0f;
    float s_v10_3LastDelta = 0.0f;
    float s_v10_3LastGameSeconds = 0.0f;
    float s_v10_3LastDayFraction = 0.0f;
    float s_v10_3LastCosPhase = 0.0f;
    bool s_v10_3LastTimeValid = false;

    // V10.4.1 native PC exposure parameter injection state. The generic
    // 008D3140 scalar setter remains stock; only CALL 0081F12B is redirected.
    // If the exact retail bytes do not match, the patch fails closed.
    volatile LONG s_v10_4NativeExposureMatchedCount = 0;
    volatile LONG s_v10_4NativeExposureAppliedCount = 0;
    volatile LONG s_v10_4NativeExposureBypassCount = 0;
    volatile LONG s_v10_4CallsitePatchState = 0; // 0=not tried, 1=installed, -1=failed
    uint8_t s_v10_4OriginalCallBytes[5] = {};

    // V10.5.49.10.6 exact late-frame CALL patch state. This is Mode-4 only.
    // Mode 0-3 retain the historical function-entry detour so their old
    // research behavior is not silently changed.
    volatile LONG s_bloomCompositeLateCallsitePatchState = 0; // 0=not tried, 2=installing, 1=installed, -1=failed
    uint8_t s_bloomCompositeLateCallsiteOriginalBytes[5] = {};
    volatile LONG s_bloomCompositeLateCallsiteHits = 0;
    volatile uint32_t s_bloomCompositeLateCallsiteLastReturn = 0u;
    uint32_t s_v10_4LastExposureReturnAddress = 0;
    uint32_t s_v10_4LastExposureParameter = 0;
    float s_v10_4LastOriginalExposure = 0.0f;
    float s_v10_4LastInjectedExposure = 0.0f;
    float s_v10_4LastXboxExposureState = 0.0f;
    bool s_v10_4LastExposureApplied = false;

    // V10.5 Mode 4: restore the retail Xbox depth-aware final combine using
    // the PC-shipped dormant DOF shader and existing packed post-FX depth.
    volatile LONG s_mode4AttemptCount = 0;
    volatile LONG s_mode4ApplyCount = 0;
    volatile LONG s_mode4FallbackCount = 0;
    uint32_t s_mode4LastReadyMask = 0;
    uint32_t s_mode4LastColorDescriptor = 0;
    uint32_t s_mode4LastColorTexture = 0;
    uint32_t s_mode4LastDepthTexture = 0;
    uint32_t s_mode4LastDofShader = 0;
    uint32_t s_mode4LastSimpleShader = 0;
    uint32_t s_mode4LastActivePSBefore = 0;
    uint32_t s_mode4LastActivePSAfter = 0;
    uint32_t s_mode4LastDepthDescriptorFlags = 0;
    uint32_t s_mode4LastDepthCacheBefore = 0;
    uint32_t s_mode4LastDepthCacheAfter = 0;
    uint32_t s_mode4LastRawStage1After = 0;
    uint32_t s_mode4LastLoggedReadyMask = 0xFFFFFFFFu;
    bool s_mode4LastLoggedApplied = false;
    HRESULT s_mode4LastDepthBindHr = E_PENDING;
    HRESULT s_mode4LastConstantHr = E_PENDING;
    HRESULT s_mode4LastPixelShaderHr = E_PENDING;
    bool s_mode4LastApplied = false;

    // V10.5.48 live PC near/far -> retail Xbox bloomDepthControl bridge.
    // Only near/far are dynamic in this build; 300/500/1.0/0.8 remain the
    // observed Xbox defaults until their exact PC-side producers are mapped.
    float s_mode4LastPcNearPlane = 0.0f;
    float s_mode4LastPcFarPlane = 0.0f;
    float s_mode4LastBloomDepthControl[4] =
    {
        kMode4BloomDepthControlFallback[0],
        kMode4BloomDepthControlFallback[1],
        kMode4BloomDepthControlFallback[2],
        kMode4BloomDepthControlFallback[3]
    };
    bool s_mode4LastDynamicC0Valid = false;
    volatile LONG s_mode4C0BridgeBuildCount = 0;
    volatile LONG s_mode4C0BridgeFallbackCount = 0;
    volatile LONG s_mode4C0BridgeLoggedSuccess = 0;

    // V10.5.4 depth-lifetime diagnostics. Runtime V10.5.2 proved E8FB78 binds
    // null even under the stock God Rays path, so do not mutate any texture,
    // shader, sampler, or constant state until a real sampleable depth source
    // is identified. Probe the active D3D9 depth-stencil surface read-only.
    volatile LONG s_mode4DepthProbeCount = 0;
    volatile LONG s_godRaysDepthBindProbeCount = 0;
    uint32_t s_mode4DepthDescriptorWords[8] = {};
    uint32_t s_mode4LastDepthSurface = 0;
    uint32_t s_mode4LastDepthSurfaceTexture = 0;
    HRESULT s_mode4LastGetDepthSurfaceHr = E_PENDING;
    HRESULT s_mode4LastDepthSurfaceDescHr = E_PENDING;
    HRESULT s_mode4LastDepthSurfaceContainerHr = E_PENDING;
    D3DSurfaceDescLite s_mode4LastDepthSurfaceDesc = {};
    uint32_t s_mode4LastGodRaysCacheBefore = 0;
    uint32_t s_mode4LastGodRaysCacheAfter = 0;
    uint32_t s_mode4LastGodRaysRawStage1 = 0;

    // V10.5.4: capture depth-stencil lifetime at the D3D9 API boundary.
    // The final-composite probe proved GetDepthStencilSurface returns
    // D3DERR_NOTFOUND there, meaning SM3 has already unbound depth by 00797CA0.
    // These fields remember the last successful non-null SetDepthStencilSurface
    // call and inspect it synchronously while the game still owns the surface.
    volatile LONG s_rawDepthCreateCount = 0;
    volatile LONG s_rawDepthBindCount = 0;
    uint32_t s_mode4CapturedDepthSurface = 0;
    uint32_t s_mode4CapturedDepthTexture = 0;
    HRESULT s_mode4CapturedDepthContainerHr = E_PENDING;
    D3DSurfaceDescLite s_mode4CapturedDepthDesc = {};
    uint32_t s_mode4CapturedDepthSetReturn = 0;

// V10.5.5: telemetry-only capability + NGL wrapper correlation.
// Runtime V10.5.4 proved the main 1920x1080 D24S8 scene depth is a
// standalone surface (GetContainer(IDirect3DTexture9) -> E_NOINTERFACE).
// Probe texture-backed depth formats without binding or using them.
volatile LONG s_mode4DepthCapsProbeState = 0; // 0=not run, 1=done
HRESULT s_mode4DepthCapsD24S8 = E_PENDING;
HRESULT s_mode4DepthCapsINTZ = E_PENDING;
HRESULT s_mode4DepthCapsDF24 = E_PENDING;
HRESULT s_mode4DepthCapsRAWZ = E_PENDING;
HRESULT s_mode4DepthCapsDF16 = E_PENDING;

// Capture the game's own NGL D24S8 wrapper created by 008D1EB0 so we can
// correlate it against the live SetDepthStencilSurface identity.
uint32_t s_mode4NglDepthWrapper = 0;
uint32_t s_mode4NglDepthWrapperFlags = 0;
uint32_t s_mode4NglDepthWrapperWidth = 0;
uint32_t s_mode4NglDepthWrapperHeight = 0;
uint32_t s_mode4NglDepthWrapperWords[32] = {};
uint32_t s_mode4NglDepthSurfaceMatchOffset = 0xFFFFFFFFu;

    // V10.5.6: telemetry-only bridge probe. V10.5.5 proved that texture-backed
    // D24S8/INTZ/DF24/DF16 creation succeeds on the active runtime. This build
    // validates the texture->surface->container round trip and follows the NGL
    // depth wrapper's backend pointer graph looking for the live scene surface.
    // No test depth resource is ever bound to the game.
    volatile LONG s_mode4DepthBridgeProbeState = 0; // 0=waiting, 1=done
    uint32_t s_mode4MainSceneDepthSurface = 0;
    D3DSurfaceDescLite s_mode4MainSceneDepthDesc = {};
    uint32_t s_mode4DepthBackendRoot = 0;
    uint32_t s_mode4DepthBackendDirectSurfaceOffset = 0xFFFFFFFFu;
    uint32_t s_mode4DepthBackendChildSlotOffset = 0xFFFFFFFFu;
    uint32_t s_mode4DepthBackendChild = 0;
    uint32_t s_mode4DepthBackendChildSurfaceOffset = 0xFFFFFFFFu;
    uint32_t s_mode4DepthBackendWords[16] = {};
    DepthTextureRoundTripLite s_mode4RoundTripD24S8 = {};
    DepthTextureRoundTripLite s_mode4RoundTripINTZ = {};

    // V10.5.11: keep ONLY the proven full-size texture-backed D24S8 scene
    // depth replacement by default. INTZ caused visible geometry popping even
    // though all D3D calls succeeded, so it is no longer the normal test path.
    // The D24 owner texture is sampled as s1 by the shipped dormant DOF PS.
    volatile LONG s_mode4DepthReplacementCreateState = 0; // 0=none, 1=ready, -1=failed
    volatile LONG s_mode4DepthReplacementBindCount = 0;
    volatile LONG s_mode4DepthReplacementAppliedCount = 0;
    void* s_mode4ReplacementDepthTexture = nullptr;
    void* s_mode4ReplacementDepthSurface = nullptr;
    uint32_t s_mode4ReplacementWidth = 0;
    uint32_t s_mode4ReplacementHeight = 0;
    HRESULT s_mode4ReplacementCreateHr = E_PENDING;
    HRESULT s_mode4ReplacementSurfaceHr = E_PENDING;
    HRESULT s_mode4ReplacementContainerHr = E_PENDING;
    uint32_t s_mode4ReplacementContainerTex = 0;

    // V10.5.11 packed-depth conversion resources. The packed texture is an
    // A8R8G8B8 render target whose A/R/G bytes reconstruct exactly as:
    // depth = A*(255/256) + R*(255/65536) + G*(255/16777216).
    volatile LONG s_mode4PackCreateState = 0; // 0=none, 1=ready, -1=failed
    volatile LONG s_mode4PackApplyCount = 0;
    volatile LONG s_mode4PackFailCount = 0;
    void* s_mode4PackedDepthTexture = nullptr;
    void* s_mode4PackedDepthSurface = nullptr;
    void* s_mode4PackedDepthPixelShader = nullptr;
    void* s_mode4PackStateBlock = nullptr;
    uint32_t s_mode4PackedDepthWidth = 0;
    uint32_t s_mode4PackedDepthHeight = 0;
    uint32_t s_mode4PackStateBlockDeviceIdentity = 0;
    HRESULT s_mode4PackCompileHr = E_PENDING;
    HRESULT s_mode4PackCreateTextureHr = E_PENDING;
    HRESULT s_mode4PackGetSurfaceHr = E_PENDING;
    HRESULT s_mode4PackCreateShaderHr = E_PENDING;
    HRESULT s_mode4PackSetupHr = E_PENDING;
    HRESULT s_mode4PackDrawHr = E_PENDING;
    HRESULT s_mode4PackRestoreHr = E_PENDING;
    char s_mode4PackCompilerText[48] = {};

    __declspec(thread) uint32_t s_nglSetRTWrapperInFlight = 0;
    __declspec(thread) bool s_insideComposite = false;
    __declspec(thread) LONG s_currentCompositeCall = 0;
    __declspec(thread) bool s_thisCompositeBlur1Routed = false;
    __declspec(thread) bool s_thisCompositeV10Scaled = false;
    __declspec(thread) bool s_thisCompositeNativeTailLevel1 = false;
    __declspec(thread) bool s_thisCompositeNativeTailLevel2 = false;

    Event* ReserveEvent(EventType type);

    uint32_t ReadU32(uintptr_t address)
    {
        return *reinterpret_cast<volatile uint32_t*>(address);
    }

    uint8_t ReadU8(uintptr_t address)
    {
        return *reinterpret_cast<volatile uint8_t*>(address);
    }

    void WriteU32(uintptr_t address, uint32_t value)
    {
        *reinterpret_cast<volatile uint32_t*>(address) = value;
    }

    void WriteU8(uintptr_t address, uint8_t value)
    {
        *reinterpret_cast<volatile uint8_t*>(address) = value;
    }

    uint32_t CurrentScene()
    {
        return ReadU32(kCurrentSceneGlobal);
    }

    uint32_t CurrentSceneRT()
    {
        const uint32_t scene = CurrentScene();
        return scene ? *reinterpret_cast<volatile uint32_t*>(scene + 0x2FC) : 0;
    }

    const char* CurrentSceneName()
    {
        const uint32_t scene = CurrentScene();
        return scene ? *reinterpret_cast<const char**>(scene + 0x2E0) : nullptr;
    }

    void CopySmallText(char* dst, size_t capacity, const char* src)
    {
        if (!dst || capacity == 0)
            return;
        dst[0] = '\0';
        if (!src)
            return;
        strncpy_s(dst, capacity, src, _TRUNCATE);
    }

    bool IsReadableMemory(const void* pointer, size_t bytes)
    {
        if (!pointer || bytes == 0)
            return false;

        MEMORY_BASIC_INFORMATION info = {};
        if (VirtualQuery(pointer, &info, sizeof(info)) != sizeof(info))
            return false;
        if (info.State != MEM_COMMIT || (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0)
            return false;

        const uintptr_t begin = reinterpret_cast<uintptr_t>(pointer);
        const uintptr_t end = begin + bytes;
        const uintptr_t regionEnd = reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize;
        return end >= begin && end <= regionEnd;
    }

    bool IsExecutableMemory(const void* pointer)
    {
        if (!pointer)
            return false;

        MEMORY_BASIC_INFORMATION info = {};
        if (VirtualQuery(pointer, &info, sizeof(info)) != sizeof(info))
            return false;
        if (info.State != MEM_COMMIT || (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0)
            return false;

        const DWORD protect = info.Protect & 0xFFu;
        return protect == PAGE_EXECUTE || protect == PAGE_EXECUTE_READ ||
            protect == PAGE_EXECUTE_READWRITE || protect == PAGE_EXECUTE_WRITECOPY;
    }

    uint32_t RawTextureFromNglDescriptor(uint32_t descriptor)
    {
        // Mirrors the direct/non-streamed branch of NGL_BindTextureCached
        // (008C6BF0): when descriptor+0x1C != -1 and descriptor[0] is non-null,
        // the real IDirect3DBaseTexture9* is **(void***)descriptor.
        if (descriptor < 0x10000u || !IsReadableMemory(
            reinterpret_cast<void*>(static_cast<uintptr_t>(descriptor)), 0x20u))
            return 0u;

        if (ReadU32(static_cast<uintptr_t>(descriptor) + 0x1Cu) == 0xFFFFFFFFu)
            return 0u; // streamed/resource-resolution branch; use raw bind mirror instead.

        const uint32_t holder = ReadU32(descriptor);
        if (holder < 0x10000u || !IsReadableMemory(
            reinterpret_cast<void*>(static_cast<uintptr_t>(holder)), sizeof(uint32_t)))
            return 0u;

        const uint32_t texture = ReadU32(holder);
        if (texture < 0x10000u || !IsReadableMemory(
            reinterpret_cast<void*>(static_cast<uintptr_t>(texture)), sizeof(void*)))
            return 0u;
        return texture;
    }

    uint32_t RawTextureFromWrapperFallback(uint32_t wrapper)
    {
        // V8's original descriptor walk is retained only as a fallback. The V8
        // run proved that it is not sufficient for the 16x12 RT wrappers.
        if (wrapper < 0x10000u || !IsReadableMemory(reinterpret_cast<void*>(static_cast<uintptr_t>(wrapper + 0x14u)), sizeof(uint32_t)))
            return 0;

        const uint32_t holder = ReadU32(static_cast<uintptr_t>(wrapper) + 0x14u);
        if (holder < 0x10000u || !IsReadableMemory(reinterpret_cast<void*>(static_cast<uintptr_t>(holder)), sizeof(uint32_t)))
            return 0;
        return ReadU32(holder);
    }

    uint32_t RawSurfaceDirectFromWrapper(uint32_t wrapper)
    {
        // NGL_CreateRenderTargetTexture passes wrapper+0x34 directly as the
        // IDirect3DTexture9::GetSurfaceLevel(0) output slot. Therefore this is
        // the authoritative native surface pointer for this retail RT layout.
        if (wrapper < 0x10000u || !IsReadableMemory(reinterpret_cast<void*>(static_cast<uintptr_t>(wrapper + 0x34u)), sizeof(uint32_t)))
            return 0;
        const uint32_t surface = ReadU32(static_cast<uintptr_t>(wrapper) + 0x34u);
        return surface >= 0x10000u ? surface : 0;
    }

    uint32_t RawTextureFromSurface(uint32_t surfaceValue, HRESULT* outHr)
    {
        if (outHr)
            *outHr = E_POINTER;
        if (surfaceValue < 0x10000u)
            return 0;

        void* surface = reinterpret_cast<void*>(static_cast<uintptr_t>(surfaceValue));
        if (!IsReadableMemory(surface, sizeof(void*)))
            return 0;

        void** surfaceVtable = *reinterpret_cast<void***>(surface);
        if (!surfaceVtable || !IsReadableMemory(surfaceVtable,
            (kD3DSurfaceGetContainerVtableIndex + 1u) * sizeof(void*)))
            return 0;

        void* method = surfaceVtable[kD3DSurfaceGetContainerVtableIndex];
        if (!IsExecutableMemory(method))
            return 0;

        const auto getContainer = reinterpret_cast<D3DSurfaceGetContainer_t>(method);
        void* texture = nullptr;
        const HRESULT hr = getContainer(surface, kIID_IDirect3DTexture9, &texture);
        if (outHr)
            *outHr = hr;
        if (FAILED(hr) || !texture)
            return 0;

        const uint32_t textureValue = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(texture));

        // GetContainer AddRefs the returned interface. Keep only the identity
        // value and immediately balance that reference; NGL owns the texture.
        if (IsReadableMemory(texture, sizeof(void*)))
        {
            void** textureVtable = *reinterpret_cast<void***>(texture);
            if (textureVtable && IsReadableMemory(textureVtable,
                (kIUnknownReleaseVtableIndex + 1u) * sizeof(void*)))
            {
                void* releaseMethod = textureVtable[kIUnknownReleaseVtableIndex];
                if (IsExecutableMemory(releaseMethod))
                {
                    const auto release = reinterpret_cast<IUnknownRelease_t>(releaseMethod);
                    release(texture);
                }
            }
        }
        return textureValue;
    }

    uint32_t RawSurface0FromWrapperFallback(uint32_t wrapper)
    {
        const uint32_t textureValue = RawTextureFromWrapperFallback(wrapper);
        if (textureValue < 0x10000u || !IsReadableMemory(reinterpret_cast<void*>(static_cast<uintptr_t>(textureValue)), sizeof(void*)))
            return 0;

        void* texture = reinterpret_cast<void*>(static_cast<uintptr_t>(textureValue));
        void** textureVtable = *reinterpret_cast<void***>(texture);
        if (!textureVtable || !IsReadableMemory(textureVtable,
            (kD3DTextureGetSurfaceLevelVtableIndex + 1u) * sizeof(void*)))
            return 0;

        void* method = textureVtable[kD3DTextureGetSurfaceLevelVtableIndex];
        if (!IsExecutableMemory(method))
            return 0;

        const auto getSurfaceLevel = reinterpret_cast<D3DTextureGetSurfaceLevel_t>(method);
        void* surface = nullptr;
        const HRESULT hr = getSurfaceLevel(texture, 0u, &surface);
        if (FAILED(hr) || !surface)
            return 0;

        const uint32_t surfaceValue = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(surface));
        if (IsReadableMemory(surface, sizeof(void*)))
        {
            void** surfaceVtable = *reinterpret_cast<void***>(surface);
            if (surfaceVtable && IsReadableMemory(surfaceVtable,
                (kIUnknownReleaseVtableIndex + 1u) * sizeof(void*)))
            {
                void* releaseMethod = surfaceVtable[kIUnknownReleaseVtableIndex];
                if (IsExecutableMemory(releaseMethod))
                {
                    const auto release = reinterpret_cast<IUnknownRelease_t>(releaseMethod);
                    release(surface);
                }
            }
        }
        return surfaceValue;
    }

    void QueueRawIdentityStatus(uint32_t which, uint32_t source, uint32_t wrapper,
        uint32_t surface, uint32_t texture, HRESULT hr)
    {
        Event* event = ReserveEvent(EventType::RawIdentityStatus);
        if (!event)
            return;
        event->a = s_presentCount;
        event->b = which;
        event->c = source;
        event->d = wrapper;
        event->e = surface;
        event->f = texture;
        event->g = static_cast<uint32_t>(hr);
        event->h = s_rawCurrentPixelShader;
        event->i = s_rawCurrentRT0;
    }

    void StoreRawIdentity(uint32_t kind, uint32_t wrapper, uint32_t surface,
        uint32_t texture, HRESULT hr, uint32_t source)
    {
        if (!wrapper)
            return;

        uint32_t previousSurface = 0;
        uint32_t previousTexture = 0;
        switch (kind)
        {
        case 1: previousSurface = s_rawSurfaceQuarterA; previousTexture = s_rawTextureQuarterA; if (surface) s_rawSurfaceQuarterA = surface; if (texture) s_rawTextureQuarterA = texture; break;
        case 2: previousSurface = s_rawSurfaceQuarterB; previousTexture = s_rawTextureQuarterB; if (surface) s_rawSurfaceQuarterB = surface; if (texture) s_rawTextureQuarterB = texture; break;
        case 3: if (texture) s_rawTexture160 = texture; break;
        case 4:
            previousSurface = s_rawSurfaceLum16;
            previousTexture = s_rawTextureLum16;
            if (surface) s_rawSurfaceLum16 = surface;
            if (texture) s_rawTextureLum16 = texture;
            s_rawLumGetContainerHr = static_cast<uint32_t>(hr);
            s_rawLumIdentitySource = source;
            break;
        case 5:
            previousSurface = s_rawSurfaceSecond16;
            previousTexture = s_rawTextureSecond16;
            if (surface) s_rawSurfaceSecond16 = surface;
            if (texture) s_rawTextureSecond16 = texture;
            s_rawSecondGetContainerHr = static_cast<uint32_t>(hr);
            s_rawSecondIdentitySource = source;
            break;
        case 6: if (texture) s_rawTextureHalfA = texture; break;
        case 7: if (texture) s_rawTextureHalfB = texture; break;
        case 8: previousSurface = s_rawSurfaceScene; previousTexture = s_rawTextureScene; if (surface) s_rawSurfaceScene = surface; if (texture) s_rawTextureScene = texture; break;
        default: break;
        }

        if ((kind == 4u || kind == 5u) &&
            ((surface && surface != previousSurface) || (texture && texture != previousTexture)))
        {
            QueueRawIdentityStatus(kind == 4u ? 1u : 2u, source, wrapper, surface, texture, hr);
        }
    }

    void RefreshOneRawIdentity(uint32_t wrapper, uint32_t kind)
    {
        if (!wrapper)
            return;

        uint32_t surface = RawSurfaceDirectFromWrapper(wrapper);
        uint32_t source = 2u; // wrapper+0x34 -> surface -> GetContainer
        if (!surface)
        {
            surface = RawSurface0FromWrapperFallback(wrapper);
            source = 3u; // legacy V8 descriptor fallback
        }

        HRESULT hr = surface ? E_FAIL : E_POINTER;
        uint32_t texture = surface ? RawTextureFromSurface(surface, &hr) : 0u;
        if (!texture && kind != 4u && kind != 5u)
        {
            const uint32_t fallbackTexture = RawTextureFromWrapperFallback(wrapper);
            if (fallbackTexture)
            {
                texture = fallbackTexture;
                if (!surface)
                    source = 3u;
            }
        }
        StoreRawIdentity(kind, wrapper, surface, texture, hr, source);
    }

    void RefreshRawTextureIdentities()
    {
        // Resolve missing identities only. This avoids calling GetContainer every
        // present after a stable D3D identity has been learned.
        if (!s_rawTextureQuarterA || !s_rawSurfaceQuarterA) RefreshOneRawIdentity(ReadU32(kRtQuarterA), 1u);
        if (!s_rawTextureQuarterB || !s_rawSurfaceQuarterB) RefreshOneRawIdentity(ReadU32(kRtQuarterB), 2u);
        if (!s_rawTexture160) RefreshOneRawIdentity(ReadU32(kRt160x120), 3u);
        if (!s_rawTextureLum16 || !s_rawSurfaceLum16) RefreshOneRawIdentity(ReadU32(kRtLuminance16x12), 4u);
        if (!s_rawTextureSecond16 || !s_rawSurfaceSecond16) RefreshOneRawIdentity(ReadU32(kRtSecond16x12), 5u);
        if (!s_rawTextureHalfA) RefreshOneRawIdentity(ReadU32(kRtHalfA), 6u);
        if (!s_rawTextureHalfB) RefreshOneRawIdentity(ReadU32(kRtHalfB), 7u);
        if (!s_rawTextureScene || !s_rawSurfaceScene) RefreshOneRawIdentity(CurrentSceneRT(), 8u);
    }

    void RefreshRawSurfaceIdentities()
    {
        // V8.1's authoritative path is wrapper+0x34 -> IDirect3DSurface9 ->
        // GetContainer(IID_IDirect3DTexture9). Retain this function as a sparse
        // retry point for device resets or targets that initialize late.
        RefreshOneRawIdentity(ReadU32(kRtQuarterA), 1u);
        RefreshOneRawIdentity(ReadU32(kRtQuarterB), 2u);
        RefreshOneRawIdentity(ReadU32(kRtLuminance16x12), 4u);
        RefreshOneRawIdentity(ReadU32(kRtSecond16x12), 5u);
        RefreshOneRawIdentity(CurrentSceneRT(), 8u);
    }

    uint32_t ClassifyWrapper(uint32_t wrapper)
    {
        if (!wrapper) return 0;
        if (wrapper == ReadU32(kRtQuarterA)) return 1;
        if (wrapper == ReadU32(kRtQuarterB)) return 2;
        if (wrapper == ReadU32(kRt160x120)) return 3;
        if (wrapper == ReadU32(kRtLuminance16x12)) return 4;
        if (wrapper == ReadU32(kRtSecond16x12)) return 5;
        if (wrapper == ReadU32(kRtHalfA)) return 6;
        if (wrapper == ReadU32(kRtHalfB)) return 7;
        if (wrapper == CurrentSceneRT()) return 8;
        return 9;
    }

    const char* WrapperKindName(uint32_t kind)
    {
        switch (kind)
        {
        case 1: return "quarterA";
        case 2: return "quarterB";
        case 3: return "160x120";
        case 4: return "E8FB04";
        case 5: return "E8FBE8";
        case 6: return "halfA";
        case 7: return "halfB";
        case 8: return "sceneRT";
        case 9: return "other";
        default: return "null";
        }
    }

    uint32_t ClassifyRawTexture(uint32_t texture)
    {
        if (!texture) return 0;
        if (texture == s_rawTextureQuarterA) return 1;
        if (texture == s_rawTextureQuarterB) return 2;
        if (texture == s_rawTexture160) return 3;
        if (texture == s_rawTextureLum16) return 4;
        if (texture == s_rawTextureSecond16) return 5;
        if (texture == s_rawTextureHalfA) return 6;
        if (texture == s_rawTextureHalfB) return 7;
        if (texture == s_rawTextureScene) return 8;
        return 9;
    }

    uint32_t ClassifyRawSurface(uint32_t surface)
    {
        if (!surface) return 0;
        if (surface == s_rawSurfaceQuarterA) return 1;
        if (surface == s_rawSurfaceQuarterB) return 2;
        if (surface == s_rawSurfaceLum16) return 4;
        if (surface == s_rawSurfaceSecond16) return 5;
        if (surface == s_rawSurfaceScene) return 8;
        return 9;
    }

    void LearnRawSurface(uint32_t wrapper, uint32_t surface)
    {
        if (!wrapper || !surface)
            return;

        const uint32_t kind = ClassifyWrapper(wrapper);
        uint32_t cachedTexture = 0;
        uint32_t cachedSurface = 0;
        switch (kind)
        {
        case 1: cachedTexture = s_rawTextureQuarterA; cachedSurface = s_rawSurfaceQuarterA; break;
        case 2: cachedTexture = s_rawTextureQuarterB; cachedSurface = s_rawSurfaceQuarterB; break;
        case 4: cachedTexture = s_rawTextureLum16; cachedSurface = s_rawSurfaceLum16; break;
        case 5: cachedTexture = s_rawTextureSecond16; cachedSurface = s_rawSurfaceSecond16; break;
        case 8: cachedTexture = s_rawTextureScene; cachedSurface = s_rawSurfaceScene; break;
        default: break;
        }

        // GetContainer is only needed when the identity is missing or the D3D
        // surface changed (for example after a device reset), not every SetRT.
        if (surface == cachedSurface && cachedTexture)
            return;

        HRESULT hr = E_FAIL;
        const uint32_t texture = RawTextureFromSurface(surface, &hr);
        StoreRawIdentity(kind, wrapper, surface, texture, hr, 1u);
    }

    bool ShouldTraceRawPresent()
    {
        return s_presentCount <= 4u || (s_presentCount != 0u && (s_presentCount % 300u) == 0u);
    }

    bool IsPostFxScene()
    {
        const char* name = CurrentSceneName();
        return name && strcmp(name, "post::render_bloom") == 0;
    }

    bool ConsumeRawTraceBudget()
    {
        if (!ShouldTraceRawPresent())
            return false;

        if (s_insideComposite || IsPostFxScene())
        {
            const LONG count = InterlockedIncrement(&s_rawPostFxEventCountThisPresent);
            return count <= kRawPostFxBudgetPerSampledPresent;
        }

        const LONG count = InterlockedIncrement(&s_rawGenericEventCountThisPresent);
        return count <= kRawGenericBudgetPerSampledPresent;
    }

    bool ShouldLogSparse(LONG count)
    {
        return count <= 16 || (count % 300) == 0;
    }

    bool ShouldValidateDebugXboxIntegrationCheckpoint(LONG count)
    {
        if (!kDebugXboxIntegrationLockdownQuietLogging || s_postProcessRoute != 3u)
            return ShouldLogSparse(count);
        return count <= 2 ||
            (count % static_cast<LONG>(kDebugXboxIntegrationValidationInterval)) == 0;
    }

    bool ShouldLogDebugXboxIntegrationDetail(LONG count)
    {
        if (s_postProcessRoute == 3u && !kDebugXboxProductionSuccessDetailLogging)
            return false;
        return ShouldValidateDebugXboxIntegrationCheckpoint(count);
    }

    uint32_t FloatBits(float value)
    {
        uint32_t bits = 0;
        memcpy(&bits, &value, sizeof(bits));
        return bits;
    }

    float BitsFloat(uint32_t bits)
    {
        float value = 0.0f;
        memcpy(&value, &bits, sizeof(value));
        return value;
    }

    float ReadF32(uintptr_t address)
    {
        float value = 0.0f;
        const uint32_t bits = ReadU32(address);
        memcpy(&value, &bits, sizeof(value));
        return value;
    }

    bool BuildMode4BloomDepthControl(float outControl[4])
    {
        if (!outControl)
            return false;

        InterlockedIncrement(&s_mode4C0BridgeBuildCount);

        float nearPlane = 0.0f;
        float farPlane = 0.0f;
        bool valid = IsReadableMemory(reinterpret_cast<const void*>(kPcPostFxNearPlane), sizeof(float)) &&
            IsReadableMemory(reinterpret_cast<const void*>(kPcPostFxFarPlane), sizeof(float));
        if (valid)
        {
            nearPlane = ReadF32(kPcPostFxNearPlane);
            farPlane = ReadF32(kPcPostFxFarPlane);
            valid = std::isfinite(nearPlane) && std::isfinite(farPlane) &&
                nearPlane > 0.0f && farPlane > nearPlane + 0.001f && farPlane < 1000000.0f;
        }

        if (valid)
        {
            // Exact retail Xbox FUN_826B9F50 algebra, with only the proven
            // live PC near/far equivalents substituted.
            const float farScale = farPlane / (farPlane - nearPlane);
            outControl[0] =
                kXboxDofControlDistanceDefault * kXboxDofControlScaleDefault * 10.0f;
            outControl[1] =
                ((kXboxDofControlDistanceDefault - nearPlane) / kXboxDofControlDistanceDefault) *
                farScale;
            outControl[2] = kXboxDofControlZDefault;
            outControl[3] =
                ((kXboxDofControlFarDefault - nearPlane) / kXboxDofControlFarDefault) * farScale;

            valid = std::isfinite(outControl[0]) && std::isfinite(outControl[1]) &&
                std::isfinite(outControl[2]) && std::isfinite(outControl[3]);
        }

        if (!valid)
        {
            memcpy(outControl, kMode4BloomDepthControlFallback, sizeof(kMode4BloomDepthControlFallback));
            InterlockedIncrement(&s_mode4C0BridgeFallbackCount);
        }

        s_mode4LastPcNearPlane = nearPlane;
        s_mode4LastPcFarPlane = farPlane;
        memcpy(s_mode4LastBloomDepthControl, outControl, sizeof(s_mode4LastBloomDepthControl));
        s_mode4LastDynamicC0Valid = valid;
        return valid;
    }

    float HalfToFloat(uint16_t h)
    {
        const uint32_t sign = static_cast<uint32_t>(h & 0x8000u) << 16;
        const uint32_t exponent = (h >> 10) & 0x1Fu;
        uint32_t mantissa = h & 0x03FFu;
        uint32_t bits = 0;

        if (exponent == 0)
        {
            if (mantissa == 0)
            {
                bits = sign;
            }
            else
            {
                int32_t exponent32 = 127 - 14;
                while ((mantissa & 0x0400u) == 0)
                {
                    mantissa <<= 1;
                    --exponent32;
                }
                mantissa &= 0x03FFu;
                bits = sign | (static_cast<uint32_t>(exponent32) << 23) | (mantissa << 13);
            }
        }
        else if (exponent == 31)
        {
            bits = sign | 0x7F800000u | (mantissa << 13);
        }
        else
        {
            const uint32_t exp32 = exponent + (127u - 15u);
            bits = sign | (exp32 << 23) | (mantissa << 13);
        }

        float value = 0.0f;
        memcpy(&value, &bits, sizeof(value));
        return value;
    }

    float ClampFloat(float value, float low, float high)
    {
        if (value < low) return low;
        if (value > high) return high;
        return value;
    }

    bool IsFiniteUsefulFloat(float value)
    {
        return value == value && value > -65504.0f && value < 65504.0f;
    }

    void SafeReleaseCom(void*& object)
    {
        if (!object)
            return;

        void** vtable = *reinterpret_cast<void***>(object);
        if (vtable && IsReadableMemory(vtable, (kIUnknownReleaseVtableIndex + 1u) * sizeof(void*)) &&
            IsExecutableMemory(vtable[kIUnknownReleaseVtableIndex]))
        {
            const auto release = reinterpret_cast<IUnknownRelease_t>(vtable[kIUnknownReleaseVtableIndex]);
            release(object);
        }
        object = nullptr;
    }

    void ResetV9SceneResources()
    {
        SafeReleaseCom(s_v9StateBlock);
        SafeReleaseCom(s_v9SceneCopySurface);
        SafeReleaseCom(s_v9SceneCopyTexture);
        s_v9SceneResourceIdentity = 0;
        s_v9SceneWidth = 0;
        s_v9SceneHeight = 0;
        s_v9SceneFormat = 0;
    }

    void ResetV9ReadbackResource()
    {
        SafeReleaseCom(s_v9ReadbackSurface);
        s_v9ReadbackFormat = 0;
        s_v9ReadbackWidth = 0;
        s_v9ReadbackHeight = 0;
    }

    void BuildXeSm3IniPath()
    {
        memset(s_postProcessIniPath, 0, sizeof(s_postProcessIniPath));

        char exePath[MAX_PATH] = {};
        const DWORD length = GetModuleFileNameA(nullptr, exePath, MAX_PATH);
        if (length == 0 || length >= MAX_PATH)
        {
            sprintf_s(s_postProcessIniPath, "%s", "xeSM3.ini");
            return;
        }

        char* slash = strrchr(exePath, '\\');
        if (slash)
            *(slash + 1) = '\0';
        else
            exePath[0] = '\0';

        sprintf_s(s_postProcessIniPath, "%s%s", exePath, "xeSM3.ini");
    }

    const char* PostProcessRouteName()
    {
        switch (s_postProcessRoute)
        {
        case 3u: return "DebugXboxD2AdaptiveBloom";
        case 2u: return "RetailXbox";
        case 1u: return "PostProcessFix";
        default: return "RetailPC";
        }
    }

    void ReadV10ConfigOnce()
    {
        if (s_v10ConfigRead)
            return;

        s_v10ConfigRead = true;
        s_v9ConfigRead = true;

        BuildXeSm3IniPath();
#if defined(XESM3_PUBLIC_BUILD)
        // Public xeSM3 restores the simple exWoS-style 0/100 toggle contract.
        // Any other numeric value is treated as disabled rather than becoming an
        // undocumented priority/strength value. The shipped release defaults to
        // the qualified Debug Xbox route.
        const int publicFixValue =
            GetPrivateProfileIntA("PostProcessing", "PostProcessFix", 100, s_postProcessIniPath);
        const int publicRetailXboxValue =
            GetPrivateProfileIntA("PostProcessing", "PostProcessRetailXbox", 0, s_postProcessIniPath);
        const int publicDebugXboxValue =
            GetPrivateProfileIntA("PostProcessing", "PostProcessDebugXbox", 100, s_postProcessIniPath);
        s_postProcessFixEnabled = publicFixValue == 100;
        s_postProcessRetailXboxEnabled = publicRetailXboxValue == 100;
        s_postProcessDebugXboxEnabled = publicDebugXboxValue == 100;
#else
        s_postProcessFixEnabled =
            GetPrivateProfileIntA("PostProcessing", "PostProcessFix", 1, s_postProcessIniPath) != 0;
        s_postProcessRetailXboxEnabled =
            GetPrivateProfileIntA("PostProcessing", "PostProcessRetailXbox", 0, s_postProcessIniPath) != 0;
        s_postProcessDebugXboxEnabled =
            GetPrivateProfileIntA("PostProcessing", "PostProcessDebugXbox", 0, s_postProcessIniPath) != 0;
#endif

        if (s_postProcessDebugXboxEnabled)
            s_postProcessRoute = 3u;
        else if (s_postProcessRetailXboxEnabled)
            s_postProcessRoute = 2u;
        else if (s_postProcessFixEnabled)
            s_postProcessRoute = 1u;
        else
            s_postProcessRoute = 0u;

        // V10.5.65 keeps the proven Retail Xbox chain locked and activates Debug Xbox D2.
        // D2 inherits the contracts proven shared by both Xbox XEXes and now applies only
        // the source-proven Debug adaptive-bloom curve. GodRay remains observation-only.
        // Priority: Debug > Retail > Fix > RetailPC.
        s_postProcessNativeRepairActive = s_postProcessRoute != 0u;
        s_v10Mode = s_postProcessNativeRepairActive ? 5 : 6;

        // Shared PostProcessFix foundation:
        //   * eight PostFX allocations promoted 0x15 -> 0x71,
        //   * native Quarter A->B->A ping-pong,
        //   * valid BE64 clamp fallback at the actual final DrawPrimitive.
        // Retail Xbox additionally reconnects the proven GPU RESZ/INTZ depth producer,
        // F18 depth-aware final combine, and the exact Retail ImageZoom controller/pass.
        s_mode4CameraMotionZBlurEnabled = false;
        s_mode4CameraMotionZBlurCopyOnlyEnabled = false;
        s_mode4CameraMotionZBlurBindOnlyEnabled = false;
        s_mode4CameraMotionZBlurTextureBindOnlyEnabled = false;
        s_mode4CameraMotionZBlurSamplerStateOnlyEnabled = false;
        s_mode4CameraMotionZBlurRenderStateOnlyEnabled = false;
        s_mode4CameraMotionZBlurOutputTargetOnlyEnabled = false;

        // V10.5.11: keep the visually safe/proven D24S8 replacement.
        // INTZ remains an explicit diagnostic only because it produced scene
        // popping even while every resource/bind call reported success.
        s_mode4ReplacementFormat = kFormatD24S8;
        char depthFormatValue[32] = {};
        const DWORD depthFormatLength = GetEnvironmentVariableA(
            "XESM3_MODE4_DEPTH_FORMAT", depthFormatValue,
            static_cast<DWORD>(sizeof(depthFormatValue)));
        if (depthFormatLength > 0 && depthFormatLength < sizeof(depthFormatValue))
        {
            const char c = depthFormatValue[0];
            if (c == '0' || c == 'd' || c == 'D')
                s_mode4ReplacementFormat = kFormatD24S8;
            else if (c == '1' || c == 'i' || c == 'I')
                s_mode4ReplacementFormat = kFormatINTZ;
        }

        // V10.5.41 cross-GPU compatibility: the old 5/6 blend was a
        // diagnostic visibility aid, not a proven retail state. Do not force
        // it by default. Leave the game's stock final-combine blend untouched.
        // Set XESM3_MODE4_BLEND_TEST=1 only for A/B diagnostics.
        s_mode4BlendDiagnostic = false;
        char blendTestValue[32] = {};
        const DWORD blendTestLength = GetEnvironmentVariableA(
            "XESM3_MODE4_BLEND_TEST", blendTestValue,
            static_cast<DWORD>(sizeof(blendTestValue)));
        if (blendTestLength > 0 && blendTestLength < sizeof(blendTestValue))
        {
            const char c = blendTestValue[0];
            if (c == '1' || c == 'y' || c == 'Y' || c == 't' || c == 'T')
                s_mode4BlendDiagnostic = true;
        }

        s_mode4PackedDepthEnabled = true;
        char packEnabledValue[32] = {};
        const DWORD packEnabledLength = GetEnvironmentVariableA(
            "XESM3_MODE4_PACKED_DEPTH", packEnabledValue,
            static_cast<DWORD>(sizeof(packEnabledValue)));
        if (packEnabledLength > 0 && packEnabledLength < sizeof(packEnabledValue))
        {
            const char c = packEnabledValue[0];
            if (c == '0' || c == 'n' || c == 'N' || c == 'f' || c == 'F')
                s_mode4PackedDepthEnabled = false;
        }

        // Legacy V10.5.12 pack controls are still parsed for source continuity,
        // then forced off below while V10.5.16 is in native shadow-depth target probe mode.
        s_mode4PackChannel = 1u;
        s_mode4PackAutoCycle = true;
        char packChannelValue[32] = {};
        const DWORD packChannelLength = GetEnvironmentVariableA(
            "XESM3_MODE4_PACK_CHANNEL", packChannelValue,
            static_cast<DWORD>(sizeof(packChannelValue)));
        if (packChannelLength > 0 && packChannelLength < sizeof(packChannelValue))
        {
            const char c = packChannelValue[0];
            if (c == '1' || c == 'g' || c == 'G') s_mode4PackChannel = 1u;
            else if (c == '2' || c == 'b' || c == 'B') s_mode4PackChannel = 2u;
            else if (c == '3' || c == 'a' || c == 'A') s_mode4PackChannel = 3u;
            else s_mode4PackChannel = 0u;
            s_mode4PackAutoCycle = false;
        }

        char packAutoValue[32] = {};
        const DWORD packAutoLength = GetEnvironmentVariableA(
            "XESM3_MODE4_PACK_AUTO", packAutoValue,
            static_cast<DWORD>(sizeof(packAutoValue)));
        if (packAutoLength > 0 && packAutoLength < sizeof(packAutoValue))
        {
            const char c = packAutoValue[0];
            if (c == '0' || c == 'n' || c == 'N' || c == 'f' || c == 'F')
                s_mode4PackAutoCycle = false;
        }

        // V10.5.16 is a non-visual native shadow-depth target probe. Force all previous Mode 4 visual
        // experiments off even if old environment variables are still present.
        if (s_mode4MidProbeOnly)
        {
            s_mode4ReplacementFormat = kFormatD24S8;
            s_mode4BlendDiagnostic = false;
            s_mode4PackedDepthEnabled = false;
            s_mode4PackAutoCycle = false;
        }

        // V9 visual exposure remains exclusive to frozen reference mode 2.
        // Modes 3/4 do not draw V9's final-frame multiply. Mode 4 is the
        // native shadow-depth target identity probe.
        s_v9ExposureEnabled = (s_v10Mode == 2);

        // Preserve the old V9 kill switch as a compatibility safety override.
        if (s_v9ExposureEnabled)
        {
            char exposureValue[32] = {};
            const DWORD exposureLength = GetEnvironmentVariableA(
                "XESM3_V9_EXPOSURE", exposureValue, static_cast<DWORD>(sizeof(exposureValue)));
            if (exposureLength > 0 && exposureLength < sizeof(exposureValue))
            {
                if (exposureValue[0] == '0' || exposureValue[0] == 'n' ||
                    exposureValue[0] == 'N' || exposureValue[0] == 'f' || exposureValue[0] == 'F')
                {
                    s_v9ExposureEnabled = false;
                }
            }
        }
    }

    void QueueV9Config()
    {
        if (s_v9ConfigQueued)
            return;
        s_v9ConfigQueued = true;

        Event* event = ReserveEvent(EventType::ExposureConfig);
        if (!event)
            return;
        event->a = s_presentCount;
        event->b = s_v9ExposureEnabled ? 1u : 0u;
        event->c = kV9ReadbackInterval;
        event->d = kV9WarmupSamples;
        event->e = FloatBits(kV9MinExposure);
        event->f = FloatBits(kV9MaxExposure);
        event->g = FloatBits(kV9DarkenAlpha);
        event->h = FloatBits(kV9BrightenAlpha);
    }

    void QueueV10Config()
    {
        if (s_v10ConfigQueued)
            return;
        s_v10ConfigQueued = true;

        Event* event = ReserveEvent(EventType::BloomControlConfig);
        if (!event)
            return;
        event->a = s_presentCount;
        event->b = static_cast<uint32_t>(s_v10Mode);
        event->c = kV10ReadbackInterval;
        event->d = FloatBits(kV10XboxDarkBrightness);
        event->e = FloatBits(kV10XboxLightBrightness);
        event->f = FloatBits(kV10XboxDarkBloom);
        event->g = FloatBits(kV10XboxLightBloom);
        event->h = FloatBits(kV10WeightR);
        event->i = FloatBits(kV10WeightG);
        event->j = FloatBits(kV10WeightB);
    }

    void QueueV10_3XboxExposureConfig()
    {
        if (s_v10_3XboxExposureConfigQueued)
            return;
        s_v10_3XboxExposureConfigQueued = true;

        Event* event = ReserveEvent(EventType::XboxExposureStateConfig);
        if (!event)
            return;
        event->a = s_presentCount;
        event->b = 1u; // recovered Xbox GRAPHOPTS_AUTOEXPOSURE default
        event->c = static_cast<uint32_t>(kPCGameTimeGlobal);
        event->d = FloatBits(kXboxExposureInitial);
        event->e = FloatBits(kXboxExposureMin);
        event->f = FloatBits(kXboxExposureMax);
        event->g = FloatBits(kXboxExposureBrightnessLow);
        event->h = FloatBits(kXboxExposureBrightnessHigh);
        event->i = FloatBits(kXboxExposureSpeed);
        event->j = FloatBits(kXboxManualExposure);
        event->extra[0] = FloatBits(kXboxExposureFallbackTarget);
        event->extra[1] = FloatBits(kXboxExposureFallbackKeep);
        event->extra[2] = FloatBits(kXboxExposureFallbackBlend);
        event->extra[3] = FloatBits(kXboxExposureDeltaScale);
        event->extra[4] = kPCGameTimeWholeSecondsOffset;
        event->extra[5] = kPCGameTimeFractionOffset;
        event->extra[6] = kV10ReadbackInterval;
    }

    bool ReadPCGameTimeSeconds(float& secondsOut, uint32_t& worldOut)
    {
        secondsOut = 0.0f;
        worldOut = ReadU32(kPCGameTimeGlobal);
        if (worldOut < 0x10000u)
            return false;

        const uintptr_t world = static_cast<uintptr_t>(worldOut);
        const void* wholePtr = reinterpret_cast<const void*>(world + kPCGameTimeWholeSecondsOffset);
        const void* fractionPtr = reinterpret_cast<const void*>(world + kPCGameTimeFractionOffset);
        if (!IsReadableMemory(wholePtr, sizeof(uint32_t)) ||
            !IsReadableMemory(fractionPtr, sizeof(float)))
        {
            return false;
        }

        const uint32_t wholeSeconds = *reinterpret_cast<volatile const uint32_t*>(wholePtr);
        const float fractionalSeconds = *reinterpret_cast<volatile const float*>(fractionPtr);
        if (!IsFiniteUsefulFloat(fractionalSeconds))
            return false;

        secondsOut = static_cast<float>(wholeSeconds) + fractionalSeconds;
        return IsFiniteUsefulFloat(secondsOut) && secondsOut >= 0.0f;
    }

    void UpdateV10_3XboxExposureState()
    {
        if (!s_v10BloomSampleValid)
            return;

        const float sceneMeanSq = s_v10Brightness255 * s_v10Brightness255;
        float gameSeconds = 0.0f;
        uint32_t worldPointer = 0u;
        const bool timeValid = ReadPCGameTimeSeconds(gameSeconds, worldPointer);

        float dayFraction = 0.0f;
        float cosPhase = 0.0f;
        float targetMeanSq = 0.0f;
        float delta = 0.0f;

        if (timeValid)
        {
            const float days = gameSeconds * kXboxSecondsToDays;
            dayFraction = days - static_cast<float>(std::floor(static_cast<double>(days)));
            const float phase = dayFraction * kXboxTwoPi;
            cosPhase = static_cast<float>(std::cos(static_cast<double>(phase)));

            const float highSq = kXboxExposureBrightnessHigh * kXboxExposureBrightnessHigh;
            const float lowSq = kXboxExposureBrightnessLow * kXboxExposureBrightnessLow;
            targetMeanSq = 0.5f * (highSq + lowSq) -
                0.5f * cosPhase * (highSq - lowSq);

            // Exact Function_82F31E60 recurrence:
            // exposure += (targetMeanSq - sceneMeanSq) * 2.5 * 1e-6
            delta = (targetMeanSq - sceneMeanSq) *
                kXboxExposureSpeed * kXboxExposureDeltaScale;
            s_v10_3XboxExposureState = ClampFloat(
                s_v10_3XboxExposureState + delta,
                kXboxExposureMin, kXboxExposureMax);
        }
        else
        {
            // Exact console fallback when its environment/time object is absent:
            // exposure = exposure * 0.99 + 2.0 * 0.01
            const float previous = s_v10_3XboxExposureState;
            s_v10_3XboxExposureState =
                previous * kXboxExposureFallbackKeep +
                kXboxExposureFallbackTarget * kXboxExposureFallbackBlend;
            delta = s_v10_3XboxExposureState - previous;
        }

        ++s_v10_3XboxExposureUpdateCount;
        s_v10_3LastSceneMeanSq = sceneMeanSq;
        s_v10_3LastTargetMeanSq = targetMeanSq;
        s_v10_3LastDelta = delta;
        s_v10_3LastGameSeconds = gameSeconds;
        s_v10_3LastDayFraction = dayFraction;
        s_v10_3LastCosPhase = cosPhase;
        s_v10_3LastTimeValid = timeValid;

        if (s_v10_3XboxExposureUpdateCount <= 32u ||
            (s_v10_3XboxExposureUpdateCount % 60u) == 0u)
        {
            Event* event = ReserveEvent(EventType::XboxExposureStateSample);
            if (event)
            {
                const uint32_t sampleAge = s_presentCount >= s_v10_3LastLuminanceSamplePresent ?
                    s_presentCount - s_v10_3LastLuminanceSamplePresent : 0u;
                event->a = s_presentCount;
                event->b = s_v10_3XboxExposureUpdateCount;
                event->c = timeValid ? 1u : 0u;
                event->d = sampleAge;
                event->e = FloatBits(gameSeconds);
                event->f = FloatBits(dayFraction);
                event->g = FloatBits(cosPhase);
                event->h = FloatBits(sceneMeanSq);
                event->i = FloatBits(targetMeanSq);
                event->j = FloatBits(delta);
                event->extra[0] = FloatBits(s_v10_3XboxExposureState);
                event->extra[1] = FloatBits(-s_v10_3XboxExposureState); // value supplied by Xbox render-data path
                event->extra[2] = FloatBits(s_v10Brightness255);
                event->extra[3] = FloatBits(s_v10BloomStrength);
                event->extra[4] = FloatBits(s_v9CurrentLuminance);
                event->extra[5] = FloatBits(s_v9TargetExposure);
                event->extra[6] = FloatBits(s_v9AdaptedExposure);
                event->extra[7] = static_cast<uint32_t>(s_v10Mode);
                event->extra[8] = worldPointer;
            }
        }
    }

    bool EnsureMode4TextureBackedSceneDepth(void* device, const D3DSurfaceDescLite& sourceDesc);
    bool EnsureMode4FullResFloatDepthTarget(void* device, const D3DSurfaceDescLite& sceneDesc, void* activeRT, void* activeDS);
    bool RunMode4RenderListMainCameraDepthPrepass(void* device, void* activeRT, void* activeDS);
    void SampleMode4DepthPrepassReadback(void* device);
    bool RunMode4IntzSampleReadbackSelfTest(void* device);
    bool ApplyMode4PackedDepthPass(void* device);
    void __cdecl Mode4_OpaqueIntzReplayMid(void* userData);
    bool RunMode4OpaqueIntzReplay(uint32_t scene);
    bool PackMode4OpaqueIntzDepthForDof(void* device);
    bool SampleMode4OpaqueIntzReplayReadback(void* device);
    void __cdecl Mode4_ReszDepthResolveMid(void* userData);
    bool RunMode4ReszDepthResolve(uint32_t scene);

    bool GetSurfaceDescSafe(void* surface, D3DSurfaceDescLite& desc)
    {
        ZeroMemory(&desc, sizeof(desc));
        if (!surface || !IsReadableMemory(surface, sizeof(void*)))
            return false;
        void** vtable = *reinterpret_cast<void***>(surface);
        if (!vtable || !IsReadableMemory(vtable, (kD3DSurfaceGetDescVtableIndex + 1u) * sizeof(void*)))
            return false;
        void* method = vtable[kD3DSurfaceGetDescVtableIndex];
        if (!IsExecutableMemory(method))
            return false;
        const auto getDesc = reinterpret_cast<D3DSurfaceGetDesc_t>(method);
        return SUCCEEDED(getDesc(surface, &desc));
    }


    void __cdecl Mode4_MidSceneDepthProbe(void* userData)
    {
        (void)userData;
        const LONG count = InterlockedIncrement(&s_mode4MidCallbackCount);
        const uint32_t scene = ReadU32(kNGLCurrentSceneGlobal);
        const uint32_t deviceValue = ReadU32(kD3DDeviceGlobal);
        const uint32_t nglBackBuffer = ReadU32(kNGLBackBufferGlobal);
        const uint32_t nglDepthWrapper = ReadU32(kNGLDepthCompanionGlobal);
        const uint32_t nglMainDepthSurface = ReadU32(kNGLMainDepthSurfaceGlobal);
        uint32_t wrapperSurface = 0u;
        if (nglDepthWrapper >= 0x10000u &&
            IsReadableMemory(reinterpret_cast<void*>(static_cast<uintptr_t>(nglDepthWrapper + kNGLWrapperSurfaceOffset)), sizeof(uint32_t)))
        {
            wrapperSurface = ReadU32(static_cast<uintptr_t>(nglDepthWrapper + kNGLWrapperSurfaceOffset));
        }

        void* activeRT = nullptr;
        void* activeDS = nullptr;
        HRESULT rtHr = E_NOINTERFACE;
        HRESULT dsHr = E_NOINTERFACE;
        D3DSurfaceDescLite rtDesc = {};
        D3DSurfaceDescLite dsDesc = {};
        bool rtDescOk = false;
        bool dsDescOk = false;

        void* device = reinterpret_cast<void*>(static_cast<uintptr_t>(deviceValue));
        if (device && s_rawGetRenderTarget)
            rtHr = s_rawGetRenderTarget(device, 0u, &activeRT);
        if (device && s_rawGetDepthStencilSurface)
            dsHr = s_rawGetDepthStencilSurface(device, &activeDS);
        if (SUCCEEDED(rtHr) && activeRT)
            rtDescOk = GetSurfaceDescSafe(activeRT, rtDesc);
        if (SUCCEEDED(dsHr) && activeDS)
            dsDescOk = GetSurfaceDescSafe(activeDS, dsDesc);

        // V10.5.15 proved this direct D24S8 StretchRect route is invalid on PC
        // (D3DERR_INVALIDCALL). V10.5.16 keeps the historical fields/logging code
        // but deliberately disables new copy attempts while we map the native
        // sm_depth_shadow target topology.
        bool resolveAttempted = false;
        bool resolveReady = false;
        HRESULT resolveHr = E_PENDING;
        D3DSurfaceDescLite resolveDesc = {};
        bool resolveDescOk = false;
        const bool mainMatchNow = activeDS && nglMainDepthSurface &&
            static_cast<uint32_t>(reinterpret_cast<uintptr_t>(activeDS)) == nglMainDepthSurface;

        // V10.5.17: reproduce only the proven native target topology at full
        // resolution. Create R32F RT texture + D24S8 DS, bind once, restore
        // immediately, and do not clear/draw/activate DOF.
        bool fullResReady = false;
        D3DSurfaceDescLite fullResRTDesc = {};
        D3DSurfaceDescLite fullResDSDesc = {};
        bool fullResRTDescOk = false;
        bool fullResDSDescOk = false;
        if (mainMatchNow && rtDescOk && dsDescOk)
        {
            fullResReady = EnsureMode4FullResFloatDepthTarget(
                device, rtDesc, activeRT, activeDS);
            if (fullResReady)
            {
                fullResRTDescOk = GetSurfaceDescSafe(s_fullResDepthSurface, fullResRTDesc);
                fullResDSDescOk = GetSurfaceDescSafe(s_fullResDepthStencil, fullResDSDesc);
            }
        }

        const LONG profileState = s_mode4DepthPrepassRunState;
        if (fullResReady && (profileState == 0 || profileState == 2))
        {
            RunMode4RenderListMainCameraDepthPrepass(device, activeRT, activeDS);
        }

        if ((count <= 32 || (count % 120) == 0) &&
            (fullResReady || s_fullResDepthTargetCreateState != 0))
        {
            Event* targetEvent = ReserveEvent(EventType::FullResDepthTargetProbe);
            if (targetEvent)
            {
                targetEvent->a = s_presentCount;
                targetEvent->b = static_cast<uint32_t>(count);
                targetEvent->c = fullResReady ? 1u : 0u;
                targetEvent->d = static_cast<uint32_t>(
                    reinterpret_cast<uintptr_t>(s_fullResDepthTexture));
                targetEvent->e = static_cast<uint32_t>(
                    reinterpret_cast<uintptr_t>(s_fullResDepthSurface));
                targetEvent->f = static_cast<uint32_t>(
                    reinterpret_cast<uintptr_t>(s_fullResDepthStencil));
                targetEvent->g = static_cast<uint32_t>(s_fullResCreateTextureHr);
                targetEvent->h = static_cast<uint32_t>(s_fullResGetSurfaceHr);
                targetEvent->i = static_cast<uint32_t>(s_fullResCreateDepthHr);
                targetEvent->j = static_cast<uint32_t>(s_fullResContainerHr);
                targetEvent->extra[0] = s_fullResContainerTex;
                targetEvent->extra[1] = fullResRTDescOk ? fullResRTDesc.Format : 0u;
                targetEvent->extra[2] = fullResRTDescOk ? fullResRTDesc.Width : 0u;
                targetEvent->extra[3] = fullResRTDescOk ? fullResRTDesc.Height : 0u;
                targetEvent->extra[4] = fullResDSDescOk ? fullResDSDesc.Format : 0u;
                targetEvent->extra[5] = fullResDSDescOk ? fullResDSDesc.Width : 0u;
                targetEvent->extra[6] = fullResDSDescOk ? fullResDSDesc.Height : 0u;
                targetEvent->extra[7] = static_cast<uint32_t>(s_fullResBindRTHr);
                targetEvent->extra[8] = static_cast<uint32_t>(s_fullResBindDSHr);
                targetEvent->extra[9] = static_cast<uint32_t>(s_fullResRestoreDSHr);
                targetEvent->extra[10] = static_cast<uint32_t>(s_fullResRestoreRTHr);
                targetEvent->extra[11] = mainMatchNow ? 1u : 0u;
            }
        }

        const bool resolveEligible = false; // retired after V10.5.15 runtime proof
        if (resolveEligible)
        {
            resolveAttempted = true;
            InterlockedIncrement(&s_mode4MidResolveAttemptCount);
            resolveReady = EnsureMode4TextureBackedSceneDepth(device, dsDesc) &&
                s_mode4ReplacementDepthSurface != nullptr && s_mode4ReplacementDepthTexture != nullptr;
            if (resolveReady)
            {
                resolveHr = s_rawStretchRect(device, activeDS, nullptr,
                    s_mode4ReplacementDepthSurface, nullptr, kD3DFilterNone);
                resolveDescOk = GetSurfaceDescSafe(s_mode4ReplacementDepthSurface, resolveDesc);
            }
            else
            {
                resolveHr = FAILED(s_mode4ReplacementCreateHr) ? s_mode4ReplacementCreateHr :
                    (FAILED(s_mode4ReplacementSurfaceHr) ? s_mode4ReplacementSurfaceHr : E_FAIL);
            }
            s_mode4MidResolveLastHr = resolveHr;
            if (SUCCEEDED(resolveHr))
                InterlockedIncrement(&s_mode4MidResolveSuccessCount);
            else
                InterlockedIncrement(&s_mode4MidResolveFailCount);
        }

        if (resolveAttempted && (count <= 32 || (count % 120) == 0))
        {
            Event* resolveEvent = ReserveEvent(EventType::MidSceneResolveProbe);
            if (resolveEvent)
            {
                resolveEvent->a = s_presentCount;
                resolveEvent->b = static_cast<uint32_t>(count);
                resolveEvent->c = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(activeDS));
                resolveEvent->d = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(s_mode4ReplacementDepthSurface));
                resolveEvent->e = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(s_mode4ReplacementDepthTexture));
                resolveEvent->f = static_cast<uint32_t>(s_mode4ReplacementCreateHr);
                resolveEvent->g = static_cast<uint32_t>(s_mode4ReplacementSurfaceHr);
                resolveEvent->h = static_cast<uint32_t>(resolveHr);
                resolveEvent->i = dsDesc.Format;
                resolveEvent->j = resolveDescOk ? resolveDesc.Format : 0u;
                resolveEvent->extra[0] = dsDesc.Width;
                resolveEvent->extra[1] = dsDesc.Height;
                resolveEvent->extra[2] = resolveDescOk ? resolveDesc.Width : 0u;
                resolveEvent->extra[3] = resolveDescOk ? resolveDesc.Height : 0u;
                resolveEvent->extra[4] = mainMatchNow ? 1u : 0u;
                resolveEvent->extra[5] = resolveReady ? 1u : 0u;
                resolveEvent->extra[6] = static_cast<uint32_t>(s_mode4MidResolveAttemptCount);
                resolveEvent->extra[7] = static_cast<uint32_t>(s_mode4MidResolveSuccessCount);
                resolveEvent->extra[8] = static_cast<uint32_t>(s_mode4MidResolveFailCount);
                resolveEvent->extra[9] = static_cast<uint32_t>(s_mode4ReplacementContainerHr);
                resolveEvent->extra[10] = s_mode4ReplacementContainerTex;
                resolveEvent->extra[11] = resolveDescOk ? resolveDesc.Usage : 0u;
            }
        }

        if (count <= 32 || (count % 120) == 0)
        {
            Event* event = ReserveEvent(EventType::MidSceneDepthProbe);
            if (event)
            {
                event->a = s_presentCount;
                event->b = static_cast<uint32_t>(count);
                event->c = scene;
                event->d = deviceValue;
                event->e = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(activeRT));
                event->f = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(activeDS));
                event->g = static_cast<uint32_t>(rtHr);
                event->h = static_cast<uint32_t>(dsHr);
                event->i = nglBackBuffer;
                event->j = nglDepthWrapper;
                event->extra[0] = wrapperSurface;
                event->extra[1] = (activeDS && wrapperSurface &&
                    static_cast<uint32_t>(reinterpret_cast<uintptr_t>(activeDS)) == wrapperSurface) ? 1u : 0u;
                event->extra[2] = rtDescOk ? rtDesc.Format : 0u;
                event->extra[3] = rtDescOk ? rtDesc.Width : 0u;
                event->extra[4] = rtDescOk ? rtDesc.Height : 0u;
                event->extra[5] = dsDescOk ? dsDesc.Format : 0u;
                event->extra[6] = dsDescOk ? dsDesc.Width : 0u;
                event->extra[7] = dsDescOk ? dsDesc.Height : 0u;
                event->extra[8] = static_cast<uint32_t>(s_rawHookState);
                event->extra[9] = (scene >= 0x10000u && IsReadableMemory(
                    reinterpret_cast<void*>(static_cast<uintptr_t>(scene + kSceneMidCallbackOffset)), sizeof(uint32_t)))
                    ? ReadU32(static_cast<uintptr_t>(scene + kSceneMidCallbackOffset)) : 0u;
                event->extra[10] = nglMainDepthSurface;
                event->extra[11] = (activeDS && nglMainDepthSurface &&
                    static_cast<uint32_t>(reinterpret_cast<uintptr_t>(activeDS)) == nglMainDepthSurface) ? 1u : 0u;
            }
        }

        SafeReleaseCom(activeDS);
        SafeReleaseCom(activeRT);
    }


    struct SceneClassBucket
    {
        uint32_t vtable;
        uint32_t render;
        uint32_t count;
        uint32_t sampleNode;
    };

    uint32_t ProfileSceneListClasses(uint32_t scene, uint32_t listType, uint32_t head,
                                     uint32_t declaredCount, uint32_t sentinel,
                                     uint32_t depth, uint32_t parentScene, const char* sceneName)
    {
        if (!head || !sentinel)
            return 0u;

        SceneClassBucket buckets[64] = {};
        uint32_t bucketCount = 0u;
        uint32_t scanned = 0u;
        uint32_t node = head;
        uint32_t guard = declaredCount ? (declaredCount + 32u) : 512u;
        if (guard > kSceneTreeProfileMaxNodesPerList)
            guard = kSceneTreeProfileMaxNodesPerList;

        while (node && node != sentinel && scanned < guard)
        {
            if (!IsReadableMemory(reinterpret_cast<void*>(static_cast<uintptr_t>(node)), 8u))
            {
                InterlockedIncrement(&s_sceneTreeProfileReadFailures);
                break;
            }

            const uint32_t vtable = ReadU32(node);
            uint32_t render = 0u;
            if (vtable >= 0x10000u && IsReadableMemory(
                reinterpret_cast<void*>(static_cast<uintptr_t>(vtable + 8u)), sizeof(uint32_t)))
            {
                render = ReadU32(static_cast<uintptr_t>(vtable + 8u));
            }

            uint32_t index = bucketCount;
            for (uint32_t n = 0; n < bucketCount; ++n)
            {
                if (buckets[n].vtable == vtable && buckets[n].render == render)
                {
                    index = n;
                    break;
                }
            }

            if (index < bucketCount)
            {
                ++buckets[index].count;
            }
            else if (bucketCount < 64u)
            {
                buckets[bucketCount].vtable = vtable;
                buckets[bucketCount].render = render;
                buckets[bucketCount].count = 1u;
                buckets[bucketCount].sampleNode = node;
                ++bucketCount;
            }

            ++scanned;
            node = ReadU32(static_cast<uintptr_t>(node + 4u));
        }

        for (uint32_t n = 0; n < bucketCount; ++n)
        {
            const LONG eventIndex = InterlockedIncrement(&s_sceneTreeProfileNodeClassEvents);
            if (eventIndex > static_cast<LONG>(kSceneTreeProfileMaxClassEvents))
                break;

            Event* event = ReserveEvent(EventType::SceneNodeClassProfile);
            if (!event)
                break;
            event->a = s_presentCount;
            event->b = scene;
            event->c = depth;
            event->d = parentScene;
            event->e = listType;
            event->f = buckets[n].count;
            event->g = buckets[n].sampleNode;
            event->h = buckets[n].vtable;
            event->i = buckets[n].render;
            event->j = scanned;
            event->extra[0] = declaredCount;
            CopySmallText(event->text, sizeof(event->text), sceneName);
        }

        return scanned;
    }

    void ProfileCurrentNGLScene(uint32_t scene, uint32_t depth, uint32_t parentScene)
    {
        if (scene < 0x10000u || !IsReadableMemory(
            reinterpret_cast<void*>(static_cast<uintptr_t>(scene)), 0x320u))
        {
            InterlockedIncrement(&s_sceneTreeProfileReadFailures);
            return;
        }

        const LONG sceneIndex = InterlockedIncrement(&s_sceneTreeProfileSceneCount);
        if (sceneIndex > static_cast<LONG>(kSceneTreeProfileMaxScenes))
            return;

        const char* sceneName = *reinterpret_cast<const char**>(static_cast<uintptr_t>(scene + 0x2E0u));
        if (!sceneName || !IsReadableMemory(sceneName, 48u))
            sceneName = nullptr;
        const uint32_t renderTarget = ReadU32(static_cast<uintptr_t>(scene + kSceneRenderTargetOffset));
        const uint32_t zTarget = ReadU32(static_cast<uintptr_t>(scene + kSceneZTargetOffset));
        const uint32_t targetWidth = ReadU32(static_cast<uintptr_t>(scene + kSceneTargetWidthOffset));
        const uint32_t targetHeight = ReadU32(static_cast<uintptr_t>(scene + kSceneTargetHeightOffset));
        const uint32_t projType = ReadU32(static_cast<uintptr_t>(scene + kSceneProjTypeOffset));
        const uint32_t opaqueHead = ReadU32(static_cast<uintptr_t>(scene + kSceneOpaqueRenderListOffset));
        const uint32_t transHead = ReadU32(static_cast<uintptr_t>(scene + kSceneTransRenderListOffset));
        const uint32_t opaqueCount = ReadU32(static_cast<uintptr_t>(scene + kSceneOpaqueListCountOffset));
        const uint32_t transCount = ReadU32(static_cast<uintptr_t>(scene + kSceneTransListCountOffset));
        const uint32_t sentinel = ReadU32(kRenderListSentinelGlobal);

        const uint32_t opaqueScanned = ProfileSceneListClasses(
            scene, 0u, opaqueHead, opaqueCount, sentinel, depth, parentScene, sceneName);
        const uint32_t transScanned = ProfileSceneListClasses(
            scene, 1u, transHead, transCount, sentinel, depth, parentScene, sceneName);

        s_sceneTreeProfileOpaqueNodes += static_cast<LONG>(opaqueScanned);
        s_sceneTreeProfileTransNodes += static_cast<LONG>(transScanned);

        Event* event = ReserveEvent(EventType::SceneListProfile);
        if (event)
        {
            event->a = s_presentCount;
            event->b = static_cast<uint32_t>(sceneIndex);
            event->c = scene;
            event->d = depth;
            event->e = parentScene;
            event->f = renderTarget;
            event->g = zTarget;
            event->h = targetWidth;
            event->i = targetHeight;
            event->j = projType;
            event->extra[0] = opaqueHead;
            event->extra[1] = transHead;
            event->extra[2] = opaqueCount;
            event->extra[3] = transCount;
            event->extra[4] = opaqueScanned;
            event->extra[5] = transScanned;
            event->extra[6] = sentinel;
            CopySmallText(event->text, sizeof(event->text), sceneName);
        }
    }

    bool RunMode4OffscreenIntzTargetProbe(uint32_t scene)
    {
        if (s_v10Mode != 4 || scene < 0x10000u ||
            !IsReadableMemory(reinterpret_cast<void*>(static_cast<uintptr_t>(scene)), 0x320u))
            return false;

        const LONG priorState = InterlockedCompareExchange(&s_offscreenIntzProbeState, 1, 0);
        if (priorState != 0)
            return priorState == 1;

        const uint32_t width = ReadU32(static_cast<uintptr_t>(scene + kSceneTargetWidthOffset));
        const uint32_t height = ReadU32(static_cast<uintptr_t>(scene + kSceneTargetHeightOffset));
        void* device = reinterpret_cast<void*>(static_cast<uintptr_t>(s_rawDeviceValue));
        bool ok = false;

        if (!device || width == 0u || height == 0u || !s_rawCreateTexture ||
            !s_rawGetRenderTarget || !s_rawGetDepthStencilSurface ||
            !s_rawSetRenderTarget || !s_rawSetDepthStencilSurface ||
            !s_rawCreateStateBlock)
        {
            InterlockedExchange(&s_offscreenIntzProbeState, -1);
            return false;
        }

        void* previousRT = nullptr;
        void* previousDS = nullptr;
        void* stateBlock = nullptr;
        void* verifyRT = nullptr;
        void* verifyDS = nullptr;
        D3DSurfaceDescLite sceneDesc = {};
        sceneDesc.Width = width;
        sceneDesc.Height = height;

        s_offscreenIntzWidth = width;
        s_offscreenIntzHeight = height;
        s_offscreenIntzGetRTHr = s_rawGetRenderTarget(device, 0u, &previousRT);
        s_offscreenIntzGetDSHr = s_rawGetDepthStencilSurface(device, &previousDS);

        if (FAILED(s_offscreenIntzGetRTHr) || !previousRT ||
            (FAILED(s_offscreenIntzGetDSHr) && s_offscreenIntzGetDSHr != kD3DErrNotFound))
            goto done;

        // Reuse the already-proven V10.5.17 R32F render-target allocator.  Its
        // companion D24S8 is not used by this test; we only need its R32F color RT.
        if (!EnsureMode4FullResFloatDepthTarget(device, sceneDesc, previousRT, previousDS) ||
            !s_fullResDepthSurface)
            goto done;

        if (!(s_offscreenIntzTexture && s_offscreenIntzSurface &&
              s_offscreenIntzWidth == width && s_offscreenIntzHeight == height))
        {
            void* texture = nullptr;
            s_offscreenIntzCreateHr = s_rawCreateTexture(
                device, width, height, 1u, kD3DUsageDepthStencil,
                kFormatINTZ, kD3DPoolDefault, &texture, nullptr);
            if (FAILED(s_offscreenIntzCreateHr) || !texture)
                goto done;

            void* surface = nullptr;
            void** textureVtable = IsReadableMemory(texture, sizeof(void*))
                ? *reinterpret_cast<void***>(texture) : nullptr;
            if (!textureVtable || !IsReadableMemory(textureVtable,
                (kD3DTextureGetSurfaceLevelVtableIndex + 1u) * sizeof(void*)) ||
                !IsExecutableMemory(textureVtable[kD3DTextureGetSurfaceLevelVtableIndex]))
            {
                s_offscreenIntzGetSurfaceHr = E_NOINTERFACE;
                SafeReleaseCom(texture);
                goto done;
            }

            const auto getSurfaceLevel = reinterpret_cast<D3DTextureGetSurfaceLevel_t>(
                textureVtable[kD3DTextureGetSurfaceLevelVtableIndex]);
            s_offscreenIntzGetSurfaceHr = getSurfaceLevel(texture, 0u, &surface);
            if (FAILED(s_offscreenIntzGetSurfaceHr) || !surface)
            {
                SafeReleaseCom(texture);
                goto done;
            }

            D3DSurfaceDescLite intzDesc = {};
            s_offscreenIntzDescHr = GetSurfaceDescSafe(surface, intzDesc) ? S_OK : E_FAIL;
            const uint32_t surfacePtr = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(surface));
            s_offscreenIntzContainerTex = RawTextureFromSurface(surfacePtr, &s_offscreenIntzContainerHr);
            const uint32_t texturePtr = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(texture));
            if (FAILED(s_offscreenIntzDescHr) || intzDesc.Format != kFormatINTZ ||
                intzDesc.Width != width || intzDesc.Height != height ||
                FAILED(s_offscreenIntzContainerHr) || s_offscreenIntzContainerTex != texturePtr)
            {
                SafeReleaseCom(surface);
                SafeReleaseCom(texture);
                goto done;
            }

            s_offscreenIntzTexture = texture;
            s_offscreenIntzSurface = surface;
        }

        s_offscreenIntzStateBlockHr = s_rawCreateStateBlock(device, kD3DStateBlockAll, &stateBlock);
        if (FAILED(s_offscreenIntzStateBlockHr) || !stateBlock)
            goto done;

        {
            void** stateVtable = IsReadableMemory(stateBlock, sizeof(void*))
                ? *reinterpret_cast<void***>(stateBlock) : nullptr;
            if (!stateVtable || !IsReadableMemory(stateVtable,
                (kD3DStateBlockApplyVtableIndex + 1u) * sizeof(void*)) ||
                !IsExecutableMemory(stateVtable[kD3DStateBlockCaptureVtableIndex]) ||
                !IsExecutableMemory(stateVtable[kD3DStateBlockApplyVtableIndex]))
                goto done;

            const auto capture = reinterpret_cast<D3DStateBlockCapture_t>(
                stateVtable[kD3DStateBlockCaptureVtableIndex]);
            const auto apply = reinterpret_cast<D3DStateBlockApply_t>(
                stateVtable[kD3DStateBlockApplyVtableIndex]);
            s_offscreenIntzCaptureHr = capture(stateBlock);
            if (FAILED(s_offscreenIntzCaptureHr))
                goto done;

            s_offscreenIntzBindRTHr = s_rawSetRenderTarget(device, 0u, s_fullResDepthSurface);
            s_offscreenIntzBindDSHr = SUCCEEDED(s_offscreenIntzBindRTHr)
                ? s_rawSetDepthStencilSurface(device, s_offscreenIntzSurface)
                : s_offscreenIntzBindRTHr;

            if (SUCCEEDED(s_offscreenIntzBindRTHr) && SUCCEEDED(s_offscreenIntzBindDSHr))
            {
                if (SUCCEEDED(s_rawGetRenderTarget(device, 0u, &verifyRT)) && verifyRT)
                    s_offscreenIntzActualRT = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(verifyRT));
                if (SUCCEEDED(s_rawGetDepthStencilSurface(device, &verifyDS)) && verifyDS)
                    s_offscreenIntzActualDS = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(verifyDS));

                void** deviceVtable = IsReadableMemory(device, sizeof(void*))
                    ? *reinterpret_cast<void***>(device) : nullptr;
                if (deviceVtable && IsReadableMemory(deviceVtable,
                    (kD3DClearVtableIndex + 1u) * sizeof(void*)) &&
                    IsExecutableMemory(deviceVtable[kD3DClearVtableIndex]))
                {
                    const auto clear = reinterpret_cast<D3DClear_t>(deviceVtable[kD3DClearVtableIndex]);
                    s_offscreenIntzClearHr = clear(
                        device, 0u, nullptr, kD3DClearTarget | kD3DClearZBuffer,
                        0u, 1.0f, 0u);
                }
                else
                {
                    s_offscreenIntzClearHr = E_NOINTERFACE;
                }
            }

            // Restore the exact live surfaces before restoring the full state block.
            s_offscreenIntzRestoreDSHr = s_rawSetDepthStencilSurface(device, previousDS);
            s_offscreenIntzRestoreRTHr = s_rawSetRenderTarget(device, 0u, previousRT);
            s_offscreenIntzApplyHr = apply(stateBlock);
        }

        ok = s_offscreenIntzTexture && s_offscreenIntzSurface &&
            SUCCEEDED(s_offscreenIntzCreateHr) && SUCCEEDED(s_offscreenIntzGetSurfaceHr) &&
            SUCCEEDED(s_offscreenIntzDescHr) && SUCCEEDED(s_offscreenIntzContainerHr) &&
            SUCCEEDED(s_offscreenIntzStateBlockHr) && SUCCEEDED(s_offscreenIntzCaptureHr) &&
            SUCCEEDED(s_offscreenIntzBindRTHr) && SUCCEEDED(s_offscreenIntzBindDSHr) &&
            SUCCEEDED(s_offscreenIntzClearHr) && SUCCEEDED(s_offscreenIntzRestoreDSHr) &&
            SUCCEEDED(s_offscreenIntzRestoreRTHr) && SUCCEEDED(s_offscreenIntzApplyHr) &&
            s_offscreenIntzActualRT == static_cast<uint32_t>(reinterpret_cast<uintptr_t>(s_fullResDepthSurface)) &&
            s_offscreenIntzActualDS == static_cast<uint32_t>(reinterpret_cast<uintptr_t>(s_offscreenIntzSurface));

    done:
        SafeReleaseCom(verifyDS);
        SafeReleaseCom(verifyRT);
        SafeReleaseCom(stateBlock);
        SafeReleaseCom(previousDS);
        SafeReleaseCom(previousRT);

        Event* event = ReserveEvent(EventType::OffscreenIntzTargetProbe);
        if (event)
        {
            D3DSurfaceDescLite intzDesc = {};
            D3DSurfaceDescLite rtDesc = {};
            const bool intzDescOk = s_offscreenIntzSurface && GetSurfaceDescSafe(s_offscreenIntzSurface, intzDesc);
            const bool rtDescOk = s_fullResDepthSurface && GetSurfaceDescSafe(s_fullResDepthSurface, rtDesc);
            event->a = s_presentCount;
            event->b = ok ? 1u : 0u;
            event->c = width;
            event->d = height;
            event->e = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(s_offscreenIntzTexture));
            event->f = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(s_offscreenIntzSurface));
            event->g = intzDescOk ? intzDesc.Format : 0u;
            event->h = s_offscreenIntzContainerTex;
            event->i = static_cast<uint32_t>(s_offscreenIntzCreateHr);
            event->j = static_cast<uint32_t>(s_offscreenIntzGetSurfaceHr);
            event->extra[0] = static_cast<uint32_t>(s_offscreenIntzDescHr);
            event->extra[1] = static_cast<uint32_t>(s_offscreenIntzContainerHr);
            event->extra[2] = (s_offscreenIntzContainerTex == static_cast<uint32_t>(
                reinterpret_cast<uintptr_t>(s_offscreenIntzTexture))) ? 1u : 0u;
            event->extra[3] = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(s_fullResDepthSurface));
            event->extra[4] = rtDescOk ? rtDesc.Format : 0u;
            event->extra[5] = static_cast<uint32_t>(s_offscreenIntzStateBlockHr);
            event->extra[6] = static_cast<uint32_t>(s_offscreenIntzCaptureHr);
            event->extra[7] = static_cast<uint32_t>(s_offscreenIntzBindRTHr);
            event->extra[8] = static_cast<uint32_t>(s_offscreenIntzBindDSHr);
            event->extra[9] = static_cast<uint32_t>(s_offscreenIntzClearHr);
            event->extra[10] = s_offscreenIntzActualRT;
            event->extra[11] = s_offscreenIntzActualDS;
            event->extra[12] = static_cast<uint32_t>(s_offscreenIntzRestoreDSHr);
            event->extra[13] = static_cast<uint32_t>(s_offscreenIntzRestoreRTHr);
            event->extra[14] = static_cast<uint32_t>(s_offscreenIntzApplyHr);
            event->extra[15] = scene;
        }

        InterlockedExchange(&s_offscreenIntzProbeState, ok ? 1 : -1);
        return ok;
    }


    bool PackMode4OpaqueIntzDepthForDof(void* device)
    {
        if (!device || !s_offscreenIntzTexture || !s_offscreenIntzSurface)
            return false;

        // GPU-only conversion of the proven scalar INTZ snapshot into the exact
        // packed A/R/G representation decoded by the dormant PC DOF shader.
        // This intentionally performs no GetRenderTargetData/CPU readback.
        const bool oldEnabled = s_mode4PackedDepthEnabled;
        const bool oldAuto = s_mode4PackAutoCycle;
        const uint32_t oldChannel = s_mode4PackChannel;
        void* oldTex = s_mode4ReplacementDepthTexture;
        void* oldSurf = s_mode4ReplacementDepthSurface;
        const LONG oldApplied = s_mode4DepthReplacementAppliedCount;

        s_mode4PackedDepthEnabled = true;
        s_mode4PackAutoCycle = false;
        s_mode4PackChannel = 0u;
        s_mode4ReplacementDepthTexture = s_offscreenIntzTexture;
        s_mode4ReplacementDepthSurface = s_offscreenIntzSurface;
        s_mode4DepthReplacementAppliedCount = 1;

        const bool packed = ApplyMode4PackedDepthPass(device);
        s_opaqueIntzPackHr = packed ? S_OK : E_FAIL;

        s_mode4ReplacementDepthTexture = oldTex;
        s_mode4ReplacementDepthSurface = oldSurf;
        s_mode4DepthReplacementAppliedCount = oldApplied;
        s_mode4PackChannel = oldChannel;
        s_mode4PackAutoCycle = oldAuto;
        s_mode4PackedDepthEnabled = oldEnabled;

        return packed && s_mode4PackedDepthSurface != nullptr;
    }

    bool SampleMode4OpaqueIntzReplayReadback(void* device)
    {
        // Never let a failed verification inherit stale statistics from a prior
        // frame. A failed/stalled readback must look invalid and keep DOF closed.
        s_opaqueIntzFinite = 0u;
        s_opaqueIntzChanged = 0u;
        s_opaqueIntzMin = 1.0f;
        s_opaqueIntzMax = 1.0f;
        s_opaqueIntzMean = 1.0f;
        s_opaqueIntzCreateReadbackHr = E_PENDING;
        s_opaqueIntzGetDataHr = E_PENDING;
        s_opaqueIntzLockHr = E_PENDING;
        s_opaqueIntzUnlockHr = E_PENDING;

        if (!device || !s_offscreenIntzTexture || !s_offscreenIntzSurface ||
            !s_mode4PackedDepthSurface || !s_rawCreateOffscreenPlainSurface ||
            !s_rawGetRenderTargetData)
            return false;

        if (!PackMode4OpaqueIntzDepthForDof(device))
            return false;

        void* readbackSurface = nullptr;
        s_opaqueIntzCreateReadbackHr = s_rawCreateOffscreenPlainSurface(
            device, s_offscreenIntzWidth, s_offscreenIntzHeight, kFormatA8R8G8B8,
            kD3DPoolSystemMem, &readbackSurface, nullptr);
        if (FAILED(s_opaqueIntzCreateReadbackHr) || !readbackSurface)
            return false;

        bool ok = false;
        s_opaqueIntzGetDataHr = s_rawGetRenderTargetData(
            device, s_mode4PackedDepthSurface, readbackSurface);
        if (SUCCEEDED(s_opaqueIntzGetDataHr))
        {
            void** vt = *reinterpret_cast<void***>(readbackSurface);
            if (vt && IsReadableMemory(vt,
                (kD3DSurfaceUnlockRectVtableIndex + 1u) * sizeof(void*)) &&
                IsExecutableMemory(vt[kD3DSurfaceLockRectVtableIndex]) &&
                IsExecutableMemory(vt[kD3DSurfaceUnlockRectVtableIndex]))
            {
                const auto lockRect = reinterpret_cast<D3DSurfaceLockRect_t>(
                    vt[kD3DSurfaceLockRectVtableIndex]);
                const auto unlockRect = reinterpret_cast<D3DSurfaceUnlockRect_t>(
                    vt[kD3DSurfaceUnlockRectVtableIndex]);
                D3DLockedRectLite locked = {};
                s_opaqueIntzLockHr = lockRect(
                    readbackSurface, &locked, nullptr, kD3DLockReadOnly);
                if (SUCCEEDED(s_opaqueIntzLockHr) && locked.pBits && locked.Pitch > 0)
                {
                    uint64_t finite = 0u;
                    uint64_t changed = 0u;
                    uint64_t belowNear = 0u;
                    uint64_t dofBand = 0u;
                    uint64_t aboveFar = 0u;
                    uint64_t below099 = 0u;
                    uint64_t below0995 = 0u;
                    uint64_t below0999 = 0u;
                    const float kXboxDofNearThreshold = s_mode4LastBloomDepthControl[1];
                    const float kXboxDofFarThreshold = s_mode4LastBloomDepthControl[3];
                    double sum = 0.0;
                    float minV = 1.0f;
                    float maxV = 0.0f;
                    for (uint32_t y = 0u; y < s_offscreenIntzHeight; ++y)
                    {
                        const uint8_t* row = reinterpret_cast<const uint8_t*>(locked.pBits) +
                            static_cast<size_t>(y) * static_cast<size_t>(locked.Pitch);
                        for (uint32_t x = 0u; x < s_offscreenIntzWidth; ++x)
                        {
                            const uint8_t* pixel = row + x * 4u;
                            const float v =
                                static_cast<float>(pixel[3]) / 256.0f +
                                static_cast<float>(pixel[2]) / 65536.0f +
                                static_cast<float>(pixel[1]) / 16777216.0f;
                            if (std::isfinite(v))
                            {
                                ++finite;
                                sum += static_cast<double>(v);
                                if (v < minV) minV = v;
                                if (v > maxV) maxV = v;
                                if (v < 0.99999f)
                                    ++changed;
                                if (v < kXboxDofNearThreshold)
                                    ++belowNear;
                                else if (v < kXboxDofFarThreshold)
                                    ++dofBand;
                                else
                                    ++aboveFar;
                                if (v < 0.99f) ++below099;
                                if (v < 0.995f) ++below0995;
                                if (v < 0.999f) ++below0999;
                            }
                        }
                    }
                    s_opaqueIntzFinite = static_cast<uint32_t>(finite > 0xFFFFFFFFull ? 0xFFFFFFFFull : finite);
                    s_opaqueIntzChanged = static_cast<uint32_t>(changed > 0xFFFFFFFFull ? 0xFFFFFFFFull : changed);
                    s_opaqueIntzMin = finite ? minV : 1.0f;
                    s_opaqueIntzMax = finite ? maxV : 1.0f;
                    s_opaqueIntzMean = finite ? static_cast<float>(sum / static_cast<double>(finite)) : 1.0f;
                    s_opaqueIntzDofBelowNear = static_cast<uint32_t>(belowNear > 0xFFFFFFFFull ? 0xFFFFFFFFull : belowNear);
                    s_opaqueIntzDofBand = static_cast<uint32_t>(dofBand > 0xFFFFFFFFull ? 0xFFFFFFFFull : dofBand);
                    s_opaqueIntzDofAboveFar = static_cast<uint32_t>(aboveFar > 0xFFFFFFFFull ? 0xFFFFFFFFull : aboveFar);
                    s_opaqueIntzDofBelow099 = static_cast<uint32_t>(below099 > 0xFFFFFFFFull ? 0xFFFFFFFFull : below099);
                    s_opaqueIntzDofBelow0995 = static_cast<uint32_t>(below0995 > 0xFFFFFFFFull ? 0xFFFFFFFFull : below0995);
                    s_opaqueIntzDofBelow0999 = static_cast<uint32_t>(below0999 > 0xFFFFFFFFull ? 0xFFFFFFFFull : below0999);
                    s_opaqueIntzUnlockHr = unlockRect(readbackSurface);
                    ok = SUCCEEDED(s_opaqueIntzUnlockHr) && finite != 0u;
                }
            }
        }

        SafeReleaseCom(readbackSurface);
        return ok;
    }


    bool RunMode4ReszDepthResolve(uint32_t scene)
    {
        void* device = reinterpret_cast<void*>(static_cast<uintptr_t>(s_rawDeviceValue));
        if (!device || scene < 0x10000u || !s_offscreenIntzTexture ||
            !s_offscreenIntzSurface || !s_rawCreateStateBlock ||
            !s_rawGetDepthStencilSurface || !s_rawSetDepthStencilSurface ||
            !s_rawSetTexture || !s_rawSetRenderState ||
            !s_rawSetVertexShader || !s_rawSetPixelShader || !s_rawSetFVF ||
            !s_rawDrawPrimitiveUP)
            return false;

        s_reszLiveDepthSurface = 0u;
        s_reszLiveDepthFormat = 0u;
        s_reszLiveDepthWidth = 0u;
        s_reszLiveDepthHeight = 0u;
        s_reszStage0Before = s_rawTextureStages[0];
        s_reszStage0CacheBefore = ReadU32(kNglTextureCacheBase);
        s_reszStage0CacheAfter = s_reszStage0CacheBefore;
        s_reszStateBlockHr = E_PENDING;
        s_reszCaptureHr = E_PENDING;
        s_reszClearBindHr = E_PENDING;
        s_reszClearHr = E_PENDING;
        s_reszRestoreLiveDepthHr = E_PENDING;
        s_reszBindIntzTextureHr = E_PENDING;
        s_reszDummySetupHr = E_PENDING;
        s_reszDummyDrawHr = E_PENDING;
        s_reszTriggerHr = E_PENDING;
        s_reszApplyHr = E_PENDING;
        s_reszReadbackOk = false;

        void* liveDepth = nullptr;
        HRESULT getDepthHr = s_rawGetDepthStencilSurface(device, &liveDepth);
        if (FAILED(getDepthHr) || !liveDepth)
            return false;

        D3DSurfaceDescLite liveDesc = {};
        if (!GetSurfaceDescSafe(liveDepth, liveDesc))
        {
            SafeReleaseCom(liveDepth);
            return false;
        }

        s_reszLiveDepthSurface = static_cast<uint32_t>(
            reinterpret_cast<uintptr_t>(liveDepth));
        s_reszLiveDepthFormat = liveDesc.Format;
        s_reszLiveDepthWidth = liveDesc.Width;
        s_reszLiveDepthHeight = liveDesc.Height;

        // Hard safety gate: resolve only SM3's full-resolution live D24S8
        // world depth into our same-size proven INTZ owner texture.
        if (liveDesc.Format != kFormatD24S8 ||
            liveDesc.Width != s_offscreenIntzWidth ||
            liveDesc.Height != s_offscreenIntzHeight)
        {
            SafeReleaseCom(liveDepth);
            return false;
        }

        void* stateBlock = nullptr;
        s_reszStateBlockHr = s_rawCreateStateBlock(
            device, kD3DStateBlockAll, &stateBlock);
        if (FAILED(s_reszStateBlockHr) || !stateBlock)
        {
            SafeReleaseCom(liveDepth);
            return false;
        }

        void** stateVtable = *reinterpret_cast<void***>(stateBlock);
        if (!stateVtable || !IsReadableMemory(stateVtable,
            (kD3DStateBlockApplyVtableIndex + 1u) * sizeof(void*)) ||
            !IsExecutableMemory(stateVtable[kD3DStateBlockCaptureVtableIndex]) ||
            !IsExecutableMemory(stateVtable[kD3DStateBlockApplyVtableIndex]))
        {
            SafeReleaseCom(stateBlock);
            SafeReleaseCom(liveDepth);
            return false;
        }
        const auto capture = reinterpret_cast<D3DStateBlockCapture_t>(
            stateVtable[kD3DStateBlockCaptureVtableIndex]);
        const auto apply = reinterpret_cast<D3DStateBlockApply_t>(
            stateVtable[kD3DStateBlockApplyVtableIndex]);

        s_reszCaptureHr = capture(stateBlock);
        if (FAILED(s_reszCaptureHr))
        {
            SafeReleaseCom(stateBlock);
            SafeReleaseCom(liveDepth);
            return false;
        }

        // Start from a known destination value so the readback can prove
        // whether RESZ actually copied the live game depth.
        s_reszClearBindHr = s_rawSetDepthStencilSurface(
            device, s_offscreenIntzSurface);
        if (SUCCEEDED(s_reszClearBindHr))
        {
            void** deviceVtable = *reinterpret_cast<void***>(device);
            if (deviceVtable && IsReadableMemory(deviceVtable,
                (kD3DClearVtableIndex + 1u) * sizeof(void*)) &&
                IsExecutableMemory(deviceVtable[kD3DClearVtableIndex]))
            {
                const auto clear = reinterpret_cast<D3DClear_t>(
                    deviceVtable[kD3DClearVtableIndex]);
                s_reszClearHr = clear(
                    device, 0u, nullptr, kD3DClearZBuffer, 0u, 1.0f, 0u);
            }
            else
            {
                s_reszClearHr = E_NOINTERFACE;
            }
        }
        else
        {
            s_reszClearHr = s_reszClearBindHr;
        }

        s_reszRestoreLiveDepthHr = s_rawSetDepthStencilSurface(device, liveDepth);

        HRESULT setupHr = S_OK;
        auto KeepFirstFailure = [&setupHr](HRESULT hr)
        {
            if (SUCCEEDED(setupHr) && FAILED(hr))
                setupHr = hr;
        };

        // ATI's historical RESZ contract binds the destination depth texture
        // to sampler 0. A no-output dummy draw serializes prior depth writes
        // before the POINTSIZE magic triggers the driver-side resolve.
        s_reszBindIntzTextureHr = s_rawSetTexture(
            device, 0u, s_offscreenIntzTexture);
        KeepFirstFailure(s_reszBindIntzTextureHr);
        KeepFirstFailure(s_rawSetRenderState(device, kRSZEnable, 0u));
        KeepFirstFailure(s_rawSetRenderState(device, kRSZWriteEnable, 0u));
        KeepFirstFailure(s_rawSetRenderState(device, kRSColorWriteEnable, 0u));
        KeepFirstFailure(s_rawSetVertexShader(device, nullptr));
        KeepFirstFailure(s_rawSetPixelShader(device, nullptr));
        KeepFirstFailure(s_rawSetFVF(device, kD3DFVFXYZRHW | kD3DFVFTex1));
        s_reszDummySetupHr = setupHr;

        const float right = static_cast<float>(s_offscreenIntzWidth) - 0.5f;
        const float bottom = static_cast<float>(s_offscreenIntzHeight) - 0.5f;
        const ExposureVertex dummyQuad[4] =
        {
            { -0.5f, -0.5f, 0.0f, 1.0f, 0.0f, 0.0f },
            { right, -0.5f, 0.0f, 1.0f, 1.0f, 0.0f },
            { -0.5f, bottom, 0.0f, 1.0f, 0.0f, 1.0f },
            { right, bottom, 0.0f, 1.0f, 1.0f, 1.0f }
        };
        if (SUCCEEDED(setupHr))
            s_reszDummyDrawHr = s_rawDrawPrimitiveUP(
                device, kD3DPrimitiveTriangleStrip, 2u, dummyQuad,
                static_cast<UINT>(sizeof(ExposureVertex)));
        else
            s_reszDummyDrawHr = setupHr;

        if (SUCCEEDED(setupHr) && SUCCEEDED(s_reszDummyDrawHr))
            s_reszTriggerHr = s_rawSetRenderState(
                device, kRSPointSize, kReszResolveMagic);
        else
            s_reszTriggerHr = FAILED(setupHr) ? setupHr : s_reszDummyDrawHr;

        s_reszApplyHr = apply(stateBlock);

        // State blocks restore the actual D3D texture but not xeSM3/NGL's
        // software mirrors because the calls above bypass the observation hook.
        s_rawTextureStages[0] = s_reszStage0Before;
        WriteU32(kNglTextureCacheBase, s_reszStage0CacheBefore);
        s_reszStage0CacheAfter = ReadU32(kNglTextureCacheBase);

        SafeReleaseCom(stateBlock);
        SafeReleaseCom(liveDepth);

        // V10.5.49.10.6: pack on GPU every frame. A CPU readback is requested
        // only while depth is untrusted or when the 120-present revalidation
        // interval expires. There is still no geometry replay anywhere here.
        bool packedDepthReady = false;
        if (SUCCEEDED(s_reszClearBindHr) &&
            SUCCEEDED(s_reszClearHr) &&
            SUCCEEDED(s_reszRestoreLiveDepthHr) &&
            SUCCEEDED(s_reszBindIntzTextureHr) &&
            SUCCEEDED(s_reszDummySetupHr) &&
            SUCCEEDED(s_reszDummyDrawHr) &&
            SUCCEEDED(s_reszTriggerHr) &&
            SUCCEEDED(s_reszApplyHr))
        {
            if (s_opaqueIntzDiagnosticReadbackThisFrame)
            {
                s_reszReadbackOk = SampleMode4OpaqueIntzReplayReadback(device);
                packedDepthReady = s_reszReadbackOk;
            }
            else
            {
                packedDepthReady = PackMode4OpaqueIntzDepthForDof(device);
                s_reszReadbackOk = packedDepthReady;
            }
        }

        const uint32_t expectedDepthPixels =
            s_offscreenIntzWidth * s_offscreenIntzHeight;
        const bool verifiedNonClearDepth =
            s_opaqueIntzDiagnosticReadbackThisFrame &&
            s_reszReadbackOk &&
            s_opaqueIntzFinite == expectedDepthPixels &&
            s_opaqueIntzChanged > 1000u &&
            s_opaqueIntzMin < 0.99999f &&
            std::isfinite(s_opaqueIntzMean) &&
            s_opaqueIntzMean < 0.999999f;

        if (s_opaqueIntzDiagnosticReadbackThisFrame)
        {
            s_mode4ReszDepthValidated = verifiedNonClearDepth;
            s_mode4ReszLastValidationPresent = s_presentCount;
        }

        // Critical V10.5.49.10.6 rule: HRESULT success is not depth proof.
        // DOF can arm only after a real non-clear packed-depth verification.
        const bool copiedDepth = packedDepthReady &&
            s_mode4PackedDepthTexture != nullptr &&
            s_mode4ReszDepthValidated;

        Event* event = nullptr;
        const bool logFirstReszSuccess = copiedDepth && s_provenIntzDofWindowProduced == 0u;
        const bool logSparseReszFailure = !copiedDepth &&
            ShouldLogSparse(static_cast<LONG>(s_provenIntzDofWindowFailures + 1u));
        if (logFirstReszSuccess || logSparseReszFailure)
            event = ReserveEvent(EventType::ReszDepthResolveProbe);
        if (event)
        {
            event->a = s_presentCount;
            event->b = copiedDepth ? 1u : 0u;
            event->c = scene;
            event->d = s_reszLiveDepthSurface;
            event->e = s_reszLiveDepthFormat;
            event->f = s_reszLiveDepthWidth;
            event->g = s_reszLiveDepthHeight;
            event->h = static_cast<uint32_t>(
                reinterpret_cast<uintptr_t>(s_offscreenIntzTexture));
            event->i = s_opaqueIntzFinite;
            event->j = s_opaqueIntzChanged;
            event->extra[0] = FloatBits(s_opaqueIntzMin);
            event->extra[1] = FloatBits(s_opaqueIntzMax);
            event->extra[2] = FloatBits(s_opaqueIntzMean);
            event->extra[3] = static_cast<uint32_t>(s_reszStateBlockHr);
            event->extra[4] = static_cast<uint32_t>(s_reszCaptureHr);
            event->extra[5] = static_cast<uint32_t>(s_reszClearBindHr);
            event->extra[6] = static_cast<uint32_t>(s_reszClearHr);
            event->extra[7] = static_cast<uint32_t>(s_reszRestoreLiveDepthHr);
            event->extra[8] = static_cast<uint32_t>(s_reszBindIntzTextureHr);
            event->extra[9] = static_cast<uint32_t>(s_reszDummySetupHr);
            event->extra[10] = static_cast<uint32_t>(s_reszDummyDrawHr);
            event->extra[11] = static_cast<uint32_t>(s_reszTriggerHr);
            event->extra[12] = static_cast<uint32_t>(s_reszApplyHr);
            event->extra[13] = s_reszStage0Before;
            event->extra[14] = s_reszStage0CacheBefore;
            event->extra[15] = s_reszStage0CacheAfter;
        }

        if (copiedDepth && kMode4ReszDofVisualWindowEnabled)
        {
            ++s_provenIntzDofWindowProduced;
            if (InterlockedCompareExchange(&s_provenIntzDofState, 1, 0) == 0)
            {
                s_provenIntzDofPresent = s_presentCount;
                s_provenIntzDofDepthTexture = static_cast<uint32_t>(
                    reinterpret_cast<uintptr_t>(s_mode4PackedDepthTexture));
            }
            else
            {
                ++s_provenIntzDofWindowFailures;
            }
        }
        else if (!copiedDepth && kMode4ReszDofVisualWindowEnabled)
        {
            ++s_provenIntzDofWindowFailures;
        }

        return copiedDepth;
    }

    bool ResolveRetailXboxDepthApisNoDetours()
    {
        if (!IsXboxPostProcessRoute())
            return false;

        const uint32_t deviceValue = ReadU32(kD3DDeviceGlobal);
        if (deviceValue < 0x10000u)
            return false;
        if (s_retailXboxDepthApiState == 1 && s_rawDeviceValue == deviceValue)
            return true;
        if (s_retailXboxDepthApiState == 1 && s_rawDeviceValue != deviceValue)
            InterlockedExchange(&s_retailXboxDepthApiState, 0);
        if (s_retailXboxDepthApiState == -1)
            return false;

        void* device = reinterpret_cast<void*>(static_cast<uintptr_t>(deviceValue));
        if (!IsReadableMemory(device, sizeof(void*)))
            return false;
        void** vtable = *reinterpret_cast<void***>(device);
        if (!vtable || !IsReadableMemory(vtable,
                (kD3DSetPixelShaderConstantFVtableIndex + 1u) * sizeof(void*)))
            return false;

        const uint32_t required[] = {
            kD3DCreateTextureVtableIndex,
            kD3DGetRenderTargetDataVtableIndex,
            kD3DCreateOffscreenPlainSurfaceVtableIndex,
            kD3DCreateDepthStencilSurfaceVtableIndex,
            kD3DSetRenderTargetVtableIndex,
            kD3DGetRenderTargetVtableIndex,
            kD3DSetDepthStencilSurfaceVtableIndex,
            kD3DGetDepthStencilSurfaceVtableIndex,
            kD3DSetViewportVtableIndex,
            kD3DSetRenderStateVtableIndex,
            kD3DGetRenderStateVtableIndex,
            kD3DCreateStateBlockVtableIndex,
            kD3DSetTextureVtableIndex,
            kD3DSetSamplerStateVtableIndex,
            kD3DDrawPrimitiveUPVtableIndex,
            kD3DSetFVFVtableIndex,
            kD3DSetVertexShaderVtableIndex,
            kD3DSetVertexShaderConstantFVtableIndex,
            kD3DCreatePixelShaderVtableIndex,
            kD3DSetPixelShaderVtableIndex,
            kD3DSetPixelShaderConstantFVtableIndex
        };
        for (uint32_t index : required)
        {
            if (!IsExecutableMemory(vtable[index]))
            {
                InterlockedExchange(&s_retailXboxDepthApiState, -1);
                return false;
            }
        }

        s_rawDeviceValue = deviceValue;
        s_rawCreateTexture = reinterpret_cast<D3DCreateTexture_t>(vtable[kD3DCreateTextureVtableIndex]);
        s_rawGetRenderTargetData = reinterpret_cast<D3DGetRenderTargetData_t>(vtable[kD3DGetRenderTargetDataVtableIndex]);
        s_rawCreateOffscreenPlainSurface = reinterpret_cast<D3DCreateOffscreenPlainSurface_t>(vtable[kD3DCreateOffscreenPlainSurfaceVtableIndex]);
        s_rawCreateDepthStencilSurface = reinterpret_cast<D3DCreateDepthStencilSurface_t>(vtable[kD3DCreateDepthStencilSurfaceVtableIndex]);
        s_rawSetRenderTarget = reinterpret_cast<D3DSetRenderTarget_t>(vtable[kD3DSetRenderTargetVtableIndex]);
        s_rawGetRenderTarget = reinterpret_cast<D3DGetRenderTarget_t>(vtable[kD3DGetRenderTargetVtableIndex]);
        s_rawSetDepthStencilSurface = reinterpret_cast<D3DSetDepthStencilSurface_t>(vtable[kD3DSetDepthStencilSurfaceVtableIndex]);
        s_rawGetDepthStencilSurface = reinterpret_cast<D3DGetDepthStencilSurface_t>(vtable[kD3DGetDepthStencilSurfaceVtableIndex]);
        s_rawSetViewport = reinterpret_cast<D3DSetViewport_t>(vtable[kD3DSetViewportVtableIndex]);
        s_rawSetRenderState = reinterpret_cast<D3DSetRenderState_t>(vtable[kD3DSetRenderStateVtableIndex]);
        s_rawGetRenderState = reinterpret_cast<D3DGetRenderState_t>(vtable[kD3DGetRenderStateVtableIndex]);
        s_rawCreateStateBlock = reinterpret_cast<D3DCreateStateBlock_t>(vtable[kD3DCreateStateBlockVtableIndex]);
        s_rawSetTexture = reinterpret_cast<D3DSetTexture_t>(vtable[kD3DSetTextureVtableIndex]);
        s_rawSetSamplerState = reinterpret_cast<D3DSetSamplerState_t>(vtable[kD3DSetSamplerStateVtableIndex]);
        s_rawDrawPrimitiveUP = reinterpret_cast<D3DDrawPrimitiveUP_t>(vtable[kD3DDrawPrimitiveUPVtableIndex]);
        s_rawSetFVF = reinterpret_cast<D3DSetFVF_t>(vtable[kD3DSetFVFVtableIndex]);
        s_rawSetVertexShader = reinterpret_cast<D3DSetVertexShader_t>(vtable[kD3DSetVertexShaderVtableIndex]);
        s_rawSetVertexShaderConstantF = reinterpret_cast<D3DSetVertexShaderConstantF_t>(
            vtable[kD3DSetVertexShaderConstantFVtableIndex]);
        s_rawCreatePixelShader = reinterpret_cast<D3DCreatePixelShader_t>(vtable[kD3DCreatePixelShaderVtableIndex]);
        s_rawSetPixelShader = reinterpret_cast<D3DSetPixelShader_t>(vtable[kD3DSetPixelShaderVtableIndex]);
        s_rawSetPixelShaderConstantF = reinterpret_cast<D3DSetPixelShaderConstantF_t>(vtable[kD3DSetPixelShaderConstantFVtableIndex]);

        InterlockedExchange(&s_retailXboxDepthApiState, 1);
        return true;
    }

    bool EnsureRetailXboxIntzDepthResource(uint32_t scene)
    {
        if (!IsXboxPostProcessRoute() || scene < 0x10000u ||
            !IsReadableMemory(reinterpret_cast<void*>(static_cast<uintptr_t>(scene)), 0x320u) ||
            !ResolveRetailXboxDepthApisNoDetours() || !s_rawCreateTexture)
            return false;

        const uint32_t width = ReadU32(static_cast<uintptr_t>(scene + kSceneTargetWidthOffset));
        const uint32_t height = ReadU32(static_cast<uintptr_t>(scene + kSceneTargetHeightOffset));
        if (width == 0u || height == 0u)
            return false;

        if (s_offscreenIntzTexture && s_offscreenIntzSurface &&
            s_offscreenIntzWidth == width && s_offscreenIntzHeight == height)
        {
            D3DSurfaceDescLite desc = {};
            if (GetSurfaceDescSafe(s_offscreenIntzSurface, desc) &&
                desc.Format == kFormatINTZ && desc.Width == width && desc.Height == height)
                return true;
        }

        SafeReleaseCom(s_offscreenIntzSurface);
        SafeReleaseCom(s_offscreenIntzTexture);
        s_offscreenIntzWidth = 0u;
        s_offscreenIntzHeight = 0u;

        void* device = reinterpret_cast<void*>(static_cast<uintptr_t>(s_rawDeviceValue));
        void* texture = nullptr;
        s_offscreenIntzCreateHr = s_rawCreateTexture(
            device, width, height, 1u, kD3DUsageDepthStencil,
            kFormatINTZ, kD3DPoolDefault, &texture, nullptr);
        if (FAILED(s_offscreenIntzCreateHr) || !texture)
            return false;

        void** textureVtable = IsReadableMemory(texture, sizeof(void*))
            ? *reinterpret_cast<void***>(texture) : nullptr;
        if (!textureVtable || !IsReadableMemory(textureVtable,
                (kD3DTextureGetSurfaceLevelVtableIndex + 1u) * sizeof(void*)) ||
            !IsExecutableMemory(textureVtable[kD3DTextureGetSurfaceLevelVtableIndex]))
        {
            s_offscreenIntzGetSurfaceHr = E_NOINTERFACE;
            SafeReleaseCom(texture);
            return false;
        }

        void* surface = nullptr;
        const auto getSurfaceLevel = reinterpret_cast<D3DTextureGetSurfaceLevel_t>(
            textureVtable[kD3DTextureGetSurfaceLevelVtableIndex]);
        s_offscreenIntzGetSurfaceHr = getSurfaceLevel(texture, 0u, &surface);
        if (FAILED(s_offscreenIntzGetSurfaceHr) || !surface)
        {
            SafeReleaseCom(texture);
            return false;
        }

        D3DSurfaceDescLite desc = {};
        s_offscreenIntzDescHr = GetSurfaceDescSafe(surface, desc) ? S_OK : E_FAIL;
        const uint32_t surfacePtr = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(surface));
        s_offscreenIntzContainerTex = RawTextureFromSurface(surfacePtr, &s_offscreenIntzContainerHr);
        const uint32_t texturePtr = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(texture));
        if (FAILED(s_offscreenIntzDescHr) || desc.Format != kFormatINTZ ||
            desc.Width != width || desc.Height != height ||
            FAILED(s_offscreenIntzContainerHr) || s_offscreenIntzContainerTex != texturePtr)
        {
            SafeReleaseCom(surface);
            SafeReleaseCom(texture);
            return false;
        }

        s_offscreenIntzTexture = texture;
        s_offscreenIntzSurface = surface;
        s_offscreenIntzWidth = width;
        s_offscreenIntzHeight = height;
        return true;
    }

    void __cdecl Mode4_ReszDepthResolveMid(void* userData)
    {
        (void)userData;
        const uint32_t scene = ReadU32(kNGLCurrentSceneGlobal);

        if (s_reszOriginalMid)
        {
            const auto originalMid = reinterpret_cast<NGLSceneCallback_t>(
                static_cast<uintptr_t>(s_reszOriginalMid));
            originalMid(reinterpret_cast<void*>(
                static_cast<uintptr_t>(s_reszOriginalMidData)));
        }

        if (scene == s_reszDepthResolveScene &&
            InterlockedCompareExchange(&s_reszDepthResolveState, 2, 1) == 1)
        {
            const bool ok = RunMode4ReszDepthResolve(scene);
            InterlockedExchange(&s_reszDepthResolveState, ok ? 3 : -1);
        }
    }

    bool RunMode4OpaqueIntzReplay(uint32_t scene)
    {
        void* device = reinterpret_cast<void*>(static_cast<uintptr_t>(s_rawDeviceValue));
        if (!device || scene < 0x10000u ||
            !IsReadableMemory(reinterpret_cast<void*>(static_cast<uintptr_t>(scene)), 0x320u) ||
            !s_offscreenIntzSurface || !s_mode4PackedDepthSurface ||
            !s_rawGetRenderTarget || !s_rawGetDepthStencilSurface ||
            !s_rawSetRenderTarget || !s_rawSetDepthStencilSurface ||
            !s_rawCreateStateBlock || !s_nglBeginRenderNode || !s_nglAdvanceRenderNode)
            return false;

        const uint32_t opaqueHead = ReadU32(static_cast<uintptr_t>(scene + kSceneOpaqueRenderListOffset));
        const uint32_t opaqueCount = ReadU32(static_cast<uintptr_t>(scene + kSceneOpaqueListCountOffset));
        const uint32_t sentinel = ReadU32(kRenderListSentinelGlobal);
        s_opaqueIntzReplayDeclared = opaqueCount;
        s_opaqueIntzReplayCount = 0u;
        s_opaqueIntzReplayFirstNode = 0u;
        s_opaqueIntzReplayLastNode = 0u;
        s_opaqueIntzReplayBadNode = 0u;
        s_opaqueIntzTraversalStartIndex = 0u;
        s_opaqueIntzTraversalFinalIndex = 0u;
        s_opaqueIntzTraversalFinalNode = 0u;
        s_opaqueIntzTraversalInternalConsumed = 0u;
        s_opaqueIntzTraversalSentinelReached = 0u;
        s_opaqueIntzDofBelowNear = 0u;
        s_opaqueIntzDofBand = 0u;
        s_opaqueIntzDofAboveFar = 0u;
        s_opaqueIntzDofBelow099 = 0u;
        s_opaqueIntzDofBelow0995 = 0u;
        s_opaqueIntzDofBelow0999 = 0u;

        if (opaqueHead < 0x10000u || sentinel == 0u || opaqueCount == 0u || opaqueCount > 4096u)
            return false;

        void* previousRT = nullptr;
        void* previousDS = nullptr;
        void* stateBlock = nullptr;
        if (FAILED(s_rawGetRenderTarget(device, 0u, &previousRT)) || !previousRT)
            goto done;
        {
            const HRESULT dsHr = s_rawGetDepthStencilSurface(device, &previousDS);
            if (FAILED(dsHr) && dsHr != kD3DErrNotFound)
                goto done;
        }

        s_opaqueIntzStateBlockHr = s_rawCreateStateBlock(device, kD3DStateBlockAll, &stateBlock);
        if (FAILED(s_opaqueIntzStateBlockHr) || !stateBlock)
            goto done;

        {
            void** stateVtable = *reinterpret_cast<void***>(stateBlock);
            if (!stateVtable || !IsReadableMemory(stateVtable,
                (kD3DStateBlockApplyVtableIndex + 1u) * sizeof(void*)) ||
                !IsExecutableMemory(stateVtable[kD3DStateBlockCaptureVtableIndex]) ||
                !IsExecutableMemory(stateVtable[kD3DStateBlockApplyVtableIndex]))
                goto done;
            const auto capture = reinterpret_cast<D3DStateBlockCapture_t>(
                stateVtable[kD3DStateBlockCaptureVtableIndex]);
            const auto apply = reinterpret_cast<D3DStateBlockApply_t>(
                stateVtable[kD3DStateBlockApplyVtableIndex]);

            s_opaqueIntzCaptureHr = capture(stateBlock);
            if (FAILED(s_opaqueIntzCaptureHr))
                goto done;

            // Use the proven full-resolution A8R8G8B8 packed-depth surface as a
            // dummy color RT so every normal material sees a stock-compatible
            // color format while hardware Z is written into INTZ.
            s_opaqueIntzBindRTHr =
                s_rawSetRenderTarget(device, 0u, s_mode4PackedDepthSurface);
            s_opaqueIntzBindDSHr = SUCCEEDED(s_opaqueIntzBindRTHr)
                ? s_rawSetDepthStencilSurface(device, s_offscreenIntzSurface)
                : s_opaqueIntzBindRTHr;

            if (SUCCEEDED(s_opaqueIntzBindRTHr) && SUCCEEDED(s_opaqueIntzBindDSHr))
            {
                void** deviceVtable = *reinterpret_cast<void***>(device);
                if (deviceVtable && IsReadableMemory(deviceVtable,
                    (kD3DClearVtableIndex + 1u) * sizeof(void*)) &&
                    IsExecutableMemory(deviceVtable[kD3DClearVtableIndex]))
                {
                    const auto clear = reinterpret_cast<D3DClear_t>(
                        deviceVtable[kD3DClearVtableIndex]);
                    s_opaqueIntzClearHr = clear(
                        device, 0u, nullptr, kD3DClearTarget | kD3DClearZBuffer,
                        0u, 1.0f, 0u);
                }
                else
                {
                    s_opaqueIntzClearHr = E_NOINTERFACE;
                }
            }
            else
            {
                s_opaqueIntzClearHr = FAILED(s_opaqueIntzBindRTHr)
                    ? s_opaqueIntzBindRTHr : s_opaqueIntzBindDSHr;
            }

            if (SUCCEEDED(s_opaqueIntzClearHr))
            {
                // Preserve Treyarch's render-list traversal globals.  The stock
                // nglBeginRenderNode/nglAdvanceRenderNode pair uses one global
                // frame index across all lists, so reset only inside this
                // offscreen replay and restore it before transparent rendering.
                const uint32_t savedCur = ReadU32(kNGLCurRenderNodeGlobal);
                const uint32_t savedPrev = ReadU32(kNGLPrevRenderNodeGlobal);
                const uint32_t savedRangeStart = ReadU32(kNGLRenderRangeStartGlobal);
                const uint32_t savedRangeEnd = ReadU32(kNGLRenderRangeEndGlobal);
                const uint32_t savedIndex = ReadU32(kNGLRenderIndexGlobal);
                const uint8_t savedShadowFlag = ReadU8(kShadowRenderPassFlagGlobal);

                WriteU32(kNGLCurRenderNodeGlobal, 0u);
                WriteU32(kNGLPrevRenderNodeGlobal, 0u);
                WriteU32(kNGLRenderRangeStartGlobal, 0u);
                WriteU32(kNGLRenderRangeEndGlobal, 100000u);
                WriteU32(kNGLRenderIndexGlobal, 0u);
                WriteU8(kShadowRenderPassFlagGlobal, 0u);

                s_nglBeginRenderNode(static_cast<int>(opaqueHead));
                for (uint32_t guard = 0u; guard < opaqueCount + 8u; ++guard)
                {
                    const uint32_t node = ReadU32(kNGLCurRenderNodeGlobal);
                    if (node == sentinel)
                        break;
                    if (node < 0x10000u ||
                        !IsReadableMemory(reinterpret_cast<void*>(static_cast<uintptr_t>(node)), 8u))
                    {
                        s_opaqueIntzReplayBadNode = node;
                        break;
                    }
                    const uint32_t vtable = ReadU32(static_cast<uintptr_t>(node));
                    if (vtable < 0x10000u ||
                        !IsReadableMemory(reinterpret_cast<void*>(static_cast<uintptr_t>(vtable + 8u)), sizeof(uint32_t)))
                    {
                        s_opaqueIntzReplayBadNode = node;
                        break;
                    }
                    const uint32_t render = ReadU32(static_cast<uintptr_t>(vtable + 8u));
                    if (render < 0x10000u ||
                        !IsExecutableMemory(reinterpret_cast<void*>(static_cast<uintptr_t>(render))))
                    {
                        s_opaqueIntzReplayBadNode = node;
                        break;
                    }

                    if (s_opaqueIntzReplayFirstNode == 0u)
                        s_opaqueIntzReplayFirstNode = node;
                    s_opaqueIntzReplayLastNode = node;
                    const auto renderFn = reinterpret_cast<NGLNodeRender_t>(
                        static_cast<uintptr_t>(render));
                    renderFn(reinterpret_cast<void*>(static_cast<uintptr_t>(node)));
                    ++s_opaqueIntzReplayCount;
                    s_nglAdvanceRenderNode();
                }

                s_opaqueIntzTraversalStartIndex = 0u;
                s_opaqueIntzTraversalFinalIndex = ReadU32(kNGLRenderIndexGlobal);
                s_opaqueIntzTraversalFinalNode = ReadU32(kNGLCurRenderNodeGlobal);
                s_opaqueIntzTraversalSentinelReached =
                    (s_opaqueIntzTraversalFinalNode == sentinel) ? 1u : 0u;
                s_opaqueIntzTraversalInternalConsumed =
                    (s_opaqueIntzTraversalFinalIndex >= s_opaqueIntzReplayCount)
                        ? (s_opaqueIntzTraversalFinalIndex - s_opaqueIntzReplayCount)
                        : 0u;

                WriteU8(kShadowRenderPassFlagGlobal, savedShadowFlag);
                WriteU32(kNGLRenderIndexGlobal, savedIndex);
                WriteU32(kNGLRenderRangeEndGlobal, savedRangeEnd);
                WriteU32(kNGLRenderRangeStartGlobal, savedRangeStart);
                WriteU32(kNGLPrevRenderNodeGlobal, savedPrev);
                WriteU32(kNGLCurRenderNodeGlobal, savedCur);
            }

            s_opaqueIntzRestoreDSHr = s_rawSetDepthStencilSurface(device, previousDS);
            s_opaqueIntzRestoreRTHr = s_rawSetRenderTarget(device, 0u, previousRT);
            s_opaqueIntzApplyHr = apply(stateBlock);
        }

        {
            const bool diagnostic = s_opaqueIntzDiagnosticReadbackThisFrame;
            const bool packed = diagnostic
                ? SampleMode4OpaqueIntzReplayReadback(device)
                : PackMode4OpaqueIntzDepthForDof(device);

            if (diagnostic)
            {
                Event* event = ReserveEvent(EventType::OpaqueIntzReplayProbe);
                if (event)
                {
                    event->a = s_presentCount;
                    event->b = (packed && s_opaqueIntzReplayCount > 0u &&
                        s_opaqueIntzReplayBadNode == 0u) ? 1u : 0u;
                    event->c = scene;
                    event->d = s_opaqueIntzReplayDeclared;
                    event->e = s_opaqueIntzReplayCount;
                    event->f = s_opaqueIntzReplayFirstNode;
                    event->g = s_opaqueIntzReplayLastNode;
                    event->h = s_opaqueIntzReplayBadNode;
                    event->i = s_opaqueIntzFinite;
                    event->j = s_opaqueIntzChanged;
                    memcpy(&event->extra[0], &s_opaqueIntzMin, sizeof(float));
                    memcpy(&event->extra[1], &s_opaqueIntzMax, sizeof(float));
                    memcpy(&event->extra[2], &s_opaqueIntzMean, sizeof(float));
                    event->extra[3] = static_cast<uint32_t>(s_opaqueIntzStateBlockHr);
                    event->extra[4] = static_cast<uint32_t>(s_opaqueIntzCaptureHr);
                    event->extra[5] = static_cast<uint32_t>(s_opaqueIntzBindRTHr);
                    event->extra[6] = static_cast<uint32_t>(s_opaqueIntzBindDSHr);
                    event->extra[7] = static_cast<uint32_t>(s_opaqueIntzClearHr);
                    event->extra[8] = static_cast<uint32_t>(s_opaqueIntzRestoreDSHr);
                    event->extra[9] = static_cast<uint32_t>(s_opaqueIntzRestoreRTHr);
                    event->extra[10] = static_cast<uint32_t>(s_opaqueIntzApplyHr);
                    event->extra[11] = static_cast<uint32_t>(s_opaqueIntzPackHr);
                    event->extra[12] = static_cast<uint32_t>(s_opaqueIntzCreateReadbackHr);
                    event->extra[13] = static_cast<uint32_t>(s_opaqueIntzGetDataHr);
                    event->extra[14] = static_cast<uint32_t>(s_opaqueIntzLockHr);
                    event->extra[15] = static_cast<uint32_t>(s_opaqueIntzUnlockHr);
                }

                Event* traversalEvent = ReserveEvent(EventType::OpaqueIntzTraversalProof);
                if (traversalEvent)
                {
                    traversalEvent->a = s_presentCount;
                    traversalEvent->b = scene;
                    traversalEvent->c = s_opaqueIntzReplayDeclared;
                    traversalEvent->d = s_opaqueIntzReplayCount;
                    traversalEvent->e = s_opaqueIntzTraversalStartIndex;
                    traversalEvent->f = s_opaqueIntzTraversalFinalIndex;
                    traversalEvent->g = s_opaqueIntzTraversalInternalConsumed;
                    traversalEvent->h = s_opaqueIntzTraversalSentinelReached;
                    traversalEvent->i = s_opaqueIntzTraversalFinalNode;
                    traversalEvent->j = sentinel;
                }

                Event* semanticEvent = ReserveEvent(EventType::OpaqueIntzDofSemanticProbe);
                if (semanticEvent)
                {
                    const float kXboxDofNearThreshold = s_mode4LastBloomDepthControl[1];
                    const float kXboxDofFarThreshold = s_mode4LastBloomDepthControl[3];
                    semanticEvent->a = s_presentCount;
                    semanticEvent->b = scene;
                    semanticEvent->c = s_opaqueIntzFinite;
                    semanticEvent->d = s_opaqueIntzChanged;
                    semanticEvent->e = s_opaqueIntzDofBelowNear;
                    semanticEvent->f = s_opaqueIntzDofBand;
                    semanticEvent->g = s_opaqueIntzDofAboveFar;
                    memcpy(&semanticEvent->h, &kXboxDofNearThreshold, sizeof(float));
                    memcpy(&semanticEvent->i, &kXboxDofFarThreshold, sizeof(float));
                    semanticEvent->j = s_opaqueIntzDofBelow099;
                    semanticEvent->extra[0] = s_opaqueIntzDofBelow0995;
                    semanticEvent->extra[1] = s_opaqueIntzDofBelow0999;
                    memcpy(&semanticEvent->extra[2], &s_opaqueIntzMin, sizeof(float));
                    memcpy(&semanticEvent->extra[3], &s_opaqueIntzMax, sizeof(float));
                    memcpy(&semanticEvent->extra[4], &s_opaqueIntzMean, sizeof(float));
                    semanticEvent->extra[5] =
                        (s_opaqueIntzTraversalFinalIndex == s_opaqueIntzReplayDeclared &&
                         s_opaqueIntzTraversalSentinelReached != 0u) ? 1u : 0u;
                }
            }

            SafeReleaseCom(stateBlock);
            SafeReleaseCom(previousDS);
            SafeReleaseCom(previousRT);

            const bool producerClosed = packed && s_opaqueIntzReplayCount > 0u &&
                s_opaqueIntzReplayBadNode == 0u &&
                s_opaqueIntzTraversalFinalIndex == s_opaqueIntzReplayDeclared &&
                s_opaqueIntzTraversalSentinelReached != 0u &&
                s_mode4PackedDepthTexture != nullptr &&
                (!diagnostic || s_opaqueIntzChanged > 0u);

            if (producerClosed)
            {
                ++s_provenIntzDofWindowProduced;
                if (InterlockedCompareExchange(&s_provenIntzDofState, 1, 0) == 0)
                {
                    s_provenIntzDofPresent = s_presentCount;
                    s_provenIntzDofDepthTexture = static_cast<uint32_t>(
                        reinterpret_cast<uintptr_t>(s_mode4PackedDepthTexture));
                }
            }
            else
            {
                ++s_provenIntzDofWindowFailures;
            }
            return producerClosed;
        }

    done:
        {
            Event* event = ReserveEvent(EventType::OpaqueIntzReplayProbe);
            if (event)
            {
                event->a = s_presentCount;
                event->b = 0u;
                event->c = scene;
                event->d = s_opaqueIntzReplayDeclared;
                event->e = s_opaqueIntzReplayCount;
                event->f = s_opaqueIntzReplayFirstNode;
                event->g = s_opaqueIntzReplayLastNode;
                event->h = s_opaqueIntzReplayBadNode;
                event->i = s_opaqueIntzFinite;
                event->j = s_opaqueIntzChanged;
                memcpy(&event->extra[0], &s_opaqueIntzMin, sizeof(float));
                memcpy(&event->extra[1], &s_opaqueIntzMax, sizeof(float));
                memcpy(&event->extra[2], &s_opaqueIntzMean, sizeof(float));
                event->extra[3] = static_cast<uint32_t>(s_opaqueIntzStateBlockHr);
                event->extra[4] = static_cast<uint32_t>(s_opaqueIntzCaptureHr);
                event->extra[5] = static_cast<uint32_t>(s_opaqueIntzBindRTHr);
                event->extra[6] = static_cast<uint32_t>(s_opaqueIntzBindDSHr);
                event->extra[7] = static_cast<uint32_t>(s_opaqueIntzClearHr);
                event->extra[8] = static_cast<uint32_t>(s_opaqueIntzRestoreDSHr);
                event->extra[9] = static_cast<uint32_t>(s_opaqueIntzRestoreRTHr);
                event->extra[10] = static_cast<uint32_t>(s_opaqueIntzApplyHr);
                event->extra[11] = static_cast<uint32_t>(s_opaqueIntzPackHr);
                event->extra[12] = static_cast<uint32_t>(s_opaqueIntzCreateReadbackHr);
                event->extra[13] = static_cast<uint32_t>(s_opaqueIntzGetDataHr);
                event->extra[14] = static_cast<uint32_t>(s_opaqueIntzLockHr);
                event->extra[15] = static_cast<uint32_t>(s_opaqueIntzUnlockHr);
            }
        }
        SafeReleaseCom(stateBlock);
        SafeReleaseCom(previousDS);
        SafeReleaseCom(previousRT);
        return false;
    }

    void __cdecl Mode4_OpaqueIntzReplayMid(void* userData)
    {
        (void)userData;
        const uint32_t scene = ReadU32(kNGLCurrentSceneGlobal);

        if (s_opaqueIntzOriginalMid)
        {
            const auto originalMid = reinterpret_cast<NGLSceneCallback_t>(
                static_cast<uintptr_t>(s_opaqueIntzOriginalMid));
            originalMid(reinterpret_cast<void*>(
                static_cast<uintptr_t>(s_opaqueIntzOriginalMidData)));
        }

        if (scene == s_opaqueIntzReplayScene &&
            InterlockedCompareExchange(&s_opaqueIntzReplayState, 2, 1) == 1)
        {
            const bool ok = RunMode4OpaqueIntzReplay(scene);
            InterlockedExchange(&s_opaqueIntzReplayState, ok ? 3 : -1);
        }
    }

    int __cdecl NGLRenderScene_Hook()
    {
        const LONG depth = s_nglRenderSceneHookDepth++;
        const uint32_t scene = ReadU32(kNGLCurrentSceneGlobal);
        const uint32_t parentScene = (depth > 0 && depth <= 64) ?
            s_sceneTreeProfileSceneStack[depth - 1] : 0u;
        if (depth >= 0 && depth < 64)
            s_sceneTreeProfileSceneStack[depth] = scene;

        const char* sceneName = nullptr;
        if (scene >= 0x10000u && IsReadableMemory(
            reinterpret_cast<void*>(static_cast<uintptr_t>(scene + 0x2E0u)), sizeof(uint32_t)))
        {
            sceneName = *reinterpret_cast<const char**>(static_cast<uintptr_t>(scene + 0x2E0u));
            if (!sceneName || !IsReadableMemory(sceneName, 48u))
                sceneName = nullptr;
        }

        const bool isWorldScene = s_v10Mode == 4 && sceneName &&
            strcmp(sceneName, "game::render (world)") == 0;
        const bool isRetailXboxWorldScene = IsXboxPostProcessRoute() && sceneName &&
            strcmp(sceneName, "game::render (world)") == 0;

        // V10.5.57 Retail Xbox R3: install the previously proven RESZ producer
        // only for the real world scene. No raw D3D vtable methods are detoured;
        // the temporary MID callback captures this frame's already-rendered D24S8.
        if (isRetailXboxWorldScene && s_retailXboxDepthLastArmedPresent != s_presentCount)
        {
            if (s_reszDepthResolveState != 0)
                InterlockedExchange(&s_reszDepthResolveState, 0);
            if (s_provenIntzDofState != 0)
                InterlockedExchange(&s_provenIntzDofState, 0);

            const bool intzReady = EnsureRetailXboxIntzDepthResource(scene);

            // The RESZ -> INTZ -> GPU packed producer was already runtime-proven
            // in the earlier PC research chain. R3 reuses that exact producer
            // without CPU GetRenderTargetData scans: same-present ownership plus
            // successful GPU resolve/pack and final s1 identity are the probe.
            s_opaqueIntzDiagnosticReadbackThisFrame = false;
            s_mode4ReszDepthValidated = intzReady;
            if (intzReady)
                s_mode4ReszLastValidationPresent = s_presentCount;

            if (intzReady && IsReadableMemory(reinterpret_cast<void*>(
                    static_cast<uintptr_t>(scene + kSceneMidCallbackOffset)), 8u))
            {
                s_reszDepthResolveScene = scene;
                s_reszOriginalMid = ReadU32(static_cast<uintptr_t>(scene + kSceneMidCallbackOffset));
                s_reszOriginalMidData = ReadU32(static_cast<uintptr_t>(scene + kSceneMidUserdataOffset));
                WriteU32(static_cast<uintptr_t>(scene + kSceneMidCallbackOffset),
                    static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&Mode4_ReszDepthResolveMid)));
                WriteU32(static_cast<uintptr_t>(scene + kSceneMidUserdataOffset), 0u);
                InterlockedExchange(&s_reszDepthResolveState, 1);
                s_retailXboxDepthLastArmedPresent = s_presentCount;
                InterlockedIncrement(&s_retailXboxDepthArmCount);
            }
            else
            {
                InterlockedIncrement(&s_retailXboxDepthArmFailCount);
            }
        }

        bool startsWorldProfile = false;
        if (isWorldScene && kMode4ReleaseSceneProfiler && s_sceneTreeProfileState == 0)
        {
            if (InterlockedCompareExchange(&s_sceneTreeProfileState, 1, 0) == 0)
            {
                s_sceneTreeProfileRootDepth = depth;
                InterlockedExchange(&s_sceneTreeProfileSceneCount, 0);
                InterlockedExchange(&s_sceneTreeProfileNodeClassEvents, 0);
                InterlockedExchange(&s_sceneTreeProfileOpaqueNodes, 0);
                InterlockedExchange(&s_sceneTreeProfileTransNodes, 0);
                InterlockedExchange(&s_sceneTreeProfileReadFailures, 0);
                startsWorldProfile = true;
            }
        }

        if (isWorldScene && kMode4ReszProbeEnabled && kMode4ReszDofVisualWindowEnabled)
        {
            if (InterlockedCompareExchange(&s_provenIntzDofWindowState, 1, 0) == 0)
            {
                s_provenIntzDofWindowStartPresent = s_presentCount;
                s_provenIntzDofWindowEndPresent = 0u; // continuous RC path
                s_provenIntzDofWindowLastArmedPresent = 0xFFFFFFFFu;
                s_provenIntzDofWindowFrameOrdinal = 0u;
                s_provenIntzDofWindowProduced = 0u;
                s_provenIntzDofWindowApplied = 0u;
                s_provenIntzDofWindowFailures = 0u;
            }

            if (s_provenIntzDofWindowState == 1 &&
                s_provenIntzDofWindowLastArmedPresent != s_presentCount)
            {
                // Every new world frame must start from a fully restored prior
                // frame. Failures are counted, then the one-frame state machines
                // are reset so the next frame can fail closed rather than wedge.
                if (s_provenIntzDofWindowFrameOrdinal > 0u)
                {
                    if (s_provenIntzDofState != 3)
                        ++s_provenIntzDofWindowFailures;
                    InterlockedExchange(&s_provenIntzDofState, 0);

                    if (s_reszDepthResolveState != 3)
                        ++s_provenIntzDofWindowFailures;
                    InterlockedExchange(&s_reszDepthResolveState, 0);
                }

                const bool intzReady = RunMode4OffscreenIntzTargetProbe(scene);
                const bool resourceReady = intzReady;

                // V10.5.49.10.6 fail-closed validator. Verify every world frame
                // until non-clear packed depth is proven, then re-verify every
                // 120 presents. A reset clears the proof and restarts validation.
                const bool depthValidationExpired =
                    !s_mode4ReszDepthValidated ||
                    (s_presentCount - s_mode4ReszLastValidationPresent) >=
                        kMode4ReszDepthRevalidatePresents;
                s_opaqueIntzDiagnosticReadbackThisFrame = depthValidationExpired;

                if (resourceReady && s_reszDepthResolveState == 0 &&
                    IsReadableMemory(reinterpret_cast<void*>(
                        static_cast<uintptr_t>(scene + kSceneMidCallbackOffset)), 8u))
                {
                    s_reszDepthResolveScene = scene;
                    s_reszOriginalMid =
                        ReadU32(static_cast<uintptr_t>(scene + kSceneMidCallbackOffset));
                    s_reszOriginalMidData =
                        ReadU32(static_cast<uintptr_t>(scene + kSceneMidUserdataOffset));
                    WriteU32(static_cast<uintptr_t>(scene + kSceneMidCallbackOffset),
                        static_cast<uint32_t>(
                            reinterpret_cast<uintptr_t>(&Mode4_ReszDepthResolveMid)));
                    WriteU32(static_cast<uintptr_t>(scene + kSceneMidUserdataOffset), 0u);
                    InterlockedExchange(&s_reszDepthResolveState, 1);
                    s_provenIntzDofWindowLastArmedPresent = s_presentCount;
                    ++s_provenIntzDofWindowFrameOrdinal;
                }
                else
                {
                    ++s_provenIntzDofWindowFailures;
                }
            }
        }
        else if (isWorldScene && kMode4ReszProbeEnabled &&
            s_reszDepthResolveState == 0)
        {
            // Retained one-shot fallback for diagnostic builds.
            const bool intzReady = RunMode4OffscreenIntzTargetProbe(scene);
            bool sampleReady = false;
            if (intzReady)
            {
                void* probeDevice = reinterpret_cast<void*>(
                    static_cast<uintptr_t>(s_rawDeviceValue));
                sampleReady = RunMode4IntzSampleReadbackSelfTest(probeDevice);
            }
            s_opaqueIntzDiagnosticReadbackThisFrame = true;
            if (intzReady && sampleReady &&
                IsReadableMemory(reinterpret_cast<void*>(
                    static_cast<uintptr_t>(scene + kSceneMidCallbackOffset)), 8u))
            {
                s_reszDepthResolveScene = scene;
                s_reszOriginalMid = ReadU32(static_cast<uintptr_t>(scene + kSceneMidCallbackOffset));
                s_reszOriginalMidData = ReadU32(static_cast<uintptr_t>(scene + kSceneMidUserdataOffset));
                WriteU32(static_cast<uintptr_t>(scene + kSceneMidCallbackOffset),
                    static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&Mode4_ReszDepthResolveMid)));
                WriteU32(static_cast<uintptr_t>(scene + kSceneMidUserdataOffset), 0u);
                InterlockedExchange(&s_reszDepthResolveState, 1);
            }
            else
            {
                InterlockedExchange(&s_reszDepthResolveState, -1);
            }
        }

        if (isWorldScene && !kMode4ReszProbeEnabled)
        {
            if (InterlockedCompareExchange(&s_provenIntzDofWindowState, 1, 0) == 0)
            {
                s_provenIntzDofWindowStartPresent = s_presentCount;
                s_provenIntzDofWindowEndPresent =
                    s_presentCount + kLegacyReplayVisualWindowFrames - 1u;
                s_provenIntzDofWindowLastArmedPresent = 0xFFFFFFFFu;
                s_provenIntzDofWindowFrameOrdinal = 0u;
                s_provenIntzDofWindowProduced = 0u;
                s_provenIntzDofWindowApplied = 0u;
                s_provenIntzDofWindowFailures = 0u;
            }

            if (s_provenIntzDofWindowState == 1 &&
                s_provenIntzDofWindowFrameOrdinal < kLegacyReplayVisualWindowFrames &&
                s_provenIntzDofWindowLastArmedPresent != s_presentCount)
            {
                // Close the previous frame's one-draw activation state before
                // arming fresh depth for this world frame. A restored state (3)
                // is the normal steady-state result after the prior final draw.
                if (s_provenIntzDofWindowFrameOrdinal > 0u)
                {
                    if (s_provenIntzDofState != 3)
                        ++s_provenIntzDofWindowFailures;
                    InterlockedExchange(&s_provenIntzDofState, 0);

                    if (s_opaqueIntzReplayState != 3)
                        ++s_provenIntzDofWindowFailures;
                    InterlockedExchange(&s_opaqueIntzReplayState, 0);
                }

                const bool intzReady = RunMode4OffscreenIntzTargetProbe(scene);
                bool sampleReady = false;
                if (intzReady)
                {
                    void* probeDevice = reinterpret_cast<void*>(
                        static_cast<uintptr_t>(s_rawDeviceValue));
                    // This self-test performs its expensive CPU verification only
                    // once; subsequent calls return the proven cached result.
                    sampleReady = RunMode4IntzSampleReadbackSelfTest(probeDevice);
                }

                s_opaqueIntzDiagnosticReadbackThisFrame =
                    (s_provenIntzDofWindowFrameOrdinal == 0u);

                // Install the temporary MID callback on every visual-window
                // world frame. The callback creates a fresh INTZ snapshot from
                // this frame's opaque list, then packs it on GPU for the final
                // combine. The slot is restored as soon as nglRenderScene returns.
                if (intzReady && sampleReady && s_opaqueIntzReplayState == 0 &&
                    IsReadableMemory(reinterpret_cast<void*>(
                        static_cast<uintptr_t>(scene + kSceneMidCallbackOffset)), 8u))
                {
                    s_opaqueIntzReplayScene = scene;
                    s_opaqueIntzOriginalMid =
                        ReadU32(static_cast<uintptr_t>(scene + kSceneMidCallbackOffset));
                    s_opaqueIntzOriginalMidData =
                        ReadU32(static_cast<uintptr_t>(scene + kSceneMidUserdataOffset));
                    WriteU32(static_cast<uintptr_t>(scene + kSceneMidCallbackOffset),
                        static_cast<uint32_t>(
                            reinterpret_cast<uintptr_t>(&Mode4_OpaqueIntzReplayMid)));
                    WriteU32(static_cast<uintptr_t>(scene + kSceneMidUserdataOffset), 0u);
                    InterlockedExchange(&s_opaqueIntzReplayState, 1);
                    s_provenIntzDofWindowLastArmedPresent = s_presentCount;
                    ++s_provenIntzDofWindowFrameOrdinal;
                }
                else
                {
                    ++s_provenIntzDofWindowFailures;
                }
            }
        }

        const bool active = (s_sceneTreeProfileState == 1 &&
            s_sceneTreeProfileRootDepth >= 0 && depth >= s_sceneTreeProfileRootDepth);
        if (active)
            ProfileCurrentNGLScene(scene, static_cast<uint32_t>(depth), parentScene);

        const int result = s_originalNGLRenderScene();

        if ((isWorldScene || isRetailXboxWorldScene) && kMode4ReszProbeEnabled &&
            scene == s_reszDepthResolveScene &&
            IsReadableMemory(reinterpret_cast<void*>(
                static_cast<uintptr_t>(scene + kSceneMidCallbackOffset)), 8u))
        {
            const uint32_t currentMid =
                ReadU32(static_cast<uintptr_t>(scene + kSceneMidCallbackOffset));
            if (currentMid == static_cast<uint32_t>(
                reinterpret_cast<uintptr_t>(&Mode4_ReszDepthResolveMid)))
            {
                WriteU32(static_cast<uintptr_t>(scene + kSceneMidCallbackOffset),
                    s_reszOriginalMid);
                WriteU32(static_cast<uintptr_t>(scene + kSceneMidUserdataOffset),
                    s_reszOriginalMidData);
            }
        }

        if (isWorldScene && scene == s_opaqueIntzReplayScene &&
            IsReadableMemory(reinterpret_cast<void*>(
                static_cast<uintptr_t>(scene + kSceneMidCallbackOffset)), 8u))
        {
            const uint32_t currentMid =
                ReadU32(static_cast<uintptr_t>(scene + kSceneMidCallbackOffset));
            if (currentMid == static_cast<uint32_t>(
                reinterpret_cast<uintptr_t>(&Mode4_OpaqueIntzReplayMid)))
            {
                WriteU32(static_cast<uintptr_t>(scene + kSceneMidCallbackOffset),
                    s_opaqueIntzOriginalMid);
                WriteU32(static_cast<uintptr_t>(scene + kSceneMidUserdataOffset),
                    s_opaqueIntzOriginalMidData);
            }
        }

        if (active && depth == s_sceneTreeProfileRootDepth)
        {
            Event* event = ReserveEvent(EventType::SceneTreeProfileSummary);
            if (event)
            {
                event->a = s_presentCount;
                event->b = static_cast<uint32_t>(s_sceneTreeProfileSceneCount);
                event->c = static_cast<uint32_t>(s_sceneTreeProfileNodeClassEvents);
                event->d = static_cast<uint32_t>(s_sceneTreeProfileOpaqueNodes);
                event->e = static_cast<uint32_t>(s_sceneTreeProfileTransNodes);
                event->f = static_cast<uint32_t>(s_sceneTreeProfileReadFailures);
                event->g = scene;
                event->h = static_cast<uint32_t>(depth);
                event->i = startsWorldProfile ? 1u : 0u;
                event->j = static_cast<uint32_t>(result);
                CopySmallText(event->text, sizeof(event->text), sceneName);
            }
            InterlockedExchange(&s_sceneTreeProfileState, 2);
            s_sceneTreeProfileRootDepth = -1;
        }

        if (depth >= 0 && depth < 64)
            s_sceneTreeProfileSceneStack[depth] = 0u;
        --s_nglRenderSceneHookDepth;
        return result;
    }

    void __cdecl NGLRegisterSceneCallback_Hook(int type, NGLSceneCallback_t callback, void* userData)
    {
        const uintptr_t returnAddress = reinterpret_cast<uintptr_t>(_ReturnAddress());
        s_originalRegisterSceneCallback(type, callback, userData);

        // Retail WDS render manager's POST registration returns to 008A7A24.
        // Install only if the MID slot is still empty. This intentionally does
        // not steal callbacks from any scene that already owns its MID phase.
        if (s_v10Mode != 4 || !s_mode4MidProbeOnly ||
            type != 2 || returnAddress != kWdsPostCallbackRegisterReturn)
        {
            return;
        }

        const uint32_t scene = ReadU32(kNGLCurrentSceneGlobal);
        uint32_t existingMid = 0u;
        if (scene >= 0x10000u && IsReadableMemory(
            reinterpret_cast<void*>(static_cast<uintptr_t>(scene + kSceneMidCallbackOffset)), sizeof(uint32_t)))
        {
            existingMid = ReadU32(static_cast<uintptr_t>(scene + kSceneMidCallbackOffset));
        }

        bool installed = false;
        if (scene >= 0x10000u && existingMid == 0u)
        {
            // New WDS scene build. V10.5.19 reads the already-built render list
            // directly at MID; these historical counters are reset only for clean
            // diagnostics before the one-shot staged prepass runs.
            if (s_mode4DepthPrepassRunState == 0)
            {
                InterlockedExchange(&s_mode4DepthCommandCount, 0);
                InterlockedExchange(&s_mode4DepthCommandOverflow, 0);
                // V10.5.28: keep the call-stack profile scoped to exactly the
                // current WDS scene whose MID slot we are about to install.
                InterlockedExchange(&s_mode4PhatCallerProfileCount, 0);
                InterlockedExchange(&s_mode4PhatCallerProfileLogged, 0);
                InterlockedExchange(&s_mode4PhatCallerProfileStackFailures, 0);
            }
            s_originalRegisterSceneCallback(1, &Mode4_MidSceneDepthProbe, nullptr);
            InterlockedIncrement(&s_mode4MidInstallCount);
            installed = true;
        }
        else if (existingMid != static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&Mode4_MidSceneDepthProbe)))
        {
            InterlockedIncrement(&s_mode4MidConflictCount);
        }

        const LONG installs = s_mode4MidInstallCount;
        if (installs <= 32 || (installs % 120) == 0 || !installed)
        {
            Event* event = ReserveEvent(EventType::MidSceneCallbackInstall);
            if (event)
            {
                event->a = s_presentCount;
                event->b = static_cast<uint32_t>(type);
                event->c = static_cast<uint32_t>(returnAddress);
                event->d = scene;
                event->e = existingMid;
                event->f = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&Mode4_MidSceneDepthProbe));
                event->g = installed ? 1u : 0u;
                event->h = static_cast<uint32_t>(s_mode4MidInstallCount);
                event->i = static_cast<uint32_t>(s_mode4MidCallbackCount);
                event->j = static_cast<uint32_t>(s_mode4MidConflictCount);
            }
        }
    }

    void __fastcall PhatRenderCommandExecute_Hook(void* command)
    {
        // V10.5.28: restore the exact known-working V10.5.18.1 collector gate.
        // Do not invent a next-frame window or parent-scene equality test. The
        // current scene itself must already own our MID callback, just as in the
        // producer proof that collected ~96 real Phat commands.
        if (s_v10Mode == 4 && s_mode4MidProbeOnly && command &&
            s_mode4DepthPrepassRunState == 0 &&
            ReadU8(kShadowRenderPassFlagGlobal) == 0u)
        {
            const uint32_t scene = ReadU32(kNGLCurrentSceneGlobal);
            uint32_t mid = 0u;
            if (scene >= 0x10000u && IsReadableMemory(
                reinterpret_cast<void*>(static_cast<uintptr_t>(scene + kSceneMidCallbackOffset)), sizeof(uint32_t)))
            {
                mid = ReadU32(static_cast<uintptr_t>(scene + kSceneMidCallbackOffset));
            }

            if (mid == static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&Mode4_MidSceneDepthProbe)) &&
                IsReadableMemory(command, 0x18u))
            {
                const uint32_t material = ReadU32(reinterpret_cast<uintptr_t>(command) + 0x14u);
                if (material >= 0x10000u && IsReadableMemory(
                    reinterpret_cast<void*>(static_cast<uintptr_t>(material + 0x24u)), sizeof(uint32_t)))
                {
                    const uint32_t materialType = ReadU32(static_cast<uintptr_t>(material + 0x24u));
                    if (materialType == 1u || materialType == 2u)
                    {
                        const LONG callIndex = InterlockedIncrement(&s_mode4PhatCallerProfileCount);
                        const uintptr_t returnAddress = reinterpret_cast<uintptr_t>(_ReturnAddress());

                        if (!s_captureStackBackTrace)
                        {
                            HMODULE ntdll = GetModuleHandleA("ntdll.dll");
                            if (ntdll)
                            {
                                s_captureStackBackTrace = reinterpret_cast<RtlCaptureStackBackTrace_t>(
                                    GetProcAddress(ntdll, "RtlCaptureStackBackTrace"));
                            }
                        }

                        void* frames[8] = {};
                        USHORT frameCount = 0u;
                        if (s_captureStackBackTrace)
                        {
                            frameCount = s_captureStackBackTrace(1u, 8u, frames, nullptr);
                        }
                        else
                        {
                            InterlockedIncrement(&s_mode4PhatCallerProfileStackFailures);
                        }

                        if (callIndex <= 512)
                        {
                            Event* event = ReserveEvent(EventType::PhatCallerProfile);
                            if (event)
                            {
                                event->a = s_presentCount;
                                event->b = static_cast<uint32_t>(callIndex);
                                event->c = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(command));
                                event->d = material;
                                event->e = materialType;
                                event->f = static_cast<uint32_t>(returnAddress);
                                event->g = static_cast<uint32_t>(frameCount);
                                event->h = scene;
                                event->i = mid;
                                for (uint32_t i = 0u; i < 8u; ++i)
                                    event->extra[i] = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(frames[i]));
                                CopySmallText(event->text, sizeof(event->text), CurrentSceneName());
                            }
                            InterlockedIncrement(&s_mode4PhatCallerProfileLogged);
                        }
                    }
                }
            }
        }

        s_originalPhatRenderCommandExecute(command);
    }

    bool EnsureV9ReadbackSurface(void* device)
    {
        const uint32_t lumSurfaceValue = s_rawSurfaceLum16;
        if (!device || lumSurfaceValue < 0x10000u || !s_rawCreateOffscreenPlainSurface)
            return false;

        void* lumSurface = reinterpret_cast<void*>(static_cast<uintptr_t>(lumSurfaceValue));
        D3DSurfaceDescLite desc = {};
        if (!GetSurfaceDescSafe(lumSurface, desc) || desc.Width == 0 || desc.Height == 0)
            return false;

        if (s_v9ReadbackSurface &&
            s_v9ReadbackFormat == desc.Format &&
            s_v9ReadbackWidth == desc.Width &&
            s_v9ReadbackHeight == desc.Height)
        {
            return true;
        }

        ResetV9ReadbackResource();

        void* surface = nullptr;
        const HRESULT hr = s_rawCreateOffscreenPlainSurface(
            device, desc.Width, desc.Height, desc.Format, kD3DPoolSystemMem, &surface, nullptr);
        if (FAILED(hr) || !surface)
        {
            s_v9LastReadbackHr = hr;
            return false;
        }

        s_v9ReadbackSurface = surface;
        s_v9ReadbackFormat = desc.Format;
        s_v9ReadbackWidth = desc.Width;
        s_v9ReadbackHeight = desc.Height;
        return true;
    }

    bool EnsureV9SceneResources(void* device, void* sourceSurface)
    {
        if (!device || !sourceSurface || !s_rawCreateTexture || !s_rawCreateStateBlock)
            return false;

        D3DSurfaceDescLite desc = {};
        if (!GetSurfaceDescSafe(sourceSurface, desc) || desc.Width == 0 || desc.Height == 0)
            return false;

        const uint32_t sourceIdentity =
            static_cast<uint32_t>(reinterpret_cast<uintptr_t>(sourceSurface));

        if (s_v9SceneCopyTexture && s_v9SceneCopySurface && s_v9StateBlock &&
            s_v9SceneResourceIdentity == sourceIdentity &&
            s_v9SceneWidth == desc.Width && s_v9SceneHeight == desc.Height &&
            s_v9SceneFormat == desc.Format)
        {
            return true;
        }

        ResetV9SceneResources();

        void* texture = nullptr;
        HRESULT hr = s_rawCreateTexture(device, desc.Width, desc.Height, 1u,
            kD3DUsageRenderTarget, desc.Format, kD3DPoolDefault, &texture, nullptr);
        if (FAILED(hr) || !texture)
        {
            s_v9LastStretchHr = hr;
            return false;
        }

        void** textureVtable = *reinterpret_cast<void***>(texture);
        if (!textureVtable || !IsReadableMemory(textureVtable,
            (kD3DTextureGetSurfaceLevelVtableIndex + 1u) * sizeof(void*)) ||
            !IsExecutableMemory(textureVtable[kD3DTextureGetSurfaceLevelVtableIndex]))
        {
            SafeReleaseCom(texture);
            return false;
        }

        const auto getSurfaceLevel =
            reinterpret_cast<D3DTextureGetSurfaceLevel_t>(textureVtable[kD3DTextureGetSurfaceLevelVtableIndex]);
        void* surface = nullptr;
        hr = getSurfaceLevel(texture, 0u, &surface);
        if (FAILED(hr) || !surface)
        {
            SafeReleaseCom(texture);
            return false;
        }

        void* stateBlock = nullptr;
        hr = s_rawCreateStateBlock(device, kD3DStateBlockAll, &stateBlock);
        if (FAILED(hr) || !stateBlock)
        {
            SafeReleaseCom(surface);
            SafeReleaseCom(texture);
            return false;
        }

        s_v9SceneCopyTexture = texture;
        s_v9SceneCopySurface = surface;
        s_v9StateBlock = stateBlock;
        s_v9SceneResourceIdentity = sourceIdentity;
        s_v9SceneWidth = desc.Width;
        s_v9SceneHeight = desc.Height;
        s_v9SceneFormat = desc.Format;
        return true;
    }

    bool SamplePostFXLuminance(void* device)
    {
        if (!device || !s_rawGetRenderTargetData || !EnsureV9ReadbackSurface(device))
            return false;

        void* lumSurface = reinterpret_cast<void*>(static_cast<uintptr_t>(s_rawSurfaceLum16));
        s_v9LastReadbackHr = s_rawGetRenderTargetData(device, lumSurface, s_v9ReadbackSurface);
        if (FAILED(s_v9LastReadbackHr))
            return false;

        if (!IsReadableMemory(s_v9ReadbackSurface, sizeof(void*)))
            return false;
        void** vtable = *reinterpret_cast<void***>(s_v9ReadbackSurface);
        if (!vtable || !IsReadableMemory(vtable, (kD3DSurfaceUnlockRectVtableIndex + 1u) * sizeof(void*)))
            return false;
        if (!IsExecutableMemory(vtable[kD3DSurfaceLockRectVtableIndex]) ||
            !IsExecutableMemory(vtable[kD3DSurfaceUnlockRectVtableIndex]))
            return false;

        const auto lockRect = reinterpret_cast<D3DSurfaceLockRect_t>(vtable[kD3DSurfaceLockRectVtableIndex]);
        const auto unlockRect = reinterpret_cast<D3DSurfaceUnlockRect_t>(vtable[kD3DSurfaceUnlockRectVtableIndex]);

        D3DLockedRectLite locked = {};
        s_v9LastLockHr = lockRect(s_v9ReadbackSurface, &locked, nullptr, kD3DLockReadOnly);
        if (FAILED(s_v9LastLockHr) || !locked.pBits || locked.Pitch <= 0)
            return false;

        double sums[4] = {};
        double weightedSquareSum = 0.0;
        double rawWeightedSquareSum = 0.0;
        float calibrationMagnitudes[256] = {};
        uint32_t calibrationMagnitudeCount = 0;
        uint32_t overOnePixels = 0;
        float rawMaxChannel = 0.0f;
        uint32_t validPixels = 0;
        const uint32_t width = s_v9ReadbackWidth;
        const uint32_t height = s_v9ReadbackHeight;

        for (uint32_t y = 0; y < height; ++y)
        {
            const uint8_t* row = reinterpret_cast<const uint8_t*>(locked.pBits) + static_cast<size_t>(y) * locked.Pitch;
            for (uint32_t x = 0; x < width; ++x)
            {
                const uint16_t* pixel = reinterpret_cast<const uint16_t*>(row + static_cast<size_t>(x) * 8u);
                float channels[4] =
                {
                    HalfToFloat(pixel[0]), HalfToFloat(pixel[1]),
                    HalfToFloat(pixel[2]), HalfToFloat(pixel[3])
                };

                if (!IsFiniteUsefulFloat(channels[0]) || !IsFiniteUsefulFloat(channels[1]) ||
                    !IsFiniteUsefulFloat(channels[2]) || !IsFiniteUsefulFloat(channels[3]))
                {
                    continue;
                }

                for (uint32_t c = 0; c < 4; ++c)
                    sums[c] += channels[c];

                // V10.2 calibration telemetry: preserve the raw positive FP16
                // energy before V10's deliberate 0..1 clamp. This is observation
                // only and is never fed back into bloom or exposure.
                const float rawR = channels[0] > 0.0f ? channels[0] : 0.0f;
                const float rawG = channels[1] > 0.0f ? channels[1] : 0.0f;
                const float rawB = channels[2] > 0.0f ? channels[2] : 0.0f;
                const float pixelRawMax = rawR > rawG ? (rawR > rawB ? rawR : rawB) : (rawG > rawB ? rawG : rawB);
                if (pixelRawMax > rawMaxChannel)
                    rawMaxChannel = pixelRawMax;
                if (pixelRawMax > 1.0f)
                    ++overOnePixels;
                rawWeightedSquareSum +=
                    static_cast<double>(kV10WeightR) * rawR * rawR +
                    static_cast<double>(kV10WeightG) * rawG * rawG +
                    static_cast<double>(kV10WeightB) * rawB * rawB;

                // The recovered Xbox routine reads byte-like RGB values, squares
                // them, averages 0.11/0.30/0.59 weighted energy, then sqrt()s.
                // PC's E8FB04 is FP16, so clamp logical RGB to 0..1 before
                // converting the RMS result to the equivalent 0..255 domain.
                const float r = ClampFloat(channels[0], 0.0f, 1.0f);
                const float g = ClampFloat(channels[1], 0.0f, 1.0f);
                const float b = ClampFloat(channels[2], 0.0f, 1.0f);
                const float clampedEnergy =
                    kV10WeightR * r * r + kV10WeightG * g * g + kV10WeightB * b * b;
                if (calibrationMagnitudeCount < 256u)
                {
                    calibrationMagnitudes[calibrationMagnitudeCount++] =
                        clampedEnergy > 0.0f ? static_cast<float>(std::sqrt(clampedEnergy)) * 255.0f : 0.0f;
                }
                weightedSquareSum +=
                    static_cast<double>(kV10WeightR) * r * r +
                    static_cast<double>(kV10WeightG) * g * g +
                    static_cast<double>(kV10WeightB) * b * b;
                ++validPixels;
            }
        }

        unlockRect(s_v9ReadbackSurface);

        if (validPixels == 0)
            return false;

        const float avgR = static_cast<float>(sums[0] / validPixels);
        const float avgG = static_cast<float>(sums[1] / validPixels);
        const float avgB = static_cast<float>(sums[2] / validPixels);
        const float avgA = static_cast<float>(sums[3] / validPixels);

        // V10 Xbox-style dynamic bloom control.
        const double weightedMeanSquare = weightedSquareSum / static_cast<double>(validPixels);
        const float weightedRms = weightedMeanSquare > 0.0 ?
            static_cast<float>(std::sqrt(weightedMeanSquare)) : 0.0f;
        const float brightness255 = ClampFloat(weightedRms * 255.0f, 0.0f, 255.0f);

        const double rawWeightedMeanSquare = rawWeightedSquareSum / static_cast<double>(validPixels);
        const float rawWeightedRms = rawWeightedMeanSquare > 0.0 ?
            static_cast<float>(std::sqrt(rawWeightedMeanSquare)) : 0.0f;

        // 16x12 = 192 samples, so an insertion sort is tiny and avoids any
        // allocation or dependency in the render hook. These percentiles are
        // diagnostic only and do not change V10/V9 state.
        for (uint32_t n = 1; n < calibrationMagnitudeCount; ++n)
        {
            const float value = calibrationMagnitudes[n];
            uint32_t m = n;
            while (m > 0u && calibrationMagnitudes[m - 1u] > value)
            {
                calibrationMagnitudes[m] = calibrationMagnitudes[m - 1u];
                --m;
            }
            calibrationMagnitudes[m] = value;
        }
        auto CalibrationPercentile = [&](float q) -> float
        {
            if (calibrationMagnitudeCount == 0u)
                return 0.0f;
            const float position = q * static_cast<float>(calibrationMagnitudeCount - 1u);
            uint32_t index = static_cast<uint32_t>(position + 0.5f);
            if (index >= calibrationMagnitudeCount)
                index = calibrationMagnitudeCount - 1u;
            return calibrationMagnitudes[index];
        };
        const float calibrationP10 = CalibrationPercentile(0.10f);
        const float calibrationP50 = CalibrationPercentile(0.50f);
        const float calibrationP90 = CalibrationPercentile(0.90f);

        const float brightnessRange = kV10XboxLightBrightness - kV10XboxDarkBrightness;
        float bloomT = 0.0f;
        if (brightnessRange > 0.000001f)
        {
            bloomT = ClampFloat(
                (brightness255 - kV10XboxDarkBrightness) / brightnessRange,
                0.0f, 1.0f);
        }
        const float bloomStrength =
            kV10XboxDarkBloom + (kV10XboxLightBloom - kV10XboxDarkBloom) * bloomT;

        ++s_v10BloomSampleCount;
        s_v10WeightedRms = weightedRms;
        s_v10Brightness255 = brightness255;
        s_v10BloomT = bloomT;
        s_v10BloomStrength = ClampFloat(bloomStrength, 0.0f, 2.0f);
        s_v10BloomSampleValid = true;
        s_v10_3LastLuminanceSamplePresent = s_presentCount;

        const bool logBloomSample = (s_v10Mode == 5 && s_postProcessRoute == 3u &&
            kDebugXboxIntegrationLockdownQuietLogging)
            ? (s_v10BloomSampleCount <= 2u || (s_v10BloomSampleCount % 300u) == 0u)
            : (s_v10BloomSampleCount <= 16u || (s_v10BloomSampleCount % 60u) == 0u);
        if (logBloomSample)
        {
            Event* event = ReserveEvent(EventType::BloomControlSample);
            if (event)
            {
                event->a = s_presentCount;
                event->b = s_v10BloomSampleCount;
                event->c = validPixels;
                event->d = static_cast<uint32_t>(s_v9LastReadbackHr);
                event->e = static_cast<uint32_t>(s_v9LastLockHr);
                event->f = FloatBits(weightedRms);
                event->g = FloatBits(brightness255);
                event->h = FloatBits(bloomT);
                event->i = FloatBits(s_v10BloomStrength);
                event->j = FloatBits(avgR);
                event->extra[0] = FloatBits(avgG);
                event->extra[1] = FloatBits(avgB);
                event->extra[2] = FloatBits(avgA);
                event->extra[3] = s_v9ReadbackFormat;
                event->extra[4] = s_v9ReadbackWidth;
                event->extra[5] = s_v9ReadbackHeight;
            }
        }

        float sampleLuminance = (avgR + avgG + avgB) / 3.0f;
        if (!(sampleLuminance > 0.000001f) && avgA > 0.000001f)
            sampleLuminance = avgA;

        auto QueueV10_2Calibration = [&](float targetExposure, float adaptedExposure)
        {
            if (s_v10Mode == 5 && s_postProcessRoute == 3u &&
                kDebugXboxIntegrationLockdownQuietLogging)
                return;
            ++s_v10_2CalibrationSampleCount;
            if (s_v10_2CalibrationSampleCount <= 24u || (s_v10_2CalibrationSampleCount % 30u) == 0u)
            {
                Event* event = ReserveEvent(EventType::CalibrationSample);
                if (event)
                {
                    event->a = s_presentCount;
                    event->b = s_v10_2CalibrationSampleCount;
                    event->c = validPixels;
                    event->d = overOnePixels;
                    event->e = FloatBits(rawWeightedRms);
                    event->f = FloatBits(weightedRms);
                    event->g = FloatBits(rawMaxChannel);
                    event->h = FloatBits(calibrationP10);
                    event->i = FloatBits(calibrationP50);
                    event->j = FloatBits(calibrationP90);
                    event->extra[0] = FloatBits(brightness255);
                    event->extra[1] = FloatBits(s_v10BloomStrength);
                    event->extra[2] = FloatBits(sampleLuminance);
                    event->extra[3] = FloatBits(s_v9ReferenceLuminance);
                    event->extra[4] = FloatBits(targetExposure);
                    event->extra[5] = FloatBits(adaptedExposure);
                    event->extra[6] = FloatBits(avgR);
                    event->extra[7] = FloatBits(avgG);
                    event->extra[8] = FloatBits(avgB);
                    event->extra[9] = static_cast<uint32_t>(s_v10Mode);
                }
            }
        };

        // Keep V9 calculations in modes 2 and 3 so V10.4 can compare the
        // native Xbox-state path against the frozen V9 controller numerically.
        // Only mode 2 is permitted to APPLY the V9 full-screen visual pass.
        if (s_v10Mode != 2 && s_v10Mode != 3)
        {
            QueueV10_2Calibration(s_v9TargetExposure, s_v9AdaptedExposure);
            return true;
        }

        float luminance = sampleLuminance;
        if (!(luminance > 0.000001f) || !IsFiniteUsefulFloat(luminance))
        {
            QueueV10_2Calibration(s_v9TargetExposure, s_v9AdaptedExposure);
            return true; // V10 bloom sample is still valid.
        }

        ++s_v9SampleCount;
        s_v9CurrentLuminance = luminance;

        if (s_v9WarmupCount < kV9WarmupSamples)
        {
            s_v9WarmupSum += luminance;
            ++s_v9WarmupCount;
            s_v9ReferenceLuminance = s_v9WarmupSum / static_cast<float>(s_v9WarmupCount);
            s_v9TargetExposure = 1.0f;
            s_v9AdaptedExposure = 1.0f;
        }
        else
        {
            const float reference = s_v9ReferenceLuminance > 0.000001f ? s_v9ReferenceLuminance : luminance;
            s_v9TargetExposure = ClampFloat(reference / luminance, kV9MinExposure, kV9MaxExposure);
            const float alpha = (s_v9TargetExposure < s_v9AdaptedExposure) ?
                kV9DarkenAlpha : kV9BrightenAlpha;
            s_v9AdaptedExposure += (s_v9TargetExposure - s_v9AdaptedExposure) * alpha;
            s_v9AdaptedExposure = ClampFloat(s_v9AdaptedExposure, kV9MinExposure, kV9MaxExposure);
        }

        if (s_v9SampleCount <= 16u || (s_v9SampleCount % 60u) == 0u)
        {
            Event* event = ReserveEvent(EventType::ExposureSample);
            if (event)
            {
                event->a = s_presentCount;
                event->b = s_v9SampleCount;
                event->c = static_cast<uint32_t>(s_v9LastReadbackHr);
                event->d = static_cast<uint32_t>(s_v9LastLockHr);
                event->e = FloatBits(avgR);
                event->f = FloatBits(avgG);
                event->g = FloatBits(avgB);
                event->h = FloatBits(avgA);
                event->i = FloatBits(luminance);
                event->j = FloatBits(s_v9ReferenceLuminance);
                event->extra[0] = FloatBits(s_v9TargetExposure);
                event->extra[1] = FloatBits(s_v9AdaptedExposure);
                event->extra[2] = validPixels;
                event->extra[3] = s_v9ReadbackFormat;
                event->extra[4] = s_v9ReadbackWidth;
                event->extra[5] = s_v9ReadbackHeight;
            }
        }
        QueueV10_2Calibration(s_v9TargetExposure, s_v9AdaptedExposure);
        return true;
    }

    uint32_t ExposureTextureFactor(float exposure, uint32_t& colorOp)
    {
        float factor = exposure;
        colorOp = kTopModulate;
        if (exposure > 1.0f)
        {
            colorOp = kTopModulate2X;
            factor = exposure * 0.5f;
        }
        factor = ClampFloat(factor, 0.0f, 1.0f);
        uint32_t channel = static_cast<uint32_t>(factor * 255.0f + 0.5f);
        if (channel > 255u)
            channel = 255u;
        return 0xFF000000u | (channel << 16) | (channel << 8) | channel;
    }

    HRESULT DrawMode4NativeFullscreen(void* device);
    void SetMode4LinearClampSampler0(void* device, HRESULT& firstFailure);
    void SetMode4OffscreenNoBlendState(void* device, HRESULT& firstFailure);
    void ResetMode4CameraMotionZBlurController();
    void ResetMode4CameraMotionZBlurResources();
    void ResetRetailXboxImageZoomProbeResources();
    void ResetMode4PackedDepthResources();
    void ReleasePostFXDeviceResourcesForReset();

    void ResetV10BloomStateBlock()
    {
        SafeReleaseCom(s_v10BloomStateBlock);
        s_v10BloomStateBlockDeviceIdentity = 0;
    }

    bool EnsureV10BloomStateBlock(void* device)
    {
        if (!device || !s_rawCreateStateBlock)
            return false;

        const uint32_t deviceIdentity = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(device));
        if (s_v10BloomStateBlock && s_v10BloomStateBlockDeviceIdentity == deviceIdentity)
            return true;

        ResetV10BloomStateBlock();
        void* stateBlock = nullptr;
        const HRESULT hr = s_rawCreateStateBlock(device, kD3DStateBlockAll, &stateBlock);
        if (FAILED(hr) || !stateBlock)
            return false;

        s_v10BloomStateBlock = stateBlock;
        s_v10BloomStateBlockDeviceIdentity = deviceIdentity;
        return true;
    }

    bool ApplyV10BloomScalePass()
    {
        // V10.5.65 reuses the recovered native weighted Scene source before the
        // stock Gaussian pair. Debug route 3 supplies the proven 0..1.5 controller.
        // Xbox Function_82F31348 binds Scene+0x31C+0x14 directly before
        // DAT_8438AAD4/DAT_8438AA68, renders the weighted result to its working
        // bloom RT, then resolves that RT before Gaussian.  PC maps this to
        // Scene -> Quarter A before the recovered Gaussian -> blurlevel1 ->
        // blurlevel2 tail.
        const bool debugD2Mode5Ready = s_v10Mode == 5 && s_postProcessRoute == 3u &&
            s_mode5DrawHookState == 2 && s_retailXboxDepthApiState == 1;
        if (s_v10Mode < 1 || !s_v10BloomSampleValid ||
            (s_rawHookState != 2 && !debugD2Mode5Ready) ||
            s_rawDeviceValue < 0x10000u || s_rawTextureScene < 0x10000u ||
            s_rawSurfaceScene < 0x10000u || s_rawTextureQuarterA < 0x10000u ||
            s_rawSurfaceQuarterA < 0x10000u || s_rawSurfaceQuarterB < 0x10000u ||
            !s_rawSetRenderTarget || !s_rawGetRenderTarget ||
            !s_rawSetDepthStencilSurface || !s_rawGetDepthStencilSurface ||
            !s_rawSetTexture || !s_rawSetPixelShader || !s_rawSetVertexShader ||
            !s_rawSetVertexShaderConstantF || !s_rawSetPixelShaderConstantF ||
            !s_rawSetViewport || !s_rawSetRenderState || !s_rawSetSamplerState ||
            !s_rawDrawPrimitive || !s_originalFullscreenQuad)
        {
            return false;
        }

        const uint32_t weightedVS = ReadU32(kWeightedBloomVS);
        const uint32_t weightedPS = ReadU32(kWeightedBloomPS);
        if (weightedVS < 0x10000u || weightedPS < 0x10000u ||
            !IsReadableMemory(reinterpret_cast<void*>(static_cast<uintptr_t>(weightedVS)), sizeof(void*)) ||
            !IsReadableMemory(reinterpret_cast<void*>(static_cast<uintptr_t>(weightedPS)), sizeof(void*)))
            return false;

        const uintptr_t psConstants[6] =
        {
            kWeightedBloomPsC0, kWeightedBloomPsC1, kWeightedBloomPsC2,
            kWeightedBloomPsC3, kWeightedBloomPsC4, kWeightedBloomPsC5
        };
        for (uintptr_t addr : psConstants)
        {
            if (!IsReadableMemory(reinterpret_cast<void*>(addr), sizeof(float) * 4u))
                return false;
        }
        if (!IsReadableMemory(reinterpret_cast<void*>(kWeightedBloomVsTexelScale), sizeof(float)))
            return false;

        // Never sample from the resource we are about to render into. This was a
        // major failure class in the old fullscreen experiments, so D2 makes the
        // non-alias contract explicit before any state mutation.
        if (s_rawTextureScene == s_rawTextureQuarterA ||
            s_rawSurfaceScene == s_rawSurfaceQuarterA)
            return false;

        void* device = reinterpret_cast<void*>(static_cast<uintptr_t>(s_rawDeviceValue));
        if (!EnsureV10BloomStateBlock(device))
            return false;

        void* sourceTexture = reinterpret_cast<void*>(static_cast<uintptr_t>(s_rawTextureScene));
        void* targetSurface = reinterpret_cast<void*>(static_cast<uintptr_t>(s_rawSurfaceQuarterA));
        void* expectedPreviousRT = targetSurface; // hook fires while stock has Quarter A bound

        D3DSurfaceDescLite targetDesc = {};
        if (!GetSurfaceDescSafe(targetSurface, targetDesc) || targetDesc.Width == 0u || targetDesc.Height == 0u)
            return false;

        void** stateVtable = *reinterpret_cast<void***>(s_v10BloomStateBlock);
        if (!stateVtable || !IsReadableMemory(stateVtable,
            (kD3DStateBlockApplyVtableIndex + 1u) * sizeof(void*)) ||
            !IsExecutableMemory(stateVtable[kD3DStateBlockCaptureVtableIndex]) ||
            !IsExecutableMemory(stateVtable[kD3DStateBlockApplyVtableIndex]))
        {
            ResetV10BloomStateBlock();
            return false;
        }

        const auto capture = reinterpret_cast<D3DStateBlockCapture_t>(stateVtable[kD3DStateBlockCaptureVtableIndex]);
        const auto apply = reinterpret_cast<D3DStateBlockApply_t>(stateVtable[kD3DStateBlockApplyVtableIndex]);
        const HRESULT captureHr = capture(s_v10BloomStateBlock);
        if (FAILED(captureHr))
        {
            s_v10LastScaleSetupHr = captureHr;
            return false;
        }

        void* previousRT = nullptr;
        void* previousDepth = nullptr;
        const HRESULT getRTHr = s_rawGetRenderTarget(device, 0u, &previousRT);
        const HRESULT getDepthHr = s_rawGetDepthStencilSurface(device, &previousDepth);
        const bool depthStateKnown = SUCCEEDED(getDepthHr) || getDepthHr == kD3DErrNotFound;
        if (FAILED(getRTHr) || !previousRT || !depthStateKnown || previousRT != expectedPreviousRT)
        {
            SafeReleaseCom(previousDepth);
            SafeReleaseCom(previousRT);
            s_v10LastScaleSetupHr = FAILED(getRTHr) ? getRTHr : E_FAIL;
            s_v10LastScaleRestoreHr = apply(s_v10BloomStateBlock);
            ++s_v10BloomScaleFailCount;
            return false;
        }

        HRESULT setupHr = S_OK;
        HRESULT drawHr = E_PENDING;
        auto KeepFirstFailure = [&setupHr](HRESULT hr)
        {
            if (SUCCEEDED(setupHr) && FAILED(hr)) setupHr = hr;
        };

        // Retail PC at 007965DB builds the exact dormant VS c0 as
        // {-5*E8FAF8, 0, +5*E8FAF8, 0}.  00796649..007966CC uploads the six
        // down4x4_g5x5offset vectors to PS c0-c5. Xbox Function_82F31348
        // directly samples Scene for the weighted pass and supplies
        // c6={b*.375,b*.25,b*.0625,0}; .49.9 mirrors that source contract.
        const float texelScale = *reinterpret_cast<const float*>(kWeightedBloomVsTexelScale);
        const float vsC0[4] = { texelScale * -5.0f, 0.0f, texelScale * 5.0f, 0.0f };
        const float bloomC6[4] =
        {
            s_v10BloomStrength * 0.375f,
            s_v10BloomStrength * 0.250f,
            s_v10BloomStrength * 0.0625f,
            0.0f
        };

        KeepFirstFailure(s_rawSetTexture(device, 0u, nullptr));
        KeepFirstFailure(s_rawSetTexture(device, 1u, nullptr));
        KeepFirstFailure(s_rawSetDepthStencilSurface(device, nullptr));
        KeepFirstFailure(s_rawSetRenderTarget(device, 0u, targetSurface));

        if (SUCCEEDED(setupHr))
        {
            D3DViewportLite viewport = {};
            viewport.Width = targetDesc.Width;
            viewport.Height = targetDesc.Height;
            viewport.MinZ = 0.0f;
            viewport.MaxZ = 1.0f;
            KeepFirstFailure(s_rawSetViewport(device, &viewport));
            SetMode4OffscreenNoBlendState(device, setupHr);
            SetMode4LinearClampSampler0(device, setupHr);
            KeepFirstFailure(s_rawSetTexture(device, 0u, sourceTexture));
            KeepFirstFailure(s_rawSetVertexShader(device,
                reinterpret_cast<void*>(static_cast<uintptr_t>(weightedVS))));
            KeepFirstFailure(s_rawSetPixelShader(device,
                reinterpret_cast<void*>(static_cast<uintptr_t>(weightedPS))));
            KeepFirstFailure(s_rawSetVertexShaderConstantF(device, 0u, vsC0, 1u));
            for (UINT reg = 0; reg < 6u; ++reg)
            {
                const float* pcC = reinterpret_cast<const float*>(psConstants[reg]);
                KeepFirstFailure(s_rawSetPixelShaderConstantF(device, reg, pcC, 1u));
            }
            KeepFirstFailure(s_rawSetPixelShaderConstantF(device, 6u, bloomC6, 1u));
            drawHr = SUCCEEDED(setupHr) ? DrawMode4NativeFullscreen(device) : setupHr;
        }
        else
        {
            drawHr = setupHr;
        }

        s_rawSetTexture(device, 0u, nullptr);
        s_rawSetTexture(device, 1u, nullptr);
        const HRESULT restoreRTHr = s_rawSetRenderTarget(device, 0u, previousRT);
        const HRESULT restoreDepthHr = s_rawSetDepthStencilSurface(device,
            SUCCEEDED(getDepthHr) ? previousDepth : nullptr);
        const HRESULT stateApplyHr = apply(s_v10BloomStateBlock);

        SafeReleaseCom(previousDepth);
        SafeReleaseCom(previousRT);

        HRESULT restoreHr = S_OK;
        const HRESULT restoreResults[3] = { restoreRTHr, restoreDepthHr, stateApplyHr };
        for (uint32_t n = 0; n < 3u; ++n)
        {
            if (SUCCEEDED(restoreHr) && FAILED(restoreResults[n])) restoreHr = restoreResults[n];
        }

        s_v10LastScaleSetupHr = setupHr;
        s_v10LastScaleDrawHr = drawHr;
        s_v10LastScaleRestoreHr = restoreHr;
        const bool ok = SUCCEEDED(setupHr) && SUCCEEDED(drawHr) && SUCCEEDED(restoreHr);
        if (ok) ++s_v10BloomScaleApplyCount; else ++s_v10BloomScaleFailCount;

        const uint32_t eventCount = ok ? s_v10BloomScaleApplyCount : s_v10BloomScaleFailCount;
        const bool integrationDetail = ShouldLogDebugXboxIntegrationDetail(static_cast<LONG>(eventCount));
        if (integrationDetail || !ok)
        {
            Event* event = ReserveEvent(EventType::BloomScaleApply);
            if (event)
            {
                event->a = s_presentCount;
                event->b = ok ? s_v10BloomScaleApplyCount : s_v10BloomScaleFailCount;
                event->c = ok ? 1u : 0u;
                event->d = FloatBits(s_v10BloomStrength);
                event->e = FloatBits(bloomC6[0]);
                event->f = FloatBits(bloomC6[1]);
                event->g = static_cast<uint32_t>(setupHr);
                event->h = static_cast<uint32_t>(drawHr);
                event->i = static_cast<uint32_t>(restoreHr);
                event->j = s_rawTextureScene;
                event->extra[0] = s_rawSurfaceScene;
                event->extra[1] = s_rawSurfaceQuarterA;
                event->extra[2] = s_rawTextureQuarterA;
                event->extra[3] = targetDesc.Width;
                event->extra[4] = targetDesc.Height;
                event->extra[5] = weightedVS;
                event->extra[6] = weightedPS;
                event->extra[7] = FloatBits(vsC0[0]);
                event->extra[8] = FloatBits(vsC0[2]);
                event->extra[9] = FloatBits(bloomC6[2]);
                event->extra[10] = static_cast<uint32_t>(s_v10Mode);
            }
        }
        return ok;
    }

    bool BuildMode4NativeBloomTailC0(float radiusPixels, float outC0[4])
    {
        if (!outC0 || !(radiusPixels > 0.0f) || s_rawSurfaceScene < 0x10000u)
            return false;

        void* sceneSurface = reinterpret_cast<void*>(static_cast<uintptr_t>(s_rawSurfaceScene));
        D3DSurfaceDescLite sceneDesc = {};
        if (!GetSurfaceDescSafe(sceneSurface, sceneDesc) ||
            sceneDesc.Width == 0u || sceneDesc.Height == 0u)
        {
            return false;
        }

        const float invW = 1.0f / static_cast<float>(sceneDesc.Width);
        const float invH = 1.0f / static_cast<float>(sceneDesc.Height);
        const float x = radiusPixels * invW;
        const float y = radiusPixels * invH;
        outC0[0] = -x;
        outC0[1] = -y;
        outC0[2] = +x;
        outC0[3] = -y;

        return std::isfinite(outC0[0]) && std::isfinite(outC0[1]) &&
            std::isfinite(outC0[2]) && std::isfinite(outC0[3]);
    }

    bool Mode4NativeXboxBloomTailReady()
    {
        if (s_v10Mode != 4 || !s_mode4MidProbeOnly || s_rawHookState != 2 ||
            s_rawDeviceValue < 0x10000u || s_rawSurfaceScene < 0x10000u ||
            s_rawTextureQuarterA < 0x10000u || s_rawSurfaceQuarterA < 0x10000u ||
            s_rawTextureQuarterB < 0x10000u || s_rawSurfaceQuarterB < 0x10000u ||
            !s_rawSetRenderTarget || !s_rawGetRenderTarget ||
            !s_rawSetDepthStencilSurface || !s_rawGetDepthStencilSurface ||
            !s_rawSetTexture || !s_rawSetPixelShader || !s_rawSetVertexShader ||
            !s_rawSetPixelShaderConstantF || !s_rawSetViewport ||
            !s_rawSetRenderState || !s_rawSetSamplerState || !s_rawDrawPrimitive ||
            !s_originalFullscreenQuad || !s_rawCreateStateBlock)
        {
            return false;
        }

        const uint32_t blurPS = ReadU32(kCompositePSBlur);
        const uint32_t level1PS = ReadU32(kCompositePSBlurLevel1);
        const uint32_t level2PS = ReadU32(kCompositePSBlurLevel2);
        const uint32_t finalPS = ReadU32(kCompositePSFinal);
        const uint32_t vs = ReadU32(kCompositeVS);
        const uint32_t shaders[5] = { blurPS, level1PS, level2PS, finalPS, vs };
        for (uint32_t shader : shaders)
        {
            if (shader < 0x10000u || !IsReadableMemory(
                reinterpret_cast<void*>(static_cast<uintptr_t>(shader)), sizeof(void*)))
                return false;
        }

        // V10.5.49.10.1: the live retail-PC path proved that these two shaders
        // do NOT share the Gaussian c0. Validate the recovered full-resolution
        // +/-6 and +/-10 vectors here. The stock pass-2 c0 remains required only
        // for the fail-closed .49.9 Gaussian fallback.
        float level1C0[4] = {};
        float level2C0[4] = {};
        if (!BuildMode4NativeBloomTailC0(kNativeBloomTailLevel1RadiusPixels, level1C0) ||
            !BuildMode4NativeBloomTailC0(kNativeBloomTailLevel2RadiusPixels, level2C0) ||
            !IsReadableMemory(reinterpret_cast<void*>(kCompositeBlurConstPass2), sizeof(float) * 4u))
        {
            return false;
        }

        void* device = reinterpret_cast<void*>(static_cast<uintptr_t>(s_rawDeviceValue));
        return EnsureV10BloomStateBlock(device);
    }

    bool ApplyMode4NativeBlurLevel1State()
    {
        s_nativeBloomTailLevel1PsHr = E_PENDING;
        s_nativeBloomTailLevel1C0Hr = E_PENDING;
        s_nativeBloomTailLevel1RestorePsHr = E_PENDING;
        s_nativeBloomTailLevel1RestoreC0Hr = E_PENDING;
        memset(s_nativeBloomTailLevel1C0, 0, sizeof(s_nativeBloomTailLevel1C0));

        if (!s_thisCompositeV10Scaled || !Mode4NativeXboxBloomTailReady())
            return false;

        void* device = reinterpret_cast<void*>(static_cast<uintptr_t>(s_rawDeviceValue));
        const uint32_t stockBlurPS = ReadU32(kCompositePSBlur);
        const uint32_t level1PS = ReadU32(kCompositePSBlurLevel1);
        float level1C0[4] = {};
        if (!BuildMode4NativeBloomTailC0(kNativeBloomTailLevel1RadiusPixels, level1C0))
        {
            s_nativeBloomTailLevel1C0Hr = E_FAIL;
            s_nativeBloomTailLevel1PsHr = E_FAIL;
            // Fail closed exactly like any other Level1 setup failure.
            s_nativeBloomTailLevel1RestoreC0Hr = s_rawSetPixelShaderConstantF(
                device, 0u, reinterpret_cast<const float*>(kCompositeBlurConstPass2), 1u);
            s_nativeBloomTailLevel1RestorePsHr = s_rawSetPixelShader(
                device, reinterpret_cast<void*>(static_cast<uintptr_t>(stockBlurPS)));
            return false;
        }
        memcpy(s_nativeBloomTailLevel1C0, level1C0, sizeof(level1C0));

        // This hook runs after stock has uploaded the second-Gaussian c0 but
        // before its draw. Replace that draw with the first recovered native
        // tail shader and upload the retail-PC +/-6 full-resolution c0 proved
        // in x32. Restore Gaussian #2 state if either operation fails.
        s_nativeBloomTailLevel1C0Hr = s_rawSetPixelShaderConstantF(device, 0u, level1C0, 1u);
        if (SUCCEEDED(s_nativeBloomTailLevel1C0Hr))
        {
            s_nativeBloomTailLevel1PsHr = s_rawSetPixelShader(
                device, reinterpret_cast<void*>(static_cast<uintptr_t>(level1PS)));
        }
        else
        {
            s_nativeBloomTailLevel1PsHr = s_nativeBloomTailLevel1C0Hr;
        }

        const bool ok = SUCCEEDED(s_nativeBloomTailLevel1C0Hr) &&
            SUCCEEDED(s_nativeBloomTailLevel1PsHr);
        if (ok)
            return true;

        // Fail closed to .49.9: second Gaussian B->A with its original vertical c0.
        s_nativeBloomTailLevel1RestoreC0Hr = s_rawSetPixelShaderConstantF(
            device, 0u, reinterpret_cast<const float*>(kCompositeBlurConstPass2), 1u);
        s_nativeBloomTailLevel1RestorePsHr = s_rawSetPixelShader(
            device, reinterpret_cast<void*>(static_cast<uintptr_t>(stockBlurPS)));
        return false;
    }

    bool RunMode4NativeBlurLevel2Tail()
    {
        s_nativeBloomTailLevel2CaptureHr = E_PENDING;
        s_nativeBloomTailLevel2SetupHr = E_PENDING;
        s_nativeBloomTailLevel2DrawHr = E_PENDING;
        s_nativeBloomTailLevel2RestoreHr = E_PENDING;
        memset(s_nativeBloomTailLevel2C0, 0, sizeof(s_nativeBloomTailLevel2C0));

        if (!s_thisCompositeNativeTailLevel1 || !Mode4NativeXboxBloomTailReady())
            return false;

        void* device = reinterpret_cast<void*>(static_cast<uintptr_t>(s_rawDeviceValue));
        void* quarterATexture = reinterpret_cast<void*>(static_cast<uintptr_t>(s_rawTextureQuarterA));
        void* quarterASurface = reinterpret_cast<void*>(static_cast<uintptr_t>(s_rawSurfaceQuarterA));
        void* quarterBSurface = reinterpret_cast<void*>(static_cast<uintptr_t>(s_rawSurfaceQuarterB));
        const uint32_t vs = ReadU32(kCompositeVS);
        const uint32_t level2PS = ReadU32(kCompositePSBlurLevel2);
        float level2C0[4] = {};
        if (!BuildMode4NativeBloomTailC0(kNativeBloomTailLevel2RadiusPixels, level2C0))
        {
            s_nativeBloomTailLevel2SetupHr = E_FAIL;
            return false;
        }
        memcpy(s_nativeBloomTailLevel2C0, level2C0, sizeof(level2C0));

        D3DSurfaceDescLite descB = {};
        if (!GetSurfaceDescSafe(quarterBSurface, descB) || descB.Width == 0u || descB.Height == 0u)
            return false;

        void** stateVtable = *reinterpret_cast<void***>(s_v10BloomStateBlock);
        if (!stateVtable || !IsReadableMemory(stateVtable,
            (kD3DStateBlockApplyVtableIndex + 1u) * sizeof(void*)) ||
            !IsExecutableMemory(stateVtable[kD3DStateBlockCaptureVtableIndex]) ||
            !IsExecutableMemory(stateVtable[kD3DStateBlockApplyVtableIndex]))
        {
            ResetV10BloomStateBlock();
            return false;
        }
        const auto capture = reinterpret_cast<D3DStateBlockCapture_t>(
            stateVtable[kD3DStateBlockCaptureVtableIndex]);
        const auto apply = reinterpret_cast<D3DStateBlockApply_t>(
            stateVtable[kD3DStateBlockApplyVtableIndex]);

        s_nativeBloomTailLevel2CaptureHr = capture(s_v10BloomStateBlock);
        if (FAILED(s_nativeBloomTailLevel2CaptureHr))
            return false;

        void* previousRT = nullptr;
        void* previousDepth = nullptr;
        const HRESULT getRTHr = s_rawGetRenderTarget(device, 0u, &previousRT);
        const HRESULT getDepthHr = s_rawGetDepthStencilSurface(device, &previousDepth);
        const bool depthStateKnown = SUCCEEDED(getDepthHr) || getDepthHr == kD3DErrNotFound;
        if (FAILED(getRTHr) || previousRT != quarterASurface || !depthStateKnown)
        {
            SafeReleaseCom(previousDepth);
            SafeReleaseCom(previousRT);
            s_nativeBloomTailLevel2RestoreHr = apply(s_v10BloomStateBlock);
            return false;
        }

        HRESULT setupHr = S_OK;
        auto KeepFirstFailure = [&setupHr](HRESULT hr)
        {
            if (SUCCEEDED(setupHr) && FAILED(hr))
                setupHr = hr;
        };

        KeepFirstFailure(s_rawSetTexture(device, 0u, nullptr));
        KeepFirstFailure(s_rawSetTexture(device, 1u, nullptr));
        KeepFirstFailure(s_rawSetDepthStencilSurface(device, nullptr));
        KeepFirstFailure(s_rawSetRenderTarget(device, 0u, quarterBSurface));
        D3DViewportLite viewport = {};
        viewport.Width = descB.Width;
        viewport.Height = descB.Height;
        viewport.MinZ = 0.0f;
        viewport.MaxZ = 1.0f;
        KeepFirstFailure(s_rawSetViewport(device, &viewport));
        SetMode4OffscreenNoBlendState(device, setupHr);
        SetMode4LinearClampSampler0(device, setupHr);
        KeepFirstFailure(s_rawSetTexture(device, 0u, quarterATexture));
        KeepFirstFailure(s_rawSetVertexShader(device,
            reinterpret_cast<void*>(static_cast<uintptr_t>(vs))));
        KeepFirstFailure(s_rawSetPixelShader(device,
            reinterpret_cast<void*>(static_cast<uintptr_t>(level2PS))));
        // Retail-PC runtime proof: BlurLevel2 uses +/-10 pixels normalized by
        // the full scene dimensions, not the preceding Gaussian c0.
        KeepFirstFailure(s_rawSetPixelShaderConstantF(device, 0u, level2C0, 1u));

        s_nativeBloomTailLevel2SetupHr = setupHr;
        s_nativeBloomTailLevel2DrawHr = SUCCEEDED(setupHr)
            ? DrawMode4NativeFullscreen(device) : setupHr;

        s_rawSetTexture(device, 0u, nullptr);
        s_rawSetTexture(device, 1u, nullptr);
        const HRESULT restoreRTHr = s_rawSetRenderTarget(device, 0u, previousRT);
        const HRESULT restoreDepthHr = s_rawSetDepthStencilSurface(device,
            SUCCEEDED(getDepthHr) ? previousDepth : nullptr);
        const HRESULT applyHr = apply(s_v10BloomStateBlock);

        SafeReleaseCom(previousDepth);
        SafeReleaseCom(previousRT);

        s_nativeBloomTailLevel2RestoreHr = S_OK;
        const HRESULT restoreResults[3] = { restoreRTHr, restoreDepthHr, applyHr };
        for (uint32_t n = 0; n < 3u; ++n)
        {
            if (SUCCEEDED(s_nativeBloomTailLevel2RestoreHr) && FAILED(restoreResults[n]))
                s_nativeBloomTailLevel2RestoreHr = restoreResults[n];
        }

        return SUCCEEDED(s_nativeBloomTailLevel2CaptureHr) &&
            SUCCEEDED(s_nativeBloomTailLevel2SetupHr) &&
            SUCCEEDED(s_nativeBloomTailLevel2DrawHr) &&
            SUCCEEDED(s_nativeBloomTailLevel2RestoreHr);
    }

    bool RestoreMode4StockGaussian2Fallback()
    {
        s_nativeBloomTailFallbackGaussianHr = E_PENDING;
        if (!Mode4NativeXboxBloomTailReady())
            return false;

        void* device = reinterpret_cast<void*>(static_cast<uintptr_t>(s_rawDeviceValue));
        void* quarterBTexture = reinterpret_cast<void*>(static_cast<uintptr_t>(s_rawTextureQuarterB));
        void* quarterASurface = reinterpret_cast<void*>(static_cast<uintptr_t>(s_rawSurfaceQuarterA));
        const uint32_t vs = ReadU32(kCompositeVS);
        const uint32_t blurPS = ReadU32(kCompositePSBlur);

        D3DSurfaceDescLite descA = {};
        if (!GetSurfaceDescSafe(quarterASurface, descA) || descA.Width == 0u || descA.Height == 0u)
            return false;

        void** stateVtable = *reinterpret_cast<void***>(s_v10BloomStateBlock);
        if (!stateVtable || !IsReadableMemory(stateVtable,
            (kD3DStateBlockApplyVtableIndex + 1u) * sizeof(void*)) ||
            !IsExecutableMemory(stateVtable[kD3DStateBlockCaptureVtableIndex]) ||
            !IsExecutableMemory(stateVtable[kD3DStateBlockApplyVtableIndex]))
            return false;

        const auto capture = reinterpret_cast<D3DStateBlockCapture_t>(
            stateVtable[kD3DStateBlockCaptureVtableIndex]);
        const auto apply = reinterpret_cast<D3DStateBlockApply_t>(
            stateVtable[kD3DStateBlockApplyVtableIndex]);
        const HRESULT captureHr = capture(s_v10BloomStateBlock);
        if (FAILED(captureHr))
        {
            s_nativeBloomTailFallbackGaussianHr = captureHr;
            return false;
        }

        void* previousRT = nullptr;
        void* previousDepth = nullptr;
        const HRESULT getRTHr = s_rawGetRenderTarget(device, 0u, &previousRT);
        const HRESULT getDepthHr = s_rawGetDepthStencilSurface(device, &previousDepth);
        const bool depthStateKnown = SUCCEEDED(getDepthHr) || getDepthHr == kD3DErrNotFound;
        if (FAILED(getRTHr) || previousRT != quarterASurface || !depthStateKnown)
        {
            SafeReleaseCom(previousDepth);
            SafeReleaseCom(previousRT);
            const HRESULT applyHr = apply(s_v10BloomStateBlock);
            s_nativeBloomTailFallbackGaussianHr = FAILED(getRTHr) ? getRTHr :
                (FAILED(applyHr) ? applyHr : E_FAIL);
            return false;
        }

        HRESULT setupHr = S_OK;
        auto KeepFirstFailure = [&setupHr](HRESULT hr)
        {
            if (SUCCEEDED(setupHr) && FAILED(hr))
                setupHr = hr;
        };

        KeepFirstFailure(s_rawSetTexture(device, 0u, nullptr));
        KeepFirstFailure(s_rawSetTexture(device, 1u, nullptr));
        KeepFirstFailure(s_rawSetDepthStencilSurface(device, nullptr));
        KeepFirstFailure(s_rawSetRenderTarget(device, 0u, quarterASurface));
        D3DViewportLite viewport = {};
        viewport.Width = descA.Width;
        viewport.Height = descA.Height;
        viewport.MinZ = 0.0f;
        viewport.MaxZ = 1.0f;
        KeepFirstFailure(s_rawSetViewport(device, &viewport));
        SetMode4OffscreenNoBlendState(device, setupHr);
        SetMode4LinearClampSampler0(device, setupHr);
        KeepFirstFailure(s_rawSetTexture(device, 0u, quarterBTexture));
        KeepFirstFailure(s_rawSetVertexShader(device,
            reinterpret_cast<void*>(static_cast<uintptr_t>(vs))));
        KeepFirstFailure(s_rawSetPixelShader(device,
            reinterpret_cast<void*>(static_cast<uintptr_t>(blurPS))));
        KeepFirstFailure(s_rawSetPixelShaderConstantF(device, 0u,
            reinterpret_cast<const float*>(kCompositeBlurConstPass2), 1u));
        const HRESULT drawHr = SUCCEEDED(setupHr)
            ? DrawMode4NativeFullscreen(device) : setupHr;

        s_rawSetTexture(device, 0u, nullptr);
        s_rawSetTexture(device, 1u, nullptr);
        const HRESULT restoreRTHr = s_rawSetRenderTarget(device, 0u, previousRT);
        const HRESULT restoreDepthHr = s_rawSetDepthStencilSurface(device,
            SUCCEEDED(getDepthHr) ? previousDepth : nullptr);
        const HRESULT applyHr = apply(s_v10BloomStateBlock);

        SafeReleaseCom(previousDepth);
        SafeReleaseCom(previousRT);

        HRESULT finalHr = S_OK;
        const HRESULT hrs[5] = { setupHr, drawHr, restoreRTHr, restoreDepthHr, applyHr };
        for (uint32_t n = 0; n < 5u; ++n)
        {
            if (SUCCEEDED(finalHr) && FAILED(hrs[n]))
                finalHr = hrs[n];
        }
        s_nativeBloomTailFallbackGaussianHr = finalHr;
        return SUCCEEDED(finalHr);
    }

    void QueueMode4NativeBloomTailEvent(uint32_t finalSource)
    {
        // Do not count early frames where the frozen weighted Scene pass itself
        // was not ready; the tail only exists after that prerequisite succeeds.
        if (!s_thisCompositeV10Scaled)
            return;

        const bool ok = s_thisCompositeNativeTailLevel1 && s_thisCompositeNativeTailLevel2;
        const LONG count = ok
            ? InterlockedIncrement(&s_nativeBloomTailSuccessCount)
            : InterlockedIncrement(&s_nativeBloomTailFailureCount);
        if (!ShouldLogSparse(count))
            return;

        Event* event = ReserveEvent(EventType::NativeBloomTail);
        if (!event)
            return;

        event->a = s_presentCount;
        event->b = static_cast<uint32_t>(s_currentCompositeCall);
        event->c = s_thisCompositeNativeTailLevel1 ? 1u : 0u;
        event->d = s_thisCompositeNativeTailLevel2 ? 1u : 0u;
        event->e = ReadU32(kCompositePSBlurLevel1);
        event->f = ReadU32(kCompositePSBlurLevel2);
        event->g = FloatBits(s_nativeBloomTailLevel1C0[0]);
        event->h = FloatBits(s_nativeBloomTailLevel1C0[1]);
        event->i = FloatBits(s_nativeBloomTailLevel1C0[2]);
        event->j = FloatBits(s_nativeBloomTailLevel1C0[3]);
        event->extra[0] = static_cast<uint32_t>(s_nativeBloomTailLevel1PsHr);
        event->extra[1] = static_cast<uint32_t>(s_nativeBloomTailLevel1C0Hr);
        event->extra[2] = static_cast<uint32_t>(s_nativeBloomTailLevel1RestorePsHr);
        event->extra[3] = static_cast<uint32_t>(s_nativeBloomTailLevel1RestoreC0Hr);
        event->extra[4] = static_cast<uint32_t>(s_nativeBloomTailLevel2CaptureHr);
        event->extra[5] = static_cast<uint32_t>(s_nativeBloomTailLevel2SetupHr);
        event->extra[6] = static_cast<uint32_t>(s_nativeBloomTailLevel2DrawHr);
        event->extra[7] = static_cast<uint32_t>(s_nativeBloomTailLevel2RestoreHr);
        event->extra[8] = finalSource;
        event->extra[9] = static_cast<uint32_t>(s_nativeBloomTailSuccessCount);
        event->extra[10] = static_cast<uint32_t>(s_nativeBloomTailFailureCount);
        event->extra[11] = FloatBits(s_nativeBloomTailLevel2C0[0]);
        event->extra[12] = FloatBits(s_nativeBloomTailLevel2C0[1]);
        event->extra[13] = FloatBits(s_nativeBloomTailLevel2C0[2]);
        event->extra[14] = FloatBits(s_nativeBloomTailLevel2C0[3]);
        event->extra[15] = static_cast<uint32_t>(s_nativeBloomTailFallbackGaussianHr);
    }

    bool Mode4FullDofFollowupReady()
    {
        if (s_v10Mode != 4 || !s_mode4MidProbeOnly || s_rawHookState != 2 ||
            s_rawDeviceValue < 0x10000u ||
            s_rawTextureScene < 0x10000u || s_rawSurfaceScene < 0x10000u ||
            s_rawTextureQuarterA < 0x10000u || s_rawSurfaceQuarterA < 0x10000u ||
            s_rawTextureQuarterB < 0x10000u || s_rawSurfaceQuarterB < 0x10000u ||
            !s_rawSetRenderTarget || !s_rawGetRenderTarget ||
            !s_rawSetDepthStencilSurface || !s_rawGetDepthStencilSurface ||
            !s_rawSetTexture || !s_rawSetPixelShader || !s_rawSetVertexShader ||
            !s_rawSetPixelShaderConstantF || !s_rawSetViewport ||
            !s_rawSetRenderState || !s_rawSetSamplerState || !s_rawDrawPrimitive ||
            !s_originalFullscreenQuad || !s_rawCreateStateBlock)
        {
            return false;
        }

        const uint32_t vs = ReadU32(kCompositeVS);
        const uint32_t initialPS = ReadU32(kCompositePSInitial);
        const uint32_t blurPS = ReadU32(kCompositePSBlur);
        const uint32_t finalPS = ReadU32(kCompositePSFinal);
        if (vs < 0x10000u || initialPS < 0x10000u || blurPS < 0x10000u || finalPS < 0x10000u)
            return false;
        if (!IsReadableMemory(reinterpret_cast<void*>(static_cast<uintptr_t>(vs)), sizeof(void*)) ||
            !IsReadableMemory(reinterpret_cast<void*>(static_cast<uintptr_t>(initialPS)), sizeof(void*)) ||
            !IsReadableMemory(reinterpret_cast<void*>(static_cast<uintptr_t>(blurPS)), sizeof(void*)) ||
            !IsReadableMemory(reinterpret_cast<void*>(static_cast<uintptr_t>(finalPS)), sizeof(void*)))
            return false;

        if (!IsReadableMemory(reinterpret_cast<void*>(kCompositeDownsampleOffsetX), sizeof(float)) ||
            !IsReadableMemory(reinterpret_cast<void*>(kCompositeDownsampleOffsetY), sizeof(float)) ||
            !IsReadableMemory(reinterpret_cast<void*>(kCompositeBlurConstPass1), sizeof(float) * 4u) ||
            !IsReadableMemory(reinterpret_cast<void*>(kCompositeBlurConstPass2), sizeof(float) * 4u) ||
            !IsReadableMemory(reinterpret_cast<void*>(kFullscreenQuadRectA), 16u) ||
            !IsReadableMemory(reinterpret_cast<void*>(kFullscreenQuadRectB), 16u))
            return false;

        void* device = reinterpret_cast<void*>(static_cast<uintptr_t>(s_rawDeviceValue));
        return EnsureV10BloomStateBlock(device);
    }

    HRESULT DrawMode4NativeFullscreen(void* device)
    {
        if (!device || !s_originalFullscreenQuad || !s_rawDrawPrimitive)
            return E_POINTER;

        s_originalFullscreenQuad(
            reinterpret_cast<void*>(kFullscreenQuadRectA),
            reinterpret_cast<void*>(kFullscreenQuadRectB), 0u, 0);
        return s_rawDrawPrimitive(device, kD3DPrimitiveTriangleStrip, 0u, 2u);
    }

    void SetMode4LinearClampSampler0(void* device, HRESULT& firstFailure)
    {
        auto KeepFirstFailure = [&firstFailure](HRESULT hr)
        {
            if (SUCCEEDED(firstFailure) && FAILED(hr))
                firstFailure = hr;
        };
        KeepFirstFailure(s_rawSetSamplerState(device, 0u, kSampAddressU, kTextureAddressClamp));
        KeepFirstFailure(s_rawSetSamplerState(device, 0u, kSampAddressV, kTextureAddressClamp));
        KeepFirstFailure(s_rawSetSamplerState(device, 0u, kSampMagFilter, kTextureFilterLinear));
        KeepFirstFailure(s_rawSetSamplerState(device, 0u, kSampMinFilter, kTextureFilterLinear));
        KeepFirstFailure(s_rawSetSamplerState(device, 0u, kSampMipFilter, kTextureFilterLinear));
        KeepFirstFailure(s_rawSetSamplerState(device, 0u, kSampMaxAnisotropy, 1u));
        KeepFirstFailure(s_rawSetSamplerState(device, 0u, kSampSrgbTexture, 0u));
    }

    void SetMode4OffscreenNoBlendState(void* device, HRESULT& firstFailure)
    {
        auto KeepFirstFailure = [&firstFailure](HRESULT hr)
        {
            if (SUCCEEDED(firstFailure) && FAILED(hr))
                firstFailure = hr;
        };
        KeepFirstFailure(s_rawSetRenderState(device, kRSZEnable, 0u));
        KeepFirstFailure(s_rawSetRenderState(device, kRSZWriteEnable, 0u));
        KeepFirstFailure(s_rawSetRenderState(device, kRSAlphaTestEnable, 0u));
        KeepFirstFailure(s_rawSetRenderState(device, kRSCullMode, 1u));
        KeepFirstFailure(s_rawSetRenderState(device, kRSAlphaBlendEnable, 0u));
        KeepFirstFailure(s_rawSetRenderState(device, kRSFogEnable, 0u));
        KeepFirstFailure(s_rawSetRenderState(device, kRSStencilEnable, 0u));
        KeepFirstFailure(s_rawSetRenderState(device, kRSLighting, 0u));
        KeepFirstFailure(s_rawSetRenderState(device, kRSColorWriteEnable, 0xFu));
        KeepFirstFailure(s_rawSetRenderState(device, kRSScissorTestEnable, 0u));
        KeepFirstFailure(s_rawSetRenderState(device, kRSSrgbWriteEnable, 0u));
    }

    bool RunMode4FullDofFollowupBlur()
    {
        const LONG attempt = InterlockedIncrement(&s_mode4FullDofAttemptCount);
        s_mode4FullDofCaptureHr = E_PENDING;
        s_mode4FullDofDownsampleHr = E_PENDING;
        s_mode4FullDofBlur1Hr = E_PENDING;
        s_mode4FullDofBlur2Hr = E_PENDING;
        s_mode4FullDofRestoreHr = E_PENDING;
        s_mode4FullDofFinalHr = E_PENDING;

        if (!Mode4FullDofFollowupReady())
        {
            ++s_mode4FullDofFailureCount;
            return false;
        }

        void* device = reinterpret_cast<void*>(static_cast<uintptr_t>(s_rawDeviceValue));
        void* sceneTexture = reinterpret_cast<void*>(static_cast<uintptr_t>(s_rawTextureScene));
        void* sceneSurface = reinterpret_cast<void*>(static_cast<uintptr_t>(s_rawSurfaceScene));
        void* quarterATexture = reinterpret_cast<void*>(static_cast<uintptr_t>(s_rawTextureQuarterA));
        void* quarterASurface = reinterpret_cast<void*>(static_cast<uintptr_t>(s_rawSurfaceQuarterA));
        void* quarterBTexture = reinterpret_cast<void*>(static_cast<uintptr_t>(s_rawTextureQuarterB));
        void* quarterBSurface = reinterpret_cast<void*>(static_cast<uintptr_t>(s_rawSurfaceQuarterB));
        const uint32_t vs = ReadU32(kCompositeVS);
        // V10.5.49.4 one-variable visual correction: use the simple final-copy
        // pixel shader for PASS 2 so Quarter A receives neutral scene RGB rather
        // than bloom-extracted/weighted RGB.
        const uint32_t sceneDownsamplePS = ReadU32(kCompositePSFinal);
        const uint32_t blurPS = ReadU32(kCompositePSBlur);

        D3DSurfaceDescLite descA = {}, descB = {};
        if (!GetSurfaceDescSafe(quarterASurface, descA) || !GetSurfaceDescSafe(quarterBSurface, descB) ||
            descA.Width == 0u || descA.Height == 0u || descB.Width == 0u || descB.Height == 0u)
        {
            ++s_mode4FullDofFailureCount;
            return false;
        }

        void** stateVtable = *reinterpret_cast<void***>(s_v10BloomStateBlock);
        if (!stateVtable || !IsReadableMemory(stateVtable,
            (kD3DStateBlockApplyVtableIndex + 1u) * sizeof(void*)) ||
            !IsExecutableMemory(stateVtable[kD3DStateBlockCaptureVtableIndex]) ||
            !IsExecutableMemory(stateVtable[kD3DStateBlockApplyVtableIndex]))
        {
            ResetV10BloomStateBlock();
            ++s_mode4FullDofFailureCount;
            return false;
        }

        const auto capture = reinterpret_cast<D3DStateBlockCapture_t>(
            stateVtable[kD3DStateBlockCaptureVtableIndex]);
        const auto apply = reinterpret_cast<D3DStateBlockApply_t>(
            stateVtable[kD3DStateBlockApplyVtableIndex]);

        s_mode4FullDofCaptureHr = capture(s_v10BloomStateBlock);
        if (FAILED(s_mode4FullDofCaptureHr))
        {
            ++s_mode4FullDofFailureCount;
            return false;
        }

        void* previousRT = nullptr;
        void* previousDepth = nullptr;
        const HRESULT getRTHr = s_rawGetRenderTarget(device, 0u, &previousRT);
        const HRESULT getDepthHr = s_rawGetDepthStencilSurface(device, &previousDepth);
        const bool depthStateKnown = SUCCEEDED(getDepthHr) || getDepthHr == kD3DErrNotFound;
        if (FAILED(getRTHr) || !previousRT || !depthStateKnown || previousRT != sceneSurface)
        {
            SafeReleaseCom(previousDepth);
            SafeReleaseCom(previousRT);
            s_mode4FullDofRestoreHr = apply(s_v10BloomStateBlock);
            ++s_mode4FullDofFailureCount;
            return false;
        }

        HRESULT setupHr = S_OK;
        auto KeepFirstFailure = [&setupHr](HRESULT hr)
        {
            if (SUCCEEDED(setupHr) && FAILED(hr))
                setupHr = hr;
        };
        auto SetTarget = [&](void* surface, const D3DSurfaceDescLite& desc)
        {
            KeepFirstFailure(s_rawSetTexture(device, 0u, nullptr));
            KeepFirstFailure(s_rawSetTexture(device, 1u, nullptr));
            KeepFirstFailure(s_rawSetRenderTarget(device, 0u, surface));
            D3DViewportLite viewport = {};
            viewport.Width = desc.Width;
            viewport.Height = desc.Height;
            viewport.MinZ = 0.0f;
            viewport.MaxZ = 1.0f;
            KeepFirstFailure(s_rawSetViewport(device, &viewport));
        };

        KeepFirstFailure(s_rawSetDepthStencilSurface(device, nullptr));
        SetMode4OffscreenNoBlendState(device, setupHr);
        SetMode4LinearClampSampler0(device, setupHr);

        // Pass 2: neutral full-scene -> quarter A downsample.
        //
        // V10.5.49.3 proved followup=1/final=1 with every HRESULT == S_OK, but
        // the gameplay remained severely white/hazy.  The remaining incorrect
        // approximation was using the retail bloom initial PS here.  That shader
        // is appropriate for the bloom chain, not for a blurred copy of the whole
        // scene.  Render the scene through the stock simple colorSampler shader
        // (the same shader used by the final PC copy/composite) so bilinear target
        // minification produces a neutral quarter-resolution scene.
        //
        // Keep the exact dormant 00AC20F8/00AC2208 console-style downsample pair
        // for a later fidelity step after this brightness diagnosis is isolated.
        SetTarget(quarterASurface, descA);
        KeepFirstFailure(s_rawSetTexture(device, 0u, sceneTexture));
        KeepFirstFailure(s_rawSetVertexShader(device,
            reinterpret_cast<void*>(static_cast<uintptr_t>(vs))));
        KeepFirstFailure(s_rawSetPixelShader(device,
            reinterpret_cast<void*>(static_cast<uintptr_t>(sceneDownsamplePS))));
        s_mode4FullDofDownsampleHr = SUCCEEDED(setupHr) ? DrawMode4NativeFullscreen(device) : setupHr;

        // Pass 3a: quarter A -> quarter B, first stock Gaussian direction.
        if (SUCCEEDED(setupHr) && SUCCEEDED(s_mode4FullDofDownsampleHr))
        {
            SetTarget(quarterBSurface, descB);
            KeepFirstFailure(s_rawSetTexture(device, 0u, quarterATexture));
            KeepFirstFailure(s_rawSetVertexShader(device,
                reinterpret_cast<void*>(static_cast<uintptr_t>(vs))));
            KeepFirstFailure(s_rawSetPixelShader(device,
                reinterpret_cast<void*>(static_cast<uintptr_t>(blurPS))));
            const float* blurC0 = reinterpret_cast<const float*>(kCompositeBlurConstPass1);
            KeepFirstFailure(s_rawSetPixelShaderConstantF(device, 0u, blurC0, 1u));
            s_mode4FullDofBlur1Hr = SUCCEEDED(setupHr) ? DrawMode4NativeFullscreen(device) : setupHr;
        }
        else
        {
            s_mode4FullDofBlur1Hr = FAILED(setupHr) ? setupHr : s_mode4FullDofDownsampleHr;
        }

        // Pass 3b: quarter B -> quarter A, second stock Gaussian direction.
        if (SUCCEEDED(setupHr) && SUCCEEDED(s_mode4FullDofBlur1Hr))
        {
            SetTarget(quarterASurface, descA);
            KeepFirstFailure(s_rawSetTexture(device, 0u, quarterBTexture));
            const float* blurC0 = reinterpret_cast<const float*>(kCompositeBlurConstPass2);
            KeepFirstFailure(s_rawSetPixelShaderConstantF(device, 0u, blurC0, 1u));
            s_mode4FullDofBlur2Hr = SUCCEEDED(setupHr) ? DrawMode4NativeFullscreen(device) : setupHr;
        }
        else
        {
            s_mode4FullDofBlur2Hr = FAILED(setupHr) ? setupHr : s_mode4FullDofBlur1Hr;
        }

        // Return to the exact state left by the first DOF/mask draw.  In .49.10.1
        // that captured state can have Quarter B on stage 0 because the recovered
        // pre-DOF bloom tail finishes in B.  The DOF follow-up itself remains the
        // frozen .49.4 Scene->A, Gaussian A->B->A algorithm, so explicitly rebind
        // Quarter A after the state-block restore before the final masked draw.
        s_rawSetTexture(device, 0u, nullptr);
        s_rawSetTexture(device, 1u, nullptr);
        const HRESULT restoreRTHr = s_rawSetRenderTarget(device, 0u, previousRT);
        const HRESULT restoreDepthHr = s_rawSetDepthStencilSurface(device,
            SUCCEEDED(getDepthHr) ? previousDepth : nullptr);
        const HRESULT applyHr = apply(s_v10BloomStateBlock);
        HRESULT rebindQuarterAHr = applyHr;
        if (SUCCEEDED(applyHr))
        {
            rebindQuarterAHr = s_rawSetTexture(device, 0u, quarterATexture);
            if (SUCCEEDED(rebindQuarterAHr))
            {
                s_rawTextureStages[0] = s_rawTextureQuarterA;
                WriteU32(kNglTextureCacheBase, s_rawTextureQuarterA);
            }
        }

        SafeReleaseCom(previousDepth);
        SafeReleaseCom(previousRT);

        s_mode4FullDofRestoreHr = S_OK;
        const HRESULT restoreResults[4] =
            { restoreRTHr, restoreDepthHr, applyHr, rebindQuarterAHr };
        for (uint32_t n = 0; n < 4u; ++n)
        {
            if (SUCCEEDED(s_mode4FullDofRestoreHr) && FAILED(restoreResults[n]))
                s_mode4FullDofRestoreHr = restoreResults[n];
        }

        const bool ok = SUCCEEDED(setupHr) &&
            SUCCEEDED(s_mode4FullDofDownsampleHr) && SUCCEEDED(s_mode4FullDofBlur1Hr) &&
            SUCCEEDED(s_mode4FullDofBlur2Hr) && SUCCEEDED(s_mode4FullDofRestoreHr);
        if (ok)
            ++s_mode4FullDofSuccessCount;
        else
            ++s_mode4FullDofFailureCount;

        (void)attempt;
        return ok;
    }

    bool DrawMode4FullDofFinalComposite()
    {
        if (s_provenIntzDofState != 3 || !s_rawDrawPrimitive || !s_originalFullscreenQuad)
            return false;

        void* device = reinterpret_cast<void*>(static_cast<uintptr_t>(s_rawDeviceValue));
        if (!device)
            return false;

        // Pass 4: the restored stock simple shader samples blurred quarter A.
        // 7/8 is the PC equivalent of the Xbox final DstAlpha/InvDstAlpha
        // composite, so the depth mask written into scene alpha by pass 1 now
        // controls how much blurred scene is mixed back into scene RGB.
        if (s_nglSetBlendState)
            s_nglSetBlendState(7u, 8u, 0u, 1u, 1u, 0u, 1u, 1u);

        s_mode4FullDofFinalHr = DrawMode4NativeFullscreen(device);
        if (SUCCEEDED(s_mode4FullDofFinalHr))
        {
            ++s_mode4FullDofFinalDrawCount;
            return true;
        }
        ++s_mode4FullDofFailureCount;
        return false;
    }

    void QueueMode4FullDofChainEvent(bool followupOk, bool finalOk)
    {
        const LONG successes = s_mode4FullDofSuccessCount;
        const LONG failures = s_mode4FullDofFailureCount;
        const LONG ordinal = successes + failures;
        if (ordinal > 16 && (ordinal % 120) != 0 && followupOk && finalOk)
            return;

        Event* event = ReserveEvent(EventType::FullDofChain);
        if (!event)
            return;
        event->a = s_presentCount;
        event->b = static_cast<uint32_t>(s_mode4FullDofAttemptCount);
        event->c = followupOk ? 1u : 0u;
        event->d = finalOk ? 1u : 0u;
        event->e = static_cast<uint32_t>(s_mode4FullDofCaptureHr);
        event->f = static_cast<uint32_t>(s_mode4FullDofDownsampleHr);
        event->g = static_cast<uint32_t>(s_mode4FullDofBlur1Hr);
        event->h = static_cast<uint32_t>(s_mode4FullDofBlur2Hr);
        event->i = static_cast<uint32_t>(s_mode4FullDofRestoreHr);
        event->j = static_cast<uint32_t>(s_mode4FullDofFinalHr);
        event->extra[0] = s_rawTextureScene;
        event->extra[1] = s_rawTextureQuarterA;
        event->extra[2] = s_rawTextureQuarterB;
        event->extra[3] = s_rawSurfaceScene;
        event->extra[4] = s_rawSurfaceQuarterA;
        event->extra[5] = s_rawSurfaceQuarterB;
        event->extra[6] = ReadU32(kCompositePSDofFinal);
        event->extra[7] = ReadU32(kCompositePSFinal);
        event->extra[8] = ReadU32(kCompositePSBlur);
        event->extra[9] = ReadU32(kCompositePSFinal);
        event->extra[10] = ReadU32(kCompositeVS);
        event->extra[11] = static_cast<uint32_t>(successes);
        event->extra[12] = static_cast<uint32_t>(failures);
        event->extra[13] = static_cast<uint32_t>(s_mode4FullDofFinalDrawCount);
        event->extra[14] = s_provenIntzDofPrevPS;
        event->extra[15] = s_provenIntzDofState;
    }

    void ResetMode4CameraMotionZBlurController()
    {
        s_cameraMotionZBlurControllerValid = false;
        s_cameraMotionZBlurHistoryValid = false;
        s_cameraMotionZBlurCameraValue = 0u;
        s_cameraMotionZBlurTransformValue = 0u;
        s_cameraMotionZBlurHistoryIndex = 0u;
        memset(s_cameraMotionZBlurPrevPosition, 0, sizeof(s_cameraMotionZBlurPrevPosition));
        memset(s_cameraMotionZBlurMotionHistory, 0, sizeof(s_cameraMotionZBlurMotionHistory));
        s_cameraMotionZBlurLastDelta = 0.0f;
        s_cameraMotionZBlurLastAverage = 0.0f;
        s_cameraMotionZBlurLastMotion = 0.0f;
        s_cameraMotionZBlurLastBlurPixels = kXboxCameraZBlurMin;
    }

    void ResetMode4CameraMotionZBlurResources()
    {
        ResetMode4CameraMotionZBlurController();
        SafeReleaseCom(s_cameraMotionZBlurCopySurface);
        SafeReleaseCom(s_cameraMotionZBlurCopyTexture);
        s_cameraMotionZBlurWidth = 0u;
        s_cameraMotionZBlurHeight = 0u;
        s_cameraMotionZBlurFormat = 0u;
        s_cameraMotionZBlurCreateHr = E_PENDING;
        InterlockedExchange(&s_cameraMotionZBlurCreateState, 0);
    }

    void ResetRetailXboxImageZoomProbeResources()
    {
        SafeReleaseCom(s_retailXboxImageZoomSourceSurface);
        SafeReleaseCom(s_retailXboxImageZoomSourceTexture);
        s_retailXboxImageZoomSourceWidth = 0u;
        s_retailXboxImageZoomSourceHeight = 0u;
        s_retailXboxImageZoomSourceFormat = 0u;
    }

    bool ReadRetailPcMainCameraPosition(float outPosition[3])
    {
        if (!outPosition || !IsReadableMemory(reinterpret_cast<void*>(kGameSingletonGlobal), sizeof(uint32_t)))
            return false;
        const uint32_t gameValue = ReadU32(kGameSingletonGlobal);
        if (gameValue < 0x10000u || !IsReadableMemory(reinterpret_cast<void*>(static_cast<uintptr_t>(gameValue) + kGameSpiderCameraOffset), sizeof(uint32_t)))
            return false;
        const uint32_t cameraValue = ReadU32(static_cast<uintptr_t>(gameValue) + kGameSpiderCameraOffset);
        if (cameraValue < 0x10000u || !IsReadableMemory(reinterpret_cast<void*>(static_cast<uintptr_t>(cameraValue) + kEntityTransformOffset), sizeof(uint32_t)))
            return false;
        const uint32_t transformValue = ReadU32(static_cast<uintptr_t>(cameraValue) + kEntityTransformOffset);
        if (transformValue < 0x10000u || !IsReadableMemory(reinterpret_cast<void*>(static_cast<uintptr_t>(transformValue) + kEntityTransformPositionOffset), sizeof(float) * 3u))
            return false;
        const volatile float* position = reinterpret_cast<volatile float*>(static_cast<uintptr_t>(transformValue) + kEntityTransformPositionOffset);
        const float x = position[0], y = position[1], z = position[2];
        if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z))
            return false;
        outPosition[0] = x; outPosition[1] = y; outPosition[2] = z;
        s_cameraMotionZBlurCameraValue = cameraValue;
        s_cameraMotionZBlurTransformValue = transformValue;
        return true;
    }

    float UpdateMode4CameraMotionZBlurScale()
    {
        float position[3] = {};
        if (!ReadRetailPcMainCameraPosition(position))
        {
            ResetMode4CameraMotionZBlurController();
            return kCameraMotionZBlurFixedMinScale;
        }
        s_cameraMotionZBlurControllerValid = true;
        float delta = 0.0f;
        if (!s_cameraMotionZBlurHistoryValid)
        {
            memcpy(s_cameraMotionZBlurPrevPosition, position, sizeof(s_cameraMotionZBlurPrevPosition));
            memset(s_cameraMotionZBlurMotionHistory, 0, sizeof(s_cameraMotionZBlurMotionHistory));
            s_cameraMotionZBlurHistoryIndex = 0u;
            s_cameraMotionZBlurHistoryValid = true;
        }
        else
        {
            const float dx = position[0] - s_cameraMotionZBlurPrevPosition[0];
            const float dy = position[1] - s_cameraMotionZBlurPrevPosition[1];
            const float dz = position[2] - s_cameraMotionZBlurPrevPosition[2];
            delta = sqrtf(dx * dx + dy * dy + dz * dz);
            if (!std::isfinite(delta) || delta < 0.0f) delta = 0.0f;
            memcpy(s_cameraMotionZBlurPrevPosition, position, sizeof(s_cameraMotionZBlurPrevPosition));
        }
        s_cameraMotionZBlurMotionHistory[s_cameraMotionZBlurHistoryIndex] = delta;
        s_cameraMotionZBlurHistoryIndex = (s_cameraMotionZBlurHistoryIndex + 1u) & (kXboxCameraZBlurHistoryCount - 1u);
        float average = 0.0f;
        for (uint32_t i = 0u; i < kXboxCameraZBlurHistoryCount; ++i) average += s_cameraMotionZBlurMotionHistory[i];
        average *= 0.25f;
        float motion = average / kXboxCameraZBlurDistanceForMax;
        if (motion < 0.0f) motion = 0.0f;
        if (motion > 1.0f) motion = 1.0f;
        const float blurPixels = kXboxCameraZBlurMin + (kXboxCameraZBlurMax - kXboxCameraZBlurMin) * motion;
        const float scale = (kXboxCameraZBlurReferenceWidth + blurPixels) / kXboxCameraZBlurReferenceWidth;
        s_cameraMotionZBlurLastDelta = delta;
        s_cameraMotionZBlurLastAverage = average;
        s_cameraMotionZBlurLastMotion = motion;
        s_cameraMotionZBlurLastBlurPixels = blurPixels;
        return std::isfinite(scale) ? scale : kCameraMotionZBlurFixedMinScale;
    }

    bool EvaluateNativePcCameraZBlurGate()
    {
        const void* gateAddress = reinterpret_cast<const void*>(kPcCameraZBlurGateByte);
        s_cameraMotionZBlurGateReadable = IsReadableMemory(gateAddress, sizeof(uint8_t));

        // Stock game::render already executed:
        //     byte_D16584 = sub_8094B0();
        // before entering the post-FX tail.  Reading that byte is therefore both
        // temporally native and side-effect free.
        bool gate = false;
        if (s_cameraMotionZBlurGateReadable)
        {
            gate = *reinterpret_cast<volatile const uint8_t*>(kPcCameraZBlurGateByte) != 0u;
        }

        s_cameraMotionZBlurGateLast = gate;
        InterlockedIncrement(&s_cameraMotionZBlurGateCalls);
        if (!gate)
            InterlockedIncrement(&s_cameraMotionZBlurGateBlocked);
        return gate;
    }

    bool EnsureMode4CameraMotionZBlurCopy(void* device, uint32_t sceneSurface)
    {
        if (!device || sceneSurface < 0x10000u || !s_rawCreateTexture)
            return false;
        D3DSurfaceDescLite desc = {};
        void* scene = reinterpret_cast<void*>(static_cast<uintptr_t>(sceneSurface));
        void** svt = IsReadableMemory(scene, sizeof(void*)) ? *reinterpret_cast<void***>(scene) : nullptr;
        if (!svt || !IsReadableMemory(svt, (kD3DSurfaceGetDescVtableIndex + 1u) * sizeof(void*)) ||
            !IsExecutableMemory(svt[kD3DSurfaceGetDescVtableIndex]))
            return false;
        const D3DSurfaceGetDesc_t getDesc = reinterpret_cast<D3DSurfaceGetDesc_t>(svt[kD3DSurfaceGetDescVtableIndex]);
        if (FAILED(getDesc(scene, &desc)) || !desc.Width || !desc.Height)
            return false;
        if (s_cameraMotionZBlurCopyTexture && s_cameraMotionZBlurCopySurface &&
            s_cameraMotionZBlurWidth == desc.Width && s_cameraMotionZBlurHeight == desc.Height &&
            s_cameraMotionZBlurFormat == desc.Format)
            return true;
        ResetMode4CameraMotionZBlurResources();
        void* texture = nullptr;
        s_cameraMotionZBlurCreateHr = s_rawCreateTexture(device, desc.Width, desc.Height, 1u, 1u, desc.Format, 0u, &texture, nullptr);
        if (FAILED(s_cameraMotionZBlurCreateHr) || !texture)
            return false;
        void** tvt = *reinterpret_cast<void***>(texture);
        if (!tvt || !IsReadableMemory(tvt, (kD3DTextureGetSurfaceLevelVtableIndex + 1u) * sizeof(void*)) ||
            !IsExecutableMemory(tvt[kD3DTextureGetSurfaceLevelVtableIndex]))
        {
            s_cameraMotionZBlurCreateHr = E_FAIL;
            SafeReleaseCom(texture);
            return false;
        }
        const D3DTextureGetSurfaceLevel_t getSurface = reinterpret_cast<D3DTextureGetSurfaceLevel_t>(tvt[kD3DTextureGetSurfaceLevelVtableIndex]);
        void* surface = nullptr;
        const HRESULT surfaceHr = getSurface(texture, 0u, &surface);
        if (FAILED(surfaceHr) || !surface)
        {
            s_cameraMotionZBlurCreateHr = FAILED(surfaceHr) ? surfaceHr : E_FAIL;
            SafeReleaseCom(texture);
            return false;
        }
        s_cameraMotionZBlurCopyTexture = texture;
        s_cameraMotionZBlurCopySurface = surface;
        s_cameraMotionZBlurWidth = desc.Width;
        s_cameraMotionZBlurHeight = desc.Height;
        s_cameraMotionZBlurFormat = desc.Format;
        InterlockedExchange(&s_cameraMotionZBlurCreateState, 1);
        return true;
    }

    bool RunMode4CameraMotionZBlurOutputTargetIsolation()
    {
        // V10.5.49.10.14 OUTPUT-TARGET ISOLATION.
        // .10.9 proved Scene -> dedicated temporary RT StretchRect is visually clean.
        // .10.10 proved the dormant ZBlur VS/PS + dynamic PS-c0 bind/readback/exact restore.
        // .10.11 proved stage-0 binding of the copied ZBlur texture + exact restore.
        // .10.12/.10.12.1 runtime-proved sampler-0 capture/set/readback/exact-restore:
        // 564 attempts / 564 passes / 0 failures in the diagnostic run.
        //
        // .10.13 runtime-proved the eleven render states as visually clean.
        // Advance exactly one architectural category here: the old two-step output-target
        // sequence surrounding the custom ZBlur draws. First bind the dedicated ZBlur copy
        // surface as RT0 with depth detached/full-size viewport; then (without drawing)
        // transition RT0 to Scene with the same DS/viewport contract. Stage 0 is temporarily
        // unbound/rebound only to prevent D3D9 feedback while the copy texture is RT0. Read back both phases,
        // restore the exact previous output state, and verify restoration. Every previously-
        // cleared GPU-state stage remains.
        //
        // Target render-state contract:
        //   ZENABLE=0, ZWRITEENABLE=0, ALPHATESTENABLE=0, CULLMODE=1,
        //   ALPHABLENDENABLE=0, FOGENABLE=0, STENCILENABLE=0, LIGHTING=0,
        //   COLORWRITEENABLE=0xF, SCISSORTESTENABLE=0, SRGBWRITEENABLE=0.
        //
        // Deliberately forbidden here:
        //   SetTextureStageState
        //   DrawPrimitive / fullscreen quad / final composite.
        // SetRenderTarget / SetDepthStencilSurface / SetViewport are the ONLY new category.
        const LONG attempt = InterlockedIncrement(&s_cameraMotionZBlurOutputTargetOnlyAttemptCount);
        const bool previousGate = s_cameraMotionZBlurGateLast;
        const bool gate = EvaluateNativePcCameraZBlurGate();
        const bool gateChanged = attempt > 1 && gate != previousGate;

        const uint32_t deviceValue = s_rawDeviceValue;
        const uint32_t sceneSurf = s_rawSurfaceScene;
        const uint32_t vsValue = ReadU32(kCameraMotionZBlurVS);
        const uint32_t psValue = ReadU32(kCameraMotionZBlurPS);
        const uint32_t activeVSBefore = ReadU32(kActiveVSCache);
        const uint32_t activePSBefore = ReadU32(kActivePSCache);
        const uint32_t stage0CacheBefore = ReadU32(kNglTextureCacheBase);

        D3DSurfaceDescLite sceneDesc = {};
        HRESULT sceneDescHr = E_PENDING;
        if (sceneSurf >= 0x10000u)
        {
            void* scene = reinterpret_cast<void*>(static_cast<uintptr_t>(sceneSurf));
            void** svt = IsReadableMemory(scene, sizeof(void*)) ? *reinterpret_cast<void***>(scene) : nullptr;
            if (svt && IsReadableMemory(svt, (kD3DSurfaceGetDescVtableIndex + 1u) * sizeof(void*)) &&
                IsExecutableMemory(svt[kD3DSurfaceGetDescVtableIndex]))
            {
                const D3DSurfaceGetDesc_t getDesc =
                    reinterpret_cast<D3DSurfaceGetDesc_t>(svt[kD3DSurfaceGetDescVtableIndex]);
                sceneDescHr = getDesc(scene, &sceneDesc);
            }
            else
            {
                sceneDescHr = E_FAIL;
            }
        }

        void* device = nullptr;
        void* beforeRT = nullptr;
        void* beforeDS = nullptr;
        void* afterRT = nullptr;
        void* afterDS = nullptr;
        HRESULT beforeRTHr = E_PENDING;
        HRESULT beforeDSHr = E_PENDING;
        HRESULT afterRTHr = E_PENDING;
        HRESULT afterDSHr = E_PENDING;
        D3DViewportLite beforeViewport = {};
        D3DViewportLite boundCopyViewport = {};
        D3DViewportLite boundSceneViewport = {};
        D3DViewportLite restoredOutputViewport = {};
        D3DViewportLite afterViewport = {};
        HRESULT getBeforeViewportHr = E_PENDING;
        HRESULT getBoundCopyViewportHr = E_PENDING;
        HRESULT getBoundSceneViewportHr = E_PENDING;
        HRESULT getRestoredViewportHr = E_PENDING;
        HRESULT getAfterViewportHr = E_PENDING;
        D3DGetViewport_t getViewport = nullptr;
        void* boundCopyRT = nullptr;
        void* boundCopyDS = nullptr;
        void* boundSceneRT = nullptr;
        void* boundSceneDS = nullptr;
        void* reboundOutputTexture = nullptr;
        void* restoredOutputRT = nullptr;
        void* restoredOutputDS = nullptr;
        HRESULT getBoundCopyRTHr = E_PENDING;
        HRESULT getBoundCopyDSHr = E_PENDING;
        HRESULT getBoundSceneRTHr = E_PENDING;
        HRESULT getBoundSceneDSHr = E_PENDING;
        HRESULT outputTextureUnbindHr = E_PENDING;
        HRESULT outputTextureRebindHr = E_PENDING;
        HRESULT getReboundOutputTextureHr = E_PENDING;
        HRESULT getRestoredOutputRTHr = E_PENDING;
        HRESULT getRestoredOutputDSHr = E_PENDING;
        if (deviceValue >= 0x10000u)
        {
            device = reinterpret_cast<void*>(static_cast<uintptr_t>(deviceValue));
            if (s_rawGetRenderTarget)
                beforeRTHr = s_rawGetRenderTarget(device, 0u, &beforeRT);
            if (s_rawGetDepthStencilSurface)
                beforeDSHr = s_rawGetDepthStencilSurface(device, &beforeDS);
        }

        const bool sceneReady = sceneSurf >= 0x10000u && SUCCEEDED(sceneDescHr) &&
            sceneDesc.Width != 0u && sceneDesc.Height != 0u;
        const bool shaderPairReady = vsValue >= 0x10000u && psValue >= 0x10000u &&
            IsReadableMemory(reinterpret_cast<const void*>(static_cast<uintptr_t>(vsValue)), sizeof(void*)) &&
            IsReadableMemory(reinterpret_cast<const void*>(static_cast<uintptr_t>(psValue)), sizeof(void*));
        const bool copyApiReady = s_rawCreateTexture && s_rawStretchRect;

        bool copyAttempted = false;
        bool copyOk = false;
        HRESULT stretchHr = E_PENDING;
        float dynamicScale = kCameraMotionZBlurFixedMinScale;

        if (!gate)
        {
            ResetMode4CameraMotionZBlurController();
        }
        else
        {
            const bool copyEligible = s_mode4CameraMotionZBlurOutputTargetOnlyEnabled &&
                deviceValue >= 0x10000u && sceneReady && copyApiReady;
            bool copyResourceReady = false;
            if (copyEligible)
                copyResourceReady = EnsureMode4CameraMotionZBlurCopy(device, sceneSurf);

            dynamicScale = UpdateMode4CameraMotionZBlurScale();

            if (copyEligible && copyResourceReady && s_cameraMotionZBlurCopySurface)
            {
                copyAttempted = true;
                stretchHr = s_rawStretchRect(
                    device,
                    reinterpret_cast<void*>(static_cast<uintptr_t>(sceneSurf)), nullptr,
                    s_cameraMotionZBlurCopySurface, nullptr,
                    kD3DFilterNone);
                copyOk = SUCCEEDED(stretchHr);
            }
            else if (copyEligible)
            {
                stretchHr = FAILED(s_cameraMotionZBlurCreateHr) ? s_cameraMotionZBlurCreateHr : E_FAIL;
            }
        }

        void* beforeVS = nullptr;
        void* beforePS = nullptr;
        void* boundVS = nullptr;
        void* boundPS = nullptr;
        void* afterVS = nullptr;
        void* afterPS = nullptr;
        void* beforeTexture0 = nullptr;
        void* boundTexture0 = nullptr;
        void* afterTexture0 = nullptr;
        constexpr uint32_t kSamplerStateCount = 7u;
        const DWORD samplerStateTypes[kSamplerStateCount] =
        {
            kSampAddressU, kSampAddressV, kSampMagFilter, kSampMinFilter,
            kSampMipFilter, kSampMaxAnisotropy, kSampSrgbTexture
        };
        const DWORD samplerTargetValues[kSamplerStateCount] =
        {
            kTextureAddressClamp, kTextureAddressClamp,
            kTextureFilterLinear, kTextureFilterLinear, kTextureFilterLinear,
            1u, 0u
        };
        DWORD beforeSampler[kSamplerStateCount] = {};
        DWORD boundSampler[kSamplerStateCount] = {};
        DWORD afterSampler[kSamplerStateCount] = {};
        uint32_t samplerCaptureMask = 0u;
        uint32_t samplerSetMask = 0u;
        uint32_t samplerBoundMask = 0u;
        uint32_t samplerRestoreSetMask = 0u;
        uint32_t samplerAfterMask = 0u;
        constexpr uint32_t kSamplerAllMask = (1u << kSamplerStateCount) - 1u;

        constexpr uint32_t kRenderStateCount = 11u;
        const DWORD renderStateTypes[kRenderStateCount] =
        {
            kRSZEnable, kRSZWriteEnable, kRSAlphaTestEnable, kRSCullMode,
            kRSAlphaBlendEnable, kRSFogEnable, kRSStencilEnable, kRSLighting,
            kRSColorWriteEnable, kRSScissorTestEnable, kRSSrgbWriteEnable
        };
        const DWORD renderTargetValues[kRenderStateCount] =
        {
            0u, 0u, 0u, 1u,
            0u, 0u, 0u, 0u,
            0xFu, 0u, 0u
        };
        DWORD beforeRender[kRenderStateCount] = {};
        DWORD boundRender[kRenderStateCount] = {};
        DWORD afterRender[kRenderStateCount] = {};
        uint32_t renderCaptureMask = 0u;
        uint32_t renderSetMask = 0u;
        uint32_t renderBoundMask = 0u;
        uint32_t renderRestoreSetMask = 0u;
        uint32_t renderAfterMask = 0u;
        constexpr uint32_t kRenderAllMask = (1u << kRenderStateCount) - 1u;

        float beforeC0[4] = {};
        float boundC0[4] = {};
        float afterC0[4] = {};
        const float testC0[4] = { 0.5f, 0.5f, dynamicScale, dynamicScale };
        memcpy(s_cameraMotionZBlurLastC0, testC0, sizeof(testC0));

        HRESULT getBeforeVsHr = E_PENDING;
        HRESULT getBeforePsHr = E_PENDING;
        HRESULT getBeforeC0Hr = E_PENDING;
        HRESULT getBeforeTextureHr = E_PENDING;
        HRESULT getBoundVsHr = E_PENDING;
        HRESULT getBoundPsHr = E_PENDING;
        HRESULT getBoundC0Hr = E_PENDING;
        HRESULT getBoundTextureHr = E_PENDING;
        HRESULT getAfterVsHr = E_PENDING;
        HRESULT getAfterPsHr = E_PENDING;
        HRESULT getAfterC0Hr = E_PENDING;
        HRESULT getAfterTextureHr = E_PENDING;
        s_cameraMotionZBlurBindSetVsHr = E_PENDING;
        s_cameraMotionZBlurBindSetPsHr = E_PENDING;
        s_cameraMotionZBlurBindSetC0Hr = E_PENDING;
        s_cameraMotionZBlurBindRestoreVsHr = E_PENDING;
        s_cameraMotionZBlurBindRestorePsHr = E_PENDING;
        s_cameraMotionZBlurBindRestoreC0Hr = E_PENDING;
        s_cameraMotionZBlurTextureSetHr = E_PENDING;
        s_cameraMotionZBlurTextureRestoreHr = E_PENDING;
        s_cameraMotionZBlurSamplerSetHr = E_PENDING;
        s_cameraMotionZBlurSamplerRestoreHr = E_PENDING;
        s_cameraMotionZBlurRenderSetHr = E_PENDING;
        s_cameraMotionZBlurRenderRestoreHr = E_PENDING;
        s_cameraMotionZBlurOutputSetCopyRtHr = E_PENDING;
        s_cameraMotionZBlurOutputSetCopyDsHr = E_PENDING;
        s_cameraMotionZBlurOutputSetCopyViewportHr = E_PENDING;
        s_cameraMotionZBlurOutputSetRtHr = E_PENDING;
        s_cameraMotionZBlurOutputSetDsHr = E_PENDING;
        s_cameraMotionZBlurOutputSetViewportHr = E_PENDING;
        s_cameraMotionZBlurOutputRestoreRtHr = E_PENDING;
        s_cameraMotionZBlurOutputRestoreDsHr = E_PENDING;
        s_cameraMotionZBlurOutputRestoreViewportHr = E_PENDING;

        bool shaderBindAttempted = false;
        bool shaderBindVerified = false;
        bool c0BindVerified = false;
        bool shaderRestoreStable = false;
        bool c0RestoreStable = false;
        bool textureBindAttempted = false;
        bool textureBindVerified = false;
        bool textureRestoreStable = false;
        bool samplerStateAttempted = false;
        bool samplerBindVerified = false;
        bool samplerRestoreStable = false;
        bool renderStateAttempted = false;
        bool renderBindVerified = false;
        bool renderRestoreStable = false;
        bool outputTargetAttempted = false;
        bool copyOutputBindVerified = false;
        bool sceneOutputBindVerified = false;
        bool outputRtBindVerified = false;
        bool outputDsBindVerified = false;
        bool outputViewportBindVerified = false;
        bool outputRestoreStable = false;

        if (copyOk && device && shaderPairReady && s_cameraMotionZBlurCopyTexture &&
            s_rawSetVertexShader && s_rawSetPixelShader && s_rawSetPixelShaderConstantF &&
            s_rawSetTexture && s_rawSetSamplerState && s_rawSetRenderState && s_rawGetRenderState)
        {
            void** dvt = IsReadableMemory(device, sizeof(void*)) ? *reinterpret_cast<void***>(device) : nullptr;
            const bool getterTableReady = dvt &&
                IsReadableMemory(dvt, (kD3DGetPixelShaderConstantFVtableIndex + 1u) * sizeof(void*)) &&
                IsExecutableMemory(dvt[kD3DGetTextureVtableIndex]) &&
                IsExecutableMemory(dvt[kD3DGetViewportVtableIndex]) &&
                IsExecutableMemory(dvt[kD3DGetRenderStateVtableIndex]) &&
                IsExecutableMemory(dvt[kD3DGetSamplerStateVtableIndex]) &&
                IsExecutableMemory(dvt[kD3DGetVertexShaderVtableIndex]) &&
                IsExecutableMemory(dvt[kD3DGetPixelShaderVtableIndex]) &&
                IsExecutableMemory(dvt[kD3DGetPixelShaderConstantFVtableIndex]);

            if (getterTableReady)
            {
                const D3DGetTexture_t getTexture =
                    reinterpret_cast<D3DGetTexture_t>(dvt[kD3DGetTextureVtableIndex]);
                getViewport = reinterpret_cast<D3DGetViewport_t>(dvt[kD3DGetViewportVtableIndex]);
                const D3DGetRenderState_t getRenderState =
                    reinterpret_cast<D3DGetRenderState_t>(dvt[kD3DGetRenderStateVtableIndex]);
                const D3DGetSamplerState_t getSamplerState =
                    reinterpret_cast<D3DGetSamplerState_t>(dvt[kD3DGetSamplerStateVtableIndex]);
                const D3DGetVertexShader_t getVS =
                    reinterpret_cast<D3DGetVertexShader_t>(dvt[kD3DGetVertexShaderVtableIndex]);
                const D3DGetPixelShader_t getPS =
                    reinterpret_cast<D3DGetPixelShader_t>(dvt[kD3DGetPixelShaderVtableIndex]);
                const D3DGetPixelShaderConstantF_t getPSC0 =
                    reinterpret_cast<D3DGetPixelShaderConstantF_t>(dvt[kD3DGetPixelShaderConstantFVtableIndex]);

                getBeforeVsHr = getVS(device, &beforeVS);
                getBeforePsHr = getPS(device, &beforePS);
                getBeforeC0Hr = getPSC0(device, 0u, beforeC0, 1u);
                getBeforeTextureHr = getTexture(device, 0u, &beforeTexture0);
                getBeforeViewportHr = getViewport ? getViewport(device, &beforeViewport) : E_FAIL;
                for (uint32_t n = 0; n < kSamplerStateCount; ++n)
                {
                    const HRESULT hr = getSamplerState(device, 0u, samplerStateTypes[n], &beforeSampler[n]);
                    if (SUCCEEDED(hr))
                        samplerCaptureMask |= (1u << n);
                }
                for (uint32_t n = 0; n < kRenderStateCount; ++n)
                {
                    const HRESULT hr = getRenderState(device, renderStateTypes[n], &beforeRender[n]);
                    if (SUCCEEDED(hr))
                        renderCaptureMask |= (1u << n);
                }

                // Fail closed: exact restoration state for every object touched by this test
                // must be captured before the first mutation.
                if (SUCCEEDED(getBeforeVsHr) && SUCCEEDED(getBeforePsHr) &&
                    SUCCEEDED(getBeforeC0Hr) && SUCCEEDED(getBeforeTextureHr) &&
                    SUCCEEDED(beforeRTHr) && beforeRT &&
                    (SUCCEEDED(beforeDSHr) || beforeDS == nullptr) && SUCCEEDED(getBeforeViewportHr) &&
                    samplerCaptureMask == kSamplerAllMask &&
                    renderCaptureMask == kRenderAllMask)
                {
                    shaderBindAttempted = true;
                    s_cameraMotionZBlurBindSetVsHr = s_rawSetVertexShader(
                        device, reinterpret_cast<void*>(static_cast<uintptr_t>(vsValue)));
                    s_cameraMotionZBlurBindSetPsHr = s_rawSetPixelShader(
                        device, reinterpret_cast<void*>(static_cast<uintptr_t>(psValue)));
                    s_cameraMotionZBlurBindSetC0Hr = s_rawSetPixelShaderConstantF(device, 0u, testC0, 1u);

                    if (SUCCEEDED(s_cameraMotionZBlurBindSetVsHr) &&
                        SUCCEEDED(s_cameraMotionZBlurBindSetPsHr) &&
                        SUCCEEDED(s_cameraMotionZBlurBindSetC0Hr))
                    {
                        getBoundVsHr = getVS(device, &boundVS);
                        getBoundPsHr = getPS(device, &boundPS);
                        getBoundC0Hr = getPSC0(device, 0u, boundC0, 1u);
                        shaderBindVerified = SUCCEEDED(getBoundVsHr) && SUCCEEDED(getBoundPsHr) &&
                            static_cast<uint32_t>(reinterpret_cast<uintptr_t>(boundVS)) == vsValue &&
                            static_cast<uint32_t>(reinterpret_cast<uintptr_t>(boundPS)) == psValue;
                        c0BindVerified = SUCCEEDED(getBoundC0Hr) &&
                            memcmp(boundC0, testC0, sizeof(testC0)) == 0;

                        // Retain the .10.11-cleared stage-0 copied-texture mutation.
                        if (shaderBindVerified && c0BindVerified)
                        {
                            textureBindAttempted = true;
                            s_cameraMotionZBlurTextureSetHr =
                                s_rawSetTexture(device, 0u, s_cameraMotionZBlurCopyTexture);
                            if (SUCCEEDED(s_cameraMotionZBlurTextureSetHr))
                            {
                                getBoundTextureHr = getTexture(device, 0u, &boundTexture0);
                                textureBindVerified = SUCCEEDED(getBoundTextureHr) &&
                                    boundTexture0 == s_cameraMotionZBlurCopyTexture;
                            }

                            // The only NEW GPU-state category in .10.12.
                            if (textureBindVerified)
                            {
                                samplerStateAttempted = true;
                                s_cameraMotionZBlurSamplerSetHr = S_OK;
                                for (uint32_t n = 0; n < kSamplerStateCount; ++n)
                                {
                                    const HRESULT hr = s_rawSetSamplerState(
                                        device, 0u, samplerStateTypes[n], samplerTargetValues[n]);
                                    if (SUCCEEDED(hr))
                                        samplerSetMask |= (1u << n);
                                    else if (SUCCEEDED(s_cameraMotionZBlurSamplerSetHr))
                                        s_cameraMotionZBlurSamplerSetHr = hr;
                                }

                                if (samplerSetMask == kSamplerAllMask)
                                {
                                    for (uint32_t n = 0; n < kSamplerStateCount; ++n)
                                    {
                                        const HRESULT hr = getSamplerState(
                                            device, 0u, samplerStateTypes[n], &boundSampler[n]);
                                        if (SUCCEEDED(hr) && boundSampler[n] == samplerTargetValues[n])
                                            samplerBoundMask |= (1u << n);
                                    }
                                }
                                samplerBindVerified =
                                    samplerSetMask == kSamplerAllMask &&
                                    samplerBoundMask == kSamplerAllMask;
                            }

                            // The only NEW GPU-state category in .10.13.
                            if (samplerBindVerified && renderCaptureMask == kRenderAllMask)
                            {
                                renderStateAttempted = true;
                                s_cameraMotionZBlurRenderSetHr = S_OK;
                                for (uint32_t n = 0; n < kRenderStateCount; ++n)
                                {
                                    const HRESULT hr = s_rawSetRenderState(
                                        device, renderStateTypes[n], renderTargetValues[n]);
                                    if (SUCCEEDED(hr))
                                        renderSetMask |= (1u << n);
                                    else if (SUCCEEDED(s_cameraMotionZBlurRenderSetHr))
                                        s_cameraMotionZBlurRenderSetHr = hr;
                                }

                                if (renderSetMask == kRenderAllMask)
                                {
                                    for (uint32_t n = 0; n < kRenderStateCount; ++n)
                                    {
                                        const HRESULT hr = getRenderState(
                                            device, renderStateTypes[n], &boundRender[n]);
                                        if (SUCCEEDED(hr) && boundRender[n] == renderTargetValues[n])
                                            renderBoundMask |= (1u << n);
                                    }
                                }
                                renderBindVerified =
                                    renderSetMask == kRenderAllMask &&
                                    renderBoundMask == kRenderAllMask;
                            }

                            // The only NEW architectural category in .10.14: replay the old
                            // output-target sequence with ZERO draws. Phase A binds the dedicated
                            // ZBlur copy surface as RT0; Phase B transitions RT0 to Scene. Both
                            // phases detach depth and use the full Scene viewport.
                            if (renderBindVerified && getViewport && getTexture && s_rawSetTexture &&
                                s_rawSetRenderTarget && s_rawGetRenderTarget &&
                                s_rawSetDepthStencilSurface && s_rawGetDepthStencilSurface &&
                                s_rawSetViewport && s_cameraMotionZBlurCopySurface)
                            {
                                outputTargetAttempted = true;
                                D3DViewportLite targetViewport = {};
                                targetViewport.Width = s_cameraMotionZBlurWidth;
                                targetViewport.Height = s_cameraMotionZBlurHeight;
                                targetViewport.MinZ = 0.0f;
                                targetViewport.MaxZ = 1.0f;

                                // Avoid the D3D9 read/write feedback hazard: the already-cleared
                                // copied texture was bound at stage 0 by the predecessor test, so
                                // unbind it before its surface becomes RT0. No draw occurs here.
                                outputTextureUnbindHr = s_rawSetTexture(device, 0u, nullptr);

                                // Phase A: old copy-pass output state, but no DrawPrimitive.
                                s_cameraMotionZBlurOutputSetCopyRtHr = SUCCEEDED(outputTextureUnbindHr)
                                    ? s_rawSetRenderTarget(device, 0u, s_cameraMotionZBlurCopySurface) : E_FAIL;
                                s_cameraMotionZBlurOutputSetCopyDsHr =
                                    SUCCEEDED(s_cameraMotionZBlurOutputSetCopyRtHr)
                                        ? s_rawSetDepthStencilSurface(device, nullptr) : E_FAIL;
                                s_cameraMotionZBlurOutputSetCopyViewportHr =
                                    SUCCEEDED(s_cameraMotionZBlurOutputSetCopyDsHr)
                                        ? s_rawSetViewport(device, &targetViewport) : E_FAIL;

                                if (SUCCEEDED(s_cameraMotionZBlurOutputSetCopyRtHr) &&
                                    SUCCEEDED(s_cameraMotionZBlurOutputSetCopyDsHr) &&
                                    SUCCEEDED(s_cameraMotionZBlurOutputSetCopyViewportHr))
                                {
                                    getBoundCopyRTHr = s_rawGetRenderTarget(device, 0u, &boundCopyRT);
                                    getBoundCopyDSHr = s_rawGetDepthStencilSurface(device, &boundCopyDS);
                                    getBoundCopyViewportHr = getViewport(device, &boundCopyViewport);
                                    const bool copyRtOk = SUCCEEDED(getBoundCopyRTHr) &&
                                        boundCopyRT == s_cameraMotionZBlurCopySurface;
                                    const bool copyDsOk = boundCopyDS == nullptr;
                                    const bool copyVpOk = SUCCEEDED(getBoundCopyViewportHr) &&
                                        boundCopyViewport.X == targetViewport.X &&
                                        boundCopyViewport.Y == targetViewport.Y &&
                                        boundCopyViewport.Width == targetViewport.Width &&
                                        boundCopyViewport.Height == targetViewport.Height &&
                                        boundCopyViewport.MinZ == targetViewport.MinZ &&
                                        boundCopyViewport.MaxZ == targetViewport.MaxZ;
                                    copyOutputBindVerified = copyRtOk && copyDsOk && copyVpOk;
                                }

                                // Phase B: old final-pass output transition, still with no draw.
                                if (copyOutputBindVerified)
                                {
                                    s_cameraMotionZBlurOutputSetRtHr = s_rawSetRenderTarget(
                                        device, 0u, reinterpret_cast<void*>(static_cast<uintptr_t>(sceneSurf)));
                                    s_cameraMotionZBlurOutputSetDsHr = SUCCEEDED(s_cameraMotionZBlurOutputSetRtHr)
                                        ? s_rawSetDepthStencilSurface(device, nullptr) : E_FAIL;
                                    s_cameraMotionZBlurOutputSetViewportHr =
                                        SUCCEEDED(s_cameraMotionZBlurOutputSetDsHr)
                                            ? s_rawSetViewport(device, &targetViewport) : E_FAIL;

                                    if (SUCCEEDED(s_cameraMotionZBlurOutputSetRtHr) &&
                                        SUCCEEDED(s_cameraMotionZBlurOutputSetDsHr) &&
                                        SUCCEEDED(s_cameraMotionZBlurOutputSetViewportHr))
                                    {
                                        getBoundSceneRTHr = s_rawGetRenderTarget(device, 0u, &boundSceneRT);
                                        getBoundSceneDSHr = s_rawGetDepthStencilSurface(device, &boundSceneDS);
                                        getBoundSceneViewportHr = getViewport(device, &boundSceneViewport);
                                        outputRtBindVerified = SUCCEEDED(getBoundSceneRTHr) &&
                                            static_cast<uint32_t>(reinterpret_cast<uintptr_t>(boundSceneRT)) == sceneSurf;
                                        outputDsBindVerified = boundSceneDS == nullptr;
                                        outputViewportBindVerified = SUCCEEDED(getBoundSceneViewportHr) &&
                                            boundSceneViewport.X == targetViewport.X &&
                                            boundSceneViewport.Y == targetViewport.Y &&
                                            boundSceneViewport.Width == targetViewport.Width &&
                                            boundSceneViewport.Height == targetViewport.Height &&
                                            boundSceneViewport.MinZ == targetViewport.MinZ &&
                                            boundSceneViewport.MaxZ == targetViewport.MaxZ;
                                        sceneOutputBindVerified = outputRtBindVerified &&
                                            outputDsBindVerified && outputViewportBindVerified;
                                    }

                                    // Re-establish the already-cleared final-pass texture state
                                    // before restoring the remaining predecessor state.
                                    outputTextureRebindHr = s_rawSetTexture(
                                        device, 0u, s_cameraMotionZBlurCopyTexture);
                                    if (SUCCEEDED(outputTextureRebindHr))
                                    {
                                        getReboundOutputTextureHr = getTexture(
                                            device, 0u, &reboundOutputTexture);
                                        sceneOutputBindVerified = sceneOutputBindVerified &&
                                            SUCCEEDED(getReboundOutputTextureHr) &&
                                            reboundOutputTexture == s_cameraMotionZBlurCopyTexture;
                                    }
                                    else
                                    {
                                        sceneOutputBindVerified = false;
                                    }
                                }
                            }
                        }
                    }

                    // Restore the NEW output-target block first, then render states, sampler 0,
                    // its texture, and finally c0/PS/VS.
                    if (outputTargetAttempted)
                    {
                        s_cameraMotionZBlurOutputRestoreRtHr = s_rawSetRenderTarget(device, 0u, beforeRT);
                        s_cameraMotionZBlurOutputRestoreDsHr = s_rawSetDepthStencilSurface(device, beforeDS);
                        s_cameraMotionZBlurOutputRestoreViewportHr = s_rawSetViewport(device, &beforeViewport);

                        getRestoredOutputRTHr = s_rawGetRenderTarget(device, 0u, &restoredOutputRT);
                        getRestoredOutputDSHr = s_rawGetDepthStencilSurface(device, &restoredOutputDS);
                        getRestoredViewportHr = getViewport(device, &restoredOutputViewport);

                        const bool rtRestored = SUCCEEDED(getRestoredOutputRTHr) &&
                            restoredOutputRT == beforeRT;
                        const bool dsRestored = (restoredOutputDS == beforeDS) &&
                            (SUCCEEDED(beforeDSHr) == SUCCEEDED(getRestoredOutputDSHr));
                        const bool viewportRestored = SUCCEEDED(getRestoredViewportHr) &&
                            memcmp(&restoredOutputViewport, &beforeViewport, sizeof(beforeViewport)) == 0;
                        outputRestoreStable = SUCCEEDED(s_cameraMotionZBlurOutputRestoreRtHr) &&
                            SUCCEEDED(s_cameraMotionZBlurOutputRestoreDsHr) &&
                            SUCCEEDED(s_cameraMotionZBlurOutputRestoreViewportHr) &&
                            rtRestored && dsRestored && viewportRestored;
                    }

                    // Restore the render-state block first, then sampler 0, its texture,
                    // and finally c0/PS/VS.
                    if (renderStateAttempted)
                    {
                        s_cameraMotionZBlurRenderRestoreHr = S_OK;
                        for (uint32_t n = 0; n < kRenderStateCount; ++n)
                        {
                            const HRESULT hr = s_rawSetRenderState(
                                device, renderStateTypes[n], beforeRender[n]);
                            if (SUCCEEDED(hr))
                                renderRestoreSetMask |= (1u << n);
                            else if (SUCCEEDED(s_cameraMotionZBlurRenderRestoreHr))
                                s_cameraMotionZBlurRenderRestoreHr = hr;
                        }

                        for (uint32_t n = 0; n < kRenderStateCount; ++n)
                        {
                            const HRESULT hr = getRenderState(
                                device, renderStateTypes[n], &afterRender[n]);
                            if (SUCCEEDED(hr) && afterRender[n] == beforeRender[n])
                                renderAfterMask |= (1u << n);
                        }
                        renderRestoreStable =
                            renderRestoreSetMask == kRenderAllMask &&
                            renderAfterMask == kRenderAllMask;
                    }
                    else
                    {
                        s_cameraMotionZBlurRenderRestoreHr = S_OK;
                    }

                    // Restore sampler 0 before restoring its texture, then restore the
                    // exact stage-0 texture and finally c0/PS/VS.
                    if (samplerStateAttempted)
                    {
                        s_cameraMotionZBlurSamplerRestoreHr = S_OK;
                        for (uint32_t n = 0; n < kSamplerStateCount; ++n)
                        {
                            const HRESULT hr = s_rawSetSamplerState(
                                device, 0u, samplerStateTypes[n], beforeSampler[n]);
                            if (SUCCEEDED(hr))
                                samplerRestoreSetMask |= (1u << n);
                            else if (SUCCEEDED(s_cameraMotionZBlurSamplerRestoreHr))
                                s_cameraMotionZBlurSamplerRestoreHr = hr;
                        }

                        for (uint32_t n = 0; n < kSamplerStateCount; ++n)
                        {
                            const HRESULT hr = getSamplerState(
                                device, 0u, samplerStateTypes[n], &afterSampler[n]);
                            if (SUCCEEDED(hr) && afterSampler[n] == beforeSampler[n])
                                samplerAfterMask |= (1u << n);
                        }
                        samplerRestoreStable =
                            samplerRestoreSetMask == kSamplerAllMask &&
                            samplerAfterMask == kSamplerAllMask;
                    }
                    else
                    {
                        s_cameraMotionZBlurSamplerRestoreHr = S_OK;
                    }

                    if (textureBindAttempted)
                        s_cameraMotionZBlurTextureRestoreHr = s_rawSetTexture(device, 0u, beforeTexture0);
                    else
                        s_cameraMotionZBlurTextureRestoreHr = S_OK;

                    getAfterTextureHr = getTexture(device, 0u, &afterTexture0);
                    textureRestoreStable = SUCCEEDED(getAfterTextureHr) && afterTexture0 == beforeTexture0;

                    s_cameraMotionZBlurBindRestoreC0Hr =
                        s_rawSetPixelShaderConstantF(device, 0u, beforeC0, 1u);
                    s_cameraMotionZBlurBindRestorePsHr = s_rawSetPixelShader(device, beforePS);
                    s_cameraMotionZBlurBindRestoreVsHr = s_rawSetVertexShader(device, beforeVS);

                    getAfterVsHr = getVS(device, &afterVS);
                    getAfterPsHr = getPS(device, &afterPS);
                    getAfterC0Hr = getPSC0(device, 0u, afterC0, 1u);
                    shaderRestoreStable = SUCCEEDED(getAfterVsHr) && SUCCEEDED(getAfterPsHr) &&
                        afterVS == beforeVS && afterPS == beforePS;
                    c0RestoreStable = SUCCEEDED(getAfterC0Hr) &&
                        memcmp(afterC0, beforeC0, sizeof(beforeC0)) == 0;
                }
            }
        }

        if (device)
        {
            if (s_rawGetRenderTarget)
                afterRTHr = s_rawGetRenderTarget(device, 0u, &afterRT);
            if (s_rawGetDepthStencilSurface)
                afterDSHr = s_rawGetDepthStencilSurface(device, &afterDS);
            if (getViewport)
                getAfterViewportHr = getViewport(device, &afterViewport);
        }

        const uint32_t activeVSAfter = ReadU32(kActiveVSCache);
        const uint32_t activePSAfter = ReadU32(kActivePSCache);
        const uint32_t stage0CacheAfter = ReadU32(kNglTextureCacheBase);
        const uint32_t beforeRTValue = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(beforeRT));
        const uint32_t afterRTValue = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(afterRT));
        const uint32_t beforeDSValue = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(beforeDS));
        const uint32_t afterDSValue = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(afterDS));
        const bool rtStable = (SUCCEEDED(beforeRTHr) == SUCCEEDED(afterRTHr)) && beforeRTValue == afterRTValue;
        const bool dsStable = (beforeDSHr == afterDSHr) && beforeDSValue == afterDSValue;
        const bool viewportStable = SUCCEEDED(getBeforeViewportHr) && SUCCEEDED(getAfterViewportHr) &&
            memcmp(&beforeViewport, &afterViewport, sizeof(beforeViewport)) == 0;
        const bool shaderCacheStable = activeVSBefore == activeVSAfter && activePSBefore == activePSAfter;
        const bool textureCacheStable = stage0CacheBefore == stage0CacheAfter;
        const bool outputEligible = copyAttempted && copyOk && shaderPairReady &&
            s_cameraMotionZBlurCopyTexture && s_rawSetTexture && s_rawSetSamplerState &&
            s_rawSetRenderState && s_rawGetRenderState && s_rawSetRenderTarget &&
            s_rawGetRenderTarget && s_rawSetDepthStencilSurface && s_rawGetDepthStencilSurface &&
            s_rawSetViewport && getViewport;
        const bool shaderRestoreCallsOk = SUCCEEDED(s_cameraMotionZBlurBindRestoreVsHr) &&
            SUCCEEDED(s_cameraMotionZBlurBindRestorePsHr) && SUCCEEDED(s_cameraMotionZBlurBindRestoreC0Hr);
        const bool textureRestoreCallOk = textureBindAttempted && SUCCEEDED(s_cameraMotionZBlurTextureRestoreHr);
        const bool samplerRestoreCallOk = samplerStateAttempted &&
            SUCCEEDED(s_cameraMotionZBlurSamplerRestoreHr);
        const bool renderRestoreCallOk = renderStateAttempted &&
            SUCCEEDED(s_cameraMotionZBlurRenderRestoreHr);
        const bool priorChainVerified = shaderBindAttempted && shaderBindVerified && c0BindVerified &&
            textureBindAttempted && textureBindVerified && samplerStateAttempted && samplerBindVerified &&
            renderStateAttempted && renderBindVerified;
        const bool isolationOk = priorChainVerified && outputTargetAttempted &&
            copyOutputBindVerified && sceneOutputBindVerified &&
            outputRtBindVerified && outputDsBindVerified && outputViewportBindVerified &&
            outputRestoreStable && renderRestoreCallOk && renderRestoreStable &&
            samplerRestoreCallOk && samplerRestoreStable && textureRestoreCallOk &&
            textureRestoreStable && shaderRestoreCallsOk && shaderRestoreStable &&
            c0RestoreStable && rtStable && dsStable && viewportStable &&
            shaderCacheStable && textureCacheStable;

        if (outputEligible)
        {
            if (isolationOk) InterlockedIncrement(&s_cameraMotionZBlurOutputTargetOnlySuccessCount);
            else InterlockedIncrement(&s_cameraMotionZBlurOutputTargetOnlyFailureCount);
        }

        const bool logNow = attempt <= 16 || (attempt % 120) == 0 || gateChanged ||
            (outputEligible && !isolationOk) || !rtStable || !dsStable || !viewportStable ||
            !shaderCacheStable || !textureCacheStable;
        if (logNow)
        {
            Event* event = ReserveEvent(EventType::CameraMotionZBlurOutputTargetOnly);
            if (event)
            {
                event->a = s_presentCount;
                event->b = static_cast<uint32_t>(attempt);
                event->c = gate ? 1u : 0u;
                event->d = copyOk ? 1u : 0u;
                event->e = outputEligible ? 1u : 0u;
                event->f = outputTargetAttempted ? 1u : 0u;
                event->g = isolationOk ? 1u : 0u;
                event->h = static_cast<uint32_t>(stretchHr);
                event->i = static_cast<uint32_t>(s_cameraMotionZBlurOutputSetCopyRtHr);
                event->j = static_cast<uint32_t>(s_cameraMotionZBlurOutputSetCopyDsHr);
                event->extra[0] = static_cast<uint32_t>(s_cameraMotionZBlurOutputSetCopyViewportHr);
                event->extra[1] = static_cast<uint32_t>(s_cameraMotionZBlurOutputSetRtHr);
                event->extra[2] = static_cast<uint32_t>(s_cameraMotionZBlurOutputSetDsHr);
                event->extra[3] = static_cast<uint32_t>(s_cameraMotionZBlurOutputSetViewportHr);
                event->extra[4] = static_cast<uint32_t>(s_cameraMotionZBlurOutputRestoreRtHr);
                event->extra[5] = static_cast<uint32_t>(s_cameraMotionZBlurOutputRestoreDsHr);
                event->extra[6] = static_cast<uint32_t>(s_cameraMotionZBlurOutputRestoreViewportHr);
                event->extra[7] = beforeRTValue;
                event->extra[8] = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(s_cameraMotionZBlurCopySurface));
                event->extra[9] = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(boundCopyRT));
                event->extra[10] = sceneSurf;
                event->extra[11] = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(boundSceneRT));
                event->extra[12] = afterRTValue;
                event->extra[13] = beforeDSValue;
                event->extra[14] = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(boundCopyDS));
                event->extra[15] = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(boundSceneDS));
                event->extra[16] = afterDSValue;
                event->extra[17] = beforeViewport.Width;
                event->extra[18] = beforeViewport.Height;
                event->extra[19] = s_cameraMotionZBlurWidth;
                event->extra[20] = s_cameraMotionZBlurHeight;
                event->extra[21] = copyOutputBindVerified ? 1u : 0u;
                event->extra[22] = sceneOutputBindVerified ? 1u : 0u;
                event->extra[23] = outputRestoreStable ? 1u : 0u;
                event->extra[24] = priorChainVerified ? 1u : 0u;
                event->extra[25] = rtStable ? 1u : 0u;
                event->extra[26] = dsStable ? 1u : 0u;
                event->extra[27] = viewportStable ? 1u : 0u;
            }
        }

        SafeReleaseCom(restoredOutputDS);
        SafeReleaseCom(restoredOutputRT);
        SafeReleaseCom(reboundOutputTexture);
        SafeReleaseCom(boundSceneDS);
        SafeReleaseCom(boundSceneRT);
        SafeReleaseCom(boundCopyDS);
        SafeReleaseCom(boundCopyRT);
        SafeReleaseCom(afterTexture0);
        SafeReleaseCom(boundTexture0);
        SafeReleaseCom(beforeTexture0);
        SafeReleaseCom(afterPS);
        SafeReleaseCom(afterVS);
        SafeReleaseCom(boundPS);
        SafeReleaseCom(boundVS);
        SafeReleaseCom(beforePS);
        SafeReleaseCom(beforeVS);
        SafeReleaseCom(afterDS);
        SafeReleaseCom(afterRT);
        SafeReleaseCom(beforeDS);
        SafeReleaseCom(beforeRT);

        // Gate-off frames are not failures. A true result means every already-cleared stage
        // plus the NEW RT0/depth-stencil/viewport bind/readback/exact-restore transition
        // completed cleanly, with no fullscreen draw.
        return isolationOk;
    }

    bool RunMode4CameraMotionZBlurDynamic()
    {
        const LONG attempt = InterlockedIncrement(&s_cameraMotionZBlurAttemptCount);
        const bool gate = EvaluateNativePcCameraZBlurGate();
        if (!gate)
        {
            ResetMode4CameraMotionZBlurController();
            if (attempt <= 16 || (attempt % 120) == 0)
            {
                Event* event = ReserveEvent(EventType::CameraMotionZBlur);
                if (event)
                {
                    event->a = s_presentCount;
                    event->b = static_cast<uint32_t>(attempt);
                    event->c = 0u;
                    event->d = ReadU32(kCameraMotionZBlurVS);
                    event->e = ReadU32(kCameraMotionZBlurPS);
                    event->f = FloatBits(0.5f);
                    event->g = FloatBits(0.5f);
                    event->h = FloatBits(kCameraMotionZBlurFixedMinScale);
                    event->i = FloatBits(kCameraMotionZBlurFixedMinScale);
                    event->j = static_cast<uint32_t>(E_PENDING);
                    event->extra[11] = static_cast<uint32_t>(s_cameraMotionZBlurSuccessCount);
                    event->extra[12] = static_cast<uint32_t>(s_cameraMotionZBlurFailureCount);
                    event->extra[24] = 0u;
                    event->extra[25] = s_cameraMotionZBlurGateReadable ? 1u : 0u;
                    event->extra[26] = static_cast<uint32_t>(s_cameraMotionZBlurGateCalls);
                    event->extra[27] = static_cast<uint32_t>(s_cameraMotionZBlurGateBlocked);
                }
            }
            return false;
        }

        s_cameraMotionZBlurCaptureHr = E_PENDING;
        s_cameraMotionZBlurCopySetupHr = E_PENDING;
        s_cameraMotionZBlurCopyDrawHr = E_PENDING;
        s_cameraMotionZBlurSetupHr = E_PENDING;
        s_cameraMotionZBlurDrawHr = E_PENDING;
        s_cameraMotionZBlurRestoreHr = E_PENDING;
        const float dynamicZBlurScale = UpdateMode4CameraMotionZBlurScale();
        const uint32_t deviceValue = s_rawDeviceValue;
        const uint32_t sceneTex = s_rawTextureScene;
        const uint32_t sceneSurf = s_rawSurfaceScene;
        const uint32_t vsValue = ReadU32(kCameraMotionZBlurVS);
        const uint32_t psValue = ReadU32(kCameraMotionZBlurPS);
        bool ok = false;
        if (deviceValue >= 0x10000u && sceneTex >= 0x10000u && sceneSurf >= 0x10000u &&
            vsValue >= 0x10000u && psValue >= 0x10000u && s_rawSetRenderTarget &&
            s_rawGetRenderTarget && s_rawSetDepthStencilSurface && s_rawGetDepthStencilSurface &&
            s_rawCreateStateBlock && s_rawSetViewport && s_rawSetRenderState &&
            s_rawSetSamplerState && s_rawSetTexture && s_rawSetVertexShader &&
            s_rawSetPixelShader && s_rawSetPixelShaderConstantF && s_rawDrawPrimitive &&
            s_originalFullscreenQuad &&
            EnsureMode4CameraMotionZBlurCopy(reinterpret_cast<void*>(static_cast<uintptr_t>(deviceValue)), sceneSurf))
        {
            void* device = reinterpret_cast<void*>(static_cast<uintptr_t>(deviceValue));
            void* previousRT = nullptr;
            void* previousDS = nullptr;
            if (s_rawGetRenderTarget) s_rawGetRenderTarget(device, 0u, &previousRT);
            if (s_rawGetDepthStencilSurface) s_rawGetDepthStencilSurface(device, &previousDS);
            void* stateBlock = nullptr;
            s_cameraMotionZBlurCaptureHr = s_rawCreateStateBlock ? s_rawCreateStateBlock(device, 1u, &stateBlock) : E_FAIL;
            if (SUCCEEDED(s_cameraMotionZBlurCaptureHr) && stateBlock)
            {
                void** sbvt = *reinterpret_cast<void***>(stateBlock);
                const D3DStateBlockCapture_t capture = (sbvt && IsExecutableMemory(sbvt[kD3DStateBlockCaptureVtableIndex])) ?
                    reinterpret_cast<D3DStateBlockCapture_t>(sbvt[kD3DStateBlockCaptureVtableIndex]) : nullptr;
                if (capture) s_cameraMotionZBlurCaptureHr = capture(stateBlock);
            }
            HRESULT firstFailure = S_OK;
            s_cameraMotionZBlurCopySetupHr = s_rawSetRenderTarget(device, 0u, s_cameraMotionZBlurCopySurface);
            if (SUCCEEDED(s_cameraMotionZBlurCopySetupHr))
            {
                if (s_rawSetDepthStencilSurface) s_rawSetDepthStencilSurface(device, nullptr);
                if (s_rawSetViewport)
                {
                    D3DViewportLite viewport = {};
                    viewport.Width = s_cameraMotionZBlurWidth;
                    viewport.Height = s_cameraMotionZBlurHeight;
                    viewport.MinZ = 0.0f;
                    viewport.MaxZ = 1.0f;
                    const HRESULT hr = s_rawSetViewport(device, &viewport);
                    if (SUCCEEDED(firstFailure) && FAILED(hr)) firstFailure = hr;
                }
                s_rawSetTexture(device, 0u, reinterpret_cast<void*>(static_cast<uintptr_t>(sceneTex)));
                s_rawSetVertexShader(device, reinterpret_cast<void*>(static_cast<uintptr_t>(vsValue)));
                s_rawSetPixelShader(device, reinterpret_cast<void*>(static_cast<uintptr_t>(psValue)));
                const float copyC0[4] = { 0.5f, 0.5f, 1.0f, 1.0f };
                s_rawSetPixelShaderConstantF(device, 0u, copyC0, 1u);
                SetMode4OffscreenNoBlendState(device, firstFailure);
                SetMode4LinearClampSampler0(device, firstFailure);
                s_cameraMotionZBlurCopyDrawHr = SUCCEEDED(firstFailure) ? DrawMode4NativeFullscreen(device) : firstFailure;
            }

            // D3D9 must not have Scene bound as a texture when Scene is made RT0.
            s_rawSetTexture(device, 0u, nullptr);
            s_cameraMotionZBlurSetupHr = s_rawSetRenderTarget(device, 0u, reinterpret_cast<void*>(static_cast<uintptr_t>(sceneSurf)));
            if (SUCCEEDED(s_cameraMotionZBlurSetupHr))
            {
                if (s_rawSetDepthStencilSurface) s_rawSetDepthStencilSurface(device, nullptr);
                if (s_rawSetViewport)
                {
                    D3DViewportLite viewport = {};
                    viewport.Width = s_cameraMotionZBlurWidth;
                    viewport.Height = s_cameraMotionZBlurHeight;
                    viewport.MinZ = 0.0f;
                    viewport.MaxZ = 1.0f;
                    const HRESULT hr = s_rawSetViewport(device, &viewport);
                    if (SUCCEEDED(s_cameraMotionZBlurSetupHr) && FAILED(hr)) s_cameraMotionZBlurSetupHr = hr;
                }
                s_rawSetTexture(device, 0u, s_cameraMotionZBlurCopyTexture);
                s_rawSetVertexShader(device, reinterpret_cast<void*>(static_cast<uintptr_t>(vsValue)));
                s_rawSetPixelShader(device, reinterpret_cast<void*>(static_cast<uintptr_t>(psValue)));
                const float c0[4] = { 0.5f, 0.5f, dynamicZBlurScale, dynamicZBlurScale };
                memcpy(s_cameraMotionZBlurLastC0, c0, sizeof(c0));
                s_rawSetPixelShaderConstantF(device, 0u, c0, 1u);
                firstFailure = S_OK;
                SetMode4OffscreenNoBlendState(device, firstFailure);
                SetMode4LinearClampSampler0(device, firstFailure);
                s_cameraMotionZBlurDrawHr = SUCCEEDED(firstFailure) ? DrawMode4NativeFullscreen(device) : firstFailure;
            }
            s_rawSetTexture(device, 0u, nullptr);
            HRESULT applyHr = S_OK;
            if (stateBlock)
            {
                void** sbvt = *reinterpret_cast<void***>(stateBlock);
                const D3DStateBlockApply_t apply = (sbvt && IsExecutableMemory(sbvt[kD3DStateBlockApplyVtableIndex])) ?
                    reinterpret_cast<D3DStateBlockApply_t>(sbvt[kD3DStateBlockApplyVtableIndex]) : nullptr;
                if (apply) applyHr = apply(stateBlock);
            }
            HRESULT restoreRTHr = previousRT && s_rawSetRenderTarget ? s_rawSetRenderTarget(device, 0u, previousRT) : S_OK;
            HRESULT restoreDSHr = s_rawSetDepthStencilSurface ? s_rawSetDepthStencilSurface(device, previousDS) : S_OK;
            SafeReleaseCom(stateBlock);
            SafeReleaseCom(previousDS);
            SafeReleaseCom(previousRT);
            s_cameraMotionZBlurRestoreHr = FAILED(applyHr) ? applyHr : (FAILED(restoreRTHr) ? restoreRTHr : restoreDSHr);
            ok = SUCCEEDED(s_cameraMotionZBlurCreateHr) && SUCCEEDED(s_cameraMotionZBlurCaptureHr) &&
                SUCCEEDED(s_cameraMotionZBlurCopySetupHr) && SUCCEEDED(s_cameraMotionZBlurCopyDrawHr) &&
                SUCCEEDED(s_cameraMotionZBlurSetupHr) && SUCCEEDED(s_cameraMotionZBlurDrawHr) &&
                SUCCEEDED(s_cameraMotionZBlurRestoreHr);
        }
        if (ok) InterlockedIncrement(&s_cameraMotionZBlurSuccessCount);
        else InterlockedIncrement(&s_cameraMotionZBlurFailureCount);

        if (attempt <= 16 || (attempt % 120) == 0 || !ok)
        {
            Event* event = ReserveEvent(EventType::CameraMotionZBlur);
            if (event)
            {
                event->a = s_presentCount;
                event->b = static_cast<uint32_t>(attempt);
                event->c = ok ? 1u : 0u;
                event->d = ReadU32(kCameraMotionZBlurVS);
                event->e = ReadU32(kCameraMotionZBlurPS);
                event->f = FloatBits(s_cameraMotionZBlurLastC0[0]);
                event->g = FloatBits(s_cameraMotionZBlurLastC0[1]);
                event->h = FloatBits(s_cameraMotionZBlurLastC0[2]);
                event->i = FloatBits(s_cameraMotionZBlurLastC0[3]);
                event->j = static_cast<uint32_t>(s_cameraMotionZBlurCreateHr);
                event->extra[0] = static_cast<uint32_t>(s_cameraMotionZBlurCaptureHr);
                event->extra[1] = static_cast<uint32_t>(s_cameraMotionZBlurCopySetupHr);
                event->extra[2] = static_cast<uint32_t>(s_cameraMotionZBlurCopyDrawHr);
                event->extra[3] = static_cast<uint32_t>(s_cameraMotionZBlurSetupHr);
                event->extra[4] = static_cast<uint32_t>(s_cameraMotionZBlurDrawHr);
                event->extra[5] = static_cast<uint32_t>(s_cameraMotionZBlurRestoreHr);
                event->extra[6] = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(s_cameraMotionZBlurCopyTexture));
                event->extra[7] = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(s_cameraMotionZBlurCopySurface));
                event->extra[8] = s_cameraMotionZBlurWidth;
                event->extra[9] = s_cameraMotionZBlurHeight;
                event->extra[10] = s_cameraMotionZBlurFormat;
                event->extra[11] = static_cast<uint32_t>(s_cameraMotionZBlurSuccessCount);
                event->extra[12] = static_cast<uint32_t>(s_cameraMotionZBlurFailureCount);
                event->extra[13] = sceneTex;
                event->extra[14] = sceneSurf;
                event->extra[15] = s_provenIntzDofState;
                event->extra[16] = s_cameraMotionZBlurControllerValid ? 1u : 0u;
                event->extra[17] = s_cameraMotionZBlurCameraValue;
                event->extra[18] = s_cameraMotionZBlurTransformValue;
                event->extra[19] = FloatBits(s_cameraMotionZBlurLastDelta);
                event->extra[20] = FloatBits(s_cameraMotionZBlurLastAverage);
                event->extra[21] = FloatBits(s_cameraMotionZBlurLastMotion);
                event->extra[22] = FloatBits(s_cameraMotionZBlurLastBlurPixels);
                event->extra[23] = s_cameraMotionZBlurHistoryIndex;
                event->extra[24] = 1u;
                event->extra[25] = s_cameraMotionZBlurGateReadable ? 1u : 0u;
                event->extra[26] = static_cast<uint32_t>(s_cameraMotionZBlurGateCalls);
                event->extra[27] = static_cast<uint32_t>(s_cameraMotionZBlurGateBlocked);
            }
        }
        return ok;
    }

    void ProbeMode4CameraMotionZBlurNativeContract()
    {
        // V10.5.49.10.8 PASSIVE ONLY. This routine deliberately performs no
        // render-target changes, texture/shader binds, state changes, allocations,
        // copies, or draws. It samples the exact inputs that .10.7 consumed so we
        // can identify the broken contract without disturbing the clean .10.6 image.
        const LONG probe = InterlockedIncrement(&s_cameraMotionZBlurProbeCount);
        const bool previousGate = s_cameraMotionZBlurGateLast;
        const bool gate = EvaluateNativePcCameraZBlurGate();
        const bool gateChanged = probe > 1 && gate != previousGate;

        float dynamicScale = kCameraMotionZBlurFixedMinScale;
        if (gate)
            dynamicScale = UpdateMode4CameraMotionZBlurScale();
        else
            ResetMode4CameraMotionZBlurController();

        const uint32_t deviceValue = s_rawDeviceValue;
        const uint32_t sceneTex = s_rawTextureScene;
        const uint32_t sceneSurf = s_rawSurfaceScene;
        const uint32_t vsValue = ReadU32(kCameraMotionZBlurVS);
        const uint32_t psValue = ReadU32(kCameraMotionZBlurPS);
        const uint32_t activeVS = ReadU32(kActiveVSCache);
        const uint32_t activePS = ReadU32(kActivePSCache);

        D3DSurfaceDescLite sceneDesc = {};
        HRESULT sceneDescHr = E_PENDING;
        if (sceneSurf >= 0x10000u)
        {
            void* scene = reinterpret_cast<void*>(static_cast<uintptr_t>(sceneSurf));
            void** svt = IsReadableMemory(scene, sizeof(void*)) ? *reinterpret_cast<void***>(scene) : nullptr;
            if (svt && IsReadableMemory(svt, (kD3DSurfaceGetDescVtableIndex + 1u) * sizeof(void*)) &&
                IsExecutableMemory(svt[kD3DSurfaceGetDescVtableIndex]))
            {
                const D3DSurfaceGetDesc_t getDesc = reinterpret_cast<D3DSurfaceGetDesc_t>(svt[kD3DSurfaceGetDescVtableIndex]);
                sceneDescHr = getDesc(scene, &sceneDesc);
            }
            else
            {
                sceneDescHr = E_FAIL;
            }
        }

        void* currentRT = nullptr;
        void* currentDS = nullptr;
        HRESULT getRTHr = E_PENDING;
        HRESULT getDSHr = E_PENDING;
        if (deviceValue >= 0x10000u)
        {
            void* device = reinterpret_cast<void*>(static_cast<uintptr_t>(deviceValue));
            if (s_rawGetRenderTarget)
                getRTHr = s_rawGetRenderTarget(device, 0u, &currentRT);
            if (s_rawGetDepthStencilSurface)
                getDSHr = s_rawGetDepthStencilSurface(device, &currentDS);
        }

        const bool shaderPairReady = vsValue >= 0x10000u && psValue >= 0x10000u &&
            IsReadableMemory(reinterpret_cast<void*>(static_cast<uintptr_t>(vsValue)), sizeof(void*)) &&
            IsReadableMemory(reinterpret_cast<void*>(static_cast<uintptr_t>(psValue)), sizeof(void*));
        const bool sceneReady = sceneTex >= 0x10000u && sceneSurf >= 0x10000u &&
            IsReadableMemory(reinterpret_cast<void*>(static_cast<uintptr_t>(sceneTex)), sizeof(void*)) &&
            SUCCEEDED(sceneDescHr) && sceneDesc.Width != 0u && sceneDesc.Height != 0u;
        const bool apiReady = s_rawCreateTexture && s_rawSetRenderTarget && s_rawGetRenderTarget &&
            s_rawSetDepthStencilSurface && s_rawGetDepthStencilSurface && s_rawCreateStateBlock &&
            s_rawSetViewport && s_rawSetRenderState && s_rawSetSamplerState && s_rawSetTexture &&
            s_rawSetVertexShader && s_rawSetPixelShader && s_rawSetPixelShaderConstantF &&
            s_rawDrawPrimitive && s_originalFullscreenQuad;
        const bool wouldRun = gate && deviceValue >= 0x10000u && shaderPairReady && sceneReady && apiReady;

        if (probe <= 16 || (probe % 120) == 0 || gateChanged)
        {
            Event* event = ReserveEvent(EventType::CameraMotionZBlurProbe);
            if (event)
            {
                event->a = s_presentCount;
                event->b = static_cast<uint32_t>(probe);
                event->c = gate ? 1u : 0u;
                event->d = wouldRun ? 1u : 0u;
                event->e = vsValue;
                event->f = psValue;
                event->g = activeVS;
                event->h = activePS;
                event->i = deviceValue;
                event->j = sceneTex;
                event->extra[0] = sceneSurf;
                event->extra[1] = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(currentRT));
                event->extra[2] = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(currentDS));
                event->extra[3] = sceneDesc.Width;
                event->extra[4] = sceneDesc.Height;
                event->extra[5] = sceneDesc.Format;
                event->extra[6] = static_cast<uint32_t>(sceneDescHr);
                event->extra[7] = static_cast<uint32_t>(getRTHr);
                event->extra[8] = static_cast<uint32_t>(getDSHr);
                event->extra[9] = s_cameraMotionZBlurControllerValid ? 1u : 0u;
                event->extra[10] = s_cameraMotionZBlurCameraValue;
                event->extra[11] = s_cameraMotionZBlurTransformValue;
                event->extra[12] = FloatBits(s_cameraMotionZBlurLastDelta);
                event->extra[13] = FloatBits(s_cameraMotionZBlurLastAverage);
                event->extra[14] = FloatBits(s_cameraMotionZBlurLastMotion);
                event->extra[15] = FloatBits(s_cameraMotionZBlurLastBlurPixels);
                event->extra[16] = FloatBits(dynamicScale);
                event->extra[17] = shaderPairReady ? 1u : 0u;
                event->extra[18] = sceneReady ? 1u : 0u;
                event->extra[19] = apiReady ? 1u : 0u;
                event->extra[20] = s_provenIntzDofState;
                event->extra[21] = s_cameraMotionZBlurGateReadable ? 1u : 0u;
                event->extra[22] = static_cast<uint32_t>(s_cameraMotionZBlurGateCalls);
                event->extra[23] = static_cast<uint32_t>(s_cameraMotionZBlurGateBlocked);
                event->extra[24] = s_mode4CameraMotionZBlurEnabled ? 1u : 0u;
                event->extra[25] = 0u; // GPU mutation/draw count is intentionally zero in .10.8.
                event->extra[26] = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(s_originalFullscreenQuad));
                event->extra[27] = FloatBits(kCameraMotionZBlurFixedMinScale);
            }
        }

        SafeReleaseCom(currentDS);
        SafeReleaseCom(currentRT);
    }

    bool ApplyV9ExposurePass(void* device, void* backbufferSurface)
    {
        if (!device || !backbufferSurface || !s_v9ExposureEnabled ||
            s_v9WarmupCount < kV9WarmupSamples ||
            !s_rawStretchRect || !s_rawSetRenderTarget || !s_rawGetRenderTarget ||
            !s_rawSetDepthStencilSurface || !s_rawGetDepthStencilSurface ||
            !s_rawBeginScene || !s_rawEndScene || !s_rawSetTexture ||
            !s_rawSetPixelShader || !s_rawSetVertexShader || !s_rawSetFVF ||
            !s_rawSetViewport || !s_rawSetRenderState || !s_rawSetTextureStageState ||
            !s_rawSetSamplerState || !s_rawDrawPrimitiveUP ||
            !EnsureV9SceneResources(device, backbufferSurface))
        {
            return false;
        }

        void** stateVtable = *reinterpret_cast<void***>(s_v9StateBlock);
        if (!stateVtable || !IsReadableMemory(stateVtable,
            (kD3DStateBlockApplyVtableIndex + 1u) * sizeof(void*)) ||
            !IsExecutableMemory(stateVtable[kD3DStateBlockCaptureVtableIndex]) ||
            !IsExecutableMemory(stateVtable[kD3DStateBlockApplyVtableIndex]))
        {
            ResetV9SceneResources();
            return false;
        }

        const auto capture =
            reinterpret_cast<D3DStateBlockCapture_t>(stateVtable[kD3DStateBlockCaptureVtableIndex]);
        const auto apply =
            reinterpret_cast<D3DStateBlockApply_t>(stateVtable[kD3DStateBlockApplyVtableIndex]);

        const HRESULT captureHr = capture(s_v9StateBlock);
        if (FAILED(captureHr))
        {
            s_v9LastApplyHr = captureHr;
            return false;
        }

        void* previousRT = nullptr;
        void* previousDepth = nullptr;
        const HRESULT getRTHr = s_rawGetRenderTarget(device, 0u, &previousRT);
        const HRESULT getDepthHr = s_rawGetDepthStencilSurface(device, &previousDepth);

        // D3DERR_NOTFOUND simply means there was no depth-stencil surface to
        // preserve. Any other GetDepthStencilSurface failure is treated as unsafe.
        const bool depthStateKnown = SUCCEEDED(getDepthHr) || getDepthHr == kD3DErrNotFound;
        if (FAILED(getRTHr) || !previousRT || !depthStateKnown)
        {
            SafeReleaseCom(previousDepth);
            SafeReleaseCom(previousRT);
            s_v9LastApplyHr = apply(s_v9StateBlock);
            return false;
        }

        // Copy the completed backbuffer before opening our own scene. This is
        // the source texture for the fixed-function exposure multiply.
        s_v9LastStretchHr = s_rawStretchRect(device, backbufferSurface, nullptr,
            s_v9SceneCopySurface, nullptr, kD3DFilterNone);

        HRESULT setupHr = S_OK;
        HRESULT beginHr = E_PENDING;
        HRESULT endHr = E_PENDING;
        HRESULT restoreRTHr = S_OK;
        HRESULT restoreDepthHr = S_OK;
        HRESULT stateApplyHr = S_OK;
        bool beganScene = false;

        auto KeepFirstFailure = [&setupHr](HRESULT hr)
        {
            if (SUCCEEDED(setupHr) && FAILED(hr))
                setupHr = hr;
        };

        if (SUCCEEDED(s_v9LastStretchHr))
        {
            // A backbuffer-sized color pass does not need the game's depth buffer.
            KeepFirstFailure(s_rawSetDepthStencilSurface(device, nullptr));
            KeepFirstFailure(s_rawSetRenderTarget(device, 0u, backbufferSurface));

            if (SUCCEEDED(setupHr))
            {
                beginHr = s_rawBeginScene(device);
                beganScene = SUCCEEDED(beginHr);
                KeepFirstFailure(beginHr);
            }

            if (beganScene && SUCCEEDED(setupHr))
            {
                D3DViewportLite viewport = {};
                viewport.Width = s_v9SceneWidth;
                viewport.Height = s_v9SceneHeight;
                viewport.MinZ = 0.0f;
                viewport.MaxZ = 1.0f;
                KeepFirstFailure(s_rawSetViewport(device, &viewport));

                KeepFirstFailure(s_rawSetPixelShader(device, nullptr));
                KeepFirstFailure(s_rawSetVertexShader(device, nullptr));
                KeepFirstFailure(s_rawSetFVF(device, kD3DFVFXYZRHW | kD3DFVFTex1));
                KeepFirstFailure(s_rawSetTexture(device, 0u, s_v9SceneCopyTexture));
                KeepFirstFailure(s_rawSetTexture(device, 1u, nullptr));

                KeepFirstFailure(s_rawSetRenderState(device, kRSZEnable, 0u));
                KeepFirstFailure(s_rawSetRenderState(device, kRSZWriteEnable, 0u));
                KeepFirstFailure(s_rawSetRenderState(device, kRSAlphaTestEnable, 0u));
                KeepFirstFailure(s_rawSetRenderState(device, kRSCullMode, 1u));
                KeepFirstFailure(s_rawSetRenderState(device, kRSAlphaBlendEnable, 0u));
                KeepFirstFailure(s_rawSetRenderState(device, kRSFogEnable, 0u));
                KeepFirstFailure(s_rawSetRenderState(device, kRSStencilEnable, 0u));
                KeepFirstFailure(s_rawSetRenderState(device, kRSLighting, 0u));
                KeepFirstFailure(s_rawSetRenderState(device, kRSColorWriteEnable, 0xFu));
                KeepFirstFailure(s_rawSetRenderState(device, kRSScissorTestEnable, 0u));
                KeepFirstFailure(s_rawSetRenderState(device, kRSSrgbWriteEnable, 0u));

                uint32_t colorOp = kTopModulate;
                const uint32_t textureFactor = ExposureTextureFactor(s_v9AdaptedExposure, colorOp);
                KeepFirstFailure(s_rawSetRenderState(device, kRSTextureFactor, textureFactor));

                KeepFirstFailure(s_rawSetTextureStageState(device, 0u, kTSSColorOp, colorOp));
                KeepFirstFailure(s_rawSetTextureStageState(device, 0u, kTSSColorArg1, kTaTexture));
                KeepFirstFailure(s_rawSetTextureStageState(device, 0u, kTSSColorArg2, kTaTFactor));
                KeepFirstFailure(s_rawSetTextureStageState(device, 0u, kTSSAlphaOp, kTopSelectArg1));
                KeepFirstFailure(s_rawSetTextureStageState(device, 0u, kTSSAlphaArg1, kTaTexture));
                KeepFirstFailure(s_rawSetTextureStageState(device, 0u, kTSSTexCoordIndex, 0u));
                KeepFirstFailure(s_rawSetTextureStageState(device, 0u, kTSSTextureTransformFlags, 0u));
                KeepFirstFailure(s_rawSetTextureStageState(device, 1u, kTSSColorOp, kTopDisable));

                KeepFirstFailure(s_rawSetSamplerState(device, 0u, kSampAddressU, kTextureAddressClamp));
                KeepFirstFailure(s_rawSetSamplerState(device, 0u, kSampAddressV, kTextureAddressClamp));
                KeepFirstFailure(s_rawSetSamplerState(device, 0u, kSampMagFilter, kTextureFilterPoint));
                KeepFirstFailure(s_rawSetSamplerState(device, 0u, kSampMinFilter, kTextureFilterPoint));
                KeepFirstFailure(s_rawSetSamplerState(device, 0u, kSampMipFilter, kTextureFilterNone));
                KeepFirstFailure(s_rawSetSamplerState(device, 0u, kSampSrgbTexture, 0u));

                const float right = static_cast<float>(s_v9SceneWidth) - 0.5f;
                const float bottom = static_cast<float>(s_v9SceneHeight) - 0.5f;
                const ExposureVertex vertices[4] =
                {
                    { -0.5f, -0.5f, 0.0f, 1.0f, 0.0f, 0.0f },
                    { right, -0.5f, 0.0f, 1.0f, 1.0f, 0.0f },
                    { -0.5f, bottom, 0.0f, 1.0f, 0.0f, 1.0f },
                    { right, bottom, 0.0f, 1.0f, 1.0f, 1.0f }
                };

                if (SUCCEEDED(setupHr))
                {
                    s_v9LastDrawHr = s_rawDrawPrimitiveUP(device,
                        kD3DPrimitiveTriangleStrip, 2u, vertices,
                        static_cast<UINT>(sizeof(ExposureVertex)));
                }
                else
                {
                    s_v9LastDrawHr = setupHr;
                }
            }
            else
            {
                s_v9LastDrawHr = setupHr;
            }
        }
        else
        {
            setupHr = s_v9LastStretchHr;
            s_v9LastDrawHr = s_v9LastStretchHr;
        }

        if (beganScene)
            endHr = s_rawEndScene(device);

        // Render targets/depth are not guaranteed to be captured by a D3D9
        // state block. Restore them explicitly first, then apply the captured
        // state so viewport, textures, shaders, transforms and render states
        // return to the exact pre-V9 values.
        restoreRTHr = s_rawSetRenderTarget(device, 0u, previousRT);
        restoreDepthHr = s_rawSetDepthStencilSurface(device,
            SUCCEEDED(getDepthHr) ? previousDepth : nullptr);
        stateApplyHr = apply(s_v9StateBlock);

        SafeReleaseCom(previousDepth);
        SafeReleaseCom(previousRT);

        s_v9LastApplyHr = S_OK;
        const HRESULT restoreResults[3] = { restoreRTHr, restoreDepthHr, stateApplyHr };
        for (uint32_t n = 0; n < 3u; ++n)
        {
            if (SUCCEEDED(s_v9LastApplyHr) && FAILED(restoreResults[n]))
                s_v9LastApplyHr = restoreResults[n];
        }

        const bool ok = SUCCEEDED(s_v9LastStretchHr) && SUCCEEDED(setupHr) &&
            SUCCEEDED(s_v9LastDrawHr) && SUCCEEDED(endHr) && SUCCEEDED(s_v9LastApplyHr);

        ++s_v9ApplyCount;
        if (s_v9ApplyCount <= 16u || (s_v9ApplyCount % 120u) == 0u || !ok)
        {
            uint32_t colorOp = kTopModulate;
            const uint32_t textureFactor = ExposureTextureFactor(s_v9AdaptedExposure, colorOp);
            Event* event = ReserveEvent(EventType::ExposureApply);
            if (event)
            {
                event->a = s_presentCount;
                event->b = FloatBits(s_v9AdaptedExposure);
                event->c = static_cast<uint32_t>(s_v9LastStretchHr);
                event->d = static_cast<uint32_t>(setupHr);
                event->e = static_cast<uint32_t>(s_v9LastDrawHr);
                event->f = static_cast<uint32_t>(s_v9LastApplyHr);
                event->g = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(backbufferSurface));
                event->h = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(s_v9SceneCopyTexture));
                event->i = s_v9SceneWidth;
                event->j = s_v9SceneHeight;
                event->extra[0] = s_v9SceneFormat;
                event->extra[1] = textureFactor;
                event->extra[2] = colorOp;
                event->extra[3] = s_v9ApplyCount;
                event->extra[4] = static_cast<uint32_t>(getRTHr);
                event->extra[5] = static_cast<uint32_t>(getDepthHr);
                event->extra[6] = static_cast<uint32_t>(beginHr);
                event->extra[7] = static_cast<uint32_t>(endHr);
            }
        }

        if (!ok && (FAILED(s_v9LastStretchHr) || FAILED(stateApplyHr)))
            ResetV9SceneResources();
        return ok;
    }

    void QueueV9Summary()
    {
        Event* event = ReserveEvent(EventType::ExposureSummary);
        if (!event)
            return;
        event->a = s_presentCount;
        event->b = s_v9ExposureEnabled ? 1u : 0u;
        event->c = s_v9SampleCount;
        event->d = s_v9WarmupCount;
        event->e = FloatBits(s_v9CurrentLuminance);
        event->f = FloatBits(s_v9ReferenceLuminance);
        event->g = FloatBits(s_v9TargetExposure);
        event->h = FloatBits(s_v9AdaptedExposure);
        event->i = static_cast<uint32_t>(s_v9LastReadbackHr);
        event->j = static_cast<uint32_t>(s_v9LastDrawHr);
        event->extra[0] = static_cast<uint32_t>(s_v9LastApplyHr);
        event->extra[1] = s_v9ReadbackFormat;
        event->extra[2] = s_v9ReadbackWidth;
        event->extra[3] = s_v9ReadbackHeight;
        event->extra[4] = s_v9SceneFormat;
        event->extra[5] = s_v9SceneWidth;
        event->extra[6] = s_v9SceneHeight;
        event->extra[7] = s_v9ApplyCount;
    }

    void QueueV10Summary()
    {
        Event* event = ReserveEvent(EventType::BloomControlSummary);
        if (!event)
            return;
        event->a = s_presentCount;
        event->b = static_cast<uint32_t>(s_v10Mode);
        event->c = s_v10BloomSampleCount;
        event->d = s_v10BloomScaleApplyCount;
        event->e = s_v10BloomScaleFailCount;
        event->f = s_v10FinalSubstituteCount;
        event->g = FloatBits(s_v10WeightedRms);
        event->h = FloatBits(s_v10Brightness255);
        event->i = FloatBits(s_v10BloomT);
        event->j = FloatBits(s_v10BloomStrength);
        event->extra[0] = s_v10BloomSampleValid ? 1u : 0u;
        event->extra[1] = static_cast<uint32_t>(s_v10LastScaleSetupHr);
        event->extra[2] = static_cast<uint32_t>(s_v10LastScaleDrawHr);
        event->extra[3] = static_cast<uint32_t>(s_v10LastScaleRestoreHr);
        event->extra[4] = s_rawTextureQuarterA;
        event->extra[5] = s_rawTextureQuarterB;
        event->extra[6] = s_rawSurfaceQuarterA;
        event->extra[7] = s_rawSurfaceQuarterB;
    }

    void QueueV10_4NativeExposureSummary()
    {
        Event* event = ReserveEvent(EventType::NativeExposureSummary);
        if (!event)
            return;

        const uint32_t sampleAge = s_presentCount >= s_v10_3LastLuminanceSamplePresent ?
            s_presentCount - s_v10_3LastLuminanceSamplePresent : 0u;
        event->a = s_presentCount;
        event->b = static_cast<uint32_t>(s_v10Mode);
        event->c = static_cast<uint32_t>(s_v10_4NativeExposureMatchedCount);
        event->d = static_cast<uint32_t>(s_v10_4NativeExposureAppliedCount);
        event->e = static_cast<uint32_t>(s_v10_4NativeExposureBypassCount);
        event->f = s_v10_3XboxExposureUpdateCount;
        event->g = s_v10_4LastExposureApplied ? 1u : 0u;
        event->h = ReadU8(kAutoExposureEnabled);
        event->i = s_v10BloomSampleValid ? 1u : 0u;
        event->j = s_v9ExposureEnabled ? 1u : 0u; // visual V9 pass, not telemetry
        event->extra[0] = FloatBits(s_v10_4LastOriginalExposure);
        event->extra[1] = FloatBits(s_v10_4LastInjectedExposure);
        event->extra[2] = FloatBits(s_v10_4LastXboxExposureState);
        event->extra[3] = s_v10_4LastExposureReturnAddress;
        event->extra[4] = s_v10_4LastExposureParameter;
        event->extra[5] = FloatBits(s_v9TargetExposure);
        event->extra[6] = FloatBits(s_v9AdaptedExposure);
        event->extra[7] = sampleAge;
    }

    void QueueMode4DofSummary()
    {
        Event* event = ReserveEvent(EventType::DofFinalSummary);
        if (!event)
            return;
        event->a = s_presentCount;
        event->b = static_cast<uint32_t>(s_v10Mode);
        event->c = static_cast<uint32_t>(s_mode4AttemptCount);
        event->d = static_cast<uint32_t>(s_mode4ApplyCount);
        event->e = static_cast<uint32_t>(s_mode4FallbackCount);
        event->f = s_mode4LastReadyMask;
        event->g = s_mode4LastApplied ? 1u : 0u;
        event->h = s_mode4LastColorDescriptor;
        event->i = s_mode4LastColorTexture;
        event->j = s_mode4LastDepthTexture;
        event->extra[0] = s_mode4LastDofShader;
        event->extra[1] = s_mode4LastSimpleShader;
        event->extra[2] = s_mode4LastActivePSBefore;
        event->extra[3] = s_mode4LastActivePSAfter;
        event->extra[4] = static_cast<uint32_t>(s_mode4LastConstantHr);
        event->extra[5] = static_cast<uint32_t>(s_mode4LastPixelShaderHr);
        event->extra[6] = FloatBits(s_mode4LastBloomDepthControl[0]);
        event->extra[7] = FloatBits(s_mode4LastBloomDepthControl[1]);
        event->extra[8] = FloatBits(s_mode4LastBloomDepthControl[2]);
        event->extra[9] = FloatBits(s_mode4LastBloomDepthControl[3]);
        event->extra[10] = ReadU32(kCompositePSDofFinal);
        event->extra[11] = ReadU32(kCompositePSFinal);
        event->extra[12] = static_cast<uint32_t>(s_mode4LastDepthBindHr);
        event->extra[13] = s_mode4LastDepthCacheBefore;
        event->extra[14] = s_mode4LastDepthCacheAfter;
        event->extra[15] = s_mode4LastRawStage1After;
    }

    bool AllowPermanentMode4Event(EventType type)
    {
        if (!kMode4ReleaseQuietLogging || !kMode4PermanentReszDofEnabled || s_v10Mode != 4)
            return true;

        switch (type)
        {
        case EventType::RawHookStatus:
        case EventType::DeviceReset:
        case EventType::OffscreenIntzTargetProbe:
        case EventType::ReszDepthResolveProbe:
        case EventType::ProvenIntzDofActivation:
        case EventType::DofC0BridgeProbe:
        case EventType::FullDofChain:
        case EventType::NativeBloomTail:
        case EventType::CameraMotionZBlur:
        case EventType::CameraMotionZBlurProbe:
        case EventType::CameraMotionZBlurCopyOnly:
        case EventType::CameraMotionZBlurBindOnly:
        case EventType::CameraMotionZBlurTextureBindOnly:
        case EventType::BloomControlSample:
        case EventType::BloomScaleApply:
        // V10.5.49.10.1: sparse route proof remains enabled so the log can prove
        // weighted Scene->A, Gaussian A->B, BlurLevel1 B->A, BlurLevel2 A->B,
        // final B->Scene.
        case EventType::RouteBlur1:
        case EventType::RouteBlur2:
        case EventType::RouteFinal:
        case EventType::PermanentReszDofHeartbeat:
            return true;
        default:
            return false;
        }
    }

    Event* ReserveEvent(EventType type)
    {
        if (!AllowPermanentMode4Event(type))
            return nullptr;

        const LONG slot = InterlockedIncrement(&s_eventWrite) - 1;
        const LONG read = s_eventRead;
        if (slot - read >= static_cast<LONG>(kEventCapacity))
        {
            InterlockedIncrement(&s_droppedEvents);
            return nullptr;
        }

        Event* event = &s_events[static_cast<uint32_t>(slot) % kEventCapacity];
        ZeroMemory(event, sizeof(*event));
        event->type = type;
        return event;
    }

    void QueueRouteEvent(EventType type, uintptr_t returnAddress, uint32_t source, uint32_t target, int nativeResult)
    {
        LONG count = 0;
        switch (type)
        {
        case EventType::RouteBlur1: count = InterlockedIncrement(&s_routeBlur1Count); break;
        case EventType::RouteBlur2: count = InterlockedIncrement(&s_routeBlur2Count); break;
        case EventType::RouteFinal: count = InterlockedIncrement(&s_routeFinalCount); break;
        default: break;
        }

        if (!ShouldLogSparse(count))
            return;

        Event* event = ReserveEvent(type);
        if (!event)
            return;
        event->a = static_cast<uint32_t>(count);
        event->b = static_cast<uint32_t>(s_currentCompositeCall);
        event->c = static_cast<uint32_t>(returnAddress);
        event->d = source;
        event->e = target;
        event->f = static_cast<uint32_t>(nativeResult);
        event->g = CurrentScene();
        event->h = CurrentSceneRT();
        event->i = ReadU32(kActivePSCache);
        event->j = ReadU32(kActiveVSCache);
    }

    void QueueMismatch(uint32_t phase, uintptr_t returnAddress, uint32_t observed, uint32_t expected)
    {
        const LONG count = InterlockedIncrement(&s_routeMismatchCount);
        if (!ShouldLogSparse(count))
            return;
        Event* event = ReserveEvent(EventType::RouteMismatch);
        if (!event)
            return;
        event->a = static_cast<uint32_t>(count);
        event->b = static_cast<uint32_t>(s_currentCompositeCall);
        event->c = phase;
        event->d = static_cast<uint32_t>(returnAddress);
        event->e = observed;
        event->f = expected;
        event->g = ReadU32(kRtQuarterA);
        event->h = ReadU32(kRtQuarterB);
        event->i = CurrentSceneRT();
    }

    HRESULT WINAPI RawSetTexture_Hook(void* device, DWORD stage, void* texture)
    {
        const uintptr_t returnAddress = reinterpret_cast<uintptr_t>(_ReturnAddress());
        const uint32_t texturePtr = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(texture));
        const uint32_t lumTexture = s_rawTextureLum16;
        const uint32_t secondTexture = s_rawTextureSecond16;

        const HRESULT hr = s_rawSetTexture(device, stage, texture);
        if (SUCCEEDED(hr) && stage < 16u)
            s_rawTextureStages[stage] = texturePtr;

        LONG exactCount = 0;
        uint32_t exactWhich = 0;
        if (lumTexture && texturePtr == lumTexture)
        {
            exactCount = InterlockedIncrement(&s_rawLumTextureCount);
            exactWhich = 1;
        }
        else if (secondTexture && texturePtr == secondTexture)
        {
            exactCount = InterlockedIncrement(&s_rawSecondTextureCount);
            exactWhich = 2;
        }

        if (exactWhich && ShouldLogSparse(exactCount))
        {
            Event* event = ReserveEvent(EventType::RawTextureExact);
            if (event)
            {
                event->a = static_cast<uint32_t>(exactCount);
                event->b = exactWhich;
                event->c = static_cast<uint32_t>(returnAddress);
                event->d = stage;
                event->e = texturePtr;
                event->f = static_cast<uint32_t>(hr);
                event->g = s_rawCurrentPixelShader;
                event->h = s_rawCurrentRT0;
                event->i = CurrentScene();
                CopySmallText(event->text, sizeof(event->text), CurrentSceneName());
            }
        }

        if (ConsumeRawTraceBudget())
        {
            const uint32_t kind = ClassifyRawTexture(texturePtr);
            Event* event = ReserveEvent(EventType::RawSetTexture);
            if (event)
            {
                event->a = s_presentCount;
                event->b = static_cast<uint32_t>(returnAddress);
                event->c = stage;
                event->d = texturePtr;
                event->e = kind;
                event->f = static_cast<uint32_t>(hr);
                event->g = s_rawCurrentPixelShader;
                event->h = s_rawCurrentRT0;
                event->i = CurrentScene();
                CopySmallText(event->text, sizeof(event->text), CurrentSceneName());
            }
        }
        return hr;
    }

    HRESULT ProbeDepthTextureFormat(void* device, DWORD format)
    {
        if (!device || !s_rawCreateTexture)
            return E_POINTER;

        void* texture = nullptr;
        const HRESULT hr = s_rawCreateTexture(
            device,
            16u, 16u, 1u,
            kD3DUsageDepthStencil,
            format,
            kD3DPoolDefault,
            &texture,
            nullptr);

        if (texture)
            SafeReleaseCom(texture);
        return hr;
    }

    void ProbeMode4DepthTextureCapabilities(void* device)
    {
        if (s_v10Mode != 4 || !device || s_mode4DepthCapsProbeState != 0)
            return;
        if (InterlockedCompareExchange(&s_mode4DepthCapsProbeState, 1, 0) != 0)
            return;

        s_mode4DepthCapsD24S8 = ProbeDepthTextureFormat(device, kFormatD24S8);
        s_mode4DepthCapsINTZ = ProbeDepthTextureFormat(device, kFormatINTZ);
        s_mode4DepthCapsDF24 = ProbeDepthTextureFormat(device, kFormatDF24);
        s_mode4DepthCapsRAWZ = ProbeDepthTextureFormat(device, kFormatRAWZ);
        s_mode4DepthCapsDF16 = ProbeDepthTextureFormat(device, kFormatDF16);

        Event* event = ReserveEvent(EventType::DepthTextureCaps);
        if (event)
        {
            event->a = s_presentCount;
            event->b = static_cast<uint32_t>(s_mode4DepthCapsD24S8);
            event->c = static_cast<uint32_t>(s_mode4DepthCapsINTZ);
            event->d = static_cast<uint32_t>(s_mode4DepthCapsDF24);
            event->e = static_cast<uint32_t>(s_mode4DepthCapsRAWZ);
            event->f = static_cast<uint32_t>(s_mode4DepthCapsDF16);
            event->g = s_mode4NglDepthWrapper;
            event->h = s_mode4NglDepthWrapperFlags;
            event->i = s_mode4NglDepthWrapperWidth;
            event->j = s_mode4NglDepthWrapperHeight;
            event->extra[0] = s_mode4CapturedDepthSurface;
            event->extra[1] = s_mode4NglDepthSurfaceMatchOffset;
        }
    }

    uint32_t FindU32InReadableObject(uint32_t object, uint32_t target, uint32_t dwordCount)
    {
        if (object < 0x10000u || target < 0x10000u || dwordCount == 0u)
            return 0xFFFFFFFFu;
        const size_t bytes = static_cast<size_t>(dwordCount) * sizeof(uint32_t);
        if (!IsReadableMemory(reinterpret_cast<void*>(static_cast<uintptr_t>(object)), bytes))
            return 0xFFFFFFFFu;

        for (uint32_t n = 0; n < dwordCount; ++n)
        {
            if (ReadU32(static_cast<uintptr_t>(object) + n * 4u) == target)
                return n * 4u;
        }
        return 0xFFFFFFFFu;
    }

    void ScanMode4DepthBackendGraph(uint32_t targetSurface)
    {
        s_mode4DepthBackendRoot = 0u;
        s_mode4DepthBackendDirectSurfaceOffset = 0xFFFFFFFFu;
        s_mode4DepthBackendChildSlotOffset = 0xFFFFFFFFu;
        s_mode4DepthBackendChild = 0u;
        s_mode4DepthBackendChildSurfaceOffset = 0xFFFFFFFFu;
        ZeroMemory(s_mode4DepthBackendWords, sizeof(s_mode4DepthBackendWords));

        if (s_mode4NglDepthWrapper < 0x10000u || targetSurface < 0x10000u)
            return;
        if (!IsReadableMemory(
            reinterpret_cast<void*>(static_cast<uintptr_t>(s_mode4NglDepthWrapper)), 0x0Cu))
            return;

        // V10.5.5 observed wrapper+0x08 as the strongest backend-object candidate.
        const uint32_t root = ReadU32(static_cast<uintptr_t>(s_mode4NglDepthWrapper) + 0x08u);
        s_mode4DepthBackendRoot = root;
        if (root < 0x10000u || !IsReadableMemory(
            reinterpret_cast<void*>(static_cast<uintptr_t>(root)), 64u * sizeof(uint32_t)))
            return;

        for (uint32_t n = 0; n < 16u; ++n)
            s_mode4DepthBackendWords[n] = ReadU32(static_cast<uintptr_t>(root) + n * 4u);

        s_mode4DepthBackendDirectSurfaceOffset = FindU32InReadableObject(root, targetSurface, 64u);
        if (s_mode4DepthBackendDirectSurfaceOffset != 0xFFFFFFFFu)
            return;

        // One bounded level of pointer chasing only. This is read-only and every
        // candidate is VirtualQuery-validated before it is touched.
        for (uint32_t n = 0; n < 64u; ++n)
        {
            const uint32_t candidate = ReadU32(static_cast<uintptr_t>(root) + n * 4u);
            if (candidate < 0x10000u || candidate == root || candidate == s_mode4NglDepthWrapper)
                continue;
            if (!IsReadableMemory(
                reinterpret_cast<void*>(static_cast<uintptr_t>(candidate)), 64u * sizeof(uint32_t)))
                continue;

            const uint32_t nestedOffset = FindU32InReadableObject(candidate, targetSurface, 64u);
            if (nestedOffset != 0xFFFFFFFFu)
            {
                s_mode4DepthBackendChildSlotOffset = n * 4u;
                s_mode4DepthBackendChild = candidate;
                s_mode4DepthBackendChildSurfaceOffset = nestedOffset;
                return;
            }
        }
    }

    void ProbeDepthTextureRoundTrip(void* device, DWORD format, DepthTextureRoundTripLite& out)
    {
        out = DepthTextureRoundTripLite{};
        if (!device || !s_rawCreateTexture)
        {
            out.createHr = E_POINTER;
            return;
        }

        void* texture = nullptr;
        out.createHr = s_rawCreateTexture(
            device, 16u, 16u, 1u, kD3DUsageDepthStencil,
            format, kD3DPoolDefault, &texture, nullptr);
        if (FAILED(out.createHr) || !texture)
            return;

        out.texture = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(texture));
        void* surface = nullptr;
        void** textureVtable = IsReadableMemory(texture, sizeof(void*))
            ? *reinterpret_cast<void***>(texture) : nullptr;
        if (!textureVtable || !IsReadableMemory(textureVtable,
            (kD3DTextureGetSurfaceLevelVtableIndex + 1u) * sizeof(void*)) ||
            !IsExecutableMemory(textureVtable[kD3DTextureGetSurfaceLevelVtableIndex]))
        {
            out.surfaceHr = E_NOINTERFACE;
            SafeReleaseCom(texture);
            return;
        }

        const auto getSurfaceLevel = reinterpret_cast<D3DTextureGetSurfaceLevel_t>(
            textureVtable[kD3DTextureGetSurfaceLevelVtableIndex]);
        out.surfaceHr = getSurfaceLevel(texture, 0u, &surface);
        if (SUCCEEDED(out.surfaceHr) && surface)
        {
            out.surface = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(surface));
            out.descHr = GetSurfaceDescSafe(surface, out.desc) ? S_OK : E_FAIL;
            out.containerTexture = RawTextureFromSurface(out.surface, &out.containerHr);
            out.containerMatchesTexture = out.containerTexture == out.texture ? 1u : 0u;
            SafeReleaseCom(surface);
        }

        SafeReleaseCom(texture);
    }

    void ProbeMode4DepthBridge(void* device)
    {
        if (s_v10Mode != 4 || !device || s_mode4DepthBridgeProbeState != 0)
            return;
        if (s_mode4MainSceneDepthSurface < 0x10000u || s_mode4NglDepthWrapper < 0x10000u)
            return;
        if (InterlockedCompareExchange(&s_mode4DepthBridgeProbeState, 1, 0) != 0)
            return;

        ScanMode4DepthBackendGraph(s_mode4MainSceneDepthSurface);
        ProbeDepthTextureRoundTrip(device, kFormatD24S8, s_mode4RoundTripD24S8);
        ProbeDepthTextureRoundTrip(device, kFormatINTZ, s_mode4RoundTripINTZ);

        Event* event = ReserveEvent(EventType::DepthBridgeProbe);
        if (event)
        {
            event->a = s_presentCount;
            event->b = s_mode4MainSceneDepthSurface;
            event->c = s_mode4NglDepthWrapper;
            event->d = s_mode4DepthBackendRoot;
            event->e = s_mode4DepthBackendDirectSurfaceOffset;
            event->f = s_mode4DepthBackendChildSlotOffset;
            event->g = s_mode4DepthBackendChild;
            event->h = s_mode4DepthBackendChildSurfaceOffset;
            event->i = static_cast<uint32_t>(s_mode4RoundTripD24S8.createHr);
            event->j = static_cast<uint32_t>(s_mode4RoundTripD24S8.surfaceHr);
            event->extra[0] = static_cast<uint32_t>(s_mode4RoundTripD24S8.descHr);
            event->extra[1] = static_cast<uint32_t>(s_mode4RoundTripD24S8.containerHr);
            event->extra[2] = s_mode4RoundTripD24S8.containerMatchesTexture;
            event->extra[3] = s_mode4RoundTripD24S8.desc.Format;
            event->extra[4] = static_cast<uint32_t>(s_mode4RoundTripINTZ.createHr);
            event->extra[5] = static_cast<uint32_t>(s_mode4RoundTripINTZ.surfaceHr);
            event->extra[6] = static_cast<uint32_t>(s_mode4RoundTripINTZ.descHr);
            event->extra[7] = static_cast<uint32_t>(s_mode4RoundTripINTZ.containerHr);
            event->extra[8] = s_mode4RoundTripINTZ.containerMatchesTexture;
            event->extra[9] = s_mode4RoundTripINTZ.desc.Format;
            event->extra[10] = s_mode4RoundTripD24S8.texture;
            event->extra[11] = s_mode4RoundTripD24S8.surface;
            event->extra[12] = s_mode4RoundTripINTZ.texture;
            event->extra[13] = s_mode4RoundTripINTZ.surface;
            event->extra[14] = s_mode4DepthBackendWords[0];
            event->extra[15] = s_mode4DepthBackendWords[1];
        }
    }

    void CaptureMode4NglDepthWrapper(uint32_t wrapper, uint32_t flags, uint32_t width, uint32_t height)
    {
        if (wrapper < 0x10000u)
            return;

        // Prefer the largest/full-size D24S8 wrapper. Do not mutate it.
        const uint64_t newArea = static_cast<uint64_t>(width) * static_cast<uint64_t>(height);
        const uint64_t oldArea =
            static_cast<uint64_t>(s_mode4NglDepthWrapperWidth) *
            static_cast<uint64_t>(s_mode4NglDepthWrapperHeight);
        if (s_mode4NglDepthWrapper != 0u && newArea < oldArea)
            return;

        s_mode4NglDepthWrapper = wrapper;
        s_mode4NglDepthWrapperFlags = flags;
        s_mode4NglDepthWrapperWidth = width;
        s_mode4NglDepthWrapperHeight = height;
        s_mode4NglDepthSurfaceMatchOffset = 0xFFFFFFFFu;

        if (IsReadableMemory(reinterpret_cast<void*>(static_cast<uintptr_t>(wrapper)),
            sizeof(s_mode4NglDepthWrapperWords)))
        {
            for (uint32_t n = 0; n < 32u; ++n)
                s_mode4NglDepthWrapperWords[n] = ReadU32(static_cast<uintptr_t>(wrapper) + n * 4u);
        }
    }

    void CorrelateMode4NglDepthWrapper(uint32_t surface)
    {
        if (surface < 0x10000u || s_mode4NglDepthWrapper < 0x10000u)
            return;
        if (!IsReadableMemory(
            reinterpret_cast<void*>(static_cast<uintptr_t>(s_mode4NglDepthWrapper)),
            sizeof(s_mode4NglDepthWrapperWords)))
            return;

        for (uint32_t n = 0; n < 32u; ++n)
        {
            const uint32_t word = ReadU32(
                static_cast<uintptr_t>(s_mode4NglDepthWrapper) + n * 4u);
            s_mode4NglDepthWrapperWords[n] = word;
            if (word == surface && s_mode4NglDepthSurfaceMatchOffset == 0xFFFFFFFFu)
                s_mode4NglDepthSurfaceMatchOffset = n * 4u;
        }
    }


    bool EnsureMode4FullResFloatDepthTarget(
        void* device, const D3DSurfaceDescLite& sceneDesc, void* activeRT, void* activeDS)
    {
        if (s_v10Mode != 4 || !device || !s_rawCreateTexture ||
            !s_rawCreateDepthStencilSurface || !s_rawSetRenderTarget ||
            !s_rawSetDepthStencilSurface || sceneDesc.Width == 0u || sceneDesc.Height == 0u)
        {
            return false;
        }

        if (!(s_fullResDepthTexture && s_fullResDepthSurface && s_fullResDepthStencil &&
              s_fullResDepthWidth == sceneDesc.Width && s_fullResDepthHeight == sceneDesc.Height))
        {
            if (InterlockedCompareExchange(&s_fullResDepthTargetCreateState, 1, 0) == 0)
            {
                void* texture = nullptr;
                s_fullResCreateTextureHr = s_rawCreateTexture(
                    device, sceneDesc.Width, sceneDesc.Height, 1u,
                    kD3DUsageRenderTarget, kFormatR32F, kD3DPoolDefault,
                    &texture, nullptr);

                void* surface = nullptr;
                if (SUCCEEDED(s_fullResCreateTextureHr) && texture &&
                    IsReadableMemory(texture, sizeof(void*)))
                {
                    void** vt = *reinterpret_cast<void***>(texture);
                    if (vt && IsReadableMemory(vt,
                        (kD3DTextureGetSurfaceLevelVtableIndex + 1u) * sizeof(void*)) &&
                        IsExecutableMemory(vt[kD3DTextureGetSurfaceLevelVtableIndex]))
                    {
                        const auto getSurfaceLevel =
                            reinterpret_cast<D3DTextureGetSurfaceLevel_t>(
                                vt[kD3DTextureGetSurfaceLevelVtableIndex]);
                        s_fullResGetSurfaceHr = getSurfaceLevel(texture, 0u, &surface);
                    }
                    else
                    {
                        s_fullResGetSurfaceHr = E_NOINTERFACE;
                    }
                }
                else
                {
                    s_fullResGetSurfaceHr = FAILED(s_fullResCreateTextureHr)
                        ? s_fullResCreateTextureHr : E_FAIL;
                }

                void* depth = nullptr;
                s_fullResCreateDepthHr = s_rawCreateDepthStencilSurface(
                    device, sceneDesc.Width, sceneDesc.Height, kFormatD24S8,
                    0u, 0u, FALSE, &depth, nullptr);

                if (texture && surface && depth &&
                    SUCCEEDED(s_fullResCreateTextureHr) &&
                    SUCCEEDED(s_fullResGetSurfaceHr) &&
                    SUCCEEDED(s_fullResCreateDepthHr))
                {
                    const uint32_t surfacePtr =
                        static_cast<uint32_t>(reinterpret_cast<uintptr_t>(surface));
                    s_fullResContainerTex = RawTextureFromSurface(
                        surfacePtr, &s_fullResContainerHr);
                    const uint32_t texturePtr =
                        static_cast<uint32_t>(reinterpret_cast<uintptr_t>(texture));

                    if (SUCCEEDED(s_fullResContainerHr) &&
                        s_fullResContainerTex == texturePtr)
                    {
                        s_fullResDepthTexture = texture;
                        s_fullResDepthSurface = surface;
                        s_fullResDepthStencil = depth;
                        s_fullResDepthWidth = sceneDesc.Width;
                        s_fullResDepthHeight = sceneDesc.Height;
                    }
                    else
                    {
                        SafeReleaseCom(depth);
                        SafeReleaseCom(surface);
                        SafeReleaseCom(texture);
                        InterlockedExchange(&s_fullResDepthTargetCreateState, -1);
                        return false;
                    }
                }
                else
                {
                    SafeReleaseCom(depth);
                    SafeReleaseCom(surface);
                    SafeReleaseCom(texture);
                    InterlockedExchange(&s_fullResDepthTargetCreateState, -1);
                    return false;
                }
            }

            if (!(s_fullResDepthTexture && s_fullResDepthSurface && s_fullResDepthStencil))
                return false;
        }

        // One-time mechanical bind/restore proof only. We do not clear or draw.
        if (InterlockedCompareExchange(&s_fullResDepthTargetBindTestState, 1, 0) == 0)
        {
            s_fullResBindRTHr =
                s_rawSetRenderTarget(device, 0u, s_fullResDepthSurface);
            if (SUCCEEDED(s_fullResBindRTHr))
                s_fullResBindDSHr =
                    s_rawSetDepthStencilSurface(device, s_fullResDepthStencil);
            else
                s_fullResBindDSHr = s_fullResBindRTHr;

            // Restore the exact live world state captured by the MID callback.
            s_fullResRestoreDSHr =
                s_rawSetDepthStencilSurface(device, activeDS);
            s_fullResRestoreRTHr =
                s_rawSetRenderTarget(device, 0u, activeRT);
        }

        return true;
    }

    bool RunMode4RenderListMainCameraDepthPrepass(void* device, void* activeRT, void* activeDS)
    {
        if (!device || !activeRT || !activeDS || !s_fullResDepthSurface ||
            !s_fullResDepthStencil || !s_rawCreateStateBlock ||
            !s_rawSetRenderTarget || !s_rawSetDepthStencilSurface)
        {
            return false;
        }

        const LONG profileState = s_mode4DepthPrepassRunState;
        if (profileState == 0)
        {
            // V10.5.28: exactly like the known-working V10.5.18.1 collector,
            // eligible Phat calls have already executed earlier in THIS scene
            // after our MID callback was installed. MID now freezes the result.
            Event* profileSummary = ReserveEvent(EventType::MainCameraDepthPrepass);
            if (profileSummary)
            {
                profileSummary->a = s_presentCount;
                profileSummary->b = static_cast<uint32_t>(s_mode4PhatCallerProfileCount);
                profileSummary->c = static_cast<uint32_t>(s_mode4PhatCallerProfileLogged);
                profileSummary->d = static_cast<uint32_t>(s_mode4PhatCallerProfileStackFailures);
                profileSummary->e = CurrentScene();
                profileSummary->f = 27u;
                profileSummary->extra[0] = 0u;
                profileSummary->extra[1] = s_presentCount;
                CopySmallText(profileSummary->text, sizeof(profileSummary->text), CurrentSceneName());
            }
            InterlockedExchange(&s_mode4DepthPrepassRunState, 1);
            s_mode4DepthReadbackPending = 0;
            return true;
        }

        return profileState == 1;

        // Historical V10.5.20-23 shadow-graph replay code retained below but
        // unreachable in this profile-only build for preservation/diffing.
        // V10.5.21: replay only top-level render-command classes that were
        // runtime-profiled in V10.5.19.2 and statically proven to branch into
        // native shadow/depth executors when g_shadow_render_pass_active != 0.
        //
        //   0x00516DD0 -> 0x00472C10   (profile count 1)
        //   0x004F9400 -> 0x004735C0   (profile count 2)
        //   0x004F9440 -> 0x0048D080   (profile count 4)
        //
        // This avoids the old hot 0x004F9340 detour and lets Treyarch's own
        // top-level graph dispatch nested geometry under shadow semantics.
        constexpr uint32_t kShadowGraphExecA = 0x00516DD0u;
        constexpr uint32_t kShadowGraphExecB = 0x004F9400u;
        constexpr uint32_t kShadowGraphExecC = 0x004F9440u;
        constexpr uint32_t kReplaySafetyCap = 16u;

        const uint32_t scene = ReadU32(kNGLCurrentSceneGlobal);
        const uint32_t sentinel = ReadU32(kRenderListSentinelGlobal);
        uint32_t node = 0u;
        if (scene >= 0x10000u && IsReadableMemory(
            reinterpret_cast<void*>(static_cast<uintptr_t>(scene + kScenePrimaryRenderListOffset)),
            sizeof(uint32_t)))
        {
            node = ReadU32(static_cast<uintptr_t>(scene + kScenePrimaryRenderListOffset));
        }

        const uint32_t listHead = node;
        uint32_t scanned = 0u;
        uint32_t eligible = 0u;
        uint32_t unreadable = 0u;
        uint32_t firstEligibleNode = 0u;
        uint32_t firstEligibleVtable = 0u;
        uint32_t firstEligibleExecute = 0u;

        // First pass: count and validate only. Do not mutate graphics state yet.
        for (uint32_t guard = 0u; node >= 0x10000u && node != sentinel && guard < 65536u; ++guard)
        {
            if (!IsReadableMemory(reinterpret_cast<void*>(static_cast<uintptr_t>(node)), 8u))
            {
                ++unreadable;
                break;
            }

            ++scanned;
            const uint32_t vtable = ReadU32(static_cast<uintptr_t>(node));
            uint32_t executeTarget = 0u;
            if (vtable >= 0x10000u && IsReadableMemory(
                reinterpret_cast<void*>(static_cast<uintptr_t>(vtable + 8u)), sizeof(uint32_t)))
            {
                executeTarget = ReadU32(static_cast<uintptr_t>(vtable + 8u));
            }

            if (executeTarget == kShadowGraphExecA ||
                executeTarget == kShadowGraphExecB ||
                executeTarget == kShadowGraphExecC)
            {
                ++eligible;
                if (firstEligibleNode == 0u)
                {
                    firstEligibleNode = node;
                    firstEligibleVtable = vtable;
                    firstEligibleExecute = executeTarget;
                }
            }

            const uint32_t next = ReadU32(static_cast<uintptr_t>(node + 4u));
            if (next == node)
                break;
            node = next;
        }

        s_mode4RenderListScanned = scanned;
        s_mode4RenderListPhatNodes = 0u;
        s_mode4RenderListEligible = eligible;
        s_mode4DepthCommandCount = 0;
        s_mode4DepthCommandOverflow = 0;
        s_mode4PrepassSelectedCommand = firstEligibleNode;
        s_mode4PrepassSelectedMaterialType = 0u;
        s_mode4PrepassReplayCount = 0u;

        if (eligible == 0u || eligible > kReplaySafetyCap || unreadable != 0u ||
            firstEligibleNode == 0u)
        {
            InterlockedExchange(&s_mode4DepthPrepassRunState, -1);

            Event* event = ReserveEvent(EventType::MainCameraDepthPrepass);
            if (event)
            {
                event->a = s_presentCount;
                event->b = scanned;
                event->c = 0u;
                event->d = eligible;
                event->e = firstEligibleNode;
                event->f = 0u;
                event->g = static_cast<uint32_t>(E_PENDING);
                event->h = static_cast<uint32_t>(E_PENDING);
                event->i = static_cast<uint32_t>(E_PENDING);
                event->j = static_cast<uint32_t>(E_PENDING);
                event->extra[0] = static_cast<uint32_t>(E_PENDING);
                event->extra[1] = 0u;
                event->extra[2] = static_cast<uint32_t>(E_PENDING);
                event->extra[3] = static_cast<uint32_t>(E_PENDING);
                event->extra[4] = static_cast<uint32_t>(E_PENDING);
                event->extra[5] = 0u;
                event->extra[6] = 0u;
                event->extra[7] = 0u;
                event->extra[8] = 0u;
                event->extra[9] = 0u;
                event->extra[10] = firstEligibleNode;
                event->extra[11] = firstEligibleVtable;
                event->extra[12] = firstEligibleExecute;
                event->extra[13] = 10u; // fail: topology selection
            }
            return false;
        }

        void* stateBlock = nullptr;
        s_mode4PrepassStateBlockHr = s_rawCreateStateBlock(device, kD3DStateBlockAll, &stateBlock);
        if (FAILED(s_mode4PrepassStateBlockHr) || !stateBlock ||
            !IsReadableMemory(stateBlock, sizeof(void*)))
        {
            SafeReleaseCom(stateBlock);
            InterlockedExchange(&s_mode4DepthPrepassRunState, -1);
            return false;
        }

        void** stateVtable = *reinterpret_cast<void***>(stateBlock);
        if (!stateVtable || !IsReadableMemory(stateVtable,
            (kD3DStateBlockApplyVtableIndex + 1u) * sizeof(void*)) ||
            !IsExecutableMemory(stateVtable[kD3DStateBlockCaptureVtableIndex]) ||
            !IsExecutableMemory(stateVtable[kD3DStateBlockApplyVtableIndex]))
        {
            SafeReleaseCom(stateBlock);
            InterlockedExchange(&s_mode4DepthPrepassRunState, -1);
            return false;
        }

        const auto capture = reinterpret_cast<D3DStateBlockCapture_t>(
            stateVtable[kD3DStateBlockCaptureVtableIndex]);
        const auto apply = reinterpret_cast<D3DStateBlockApply_t>(
            stateVtable[kD3DStateBlockApplyVtableIndex]);

        s_mode4PrepassCaptureHr = capture(stateBlock);
        if (FAILED(s_mode4PrepassCaptureHr))
        {
            SafeReleaseCom(stateBlock);
            InterlockedExchange(&s_mode4DepthPrepassRunState, -1);
            return false;
        }

        const uint32_t cache16 = ReadU32(kCacheRenderState16);
        const uint32_t cache1B = ReadU32(kCacheRenderState1B);
        const uint32_t cacheAF = ReadU32(kCacheRenderStateAF);
        const uint32_t cacheC3 = ReadU32(kCacheRenderStateC3);
        const uint32_t cachePS = ReadU32(kActivePSCache);
        const uint32_t cacheVS = ReadU32(kActiveVSCache);
        const uint8_t shadowFlagBefore = ReadU8(kShadowRenderPassFlagGlobal);

        s_mode4PrepassBindRTHr = s_rawSetRenderTarget(device, 0u, s_fullResDepthSurface);
        s_mode4PrepassBindDSHr = SUCCEEDED(s_mode4PrepassBindRTHr)
            ? s_rawSetDepthStencilSurface(device, s_fullResDepthStencil)
            : s_mode4PrepassBindRTHr;

        s_mode4PrepassClearHr = E_NOINTERFACE;
        if (SUCCEEDED(s_mode4PrepassBindRTHr) && SUCCEEDED(s_mode4PrepassBindDSHr) &&
            IsReadableMemory(device, sizeof(void*)))
        {
            void** deviceVtable = *reinterpret_cast<void***>(device);
            if (deviceVtable && IsReadableMemory(deviceVtable,
                (kD3DClearVtableIndex + 1u) * sizeof(void*)) &&
                IsExecutableMemory(deviceVtable[kD3DClearVtableIndex]))
            {
                const auto clear = reinterpret_cast<D3DClear_t>(deviceVtable[kD3DClearVtableIndex]);
                s_mode4PrepassClearHr = clear(device, 0u, nullptr,
                    kD3DClearTarget | kD3DClearZBuffer | kD3DClearStencil,
                    0xFFFFFFFFu, 1.0f, 0u);
            }
        }

        uint32_t replayed = 0u;
        if (SUCCEEDED(s_mode4PrepassClearHr) &&
            IsReadableMemory(reinterpret_cast<void*>(kShadowRenderPassFlagGlobal), 1u))
        {
            // V10.5.21: the native wrappers restore the scene/world color RT
            // internally. During this tiny replay window only, the raw D3D9 RT
            // hook redirects that exact captured world surface back to R32F.
            s_mode4ShadowGraphWorldRT = static_cast<uint32_t>(
                reinterpret_cast<uintptr_t>(activeRT));
            s_mode4ShadowGraphWorldRTWrapper = CurrentSceneRT();
            s_mode4ShadowGraphWorldScene = CurrentScene();
            s_mode4ShadowGraphReplacementRT = static_cast<uint32_t>(
                reinterpret_cast<uintptr_t>(s_fullResDepthSurface));
            InterlockedExchange(&s_mode4ShadowGraphRTPinAttempts, 0);
            InterlockedExchange(&s_mode4ShadowGraphRTPinRedirects, 0);
            s_mode4ShadowGraphLastRequestedRT = 0u;
            s_mode4ShadowGraphLastEffectiveRT = 0u;
            s_mode4ShadowGraphLastReturnAddress = 0u;
            s_mode4ShadowGraphLastRTHr = E_PENDING;
            s_mode4ShadowGraphLastRedirectReturnAddress = 0u;
            s_mode4ShadowGraphLastRedirectHr = E_PENDING;
            s_mode4ShadowGraphLastNglResult = 0;
            s_mode4ShadowGraphLastActualRTBefore = 0u;
            s_mode4ShadowGraphLastActualRTAfter = 0u;
            s_mode4ShadowGraphLastShadowPS = 0u;
            InterlockedExchange(&s_mode4ShadowGraphRTPinActive, 1);

            *reinterpret_cast<volatile uint8_t*>(kShadowRenderPassFlagGlobal) = 1u;

            node = listHead;
            for (uint32_t guard = 0u;
                 node >= 0x10000u && node != sentinel && guard < 65536u;
                 ++guard)
            {
                if (!IsReadableMemory(reinterpret_cast<void*>(static_cast<uintptr_t>(node)), 8u))
                    break;

                const uint32_t vtable = ReadU32(static_cast<uintptr_t>(node));
                uint32_t executeTarget = 0u;
                if (vtable >= 0x10000u && IsReadableMemory(
                    reinterpret_cast<void*>(static_cast<uintptr_t>(vtable + 8u)), sizeof(uint32_t)))
                {
                    executeTarget = ReadU32(static_cast<uintptr_t>(vtable + 8u));
                }

                if ((executeTarget == kShadowGraphExecA ||
                     executeTarget == kShadowGraphExecB ||
                     executeTarget == kShadowGraphExecC) &&
                    IsExecutableMemory(reinterpret_cast<void*>(static_cast<uintptr_t>(executeTarget))))
                {
                    const auto execute = reinterpret_cast<PhatRenderCommandExecute_t>(
                        static_cast<uintptr_t>(executeTarget));
                    execute(reinterpret_cast<void*>(static_cast<uintptr_t>(node)));
                    ++replayed;
                    if (replayed >= kReplaySafetyCap)
                        break;
                }

                const uint32_t next = ReadU32(static_cast<uintptr_t>(node + 4u));
                if (next == node)
                    break;
                node = next;
            }

            // Stop redirecting BEFORE restoring any stock state/surfaces.
            InterlockedExchange(&s_mode4ShadowGraphRTPinActive, 0);
            *reinterpret_cast<volatile uint8_t*>(kShadowRenderPassFlagGlobal) = shadowFlagBefore;
        }

        // Fail closed if an early condition ever bypasses the normal replay exit.
        InterlockedExchange(&s_mode4ShadowGraphRTPinActive, 0);
        s_mode4PrepassReplayCount = replayed;

        // Restore world surfaces first, then full D3D state, then NGL's matching caches.
        s_mode4PrepassRestoreDSHr = s_rawSetDepthStencilSurface(device, activeDS);
        s_mode4PrepassRestoreRTHr = s_rawSetRenderTarget(device, 0u, activeRT);
        s_mode4PrepassApplyHr = apply(stateBlock);
        SafeReleaseCom(stateBlock);

        WriteU32(kCacheRenderState16, cache16);
        WriteU32(kCacheRenderState1B, cache1B);
        WriteU32(kCacheRenderStateAF, cacheAF);
        WriteU32(kCacheRenderStateC3, cacheC3);
        WriteU32(kActivePSCache, cachePS);
        WriteU32(kActiveVSCache, cacheVS);

        const bool ok = replayed == eligible &&
            replayed > 0u &&
            SUCCEEDED(s_mode4PrepassRestoreDSHr) &&
            SUCCEEDED(s_mode4PrepassRestoreRTHr) &&
            SUCCEEDED(s_mode4PrepassApplyHr) &&
            ReadU8(kShadowRenderPassFlagGlobal) == shadowFlagBefore;

        if (ok)
        {
            InterlockedExchange(&s_mode4DepthReadbackPending, 1);
            InterlockedExchange(&s_mode4DepthPrepassRunState, 1);
        }
        else
        {
            InterlockedExchange(&s_mode4DepthPrepassRunState, -1);
        }

        Event* pinEvent = ReserveEvent(EventType::ShadowGraphRTPin);
        if (pinEvent)
        {
            pinEvent->a = s_presentCount;
            pinEvent->b = static_cast<uint32_t>(s_mode4ShadowGraphRTPinAttempts);
            pinEvent->c = static_cast<uint32_t>(s_mode4ShadowGraphRTPinRedirects);
            pinEvent->d = s_mode4ShadowGraphWorldScene;
            pinEvent->e = s_mode4ShadowGraphReplacementRT;
            pinEvent->f = s_mode4ShadowGraphLastActualRTBefore;
            pinEvent->g = s_mode4ShadowGraphLastActualRTAfter;
            pinEvent->h = s_mode4ShadowGraphLastReturnAddress;
            pinEvent->i = static_cast<uint32_t>(s_mode4ShadowGraphLastRTHr);
            pinEvent->j = replayed;
            pinEvent->extra[0] = s_mode4ShadowGraphLastShadowPS;
            pinEvent->extra[1] = static_cast<uint32_t>(s_mode4ShadowGraphLastRedirectHr);
            pinEvent->extra[2] = s_mode4ShadowGraphWorldRTWrapper;
            pinEvent->extra[3] = s_mode4ShadowGraphWorldRT;
        }

        Event* event = ReserveEvent(EventType::MainCameraDepthPrepass);
        if (event)
        {
            event->a = s_presentCount;
            event->b = scanned;
            event->c = 0u;
            event->d = eligible;
            event->e = firstEligibleNode;
            event->f = static_cast<uint32_t>(shadowFlagBefore);
            event->g = static_cast<uint32_t>(s_mode4PrepassStateBlockHr);
            event->h = static_cast<uint32_t>(s_mode4PrepassCaptureHr);
            event->i = static_cast<uint32_t>(s_mode4PrepassBindRTHr);
            event->j = static_cast<uint32_t>(s_mode4PrepassBindDSHr);
            event->extra[0] = static_cast<uint32_t>(s_mode4PrepassClearHr);
            event->extra[1] = replayed;
            event->extra[2] = static_cast<uint32_t>(s_mode4PrepassRestoreDSHr);
            event->extra[3] = static_cast<uint32_t>(s_mode4PrepassRestoreRTHr);
            event->extra[4] = static_cast<uint32_t>(s_mode4PrepassApplyHr);
            event->extra[5] = cachePS;
            event->extra[6] = cacheVS;
            event->extra[7] = ReadU32(kActivePSCache);
            event->extra[8] = ReadU32(kActiveVSCache);
            event->extra[9] = ok ? 1u : 0u;
            event->extra[10] = firstEligibleNode;
            event->extra[11] = firstEligibleVtable;
            event->extra[12] = firstEligibleExecute;
            event->extra[13] = ok ? 0u : 20u;
        }
        return ok;
    }

    void SampleMode4DepthPrepassReadback(void* device)
    {
        if (InterlockedCompareExchange(&s_mode4DepthReadbackPending, 0, 1) != 1)
            return;
        if (!device || !s_fullResDepthSurface || !s_rawCreateOffscreenPlainSurface ||
            !s_rawGetRenderTargetData)
            return;

        if (!s_mode4DepthReadbackSurface)
        {
            s_mode4DepthReadbackCreateHr = s_rawCreateOffscreenPlainSurface(
                device, s_fullResDepthWidth, s_fullResDepthHeight, kFormatR32F,
                kD3DPoolSystemMem, &s_mode4DepthReadbackSurface, nullptr);
        }
        if (FAILED(s_mode4DepthReadbackCreateHr) || !s_mode4DepthReadbackSurface)
            return;

        s_mode4DepthReadbackHr = s_rawGetRenderTargetData(
            device, s_fullResDepthSurface, s_mode4DepthReadbackSurface);
        if (FAILED(s_mode4DepthReadbackHr) ||
            !IsReadableMemory(s_mode4DepthReadbackSurface, sizeof(void*)))
            return;

        void** vt = *reinterpret_cast<void***>(s_mode4DepthReadbackSurface);
        if (!vt || !IsReadableMemory(vt,
            (kD3DSurfaceUnlockRectVtableIndex + 1u) * sizeof(void*)) ||
            !IsExecutableMemory(vt[kD3DSurfaceLockRectVtableIndex]) ||
            !IsExecutableMemory(vt[kD3DSurfaceUnlockRectVtableIndex]))
            return;

        const auto lockRect = reinterpret_cast<D3DSurfaceLockRect_t>(
            vt[kD3DSurfaceLockRectVtableIndex]);
        const auto unlockRect = reinterpret_cast<D3DSurfaceUnlockRect_t>(
            vt[kD3DSurfaceUnlockRectVtableIndex]);
        D3DLockedRectLite locked = {};
        s_mode4DepthReadbackLockHr = lockRect(
            s_mode4DepthReadbackSurface, &locked, nullptr, kD3DLockReadOnly);
        if (FAILED(s_mode4DepthReadbackLockHr) || !locked.pBits || locked.Pitch <= 0)
            return;

        float minV = 3.402823466e+38F;
        float maxV = -3.402823466e+38F;
        double sum = 0.0;
        uint32_t finiteCount = 0u;
        uint32_t changedCount = 0u;
        for (uint32_t y = 0; y < s_fullResDepthHeight; ++y)
        {
            const float* row = reinterpret_cast<const float*>(
                static_cast<const uint8_t*>(locked.pBits) + static_cast<size_t>(y) * locked.Pitch);
            for (uint32_t x = 0; x < s_fullResDepthWidth; ++x)
            {
                const float value = row[x];
                if (!IsFiniteUsefulFloat(value))
                    continue;
                if (value < minV) minV = value;
                if (value > maxV) maxV = value;
                sum += static_cast<double>(value);
                ++finiteCount;
                if (std::fabs(static_cast<double>(value) - 1.0) > 0.00001)
                    ++changedCount;
            }
        }
        unlockRect(s_mode4DepthReadbackSurface);

        if (finiteCount == 0u)
        {
            minV = 0.0f;
            maxV = 0.0f;
        }
        s_mode4DepthReadbackMin = minV;
        s_mode4DepthReadbackMax = maxV;
        s_mode4DepthReadbackMean = finiteCount ? static_cast<float>(sum / finiteCount) : 0.0f;
        s_mode4DepthReadbackFiniteCount = finiteCount;
        s_mode4DepthReadbackChangedCount = changedCount;

        Event* event = ReserveEvent(EventType::MainCameraDepthReadback);
        if (event)
        {
            event->a = s_presentCount;
            event->b = static_cast<uint32_t>(s_mode4DepthReadbackCreateHr);
            event->c = static_cast<uint32_t>(s_mode4DepthReadbackHr);
            event->d = static_cast<uint32_t>(s_mode4DepthReadbackLockHr);
            event->e = s_fullResDepthWidth;
            event->f = s_fullResDepthHeight;
            event->g = finiteCount;
            event->h = changedCount;
            event->i = FloatBits(minV);
            event->j = FloatBits(maxV);
            event->extra[0] = FloatBits(s_mode4DepthReadbackMean);
            event->extra[1] = static_cast<uint32_t>(s_mode4DepthPrepassRunState);
            event->extra[2] = s_mode4PrepassSelectedCommand;
            event->extra[3] = s_mode4PrepassSelectedMaterialType;
        }
    }

    bool EnsureMode4TextureBackedSceneDepth(void* device, const D3DSurfaceDescLite& sourceDesc)
    {
        if (s_v10Mode != 4 || !device || !s_rawCreateTexture)
            return false;

        if (s_mode4ReplacementDepthTexture && s_mode4ReplacementDepthSurface &&
            s_mode4ReplacementWidth == sourceDesc.Width &&
            s_mode4ReplacementHeight == sourceDesc.Height)
        {
            D3DSurfaceDescLite check = {};
            if (GetSurfaceDescSafe(s_mode4ReplacementDepthSurface, check) &&
                check.Width == sourceDesc.Width && check.Height == sourceDesc.Height &&
                check.Format == s_mode4ReplacementFormat)
                return true;
        }

        // Safety build: create only once. Do not try to recover from device reset
        // in this experiment; the user should avoid alt-tab/resolution changes.
        if (InterlockedCompareExchange(&s_mode4DepthReplacementCreateState, 1, 0) != 0)
            return s_mode4ReplacementDepthTexture && s_mode4ReplacementDepthSurface;

        void* texture = nullptr;
        s_mode4ReplacementCreateHr = s_rawCreateTexture(
            device, sourceDesc.Width, sourceDesc.Height, 1u,
            kD3DUsageDepthStencil, s_mode4ReplacementFormat, kD3DPoolDefault,
            &texture, nullptr);
        if (FAILED(s_mode4ReplacementCreateHr) || !texture)
        {
            InterlockedExchange(&s_mode4DepthReplacementCreateState, -1);
            return false;
        }

        void** textureVtable = IsReadableMemory(texture, sizeof(void*))
            ? *reinterpret_cast<void***>(texture) : nullptr;
        if (!textureVtable || !IsReadableMemory(textureVtable,
            (kD3DTextureGetSurfaceLevelVtableIndex + 1u) * sizeof(void*)) ||
            !IsExecutableMemory(textureVtable[kD3DTextureGetSurfaceLevelVtableIndex]))
        {
            SafeReleaseCom(texture);
            s_mode4ReplacementSurfaceHr = E_NOINTERFACE;
            InterlockedExchange(&s_mode4DepthReplacementCreateState, -1);
            return false;
        }

        const auto getSurfaceLevel = reinterpret_cast<D3DTextureGetSurfaceLevel_t>(
            textureVtable[kD3DTextureGetSurfaceLevelVtableIndex]);
        void* surface = nullptr;
        s_mode4ReplacementSurfaceHr = getSurfaceLevel(texture, 0u, &surface);
        if (FAILED(s_mode4ReplacementSurfaceHr) || !surface)
        {
            SafeReleaseCom(texture);
            InterlockedExchange(&s_mode4DepthReplacementCreateState, -1);
            return false;
        }

        const uint32_t surfacePtr = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(surface));
        s_mode4ReplacementContainerTex = RawTextureFromSurface(
            surfacePtr, &s_mode4ReplacementContainerHr);
        const uint32_t texturePtr = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(texture));
        if (FAILED(s_mode4ReplacementContainerHr) ||
            s_mode4ReplacementContainerTex != texturePtr)
        {
            SafeReleaseCom(surface);
            SafeReleaseCom(texture);
            InterlockedExchange(&s_mode4DepthReplacementCreateState, -1);
            return false;
        }

        s_mode4ReplacementDepthTexture = texture;
        s_mode4ReplacementDepthSurface = surface;
        s_mode4ReplacementWidth = sourceDesc.Width;
        s_mode4ReplacementHeight = sourceDesc.Height;
        InterlockedExchange(&s_mode4DepthReplacementCreateState, 1);
        return true;
    }

    HRESULT WINAPI RawCreateDepthStencilSurface_Hook(
        void* device, UINT width, UINT height, DWORD format, DWORD multiSampleType,
        DWORD multiSampleQuality, BOOL discard, void** outSurface, HANDLE* sharedHandle)
    {
        const uintptr_t returnAddress = reinterpret_cast<uintptr_t>(_ReturnAddress());
        const HRESULT hr = s_rawCreateDepthStencilSurface(
            device, width, height, format, multiSampleType, multiSampleQuality,
            discard, outSurface, sharedHandle);

        const uint32_t surfacePtr =
            (SUCCEEDED(hr) && outSurface && *outSurface)
            ? static_cast<uint32_t>(reinterpret_cast<uintptr_t>(*outSurface))
            : 0u;

        const LONG count = InterlockedIncrement(&s_rawDepthCreateCount);
        if (count <= 24 || ShouldLogSparse(count))
        {
            HRESULT containerHr = E_PENDING;
            uint32_t containerTex = 0u;
            D3DSurfaceDescLite desc = {};
            HRESULT descHr = E_PENDING;

            if (surfacePtr >= 0x10000u)
            {
                if (GetSurfaceDescSafe(reinterpret_cast<void*>(static_cast<uintptr_t>(surfacePtr)), desc))
                    descHr = S_OK;
                else
                    descHr = E_FAIL;
                containerTex = RawTextureFromSurface(surfacePtr, &containerHr);
            }

            Event* event = ReserveEvent(EventType::RawDepthStencilCreate);
            if (event)
            {
                event->a = s_presentCount;
                event->b = static_cast<uint32_t>(count);
                event->c = static_cast<uint32_t>(returnAddress);
                event->d = static_cast<uint32_t>(hr);
                event->e = surfacePtr;
                event->f = format;
                event->g = width;
                event->h = height;
                event->i = multiSampleType;
                event->j = multiSampleQuality;
                event->extra[0] = static_cast<uint32_t>(discard);
                event->extra[1] = static_cast<uint32_t>(descHr);
                event->extra[2] = static_cast<uint32_t>(containerHr);
                event->extra[3] = containerTex;
                event->extra[4] = desc.Format;
                event->extra[5] = desc.Usage;
                event->extra[6] = desc.Pool;
            }
        }
        return hr;
    }

    HRESULT WINAPI RawSetDepthStencilSurface_Hook(void* device, void* surface)
    {
        const uintptr_t returnAddress = reinterpret_cast<uintptr_t>(_ReturnAddress());
        const uint32_t surfacePtr = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(surface));

        HRESULT sourceDescHr = E_PENDING;
        D3DSurfaceDescLite sourceDesc = {};
        if (surfacePtr >= 0x10000u)
            sourceDescHr = GetSurfaceDescSafe(surface, sourceDesc) ? S_OK : E_FAIL;

        bool replacementCandidate = false;
        bool replacementAttempted = false;
        bool replacementApplied = false;
        HRESULT replacementBindHr = E_PENDING;
        HRESULT fallbackBindHr = E_PENDING;
        void* effectiveSurface = surface;

        // Exact safety gate: only the full-size NGL D24S8 scene depth. The
        // explicit 512x512 / 1024x1024 auxiliary depth surfaces are untouched.
        if (s_v10Mode == 4 && !s_mode4MidProbeOnly && SUCCEEDED(sourceDescHr) && sourceDesc.Format == kFormatD24S8 &&
            s_mode4NglDepthWrapperWidth != 0u && s_mode4NglDepthWrapperHeight != 0u &&
            sourceDesc.Width == s_mode4NglDepthWrapperWidth &&
            sourceDesc.Height == s_mode4NglDepthWrapperHeight)
        {
            replacementCandidate = true;
            if (EnsureMode4TextureBackedSceneDepth(device, sourceDesc))
            {
                replacementAttempted = true;
                effectiveSurface = s_mode4ReplacementDepthSurface;
            }
        }

        HRESULT hr = s_rawSetDepthStencilSurface(device, effectiveSurface);
        if (replacementAttempted)
        {
            replacementBindHr = hr;
            if (SUCCEEDED(hr))
            {
                replacementApplied = true;
                InterlockedIncrement(&s_mode4DepthReplacementAppliedCount);
            }
            else
            {
                // Immediate fail-safe: restore the game's original standalone
                // D24S8 surface if our texture-backed surface is rejected.
                fallbackBindHr = s_rawSetDepthStencilSurface(device, surface);
                hr = fallbackBindHr;
                effectiveSurface = surface;
            }
        }

        const LONG count = InterlockedIncrement(&s_rawDepthBindCount);

        HRESULT descHr = sourceDescHr;
        HRESULT containerHr = E_PENDING;
        uint32_t containerTex = 0u;
        D3DSurfaceDescLite desc = sourceDesc;

        if (SUCCEEDED(hr) && surfacePtr >= 0x10000u)
        {
            containerTex = RawTextureFromSurface(surfacePtr, &containerHr);

            // Preserve original game-surface identity for research correlation.
            s_mode4CapturedDepthSurface = surfacePtr;
            s_mode4CapturedDepthTexture = containerTex;
            s_mode4CapturedDepthContainerHr = containerHr;
            s_mode4CapturedDepthDesc = desc;
            s_mode4CapturedDepthSetReturn = static_cast<uint32_t>(returnAddress);

            if (desc.Format == kFormatD24S8)
            {
                CorrelateMode4NglDepthWrapper(surfacePtr);
                if (desc.Width == s_mode4NglDepthWrapperWidth &&
                    desc.Height == s_mode4NglDepthWrapperHeight)
                {
                    s_mode4MainSceneDepthSurface = surfacePtr;
                    s_mode4MainSceneDepthDesc = desc;
                }
            }
        }

        if (replacementCandidate)
        {
            const LONG replaceCount = InterlockedIncrement(&s_mode4DepthReplacementBindCount);
            if (replaceCount <= 32 || ShouldLogSparse(replaceCount))
            {
                uint32_t activeDepth = 0u;
                HRESULT activeDepthHr = E_PENDING;
                void* activeSurface = nullptr;
                if (s_rawGetDepthStencilSurface)
                {
                    activeDepthHr = s_rawGetDepthStencilSurface(device, &activeSurface);
                    if (SUCCEEDED(activeDepthHr) && activeSurface)
                    {
                        activeDepth = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(activeSurface));
                        SafeReleaseCom(activeSurface);
                    }
                }

                Event* event = ReserveEvent(EventType::DepthReplacementTest);
                if (event)
                {
                    event->a = s_presentCount;
                    event->b = static_cast<uint32_t>(replaceCount);
                    event->c = static_cast<uint32_t>(returnAddress);
                    event->d = surfacePtr;
                    event->e = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(s_mode4ReplacementDepthSurface));
                    event->f = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(s_mode4ReplacementDepthTexture));
                    event->g = replacementAttempted ? 1u : 0u;
                    event->h = replacementApplied ? 1u : 0u;
                    event->i = static_cast<uint32_t>(replacementBindHr);
                    event->j = static_cast<uint32_t>(fallbackBindHr);
                    event->extra[0] = static_cast<uint32_t>(s_mode4ReplacementCreateHr);
                    event->extra[1] = static_cast<uint32_t>(s_mode4ReplacementSurfaceHr);
                    event->extra[2] = static_cast<uint32_t>(s_mode4ReplacementContainerHr);
                    event->extra[3] = s_mode4ReplacementContainerTex;
                    event->extra[4] = sourceDesc.Width;
                    event->extra[5] = sourceDesc.Height;
                    event->extra[6] = sourceDesc.Format;
                    event->extra[7] = static_cast<uint32_t>(activeDepthHr);
                    event->extra[8] = activeDepth;
                    event->extra[9] = static_cast<uint32_t>(hr);
                    event->extra[10] = s_mode4ReplacementFormat;
                }
            }
        }

        if (count <= 40 || ShouldLogSparse(count))
        {
            Event* event = ReserveEvent(EventType::RawDepthStencilBind);
            if (event)
            {
                event->a = s_presentCount;
                event->b = static_cast<uint32_t>(count);
                event->c = static_cast<uint32_t>(returnAddress);
                event->d = surfacePtr;
                event->e = static_cast<uint32_t>(hr);
                event->f = static_cast<uint32_t>(descHr);
                event->g = static_cast<uint32_t>(containerHr);
                event->h = containerTex;
                event->i = desc.Format;
                event->j = desc.Usage;
                event->extra[0] = desc.MultiSampleType;
                event->extra[1] = desc.MultiSampleQuality;
                event->extra[2] = desc.Width;
                event->extra[3] = desc.Height;
                event->extra[4] = s_rawCurrentRT0;
                event->extra[5] = s_rawCurrentPixelShader;
                event->extra[6] = s_mode4CapturedDepthSurface;
                event->extra[7] = s_mode4CapturedDepthTexture;
                event->extra[8] = s_mode4NglDepthWrapper;
                event->extra[9] = s_mode4NglDepthSurfaceMatchOffset;
            }
        }
        return hr;
    }

    HRESULT WINAPI RawSetRenderTarget_Hook(void* device, DWORD index, void* surface)
    {
        const uintptr_t returnAddress = reinterpret_cast<uintptr_t>(_ReturnAddress());
        const uint32_t surfacePtr = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(surface));
        const uint32_t wrapperInFlight = s_nglSetRTWrapperInFlight;

        void* effectiveSurface = surface;
        uint32_t effectiveSurfacePtr = surfacePtr;
        bool rtPinRedirected = false;
        const bool rtPinActive = false; // V10.5.23 pin moved to NGL SetRT_Hook; raw hook is observation-only.
        if (rtPinActive && index == 0u)
        {
            InterlockedIncrement(&s_mode4ShadowGraphRTPinAttempts);
            s_mode4ShadowGraphLastRequestedRT = surfacePtr;
            s_mode4ShadowGraphLastReturnAddress = static_cast<uint32_t>(returnAddress);

            const uint32_t worldRT = s_mode4ShadowGraphWorldRT;
            const uint32_t replacementRT = s_mode4ShadowGraphReplacementRT;
            if (worldRT != 0u && replacementRT != 0u && surfacePtr == worldRT)
            {
                effectiveSurface = reinterpret_cast<void*>(static_cast<uintptr_t>(replacementRT));
                effectiveSurfacePtr = replacementRT;
                rtPinRedirected = true;
                InterlockedIncrement(&s_mode4ShadowGraphRTPinRedirects);
            }
        }

        const HRESULT hr = s_rawSetRenderTarget(device, index, effectiveSurface);
        if (rtPinActive && index == 0u)
        {
            s_mode4ShadowGraphLastEffectiveRT = effectiveSurfacePtr;
            s_mode4ShadowGraphLastRTHr = hr;
            if (rtPinRedirected)
            {
                s_mode4ShadowGraphLastRedirectReturnAddress = static_cast<uint32_t>(returnAddress);
                s_mode4ShadowGraphLastRedirectHr = hr;
            }
        }
        if (SUCCEEDED(hr) && index == 0u)
        {
            s_rawCurrentRT0 = effectiveSurfacePtr;
            // Never teach the persistent NGL-wrapper map a temporary pinned RT.
            if (wrapperInFlight && !rtPinActive)
                LearnRawSurface(wrapperInFlight, effectiveSurfacePtr);
        }

        const uint32_t kind = ClassifyRawSurface(effectiveSurfacePtr);
        LONG exactCount = 0;
        uint32_t exactWhich = 0;
        if (kind == 4u)
        {
            exactCount = InterlockedIncrement(&s_rawLumSetRTCount);
            exactWhich = 1;
        }
        else if (kind == 5u)
        {
            exactCount = InterlockedIncrement(&s_rawSecondSetRTCount);
            exactWhich = 2;
        }

        if (exactWhich && ShouldLogSparse(exactCount))
        {
            Event* event = ReserveEvent(EventType::RawRenderTargetExact);
            if (event)
            {
                event->a = static_cast<uint32_t>(exactCount);
                event->b = exactWhich;
                event->c = static_cast<uint32_t>(returnAddress);
                event->d = index;
                event->e = surfacePtr;
                event->f = wrapperInFlight;
                event->g = static_cast<uint32_t>(hr);
                event->h = s_rawCurrentPixelShader;
                event->i = CurrentScene();
                CopySmallText(event->text, sizeof(event->text), CurrentSceneName());
            }
        }

        if (ConsumeRawTraceBudget())
        {
            Event* event = ReserveEvent(EventType::RawSetRenderTarget);
            if (event)
            {
                event->a = s_presentCount;
                event->b = static_cast<uint32_t>(returnAddress);
                event->c = index;
                event->d = effectiveSurfacePtr;
                event->e = kind;
                event->f = wrapperInFlight;
                event->g = static_cast<uint32_t>(hr);
                event->h = s_rawCurrentPixelShader;
                event->i = CurrentScene();
                event->extra[0] = surfacePtr;
                event->extra[1] = rtPinRedirected ? 1u : 0u;
                CopySmallText(event->text, sizeof(event->text), CurrentSceneName());
            }
        }
        return hr;
    }

    HRESULT WINAPI RawSetPixelShader_Hook(void* device, void* shader)
    {
        const uintptr_t returnAddress = reinterpret_cast<uintptr_t>(_ReturnAddress());
        const uint32_t shaderPtr = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(shader));
        const HRESULT hr = s_rawSetPixelShader(device, shader);
        if (SUCCEEDED(hr))
            s_rawCurrentPixelShader = shaderPtr;
        InterlockedIncrement(&s_rawSetPixelShaderCount);

        // V10.5.23: this hook is the first interception point proven to fire
        // inside the one-shot 7-wrapper world replay. When sm_depth_shadow is
        // selected in that exact world scene, immediately reassert xeSM3's
        // full-resolution R32F as physical RT0 before control returns to the
        // caller and the nested geometry draw proceeds.
        if (s_v10Mode == 4)
        {
            const uint32_t shadowPS = ReadU32(kShadowDepthPixelShaderGlobal);
            if (shadowPS >= 0x10000u && shaderPtr == shadowPS)
            {
                const bool pinActive =
                    s_mode4ShadowGraphRTPinActive != 0 &&
                    CurrentScene() == s_mode4ShadowGraphWorldScene &&
                    s_mode4ShadowGraphReplacementRT >= 0x10000u &&
                    device && s_rawSetRenderTarget;

                if (pinActive)
                {
                    InterlockedIncrement(&s_mode4ShadowGraphRTPinAttempts);
                    s_mode4ShadowGraphLastShadowPS = shadowPS;
                    s_mode4ShadowGraphLastReturnAddress =
                        static_cast<uint32_t>(returnAddress);

                    void* beforeRT = nullptr;
                    if (s_rawGetRenderTarget &&
                        SUCCEEDED(s_rawGetRenderTarget(device, 0u, &beforeRT)) &&
                        beforeRT)
                    {
                        s_mode4ShadowGraphLastActualRTBefore =
                            static_cast<uint32_t>(reinterpret_cast<uintptr_t>(beforeRT));
                        SafeReleaseCom(beforeRT);
                    }

                    void* replacement = reinterpret_cast<void*>(
                        static_cast<uintptr_t>(s_mode4ShadowGraphReplacementRT));
                    const HRESULT pinHr =
                        s_rawSetRenderTarget(device, 0u, replacement);
                    s_mode4ShadowGraphLastRTHr = pinHr;
                    s_mode4ShadowGraphLastRedirectHr = pinHr;
                    s_mode4ShadowGraphLastRedirectReturnAddress =
                        static_cast<uint32_t>(returnAddress);

                    if (SUCCEEDED(pinHr))
                    {
                        InterlockedIncrement(&s_mode4ShadowGraphRTPinRedirects);
                        s_rawCurrentRT0 = s_mode4ShadowGraphReplacementRT;
                        s_mode4ShadowGraphLastEffectiveRT =
                            s_mode4ShadowGraphReplacementRT;
                    }

                    void* afterRT = nullptr;
                    if (s_rawGetRenderTarget &&
                        SUCCEEDED(s_rawGetRenderTarget(device, 0u, &afterRT)) &&
                        afterRT)
                    {
                        s_mode4ShadowGraphLastActualRTAfter =
                            static_cast<uint32_t>(reinterpret_cast<uintptr_t>(afterRT));
                        SafeReleaseCom(afterRT);
                    }
                }

                const LONG shadowCount =
                    InterlockedIncrement(&s_shadowDepthTargetProbeCount);
                if (ShouldLogSparse(shadowCount))
                {
                    // Query the physical device RT0. Older builds used
                    // s_rawCurrentRT0 here, which can be stale when xeSM3 calls
                    // the original SetRenderTarget trampoline directly.
                    uint32_t rt0 = 0u;
                    void* activeRT = nullptr;
                    HRESULT rtHr = E_PENDING;
                    if (device && s_rawGetRenderTarget)
                    {
                        rtHr = s_rawGetRenderTarget(device, 0u, &activeRT);
                        if (SUCCEEDED(rtHr) && activeRT)
                            rt0 = static_cast<uint32_t>(
                                reinterpret_cast<uintptr_t>(activeRT));
                    }
                    if (rt0 == 0u)
                        rt0 = s_rawCurrentRT0;

                    D3DSurfaceDescLite rtDesc = {};
                    D3DSurfaceDescLite dsDesc = {};
                    bool rtDescOk = false;
                    bool dsDescOk = false;
                    HRESULT containerHr = E_PENDING;
                    uint32_t rtTexture = 0u;
                    void* activeDS = nullptr;
                    HRESULT dsHr = E_PENDING;

                    if (rt0 >= 0x10000u)
                    {
                        rtDescOk = GetSurfaceDescSafe(
                            reinterpret_cast<void*>(static_cast<uintptr_t>(rt0)),
                            rtDesc);
                        rtTexture = RawTextureFromSurface(rt0, &containerHr);
                    }

                    if (device && s_rawGetDepthStencilSurface)
                    {
                        dsHr = s_rawGetDepthStencilSurface(device, &activeDS);
                        if (SUCCEEDED(dsHr) && activeDS)
                            dsDescOk = GetSurfaceDescSafe(activeDS, dsDesc);
                    }

                    Event* shadowEvent =
                        ReserveEvent(EventType::ShadowDepthTargetProbe);
                    if (shadowEvent)
                    {
                        shadowEvent->a = s_presentCount;
                        shadowEvent->b = static_cast<uint32_t>(shadowCount);
                        shadowEvent->c = shadowPS;
                        shadowEvent->d = rt0;
                        shadowEvent->e = rtDescOk ? rtDesc.Format : 0u;
                        shadowEvent->f = rtDescOk ? rtDesc.Width : 0u;
                        shadowEvent->g = rtDescOk ? rtDesc.Height : 0u;
                        shadowEvent->h = static_cast<uint32_t>(containerHr);
                        shadowEvent->i = rtTexture;
                        shadowEvent->j = static_cast<uint32_t>(
                            reinterpret_cast<uintptr_t>(activeDS));
                        shadowEvent->extra[0] = static_cast<uint32_t>(dsHr);
                        shadowEvent->extra[1] = dsDescOk ? dsDesc.Format : 0u;
                        shadowEvent->extra[2] = dsDescOk ? dsDesc.Width : 0u;
                        shadowEvent->extra[3] = dsDescOk ? dsDesc.Height : 0u;
                        shadowEvent->extra[4] = CurrentScene();
                        shadowEvent->extra[5] =
                            ReadU32(kShadowDepthVertexShaderGlobal);
                        shadowEvent->extra[6] =
                            rtDescOk ? rtDesc.Usage : 0u;
                        shadowEvent->extra[7] =
                            dsDescOk ? dsDesc.Usage : 0u;
                        // Preserve a proof that the RT was physically queried.
                        shadowEvent->extra[8] =
                            static_cast<uint32_t>(rtHr);
                        CopySmallText(
                            shadowEvent->text,
                            sizeof(shadowEvent->text),
                            CurrentSceneName());
                    }

                    if (activeRT)
                        SafeReleaseCom(activeRT);
                    if (activeDS)
                        SafeReleaseCom(activeDS);
                }
            }
        }

        if (ConsumeRawTraceBudget())
        {
            Event* event = ReserveEvent(EventType::RawSetPixelShader);
            if (event)
            {
                event->a = s_presentCount;
                event->b = static_cast<uint32_t>(returnAddress);
                event->c = shaderPtr;
                event->d = static_cast<uint32_t>(hr);
                event->e = s_rawCurrentRT0;
                event->f = s_rawTextureStages[0];
                event->g = s_rawTextureStages[1];
                event->h = s_rawTextureStages[2];
                event->i = s_rawTextureStages[3];
                event->j = CurrentScene();
                CopySmallText(event->text, sizeof(event->text), CurrentSceneName());
            }
        }
        return hr;
    }

    HRESULT WINAPI RawSetPixelShaderConstantF_Hook(void* device, UINT startRegister, const float* data, UINT vector4Count)
    {
        const uintptr_t returnAddress = reinterpret_cast<uintptr_t>(_ReturnAddress());
        const uint32_t activePS = s_rawCurrentPixelShader;
        const HRESULT hr = s_rawSetPixelShaderConstantF(device, startRegister, data, vector4Count);
        InterlockedIncrement(&s_rawPSConstantCount);

        if (data && vector4Count && ConsumeRawTraceBudget())
        {
            Event* event = ReserveEvent(EventType::RawPixelShaderConstantF);
            if (event)
            {
                event->a = s_presentCount;
                event->b = static_cast<uint32_t>(returnAddress);
                event->c = startRegister;
                event->d = vector4Count;
                event->e = activePS;
                event->f = s_rawCurrentRT0;
                event->g = static_cast<uint32_t>(hr);
                event->h = CurrentScene();
                event->i = (vector4Count > 4u) ? 4u : vector4Count;
                const uint32_t floatCount = event->i * 4u;
                for (uint32_t n = 0; n < floatCount && n < 16u; ++n)
                    memcpy(&event->extra[n], &data[n], sizeof(uint32_t));
                CopySmallText(event->text, sizeof(event->text), CurrentSceneName());
            }
        }
        return hr;
    }

    // Forward declarations for the exact DrawPrimitive hook. Implementations live
    // in the native final/ImageZoom section below.
    bool ResolveMode5FinalShaderDeviceApis(
        void*& device, D3DCreatePixelShader_t& createPixelShader,
        D3DSetPixelShader_t& setPixelShader, D3DGetPixelShader_t& getPixelShader);
    bool EnsureMode5FinalCloneShader(HRESULT& createHr, HRESULT& verifyQueryHr, HRESULT& verifyFetchHr);
    bool QueryPixelShaderIdentity(void* shader, uint32_t& byteCountOut, uint32_t& hashOut,
        HRESULT& queryHrOut, HRESULT& fetchHrOut);
    void RunRetailXboxImageZoomRouteBindProbe();
    bool RunRetailXboxImageZoomNativeDraw(
        void* device, DWORD primitiveType, UINT startVertex, UINT primitiveCount);

    HRESULT DrawMode5ActualClampAtFinalPrimitive(
        void* device, DWORD primitiveType, UINT startVertex, UINT primitiveCount,
        uintptr_t returnAddress)
    {
        const LONG attempt = InterlockedIncrement(&s_actualFinalClampDrawCount);
        InterlockedIncrement(&s_mode5FinalCloneDrawCount);
        HRESULT createHr = E_PENDING, verifyQueryHr = E_PENDING, verifyFetchHr = E_PENDING;
        HRESULT beforeGetHr = E_PENDING, bindHr = E_PENDING, boundGetHr = E_PENDING;
        HRESULT drawHr = E_PENDING, afterGetHr = E_PENDING, restoreHr = E_PENDING, restoredGetHr = E_PENDING;
        void* apiDevice = nullptr;
        D3DCreatePixelShader_t createPS = nullptr;
        D3DSetPixelShader_t setPS = nullptr;
        D3DGetPixelShader_t getPS = nullptr;
        void* beforePS = nullptr;
        void* boundPS = nullptr;
        void* afterPS = nullptr;
        void* restoredPS = nullptr;
        const uint32_t expectedFinal = ReadU32(kCompositePSFinal);
        bool cloneReady = EnsureMode5FinalCloneShader(createHr, verifyQueryHr, verifyFetchHr);
        bool beforeVerified = false, bindVerified = false, heldDuringDraw = false, restoreVerified = false;

        if (cloneReady && ResolveMode5FinalShaderDeviceApis(apiDevice, createPS, setPS, getPS) && apiDevice == device)
        {
            beforeGetHr = getPS(device, &beforePS);
            beforeVerified = SUCCEEDED(beforeGetHr) && beforePS &&
                static_cast<uint32_t>(reinterpret_cast<uintptr_t>(beforePS)) == expectedFinal;
            if (beforeVerified)
            {
                bindHr = setPS(device, s_mode5FinalCloneShader);
                if (SUCCEEDED(bindHr))
                {
                    boundGetHr = getPS(device, &boundPS);
                    bindVerified = SUCCEEDED(boundGetHr) && boundPS == s_mode5FinalCloneShader;
                }
            }
        }

        if (bindVerified)
        {
            drawHr = s_rawDrawPrimitive(device, primitiveType, startVertex, primitiveCount);
            afterGetHr = getPS(device, &afterPS);
            heldDuringDraw = SUCCEEDED(afterGetHr) && afterPS == s_mode5FinalCloneShader;
            restoreHr = setPS(device, beforePS);
            if (SUCCEEDED(restoreHr))
            {
                restoredGetHr = getPS(device, &restoredPS);
                restoreVerified = SUCCEEDED(restoredGetHr) && restoredPS == beforePS;
            }
        }
        else
        {
            if (setPS && beforePS)
                setPS(device, beforePS);
            drawHr = s_rawDrawPrimitive(device, primitiveType, startVertex, primitiveCount);
            InterlockedIncrement(&s_actualFinalStockFallbackCount);
        }

        const bool pass = cloneReady && beforeVerified && bindVerified && SUCCEEDED(drawHr) &&
            heldDuringDraw && restoreVerified;
        if (pass)
        {
            InterlockedIncrement(&s_actualFinalClampPassCount);
            InterlockedIncrement(&s_mode5FinalClonePassCount);
        }
        else
        {
            InterlockedIncrement(&s_actualFinalClampFailCount);
            InterlockedIncrement(&s_mode5FinalCloneFailCount);
        }

        const bool logClampSuccess = s_postProcessRoute == 3u
            ? ShouldLogDebugXboxIntegrationDetail(attempt)
            : ShouldLogSparse(attempt);
        if (logClampSuccess || !pass)
        {
            Event* event = ReserveEvent(EventType::NativeFinalShaderActualClampDraw);
            if (event)
            {
                event->a = s_presentCount;
                event->b = static_cast<uint32_t>(attempt);
                event->c = static_cast<uint32_t>(returnAddress);
                event->d = expectedFinal;
                event->e = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(s_mode5FinalCloneShader));
                event->f = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(beforePS));
                event->g = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(boundPS));
                event->h = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(afterPS));
                event->i = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(restoredPS));
                event->j = pass ? 1u : 0u;
                event->extra[0] = cloneReady ? 1u : 0u;
                event->extra[1] = beforeVerified ? 1u : 0u;
                event->extra[2] = bindVerified ? 1u : 0u;
                event->extra[3] = heldDuringDraw ? 1u : 0u;
                event->extra[4] = restoreVerified ? 1u : 0u;
                event->extra[5] = static_cast<uint32_t>(drawHr);
                event->extra[6] = static_cast<uint32_t>(bindHr);
                event->extra[7] = static_cast<uint32_t>(afterGetHr);
                event->extra[8] = static_cast<uint32_t>(restoreHr);
                event->extra[9] = static_cast<uint32_t>(s_actualFinalClampPassCount);
                event->extra[10] = static_cast<uint32_t>(s_actualFinalClampFailCount);
                event->extra[11] = static_cast<uint32_t>(s_actualFinalStockFallbackCount);
                event->extra[12] = primitiveType;
                event->extra[13] = startVertex;
                event->extra[14] = primitiveCount;
                event->extra[15] = s_postProcessRoute;
            }
        }

        SafeReleaseCom(beforePS);
        SafeReleaseCom(boundPS);
        SafeReleaseCom(afterPS);
        SafeReleaseCom(restoredPS);
        return drawHr;
    }

    bool TryRetailXboxF18AtActualDraw(
        void* device, DWORD primitiveType, UINT startVertex, UINT primitiveCount,
        uintptr_t returnAddress, HRESULT& drawResult)
    {
        drawResult = E_PENDING;
        const LONG attempt = InterlockedIncrement(&s_actualFinalDrawAttemptCount);
        InterlockedIncrement(&s_retailXboxDofDrawAttemptCount);
        if (s_actualFinalF18Disabled != 0 || !IsXboxPostProcessRoute())
            return false;

        const uint32_t expectedFinal = ReadU32(kCompositePSFinal);
        const uint32_t dofShaderValue = ReadU32(kCompositePSDofFinal);
        const uint32_t depthTextureValue = s_provenIntzDofDepthTexture;
        const uint32_t depthAge = (s_provenIntzDofPresent <= s_presentCount)
            ? (s_presentCount - s_provenIntzDofPresent) : 0xFFFFFFFFu;
        const bool depthReady = s_mode4ReszDepthValidated && s_reszDepthResolveState == 3 &&
            s_provenIntzDofState == 1 && s_provenIntzDofPresent == s_presentCount &&
            depthTextureValue >= 0x10000u &&
            depthTextureValue == static_cast<uint32_t>(reinterpret_cast<uintptr_t>(s_mode4PackedDepthTexture));

        void** dvt = device && IsReadableMemory(device, sizeof(void*))
            ? *reinterpret_cast<void***>(device) : nullptr;
        if (!dvt || !IsReadableMemory(dvt, (kD3DGetPixelShaderConstantFVtableIndex + 1u) * sizeof(void*)) ||
            !IsExecutableMemory(dvt[kD3DGetPixelShaderVtableIndex]) ||
            !IsExecutableMemory(dvt[kD3DSetPixelShaderVtableIndex]) ||
            !IsExecutableMemory(dvt[kD3DGetTextureVtableIndex]) ||
            !IsExecutableMemory(dvt[kD3DSetTextureVtableIndex]) ||
            !IsExecutableMemory(dvt[kD3DGetPixelShaderConstantFVtableIndex]) ||
            !IsExecutableMemory(dvt[kD3DSetPixelShaderConstantFVtableIndex]))
            return false;

        const auto getPS = reinterpret_cast<D3DGetPixelShader_t>(dvt[kD3DGetPixelShaderVtableIndex]);
        const auto setPS = reinterpret_cast<D3DSetPixelShader_t>(dvt[kD3DSetPixelShaderVtableIndex]);
        const auto getTexture = reinterpret_cast<D3DGetTexture_t>(dvt[kD3DGetTextureVtableIndex]);
        const auto setTexture = reinterpret_cast<D3DSetTexture_t>(dvt[kD3DSetTextureVtableIndex]);
        const auto getPSC0 = reinterpret_cast<D3DGetPixelShaderConstantF_t>(dvt[kD3DGetPixelShaderConstantFVtableIndex]);
        const auto setPSC0 = reinterpret_cast<D3DSetPixelShaderConstantF_t>(dvt[kD3DSetPixelShaderConstantFVtableIndex]);

        HRESULT queryHr = E_FAIL, fetchHr = E_PENDING;
        uint32_t dofBytes = 0u, dofHash = 0u;
        void* dofShader = dofShaderValue >= 0x10000u
            ? reinterpret_cast<void*>(static_cast<uintptr_t>(dofShaderValue)) : nullptr;
        const bool dofValid = dofShader && QueryPixelShaderIdentity(dofShader, dofBytes, dofHash, queryHr, fetchHr);
        if (dofValid)
        {
            s_retailXboxDofShaderBytes = dofBytes;
            s_retailXboxDofShaderHash = dofHash;
        }

        HRESULT beforePsHr=E_FAIL, tex0Hr=E_FAIL, tex1Hr=E_FAIL, oldC0Hr=E_FAIL;
        HRESULT bindDepthHr=E_PENDING, boundDepthHr=E_PENDING, setC0Hr=E_PENDING, boundC0Hr=E_PENDING;
        HRESULT bindF18Hr=E_PENDING, boundPsHr=E_PENDING, afterPsHr=E_PENDING, afterDepthHr=E_PENDING;
        HRESULT restorePsHr=E_PENDING, restoreDepthHr=E_PENDING, restoreC0Hr=E_PENDING;
        HRESULT restoredPsHr=E_PENDING, restoredDepthHr=E_PENDING, restoredC0Hr=E_PENDING;
        void* beforePS=nullptr; void* source0=nullptr; void* oldS1=nullptr; void* boundS1=nullptr;
        void* boundPS=nullptr; void* afterPS=nullptr; void* afterS1=nullptr; void* restoredPS=nullptr; void* restoredS1=nullptr;
        float oldC0[4]={}, dofC0[4]={}, boundC0[4]={}, restoredC0[4]={};

        beforePsHr = getPS(device, &beforePS);
        tex0Hr = getTexture(device, 0u, &source0);
        tex1Hr = getTexture(device, 1u, &oldS1);
        oldC0Hr = getPSC0(device, 0u, oldC0, 1u);
        const bool beforeVerified = SUCCEEDED(beforePsHr) && beforePS &&
            static_cast<uint32_t>(reinterpret_cast<uintptr_t>(beforePS)) == expectedFinal;
        const bool sourcePresent = SUCCEEDED(tex0Hr) && source0 != nullptr;
        const bool oldC0Readable = SUCCEEDED(oldC0Hr);
        const bool dynamicC0Valid = BuildMode4BloomDepthControl(dofC0);

        bool depthVerified=false, c0Verified=false, f18Verified=false;
        bool drawHeldF18=false, drawHeldDepth=false;
        bool psRestoreVerified=false, depthRestoreVerified=false, c0RestoreVerified=false;
        bool drew=false;

        const bool preflight = depthReady && dofValid && beforeVerified && sourcePresent &&
            SUCCEEDED(tex1Hr) && oldC0Readable && dynamicC0Valid;
        if (preflight)
        {
            void* expectedDepth = reinterpret_cast<void*>(static_cast<uintptr_t>(depthTextureValue));
            bindDepthHr = setTexture(device, 1u, expectedDepth);
            if (SUCCEEDED(bindDepthHr))
            {
                boundDepthHr = getTexture(device, 1u, &boundS1);
                depthVerified = SUCCEEDED(boundDepthHr) && boundS1 == expectedDepth;
            }
            if (depthVerified)
            {
                setC0Hr = setPSC0(device, 0u, dofC0, 1u);
                if (SUCCEEDED(setC0Hr))
                {
                    boundC0Hr = getPSC0(device, 0u, boundC0, 1u);
                    c0Verified = SUCCEEDED(boundC0Hr) && memcmp(boundC0, dofC0, sizeof(dofC0)) == 0;
                }
            }
            if (c0Verified)
            {
                bindF18Hr = setPS(device, dofShader);
                if (SUCCEEDED(bindF18Hr))
                {
                    boundPsHr = getPS(device, &boundPS);
                    f18Verified = SUCCEEDED(boundPsHr) && boundPS == dofShader;
                }
            }
            if (f18Verified)
            {
                drawResult = s_rawDrawPrimitive(device, primitiveType, startVertex, primitiveCount);
                drew = true;
                InterlockedIncrement(&s_actualFinalF18DrawCount);
                InterlockedIncrement(&s_retailXboxDofDrawCount);
                afterPsHr = getPS(device, &afterPS);
                afterDepthHr = getTexture(device, 1u, &afterS1);
                drawHeldF18 = SUCCEEDED(afterPsHr) && afterPS == dofShader;
                drawHeldDepth = SUCCEEDED(afterDepthHr) && afterS1 == expectedDepth;
            }
        }

        // Restore every touched input whether or not the pre-draw chain completed.
        if (beforePS)
        {
            restorePsHr = setPS(device, beforePS);
            if (SUCCEEDED(restorePsHr))
            {
                restoredPsHr = getPS(device, &restoredPS);
                psRestoreVerified = SUCCEEDED(restoredPsHr) && restoredPS == beforePS;
            }
        }
        if (SUCCEEDED(tex1Hr))
        {
            restoreDepthHr = setTexture(device, 1u, oldS1);
            if (SUCCEEDED(restoreDepthHr))
            {
                restoredDepthHr = getTexture(device, 1u, &restoredS1);
                depthRestoreVerified = SUCCEEDED(restoredDepthHr) && restoredS1 == oldS1;
            }
        }
        if (oldC0Readable)
        {
            restoreC0Hr = setPSC0(device, 0u, oldC0, 1u);
            if (SUCCEEDED(restoreC0Hr))
            {
                restoredC0Hr = getPSC0(device, 0u, restoredC0, 1u);
                c0RestoreVerified = SUCCEEDED(restoredC0Hr) && memcmp(restoredC0, oldC0, sizeof(oldC0)) == 0;
            }
        }

        const bool pass = drew && SUCCEEDED(drawResult) && depthReady && dofValid && beforeVerified &&
            sourcePresent && dynamicC0Valid && depthVerified && c0Verified && f18Verified &&
            drawHeldF18 && drawHeldDepth && psRestoreVerified && depthRestoreVerified && c0RestoreVerified;

        if (pass)
        {
            InterlockedIncrement(&s_actualFinalF18PassCount);
            InterlockedIncrement(&s_retailXboxDofDrawPassCount);
            InterlockedIncrement(&s_retailXboxDepthFeedPassCount);
            InterlockedExchange(&s_provenIntzDofState, 3);
            // V10.5.63 Retail integration: the real F18 final DrawPrimitive has completed and
            // all F18 state is restored. The ImageZoom pass now copies that finished
            // target into the proven non-aliased safe source and, only if every R5
            // contract + render-state guard passes, issues exactly ONE second real
            // DrawPrimitive through the Detours trampoline (no hook recursion).
            RunRetailXboxImageZoomNativeDraw(device, primitiveType, startVertex, primitiveCount);
        }
        else if (drew)
        {
            InterlockedIncrement(&s_actualFinalF18FailCount);
            InterlockedIncrement(&s_retailXboxDofDrawFailCount);
            InterlockedExchange(&s_actualFinalF18Disabled, 1);
        }

        if (ShouldLogDebugXboxIntegrationDetail(attempt) || (drew && !pass) || (depthReady && !pass))
        {
            Event* event = ReserveEvent(EventType::RetailXboxActualFinalDraw);
            if (event)
            {
                event->a=s_presentCount; event->b=static_cast<uint32_t>(attempt);
                event->c=static_cast<uint32_t>(returnAddress); event->d=expectedFinal;
                event->e=dofShaderValue; event->f=depthTextureValue;
                event->g=static_cast<uint32_t>(reinterpret_cast<uintptr_t>(beforePS));
                event->h=static_cast<uint32_t>(reinterpret_cast<uintptr_t>(afterPS));
                event->i=pass?1u:0u; event->j=drew?1u:0u;
                event->extra[0]=depthReady?1u:0u; event->extra[1]=depthAge;
                event->extra[2]=dofValid?1u:0u; event->extra[3]=beforeVerified?1u:0u;
                event->extra[4]=sourcePresent?1u:0u; event->extra[5]=dynamicC0Valid?1u:0u;
                event->extra[6]=depthVerified?1u:0u; event->extra[7]=c0Verified?1u:0u;
                event->extra[8]=f18Verified?1u:0u; event->extra[9]=drawHeldF18?1u:0u;
                event->extra[10]=drawHeldDepth?1u:0u; event->extra[11]=psRestoreVerified?1u:0u;
                event->extra[12]=depthRestoreVerified?1u:0u; event->extra[13]=c0RestoreVerified?1u:0u;
                event->extra[14]=static_cast<uint32_t>(drawResult); event->extra[15]=primitiveType;
                event->extra[16]=startVertex; event->extra[17]=primitiveCount;
                event->extra[18]=static_cast<uint32_t>(s_actualFinalF18DrawCount);
                event->extra[19]=static_cast<uint32_t>(s_actualFinalF18PassCount);
                event->extra[20]=static_cast<uint32_t>(s_actualFinalF18FailCount);
                event->extra[21]=static_cast<uint32_t>(s_actualFinalF18Disabled);
                event->extra[22]=FloatBits(dofC0[0]); event->extra[23]=FloatBits(dofC0[1]);
                event->extra[24]=FloatBits(dofC0[2]); event->extra[25]=FloatBits(dofC0[3]);
                event->extra[26]=s_postProcessRoute; event->extra[27]=static_cast<uint32_t>(s_retailXboxImageZoomDrawCount); // R6 second-pass ImageZoom draw count.
            }
        }

        SafeReleaseCom(beforePS); SafeReleaseCom(source0); SafeReleaseCom(oldS1); SafeReleaseCom(boundS1);
        SafeReleaseCom(boundPS); SafeReleaseCom(afterPS); SafeReleaseCom(afterS1); SafeReleaseCom(restoredPS); SafeReleaseCom(restoredS1);
        return drew;
    }

    bool CaptureGodRayG1FinalContract(void* device, DWORD primitiveType, UINT startVertex, UINT primitiveCount, uintptr_t returnAddress)
    {
        if (s_v10Mode != 5 || s_postProcessRoute != 3u ||
            returnAddress != kGodRayFinalDrawPrimitiveReturn ||
            primitiveType != kD3DPrimitiveTriangleStrip || startVertex != 0u || primitiveCount != 2u)
            return false;

        const bool selectorReadable = IsReadableMemory(
            reinterpret_cast<void*>(kPcDormantGodRaySelector), sizeof(uint32_t));
        const uint32_t selectorRaw = selectorReadable ? ReadU32(kPcDormantGodRaySelector) : 0u;
        if (!selectorReadable || selectorRaw == 0u)
            return false;

        const LONG seen = InterlockedIncrement(&s_godRayG1FinalSeenCount);
        const bool captureNow = ShouldValidateDebugXboxIntegrationCheckpoint(seen) ||
            s_godRayG1LastTransitionPresent == s_presentCount;
        if (!captureNow)
            return true;
        s_godRayG1LastCapturePresent = s_presentCount;

        void** dvt = device && IsReadableMemory(device, sizeof(void*))
            ? *reinterpret_cast<void***>(device) : nullptr;
        const uint32_t requiredMax = kD3DGetPixelShaderConstantFVtableIndex;
        if (!dvt || !IsReadableMemory(dvt, (requiredMax + 1u) * sizeof(void*)) ||
            !IsExecutableMemory(dvt[kD3DGetRenderTargetVtableIndex]) ||
            !IsExecutableMemory(dvt[kD3DGetDepthStencilSurfaceVtableIndex]) ||
            !IsExecutableMemory(dvt[kD3DGetViewportVtableIndex]) ||
            !IsExecutableMemory(dvt[kD3DGetRenderStateVtableIndex]) ||
            !IsExecutableMemory(dvt[kD3DGetTextureVtableIndex]) ||
            !IsExecutableMemory(dvt[kD3DGetSamplerStateVtableIndex]) ||
            !IsExecutableMemory(dvt[kD3DGetVertexShaderVtableIndex]) ||
            !IsExecutableMemory(dvt[kD3DGetVertexShaderConstantFVtableIndex]) ||
            !IsExecutableMemory(dvt[kD3DGetPixelShaderVtableIndex]) ||
            !IsExecutableMemory(dvt[kD3DGetPixelShaderConstantFVtableIndex]))
        {
            InterlockedIncrement(&s_godRayG1ContractFailCount);
            return true;
        }

        const auto getRT = reinterpret_cast<D3DGetRenderTarget_t>(dvt[kD3DGetRenderTargetVtableIndex]);
        const auto getDS = reinterpret_cast<D3DGetDepthStencilSurface_t>(dvt[kD3DGetDepthStencilSurfaceVtableIndex]);
        const auto getViewport = reinterpret_cast<D3DGetViewport_t>(dvt[kD3DGetViewportVtableIndex]);
        const auto getRender = reinterpret_cast<D3DGetRenderState_t>(dvt[kD3DGetRenderStateVtableIndex]);
        const auto getTexture = reinterpret_cast<D3DGetTexture_t>(dvt[kD3DGetTextureVtableIndex]);
        const auto getSampler = reinterpret_cast<D3DGetSamplerState_t>(dvt[kD3DGetSamplerStateVtableIndex]);
        const auto getVS = reinterpret_cast<D3DGetVertexShader_t>(dvt[kD3DGetVertexShaderVtableIndex]);
        const auto getVSC = reinterpret_cast<D3DGetVertexShaderConstantF_t>(dvt[kD3DGetVertexShaderConstantFVtableIndex]);
        const auto getPS = reinterpret_cast<D3DGetPixelShader_t>(dvt[kD3DGetPixelShaderVtableIndex]);
        const auto getPSC = reinterpret_cast<D3DGetPixelShaderConstantF_t>(dvt[kD3DGetPixelShaderConstantFVtableIndex]);

        void* activeVS=nullptr; void* activePS=nullptr;
        void* tex0=nullptr; void* tex1=nullptr; void* tex2=nullptr;
        void* rt0=nullptr; void* ds=nullptr;
        float psC[28]={};
        float vsC[16]={};
        D3DViewportLite viewport={};
        DWORD srcBlend=0u, dstBlend=0u, alphaBlend=0u, colorWrite=0u, zEnable=0u, zWrite=0u;
        DWORD samp0AddrU=0u, samp0AddrV=0u, samp0Min=0u, samp0Mag=0u;
        DWORD samp1AddrU=0u, samp1AddrV=0u, samp1Min=0u, samp1Mag=0u;
        DWORD samp2AddrU=0u, samp2AddrV=0u, samp2Min=0u, samp2Mag=0u;

        const HRESULT vsHr=getVS(device,&activeVS);
        const HRESULT psHr=getPS(device,&activePS);
        const HRESULT t0Hr=getTexture(device,0u,&tex0);
        const HRESULT t1Hr=getTexture(device,1u,&tex1);
        const HRESULT t2Hr=getTexture(device,2u,&tex2);
        const HRESULT psCHr=getPSC(device,0u,psC,7u);
        const HRESULT vsCHr=getVSC(device,0u,vsC,4u);
        const HRESULT rtHr=getRT(device,0u,&rt0);
        const HRESULT dsHr=getDS(device,&ds);
        const HRESULT vpHr=getViewport(device,&viewport);
        const HRESULT rs0Hr=getRender(device,kRSSrcBlend,&srcBlend);
        const HRESULT rs1Hr=getRender(device,kRSDestBlend,&dstBlend);
        const HRESULT rs2Hr=getRender(device,kRSAlphaBlendEnable,&alphaBlend);
        const HRESULT rs3Hr=getRender(device,kRSColorWriteEnable,&colorWrite);
        const HRESULT rs4Hr=getRender(device,kRSZEnable,&zEnable);
        const HRESULT rs5Hr=getRender(device,kRSZWriteEnable,&zWrite);
        const HRESULT s00=getSampler(device,0u,kSampAddressU,&samp0AddrU);
        const HRESULT s01=getSampler(device,0u,kSampAddressV,&samp0AddrV);
        const HRESULT s02=getSampler(device,0u,kSampMinFilter,&samp0Min);
        const HRESULT s03=getSampler(device,0u,kSampMagFilter,&samp0Mag);
        const HRESULT s10=getSampler(device,1u,kSampAddressU,&samp1AddrU);
        const HRESULT s11=getSampler(device,1u,kSampAddressV,&samp1AddrV);
        const HRESULT s12=getSampler(device,1u,kSampMinFilter,&samp1Min);
        const HRESULT s13=getSampler(device,1u,kSampMagFilter,&samp1Mag);
        const HRESULT s20=getSampler(device,2u,kSampAddressU,&samp2AddrU);
        const HRESULT s21=getSampler(device,2u,kSampAddressV,&samp2AddrV);
        const HRESULT s22=getSampler(device,2u,kSampMinFilter,&samp2Min);
        const HRESULT s23=getSampler(device,2u,kSampMagFilter,&samp2Mag);

        uint32_t vsBytes=0u, vsHash=0u, psBytes=0u, psHash=0u;
        HRESULT vsQuery=E_FAIL, vsFetch=E_PENDING, psQuery=E_FAIL, psFetch=E_PENDING;
        const bool vsId = SUCCEEDED(vsHr) && activeVS &&
            QueryPixelShaderIdentity(activeVS,vsBytes,vsHash,vsQuery,vsFetch);
        const bool psId = SUCCEEDED(psHr) && activePS &&
            QueryPixelShaderIdentity(activePS,psBytes,psHash,psQuery,psFetch);
        if (vsId) s_godRayG1LastVsHash=vsHash;
        if (psId) s_godRayG1LastPsHash=psHash;

        const uint32_t expectedVS=ReadU32(kCompositeVS);
        const uint32_t expectedPS=ReadU32(kGodRayFinalPixelShader);
        const uint32_t sourceSelectBits=ReadU32(0x00EE5E8Cu);
        const uint32_t quarterWrapper = sourceSelectBits == 0u ? ReadU32(kRtQuarterA) : ReadU32(kRtQuarterB);

        // V10.5.66.1 G1 validation fix: the final GodRay s0 is the real D3D texture
        // owned by the selected quarter-resolution RT wrapper. The old descriptor walk
        // (wrapper+0x14 -> RawTextureFromNglDescriptor) returned 0 for this RT layout.
        // Reuse the raw identity already proven by Debug D2, with an authoritative
        // wrapper+0x34 surface -> GetContainer fallback if the cache is not populated.
        uint32_t expectedTex0 = sourceSelectBits == 0u
            ? static_cast<uint32_t>(s_rawTextureQuarterA)
            : static_cast<uint32_t>(s_rawTextureQuarterB);
        if (expectedTex0 < 0x10000u && quarterWrapper >= 0x10000u)
        {
            const uint32_t quarterSurface = RawSurfaceDirectFromWrapper(quarterWrapper);
            HRESULT expectedTex0Hr = E_PENDING;
            expectedTex0 = RawTextureFromSurface(quarterSurface, &expectedTex0Hr);
        }

        const uint32_t expectedTex1 = RawTextureFromNglDescriptor(kPostFxDepthDescriptor);
        const uint32_t optionalWrapper = ReadU32(kGodRayOptionalTextureWrapper);
        uint32_t expectedTex2 = 0u;
        if (optionalWrapper >= 0x10000u)
        {
            const uint32_t optionalSurface = RawSurfaceDirectFromWrapper(optionalWrapper);
            HRESULT expectedTex2Hr = E_PENDING;
            expectedTex2 = RawTextureFromSurface(optionalSurface, &expectedTex2Hr);
            if (expectedTex2 < 0x10000u)
                expectedTex2 = RawTextureFromNglDescriptor(optionalWrapper + 0x14u);
        }
        const uint32_t sceneSurface=RawSurfaceDirectFromWrapper(CurrentSceneRT());

        // Stock GodRay runs with Z disabled and no depth-stencil surface bound.
        // D3D9 reports D3DERR_NOTFOUND in that valid state, so do not require
        // SUCCEEDED(GetDepthStencilSurface). Any other failure remains unsafe.
        const bool noDepthExpected = dsHr == kD3DErrNotFound && ds == nullptr &&
            SUCCEEDED(rs4Hr) && SUCCEEDED(rs5Hr) && zEnable == 0u && zWrite == 0u;
        const bool depthStateKnown = SUCCEEDED(dsHr) || noDepthExpected;
        const uint32_t actualTex0 = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(tex0));
        const uint32_t actualTex1 = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(tex1));
        const uint32_t actualTex2 = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(tex2));
        const uint32_t actualRt0 = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(rt0));
        const bool textureContract = expectedTex0 >= 0x10000u && actualTex0 == expectedTex0 &&
            actualTex1 == expectedTex1 && actualTex2 == expectedTex2;
        const bool targetContract = sceneSurface >= 0x10000u && actualRt0 == sceneSurface;
        const bool renderQueriesOk = SUCCEEDED(rs0Hr) && SUCCEEDED(rs1Hr) && SUCCEEDED(rs2Hr) &&
            SUCCEEDED(rs3Hr) && SUCCEEDED(rs4Hr) && SUCCEEDED(rs5Hr);
        const bool samplerQueriesOk = SUCCEEDED(s00) && SUCCEEDED(s01) && SUCCEEDED(s02) && SUCCEEDED(s03) &&
            SUCCEEDED(s10) && SUCCEEDED(s11) && SUCCEEDED(s12) && SUCCEEDED(s13) &&
            SUCCEEDED(s20) && SUCCEEDED(s21) && SUCCEEDED(s22) && SUCCEEDED(s23);

        const bool coreOk = SUCCEEDED(vsHr)&&SUCCEEDED(psHr)&&SUCCEEDED(t0Hr)&&SUCCEEDED(t1Hr)&&SUCCEEDED(t2Hr)&&
            SUCCEEDED(psCHr)&&SUCCEEDED(vsCHr)&&SUCCEEDED(rtHr)&&depthStateKnown&&SUCCEEDED(vpHr)&&
            renderQueriesOk&&samplerQueriesOk&&vsId&&psId&&activeVS&&activePS&&rt0&&
            static_cast<uint32_t>(reinterpret_cast<uintptr_t>(activeVS))==expectedVS&&
            static_cast<uint32_t>(reinterpret_cast<uintptr_t>(activePS))==expectedPS&&
            textureContract&&targetContract;
        if (coreOk) InterlockedIncrement(&s_godRayG1ContractCaptureCount);
        else InterlockedIncrement(&s_godRayG1ContractFailCount);

        const bool emitG1Detail = !coreOk || ShouldLogDebugXboxIntegrationDetail(seen);
        Event* summary = emitG1Detail ? ReserveEvent(EventType::GodRayG1FinalContract) : nullptr;
        if (summary)
        {
            summary->a=s_presentCount; summary->b=static_cast<uint32_t>(seen); summary->c=selectorRaw;
            summary->d=static_cast<uint32_t>(returnAddress); summary->e=coreOk?1u:0u;
            summary->f=static_cast<uint32_t>(reinterpret_cast<uintptr_t>(activeVS));
            summary->g=static_cast<uint32_t>(reinterpret_cast<uintptr_t>(activePS));
            summary->h=vsHash; summary->i=psHash;
            summary->j=static_cast<uint32_t>(reinterpret_cast<uintptr_t>(tex0));
            summary->extra[0]=static_cast<uint32_t>(reinterpret_cast<uintptr_t>(tex1));
            summary->extra[1]=static_cast<uint32_t>(reinterpret_cast<uintptr_t>(tex2));
            summary->extra[2]=expectedVS; summary->extra[3]=expectedPS;
            summary->extra[4]=expectedTex0; summary->extra[5]=expectedTex1; summary->extra[6]=expectedTex2;
            summary->extra[7]=static_cast<uint32_t>(reinterpret_cast<uintptr_t>(rt0));
            summary->extra[8]=static_cast<uint32_t>(reinterpret_cast<uintptr_t>(ds));
            summary->extra[9]=sceneSurface; summary->extra[10]=sourceSelectBits;
            summary->extra[11]=vsBytes; summary->extra[12]=psBytes;
            summary->extra[13]=static_cast<uint32_t>(vsHr); summary->extra[14]=static_cast<uint32_t>(psHr);
            summary->extra[15]=static_cast<uint32_t>(t0Hr); summary->extra[16]=static_cast<uint32_t>(t1Hr);
            summary->extra[17]=static_cast<uint32_t>(t2Hr); summary->extra[18]=static_cast<uint32_t>(psCHr);
            summary->extra[19]=static_cast<uint32_t>(vsCHr); summary->extra[20]=static_cast<uint32_t>(rtHr);
            summary->extra[21]=static_cast<uint32_t>(dsHr); summary->extra[22]=static_cast<uint32_t>(vpHr);
            summary->extra[23]=static_cast<uint32_t>(s_godRayG1ContractCaptureCount);
            summary->extra[24]=static_cast<uint32_t>(s_godRayG1ContractFailCount);
            summary->extra[25]=primitiveType; summary->extra[26]=startVertex; summary->extra[27]=primitiveCount;
        }

        Event* constants = emitG1Detail ? ReserveEvent(EventType::GodRayG1FinalConstants) : nullptr;
        if (constants)
        {
            constants->a=s_presentCount; constants->b=static_cast<uint32_t>(seen);
            constants->c=FloatBits(psC[0]); constants->d=FloatBits(psC[1]); constants->e=FloatBits(psC[2]); constants->f=FloatBits(psC[3]);
            for (uint32_t n=0;n<24u;++n) constants->extra[n]=FloatBits(psC[4u+n]);
            constants->g=FloatBits(vsC[0]); constants->h=FloatBits(vsC[1]); constants->i=FloatBits(vsC[2]); constants->j=FloatBits(vsC[3]);
            constants->extra[24]=FloatBits(vsC[4]); constants->extra[25]=FloatBits(vsC[5]);
            constants->extra[26]=FloatBits(vsC[6]); constants->extra[27]=FloatBits(vsC[7]);
        }

        Event* vertexConstants = emitG1Detail ? ReserveEvent(EventType::GodRayG1VertexConstants) : nullptr;
        if (vertexConstants)
        {
            vertexConstants->a=s_presentCount; vertexConstants->b=static_cast<uint32_t>(seen);
            for (uint32_t n=0;n<16u;++n) vertexConstants->extra[n]=FloatBits(vsC[n]);
            vertexConstants->c=static_cast<uint32_t>(vsCHr);
            vertexConstants->d=static_cast<uint32_t>(reinterpret_cast<uintptr_t>(activeVS));
            vertexConstants->e=vsHash;
        }

        Event* states = emitG1Detail ? ReserveEvent(EventType::GodRayG1FinalStates) : nullptr;
        if (states)
        {
            states->a=s_presentCount; states->b=static_cast<uint32_t>(seen);
            states->c=viewport.X; states->d=viewport.Y; states->e=viewport.Width; states->f=viewport.Height;
            states->g=FloatBits(viewport.MinZ); states->h=FloatBits(viewport.MaxZ);
            states->i=srcBlend; states->j=dstBlend;
            states->extra[0]=alphaBlend; states->extra[1]=colorWrite; states->extra[2]=zEnable; states->extra[3]=zWrite;
            states->extra[4]=samp0AddrU; states->extra[5]=samp0AddrV; states->extra[6]=samp0Min; states->extra[7]=samp0Mag;
            states->extra[8]=samp1AddrU; states->extra[9]=samp1AddrV; states->extra[10]=samp1Min; states->extra[11]=samp1Mag;
            states->extra[12]=samp2AddrU; states->extra[13]=samp2AddrV; states->extra[14]=samp2Min; states->extra[15]=samp2Mag;
            states->extra[16]=static_cast<uint32_t>(rs0Hr); states->extra[17]=static_cast<uint32_t>(rs1Hr);
            states->extra[18]=static_cast<uint32_t>(rs2Hr); states->extra[19]=static_cast<uint32_t>(rs3Hr);
            states->extra[20]=static_cast<uint32_t>(rs4Hr); states->extra[21]=static_cast<uint32_t>(rs5Hr);
            states->extra[22]=static_cast<uint32_t>(s00|s01|s02|s03); states->extra[23]=static_cast<uint32_t>(s10|s11|s12|s13);
            states->extra[24]=static_cast<uint32_t>(s20|s21|s22|s23); states->extra[25]=FloatBits(vsC[8]);
            states->extra[26]=FloatBits(vsC[9]); states->extra[27]=FloatBits(vsC[10]);
        }

        SafeReleaseCom(activeVS); SafeReleaseCom(activePS);
        SafeReleaseCom(tex0); SafeReleaseCom(tex1); SafeReleaseCom(tex2);
        SafeReleaseCom(rt0); SafeReleaseCom(ds);
        return true;
    }


    bool RunGodRayG2BindStateProbe(void* device, DWORD primitiveType, UINT startVertex, UINT primitiveCount, uintptr_t returnAddress, bool* outG3Pass)
    {
        if (outG3Pass) *outG3Pass = false;
        if (s_v10Mode != 5 || s_postProcessRoute != 3u ||
            returnAddress != kGodRayFinalDrawPrimitiveReturn ||
            primitiveType != kD3DPrimitiveTriangleStrip || startVertex != 0u || primitiveCount != 2u)
            return false;

        const bool selectorReadable = IsReadableMemory(
            reinterpret_cast<void*>(kPcDormantGodRaySelector), sizeof(uint32_t));
        const uint32_t selectorRaw = selectorReadable ? ReadU32(kPcDormantGodRaySelector) : 0u;
        if (!selectorReadable || selectorRaw == 0u)
            return false;

        const LONG attempt = InterlockedIncrement(&s_godRayG2BindAttemptCount);

        void** dvt = device && IsReadableMemory(device, sizeof(void*))
            ? *reinterpret_cast<void***>(device) : nullptr;
        const uint32_t requiredMax = kD3DGetPixelShaderConstantFVtableIndex;
        if (!dvt || !IsReadableMemory(dvt, (requiredMax + 1u) * sizeof(void*)) ||
            !IsExecutableMemory(dvt[kD3DSetRenderStateVtableIndex]) ||
            !IsExecutableMemory(dvt[kD3DGetRenderStateVtableIndex]) ||
            !IsExecutableMemory(dvt[kD3DSetTextureVtableIndex]) ||
            !IsExecutableMemory(dvt[kD3DGetTextureVtableIndex]) ||
            !IsExecutableMemory(dvt[kD3DSetSamplerStateVtableIndex]) ||
            !IsExecutableMemory(dvt[kD3DGetSamplerStateVtableIndex]) ||
            !IsExecutableMemory(dvt[kD3DSetVertexShaderVtableIndex]) ||
            !IsExecutableMemory(dvt[kD3DGetVertexShaderVtableIndex]) ||
            !IsExecutableMemory(dvt[kD3DSetVertexShaderConstantFVtableIndex]) ||
            !IsExecutableMemory(dvt[kD3DGetVertexShaderConstantFVtableIndex]) ||
            !IsExecutableMemory(dvt[kD3DSetPixelShaderVtableIndex]) ||
            !IsExecutableMemory(dvt[kD3DGetPixelShaderVtableIndex]) ||
            !IsExecutableMemory(dvt[kD3DSetPixelShaderConstantFVtableIndex]) ||
            !IsExecutableMemory(dvt[kD3DGetPixelShaderConstantFVtableIndex]) ||
            !IsExecutableMemory(dvt[kD3DGetRenderTargetVtableIndex]) ||
            !IsExecutableMemory(dvt[kD3DGetDepthStencilSurfaceVtableIndex]) ||
            !IsExecutableMemory(dvt[kD3DGetViewportVtableIndex]))
        {
            InterlockedIncrement(&s_godRayG2BindFailCount);
            return true;
        }

        const auto setRender = reinterpret_cast<D3DSetRenderState_t>(dvt[kD3DSetRenderStateVtableIndex]);
        const auto getRender = reinterpret_cast<D3DGetRenderState_t>(dvt[kD3DGetRenderStateVtableIndex]);
        const auto setTexture = reinterpret_cast<D3DSetTexture_t>(dvt[kD3DSetTextureVtableIndex]);
        const auto getTexture = reinterpret_cast<D3DGetTexture_t>(dvt[kD3DGetTextureVtableIndex]);
        const auto setSampler = reinterpret_cast<D3DSetSamplerState_t>(dvt[kD3DSetSamplerStateVtableIndex]);
        const auto getSampler = reinterpret_cast<D3DGetSamplerState_t>(dvt[kD3DGetSamplerStateVtableIndex]);
        const auto setVS = reinterpret_cast<D3DSetVertexShader_t>(dvt[kD3DSetVertexShaderVtableIndex]);
        const auto getVS = reinterpret_cast<D3DGetVertexShader_t>(dvt[kD3DGetVertexShaderVtableIndex]);
        const auto setVSC = reinterpret_cast<D3DSetVertexShaderConstantF_t>(dvt[kD3DSetVertexShaderConstantFVtableIndex]);
        const auto getVSC = reinterpret_cast<D3DGetVertexShaderConstantF_t>(dvt[kD3DGetVertexShaderConstantFVtableIndex]);
        const auto setPS = reinterpret_cast<D3DSetPixelShader_t>(dvt[kD3DSetPixelShaderVtableIndex]);
        const auto getPS = reinterpret_cast<D3DGetPixelShader_t>(dvt[kD3DGetPixelShaderVtableIndex]);
        const auto setPSC = reinterpret_cast<D3DSetPixelShaderConstantF_t>(dvt[kD3DSetPixelShaderConstantFVtableIndex]);
        const auto getPSC = reinterpret_cast<D3DGetPixelShaderConstantF_t>(dvt[kD3DGetPixelShaderConstantFVtableIndex]);
        const auto getRT = reinterpret_cast<D3DGetRenderTarget_t>(dvt[kD3DGetRenderTargetVtableIndex]);
        const auto getDS = reinterpret_cast<D3DGetDepthStencilSurface_t>(dvt[kD3DGetDepthStencilSurfaceVtableIndex]);
        const auto getViewport = reinterpret_cast<D3DGetViewport_t>(dvt[kD3DGetViewportVtableIndex]);

        const uint32_t expectedVS = ReadU32(kCompositeVS);
        const uint32_t expectedPS = ReadU32(kGodRayFinalPixelShader);
        const uint32_t sourceSelectBits = ReadU32(0x00EE5E8Cu);
        const uint32_t quarterWrapper = sourceSelectBits == 0u ? ReadU32(kRtQuarterA) : ReadU32(kRtQuarterB);
        uint32_t expectedTex0 = sourceSelectBits == 0u
            ? static_cast<uint32_t>(s_rawTextureQuarterA)
            : static_cast<uint32_t>(s_rawTextureQuarterB);
        if (expectedTex0 < 0x10000u && quarterWrapper >= 0x10000u)
        {
            const uint32_t quarterSurface = RawSurfaceDirectFromWrapper(quarterWrapper);
            HRESULT texHr = E_PENDING;
            expectedTex0 = RawTextureFromSurface(quarterSurface, &texHr);
        }
        const uint32_t expectedTex1 = RawTextureFromNglDescriptor(kPostFxDepthDescriptor);
        const uint32_t optionalWrapper = ReadU32(kGodRayOptionalTextureWrapper);
        uint32_t expectedTex2 = 0u;
        if (optionalWrapper >= 0x10000u)
        {
            const uint32_t optionalSurface = RawSurfaceDirectFromWrapper(optionalWrapper);
            HRESULT texHr = E_PENDING;
            expectedTex2 = RawTextureFromSurface(optionalSurface, &texHr);
            if (expectedTex2 < 0x10000u)
                expectedTex2 = RawTextureFromNglDescriptor(optionalWrapper + 0x14u);
        }
        const uint32_t sceneSurface = RawSurfaceDirectFromWrapper(CurrentSceneRT());

        // G1 proved these final-pass states repeatedly. G2 binds the exact captured
        // contract rather than merely re-setting whatever happens to be live.
        constexpr DWORD kExpectedSrcBlend = 2u;
        constexpr DWORD kExpectedDstBlend = 2u;
        constexpr DWORD kExpectedAlphaBlend = 1u;
        constexpr DWORD kExpectedColorWrite = 0xFu;
        constexpr DWORD kExpectedZEnable = 0u;
        constexpr DWORD kExpectedZWrite = 0u;
        constexpr DWORD kSamplerTypes[4] = { kSampAddressU, kSampAddressV, kSampMinFilter, kSampMagFilter };
        constexpr DWORD kExpectedSampler[3][4] = {
            { 3u, 3u, 2u, 2u },
            { 3u, 3u, 1u, 1u },
            { 1u, 1u, 1u, 1u }
        };
        constexpr DWORD kRenderTypes[6] = {
            kRSSrcBlend, kRSDestBlend, kRSAlphaBlendEnable,
            kRSColorWriteEnable, kRSZEnable, kRSZWriteEnable
        };
        constexpr DWORD kExpectedRender[6] = {
            kExpectedSrcBlend, kExpectedDstBlend, kExpectedAlphaBlend,
            kExpectedColorWrite, kExpectedZEnable, kExpectedZWrite
        };

        void* beforeVS=nullptr; void* beforePS=nullptr;
        void* beforeTex[3]={}; void* beforeRT=nullptr; void* beforeDS=nullptr;
        float beforePSC[28]={}; float beforeVSC[16]={};
        DWORD beforeRender[6]={}; DWORD beforeSampler[3][4]={};
        D3DViewportLite beforeVP={};

        HRESULT beforeHrOr=S_OK;
        auto MergeHr = [](HRESULT& accumulator, HRESULT hr) {
            accumulator = static_cast<HRESULT>(static_cast<uint32_t>(accumulator) | static_cast<uint32_t>(hr));
        };
        const HRESULT beforeVsHr=getVS(device,&beforeVS); MergeHr(beforeHrOr,beforeVsHr);
        const HRESULT beforePsHr=getPS(device,&beforePS); MergeHr(beforeHrOr,beforePsHr);
        for (DWORD stage=0; stage<3u; ++stage) MergeHr(beforeHrOr,getTexture(device,stage,&beforeTex[stage]));
        const HRESULT beforePsCHr=getPSC(device,0u,beforePSC,7u); MergeHr(beforeHrOr,beforePsCHr);
        const HRESULT beforeVsCHr=getVSC(device,0u,beforeVSC,4u); MergeHr(beforeHrOr,beforeVsCHr);
        for (uint32_t n=0;n<6u;++n) MergeHr(beforeHrOr,getRender(device,kRenderTypes[n],&beforeRender[n]));
        for (DWORD stage=0;stage<3u;++stage)
            for (uint32_t n=0;n<4u;++n)
                MergeHr(beforeHrOr,getSampler(device,stage,kSamplerTypes[n],&beforeSampler[stage][n]));
        const HRESULT beforeRtHr=getRT(device,0u,&beforeRT); MergeHr(beforeHrOr,beforeRtHr);
        const HRESULT beforeDsHr=getDS(device,&beforeDS);
        const HRESULT beforeVpHr=getViewport(device,&beforeVP); MergeHr(beforeHrOr,beforeVpHr);

        uint32_t beforeVsBytes=0u,beforeVsHash=0u,beforePsBytes=0u,beforePsHash=0u;
        HRESULT beforeVsQuery=E_FAIL,beforeVsFetch=E_PENDING,beforePsQuery=E_FAIL,beforePsFetch=E_PENDING;
        const bool beforeVsId=SUCCEEDED(beforeVsHr)&&beforeVS&&
            QueryPixelShaderIdentity(beforeVS,beforeVsBytes,beforeVsHash,beforeVsQuery,beforeVsFetch);
        const bool beforePsId=SUCCEEDED(beforePsHr)&&beforePS&&
            QueryPixelShaderIdentity(beforePS,beforePsBytes,beforePsHash,beforePsQuery,beforePsFetch);

        const bool noDepthBefore = beforeDsHr == kD3DErrNotFound && beforeDS == nullptr &&
            beforeRender[4] == 0u && beforeRender[5] == 0u;
        const bool beforeDepthKnown = SUCCEEDED(beforeDsHr) || noDepthBefore;
        const bool beforeTexturesMatch =
            static_cast<uint32_t>(reinterpret_cast<uintptr_t>(beforeTex[0])) == expectedTex0 &&
            static_cast<uint32_t>(reinterpret_cast<uintptr_t>(beforeTex[1])) == expectedTex1 &&
            static_cast<uint32_t>(reinterpret_cast<uintptr_t>(beforeTex[2])) == expectedTex2;
        bool beforeRenderMatch=true, beforeSamplerMatch=true;
        for (uint32_t n=0;n<6u;++n) beforeRenderMatch = beforeRenderMatch && beforeRender[n]==kExpectedRender[n];
        for (uint32_t stage=0;stage<3u;++stage)
            for (uint32_t n=0;n<4u;++n)
                beforeSamplerMatch = beforeSamplerMatch && beforeSampler[stage][n]==kExpectedSampler[stage][n];
        const bool beforeContract = SUCCEEDED(beforeHrOr) && beforeDepthKnown &&
            expectedVS>=0x10000u && expectedPS>=0x10000u && expectedTex0>=0x10000u &&
            sceneSurface>=0x10000u && beforeVS && beforePS && beforeRT &&
            static_cast<uint32_t>(reinterpret_cast<uintptr_t>(beforeVS))==expectedVS &&
            static_cast<uint32_t>(reinterpret_cast<uintptr_t>(beforePS))==expectedPS &&
            beforeVsId && beforePsId && beforeVsHash==kGodRayG2ExpectedVsHash &&
            beforePsHash==kGodRayG2ExpectedPsHash && beforeVsBytes==kGodRayG2ExpectedVsBytes &&
            beforePsBytes==kGodRayG2ExpectedPsBytes && beforeTexturesMatch &&
            static_cast<uint32_t>(reinterpret_cast<uintptr_t>(beforeRT))==sceneSurface &&
            beforeRenderMatch && beforeSamplerMatch;

        HRESULT setHrOr=S_OK;
        if (beforeContract)
        {
            MergeHr(setHrOr,setVS(device,reinterpret_cast<void*>(expectedVS)));
            MergeHr(setHrOr,setPS(device,reinterpret_cast<void*>(expectedPS)));
            MergeHr(setHrOr,setTexture(device,0u,reinterpret_cast<void*>(expectedTex0)));
            MergeHr(setHrOr,setTexture(device,1u,reinterpret_cast<void*>(expectedTex1)));
            MergeHr(setHrOr,setTexture(device,2u,reinterpret_cast<void*>(expectedTex2)));
            MergeHr(setHrOr,setPSC(device,0u,beforePSC,7u));
            MergeHr(setHrOr,setVSC(device,0u,beforeVSC,4u));
            for (uint32_t n=0;n<6u;++n) MergeHr(setHrOr,setRender(device,kRenderTypes[n],kExpectedRender[n]));
            for (DWORD stage=0;stage<3u;++stage)
                for (uint32_t n=0;n<4u;++n)
                    MergeHr(setHrOr,setSampler(device,stage,kSamplerTypes[n],kExpectedSampler[stage][n]));
        }
        else
        {
            setHrOr=E_FAIL;
        }

        void* boundVS=nullptr; void* boundPS=nullptr; void* boundTex[3]={};
        void* boundRT=nullptr; void* boundDS=nullptr;
        float boundPSC[28]={}; float boundVSC[16]={};
        DWORD boundRender[6]={}; DWORD boundSampler[3][4]={};
        D3DViewportLite boundVP={};
        HRESULT boundHrOr=S_OK;
        const HRESULT boundVsHr=getVS(device,&boundVS); MergeHr(boundHrOr,boundVsHr);
        const HRESULT boundPsHr=getPS(device,&boundPS); MergeHr(boundHrOr,boundPsHr);
        for (DWORD stage=0;stage<3u;++stage) MergeHr(boundHrOr,getTexture(device,stage,&boundTex[stage]));
        MergeHr(boundHrOr,getPSC(device,0u,boundPSC,7u));
        MergeHr(boundHrOr,getVSC(device,0u,boundVSC,4u));
        for (uint32_t n=0;n<6u;++n) MergeHr(boundHrOr,getRender(device,kRenderTypes[n],&boundRender[n]));
        for (DWORD stage=0;stage<3u;++stage)
            for (uint32_t n=0;n<4u;++n)
                MergeHr(boundHrOr,getSampler(device,stage,kSamplerTypes[n],&boundSampler[stage][n]));
        const HRESULT boundRtHr=getRT(device,0u,&boundRT); MergeHr(boundHrOr,boundRtHr);
        const HRESULT boundDsHr=getDS(device,&boundDS);
        const HRESULT boundVpHr=getViewport(device,&boundVP); MergeHr(boundHrOr,boundVpHr);

        uint32_t boundVsBytes=0u,boundVsHash=0u,boundPsBytes=0u,boundPsHash=0u;
        HRESULT boundVsQuery=E_FAIL,boundVsFetch=E_PENDING,boundPsQuery=E_FAIL,boundPsFetch=E_PENDING;
        const bool boundVsId=SUCCEEDED(boundVsHr)&&boundVS&&
            QueryPixelShaderIdentity(boundVS,boundVsBytes,boundVsHash,boundVsQuery,boundVsFetch);
        const bool boundPsId=SUCCEEDED(boundPsHr)&&boundPS&&
            QueryPixelShaderIdentity(boundPS,boundPsBytes,boundPsHash,boundPsQuery,boundPsFetch);

        const bool shaderVerified = beforeContract && SUCCEEDED(setHrOr) && SUCCEEDED(boundHrOr) &&
            boundVS==reinterpret_cast<void*>(expectedVS) && boundPS==reinterpret_cast<void*>(expectedPS) &&
            boundVsId && boundPsId && boundVsHash==kGodRayG2ExpectedVsHash && boundPsHash==kGodRayG2ExpectedPsHash &&
            boundVsBytes==kGodRayG2ExpectedVsBytes && boundPsBytes==kGodRayG2ExpectedPsBytes;
        const bool textureVerified =
            static_cast<uint32_t>(reinterpret_cast<uintptr_t>(boundTex[0]))==expectedTex0 &&
            static_cast<uint32_t>(reinterpret_cast<uintptr_t>(boundTex[1]))==expectedTex1 &&
            static_cast<uint32_t>(reinterpret_cast<uintptr_t>(boundTex[2]))==expectedTex2;
        const bool constantsVerified =
            memcmp(beforePSC,boundPSC,sizeof(beforePSC))==0 && memcmp(beforeVSC,boundVSC,sizeof(beforeVSC))==0;
        bool renderVerified=true,samplerVerified=true;
        for (uint32_t n=0;n<6u;++n) renderVerified=renderVerified&&boundRender[n]==kExpectedRender[n];
        for (uint32_t stage=0;stage<3u;++stage)
            for (uint32_t n=0;n<4u;++n)
                samplerVerified=samplerVerified&&boundSampler[stage][n]==kExpectedSampler[stage][n];
        const bool boundNoDepth = boundDsHr==kD3DErrNotFound && boundDS==nullptr && boundRender[4]==0u && boundRender[5]==0u;
        const bool dsStable = (SUCCEEDED(beforeDsHr)&&SUCCEEDED(boundDsHr)&&boundDS==beforeDS) ||
            (noDepthBefore&&boundNoDepth);
        const bool vpStable = SUCCEEDED(beforeVpHr)&&SUCCEEDED(boundVpHr)&&
            beforeVP.X==boundVP.X&&beforeVP.Y==boundVP.Y&&beforeVP.Width==boundVP.Width&&beforeVP.Height==boundVP.Height&&
            FloatBits(beforeVP.MinZ)==FloatBits(boundVP.MinZ)&&FloatBits(beforeVP.MaxZ)==FloatBits(boundVP.MaxZ);
        const bool rtStable = SUCCEEDED(beforeRtHr)&&SUCCEEDED(boundRtHr)&&boundRT==beforeRT&&
            static_cast<uint32_t>(reinterpret_cast<uintptr_t>(boundRT))==sceneSurface;
        const bool stateVerified = renderVerified&&samplerVerified;
        const bool untouchedTargets = rtStable&&dsStable&&vpStable;

        // V10.5.70 lockdown freezes the proven G4 ownership path; no render-contract change. The original
        // DrawPrimitive trampoline is called only after every G2 bind/constant/state/
        // target verification passed. No RT/DS/viewport setter exists in this path.
        // The outer hook suppresses the duplicate stock draw ONLY if this G3 result
        // later proves draw-held integrity + exact restoration.
        const LONG g3Attempt = InterlockedIncrement(&s_godRayG3AttemptCount);
        const bool g3WasDisabled = InterlockedCompareExchange(&s_godRayG3Disabled, 0, 0) != 0;
        const bool g3PreDrawVerified = !g3WasDisabled && s_rawDrawPrimitive &&
            beforeContract && shaderVerified && textureVerified && constantsVerified &&
            stateVerified && untouchedTargets;
        HRESULT g3DrawHr = E_PENDING;
        bool g3Drew = false;
        if (g3PreDrawVerified)
        {
            g3DrawHr = s_rawDrawPrimitive(device, primitiveType, startVertex, primitiveCount);
            InterlockedIncrement(&s_godRayG3DrawCount);
            g3Drew = true;
        }

        // Verify that the actual xeSM3 draw held the exact proven GodRay contract
        // and did not disturb RT/DS/viewport before restoration.
        void* heldVS=nullptr; void* heldPS=nullptr; void* heldTex[3]={};
        void* heldRT=nullptr; void* heldDS=nullptr;
        float heldPSC[28]={}; float heldVSC[16]={};
        DWORD heldRender[6]={}; DWORD heldSampler[3][4]={};
        D3DViewportLite heldVP={};
        HRESULT heldHrOr=S_OK;
        HRESULT heldDsHr=E_PENDING;
        if (g3Drew)
        {
            MergeHr(heldHrOr,getVS(device,&heldVS));
            MergeHr(heldHrOr,getPS(device,&heldPS));
            for (DWORD stage=0;stage<3u;++stage) MergeHr(heldHrOr,getTexture(device,stage,&heldTex[stage]));
            MergeHr(heldHrOr,getPSC(device,0u,heldPSC,7u));
            MergeHr(heldHrOr,getVSC(device,0u,heldVSC,4u));
            for (uint32_t n=0;n<6u;++n) MergeHr(heldHrOr,getRender(device,kRenderTypes[n],&heldRender[n]));
            for (DWORD stage=0;stage<3u;++stage)
                for (uint32_t n=0;n<4u;++n)
                    MergeHr(heldHrOr,getSampler(device,stage,kSamplerTypes[n],&heldSampler[stage][n]));
            MergeHr(heldHrOr,getRT(device,0u,&heldRT));
            heldDsHr=getDS(device,&heldDS);
            MergeHr(heldHrOr,getViewport(device,&heldVP));
        }

        const bool drawHeldShader = g3Drew && SUCCEEDED(heldHrOr) &&
            heldVS==reinterpret_cast<void*>(expectedVS) && heldPS==reinterpret_cast<void*>(expectedPS);
        const bool drawHeldTextures = g3Drew &&
            static_cast<uint32_t>(reinterpret_cast<uintptr_t>(heldTex[0]))==expectedTex0 &&
            static_cast<uint32_t>(reinterpret_cast<uintptr_t>(heldTex[1]))==expectedTex1 &&
            static_cast<uint32_t>(reinterpret_cast<uintptr_t>(heldTex[2]))==expectedTex2;
        const bool drawHeldConstants = g3Drew &&
            memcmp(beforePSC,heldPSC,sizeof(beforePSC))==0 && memcmp(beforeVSC,heldVSC,sizeof(beforeVSC))==0;
        bool drawHeldRender=true,drawHeldSampler=true;
        if (g3Drew)
        {
            for (uint32_t n=0;n<6u;++n) drawHeldRender=drawHeldRender&&heldRender[n]==kExpectedRender[n];
            for (uint32_t stage=0;stage<3u;++stage)
                for (uint32_t n=0;n<4u;++n)
                    drawHeldSampler=drawHeldSampler&&heldSampler[stage][n]==kExpectedSampler[stage][n];
        }
        else
        {
            drawHeldRender=false; drawHeldSampler=false;
        }
        const bool drawHeldState = drawHeldRender&&drawHeldSampler;
        const bool heldNoDepth = g3Drew && heldDsHr==kD3DErrNotFound && heldDS==nullptr &&
            heldRender[4]==0u && heldRender[5]==0u;
        const bool heldDsStable = g3Drew && ((SUCCEEDED(beforeDsHr)&&SUCCEEDED(heldDsHr)&&heldDS==beforeDS) ||
            (noDepthBefore&&heldNoDepth));
        const bool heldVpStable = g3Drew &&
            beforeVP.X==heldVP.X&&beforeVP.Y==heldVP.Y&&beforeVP.Width==heldVP.Width&&beforeVP.Height==heldVP.Height&&
            FloatBits(beforeVP.MinZ)==FloatBits(heldVP.MinZ)&&FloatBits(beforeVP.MaxZ)==FloatBits(heldVP.MaxZ);
        const bool heldRtStable = g3Drew && heldRT==beforeRT &&
            static_cast<uint32_t>(reinterpret_cast<uintptr_t>(heldRT))==sceneSurface;
        const bool drawHeldRtDsVp = heldRtStable&&heldDsStable&&heldVpStable;

        HRESULT restoreHrOr=S_OK;
        MergeHr(restoreHrOr,setVS(device,beforeVS));
        MergeHr(restoreHrOr,setPS(device,beforePS));
        for (DWORD stage=0;stage<3u;++stage) MergeHr(restoreHrOr,setTexture(device,stage,beforeTex[stage]));
        MergeHr(restoreHrOr,setPSC(device,0u,beforePSC,7u));
        MergeHr(restoreHrOr,setVSC(device,0u,beforeVSC,4u));
        for (uint32_t n=0;n<6u;++n) MergeHr(restoreHrOr,setRender(device,kRenderTypes[n],beforeRender[n]));
        for (DWORD stage=0;stage<3u;++stage)
            for (uint32_t n=0;n<4u;++n)
                MergeHr(restoreHrOr,setSampler(device,stage,kSamplerTypes[n],beforeSampler[stage][n]));

        void* restoredVS=nullptr; void* restoredPS=nullptr; void* restoredTex[3]={};
        void* restoredRT=nullptr; void* restoredDS=nullptr;
        float restoredPSC[28]={}; float restoredVSC[16]={};
        DWORD restoredRender[6]={}; DWORD restoredSampler[3][4]={};
        D3DViewportLite restoredVP={};
        HRESULT restoreGetHrOr=S_OK;
        MergeHr(restoreGetHrOr,getVS(device,&restoredVS));
        MergeHr(restoreGetHrOr,getPS(device,&restoredPS));
        for (DWORD stage=0;stage<3u;++stage) MergeHr(restoreGetHrOr,getTexture(device,stage,&restoredTex[stage]));
        MergeHr(restoreGetHrOr,getPSC(device,0u,restoredPSC,7u));
        MergeHr(restoreGetHrOr,getVSC(device,0u,restoredVSC,4u));
        for (uint32_t n=0;n<6u;++n) MergeHr(restoreGetHrOr,getRender(device,kRenderTypes[n],&restoredRender[n]));
        for (DWORD stage=0;stage<3u;++stage)
            for (uint32_t n=0;n<4u;++n)
                MergeHr(restoreGetHrOr,getSampler(device,stage,kSamplerTypes[n],&restoredSampler[stage][n]));
        const HRESULT restoredRtHr=getRT(device,0u,&restoredRT); MergeHr(restoreGetHrOr,restoredRtHr);
        const HRESULT restoredDsHr=getDS(device,&restoredDS);
        const HRESULT restoredVpHr=getViewport(device,&restoredVP); MergeHr(restoreGetHrOr,restoredVpHr);

        bool restoreRenderMatch=true,restoreSamplerMatch=true;
        for (uint32_t n=0;n<6u;++n) restoreRenderMatch=restoreRenderMatch&&restoredRender[n]==beforeRender[n];
        for (uint32_t stage=0;stage<3u;++stage)
            for (uint32_t n=0;n<4u;++n)
                restoreSamplerMatch=restoreSamplerMatch&&restoredSampler[stage][n]==beforeSampler[stage][n];
        const bool restoredNoDepth=restoredDsHr==kD3DErrNotFound&&restoredDS==nullptr&&restoredRender[4]==0u&&restoredRender[5]==0u;
        const bool restoreDsStable=(SUCCEEDED(beforeDsHr)&&SUCCEEDED(restoredDsHr)&&restoredDS==beforeDS)||(noDepthBefore&&restoredNoDepth);
        const bool restoreVpStable=SUCCEEDED(restoredVpHr)&&
            beforeVP.X==restoredVP.X&&beforeVP.Y==restoredVP.Y&&beforeVP.Width==restoredVP.Width&&beforeVP.Height==restoredVP.Height&&
            FloatBits(beforeVP.MinZ)==FloatBits(restoredVP.MinZ)&&FloatBits(beforeVP.MaxZ)==FloatBits(restoredVP.MaxZ);
        const bool restoreVerified=SUCCEEDED(restoreHrOr)&&SUCCEEDED(restoreGetHrOr)&&
            restoredVS==beforeVS&&restoredPS==beforePS&&
            restoredTex[0]==beforeTex[0]&&restoredTex[1]==beforeTex[1]&&restoredTex[2]==beforeTex[2]&&
            memcmp(restoredPSC,beforePSC,sizeof(beforePSC))==0&&memcmp(restoredVSC,beforeVSC,sizeof(beforeVSC))==0&&
            restoreRenderMatch&&restoreSamplerMatch&&restoredRT==beforeRT&&restoreDsStable&&restoreVpStable;

        const bool pass=beforeContract&&shaderVerified&&textureVerified&&constantsVerified&&stateVerified&&
            untouchedTargets&&restoreVerified&&s_godRayG2DrawCount==0;
        if (pass) InterlockedIncrement(&s_godRayG2BindPassCount);
        else InterlockedIncrement(&s_godRayG2BindFailCount);

        const bool g3Pass = g3PreDrawVerified && g3Drew && SUCCEEDED(g3DrawHr) &&
            drawHeldShader && drawHeldTextures && drawHeldConstants && drawHeldState &&
            drawHeldRtDsVp && restoreVerified;
        if (outG3Pass) *outG3Pass = g3Pass;
        if (!g3WasDisabled)
        {
            if (g3Pass)
                InterlockedIncrement(&s_godRayG3PassCount);
            else
            {
                InterlockedIncrement(&s_godRayG3FailCount);
                // After the first real draw, any draw-held or restore failure is a
                // hard fail-closed condition for the remainder of this process.
                InterlockedExchange(&s_godRayG3Disabled, 1);
            }
        }

        if (ShouldLogDebugXboxIntegrationDetail(g3Attempt) || (!g3Pass && !g3WasDisabled))
        {
            Event* g3Event=ReserveEvent(EventType::GodRayG3FirstRealDraw);
            if (g3Event)
            {
                g3Event->a=s_presentCount; g3Event->b=static_cast<uint32_t>(g3Attempt); g3Event->c=g3Pass?1u:0u; g3Event->d=selectorRaw;
                g3Event->e=static_cast<uint32_t>(g3DrawHr); g3Event->f=g3PreDrawVerified?1u:0u;
                g3Event->g=drawHeldShader?1u:0u; g3Event->h=drawHeldTextures?1u:0u;
                g3Event->i=drawHeldConstants?1u:0u; g3Event->j=drawHeldState?1u:0u;
                g3Event->extra[0]=drawHeldRtDsVp?1u:0u; g3Event->extra[1]=restoreVerified?1u:0u;
                g3Event->extra[2]=static_cast<uint32_t>(s_godRayG3DrawCount);
                g3Event->extra[3]=static_cast<uint32_t>(s_godRayG3PassCount);
                g3Event->extra[4]=static_cast<uint32_t>(s_godRayG3FailCount);
                g3Event->extra[5]=static_cast<uint32_t>(s_godRayG3Disabled);
                g3Event->extra[6]=boundVsHash; g3Event->extra[7]=boundPsHash;
                g3Event->extra[8]=expectedTex0; g3Event->extra[9]=expectedTex1; g3Event->extra[10]=expectedTex2;
                g3Event->extra[11]=sceneSurface; g3Event->extra[12]=static_cast<uint32_t>(reinterpret_cast<uintptr_t>(heldRT));
                g3Event->extra[13]=static_cast<uint32_t>(reinterpret_cast<uintptr_t>(heldDS));
                g3Event->extra[14]=heldVP.Width; g3Event->extra[15]=heldVP.Height;
                g3Event->extra[16]=static_cast<uint32_t>(heldHrOr); g3Event->extra[17]=static_cast<uint32_t>(heldDsHr);
                g3Event->extra[18]=static_cast<uint32_t>(restoreHrOr); g3Event->extra[19]=static_cast<uint32_t>(restoreGetHrOr);
                g3Event->extra[20]=beforeContract?1u:0u; g3Event->extra[21]=shaderVerified?1u:0u;
                g3Event->extra[22]=textureVerified?1u:0u; g3Event->extra[23]=constantsVerified?1u:0u;
                g3Event->extra[24]=stateVerified?1u:0u; g3Event->extra[25]=untouchedTargets?1u:0u;
                g3Event->extra[26]=primitiveType; g3Event->extra[27]=primitiveCount;
            }
        }

        if (ShouldLogDebugXboxIntegrationDetail(attempt) || !pass)
        {
            Event* event=ReserveEvent(EventType::GodRayG2BindStateProbe);
            if (event)
            {
                event->a=s_presentCount; event->b=static_cast<uint32_t>(attempt); event->c=pass?1u:0u; event->d=selectorRaw;
                event->e=expectedVS; event->f=expectedPS; event->g=expectedTex0; event->h=expectedTex1; event->i=expectedTex2; event->j=sceneSurface;
                event->extra[0]=static_cast<uint32_t>(reinterpret_cast<uintptr_t>(beforeVS));
                event->extra[1]=static_cast<uint32_t>(reinterpret_cast<uintptr_t>(beforePS));
                event->extra[2]=static_cast<uint32_t>(reinterpret_cast<uintptr_t>(boundVS));
                event->extra[3]=static_cast<uint32_t>(reinterpret_cast<uintptr_t>(boundPS));
                event->extra[4]=static_cast<uint32_t>(reinterpret_cast<uintptr_t>(restoredVS));
                event->extra[5]=static_cast<uint32_t>(reinterpret_cast<uintptr_t>(restoredPS));
                event->extra[6]=static_cast<uint32_t>(reinterpret_cast<uintptr_t>(beforeTex[0]));
                event->extra[7]=static_cast<uint32_t>(reinterpret_cast<uintptr_t>(beforeTex[1]));
                event->extra[8]=static_cast<uint32_t>(reinterpret_cast<uintptr_t>(beforeTex[2]));
                event->extra[9]=static_cast<uint32_t>(reinterpret_cast<uintptr_t>(boundTex[0]));
                event->extra[10]=static_cast<uint32_t>(reinterpret_cast<uintptr_t>(boundTex[1]));
                event->extra[11]=static_cast<uint32_t>(reinterpret_cast<uintptr_t>(boundTex[2]));
                event->extra[12]=static_cast<uint32_t>(reinterpret_cast<uintptr_t>(restoredTex[0]));
                event->extra[13]=static_cast<uint32_t>(reinterpret_cast<uintptr_t>(restoredTex[1]));
                event->extra[14]=static_cast<uint32_t>(reinterpret_cast<uintptr_t>(restoredTex[2]));
                event->extra[15]=boundVsHash; event->extra[16]=boundPsHash;
                event->extra[17]=static_cast<uint32_t>(setHrOr); event->extra[18]=static_cast<uint32_t>(boundHrOr);
                event->extra[19]=static_cast<uint32_t>(restoreHrOr); event->extra[20]=static_cast<uint32_t>(restoreGetHrOr);
                event->extra[21]=shaderVerified?1u:0u; event->extra[22]=textureVerified?1u:0u;
                event->extra[23]=constantsVerified?1u:0u; event->extra[24]=stateVerified?1u:0u;
                event->extra[25]=untouchedTargets?1u:0u; event->extra[26]=restoreVerified?1u:0u;
                event->extra[27]=static_cast<uint32_t>(s_godRayG2DrawCount);
            }
        }

        SafeReleaseCom(beforeVS); SafeReleaseCom(beforePS);
        for (uint32_t n=0;n<3u;++n) SafeReleaseCom(beforeTex[n]);
        SafeReleaseCom(beforeRT); SafeReleaseCom(beforeDS);
        SafeReleaseCom(boundVS); SafeReleaseCom(boundPS);
        for (uint32_t n=0;n<3u;++n) SafeReleaseCom(boundTex[n]);
        SafeReleaseCom(boundRT); SafeReleaseCom(boundDS);
        SafeReleaseCom(restoredVS); SafeReleaseCom(restoredPS);
        for (uint32_t n=0;n<3u;++n) SafeReleaseCom(restoredTex[n]);
        SafeReleaseCom(restoredRT); SafeReleaseCom(restoredDS);
        SafeReleaseCom(heldVS); SafeReleaseCom(heldPS);
        for (uint32_t n=0;n<3u;++n) SafeReleaseCom(heldTex[n]);
        SafeReleaseCom(heldRT); SafeReleaseCom(heldDS);
        return true;
    }

    HRESULT WINAPI RawDrawPrimitive_Hook(void* device, DWORD primitiveType, UINT startVertex, UINT primitiveCount)
    {
        const uintptr_t returnAddress = reinterpret_cast<uintptr_t>(_ReturnAddress());

        // V10.5.72 device-lost fail-closed gate. The detour itself stays installed
        // across Reset, but xeSM3 performs no custom PostFX work until the game's
        // original Reset reports success. Stock DrawPrimitive remains the fallback.
        if (s_v10Mode == 5 && InterlockedCompareExchange(&s_postFxDeviceReady, 0, 0) == 0)
            return s_rawDrawPrimitive(device, primitiveType, startVertex, primitiveCount);

        // G1 observes the stock dormant GodRay final draw. G2 reproduces/verifies
        // the validated bind/state contract. G3 issues one real xeSM3 DrawPrimitive
        // through the original trampoline while that verified contract is held and
        // restores the pre-probe state. G4 suppresses the duplicate stock draw ONLY
        // when that exact G3 invocation returned a complete pass.
        CaptureGodRayG1FinalContract(device, primitiveType, startVertex, primitiveCount, returnAddress);
        bool g3PassThisCall = false;
        const bool godRayProbeHandled = RunGodRayG2BindStateProbe(
            device, primitiveType, startVertex, primitiveCount, returnAddress, &g3PassThisCall);

        const bool selectorReadable = IsReadableMemory(
            reinterpret_cast<void*>(kPcDormantGodRaySelector), sizeof(uint32_t));
        const uint32_t selectorRaw = selectorReadable ? ReadU32(kPcDormantGodRaySelector) : 0u;
        const bool exactGodRayStockFinal = s_v10Mode == 5 && s_postProcessRoute == 3u &&
            returnAddress == kGodRayFinalDrawPrimitiveReturn &&
            primitiveType == kD3DPrimitiveTriangleStrip && startVertex == 0u && primitiveCount == 2u &&
            selectorReadable && selectorRaw != 0u;
        if (exactGodRayStockFinal)
        {
            const LONG g4Attempt = InterlockedIncrement(&s_godRayG4BoundaryAttemptCount);
            const bool suppressStock = godRayProbeHandled && g3PassThisCall;
            if (suppressStock)
                InterlockedIncrement(&s_godRayG4StockSuppressCount);
            else
                InterlockedIncrement(&s_godRayG4StockFallbackCount);

            if (ShouldLogDebugXboxIntegrationDetail(g4Attempt) || !suppressStock)
            {
                Event* event = ReserveEvent(EventType::GodRayG4OwnedDrawSuppress);
                if (event)
                {
                    event->a = s_presentCount;
                    event->b = static_cast<uint32_t>(g4Attempt);
                    event->c = suppressStock ? 1u : 0u;
                    event->d = selectorRaw;
                    event->e = godRayProbeHandled ? 1u : 0u;
                    event->f = g3PassThisCall ? 1u : 0u;
                    event->g = static_cast<uint32_t>(s_godRayG3DrawCount);
                    event->h = static_cast<uint32_t>(s_godRayG3PassCount);
                    event->i = static_cast<uint32_t>(s_godRayG3FailCount);
                    event->j = static_cast<uint32_t>(s_godRayG3Disabled);
                    event->extra[0] = static_cast<uint32_t>(s_godRayG4StockSuppressCount);
                    event->extra[1] = static_cast<uint32_t>(s_godRayG4StockFallbackCount);
                    event->extra[2] = suppressStock ? static_cast<uint32_t>(S_OK) : 0u;
                    event->extra[3] = static_cast<uint32_t>(returnAddress);
                    event->extra[4] = primitiveType;
                    event->extra[5] = startVertex;
                    event->extra[6] = primitiveCount;
                }
            }

            // Ownership transfer: the xeSM3 G3 draw already produced this exact
            // stock GodRay primitive and restored the state. Return stock success
            // instead of issuing the duplicate game DrawPrimitive.
            if (suppressStock)
                return S_OK;
            // Fail closed: if G3/G2 did not fully pass, continue below and let the
            // original stock GodRay DrawPrimitive execute normally.
        }

        const bool exactFinal = s_v10Mode == 5 && s_insideComposite &&
            returnAddress == kCompositeFinalDrawPrimitiveReturn &&
            primitiveType == kD3DPrimitiveTriangleStrip && startVertex == 0u && primitiveCount == 2u;
        if (!exactFinal)
            return s_rawDrawPrimitive(device, primitiveType, startVertex, primitiveCount);

        HRESULT retailHr = E_PENDING;
        if (IsXboxPostProcessRoute())
        {
            if (TryRetailXboxF18AtActualDraw(
                    device, primitiveType, startVertex, primitiveCount, returnAddress, retailHr))
                return retailHr;
            InterlockedIncrement(&s_retailXboxDofDrawFallbackCount);
        }

        // Fix-only, Debug-reserved, or Retail stale-depth frames use the proven
        // valid BE64 clamp, now held across the ACTUAL GPU draw for the first time.
        return DrawMode5ActualClampAtFinalPrimitive(
            device, primitiveType, startVertex, primitiveCount, returnAddress);
    }

    HRESULT WINAPI RawReset_Hook(void* device, void* presentParameters)
    {
        if (!s_rawReset) return E_FAIL;
        if (InterlockedCompareExchange(&s_deviceResetInFlight, 1, 0) != 0)
            return s_rawReset(device, presentParameters);
        const LONG resetCount = InterlockedIncrement(&s_deviceResetCount);
        uint32_t width = 0u, height = 0u, format = 0u;
        if (presentParameters && IsReadableMemory(presentParameters, 12u))
        {
            const uint32_t* pp = reinterpret_cast<const uint32_t*>(presentParameters);
            width = pp[0]; height = pp[1]; format = pp[2];
        }
        const bool hadZBlurResource = s_cameraMotionZBlurCopyTexture || s_cameraMotionZBlurCopySurface;
        const bool hadIntzResource = s_offscreenIntzTexture || s_offscreenIntzSurface;
        const bool hadPackedDepthResource = s_mode4PackedDepthTexture || s_mode4PackedDepthSurface;

        // Fail closed while the device is lost/resetting. Any failed Reset leaves
        // this gate closed, so no PostFX resource recreation is attempted against a
        // lost device. A successful Reset restores both the device identity and gate.
        InterlockedExchange(&s_postFxDeviceReady, 0);
        s_rawDeviceValue = 0u;
        ReleasePostFXDeviceResourcesForReset();
        const HRESULT hr = s_rawReset(device, presentParameters);
        s_deviceResetLastHr = hr;
        s_deviceResetLastWidth = width;
        s_deviceResetLastHeight = height;
        s_deviceResetLastFormat = format;
        if (SUCCEEDED(hr))
        {
            s_rawDeviceValue = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(device));
            InterlockedExchange(&s_postFxDeviceReady, 1);
        }
        Event* event = ReserveEvent(EventType::DeviceReset);
        if (event)
        {
            event->a = static_cast<uint32_t>(resetCount);
            event->b = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(device));
            event->c = width; event->d = height; event->e = format; event->f = static_cast<uint32_t>(hr);
            event->g = hadZBlurResource ? 1u : 0u; event->h = hadIntzResource ? 1u : 0u;
            event->i = hadPackedDepthResource ? 1u : 0u; event->j = static_cast<uint32_t>(s_v10Mode);
            event->extra[0] = static_cast<uint32_t>(s_postFxDeviceReady);
        }
        InterlockedExchange(&s_deviceResetInFlight, 0);
        return hr;
    }

    HRESULT WINAPI RawPresent_Hook(
        void* device,
        const void* sourceRect,
        const void* destRect,
        void* destWindowOverride,
        const void* dirtyRegion)
    {
        // The original Present trampoline is always called. V10.4 advances
        // the recovered Xbox exposure state once per processed present. Mode 2
        // keeps the frozen V9 visual multiply; mode 3 applies no Present-stage
        // exposure because it uses PC's native exposure shader parameter.
        if (!s_rawPresent)
            return E_FAIL;

        if (InterlockedCompareExchange(&s_v9PresentInFlight, 1, 0) != 0)
            return s_rawPresent(device, sourceRect, destRect, destWindowOverride, dirtyRegion);

        const uint32_t presentIndex = s_presentCount;

        // V10.5.18 readback runs here because the game scene has ended.
        if (s_v10Mode == 4 && s_mode4DepthReadbackPending == 1)
            SampleMode4DepthPrepassReadback(device);

        const bool ready =
            s_v10Mode >= 1 &&
            ReadU8(kAutoExposureEnabled) != 0u &&
            s_compositeCount > 30 &&
            s_rawSurfaceLum16 != 0u &&
            device != nullptr;

        if (ready && presentIndex != 0u && s_v9LastProcessedPresent != presentIndex)
        {
            s_v9LastProcessedPresent = presentIndex;

            // The game's scene has ended, making GetRenderTargetData legal.
            // This one readback feeds V10 bloom and V9 comparison telemetry in
            // modes 2/3 so all exposure controllers observe the same luminance.
            if ((presentIndex % kV10ReadbackInterval) == 0u)
                SamplePostFXLuminance(device);

            // Modes 1-3 retain the historical Xbox exposure-state research.
            // Mode 4 deliberately does NOT advance or apply that exposure state:
            // V10.5.49.10.1 changes only the recovered pre-DOF bloom tail constants over
            // the .49.9 weighted source and keeps the .49.4 DOF/exposure path frozen.
            if (s_v10Mode != 4)
                UpdateV10_3XboxExposureState();

            if (s_v10Mode == 2 && s_v9ExposureEnabled &&
                s_v9WarmupCount >= kV9WarmupSamples && s_rawGetBackBuffer)
            {
                void* backbuffer = nullptr;
                const HRESULT getBackBufferHr =
                    s_rawGetBackBuffer(device, 0u, 0u, 0u, &backbuffer);
                if (SUCCEEDED(getBackBufferHr) && backbuffer)
                {
                    ApplyV9ExposurePass(device, backbuffer);
                    SafeReleaseCom(backbuffer);
                }
            }
        }

        InterlockedExchange(&s_v9PresentInFlight, 0);
        return s_rawPresent(device, sourceRect, destRect, destWindowOverride, dirtyRegion);
    }

    void QueueMode5DedicatedDrawHookStatus(uint32_t phase, uint32_t errorCode)
    {
        Event* event = ReserveEvent(EventType::Mode5DedicatedDrawHookStatus);
        if (!event)
            return;
        event->a = s_presentCount;
        event->b = phase;
        event->c = static_cast<uint32_t>(s_mode5DrawHookState);
        event->d = s_mode5DrawHookDevice;
        event->e = s_mode5DrawHookTarget;
        event->f = errorCode;
        event->g = static_cast<uint32_t>(s_mode5DrawHookAttemptCount);
        event->h = static_cast<uint32_t>(s_mode5DrawHookAttachCount);
        event->i = static_cast<uint32_t>(s_mode5DrawHookFailCount);
        event->j = kD3DDrawPrimitiveVtableIndex;
        event->extra[0] = s_rawHookState;
        event->extra[1] = s_postProcessRoute;
        event->extra[2] = static_cast<uint32_t>(s_v10Mode);
        event->extra[3] = kD3DResetVtableIndex;
        event->extra[4] = s_mode5ResetHookTarget;
    }

    void TryAttachMode5DedicatedDeviceDetours()
    {
        if (s_v10Mode != 5 || !s_postProcessNativeRepairActive)
            return;
        if (s_mode5DrawHookState == 2 || s_mode5DrawHookState == -1)
            return;
        if (InterlockedCompareExchange(&s_mode5DrawHookState, 1, 0) != 0)
            return;

        InterlockedIncrement(&s_mode5DrawHookAttemptCount);
        const uint32_t deviceValue = ReadU32(kD3DDeviceGlobal);
        s_mode5DrawHookDevice = deviceValue;
        if (deviceValue < 0x10000u)
        {
            // Device not ready yet: retry on the next present. This is not a hard failure.
            InterlockedExchange(&s_mode5DrawHookState, 0);
            return;
        }

        void* device = reinterpret_cast<void*>(static_cast<uintptr_t>(deviceValue));
        if (!IsReadableMemory(device, sizeof(void*)))
        {
            s_mode5DrawHookLastError = ERROR_INVALID_ADDRESS;
            InterlockedIncrement(&s_mode5DrawHookFailCount);
            InterlockedExchange(&s_mode5DrawHookState, -1);
            QueueMode5DedicatedDrawHookStatus(0xFFFFFFFFu, ERROR_INVALID_ADDRESS);
            return;
        }

        void** vtable = *reinterpret_cast<void***>(device);
        if (!vtable || !IsReadableMemory(vtable,
                (kD3DDrawPrimitiveVtableIndex + 1u) * sizeof(void*)))
        {
            s_mode5DrawHookLastError = ERROR_INVALID_ADDRESS;
            InterlockedIncrement(&s_mode5DrawHookFailCount);
            InterlockedExchange(&s_mode5DrawHookState, -1);
            QueueMode5DedicatedDrawHookStatus(0xFFFFFFFEu, ERROR_INVALID_ADDRESS);
            return;
        }

        void* drawTarget = vtable[kD3DDrawPrimitiveVtableIndex];
        void* resetTarget = vtable[kD3DResetVtableIndex];
        s_mode5DrawHookTarget = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(drawTarget));
        s_mode5ResetHookTarget = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(resetTarget));
        if (!drawTarget || !IsExecutableMemory(drawTarget) ||
            !resetTarget || !IsExecutableMemory(resetTarget))
        {
            s_mode5DrawHookLastError = ERROR_INVALID_ADDRESS;
            InterlockedIncrement(&s_mode5DrawHookFailCount);
            InterlockedExchange(&s_mode5DrawHookState, -1);
            QueueMode5DedicatedDrawHookStatus(0xFFFFFFFDu, ERROR_INVALID_ADDRESS);
            return;
        }

        // V10.5.72 production exception set: slot 16 Reset + slot 81 DrawPrimitive.
        // Reset exists only to release/rebuild xeSM3-owned D3DPOOL_DEFAULT PostFX
        // resources across Alt-Tab/resolution changes. Present/RT/texture/shader/
        // constant/depth research hooks remain unreachable in Mode 5.
        s_rawReset = reinterpret_cast<D3DReset_t>(resetTarget);
        s_rawDrawPrimitive = reinterpret_cast<D3DDrawPrimitive_t>(drawTarget);
        s_rawDeviceValue = deviceValue;

        LONG err = DetourTransactionBegin();
        const bool transactionStarted = err == NO_ERROR;
        if (err == NO_ERROR)
            err = DetourUpdateThread(GetCurrentThread());
        if (err == NO_ERROR)
            err = DetourAttach(&reinterpret_cast<PVOID&>(s_rawReset), RawReset_Hook);
        if (err == NO_ERROR)
            err = DetourAttach(&reinterpret_cast<PVOID&>(s_rawDrawPrimitive), RawDrawPrimitive_Hook);
        if (err != NO_ERROR)
        {
            if (transactionStarted)
                DetourTransactionAbort();
            s_mode5DrawHookLastError = static_cast<uint32_t>(err);
            InterlockedIncrement(&s_mode5DrawHookFailCount);
            InterlockedExchange(&s_mode5DrawHookState, -1);
            QueueMode5DedicatedDrawHookStatus(0xFFFFFFFCu, static_cast<uint32_t>(err));
            return;
        }

        err = DetourTransactionCommit();
        s_mode5DrawHookLastError = static_cast<uint32_t>(err);
        if (err == NO_ERROR)
        {
            InterlockedIncrement(&s_mode5DrawHookAttachCount);
            InterlockedExchange(&s_mode5DrawHookState, 2);
            QueueMode5DedicatedDrawHookStatus(2u, 0u);
        }
        else
        {
            InterlockedIncrement(&s_mode5DrawHookFailCount);
            InterlockedExchange(&s_mode5DrawHookState, -1);
            QueueMode5DedicatedDrawHookStatus(0xFFFFFFFBu, static_cast<uint32_t>(err));
        }
    }

    void QueueRawHookStatus(uint32_t status, uint32_t device, uint32_t setRT, uint32_t setTexture, uint32_t setPS, uint32_t setConst)
    {
        Event* event = ReserveEvent(EventType::RawHookStatus);
        if (!event)
            return;
        event->a = status;
        event->b = device;
        event->c = setRT;
        event->d = setTexture;
        event->e = setPS;
        event->f = setConst;
    }

    void TryAttachRawD3DDetours()
    {
        if (s_rawHookState == 2 || s_rawHookState == -1)
            return;
        if (InterlockedCompareExchange(&s_rawHookState, 1, 0) != 0)
            return;

        const uint32_t deviceValue = ReadU32(kD3DDeviceGlobal);
        if (deviceValue < 0x10000u)
        {
            InterlockedExchange(&s_rawHookState, 0);
            return;
        }

        void* device = reinterpret_cast<void*>(static_cast<uintptr_t>(deviceValue));
        void** vtable = *reinterpret_cast<void***>(device);
        if (!vtable)
        {
            InterlockedExchange(&s_rawHookState, -1);
            QueueRawHookStatus(0xFFFFFFFFu, deviceValue, 0, 0, 0, 0);
            return;
        }

        s_rawDeviceValue = deviceValue;
        s_rawReset = reinterpret_cast<D3DReset_t>(vtable[kD3DResetVtableIndex]);
        s_rawPresent = reinterpret_cast<D3DPresent_t>(vtable[kD3DPresentVtableIndex]);
        s_rawGetBackBuffer = reinterpret_cast<D3DGetBackBuffer_t>(vtable[kD3DGetBackBufferVtableIndex]);
        s_rawCreateTexture = reinterpret_cast<D3DCreateTexture_t>(vtable[kD3DCreateTextureVtableIndex]);
        s_rawCreateDepthStencilSurface = reinterpret_cast<D3DCreateDepthStencilSurface_t>(vtable[kD3DCreateDepthStencilSurfaceVtableIndex]);
        s_rawGetRenderTargetData = reinterpret_cast<D3DGetRenderTargetData_t>(vtable[kD3DGetRenderTargetDataVtableIndex]);
        s_rawStretchRect = reinterpret_cast<D3DStretchRect_t>(vtable[kD3DStretchRectVtableIndex]);
        s_rawCreateOffscreenPlainSurface =
            reinterpret_cast<D3DCreateOffscreenPlainSurface_t>(vtable[kD3DCreateOffscreenPlainSurfaceVtableIndex]);
        s_rawSetRenderTarget = reinterpret_cast<D3DSetRenderTarget_t>(vtable[kD3DSetRenderTargetVtableIndex]);
        s_rawGetRenderTarget = reinterpret_cast<D3DGetRenderTarget_t>(vtable[kD3DGetRenderTargetVtableIndex]);
        s_rawSetDepthStencilSurface = reinterpret_cast<D3DSetDepthStencilSurface_t>(vtable[kD3DSetDepthStencilSurfaceVtableIndex]);
        s_rawGetDepthStencilSurface = reinterpret_cast<D3DGetDepthStencilSurface_t>(vtable[kD3DGetDepthStencilSurfaceVtableIndex]);
        s_rawBeginScene = reinterpret_cast<D3DBeginScene_t>(vtable[kD3DBeginSceneVtableIndex]);
        s_rawEndScene = reinterpret_cast<D3DEndScene_t>(vtable[kD3DEndSceneVtableIndex]);
        s_rawSetViewport = reinterpret_cast<D3DSetViewport_t>(vtable[kD3DSetViewportVtableIndex]);
        s_rawSetRenderState = reinterpret_cast<D3DSetRenderState_t>(vtable[kD3DSetRenderStateVtableIndex]);
        s_rawGetRenderState = reinterpret_cast<D3DGetRenderState_t>(vtable[kD3DGetRenderStateVtableIndex]);
        s_rawCreateStateBlock = reinterpret_cast<D3DCreateStateBlock_t>(vtable[kD3DCreateStateBlockVtableIndex]);
        s_rawSetTexture = reinterpret_cast<D3DSetTexture_t>(vtable[kD3DSetTextureVtableIndex]);
        s_rawSetTextureStageState =
            reinterpret_cast<D3DSetTextureStageState_t>(vtable[kD3DSetTextureStageStateVtableIndex]);
        s_rawSetSamplerState = reinterpret_cast<D3DSetSamplerState_t>(vtable[kD3DSetSamplerStateVtableIndex]);
        s_rawDrawPrimitive = reinterpret_cast<D3DDrawPrimitive_t>(vtable[kD3DDrawPrimitiveVtableIndex]);
        s_rawDrawPrimitiveUP = reinterpret_cast<D3DDrawPrimitiveUP_t>(vtable[kD3DDrawPrimitiveUPVtableIndex]);
        s_rawSetFVF = reinterpret_cast<D3DSetFVF_t>(vtable[kD3DSetFVFVtableIndex]);
        s_rawSetVertexShader = reinterpret_cast<D3DSetVertexShader_t>(vtable[kD3DSetVertexShaderVtableIndex]);
        s_rawSetVertexShaderConstantF = reinterpret_cast<D3DSetVertexShaderConstantF_t>(vtable[kD3DSetVertexShaderConstantFVtableIndex]);
        s_rawCreatePixelShader = reinterpret_cast<D3DCreatePixelShader_t>(vtable[kD3DCreatePixelShaderVtableIndex]);
        s_rawSetPixelShader = reinterpret_cast<D3DSetPixelShader_t>(vtable[kD3DSetPixelShaderVtableIndex]);
        s_rawSetPixelShaderConstantF = reinterpret_cast<D3DSetPixelShaderConstantF_t>(vtable[kD3DSetPixelShaderConstantFVtableIndex]);

        const uint32_t presentAddress = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(s_rawPresent));
        const uint32_t setRTAddress = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(s_rawSetRenderTarget));
        const uint32_t setTextureAddress = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(s_rawSetTexture));
        const uint32_t setPSAddress = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(s_rawSetPixelShader));
        const uint32_t setConstAddress = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(s_rawSetPixelShaderConstantF));

        LONG err = DetourTransactionBegin();
        if (err == NO_ERROR)
            err = DetourUpdateThread(GetCurrentThread());
        if (err == NO_ERROR)
            err = DetourAttach(&reinterpret_cast<PVOID&>(s_rawReset), RawReset_Hook);
        if (err == NO_ERROR)
            err = DetourAttach(&reinterpret_cast<PVOID&>(s_rawPresent), RawPresent_Hook);
        if (err == NO_ERROR)
            err = DetourAttach(&reinterpret_cast<PVOID&>(s_rawCreateDepthStencilSurface), RawCreateDepthStencilSurface_Hook);
        if (err == NO_ERROR)
            err = DetourAttach(&reinterpret_cast<PVOID&>(s_rawSetDepthStencilSurface), RawSetDepthStencilSurface_Hook);
        if (err == NO_ERROR)
            err = DetourAttach(&reinterpret_cast<PVOID&>(s_rawSetRenderTarget), RawSetRenderTarget_Hook);
        if (err == NO_ERROR)
            err = DetourAttach(&reinterpret_cast<PVOID&>(s_rawSetTexture), RawSetTexture_Hook);
        if (err == NO_ERROR)
            err = DetourAttach(&reinterpret_cast<PVOID&>(s_rawSetPixelShader), RawSetPixelShader_Hook);
        if (err == NO_ERROR)
            err = DetourAttach(&reinterpret_cast<PVOID&>(s_rawSetPixelShaderConstantF), RawSetPixelShaderConstantF_Hook);
        if (err == NO_ERROR)
            err = DetourAttach(&reinterpret_cast<PVOID&>(s_rawDrawPrimitive), RawDrawPrimitive_Hook);

        if (err != NO_ERROR)
        {
            DetourTransactionAbort();
            InterlockedExchange(&s_rawHookState, -1);
            QueueRawHookStatus(static_cast<uint32_t>(err), deviceValue,
                setRTAddress, setTextureAddress, setPSAddress, setConstAddress);
            return;
        }

        err = DetourTransactionCommit();
        if (err == NO_ERROR)
            InterlockedExchange(&s_rawHookState, 2);
        else
            InterlockedExchange(&s_rawHookState, -1);

        QueueRawHookStatus(static_cast<uint32_t>(err), deviceValue,
            setRTAddress, setTextureAddress, setPSAddress, setConstAddress);
    }

    int CallOriginalSetRTTracked(int renderTarget)
    {
        const uint32_t previous = s_nglSetRTWrapperInFlight;
        s_nglSetRTWrapperInFlight = static_cast<uint32_t>(renderTarget);
        const int result = s_originalSetRT(renderTarget);
        s_nglSetRTWrapperInFlight = previous;
        return result;
    }

    int __cdecl ExposureScalarCallsite_Hook(int parameter, float value)
    {
        const uintptr_t returnAddress = reinterpret_cast<uintptr_t>(_ReturnAddress());

        // This wrapper is reachable ONLY from the patched call at 0081F12B.
        // V10.4.1 therefore avoids detouring the generic 008D3140 setter and
        // cannot intercept unrelated shader-scalar traffic during startup.
        const LONG matched = InterlockedIncrement(&s_v10_4NativeExposureMatchedCount);
        const uint8_t autoExposure = ReadU8(kAutoExposureEnabled);
        const bool stateFinite = IsFiniteUsefulFloat(s_v10_3XboxExposureState);
        const bool ready =
            s_v10Mode == 3 &&
            autoExposure != 0u &&
            s_v10BloomSampleValid &&
            s_v10_3XboxExposureUpdateCount > 0u &&
            stateFinite;

        const float xboxState = s_v10_3XboxExposureState;
        const float injectedExposure = ready ? -xboxState : value;
        LONG appliedCount = s_v10_4NativeExposureAppliedCount;
        LONG bypassCount = s_v10_4NativeExposureBypassCount;
        if (ready)
            appliedCount = InterlockedIncrement(&s_v10_4NativeExposureAppliedCount);
        else
            bypassCount = InterlockedIncrement(&s_v10_4NativeExposureBypassCount);

        const int result = s_stockShaderScalarSetter(parameter, injectedExposure);

        s_v10_4LastExposureReturnAddress = static_cast<uint32_t>(returnAddress);
        s_v10_4LastExposureParameter = static_cast<uint32_t>(parameter);
        s_v10_4LastOriginalExposure = value;
        s_v10_4LastInjectedExposure = injectedExposure;
        s_v10_4LastXboxExposureState = xboxState;
        s_v10_4LastExposureApplied = ready;

        if (matched <= 32 || (matched % 120) == 0 || (s_v10Mode == 3 && !ready && matched <= 120))
        {
            uint32_t readyMask = 0u;
            if (s_v10Mode == 3) readyMask |= 1u << 0;
            if (autoExposure != 0u) readyMask |= 1u << 1;
            if (s_v10BloomSampleValid) readyMask |= 1u << 2;
            if (s_v10_3XboxExposureUpdateCount > 0u) readyMask |= 1u << 3;
            if (stateFinite) readyMask |= 1u << 4;

            const uint32_t sampleAge = s_presentCount >= s_v10_3LastLuminanceSamplePresent ?
                s_presentCount - s_v10_3LastLuminanceSamplePresent : 0u;
            Event* event = ReserveEvent(EventType::NativeExposureApply);
            if (event)
            {
                event->a = s_presentCount;
                event->b = static_cast<uint32_t>(matched);
                event->c = ready ? 1u : 0u;
                event->d = static_cast<uint32_t>(s_v10Mode);
                event->e = static_cast<uint32_t>(returnAddress);
                event->f = static_cast<uint32_t>(parameter);
                event->g = FloatBits(value);
                event->h = FloatBits(injectedExposure);
                event->i = FloatBits(xboxState);
                event->j = autoExposure;
                event->extra[0] = s_v10BloomSampleValid ? 1u : 0u;
                event->extra[1] = s_v10_3XboxExposureUpdateCount;
                event->extra[2] = sampleAge;
                event->extra[3] = FloatBits(s_v9CurrentLuminance);
                event->extra[4] = FloatBits(s_v9TargetExposure);
                event->extra[5] = FloatBits(s_v9AdaptedExposure);
                event->extra[6] = s_v9ExposureEnabled ? 1u : 0u;
                event->extra[7] = readyMask;
                event->extra[8] = static_cast<uint32_t>(result);
                event->extra[9] = static_cast<uint32_t>(appliedCount);
                event->extra[10] = static_cast<uint32_t>(bypassCount);
            }
        }

        return result;
    }

    int __cdecl CreateRT_Hook(uint32_t flags, int format, int width, int height, int p5, int p6)
    {
        const uintptr_t returnAddress = reinterpret_cast<uintptr_t>(_ReturnAddress());
        int effectiveFormat = format;
        bool overrideFormat = false;

        const bool requestedLdr = static_cast<uint32_t>(format) == kFormatA8R8G8B8;

        // Historical research modes retain the previously proven partial FP16 route.
        const bool quarter =
            (returnAddress == kQuarterAReturnAddress || returnAddress == kQuarterBReturnAddress) &&
            flags == kQuarterRTFlags && requestedLdr;

        const bool lum160 =
            returnAddress == kLuminance160ReturnAddress && flags == kLuminanceRTFlags &&
            width == 160 && height == 120 && requestedLdr;

        const bool lum16 =
            returnAddress == kTerminalLuminance16x12ReturnAddress && flags == kLuminanceRTFlags &&
            width == 16 && height == 12 && requestedLdr;

        // V10.5.55 PostProcessFix: retain Kirby-style native resource repair. Upgrade only
        // allocations proven to originate from SM3_PostProcess_Initialize.
        // No other NGL render target in the game is affected.
        const bool nativePostFxFull =
            returnAddress == kFullSizeReturnAddress && flags == kColorRTFlags && requestedLdr;
        const bool nativePostFxQuarter = quarter;
        const bool nativePostFxLum160 = lum160;
        const bool nativePostFxLum16A = lum16;
        const bool nativePostFxLum16B =
            returnAddress == kSecondLuminance16x12ReturnAddress &&
            flags == kLuminanceRTFlags && width == 16 && height == 12 && requestedLdr;
        const bool nativePostFxHalf =
            (returnAddress == kHalfAReturnAddress || returnAddress == kHalfBReturnAddress) &&
            flags == kColorRTFlags && requestedLdr;

        const bool nativePostFxTarget =
            nativePostFxFull || nativePostFxQuarter || nativePostFxLum160 ||
            nativePostFxLum16A || nativePostFxLum16B || nativePostFxHalf;

        if (s_postProcessNativeRepairActive && nativePostFxTarget)
        {
            effectiveFormat = static_cast<int>(kFormatA16B16G16R16F);
            overrideFormat = true;
        }

        const int result = s_originalCreateRT(flags, effectiveFormat, width, height, p5, p6);

        if (static_cast<uint32_t>(format) == kFormatD24S8 &&
            (flags & 0x80u) != 0u && result >= 0x10000)
        {
            CaptureMode4NglDepthWrapper(
                static_cast<uint32_t>(result),
                flags,
                static_cast<uint32_t>(width),
                static_cast<uint32_t>(height));
        }

        Event* event = ReserveEvent(EventType::CreateRT);
        if (event)
        {
            event->a = static_cast<uint32_t>(returnAddress);
            event->b = flags;
            event->c = static_cast<uint32_t>(format);
            event->d = static_cast<uint32_t>(effectiveFormat);
            event->e = overrideFormat ? 1u : 0u;
            event->f = static_cast<uint32_t>(width);
            event->g = static_cast<uint32_t>(height);
            event->h = static_cast<uint32_t>(result);
        }
        return result;
    }

    void ProbeMode4DepthSource(void* device)
    {
        InterlockedIncrement(&s_mode4DepthProbeCount);

        for (uint32_t i = 0; i < 8u; ++i)
        {
            const uintptr_t wordAddress = kPostFxDepthDescriptor + i * sizeof(uint32_t);
            s_mode4DepthDescriptorWords[i] = IsReadableMemory(
                reinterpret_cast<void*>(wordAddress), sizeof(uint32_t)) ? ReadU32(wordAddress) : 0xDEADBEEFu;
        }

        s_mode4LastDepthSurface = 0u;
        s_mode4LastDepthSurfaceTexture = 0u;
        s_mode4LastGetDepthSurfaceHr = E_PENDING;
        s_mode4LastDepthSurfaceDescHr = E_PENDING;
        s_mode4LastDepthSurfaceContainerHr = E_PENDING;
        ZeroMemory(&s_mode4LastDepthSurfaceDesc, sizeof(s_mode4LastDepthSurfaceDesc));

        if (!device || !s_rawGetDepthStencilSurface)
            return;

        void* depthSurface = nullptr;
        s_mode4LastGetDepthSurfaceHr = s_rawGetDepthStencilSurface(device, &depthSurface);
        if (FAILED(s_mode4LastGetDepthSurfaceHr) || !depthSurface)
            return;

        s_mode4LastDepthSurface = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(depthSurface));

        D3DSurfaceDescLite desc = {};
        if (GetSurfaceDescSafe(depthSurface, desc))
        {
            s_mode4LastDepthSurfaceDescHr = S_OK;
            s_mode4LastDepthSurfaceDesc = desc;
        }
        else
        {
            s_mode4LastDepthSurfaceDescHr = E_FAIL;
        }

        HRESULT containerHr = E_PENDING;
        s_mode4LastDepthSurfaceTexture = RawTextureFromSurface(
            s_mode4LastDepthSurface, &containerHr);
        s_mode4LastDepthSurfaceContainerHr = containerHr;

        SafeReleaseCom(depthSurface);
    }

    void ResetMode4PackedDepthResources()
    {
        SafeReleaseCom(s_mode4PackStateBlock);
        SafeReleaseCom(s_mode4PackedDepthPixelShader);
        SafeReleaseCom(s_mode4PackedDepthSurface);
        SafeReleaseCom(s_mode4PackedDepthTexture);
        s_mode4PackedDepthWidth = 0u;
        s_mode4PackedDepthHeight = 0u;
        s_mode4PackStateBlockDeviceIdentity = 0u;
        InterlockedExchange(&s_mode4PackCreateState, 0);
    }

    // D3D9 Reset / resolution-change hygiene. Release only xeSM3-owned
    // resources; stock game render targets/textures are never released here.
    void ReleasePostFXDeviceResourcesForReset()
    {
        ResetMode4CameraMotionZBlurResources();
        ResetRetailXboxImageZoomProbeResources();
        ResetV10BloomStateBlock();
        ResetV9SceneResources();
        ResetV9ReadbackResource();

        // xeSM3-owned final-clamp shader clone. Recreate lazily after a successful
        // Reset instead of carrying any device-object identity across the reset.
        SafeReleaseCom(s_mode5FinalCloneShader);
        s_mode5FinalCloneDevice = 0u;
        s_mode5FinalCloneBytecodeHash = 0u;
        InterlockedExchange(&s_mode5FinalCloneState, 0);

        SafeReleaseCom(s_fullResDepthStencil);
        SafeReleaseCom(s_fullResDepthSurface);
        SafeReleaseCom(s_fullResDepthTexture);
        s_fullResDepthWidth = 0u;
        s_fullResDepthHeight = 0u;
        s_fullResContainerTex = 0u;
        s_fullResCreateTextureHr = E_PENDING;
        s_fullResGetSurfaceHr = E_PENDING;
        s_fullResCreateDepthHr = E_PENDING;
        s_fullResContainerHr = E_PENDING;
        s_fullResBindRTHr = E_PENDING;
        s_fullResBindDSHr = E_PENDING;
        s_fullResRestoreDSHr = E_PENDING;
        s_fullResRestoreRTHr = E_PENDING;
        InterlockedExchange(&s_fullResDepthTargetCreateState, 0);
        InterlockedExchange(&s_fullResDepthTargetBindTestState, 0);

        SafeReleaseCom(s_mode4DepthReadbackSurface);
        s_mode4DepthReadbackCreateHr = E_PENDING;
        s_mode4DepthReadbackHr = E_PENDING;
        s_mode4DepthReadbackLockHr = E_PENDING;

        if (s_mode4ReplacementDepthSurface == s_offscreenIntzSurface)
            s_mode4ReplacementDepthSurface = nullptr;
        if (s_mode4ReplacementDepthTexture == s_offscreenIntzTexture)
            s_mode4ReplacementDepthTexture = nullptr;

        SafeReleaseCom(s_offscreenIntzSurface);
        SafeReleaseCom(s_offscreenIntzTexture);
        s_offscreenIntzWidth = 0u;
        s_offscreenIntzHeight = 0u;
        s_offscreenIntzContainerTex = 0u;
        s_offscreenIntzCreateHr = E_PENDING;
        s_offscreenIntzGetSurfaceHr = E_PENDING;
        s_offscreenIntzDescHr = E_PENDING;
        s_offscreenIntzContainerHr = E_PENDING;
        s_offscreenIntzStateBlockHr = E_PENDING;
        s_offscreenIntzCaptureHr = E_PENDING;
        s_offscreenIntzGetRTHr = E_PENDING;
        s_offscreenIntzGetDSHr = E_PENDING;
        s_offscreenIntzBindRTHr = E_PENDING;
        s_offscreenIntzBindDSHr = E_PENDING;
        s_offscreenIntzClearHr = E_PENDING;
        s_offscreenIntzRestoreDSHr = E_PENDING;
        s_offscreenIntzRestoreRTHr = E_PENDING;
        s_offscreenIntzApplyHr = E_PENDING;
        s_offscreenIntzActualRT = 0u;
        s_offscreenIntzActualDS = 0u;
        InterlockedExchange(&s_offscreenIntzProbeState, 0);

        SafeReleaseCom(s_mode4ReplacementDepthSurface);
        SafeReleaseCom(s_mode4ReplacementDepthTexture);
        s_mode4ReplacementWidth = 0u;
        s_mode4ReplacementHeight = 0u;
        s_mode4ReplacementContainerTex = 0u;
        s_mode4ReplacementCreateHr = E_PENDING;
        s_mode4ReplacementSurfaceHr = E_PENDING;
        s_mode4ReplacementContainerHr = E_PENDING;
        InterlockedExchange(&s_mode4DepthReplacementCreateState, 0);

        ResetMode4PackedDepthResources();
        InterlockedExchange(&s_provenIntzDofState, 0);
        InterlockedExchange(&s_reszDepthResolveState, 0);
        InterlockedExchange(&s_provenIntzDofWindowState, 0);
        s_provenIntzDofWindowStartPresent = 0u;
        s_provenIntzDofWindowEndPresent = 0u;
        s_provenIntzDofWindowLastArmedPresent = 0xFFFFFFFFu;
        s_provenIntzDofWindowFrameOrdinal = 0u;
        s_reszDepthResolveScene = 0u;
        s_reszLiveDepthSurface = 0u;
        s_reszLiveDepthFormat = 0u;
        s_reszLiveDepthWidth = 0u;
        s_reszLiveDepthHeight = 0u;
        s_mode4ReszDepthValidated = false;
        s_mode4ReszLastValidationPresent = 0u;
        s_opaqueIntzDiagnosticReadbackThisFrame = false;
        s_opaqueIntzFinite = 0u;
        s_opaqueIntzChanged = 0u;
        s_opaqueIntzMin = 1.0f;
        s_opaqueIntzMax = 1.0f;
        s_opaqueIntzMean = 1.0f;

        s_rawCurrentPixelShader = 0u;
        s_rawCurrentRT0 = 0u;
        for (uint32_t stage = 0u; stage < 16u; ++stage)
            s_rawTextureStages[stage] = 0u;
        s_rawTextureQuarterA = 0u;
        s_rawTextureQuarterB = 0u;
        s_rawTexture160 = 0u;
        s_rawTextureLum16 = 0u;
        s_rawTextureSecond16 = 0u;
        s_rawTextureHalfA = 0u;
        s_rawTextureHalfB = 0u;
        s_rawTextureScene = 0u;
        s_rawSurfaceQuarterA = 0u;
        s_rawSurfaceQuarterB = 0u;
        s_rawSurfaceLum16 = 0u;
        s_rawSurfaceSecond16 = 0u;
        s_rawSurfaceScene = 0u;
        s_v10BloomSampleValid = false;
    }

    bool GetBlobPayload(void* blob, void*& data, SIZE_T& size)
    {
        data = nullptr;
        size = 0u;
        if (!blob || !IsReadableMemory(blob, sizeof(void*)))
            return false;
        void** vtable = *reinterpret_cast<void***>(blob);
        if (!vtable || !IsReadableMemory(vtable, 5u * sizeof(void*)) ||
            !IsExecutableMemory(vtable[3]) || !IsExecutableMemory(vtable[4]))
            return false;
        const auto getPointer = reinterpret_cast<D3DBlobGetBufferPointer_t>(vtable[3]);
        const auto getSize = reinterpret_cast<D3DBlobGetBufferSize_t>(vtable[4]);
        data = getPointer(blob);
        size = getSize(blob);
        return data != nullptr && size >= sizeof(DWORD);
    }

    HRESULT CompileMode4DepthPackShader(void** outShader)
    {
        if (outShader)
            *outShader = nullptr;
        if (!outShader || !s_rawCreatePixelShader || s_rawDeviceValue < 0x10000u)
            return E_FAIL;

        static const char kDepthPackHlsl[] =
            "sampler2D depthSampler : register(s0);\n"
            "float4 channelSelect : register(c0);\n"
            "float4 main(float2 uv : TEXCOORD0) : COLOR0\n"
            "{\n"
            "  float4 rawDepth = tex2D(depthSampler, uv);\n"
            "  float d = saturate(dot(rawDepth, channelSelect));\n"
            "  d = min(d, 0.99999994);\n"
            "  float n = floor(d * 16777216.0);\n"
            "  float hi = floor(n * 0.0000152587890625);\n"
            "  n -= hi * 65536.0;\n"
            "  float mid = floor(n * 0.00390625);\n"
            "  float lo = n - mid * 256.0;\n"
            "  return float4(mid, lo, 0.0, hi) * 0.003921568627451;\n"
            "}\n";

        const char* compilerDlls[] =
        {
            "d3dcompiler_47.dll", "d3dcompiler_46.dll", "d3dcompiler_43.dll"
        };
        HMODULE compilerModule = nullptr;
        D3DCompile_t compileFn = nullptr;
        for (const char* dllName : compilerDlls)
        {
            compilerModule = LoadLibraryA(dllName);
            if (!compilerModule)
                continue;
            compileFn = reinterpret_cast<D3DCompile_t>(GetProcAddress(compilerModule, "D3DCompile"));
            if (compileFn)
                break;
            FreeLibrary(compilerModule);
            compilerModule = nullptr;
        }
        if (!compileFn || !compilerModule)
        {
            strncpy_s(s_mode4PackCompilerText, sizeof(s_mode4PackCompilerText),
                "D3DCompile unavailable", _TRUNCATE);
            return HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND);
        }

        void* codeBlob = nullptr;
        void* errorBlob = nullptr;
        const HRESULT compileHr = compileFn(
            kDepthPackHlsl, sizeof(kDepthPackHlsl) - 1u, "xeSM3_Mode4_DepthPack",
            nullptr, nullptr, "main", "ps_2_0", 0u, 0u, &codeBlob, &errorBlob);

        if (errorBlob)
        {
            void* errorData = nullptr;
            SIZE_T errorSize = 0u;
            if (GetBlobPayload(errorBlob, errorData, errorSize) && errorData && errorSize)
            {
                const size_t copyBytes = (errorSize < sizeof(s_mode4PackCompilerText) - 1u)
                    ? static_cast<size_t>(errorSize)
                    : sizeof(s_mode4PackCompilerText) - 1u;
                memcpy(s_mode4PackCompilerText, errorData, copyBytes);
                s_mode4PackCompilerText[copyBytes] = '\0';
            }
        }

        HRESULT createHr = compileHr;
        if (SUCCEEDED(compileHr) && codeBlob)
        {
            void* codeData = nullptr;
            SIZE_T codeSize = 0u;
            if (GetBlobPayload(codeBlob, codeData, codeSize))
            {
                void* device = reinterpret_cast<void*>(static_cast<uintptr_t>(s_rawDeviceValue));
                createHr = s_rawCreatePixelShader(
                    device, reinterpret_cast<const DWORD*>(codeData), outShader);
            }
            else
            {
                createHr = E_FAIL;
            }
        }

        SafeReleaseCom(errorBlob);
        SafeReleaseCom(codeBlob);
        FreeLibrary(compilerModule);
        return createHr;
    }

    bool EnsureMode4PackedDepthResources(void* device, uint32_t width, uint32_t height)
    {
        if (!s_mode4PackedDepthEnabled || !device || !s_rawCreateTexture ||
            !s_rawCreatePixelShader || width == 0u || height == 0u)
            return false;

        if (s_mode4PackedDepthTexture && s_mode4PackedDepthSurface &&
            s_mode4PackedDepthPixelShader && s_mode4PackedDepthWidth == width &&
            s_mode4PackedDepthHeight == height)
        {
            D3DSurfaceDescLite check = {};
            if (GetSurfaceDescSafe(s_mode4PackedDepthSurface, check) &&
                check.Width == width && check.Height == height &&
                check.Format == kFormatA8R8G8B8)
                return true;
        }

        ResetMode4PackedDepthResources();
        InterlockedExchange(&s_mode4PackCreateState, 2);
        s_mode4PackCompilerText[0] = '\0';
        s_mode4PackCompileHr = E_PENDING;
        s_mode4PackCreateTextureHr = E_PENDING;
        s_mode4PackGetSurfaceHr = E_PENDING;
        s_mode4PackCreateShaderHr = E_PENDING;

        void* texture = nullptr;
        s_mode4PackCreateTextureHr = s_rawCreateTexture(
            device, width, height, 1u, kD3DUsageRenderTarget,
            kFormatA8R8G8B8, kD3DPoolDefault, &texture, nullptr);
        if (FAILED(s_mode4PackCreateTextureHr) || !texture)
        {
            InterlockedExchange(&s_mode4PackCreateState, -1);
            return false;
        }

        void* surface = nullptr;
        void** textureVtable = *reinterpret_cast<void***>(texture);
        if (!textureVtable || !IsReadableMemory(textureVtable,
            (kD3DTextureGetSurfaceLevelVtableIndex + 1u) * sizeof(void*)) ||
            !IsExecutableMemory(textureVtable[kD3DTextureGetSurfaceLevelVtableIndex]))
        {
            SafeReleaseCom(texture);
            InterlockedExchange(&s_mode4PackCreateState, -1);
            return false;
        }
        const auto getSurfaceLevel = reinterpret_cast<D3DTextureGetSurfaceLevel_t>(
            textureVtable[kD3DTextureGetSurfaceLevelVtableIndex]);
        s_mode4PackGetSurfaceHr = getSurfaceLevel(texture, 0u, &surface);
        if (FAILED(s_mode4PackGetSurfaceHr) || !surface)
        {
            SafeReleaseCom(surface);
            SafeReleaseCom(texture);
            InterlockedExchange(&s_mode4PackCreateState, -1);
            return false;
        }

        void* shader = nullptr;
        s_mode4PackCreateShaderHr = CompileMode4DepthPackShader(&shader);
        s_mode4PackCompileHr = s_mode4PackCreateShaderHr;
        if (FAILED(s_mode4PackCreateShaderHr) || !shader)
        {
            SafeReleaseCom(shader);
            SafeReleaseCom(surface);
            SafeReleaseCom(texture);
            InterlockedExchange(&s_mode4PackCreateState, -1);
            return false;
        }

        s_mode4PackedDepthTexture = texture;
        s_mode4PackedDepthSurface = surface;
        s_mode4PackedDepthPixelShader = shader;
        s_mode4PackedDepthWidth = width;
        s_mode4PackedDepthHeight = height;
        InterlockedExchange(&s_mode4PackCreateState, 1);
        return true;
    }

    bool EnsureMode4PackStateBlock(void* device)
    {
        if (!device || !s_rawCreateStateBlock)
            return false;
        const uint32_t identity = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(device));
        if (s_mode4PackStateBlock && s_mode4PackStateBlockDeviceIdentity == identity)
            return true;
        SafeReleaseCom(s_mode4PackStateBlock);
        s_mode4PackStateBlockDeviceIdentity = 0u;
        void* stateBlock = nullptr;
        const HRESULT hr = s_rawCreateStateBlock(device, kD3DStateBlockAll, &stateBlock);
        if (FAILED(hr) || !stateBlock)
            return false;
        s_mode4PackStateBlock = stateBlock;
        s_mode4PackStateBlockDeviceIdentity = identity;
        return true;
    }

    bool ApplyMode4PackedDepthPass(void* device)
    {
        s_mode4PackSetupHr = E_PENDING;
        s_mode4PackDrawHr = E_PENDING;
        s_mode4PackRestoreHr = E_PENDING;

        if (!s_mode4PackedDepthEnabled || !device ||
            !s_mode4ReplacementDepthTexture || !s_mode4ReplacementDepthSurface ||
            s_mode4DepthReplacementAppliedCount <= 0 || !s_rawSetRenderTarget ||
            !s_rawGetRenderTarget || !s_rawSetDepthStencilSurface ||
            !s_rawGetDepthStencilSurface || !s_rawSetTexture ||
            !s_rawSetPixelShader || !s_rawSetPixelShaderConstantF ||
            !s_rawSetVertexShader || !s_rawSetFVF || !s_rawSetViewport ||
            !s_rawSetRenderState || !s_rawSetSamplerState || !s_rawDrawPrimitiveUP)
        {
            InterlockedIncrement(&s_mode4PackFailCount);
            return false;
        }

        D3DSurfaceDescLite sourceDesc = {};
        if (!GetSurfaceDescSafe(s_mode4ReplacementDepthSurface, sourceDesc) ||
            sourceDesc.Width == 0u || sourceDesc.Height == 0u)
        {
            InterlockedIncrement(&s_mode4PackFailCount);
            return false;
        }

        if (!EnsureMode4PackedDepthResources(device, sourceDesc.Width, sourceDesc.Height) ||
            !EnsureMode4PackStateBlock(device))
        {
            InterlockedIncrement(&s_mode4PackFailCount);
            return false;
        }

        void** stateVtable = *reinterpret_cast<void***>(s_mode4PackStateBlock);
        if (!stateVtable || !IsReadableMemory(stateVtable,
            (kD3DStateBlockApplyVtableIndex + 1u) * sizeof(void*)) ||
            !IsExecutableMemory(stateVtable[kD3DStateBlockCaptureVtableIndex]) ||
            !IsExecutableMemory(stateVtable[kD3DStateBlockApplyVtableIndex]))
        {
            InterlockedIncrement(&s_mode4PackFailCount);
            return false;
        }
        const auto capture = reinterpret_cast<D3DStateBlockCapture_t>(
            stateVtable[kD3DStateBlockCaptureVtableIndex]);
        const auto apply = reinterpret_cast<D3DStateBlockApply_t>(
            stateVtable[kD3DStateBlockApplyVtableIndex]);

        const HRESULT captureHr = capture(s_mode4PackStateBlock);
        if (FAILED(captureHr))
        {
            s_mode4PackSetupHr = captureHr;
            InterlockedIncrement(&s_mode4PackFailCount);
            return false;
        }

        void* previousRT = nullptr;
        void* previousDepth = nullptr;
        const HRESULT getRTHr = s_rawGetRenderTarget(device, 0u, &previousRT);
        const HRESULT getDepthHr = s_rawGetDepthStencilSurface(device, &previousDepth);
        const bool depthStateKnown = SUCCEEDED(getDepthHr) || getDepthHr == kD3DErrNotFound;
        if (FAILED(getRTHr) || !previousRT || !depthStateKnown)
        {
            SafeReleaseCom(previousDepth);
            SafeReleaseCom(previousRT);
            s_mode4PackSetupHr = FAILED(getRTHr) ? getRTHr : E_FAIL;
            s_mode4PackRestoreHr = apply(s_mode4PackStateBlock);
            InterlockedIncrement(&s_mode4PackFailCount);
            return false;
        }

        HRESULT setupHr = S_OK;
        auto KeepFirstFailure = [&setupHr](HRESULT hr)
        {
            if (SUCCEEDED(setupHr) && FAILED(hr))
                setupHr = hr;
        };

        KeepFirstFailure(s_rawSetTexture(device, 0u, nullptr));
        KeepFirstFailure(s_rawSetTexture(device, 1u, nullptr));
        KeepFirstFailure(s_rawSetDepthStencilSurface(device, nullptr));
        KeepFirstFailure(s_rawSetRenderTarget(device, 0u, s_mode4PackedDepthSurface));

        if (SUCCEEDED(setupHr))
        {
            D3DViewportLite viewport = {};
            viewport.Width = sourceDesc.Width;
            viewport.Height = sourceDesc.Height;
            viewport.MinZ = 0.0f;
            viewport.MaxZ = 1.0f;
            KeepFirstFailure(s_rawSetViewport(device, &viewport));
            KeepFirstFailure(s_rawSetVertexShader(device, nullptr));
            KeepFirstFailure(s_rawSetFVF(device, kD3DFVFXYZRHW | kD3DFVFTex1));
            KeepFirstFailure(s_rawSetPixelShader(device, s_mode4PackedDepthPixelShader));
            KeepFirstFailure(s_rawSetTexture(device, 0u, s_mode4ReplacementDepthTexture));

            const float selectors[4][4] =
            {
                { 1.0f, 0.0f, 0.0f, 0.0f },
                { 0.0f, 1.0f, 0.0f, 0.0f },
                { 0.0f, 0.0f, 1.0f, 0.0f },
                { 0.0f, 0.0f, 0.0f, 1.0f }
            };
            uint32_t channel = (s_mode4PackChannel <= 3u) ? s_mode4PackChannel : 1u;
            if (s_mode4PackAutoCycle)
            {
                // Three long visual blocks: G -> B -> A. Advance only after
                // successful packed-depth draws so failures cannot desync the phase.
                static const uint32_t kAutoChannels[3] = { 1u, 2u, 3u };
                const uint32_t completed = static_cast<uint32_t>(
                    s_mode4PackApplyCount > 0 ? s_mode4PackApplyCount : 0);
                const uint32_t phase = (completed / kMode4PackAutoBlockFrames) % 3u;
                channel = kAutoChannels[phase];
            }
            s_mode4LastPackChannel = channel;
            KeepFirstFailure(s_rawSetPixelShaderConstantF(device, 0u, selectors[channel], 1u));

            KeepFirstFailure(s_rawSetRenderState(device, kRSZEnable, 0u));
            KeepFirstFailure(s_rawSetRenderState(device, kRSZWriteEnable, 0u));
            KeepFirstFailure(s_rawSetRenderState(device, kRSAlphaTestEnable, 0u));
            KeepFirstFailure(s_rawSetRenderState(device, kRSCullMode, 1u));
            KeepFirstFailure(s_rawSetRenderState(device, kRSAlphaBlendEnable, 0u));
            KeepFirstFailure(s_rawSetRenderState(device, kRSFogEnable, 0u));
            KeepFirstFailure(s_rawSetRenderState(device, kRSStencilEnable, 0u));
            KeepFirstFailure(s_rawSetRenderState(device, kRSLighting, 0u));
            KeepFirstFailure(s_rawSetRenderState(device, kRSColorWriteEnable, 0xFu));
            KeepFirstFailure(s_rawSetRenderState(device, kRSScissorTestEnable, 0u));
            KeepFirstFailure(s_rawSetRenderState(device, kRSSrgbWriteEnable, 0u));
            KeepFirstFailure(s_rawSetSamplerState(device, 0u, kSampAddressU, kTextureAddressClamp));
            KeepFirstFailure(s_rawSetSamplerState(device, 0u, kSampAddressV, kTextureAddressClamp));
            KeepFirstFailure(s_rawSetSamplerState(device, 0u, kSampMagFilter, kTextureFilterPoint));
            KeepFirstFailure(s_rawSetSamplerState(device, 0u, kSampMinFilter, kTextureFilterPoint));
            KeepFirstFailure(s_rawSetSamplerState(device, 0u, kSampMipFilter, kTextureFilterNone));
            KeepFirstFailure(s_rawSetSamplerState(device, 0u, kSampSrgbTexture, 0u));

            const float right = static_cast<float>(sourceDesc.Width) - 0.5f;
            const float bottom = static_cast<float>(sourceDesc.Height) - 0.5f;
            const ExposureVertex vertices[4] =
            {
                { -0.5f, -0.5f, 0.0f, 1.0f, 0.0f, 0.0f },
                { right, -0.5f, 0.0f, 1.0f, 1.0f, 0.0f },
                { -0.5f, bottom, 0.0f, 1.0f, 0.0f, 1.0f },
                { right, bottom, 0.0f, 1.0f, 1.0f, 1.0f }
            };
            if (SUCCEEDED(setupHr))
                s_mode4PackDrawHr = s_rawDrawPrimitiveUP(
                    device, kD3DPrimitiveTriangleStrip, 2u, vertices,
                    static_cast<UINT>(sizeof(ExposureVertex)));
            else
                s_mode4PackDrawHr = setupHr;
        }
        else
        {
            s_mode4PackDrawHr = setupHr;
        }

        s_rawSetTexture(device, 0u, nullptr);
        s_rawSetTexture(device, 1u, nullptr);
        const HRESULT restoreRTHr = s_rawSetRenderTarget(device, 0u, previousRT);
        const HRESULT restoreDepthHr = s_rawSetDepthStencilSurface(
            device, SUCCEEDED(getDepthHr) ? previousDepth : nullptr);
        const HRESULT stateApplyHr = apply(s_mode4PackStateBlock);
        SafeReleaseCom(previousDepth);
        SafeReleaseCom(previousRT);

        HRESULT restoreHr = S_OK;
        const HRESULT restoreResults[3] = { restoreRTHr, restoreDepthHr, stateApplyHr };
        for (uint32_t n = 0; n < 3u; ++n)
        {
            if (SUCCEEDED(restoreHr) && FAILED(restoreResults[n]))
                restoreHr = restoreResults[n];
        }
        s_mode4PackSetupHr = setupHr;
        s_mode4PackRestoreHr = restoreHr;

        const bool ok = SUCCEEDED(setupHr) && SUCCEEDED(s_mode4PackDrawHr) &&
            SUCCEEDED(restoreHr);
        const LONG count = ok
            ? InterlockedIncrement(&s_mode4PackApplyCount)
            : InterlockedIncrement(&s_mode4PackFailCount);

        const bool channelChanged = s_mode4LastPackChannel != s_mode4LastLoggedPackChannel;
        if (count <= 24 || (count % 120) == 0 || !ok || channelChanged)
        {
            Event* event = ReserveEvent(EventType::DepthPackProbe);
            if (event)
            {
                event->a = s_presentCount;
                event->b = static_cast<uint32_t>(count);
                event->c = ok ? 1u : 0u;
                event->d = s_mode4LastPackChannel;
                event->e = static_cast<uint32_t>(s_mode4PackCreateState);
                event->f = static_cast<uint32_t>(s_mode4PackCompileHr);
                event->g = static_cast<uint32_t>(s_mode4PackCreateTextureHr);
                event->h = static_cast<uint32_t>(s_mode4PackGetSurfaceHr);
                event->i = static_cast<uint32_t>(s_mode4PackCreateShaderHr);
                event->j = static_cast<uint32_t>(setupHr);
                event->extra[0] = static_cast<uint32_t>(s_mode4PackDrawHr);
                event->extra[1] = static_cast<uint32_t>(restoreHr);
                event->extra[2] = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(s_mode4ReplacementDepthTexture));
                event->extra[3] = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(s_mode4PackedDepthTexture));
                event->extra[4] = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(s_mode4PackedDepthSurface));
                event->extra[5] = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(s_mode4PackedDepthPixelShader));
                event->extra[6] = sourceDesc.Width;
                event->extra[7] = sourceDesc.Height;
                event->extra[8] = static_cast<uint32_t>(s_mode4PackApplyCount);
                event->extra[9] = static_cast<uint32_t>(s_mode4PackFailCount);
                event->extra[10] = s_mode4PackAutoCycle ? 1u : 0u;
                event->extra[11] = kMode4PackAutoBlockFrames;
                CopySmallText(event->text, sizeof(event->text), s_mode4PackCompilerText);
                s_mode4LastLoggedPackChannel = s_mode4LastPackChannel;
            }
        }
        return ok;
    }

    bool RunMode4IntzSampleReadbackSelfTest(void* device)
    {
        const LONG prior = InterlockedCompareExchange(&s_intzSampleReadbackState, 1, 0);
        if (prior != 0)
            return prior == 1;

        bool ok = false;
        void* previousRT = nullptr;
        void* previousDS = nullptr;
        void* stateBlock = nullptr;
        void* readbackSurface = nullptr;

        if (!device || !s_offscreenIntzTexture || !s_offscreenIntzSurface ||
            !s_fullResDepthSurface || s_offscreenIntzWidth == 0u || s_offscreenIntzHeight == 0u ||
            !s_rawGetRenderTarget || !s_rawGetDepthStencilSurface || !s_rawSetRenderTarget ||
            !s_rawSetDepthStencilSurface || !s_rawCreateStateBlock || !s_rawCreateOffscreenPlainSurface ||
            !s_rawGetRenderTargetData)
            goto done;

        if (FAILED(s_rawGetRenderTarget(device, 0u, &previousRT)) || !previousRT)
            goto done;
        {
            const HRESULT dsHr = s_rawGetDepthStencilSurface(device, &previousDS);
            if (FAILED(dsHr) && dsHr != kD3DErrNotFound)
                goto done;
        }

        if (FAILED(s_rawCreateStateBlock(device, kD3DStateBlockAll, &stateBlock)) || !stateBlock)
            goto done;

        {
            void** stateVtable = *reinterpret_cast<void***>(stateBlock);
            if (!stateVtable || !IsReadableMemory(stateVtable,
                (kD3DStateBlockApplyVtableIndex + 1u) * sizeof(void*)) ||
                !IsExecutableMemory(stateVtable[kD3DStateBlockCaptureVtableIndex]) ||
                !IsExecutableMemory(stateVtable[kD3DStateBlockApplyVtableIndex]))
                goto done;
            const auto capture = reinterpret_cast<D3DStateBlockCapture_t>(
                stateVtable[kD3DStateBlockCaptureVtableIndex]);
            const auto apply = reinterpret_cast<D3DStateBlockApply_t>(
                stateVtable[kD3DStateBlockApplyVtableIndex]);
            if (FAILED(capture(stateBlock)))
                goto done;

            HRESULT hr = s_rawSetRenderTarget(device, 0u, s_fullResDepthSurface);
            if (SUCCEEDED(hr))
                hr = s_rawSetDepthStencilSurface(device, s_offscreenIntzSurface);
            if (SUCCEEDED(hr))
            {
                void** deviceVtable = *reinterpret_cast<void***>(device);
                if (deviceVtable && IsReadableMemory(deviceVtable,
                    (kD3DClearVtableIndex + 1u) * sizeof(void*)) &&
                    IsExecutableMemory(deviceVtable[kD3DClearVtableIndex]))
                {
                    const auto clear = reinterpret_cast<D3DClear_t>(deviceVtable[kD3DClearVtableIndex]);
                    s_intzSampleClearHr = clear(device, 0u, nullptr, kD3DClearTarget | kD3DClearZBuffer,
                        0u, s_intzSampleExpected, 0u);
                }
                else
                    s_intzSampleClearHr = E_NOINTERFACE;
            }
            else
                s_intzSampleClearHr = hr;

            s_rawSetDepthStencilSurface(device, previousDS);
            s_rawSetRenderTarget(device, 0u, previousRT);
            apply(stateBlock);
        }

        if (FAILED(s_intzSampleClearHr))
            goto done;

        // Reuse the already-audited packed-depth diagnostic shader.  Override its
        // source globals only for this one call, then restore them immediately.
        {
            const bool oldEnabled = s_mode4PackedDepthEnabled;
            const bool oldAuto = s_mode4PackAutoCycle;
            const uint32_t oldChannel = s_mode4PackChannel;
            void* oldTex = s_mode4ReplacementDepthTexture;
            void* oldSurf = s_mode4ReplacementDepthSurface;
            const LONG oldApplied = s_mode4DepthReplacementAppliedCount;

            s_mode4PackedDepthEnabled = true;
            s_mode4PackAutoCycle = false;
            s_mode4PackChannel = 0u; // INTZ samples depth through R on D3D9 hardware.
            s_mode4ReplacementDepthTexture = s_offscreenIntzTexture;
            s_mode4ReplacementDepthSurface = s_offscreenIntzSurface;
            s_mode4DepthReplacementAppliedCount = 1;

            const bool packed = ApplyMode4PackedDepthPass(device);
            s_intzSamplePackHr = packed ? S_OK : E_FAIL;

            s_mode4ReplacementDepthTexture = oldTex;
            s_mode4ReplacementDepthSurface = oldSurf;
            s_mode4DepthReplacementAppliedCount = oldApplied;
            s_mode4PackChannel = oldChannel;
            s_mode4PackAutoCycle = oldAuto;
            s_mode4PackedDepthEnabled = oldEnabled;
        }

        if (FAILED(s_intzSamplePackHr) || !s_mode4PackedDepthSurface)
            goto done;

        s_intzSampleCreateReadbackHr = s_rawCreateOffscreenPlainSurface(
            device, s_offscreenIntzWidth, s_offscreenIntzHeight, kFormatA8R8G8B8,
            kD3DPoolSystemMem, &readbackSurface, nullptr);
        if (FAILED(s_intzSampleCreateReadbackHr) || !readbackSurface)
            goto done;

        s_intzSampleGetDataHr = s_rawGetRenderTargetData(
            device, s_mode4PackedDepthSurface, readbackSurface);
        if (FAILED(s_intzSampleGetDataHr))
            goto done;

        {
            void** vt = *reinterpret_cast<void***>(readbackSurface);
            if (!vt || !IsReadableMemory(vt,
                (kD3DSurfaceUnlockRectVtableIndex + 1u) * sizeof(void*)) ||
                !IsExecutableMemory(vt[kD3DSurfaceLockRectVtableIndex]) ||
                !IsExecutableMemory(vt[kD3DSurfaceUnlockRectVtableIndex]))
                goto done;
            const auto lockRect = reinterpret_cast<D3DSurfaceLockRect_t>(
                vt[kD3DSurfaceLockRectVtableIndex]);
            const auto unlockRect = reinterpret_cast<D3DSurfaceUnlockRect_t>(
                vt[kD3DSurfaceUnlockRectVtableIndex]);
            D3DLockedRectLite locked = {};
            s_intzSampleLockHr = lockRect(readbackSurface, &locked, nullptr, kD3DLockReadOnly);
            if (FAILED(s_intzSampleLockHr) || !locked.pBits || locked.Pitch <= 0)
                goto done;

            const uint32_t x = s_offscreenIntzWidth / 2u;
            const uint32_t y = s_offscreenIntzHeight / 2u;
            const uint8_t* pixel = reinterpret_cast<const uint8_t*>(locked.pBits) +
                static_cast<size_t>(y) * static_cast<size_t>(locked.Pitch) + x * 4u;
            memcpy(&s_intzSamplePackedBGRA, pixel, sizeof(uint32_t));
            const uint32_t g = pixel[1];
            const uint32_t r = pixel[2];
            const uint32_t a = pixel[3];
            s_intzSampleReconstructed =
                static_cast<float>(a) / 256.0f +
                static_cast<float>(r) / 65536.0f +
                static_cast<float>(g) / 16777216.0f;
            s_intzSampleUnlockHr = unlockRect(readbackSurface);
            if (FAILED(s_intzSampleUnlockHr))
                goto done;
        }

        ok = fabsf(s_intzSampleReconstructed - s_intzSampleExpected) < 0.01f;

    done:
        SafeReleaseCom(readbackSurface);
        SafeReleaseCom(stateBlock);
        SafeReleaseCom(previousDS);
        SafeReleaseCom(previousRT);
        if (!ok)
            InterlockedExchange(&s_intzSampleReadbackState, -1);

        Event* event = ReserveEvent(EventType::IntzSampleReadbackProbe);
        if (event)
        {
            event->a = s_presentCount;
            event->b = ok ? 1u : 0u;
            event->c = s_offscreenIntzWidth;
            event->d = s_offscreenIntzHeight;
            event->e = static_cast<uint32_t>(s_intzSampleClearHr);
            event->f = static_cast<uint32_t>(s_intzSamplePackHr);
            event->g = static_cast<uint32_t>(s_intzSampleCreateReadbackHr);
            event->h = static_cast<uint32_t>(s_intzSampleGetDataHr);
            event->i = static_cast<uint32_t>(s_intzSampleLockHr);
            event->j = static_cast<uint32_t>(s_intzSampleUnlockHr);
            event->extra[0] = s_intzSamplePackedBGRA;
            memcpy(&event->extra[1], &s_intzSampleExpected, sizeof(float));
            memcpy(&event->extra[2], &s_intzSampleReconstructed, sizeof(float));
            event->extra[3] = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(s_offscreenIntzTexture));
            event->extra[4] = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(s_mode4PackedDepthTexture));
            event->extra[5] = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(s_mode4PackedDepthSurface));
        }
        return ok;
    }

    void QueueProvenIntzDofActivationEvent(uint32_t applied)
    {
        if (applied != 0u && s_provenIntzDofWindowApplied > 1u)
            return;
        if (applied == 0u &&
            !ShouldLogSparse(static_cast<LONG>(s_provenIntzDofWindowFailures)))
            return;
        Event* event = ReserveEvent(EventType::ProvenIntzDofActivation);
        if (!event)
            return;
        event->a = s_presentCount;
        event->b = applied;
        event->c = s_provenIntzDofColorDescriptor;
        event->d = s_provenIntzDofDepthTexture;
        event->e = s_provenIntzDofShader;
        event->f = static_cast<uint32_t>(s_provenIntzDofBindHr);
        event->g = static_cast<uint32_t>(s_provenIntzDofConstHr);
        event->h = static_cast<uint32_t>(s_provenIntzDofPsHr);
        event->i = static_cast<uint32_t>(s_provenIntzDofSrcBlendHr);
        event->j = static_cast<uint32_t>(s_provenIntzDofDstBlendHr);
        event->extra[0] = s_provenIntzDofPrevStage1;
        event->extra[1] = s_provenIntzDofPrevPS;
        event->extra[2] = static_cast<uint32_t>(s_provenIntzDofRestoreTexHr);
        event->extra[3] = static_cast<uint32_t>(s_provenIntzDofRestorePsHr);
        event->extra[4] = static_cast<uint32_t>(s_provenIntzDofRestoreSrcBlendHr);
        event->extra[5] = static_cast<uint32_t>(s_provenIntzDofRestoreDstBlendHr);
        event->extra[6] = s_provenIntzDofPresent;
        event->extra[7] = kMode4ReszProbeEnabled ? 0u : s_opaqueIntzTraversalFinalIndex;
        event->extra[8] = kMode4ReszProbeEnabled ? 0u : s_opaqueIntzReplayDeclared;
        event->extra[9] = s_opaqueIntzChanged;
        event->extra[10] = s_provenIntzDofWindowFrameOrdinal;
        event->extra[11] = s_provenIntzDofWindowProduced;
        event->extra[12] = s_provenIntzDofWindowApplied;
        event->extra[13] = s_provenIntzDofWindowFailures;
        event->extra[14] = s_provenIntzDofWindowStartPresent;
        event->extra[15] = s_provenIntzDofWindowEndPresent;
    }

    void QueueMode4DofC0BridgeProbe(bool dynamicValid)
    {
        const LONG fallbackCount = s_mode4C0BridgeFallbackCount;
        if (dynamicValid)
        {
            if (InterlockedCompareExchange(&s_mode4C0BridgeLoggedSuccess, 1, 0) != 0)
                return;
        }
        else if (!ShouldLogSparse(fallbackCount))
        {
            return;
        }

        Event* event = ReserveEvent(EventType::DofC0BridgeProbe);
        if (!event)
            return;

        event->a = s_presentCount;
        event->b = dynamicValid ? 1u : 0u;
        event->c = FloatBits(s_mode4LastPcNearPlane);
        event->d = FloatBits(s_mode4LastPcFarPlane);
        event->e = FloatBits(s_mode4LastBloomDepthControl[0]);
        event->f = FloatBits(s_mode4LastBloomDepthControl[1]);
        event->g = FloatBits(s_mode4LastBloomDepthControl[2]);
        event->h = FloatBits(s_mode4LastBloomDepthControl[3]);
        event->i = static_cast<uint32_t>(s_mode4C0BridgeBuildCount);
        event->j = static_cast<uint32_t>(fallbackCount);
        event->extra[0] = ReadU32(kPcPostFxNearPlane);
        event->extra[1] = ReadU32(kPcPostFxFarPlane);
    }

    bool ApplyProvenIntzPackedDofFinalState(uint32_t colorDescriptor)
    {
        if (s_v10Mode != 4 || !s_mode4MidProbeOnly ||
            s_presentCount != s_provenIntzDofPresent ||
            s_provenIntzDofState != 1 || !s_mode4PackedDepthTexture ||
            !s_rawSetTexture || !s_rawSetPixelShader || !s_rawSetPixelShaderConstantF ||
            !s_rawSetRenderState || !s_setSamplerAddressUV || !s_setSamplerFilterState ||
            !Mode4FullDofFollowupReady())
            return false;

        if (InterlockedCompareExchange(&s_provenIntzDofState, 2, 1) != 1)
            return false;

        void* device = reinterpret_cast<void*>(static_cast<uintptr_t>(ReadU32(kD3DDeviceGlobal)));
        const uint32_t dofShader = ReadU32(kCompositePSDofFinal);
        const uint32_t packedDepth = static_cast<uint32_t>(
            reinterpret_cast<uintptr_t>(s_mode4PackedDepthTexture));
        const uintptr_t stage1CacheAddress = kNglTextureCacheBase + sizeof(uint32_t);

        s_provenIntzDofColorDescriptor = colorDescriptor;
        s_provenIntzDofDepthTexture = packedDepth;
        s_provenIntzDofShader = dofShader;
        s_provenIntzDofPrevStage1 = s_rawTextureStages[1];
        s_provenIntzDofPrevStage1Cache = ReadU32(stage1CacheAddress);
        s_provenIntzDofPrevPS = s_rawCurrentPixelShader;
        s_provenIntzDofPrevPSCache = ReadU32(kActivePSCache);
        s_provenIntzDofBindHr = E_PENDING;
        s_provenIntzDofConstHr = E_PENDING;
        s_provenIntzDofPsHr = E_PENDING;
        s_provenIntzDofSrcBlendHr = E_PENDING;
        s_provenIntzDofDstBlendHr = E_PENDING;
        s_provenIntzDofRestoreTexHr = E_PENDING;
        s_provenIntzDofRestorePsHr = E_PENDING;
        s_provenIntzDofRestoreSrcBlendHr = E_PENDING;
        s_provenIntzDofRestoreDstBlendHr = E_PENDING;

        bool ok = device && dofShader >= 0x10000u && packedDepth >= 0x10000u &&
            IsReadableMemory(reinterpret_cast<void*>(static_cast<uintptr_t>(dofShader)), sizeof(void*));
        if (ok)
        {
            s_provenIntzDofBindHr = s_rawSetTexture(device, 1u, s_mode4PackedDepthTexture);
            ok = SUCCEEDED(s_provenIntzDofBindHr);
        }
        if (ok)
        {
            s_rawTextureStages[1] = packedDepth;
            WriteU32(stage1CacheAddress, packedDepth);
            s_setSamplerAddressUV(1, 3, 3);
            s_setSamplerFilterState(1, 1, 1, 1, 1);

            float bloomDepthControl[4] = {};
            const bool dynamicC0Valid = BuildMode4BloomDepthControl(bloomDepthControl);
            QueueMode4DofC0BridgeProbe(dynamicC0Valid);
            s_provenIntzDofConstHr = s_rawSetPixelShaderConstantF(
                device, 0u, bloomDepthControl, 1u);
            ok = SUCCEEDED(s_provenIntzDofConstHr);
        }
        if (ok)
        {
            s_provenIntzDofPsHr = s_rawSetPixelShader(
                device, reinterpret_cast<void*>(static_cast<uintptr_t>(dofShader)));
            ok = SUCCEEDED(s_provenIntzDofPsHr);
        }
        if (ok)
        {
            // V10.5.49 FULL NATIVE-STYLE DOF CHAIN.
            //
            // Xbox static analysis proved the dormant BloomFinalCombineDOF draw
            // is PASS 1, not the final visible blur.  It adds the bloom/color
            // contribution while replacing destination alpha with the depth-based
            // DOF mask.  The later Gaussian + 7/8 DstAlpha composite consumes
            // that mask.  V10.5.45 white-out happened because this first-pass
            // state was run without the required follow-up chain.
            //
            // Keep RESZ, packed s1, live c0, s0 and 010FBF18 frozen; change only
            // the pass topology and attach the recovered follow-up stages.
            if (s_nglSetBlendState)
            {
                // V10.5.49.3 PC-COMPAT MASK PASS.
                //
                // Xbox uses ONE/ONE RGB here, but the PC quarter-bloom buffer is
                // calibrated for retail PC's DESTALPHA/INVDESTALPHA final combine.
                // Reusing Xbox additive RGB on the PC buffer caused the severe
                // white/hazy gameplay seen in the V10.5.49.2 capture.
                //
                // Keep PC's proven stock RGB blend (7/8) while independently
                // replacing alpha with F18's depth mask (ONE/ZERO = 2/1).
                // This preserves stock PC brightness while still feeding the
                // recovered mask to PASS 4.  It is deliberately a PC-compat
                // translation, not a claim that Xbox used 7/8 in PASS 1.
                s_nglSetBlendState(7u, 8u, 0u, 2u, 1u, 0u, 1u, 1u);
                s_provenIntzDofSrcBlendHr = S_OK;
                s_provenIntzDofDstBlendHr = S_OK;
            }
            else
            {
                s_provenIntzDofSrcBlendHr = E_FAIL;
                s_provenIntzDofDstBlendHr = E_FAIL;
                ok = false;
            }
        }
        if (ok)
        {
            s_rawCurrentPixelShader = dofShader;
            WriteU32(kActivePSCache, dofShader);
            return true;
        }

        // Fail closed before the final draw.
        if (device)
        {
            s_provenIntzDofRestoreTexHr = s_rawSetTexture(device, 1u, reinterpret_cast<void*>(
                static_cast<uintptr_t>(s_provenIntzDofPrevStage1)));
            s_rawTextureStages[1] = s_provenIntzDofPrevStage1;
            WriteU32(stage1CacheAddress, s_provenIntzDofPrevStage1Cache);
            s_provenIntzDofRestorePsHr = s_rawSetPixelShader(device,
                reinterpret_cast<void*>(static_cast<uintptr_t>(s_provenIntzDofPrevPS)));
            s_rawCurrentPixelShader = s_provenIntzDofPrevPS;
            WriteU32(kActivePSCache, s_provenIntzDofPrevPSCache);
            if (s_nglSetBlendState)
            {
                // Exact stock 00797A70 final-composite state.
                s_nglSetBlendState(7u, 8u, 0u, 1u, 1u, 0u, 1u, 1u);
                s_provenIntzDofRestoreSrcBlendHr = S_OK;
                s_provenIntzDofRestoreDstBlendHr = S_OK;
            }
            else
            {
                s_provenIntzDofRestoreSrcBlendHr = E_FAIL;
                s_provenIntzDofRestoreDstBlendHr = E_FAIL;
            }
        }
        InterlockedExchange(&s_provenIntzDofState, -1);
        ++s_provenIntzDofWindowFailures;
        QueueProvenIntzDofActivationEvent(0u);
        return false;
    }

    void RestoreProvenIntzPackedDofFinalState()
    {
        if (s_provenIntzDofState != 2)
            return;

        void* device = reinterpret_cast<void*>(static_cast<uintptr_t>(ReadU32(kD3DDeviceGlobal)));
        const uintptr_t stage1CacheAddress = kNglTextureCacheBase + sizeof(uint32_t);
        if (device)
        {
            s_provenIntzDofRestoreTexHr = s_rawSetTexture(device, 1u,
                reinterpret_cast<void*>(static_cast<uintptr_t>(s_provenIntzDofPrevStage1)));
            s_rawTextureStages[1] = s_provenIntzDofPrevStage1;
            WriteU32(stage1CacheAddress, s_provenIntzDofPrevStage1Cache);

            s_provenIntzDofRestorePsHr = s_rawSetPixelShader(device,
                reinterpret_cast<void*>(static_cast<uintptr_t>(s_provenIntzDofPrevPS)));
            s_rawCurrentPixelShader = s_provenIntzDofPrevPS;
            WriteU32(kActivePSCache, s_provenIntzDofPrevPSCache);
            if (s_nglSetBlendState)
            {
                // Exact stock 00797A70 final-composite state.
                s_nglSetBlendState(7u, 8u, 0u, 1u, 1u, 0u, 1u, 1u);
                s_provenIntzDofRestoreSrcBlendHr = S_OK;
                s_provenIntzDofRestoreDstBlendHr = S_OK;
            }
            else
            {
                s_provenIntzDofRestoreSrcBlendHr = E_FAIL;
                s_provenIntzDofRestoreDstBlendHr = E_FAIL;
            }
        }
        else
        {
            s_provenIntzDofRestoreTexHr = E_FAIL;
            s_provenIntzDofRestorePsHr = E_FAIL;
            s_provenIntzDofRestoreSrcBlendHr = E_FAIL;
            s_provenIntzDofRestoreDstBlendHr = E_FAIL;
        }

        ++s_provenIntzDofWindowApplied;
        QueueProvenIntzDofActivationEvent(1u);
        InterlockedExchange(&s_provenIntzDofState, 3);
    }

    bool ApplyMode4DofFinalState(uint32_t colorDescriptor)
    {
        const LONG attempt = InterlockedIncrement(&s_mode4AttemptCount);
        s_mode4LastApplied = false;
        s_mode4LastColorDescriptor = colorDescriptor;
        s_mode4LastSimpleShader = ReadU32(kCompositePSFinal);
        s_mode4LastActivePSBefore = s_rawCurrentPixelShader;
        s_mode4LastActivePSAfter = s_rawCurrentPixelShader;
        s_mode4LastConstantHr = E_PENDING;
        s_mode4LastPixelShaderHr = E_PENDING;
        s_mode4LastDepthBindHr = E_PENDING;
        s_mode4LastDepthTexture = 0u;
        s_mode4LastDepthCacheBefore = 0u;
        s_mode4LastDepthCacheAfter = 0u;
        s_mode4LastRawStage1After = s_rawTextureStages[1];

        uint32_t readyMask = 0u;
        if (s_v10Mode == 4)
            readyMask |= 1u << 0;

        void* device = reinterpret_cast<void*>(static_cast<uintptr_t>(ReadU32(kD3DDeviceGlobal)));
        if (device && IsReadableMemory(device, sizeof(void*)))
            readyMask |= 1u << 1;

        if (s_rawGetDepthStencilSurface != nullptr)
            readyMask |= 1u << 2;

        const uint32_t dofShader = ReadU32(kCompositePSDofFinal);
        s_mode4LastDofShader = dofShader;
        if (dofShader >= 0x10000u && IsReadableMemory(
            reinterpret_cast<void*>(static_cast<uintptr_t>(dofShader)), sizeof(void*)))
            readyMask |= 1u << 3;

        const bool depthDescriptorReadable = IsReadableMemory(
            reinterpret_cast<void*>(kPostFxDepthDescriptor), 0x20u);
        if (depthDescriptorReadable)
        {
            readyMask |= 1u << 4;
            s_mode4LastDepthDescriptorFlags = ReadU32(kPostFxDepthDescriptor + 0x1Cu);
        }

        uint32_t colorTexture = s_rawTextureStages[0];
        if (colorTexture < 0x10000u)
            colorTexture = RawTextureFromNglDescriptor(colorDescriptor);
        s_mode4LastColorTexture = colorTexture;
        if (colorTexture >= 0x10000u)
            readyMask |= 1u << 5;

        bool c0Finite = true;
        for (float value : s_mode4LastBloomDepthControl)
            c0Finite = c0Finite && IsFiniteUsefulFloat(value);
        if (c0Finite)
            readyMask |= 1u << 7;

        const uint32_t sceneRT = CurrentSceneRT();
        if (sceneRT >= 0x10000u)
            readyMask |= 1u << 11;

        // V10.5.11 keeps the safe D24S8 world-depth replacement, then repacks
        // one sampled hardware channel into the A/R/G byte layout expected by
        // the dormant DOF shader. Raw D24 is no longer fed directly to s1.
        const uint32_t replacementTexture = static_cast<uint32_t>(
            reinterpret_cast<uintptr_t>(s_mode4ReplacementDepthTexture));
        const uint32_t replacementSurface = static_cast<uint32_t>(
            reinterpret_cast<uintptr_t>(s_mode4ReplacementDepthSurface));

        bool packedDepthReady = false;
        if (replacementTexture >= 0x10000u && replacementSurface >= 0x10000u &&
            s_mode4DepthReplacementCreateState == 1 &&
            s_mode4DepthReplacementAppliedCount > 0)
        {
            readyMask |= 1u << 13;
            packedDepthReady = ApplyMode4PackedDepthPass(device);
            if (packedDepthReady)
            {
                readyMask |= 1u << 12;
                s_mode4LastDepthTexture = static_cast<uint32_t>(
                    reinterpret_cast<uintptr_t>(s_mode4PackedDepthTexture));
            }
        }

        // Keep the old final-time depth probe as telemetry only. It should still
        // report D3DERR_NOTFOUND because SM3 intentionally unbinds depth before
        // post processing. That no longer blocks Mode 4 because we retain the
        // owning texture from the earlier replacement.
        ProbeMode4DepthSource(device);

        if (s_mode4CapturedDepthSurface >= 0x10000u)
            readyMask |= 1u << 10;

        const bool canApply =
            (readyMask & (1u << 0)) &&
            (readyMask & (1u << 1)) &&
            (readyMask & (1u << 3)) &&
            (readyMask & (1u << 5)) &&
            (readyMask & (1u << 7)) &&
            (readyMask & (1u << 11)) &&
            (readyMask & (1u << 12)) &&
            (readyMask & (1u << 13)) &&
            s_rawSetTexture &&
            s_rawSetPixelShader &&
            s_rawSetPixelShaderConstantF;

        if (canApply)
        {
            const uint32_t previousStage0 = s_rawTextureStages[0];
            const uint32_t previousStage1 = s_rawTextureStages[1];
            const uint32_t previousPS = s_rawCurrentPixelShader;
            const uintptr_t stage0CacheAddress = kNglTextureCacheBase;
            const uintptr_t stage1CacheAddress = kNglTextureCacheBase + sizeof(uint32_t);

            s_mode4LastDepthCacheBefore = ReadU32(stage1CacheAddress);

            // V10.5.48: CTAB-LOCKED FINAL INPUT ROUTING (legacy non-MID path).
            // The shipped dormant BloomFinalCombineDOF shader declares exactly:
            //   s0=colorSampler, s1=depthSampler, c0=bloomDepthControl.
            // Retail PC's final simple combine binds DAT_00E8FCA4 as its color
            // source at the exact final fullscreen draw. Do not depend on whatever
            // raw stage-0 state happened to survive earlier passes: explicitly bind
            // the descriptor-derived final color texture to s0 and keep NGL's cache
            // coherent. No s2/dither input is invented for this shader.
            HRESULT colorBindHr = E_FAIL;
            if (colorTexture >= 0x10000u)
            {
                colorBindHr = s_rawSetTexture(
                    device, 0u,
                    reinterpret_cast<void*>(static_cast<uintptr_t>(colorTexture)));
                if (SUCCEEDED(colorBindHr))
                {
                    s_rawTextureStages[0] = colorTexture;
                    WriteU32(stage0CacheAddress, colorTexture);
                    s_setSamplerAddressUV(0, 3, 3);
                    s_setSamplerFilterState(0, 2, 2, 2, 1);
                }
            }

            // s1 = texture-backed scene depth. Keep NGL's cache coherent because
            // this is a direct D3D bind rather than a normal NGL descriptor bind.
            s_mode4LastDepthBindHr = SUCCEEDED(colorBindHr)
                ? s_rawSetTexture(device, 1u, s_mode4PackedDepthTexture)
                : E_FAIL;
            if (SUCCEEDED(s_mode4LastDepthBindHr))
            {
                const uint32_t packedTexture = static_cast<uint32_t>(
                    reinterpret_cast<uintptr_t>(s_mode4PackedDepthTexture));
                s_rawTextureStages[1] = packedTexture;
                WriteU32(stage1CacheAddress, packedTexture);

                // Reuse the stock PC God Rays depth sampler policy.
                s_setSamplerAddressUV(1, 3, 3);
                s_setSamplerFilterState(1, 1, 1, 1, 1);

                // Xbox retail BloomFinalCombineDOF contract:
                // c0=bloomDepthControl, s0=colorSampler, s1=depthSampler.
                float bloomDepthControl[4] = {};
                BuildMode4BloomDepthControl(bloomDepthControl);
                s_mode4LastConstantHr = s_rawSetPixelShaderConstantF(
                    device, 0u, bloomDepthControl, 1u);

                if (SUCCEEDED(s_mode4LastConstantHr))
                {
                    void* dofShaderPtr = reinterpret_cast<void*>(
                        static_cast<uintptr_t>(dofShader));
                    s_mode4LastPixelShaderHr = s_rawSetPixelShader(device, dofShaderPtr);

                    if (SUCCEEDED(s_mode4LastPixelShaderHr))
                    {
                        bool blendReady = true;
                        if (s_mode4BlendDiagnostic)
                        {
                            // DIAGNOSTIC ONLY. Stock PC final combine uses
                            // SRCBLEND=DESTALPHA (7), DESTBLEND=INVDESTALPHA (8).
                            // The dormant DOF shader computes a depth-dependent
                            // source alpha, so test SRCALPHA/INVSRCALPHA (5/6)
                            // for this one final fullscreen draw. This is not yet
                            // claimed as the retail Xbox blend state.
                            blendReady = s_rawSetRenderState &&
                                SUCCEEDED(s_rawSetRenderState(device, kRSSrcBlend, 5u)) &&
                                SUCCEEDED(s_rawSetRenderState(device, kRSDestBlend, 6u));
                            if (blendReady)
                                readyMask |= 1u << 15;
                        }

                        if (blendReady)
                        {
                            s_rawCurrentPixelShader = dofShader;
                            WriteU32(kActivePSCache, dofShader);
                            s_mode4LastApplied = true;
                            s_mode4LastActivePSAfter = dofShader;
                            readyMask |= 1u << 14;
                            InterlockedIncrement(&s_mode4ApplyCount);
                        }
                    }
                }
            }

            if (!s_mode4LastApplied)
            {
                // Fail-safe before the stock fullscreen draw: restore the exact
                // stage-0/stage-1 textures and pixel shader observed before this attempt.
                s_rawSetTexture(
                    device, 0u,
                    reinterpret_cast<void*>(static_cast<uintptr_t>(previousStage0)));
                s_rawTextureStages[0] = previousStage0;
                WriteU32(stage0CacheAddress, previousStage0);

                s_rawSetTexture(
                    device, 1u,
                    reinterpret_cast<void*>(static_cast<uintptr_t>(previousStage1)));
                s_rawTextureStages[1] = previousStage1;
                WriteU32(stage1CacheAddress, previousStage1);

                if (previousPS >= 0x10000u)
                {
                    s_rawSetPixelShader(
                        device,
                        reinterpret_cast<void*>(static_cast<uintptr_t>(previousPS)));
                    s_rawCurrentPixelShader = previousPS;
                    WriteU32(kActivePSCache, previousPS);
                    s_mode4LastActivePSAfter = previousPS;
                }

                InterlockedIncrement(&s_mode4FallbackCount);
            }

            s_mode4LastDepthCacheAfter = ReadU32(stage1CacheAddress);
            s_mode4LastRawStage1After = s_rawTextureStages[1];
        }
        else
        {
            InterlockedIncrement(&s_mode4FallbackCount);
        }

        s_mode4LastReadyMask = readyMask;

        const bool stateChanged = readyMask != s_mode4LastLoggedReadyMask;
        const bool appliedChanged = s_mode4LastApplied != s_mode4LastLoggedApplied;
        if (attempt <= 20 || (attempt % 120) == 0 || stateChanged || appliedChanged)
        {
            s_mode4LastLoggedReadyMask = readyMask;
            s_mode4LastLoggedApplied = s_mode4LastApplied;

            Event* event = ReserveEvent(EventType::DofFinalState);
            if (event)
            {
                event->a = s_presentCount;
                event->b = static_cast<uint32_t>(s_currentCompositeCall);
                event->c = static_cast<uint32_t>(attempt);
                event->d = static_cast<uint32_t>(s_v10Mode);
                event->e = readyMask;
                event->f = s_mode4LastApplied ? 1u : 0u;
                event->g = colorDescriptor;
                event->h = colorTexture;
                event->i = s_mode4LastDepthTexture;
                event->j = dofShader;
                event->extra[0] = s_mode4LastSimpleShader;
                event->extra[1] = s_mode4LastActivePSBefore;
                event->extra[2] = s_mode4LastActivePSAfter;
                event->extra[3] = static_cast<uint32_t>(s_mode4LastConstantHr);
                event->extra[4] = static_cast<uint32_t>(s_mode4LastPixelShaderHr);
                event->extra[5] = FloatBits(s_mode4LastBloomDepthControl[0]);
                event->extra[6] = FloatBits(s_mode4LastBloomDepthControl[1]);
                event->extra[7] = FloatBits(s_mode4LastBloomDepthControl[2]);
                event->extra[8] = FloatBits(s_mode4LastBloomDepthControl[3]);
                event->extra[9] = sceneRT;
                event->extra[10] = static_cast<uint32_t>(s_mode4ApplyCount);
                event->extra[11] = static_cast<uint32_t>(s_mode4FallbackCount);
                event->extra[12] = static_cast<uint32_t>(s_mode4LastDepthBindHr);
                event->extra[13] = s_mode4LastDepthCacheBefore;
                event->extra[14] = s_mode4LastDepthCacheAfter;
                event->extra[15] = s_mode4LastRawStage1After;
            }
        }

        return s_mode4LastApplied;
    }

    void __cdecl BloomCallback_Hook()
    {
        const LONG count = InterlockedIncrement(&s_bloomCallbackCount);
        if (ShouldLogSparse(count))
        {
            Event* event = ReserveEvent(EventType::BloomCallback);
            if (event)
            {
                event->a = static_cast<uint32_t>(count);
                event->b = ReadU8(kAutoExposureEnabled);
                event->c = ReadU32(kRtQuarterA);
                event->d = ReadU32(kRtQuarterB);
                event->e = ReadU32(kRt160x120);
                event->f = ReadU32(kRtLuminance16x12);
                event->g = ReadU32(kRtSecond16x12);
                event->h = CurrentSceneRT();
            }
        }
        s_originalBloomCallback();
    }

    int __cdecl BloomComposite_Hook()
    {
        const uintptr_t wrapperReturnAddress =
            reinterpret_cast<uintptr_t>(_ReturnAddress());
        if (s_v10Mode == 4)
        {
            s_bloomCompositeLateCallsiteLastReturn =
                static_cast<uint32_t>(wrapperReturnAddress);
            const LONG lateCallsiteHits =
                InterlockedIncrement(&s_bloomCompositeLateCallsiteHits);

            // Emit sparse live proof in addition to the one-time install event.
            // This lets the test log prove the patched wrapper is actually being
            // reached from 0079618A and returning to 0079618F during gameplay.
            if (ShouldLogSparse(lateCallsiteHits))
            {
                Event* event = ReserveEvent(EventType::BloomCompositeCallsitePatchStatus);
                if (event)
                {
                    uint8_t current[5] = {};
                    const uint8_t* site = reinterpret_cast<const uint8_t*>(
                        kBloomCompositeLateCallsiteAddress);
                    if (IsReadableMemory(site, sizeof(current)))
                        memcpy(current, site, sizeof(current));
                    event->a = static_cast<uint32_t>(s_bloomCompositeLateCallsitePatchState);
                    event->b = static_cast<uint32_t>(kBloomCompositeLateCallsiteAddress);
                    event->c = static_cast<uint32_t>(kBloomBlurCompositeAddress);
                    event->d = static_cast<uint32_t>(kBloomCompositeLateCallsiteReturn);
                    event->e = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&BloomComposite_Hook));
                    event->f = s_bloomCompositeLateCallsiteOriginalBytes[0];
                    event->g = s_bloomCompositeLateCallsiteOriginalBytes[1];
                    event->h = s_bloomCompositeLateCallsiteOriginalBytes[2];
                    event->i = s_bloomCompositeLateCallsiteOriginalBytes[3];
                    event->j = s_bloomCompositeLateCallsiteOriginalBytes[4];
                    event->extra[0] = current[0];
                    event->extra[1] = current[1];
                    event->extra[2] = current[2];
                    event->extra[3] = current[3];
                    event->extra[4] = current[4];
                    event->extra[5] = static_cast<uint32_t>(lateCallsiteHits);
                    event->extra[6] = static_cast<uint32_t>(wrapperReturnAddress);
                }
            }

            // Fail closed if this wrapper is ever reached from a different
            // Mode-4 caller. The September 19 proof is specifically
            // 0079618A -> 00797A70 -> 0079618F. Stock 00797A70 still runs,
            // but xeSM3 will not append the recovered bloom/DOF tail from an
            // unproven timing context.
            if (wrapperReturnAddress != kBloomCompositeLateCallsiteReturn)
                return s_originalBloomComposite();
        }

        const LONG count = InterlockedIncrement(&s_compositeCount);
        s_insideComposite = true;
        s_currentCompositeCall = count;
        s_thisCompositeBlur1Routed = false;
        s_thisCompositeV10Scaled = false;
        s_thisCompositeNativeTailLevel1 = false;
        s_thisCompositeNativeTailLevel2 = false;
        s_nativeBloomTailLevel1PsHr = E_PENDING;
        s_nativeBloomTailLevel1C0Hr = E_PENDING;
        s_nativeBloomTailLevel1RestorePsHr = E_PENDING;
        s_nativeBloomTailLevel1RestoreC0Hr = E_PENDING;
        s_nativeBloomTailLevel2CaptureHr = E_PENDING;
        s_nativeBloomTailLevel2SetupHr = E_PENDING;
        s_nativeBloomTailLevel2DrawHr = E_PENDING;
        s_nativeBloomTailLevel2RestoreHr = E_PENDING;
        s_nativeBloomTailFallbackGaussianHr = E_PENDING;
        memset(s_nativeBloomTailLevel1C0, 0, sizeof(s_nativeBloomTailLevel1C0));
        memset(s_nativeBloomTailLevel2C0, 0, sizeof(s_nativeBloomTailLevel2C0));

        if (ShouldLogSparse(count))
        {
            Event* event = ReserveEvent(EventType::CompositeEnter);
            if (event)
            {
                event->a = static_cast<uint32_t>(count);
                event->b = ReadU32(kRtQuarterA);
                event->c = ReadU32(kRtQuarterB);
                event->d = CurrentSceneRT();
                event->e = ReadU32(kCompositePSInitial);
                event->f = ReadU32(kCompositePSBlur);
                event->g = ReadU32(kCompositePSFinal);
                event->h = ReadU32(kCompositeVS);
                event->i = ReadU32(kActivePSCache);
                event->j = ReadU32(kActiveVSCache);
            }
        }

        const int result = s_originalBloomComposite();

        // V10.5.49: the stock final draw above is now PASS 1 of the recovered
        // Xbox-proven chain, translated for PC compatibility. F18 still replaces
        // alpha with the depth mask, but PASS-1 RGB now keeps PC's stock 7/8
        // combine instead of Xbox ONE/ONE to avoid the PC bloom-buffer blowout.
        // Before restoring the tracked PS/stage-1 state, rebuild the follow-up
        // scene->quarter + Gaussian passes.  Then restore the stock simple final
        // shader and 7/8 state and issue one extra fullscreen draw so destination
        // alpha (the depth mask) controls the blurred-scene composite.
        if (s_v10Mode == 4 && s_mode4MidProbeOnly)
        {
            const bool hadFirstPass = (s_provenIntzDofState == 2);
            bool followupOk = false;
            bool finalOk = false;
            if (hadFirstPass)
                followupOk = RunMode4FullDofFollowupBlur();

            RestoreProvenIntzPackedDofFinalState();

            if (hadFirstPass && followupOk && s_provenIntzDofState == 3)
                finalOk = DrawMode4FullDofFinalComposite();

            // V10.5.49.10.14 OUTPUT-TARGET ISOLATION. .10.9-.10.13 cleared every
            // pre-draw state category. Add only the old final-pass RT0/depth/viewport
            // transition, verify it, then exact-restore it. Fullscreen draw/composite stays OFF.
            RunMode4CameraMotionZBlurOutputTargetIsolation();

            if (hadFirstPass)
                QueueMode4FullDofChainEvent(followupOk, finalOk);
        }

        // V10.5.11 keeps the diagnostic blend deliberately scoped to the final
        // composite draw only. Restore PC's stock final-combine RGB factors so
        // the renderer's cached state remains truthful for later passes.
        if (s_v10Mode == 4 && !s_mode4MidProbeOnly && s_mode4BlendDiagnostic && s_rawSetRenderState)
        {
            void* device = reinterpret_cast<void*>(static_cast<uintptr_t>(ReadU32(kD3DDeviceGlobal)));
            if (device)
            {
                s_rawSetRenderState(device, kRSSrcBlend, 7u);
                s_rawSetRenderState(device, kRSDestBlend, 8u);
            }
        }

        if (ShouldLogSparse(count))
        {
            Event* event = ReserveEvent(EventType::CompositeExit);
            if (event)
            {
                event->a = static_cast<uint32_t>(count);
                event->b = ReadU32(kActivePSCache);
                event->c = ReadU32(kActiveVSCache);
                event->d = CurrentSceneRT();
                event->e = static_cast<uint32_t>(result);
            }
        }

        s_thisCompositeBlur1Routed = false;
        s_thisCompositeV10Scaled = false;
        s_thisCompositeNativeTailLevel1 = false;
        s_thisCompositeNativeTailLevel2 = false;
        s_currentCompositeCall = 0;
        s_insideComposite = false;
        return result;
    }

    void __cdecl BindTexture_Hook(int stage, int descriptor)
    {
        const uintptr_t returnAddress = reinterpret_cast<uintptr_t>(_ReturnAddress());
        const uint32_t quarterA = ReadU32(kRtQuarterA);
        const uint32_t quarterB = ReadU32(kRtQuarterB);
        const uint32_t lum16 = ReadU32(kRtLuminance16x12);
        const uint32_t second16 = ReadU32(kRtSecond16x12);

        if (lum16 && static_cast<uint32_t>(descriptor) == lum16 + 0x14u)
            InterlockedIncrement(&s_bindLum16Count);
        if (second16 && static_cast<uint32_t>(descriptor) == second16 + 0x14u)
            InterlockedIncrement(&s_bindSecond16Count);

        if (s_insideComposite && returnAddress == kCompositeSceneBindReturn &&
            ((s_v10Mode == 4 && s_mode4MidProbeOnly) ||
             (s_v10Mode == 5 && s_postProcessRoute == 3u)))
        {
            // Learn the Scene texture at the exact native bind that feeds the
            // quarter-resolution bloom stage. Debug D2 needs this identity for
            // the source-proven weighted Scene->QuarterA pass. No raw SetTexture
            // detour is enabled in Mode 5, so use a direct descriptor walk first
            // and IDirect3DDevice9::GetTexture(0) as the narrow authoritative fallback.
            const uint32_t sceneRT = CurrentSceneRT();
            const uint32_t expected = sceneRT ? sceneRT + 0x14u : 0u;

            s_originalBindTexture(stage, descriptor);

            if (stage == 0 && sceneRT && static_cast<uint32_t>(descriptor) == expected)
            {
                uint32_t surface = RawSurfaceDirectFromWrapper(sceneRT);
                if (surface < 0x10000u)
                    surface = s_rawSurfaceScene;

                uint32_t texture = RawTextureFromNglDescriptor(
                    static_cast<uint32_t>(descriptor));
                uint32_t source = 4u; // exact scene descriptor

                if (texture < 0x10000u && s_v10Mode == 4)
                {
                    texture = s_rawTextureStages[0];
                    source = 5u; // legacy exact scene bind raw-stage mirror
                }

                if (texture < 0x10000u && s_v10Mode == 5 && s_postProcessRoute == 3u)
                {
                    const uint32_t deviceValue = ReadU32(kD3DDeviceGlobal);
                    void* device = deviceValue >= 0x10000u
                        ? reinterpret_cast<void*>(static_cast<uintptr_t>(deviceValue)) : nullptr;
                    void** vtable = device && IsReadableMemory(device, sizeof(void*))
                        ? *reinterpret_cast<void***>(device) : nullptr;
                    if (vtable && IsReadableMemory(vtable,
                            (kD3DGetTextureVtableIndex + 1u) * sizeof(void*)) &&
                        IsExecutableMemory(vtable[kD3DGetTextureVtableIndex]))
                    {
                        const auto getTexture = reinterpret_cast<D3DGetTexture_t>(
                            vtable[kD3DGetTextureVtableIndex]);
                        void* boundTexture = nullptr;
                        const HRESULT getTextureHr = getTexture(device, 0u, &boundTexture);
                        if (SUCCEEDED(getTextureHr) && boundTexture)
                        {
                            texture = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(boundTexture));
                            source = 6u; // exact post-bind D3D stage-0 query
                            StoreRawIdentity(8u, sceneRT, surface, texture, getTextureHr, source);
                            SafeReleaseCom(boundTexture);
                            return;
                        }
                    }
                }

                if (texture >= 0x10000u && IsReadableMemory(
                    reinterpret_cast<void*>(static_cast<uintptr_t>(texture)), sizeof(void*)))
                {
                    StoreRawIdentity(8u, sceneRT, surface, texture, S_OK, source);
                }
            }
            return;
        }
        else if (s_insideComposite && returnAddress == kCompositeBlur1BindReturn)
        {
            const uint32_t expected = quarterA ? quarterA + 0x14u : 0u;
            if (stage == 0 && quarterA && quarterB && static_cast<uint32_t>(descriptor) == expected)
            {
                // V10.5.49.10.1 keeps the .49.9 source correction. At this exact point retail PC has
                // Quarter A bound as the next Gaussian source/RT.  Xbox
                // Function_82F31348 instead feeds the weighted shader directly from
                // Scene, writes the weighted result into its working bloom RT, then
                // runs Gaussian.  ApplyV10BloomScalePass therefore overwrites
                // Quarter A with Scene->weighted output and restores Quarter A as RT.
                // Gaussian parity then returns to the proven V7 route A->B->A.
                bool weightedSceneSource = false;
                // V10.5.66 Debug Xbox D2 + GodRay G1: restore the source-proven adaptive weighted
                // bloom stage only for route 3. Retail .63 stays locked. The controller
                // value comes from the validated 16x12 sampler (28..96 -> 0..1.5).
                // Apply only on a same-present world/depth frame with a fresh sample;
                // otherwise fall closed to the .64.1 QuarterA path.
                if (s_v10Mode == 5 && s_postProcessRoute == 3u)
                {
                    const uint32_t sampleAge =
                        (s_v10BloomSampleValid && s_v10_3LastLuminanceSamplePresent <= s_presentCount)
                            ? (s_presentCount - s_v10_3LastLuminanceSamplePresent)
                            : 0xFFFFFFFFu;
                    const bool freshSample = s_v10BloomSampleValid &&
                        sampleAge <= kV10ReadbackInterval;
                    const bool samePresentWorldDepth = s_mode4ReszDepthValidated &&
                        s_provenIntzDofPresent == s_presentCount &&
                        s_provenIntzDofDepthTexture >= 0x10000u;

                    if (freshSample && samePresentWorldDepth &&
                        s_mode5DrawHookState == 2 && ResolveRetailXboxDepthApisNoDetours())
                    {
                        InterlockedIncrement(&s_debugXboxD2BloomApplyAttemptCount);
                        if (!s_rawTextureQuarterA || !s_rawSurfaceQuarterA)
                            RefreshOneRawIdentity(ReadU32(kRtQuarterA), 1u);
                        if (!s_rawTextureQuarterB || !s_rawSurfaceQuarterB)
                            RefreshOneRawIdentity(ReadU32(kRtQuarterB), 2u);
                        if (!s_rawTextureScene || !s_rawSurfaceScene)
                            RefreshOneRawIdentity(CurrentSceneRT(), 8u);

                        weightedSceneSource = ApplyV10BloomScalePass();
                        if (weightedSceneSource)
                        {
                            InterlockedIncrement(&s_debugXboxD2BloomApplyPassCount);
                            s_debugXboxD2LastApplyPresent = s_presentCount;
                            s_debugXboxD2LastAppliedBloom = s_v10BloomStrength;
                        }
                        else
                        {
                            InterlockedIncrement(&s_debugXboxD2BloomApplyFailCount);
                        }
                    }
                    else
                    {
                        InterlockedIncrement(&s_debugXboxD2BloomApplySkipCount);
                    }
                }
                else if (s_v10Mode != 5 && s_v10Mode >= 1 && s_v10BloomSampleValid)
                {
                    weightedSceneSource = ApplyV10BloomScalePass();
                }
                s_thisCompositeV10Scaled = weightedSceneSource;

                // Gaussian #1 must sample Quarter A and render to Quarter B.
                // This is the proven V7 routing fix and preserves the game's
                // own shader, constant upload and fullscreen draw.
                const int rtResult = CallOriginalSetRTTracked(static_cast<int>(quarterB));
                s_thisCompositeBlur1Routed = true;
                QueueRouteEvent(EventType::RouteBlur1, returnAddress, quarterA, quarterB, rtResult);
                s_originalBindTexture(stage, descriptor);
            }
            else
            {
                QueueMismatch(1, returnAddress, static_cast<uint32_t>(descriptor), expected);
                s_originalBindTexture(stage, descriptor);
            }
            return;
        }
        else if (s_insideComposite && returnAddress == kCompositeFinalBindReturn)
        {
            const uint32_t expected = quarterA ? quarterA + 0x14u : 0u;
            const uint32_t sceneRT = CurrentSceneRT();
            if (stage == 0 && quarterA && quarterB && sceneRT &&
                static_cast<uint32_t>(descriptor) == expected)
            {
                // V10.5.49.10.1 recovered Xbox-order tail with retail-PC native c0:
                //   Scene -> weighted A -> Gaussian B -> BlurLevel1 A
                //         -> BlurLevel2 B -> F18/final Scene.
                // BlurLevel2 is inserted here, immediately before the stock final
                // bind. If either recovered tail stage is unavailable, fall closed
                // to the .49.9 final source (Quarter A) when the stock fallback redraw succeeds.
                s_nativeBloomTailFallbackGaussianHr = E_PENDING;
                if (s_v10Mode == 4 && s_mode4MidProbeOnly &&
                    s_thisCompositeNativeTailLevel1)
                {
                    s_thisCompositeNativeTailLevel2 = RunMode4NativeBlurLevel2Tail();
                    if (!s_thisCompositeNativeTailLevel2)
                    {
                        // Level1 already replaced stock Gaussian #2. If Level2
                        // fails, redraw the frozen .49.9 Gaussian B->A. On a successful
                        // fallback draw, Quarter A is restored to the old visual path.
                        RestoreMode4StockGaussian2Fallback();
                    }
                }

                const bool nativeTailOk =
                    s_thisCompositeNativeTailLevel1 && s_thisCompositeNativeTailLevel2;
                // V10.5.55 PostProcessFix keeps the proven native A->B->A
                // Gaussian repair. No recovered Xbox tail is injected yet, so
                // Quarter A remains the stock final bloom source.
                const uint32_t finalSource =
                    (s_v10Mode == 5) ? quarterA : (nativeTailOk ? quarterB : quarterA);
                const uint32_t finalDescriptor = finalSource + 0x14u;

                // Switch away from Quarter A before binding it as the final
                // texture source. This avoids sampling the same resource while
                // it is still RT0; the stock fullscreen draw that follows is unchanged.
                const int rtResult = CallOriginalSetRTTracked(static_cast<int>(sceneRT));
                QueueRouteEvent(EventType::RouteFinal, returnAddress, finalSource, sceneRT, rtResult);

                s_originalBindTexture(stage, static_cast<int>(finalDescriptor));
                if (s_v10Mode == 4 && s_mode4MidProbeOnly)
                {
                    QueueMode4NativeBloomTailEvent(finalSource);
                    ApplyProvenIntzPackedDofFinalState(finalDescriptor);
                }
                else if (s_v10Mode == 4)
                {
                    ApplyMode4DofFinalState(finalDescriptor);
                }
                return;
            }
            else
            {
                QueueMismatch(3, returnAddress, static_cast<uint32_t>(descriptor), expected);
            }
        }

        if (s_v10Mode == 4 && returnAddress == kGodRaysDepthBindReturn &&
            stage == 1 && static_cast<uint32_t>(descriptor) == kPostFxDepthDescriptor)
        {
            const LONG probeCount = InterlockedIncrement(&s_godRaysDepthBindProbeCount);
            const uintptr_t cacheAddress = kNglTextureCacheBase + sizeof(uint32_t);
            s_mode4LastGodRaysCacheBefore = IsReadableMemory(
                reinterpret_cast<void*>(cacheAddress), sizeof(uint32_t)) ? ReadU32(cacheAddress) : 0xDEADBEEFu;

            s_originalBindTexture(stage, descriptor);

            s_mode4LastGodRaysCacheAfter = IsReadableMemory(
                reinterpret_cast<void*>(cacheAddress), sizeof(uint32_t)) ? ReadU32(cacheAddress) : 0xDEADBEEFu;
            s_mode4LastGodRaysRawStage1 = s_rawTextureStages[1];

            if (probeCount <= 20 || (probeCount % 120) == 0)
            {
                Event* event = ReserveEvent(EventType::GodRaysDepthBindProbe);
                if (event)
                {
                    event->a = s_presentCount;
                    event->b = static_cast<uint32_t>(probeCount);
                    event->c = static_cast<uint32_t>(returnAddress);
                    event->d = static_cast<uint32_t>(descriptor);
                    event->e = s_mode4LastGodRaysCacheBefore;
                    event->f = s_mode4LastGodRaysCacheAfter;
                    event->g = s_mode4LastGodRaysRawStage1;
                    for (uint32_t i = 0; i < 8u; ++i)
                    {
                        const uintptr_t wordAddress = kPostFxDepthDescriptor + i * sizeof(uint32_t);
                        event->extra[i] = IsReadableMemory(
                            reinterpret_cast<void*>(wordAddress), sizeof(uint32_t)) ? ReadU32(wordAddress) : 0xDEADBEEFu;
                    }
                }
            }
            return;
        }

        s_originalBindTexture(stage, descriptor);
    }

    int __cdecl SetRT_Hook(int renderTarget)
    {
        const uintptr_t returnAddress = reinterpret_cast<uintptr_t>(_ReturnAddress());
        const uint32_t lum16 = ReadU32(kRtLuminance16x12);
        const uint32_t second16 = ReadU32(kRtSecond16x12);
        if (lum16 && static_cast<uint32_t>(renderTarget) == lum16)
            InterlockedIncrement(&s_setRTLum16Count);
        if (second16 && static_cast<uint32_t>(renderTarget) == second16)
            InterlockedIncrement(&s_setRTSecond16Count);

        if (s_insideComposite &&
            (returnAddress == kCompositeInitialSetRTReturn || returnAddress == kCompositeFinalSetRTReturn))
        {
            Event* event = ReserveEvent(EventType::NativeSetRT);
            if (event)
            {
                event->a = static_cast<uint32_t>(s_currentCompositeCall);
                event->b = static_cast<uint32_t>(returnAddress);
                event->c = static_cast<uint32_t>(renderTarget);
                event->d = ReadU32(kRtQuarterA);
                event->e = ReadU32(kRtQuarterB);
                event->f = CurrentSceneRT();
            }
        }

        // V10.5.23: NGL-level pinning is retired after V10.5.22 proved this
        // hook sees zero replay-time RT transitions. Keep this hook stock and
        // move the one-shot R32F pin to the proven sm_depth_shadow shader bind.
        return CallOriginalSetRTTracked(renderTarget);
    }

    using D3DPixelShaderGetFunction_t = HRESULT(WINAPI*)(void*, void*, UINT*);

    uint32_t Fnv1a32(const uint8_t* data, uint32_t size)
    {
        uint32_t hash = 2166136261u;
        for (uint32_t i = 0; i < size; ++i)
        {
            hash ^= data[i];
            hash *= 16777619u;
        }
        return hash;
    }

    bool ResolveMode5FinalShaderDeviceApis(
        void*& device,
        D3DCreatePixelShader_t& createPixelShader,
        D3DSetPixelShader_t& setPixelShader,
        D3DGetPixelShader_t& getPixelShader);

    bool QueryPixelShaderIdentity(void* shader, uint32_t& byteCountOut, uint32_t& hashOut,
        HRESULT& queryHrOut, HRESULT& fetchHrOut)
    {
        byteCountOut = 0u;
        hashOut = 0u;
        queryHrOut = E_FAIL;
        fetchHrOut = E_PENDING;
        if (!shader || !IsReadableMemory(shader, sizeof(void*)))
            return false;

        void** vtable = *reinterpret_cast<void***>(shader);
        if (!vtable || !IsReadableMemory(vtable,
                (kD3DPixelShaderGetFunctionVtableIndex + 1u) * sizeof(void*)) ||
            !IsExecutableMemory(vtable[kD3DPixelShaderGetFunctionVtableIndex]))
            return false;

        const D3DPixelShaderGetFunction_t getFunction =
            reinterpret_cast<D3DPixelShaderGetFunction_t>(
                vtable[kD3DPixelShaderGetFunctionVtableIndex]);
        UINT byteCount = 0u;
        queryHrOut = getFunction(shader, nullptr, &byteCount);
        if (FAILED(queryHrOut) || byteCount < sizeof(uint32_t) ||
            byteCount > kMode5FinalShaderCaptureMaxBytes)
            return false;

        alignas(4) uint8_t bytecode[kMode5FinalShaderCaptureMaxBytes] = {};
        UINT fetched = byteCount;
        fetchHrOut = getFunction(shader, bytecode, &fetched);
        if (FAILED(fetchHrOut) || fetched < sizeof(uint32_t) ||
            fetched > kMode5FinalShaderCaptureMaxBytes)
            return false;

        byteCountOut = fetched;
        hashOut = Fnv1a32(bytecode, fetched);
        return hashOut != 0u;
    }

    bool CaptureRetailXboxZBlurShaderOnce(uint32_t shaderObject, uint32_t shaderKind)
    {
        if (shaderObject < 0x10000u || shaderKind > 1u)
            return false;

        volatile LONG* captureState = shaderKind == 0u
            ? &s_retailXboxZBlurVsCaptureState : &s_retailXboxZBlurPsCaptureState;
        if (*captureState == 2)
            return true;
        if (InterlockedCompareExchange(captureState, 1, 0) != 0)
            return *captureState == 2;

        uint8_t* destination = shaderKind == 0u
            ? s_retailXboxZBlurVsBytecode : s_retailXboxZBlurPsBytecode;
        HRESULT queryHr = E_FAIL;
        HRESULT fetchHr = E_PENDING;
        UINT byteCount = 0u;
        uint32_t versionToken = 0u;
        uint32_t lastToken = 0u;
        uint32_t hash = 0u;
        uint32_t chunkCount = 0u;
        bool captured = false;

        void* shader = reinterpret_cast<void*>(static_cast<uintptr_t>(shaderObject));
        if (IsReadableMemory(shader, sizeof(void*)))
        {
            void** vtable = *reinterpret_cast<void***>(shader);
            if (vtable && IsReadableMemory(vtable,
                    (kD3DPixelShaderGetFunctionVtableIndex + 1u) * sizeof(void*)) &&
                IsExecutableMemory(vtable[kD3DPixelShaderGetFunctionVtableIndex]))
            {
                const D3DPixelShaderGetFunction_t getFunction =
                    reinterpret_cast<D3DPixelShaderGetFunction_t>(
                        vtable[kD3DPixelShaderGetFunctionVtableIndex]);
                queryHr = getFunction(shader, nullptr, &byteCount);
                if (SUCCEEDED(queryHr) && byteCount >= sizeof(uint32_t) &&
                    byteCount <= kMode5FinalShaderCaptureMaxBytes)
                {
                    memset(destination, 0, kMode5FinalShaderCaptureMaxBytes);
                    UINT fetched = byteCount;
                    fetchHr = getFunction(shader, destination, &fetched);
                    if (SUCCEEDED(fetchHr) && fetched >= sizeof(uint32_t) &&
                        fetched <= kMode5FinalShaderCaptureMaxBytes)
                    {
                        byteCount = fetched;
                        hash = Fnv1a32(destination, byteCount);
                        memcpy(&versionToken, destination, sizeof(versionToken));
                        const uint32_t alignedDwords = (byteCount + 3u) / 4u;
                        if (alignedDwords != 0u)
                        {
                            const uint32_t byteOffset = (alignedDwords - 1u) * 4u;
                            const uint32_t bytesLeft = byteCount > byteOffset
                                ? byteCount - byteOffset : 0u;
                            const uint32_t copyBytes = bytesLeft >= 4u ? 4u : bytesLeft;
                            if (copyBytes)
                                memcpy(&lastToken, destination + byteOffset, copyBytes);
                        }
                        chunkCount = (alignedDwords + 27u) / 28u;

                        if (kRetailXboxLogShaderBytecodeChunks)
                        for (uint32_t chunk = 0u; chunk < chunkCount; ++chunk)
                        {
                            Event* event = ReserveEvent(EventType::RetailXboxZBlurShaderChunk);
                            if (!event)
                                break;
                            const uint32_t tokenOffset = chunk * 28u;
                            const uint32_t remaining = alignedDwords - tokenOffset;
                            const uint32_t tokenCount = remaining > 28u ? 28u : remaining;
                            event->a = shaderKind;
                            event->b = chunk;
                            event->c = chunkCount;
                            event->d = tokenOffset;
                            event->e = tokenCount;
                            event->f = shaderObject;
                            for (uint32_t n = 0u; n < tokenCount; ++n)
                            {
                                uint32_t token = 0u;
                                const uint32_t offset = (tokenOffset + n) * 4u;
                                const uint32_t left = byteCount > offset ? byteCount - offset : 0u;
                                const uint32_t bytes = left >= 4u ? 4u : left;
                                if (bytes)
                                    memcpy(&token, destination + offset, bytes);
                                event->extra[n] = token;
                            }
                        }
                        captured = hash != 0u;
                    }
                }
            }
        }

        if (shaderKind == 0u)
        {
            s_retailXboxZBlurVsBytes = byteCount;
            s_retailXboxZBlurVsHash = hash;
            s_retailXboxZBlurVsVersion = versionToken;
            s_retailXboxZBlurVsLastToken = lastToken;
            s_retailXboxZBlurVsQueryHr = static_cast<uint32_t>(queryHr);
            s_retailXboxZBlurVsFetchHr = static_cast<uint32_t>(fetchHr);
        }
        else
        {
            s_retailXboxZBlurPsBytes = byteCount;
            s_retailXboxZBlurPsHash = hash;
            s_retailXboxZBlurPsVersion = versionToken;
            s_retailXboxZBlurPsLastToken = lastToken;
            s_retailXboxZBlurPsQueryHr = static_cast<uint32_t>(queryHr);
            s_retailXboxZBlurPsFetchHr = static_cast<uint32_t>(fetchHr);
        }

        InterlockedExchange(captureState, captured ? 2 : -1);
        return captured;
    }

    void ProbeRetailXboxCameraMotionContract(uintptr_t returnAddress)
    {
        if (!IsXboxPostProcessRoute() || returnAddress != kCompositeFinalQuadReturn)
            return;

        const LONG probe = InterlockedIncrement(&s_retailXboxZBlurProbeCount);
        const bool previousGate = s_cameraMotionZBlurGateLast;
        const bool gate = EvaluateNativePcCameraZBlurGate();
        const bool gateChanged = probe > 1 && gate != previousGate;

        float dynamicScale = kCameraMotionZBlurFixedMinScale;
        if (gate)
            dynamicScale = UpdateMode4CameraMotionZBlurScale();
        else
            ResetMode4CameraMotionZBlurController();

        const uint32_t vsValue = ReadU32(kCameraMotionZBlurVS);
        const uint32_t psValue = ReadU32(kCameraMotionZBlurPS);
        const uint32_t sceneRT = CurrentSceneRT();
        const uint32_t sceneSurface = RawSurfaceDirectFromWrapper(sceneRT);
        const uint32_t sceneTexture = sceneRT >= 0x10000u
            ? RawTextureFromNglDescriptor(sceneRT + 0x14u) : 0u;
        const uint32_t activeVS = ReadU32(kActiveVSCache);
        const uint32_t activePS = ReadU32(kActivePSCache);

        const bool vsReady = vsValue >= 0x10000u &&
            IsReadableMemory(reinterpret_cast<void*>(static_cast<uintptr_t>(vsValue)), sizeof(void*));
        const bool psReady = psValue >= 0x10000u &&
            IsReadableMemory(reinterpret_cast<void*>(static_cast<uintptr_t>(psValue)), sizeof(void*));
        const bool shaderPairReady = vsReady && psReady;
        const bool sceneReady = sceneRT >= 0x10000u && sceneSurface >= 0x10000u;

        if (shaderPairReady && s_retailXboxZBlurCaptureState == 0 &&
            InterlockedCompareExchange(&s_retailXboxZBlurCaptureState, 1, 0) == 0)
        {
            const bool vsCaptured = CaptureRetailXboxZBlurShaderOnce(vsValue, 0u);
            const bool psCaptured = CaptureRetailXboxZBlurShaderOnce(psValue, 1u);
            InterlockedExchange(&s_retailXboxZBlurCaptureState,
                (vsCaptured && psCaptured) ? 2 : -1);
        }

        if (probe <= 16 || (probe % 120) == 0 || gateChanged)
        {
            Event* event = ReserveEvent(EventType::RetailXboxZBlurContractProbe);
            if (event)
            {
                event->a = s_presentCount;
                event->b = static_cast<uint32_t>(probe);
                event->c = gate ? 1u : 0u;
                event->d = static_cast<uint32_t>(s_retailXboxZBlurCaptureState);
                event->e = vsValue;
                event->f = psValue;
                event->g = activeVS;
                event->h = activePS;
                event->i = sceneRT;
                event->j = sceneSurface;
                event->extra[0] = sceneTexture;
                event->extra[1] = shaderPairReady ? 1u : 0u;
                event->extra[2] = sceneReady ? 1u : 0u;
                event->extra[3] = s_cameraMotionZBlurGateReadable ? 1u : 0u;
                event->extra[4] = s_cameraMotionZBlurControllerValid ? 1u : 0u;
                event->extra[5] = s_cameraMotionZBlurCameraValue;
                event->extra[6] = s_cameraMotionZBlurTransformValue;
                event->extra[7] = FloatBits(s_cameraMotionZBlurLastDelta);
                event->extra[8] = FloatBits(s_cameraMotionZBlurLastAverage);
                event->extra[9] = FloatBits(s_cameraMotionZBlurLastMotion);
                event->extra[10] = FloatBits(s_cameraMotionZBlurLastBlurPixels);
                event->extra[11] = FloatBits(dynamicScale);
                event->extra[12] = s_retailXboxZBlurVsBytes;
                event->extra[13] = s_retailXboxZBlurVsHash;
                event->extra[14] = s_retailXboxZBlurVsVersion;
                event->extra[15] = s_retailXboxZBlurVsLastToken;
                event->extra[16] = s_retailXboxZBlurPsBytes;
                event->extra[17] = s_retailXboxZBlurPsHash;
                event->extra[18] = s_retailXboxZBlurPsVersion;
                event->extra[19] = s_retailXboxZBlurPsLastToken;
                event->extra[20] = static_cast<uint32_t>(s_retailXboxZBlurVsCaptureState);
                event->extra[21] = static_cast<uint32_t>(s_retailXboxZBlurPsCaptureState);
                event->extra[22] = s_retailXboxZBlurVsQueryHr;
                event->extra[23] = s_retailXboxZBlurVsFetchHr;
                event->extra[24] = s_retailXboxZBlurPsQueryHr;
                event->extra[25] = s_retailXboxZBlurPsFetchHr;
                event->extra[26] = 0u; // camera-motion GPU mutations/draws in R4
                event->extra[27] = s_postProcessRoute;
            }
        }
    }

    bool EnsureRetailXboxImageZoomSafeSource(void* device, void* targetSurface)
    {
        if (!device || !targetSurface)
            return false;
        void** dvt = IsReadableMemory(device, sizeof(void*)) ? *reinterpret_cast<void***>(device) : nullptr;
        void** svt = IsReadableMemory(targetSurface, sizeof(void*)) ? *reinterpret_cast<void***>(targetSurface) : nullptr;
        if (!dvt || !svt ||
            !IsReadableMemory(dvt, (kD3DCreateTextureVtableIndex + 1u) * sizeof(void*)) ||
            !IsReadableMemory(svt, (kD3DSurfaceGetDescVtableIndex + 1u) * sizeof(void*)) ||
            !IsExecutableMemory(dvt[kD3DCreateTextureVtableIndex]) ||
            !IsExecutableMemory(svt[kD3DSurfaceGetDescVtableIndex]))
            return false;

        const auto createTexture = reinterpret_cast<D3DCreateTexture_t>(dvt[kD3DCreateTextureVtableIndex]);
        const auto getDesc = reinterpret_cast<D3DSurfaceGetDesc_t>(svt[kD3DSurfaceGetDescVtableIndex]);
        D3DSurfaceDescLite desc = {};
        if (FAILED(getDesc(targetSurface, &desc)) || !desc.Width || !desc.Height)
            return false;

        if (s_retailXboxImageZoomSourceTexture && s_retailXboxImageZoomSourceSurface &&
            s_retailXboxImageZoomSourceWidth == desc.Width &&
            s_retailXboxImageZoomSourceHeight == desc.Height &&
            s_retailXboxImageZoomSourceFormat == desc.Format)
            return true;

        ResetRetailXboxImageZoomProbeResources();
        void* texture = nullptr;
        const HRESULT createHr = createTexture(
            device, desc.Width, desc.Height, 1u, 1u, desc.Format, 0u, &texture, nullptr);
        if (FAILED(createHr) || !texture)
            return false;

        void** tvt = IsReadableMemory(texture, sizeof(void*)) ? *reinterpret_cast<void***>(texture) : nullptr;
        if (!tvt || !IsReadableMemory(tvt,
                (kD3DTextureGetSurfaceLevelVtableIndex + 1u) * sizeof(void*)) ||
            !IsExecutableMemory(tvt[kD3DTextureGetSurfaceLevelVtableIndex]))
        {
            SafeReleaseCom(texture);
            return false;
        }
        const auto getSurfaceLevel = reinterpret_cast<D3DTextureGetSurfaceLevel_t>(
            tvt[kD3DTextureGetSurfaceLevelVtableIndex]);
        void* surface = nullptr;
        const HRESULT surfaceHr = getSurfaceLevel(texture, 0u, &surface);
        if (FAILED(surfaceHr) || !surface)
        {
            SafeReleaseCom(texture);
            return false;
        }

        s_retailXboxImageZoomSourceTexture = texture;
        s_retailXboxImageZoomSourceSurface = surface;
        s_retailXboxImageZoomSourceWidth = desc.Width;
        s_retailXboxImageZoomSourceHeight = desc.Height;
        s_retailXboxImageZoomSourceFormat = desc.Format;
        return true;
    }

    void RunRetailXboxImageZoomRouteBindProbe()
    {
        if (!IsXboxPostProcessRoute())
            return;

        const LONG probe = InterlockedIncrement(&s_retailXboxImageZoomProbeCount);
        const bool gate = EvaluateNativePcCameraZBlurGate();
        if (!gate)
        {
            ResetMode4CameraMotionZBlurController();
            InterlockedIncrement(&s_retailXboxImageZoomSkipCount);
            if (ShouldLogSparse(probe))
            {
                Event* event = ReserveEvent(EventType::RetailXboxImageZoomRouteBindProbe);
                if (event)
                {
                    event->a = s_presentCount;
                    event->b = static_cast<uint32_t>(probe);
                    event->c = 0u;
                    event->d = 0u;
                    event->extra[27] = s_postProcessRoute;
                }
            }
            return;
        }

        InterlockedIncrement(&s_retailXboxImageZoomEligibleCount);
        const float zoomScale = UpdateMode4CameraMotionZBlurScale();
        const float zoomC0[4] = { 0.5f, 0.5f, zoomScale, zoomScale };
        memcpy(s_cameraMotionZBlurLastC0, zoomC0, sizeof(zoomC0));

        const uint32_t vsValue = ReadU32(kCameraMotionZBlurVS);
        const uint32_t psValue = ReadU32(kCameraMotionZBlurPS);
        CaptureRetailXboxZBlurShaderOnce(vsValue, 0u);
        CaptureRetailXboxZBlurShaderOnce(psValue, 1u);

        const uint32_t deviceValue = ReadU32(kD3DDeviceGlobal);
        void* device = deviceValue >= 0x10000u
            ? reinterpret_cast<void*>(static_cast<uintptr_t>(deviceValue)) : nullptr;
        void** dvt = device && IsReadableMemory(device, sizeof(void*))
            ? *reinterpret_cast<void***>(device) : nullptr;

        HRESULT getRtBeforeHr = E_FAIL, getRtAfterHr = E_FAIL;
        HRESULT getDsBeforeHr = E_FAIL, getDsAfterHr = E_FAIL;
        HRESULT stretchHr = E_FAIL;
        HRESULT getVsHr = E_FAIL, getPsHr = E_FAIL, getVsC0Hr = E_FAIL, getTexHr = E_FAIL;
        HRESULT setVsHr = E_FAIL, setPsHr = E_FAIL, setVsC0Hr = E_FAIL, setTexHr = E_FAIL;
        HRESULT getBoundVsHr = E_FAIL, getBoundPsHr = E_FAIL, getBoundVsC0Hr = E_FAIL, getBoundTexHr = E_FAIL;
        HRESULT restoreVsHr = E_FAIL, restorePsHr = E_FAIL, restoreVsC0Hr = E_FAIL, restoreTexHr = E_FAIL;
        HRESULT getRestoredVsHr = E_FAIL, getRestoredPsHr = E_FAIL, getRestoredVsC0Hr = E_FAIL, getRestoredTexHr = E_FAIL;

        void* targetBefore = nullptr;
        void* targetAfter = nullptr;
        void* dsBefore = nullptr;
        void* dsAfter = nullptr;
        void* beforeVS = nullptr;
        void* beforePS = nullptr;
        void* beforeTex0 = nullptr;
        void* boundVS = nullptr;
        void* boundPS = nullptr;
        void* boundTex0 = nullptr;
        void* restoredVS = nullptr;
        void* restoredPS = nullptr;
        void* restoredTex0 = nullptr;
        float beforeVsC0[4] = {};
        float boundVsC0[4] = {};
        float restoredVsC0[4] = {};

        constexpr uint32_t kSamplerStateCount = 7u;
        const DWORD samplerTypes[kSamplerStateCount] = {
            kSampAddressU, kSampAddressV, kSampMagFilter, kSampMinFilter,
            kSampMipFilter, kSampMaxAnisotropy, kSampSrgbTexture
        };
        const DWORD samplerTargets[kSamplerStateCount] = {
            kTextureAddressClamp, kTextureAddressClamp,
            kTextureFilterLinear, kTextureFilterLinear, kTextureFilterLinear,
            1u, 0u
        };
        DWORD samplerBefore[kSamplerStateCount] = {};
        DWORD samplerBound[kSamplerStateCount] = {};
        DWORD samplerRestored[kSamplerStateCount] = {};
        uint32_t samplerCaptureMask = 0u, samplerSetMask = 0u;
        uint32_t samplerBoundMask = 0u, samplerRestoreSetMask = 0u, samplerRestoreMask = 0u;
        constexpr uint32_t kSamplerAllMask = (1u << kSamplerStateCount) - 1u;

        bool apiReady = false;
        bool sourceCreated = false;
        bool sourceCopied = false;
        bool sourceTargetAlias = true;
        bool sceneTargetVerified = false;
        bool shaderBindVerified = false;
        bool vsC0Verified = false;
        bool textureBindVerified = false;
        bool samplerBindVerified = false;
        bool shaderRestoreVerified = false;
        bool vsC0RestoreVerified = false;
        bool textureRestoreVerified = false;
        bool samplerRestoreVerified = false;
        bool rtStable = false;
        bool dsStable = false;

        uint32_t sceneWrapper = CurrentSceneRT();
        uint32_t sceneSurface = RawSurfaceDirectFromWrapper(sceneWrapper);
        uint32_t targetContainerTex = 0u;
        HRESULT targetContainerHr = E_PENDING;

        if (dvt && IsReadableMemory(dvt,
                (kD3DGetPixelShaderVtableIndex + 1u) * sizeof(void*)) &&
            IsExecutableMemory(dvt[kD3DStretchRectVtableIndex]) &&
            IsExecutableMemory(dvt[kD3DGetRenderTargetVtableIndex]) &&
            IsExecutableMemory(dvt[kD3DGetDepthStencilSurfaceVtableIndex]) &&
            IsExecutableMemory(dvt[kD3DSetVertexShaderVtableIndex]) &&
            IsExecutableMemory(dvt[kD3DGetVertexShaderVtableIndex]) &&
            IsExecutableMemory(dvt[kD3DSetVertexShaderConstantFVtableIndex]) &&
            IsExecutableMemory(dvt[kD3DGetVertexShaderConstantFVtableIndex]) &&
            IsExecutableMemory(dvt[kD3DSetPixelShaderVtableIndex]) &&
            IsExecutableMemory(dvt[kD3DGetPixelShaderVtableIndex]) &&
            IsExecutableMemory(dvt[kD3DSetTextureVtableIndex]) &&
            IsExecutableMemory(dvt[kD3DGetTextureVtableIndex]) &&
            IsExecutableMemory(dvt[kD3DSetSamplerStateVtableIndex]) &&
            IsExecutableMemory(dvt[kD3DGetSamplerStateVtableIndex]))
        {
            const auto stretchRect = reinterpret_cast<D3DStretchRect_t>(dvt[kD3DStretchRectVtableIndex]);
            const auto getRT = reinterpret_cast<D3DGetRenderTarget_t>(dvt[kD3DGetRenderTargetVtableIndex]);
            const auto getDS = reinterpret_cast<D3DGetDepthStencilSurface_t>(dvt[kD3DGetDepthStencilSurfaceVtableIndex]);
            const auto setVS = reinterpret_cast<D3DSetVertexShader_t>(dvt[kD3DSetVertexShaderVtableIndex]);
            const auto getVS = reinterpret_cast<D3DGetVertexShader_t>(dvt[kD3DGetVertexShaderVtableIndex]);
            const auto setVSC0 = reinterpret_cast<D3DSetVertexShaderConstantF_t>(dvt[kD3DSetVertexShaderConstantFVtableIndex]);
            const auto getVSC0 = reinterpret_cast<D3DGetVertexShaderConstantF_t>(dvt[kD3DGetVertexShaderConstantFVtableIndex]);
            const auto setPS = reinterpret_cast<D3DSetPixelShader_t>(dvt[kD3DSetPixelShaderVtableIndex]);
            const auto getPS = reinterpret_cast<D3DGetPixelShader_t>(dvt[kD3DGetPixelShaderVtableIndex]);
            const auto setTexture = reinterpret_cast<D3DSetTexture_t>(dvt[kD3DSetTextureVtableIndex]);
            const auto getTexture = reinterpret_cast<D3DGetTexture_t>(dvt[kD3DGetTextureVtableIndex]);
            const auto setSampler = reinterpret_cast<D3DSetSamplerState_t>(dvt[kD3DSetSamplerStateVtableIndex]);
            const auto getSampler = reinterpret_cast<D3DGetSamplerState_t>(dvt[kD3DGetSamplerStateVtableIndex]);
            apiReady = true;

            getRtBeforeHr = getRT(device, 0u, &targetBefore);
            getDsBeforeHr = getDS(device, &dsBefore);
            sceneTargetVerified = SUCCEEDED(getRtBeforeHr) && targetBefore &&
                sceneSurface >= 0x10000u &&
                static_cast<uint32_t>(reinterpret_cast<uintptr_t>(targetBefore)) == sceneSurface;
            if (targetBefore)
                targetContainerTex = RawTextureFromSurface(
                    static_cast<uint32_t>(reinterpret_cast<uintptr_t>(targetBefore)), &targetContainerHr);

            sourceCreated = SUCCEEDED(getRtBeforeHr) && targetBefore &&
                EnsureRetailXboxImageZoomSafeSource(device, targetBefore);
            if (sourceCreated && s_retailXboxImageZoomSourceSurface &&
                s_retailXboxImageZoomSourceTexture)
            {
                stretchHr = stretchRect(device, targetBefore, nullptr,
                    s_retailXboxImageZoomSourceSurface, nullptr, kD3DFilterNone);
                sourceCopied = SUCCEEDED(stretchHr);
                const uint32_t sourceSurfaceValue = static_cast<uint32_t>(
                    reinterpret_cast<uintptr_t>(s_retailXboxImageZoomSourceSurface));
                const uint32_t sourceTextureValue = static_cast<uint32_t>(
                    reinterpret_cast<uintptr_t>(s_retailXboxImageZoomSourceTexture));
                const bool surfaceDistinct = sourceSurfaceValue != 0u &&
                    sourceSurfaceValue != static_cast<uint32_t>(reinterpret_cast<uintptr_t>(targetBefore));
                const bool textureDistinct = targetContainerTex == 0u || sourceTextureValue != targetContainerTex;
                sourceTargetAlias = !(surfaceDistinct && textureDistinct);
            }

            getVsHr = getVS(device, &beforeVS);
            getPsHr = getPS(device, &beforePS);
            getVsC0Hr = getVSC0(device, 0u, beforeVsC0, 1u);
            getTexHr = getTexture(device, 0u, &beforeTex0);
            for (uint32_t n = 0u; n < kSamplerStateCount; ++n)
                if (SUCCEEDED(getSampler(device, 0u, samplerTypes[n], &samplerBefore[n])))
                    samplerCaptureMask |= (1u << n);

            const bool preflight = sourceCopied && !sourceTargetAlias && sceneTargetVerified &&
                vsValue >= 0x10000u && psValue >= 0x10000u &&
                s_retailXboxZBlurVsCaptureState == 2 && s_retailXboxZBlurPsCaptureState == 2 &&
                SUCCEEDED(getVsHr) && SUCCEEDED(getPsHr) && SUCCEEDED(getVsC0Hr) && SUCCEEDED(getTexHr) &&
                samplerCaptureMask == kSamplerAllMask;

            if (preflight)
            {
                setVsHr = setVS(device, reinterpret_cast<void*>(static_cast<uintptr_t>(vsValue)));
                setPsHr = setPS(device, reinterpret_cast<void*>(static_cast<uintptr_t>(psValue)));
                setVsC0Hr = setVSC0(device, 0u, zoomC0, 1u);
                setTexHr = setTexture(device, 0u, s_retailXboxImageZoomSourceTexture);
                if (SUCCEEDED(setVsHr) && SUCCEEDED(setPsHr) && SUCCEEDED(setVsC0Hr) && SUCCEEDED(setTexHr))
                {
                    for (uint32_t n = 0u; n < kSamplerStateCount; ++n)
                        if (SUCCEEDED(setSampler(device, 0u, samplerTypes[n], samplerTargets[n])))
                            samplerSetMask |= (1u << n);

                    getBoundVsHr = getVS(device, &boundVS);
                    getBoundPsHr = getPS(device, &boundPS);
                    getBoundVsC0Hr = getVSC0(device, 0u, boundVsC0, 1u);
                    getBoundTexHr = getTexture(device, 0u, &boundTex0);
                    for (uint32_t n = 0u; n < kSamplerStateCount; ++n)
                        if (SUCCEEDED(getSampler(device, 0u, samplerTypes[n], &samplerBound[n])) &&
                            samplerBound[n] == samplerTargets[n])
                            samplerBoundMask |= (1u << n);

                    shaderBindVerified = SUCCEEDED(getBoundVsHr) && SUCCEEDED(getBoundPsHr) &&
                        static_cast<uint32_t>(reinterpret_cast<uintptr_t>(boundVS)) == vsValue &&
                        static_cast<uint32_t>(reinterpret_cast<uintptr_t>(boundPS)) == psValue;
                    vsC0Verified = SUCCEEDED(getBoundVsC0Hr) &&
                        memcmp(boundVsC0, zoomC0, sizeof(zoomC0)) == 0;
                    textureBindVerified = SUCCEEDED(getBoundTexHr) &&
                        boundTex0 == s_retailXboxImageZoomSourceTexture;
                    samplerBindVerified = samplerSetMask == kSamplerAllMask &&
                        samplerBoundMask == kSamplerAllMask;
                }

                // Exact restoration. No fullscreen/ImageZoom draw occurs anywhere in R5.
                for (uint32_t n = 0u; n < kSamplerStateCount; ++n)
                    if (SUCCEEDED(setSampler(device, 0u, samplerTypes[n], samplerBefore[n])))
                        samplerRestoreSetMask |= (1u << n);
                restoreTexHr = setTexture(device, 0u, beforeTex0);
                restoreVsC0Hr = setVSC0(device, 0u, beforeVsC0, 1u);
                restorePsHr = setPS(device, beforePS);
                restoreVsHr = setVS(device, beforeVS);

                getRestoredVsHr = getVS(device, &restoredVS);
                getRestoredPsHr = getPS(device, &restoredPS);
                getRestoredVsC0Hr = getVSC0(device, 0u, restoredVsC0, 1u);
                getRestoredTexHr = getTexture(device, 0u, &restoredTex0);
                for (uint32_t n = 0u; n < kSamplerStateCount; ++n)
                    if (SUCCEEDED(getSampler(device, 0u, samplerTypes[n], &samplerRestored[n])) &&
                        samplerRestored[n] == samplerBefore[n])
                        samplerRestoreMask |= (1u << n);

                shaderRestoreVerified = SUCCEEDED(restoreVsHr) && SUCCEEDED(restorePsHr) &&
                    SUCCEEDED(getRestoredVsHr) && SUCCEEDED(getRestoredPsHr) &&
                    restoredVS == beforeVS && restoredPS == beforePS;
                vsC0RestoreVerified = SUCCEEDED(restoreVsC0Hr) && SUCCEEDED(getRestoredVsC0Hr) &&
                    memcmp(restoredVsC0, beforeVsC0, sizeof(beforeVsC0)) == 0;
                textureRestoreVerified = SUCCEEDED(restoreTexHr) && SUCCEEDED(getRestoredTexHr) &&
                    restoredTex0 == beforeTex0;
                samplerRestoreVerified = samplerRestoreSetMask == kSamplerAllMask &&
                    samplerRestoreMask == kSamplerAllMask;
            }

            getRtAfterHr = getRT(device, 0u, &targetAfter);
            getDsAfterHr = getDS(device, &dsAfter);
            rtStable = SUCCEEDED(getRtBeforeHr) && SUCCEEDED(getRtAfterHr) && targetAfter == targetBefore;
            dsStable = getDsAfterHr == getDsBeforeHr && dsAfter == dsBefore;
        }

        const bool eligible = apiReady && sourceCopied && !sourceTargetAlias && sceneTargetVerified &&
            s_retailXboxZBlurVsCaptureState == 2 && s_retailXboxZBlurPsCaptureState == 2;
        const bool pass = eligible && shaderBindVerified && vsC0Verified && textureBindVerified &&
            samplerBindVerified && shaderRestoreVerified && vsC0RestoreVerified &&
            textureRestoreVerified && samplerRestoreVerified && rtStable && dsStable;
        if (eligible)
        {
            if (pass) InterlockedIncrement(&s_retailXboxImageZoomPassCount);
            else InterlockedIncrement(&s_retailXboxImageZoomFailCount);
        }
        else
        {
            InterlockedIncrement(&s_retailXboxImageZoomFailCount);
        }

        if (ShouldLogSparse(probe) || !pass)
        {
            Event* event = ReserveEvent(EventType::RetailXboxImageZoomRouteBindProbe);
            if (event)
            {
                event->a = s_presentCount;
                event->b = static_cast<uint32_t>(probe);
                event->c = gate ? 1u : 0u;
                event->d = pass ? 1u : 0u;
                event->e = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(s_retailXboxImageZoomSourceTexture));
                event->f = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(s_retailXboxImageZoomSourceSurface));
                event->g = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(targetBefore));
                event->h = sceneWrapper;
                event->i = sceneSurface;
                event->j = targetContainerTex;
                event->extra[0] = sourceTargetAlias ? 1u : 0u;
                event->extra[1] = sceneTargetVerified ? 1u : 0u;
                event->extra[2] = sourceCopied ? 1u : 0u;
                event->extra[3] = static_cast<uint32_t>(stretchHr);
                event->extra[4] = vsValue;
                event->extra[5] = psValue;
                event->extra[6] = s_retailXboxZBlurVsHash;
                event->extra[7] = s_retailXboxZBlurPsHash;
                event->extra[8] = FloatBits(s_cameraMotionZBlurLastMotion);
                event->extra[9] = FloatBits(s_cameraMotionZBlurLastBlurPixels);
                event->extra[10] = FloatBits(zoomScale);
                event->extra[11] = shaderBindVerified ? 1u : 0u;
                event->extra[12] = vsC0Verified ? 1u : 0u;
                event->extra[13] = textureBindVerified ? 1u : 0u;
                event->extra[14] = samplerBindVerified ? 1u : 0u;
                event->extra[15] = shaderRestoreVerified ? 1u : 0u;
                event->extra[16] = vsC0RestoreVerified ? 1u : 0u;
                event->extra[17] = textureRestoreVerified ? 1u : 0u;
                event->extra[18] = samplerRestoreVerified ? 1u : 0u;
                event->extra[19] = rtStable ? 1u : 0u;
                event->extra[20] = dsStable ? 1u : 0u;
                event->extra[21] = samplerCaptureMask;
                event->extra[22] = samplerSetMask;
                event->extra[23] = samplerBoundMask;
                event->extra[24] = samplerRestoreMask;
                event->extra[25] = static_cast<uint32_t>(s_retailXboxImageZoomPassCount);
                event->extra[26] = static_cast<uint32_t>(s_retailXboxImageZoomFailCount);
                event->extra[27] = 0u; // R5 ImageZoom draw count: deliberately zero.
            }
        }

        SafeReleaseCom(targetBefore);
        SafeReleaseCom(targetAfter);
        SafeReleaseCom(dsBefore);
        SafeReleaseCom(dsAfter);
        SafeReleaseCom(beforeVS);
        SafeReleaseCom(beforePS);
        SafeReleaseCom(beforeTex0);
        SafeReleaseCom(boundVS);
        SafeReleaseCom(boundPS);
        SafeReleaseCom(boundTex0);
        SafeReleaseCom(restoredVS);
        SafeReleaseCom(restoredPS);
        SafeReleaseCom(restoredTex0);
    }

    bool RunRetailXboxImageZoomNativeDraw(
        void* device, DWORD primitiveType, UINT startVertex, UINT primitiveCount)
    {
        const LONG attempt = InterlockedIncrement(&s_retailXboxImageZoomDrawAttemptCount);
        if (!IsXboxPostProcessRoute() || s_retailXboxImageZoomDrawDisabled != 0 ||
            !device || !s_rawDrawPrimitive || primitiveType != kD3DPrimitiveTriangleStrip ||
            startVertex != 0u || primitiveCount != 2u)
        {
            InterlockedIncrement(&s_retailXboxImageZoomDrawSkipCount);
            return false;
        }

        const bool gate = EvaluateNativePcCameraZBlurGate();
        if (!gate)
        {
            ResetMode4CameraMotionZBlurController();
            InterlockedIncrement(&s_retailXboxImageZoomDrawSkipCount);
            return false;
        }

        const float zoomScale = UpdateMode4CameraMotionZBlurScale();
        const float zoomC0[4] = { 0.5f, 0.5f, zoomScale, zoomScale };
        memcpy(s_cameraMotionZBlurLastC0, zoomC0, sizeof(zoomC0));

        const uint32_t vsValue = ReadU32(kCameraMotionZBlurVS);
        const uint32_t psValue = ReadU32(kCameraMotionZBlurPS);
        CaptureRetailXboxZBlurShaderOnce(vsValue, 0u);
        CaptureRetailXboxZBlurShaderOnce(psValue, 1u);
        constexpr uint32_t kExpectedImageZoomVsHash = 0xB856422Du;
        constexpr uint32_t kExpectedImageZoomPsHash = 0x3FA172A3u;
        const bool shaderIdentityReady =
            s_retailXboxZBlurVsCaptureState == 2 && s_retailXboxZBlurPsCaptureState == 2 &&
            s_retailXboxZBlurVsHash == kExpectedImageZoomVsHash &&
            s_retailXboxZBlurPsHash == kExpectedImageZoomPsHash;

        void** dvt = IsReadableMemory(device, sizeof(void*))
            ? *reinterpret_cast<void***>(device) : nullptr;

        HRESULT getRtBeforeHr=E_FAIL, getRtAfterHr=E_FAIL, getDsBeforeHr=E_FAIL, getDsAfterHr=E_FAIL;
        HRESULT stretchHr=E_FAIL;
        HRESULT getVsHr=E_FAIL, getPsHr=E_FAIL, getVsC0Hr=E_FAIL, getTexHr=E_FAIL;
        HRESULT setVsHr=E_FAIL, setPsHr=E_FAIL, setVsC0Hr=E_FAIL, setTexHr=E_FAIL;
        HRESULT getBoundVsHr=E_FAIL, getBoundPsHr=E_FAIL, getBoundVsC0Hr=E_FAIL, getBoundTexHr=E_FAIL;
        HRESULT drawHr=E_PENDING;
        HRESULT getHeldVsHr=E_PENDING, getHeldPsHr=E_PENDING, getHeldVsC0Hr=E_PENDING, getHeldTexHr=E_PENDING;
        HRESULT restoreVsHr=E_PENDING, restorePsHr=E_PENDING, restoreVsC0Hr=E_PENDING, restoreTexHr=E_PENDING;
        HRESULT getRestoredVsHr=E_PENDING, getRestoredPsHr=E_PENDING, getRestoredVsC0Hr=E_PENDING, getRestoredTexHr=E_PENDING;

        void* targetBefore=nullptr; void* targetAfter=nullptr; void* dsBefore=nullptr; void* dsAfter=nullptr;
        void* beforeVS=nullptr; void* beforePS=nullptr; void* beforeTex0=nullptr;
        void* boundVS=nullptr; void* boundPS=nullptr; void* boundTex0=nullptr;
        void* heldVS=nullptr; void* heldPS=nullptr; void* heldTex0=nullptr;
        void* restoredVS=nullptr; void* restoredPS=nullptr; void* restoredTex0=nullptr;
        float beforeVsC0[4]={}, boundVsC0[4]={}, heldVsC0[4]={}, restoredVsC0[4]={};

        constexpr uint32_t kSamplerStateCount = 7u;
        const DWORD samplerTypes[kSamplerStateCount] = {
            kSampAddressU, kSampAddressV, kSampMagFilter, kSampMinFilter,
            kSampMipFilter, kSampMaxAnisotropy, kSampSrgbTexture
        };
        const DWORD samplerTargets[kSamplerStateCount] = {
            kTextureAddressClamp, kTextureAddressClamp,
            kTextureFilterLinear, kTextureFilterLinear, kTextureFilterLinear,
            1u, 0u
        };
        DWORD samplerBefore[kSamplerStateCount]={}, samplerBound[kSamplerStateCount]={};
        DWORD samplerHeld[kSamplerStateCount]={}, samplerRestored[kSamplerStateCount]={};
        uint32_t samplerCaptureMask=0u, samplerSetMask=0u, samplerBoundMask=0u;
        uint32_t samplerHeldMask=0u, samplerRestoreSetMask=0u, samplerRestoreMask=0u;
        constexpr uint32_t kSamplerAllMask = (1u << kSamplerStateCount) - 1u;

        constexpr uint32_t kRenderStateCount = 11u;
        const DWORD renderTypes[kRenderStateCount] = {
            kRSZEnable, kRSZWriteEnable, kRSAlphaTestEnable, kRSCullMode,
            kRSAlphaBlendEnable, kRSFogEnable, kRSStencilEnable, kRSLighting,
            kRSColorWriteEnable, kRSScissorTestEnable, kRSSrgbWriteEnable
        };
        const DWORD renderTargets[kRenderStateCount] = {
            0u, 0u, 0u, 1u,
            0u, 0u, 0u, 0u,
            0xFu, 0u, 0u
        };
        DWORD renderBefore[kRenderStateCount]={}, renderBound[kRenderStateCount]={};
        DWORD renderHeld[kRenderStateCount]={}, renderRestored[kRenderStateCount]={};
        uint32_t renderCaptureMask=0u, renderSetMask=0u, renderBoundMask=0u;
        uint32_t renderHeldMask=0u, renderRestoreSetMask=0u, renderRestoreMask=0u;
        constexpr uint32_t kRenderAllMask = (1u << kRenderStateCount) - 1u;

        bool apiReady=false, sourceCreated=false, sourceCopied=false, sourceTargetAlias=true;
        bool sceneTargetVerified=false, shaderBindVerified=false, vsC0Verified=false;
        bool textureBindVerified=false, samplerBindVerified=false, renderBindVerified=false;
        bool drawHeldShaders=false, drawHeldVsC0=false, drawHeldTexture=false;
        bool drawHeldSampler=false, drawHeldRender=false, drew=false;
        bool shaderRestoreVerified=false, vsC0RestoreVerified=false, textureRestoreVerified=false;
        bool samplerRestoreVerified=false, renderRestoreVerified=false, rtStable=false, dsStable=false;

        const uint32_t sceneWrapper = CurrentSceneRT();
        const uint32_t sceneSurface = RawSurfaceDirectFromWrapper(sceneWrapper);
        uint32_t targetContainerTex=0u;
        HRESULT targetContainerHr=E_PENDING;

        if (dvt && IsReadableMemory(dvt, (kD3DGetSamplerStateVtableIndex + 1u) * sizeof(void*)) &&
            IsExecutableMemory(dvt[kD3DStretchRectVtableIndex]) &&
            IsExecutableMemory(dvt[kD3DGetRenderTargetVtableIndex]) &&
            IsExecutableMemory(dvt[kD3DGetDepthStencilSurfaceVtableIndex]) &&
            IsExecutableMemory(dvt[kD3DSetVertexShaderVtableIndex]) &&
            IsExecutableMemory(dvt[kD3DGetVertexShaderVtableIndex]) &&
            IsExecutableMemory(dvt[kD3DSetVertexShaderConstantFVtableIndex]) &&
            IsExecutableMemory(dvt[kD3DGetVertexShaderConstantFVtableIndex]) &&
            IsExecutableMemory(dvt[kD3DSetPixelShaderVtableIndex]) &&
            IsExecutableMemory(dvt[kD3DGetPixelShaderVtableIndex]) &&
            IsExecutableMemory(dvt[kD3DSetTextureVtableIndex]) &&
            IsExecutableMemory(dvt[kD3DGetTextureVtableIndex]) &&
            IsExecutableMemory(dvt[kD3DSetSamplerStateVtableIndex]) &&
            IsExecutableMemory(dvt[kD3DGetSamplerStateVtableIndex]) &&
            IsExecutableMemory(dvt[kD3DSetRenderStateVtableIndex]) &&
            IsExecutableMemory(dvt[kD3DGetRenderStateVtableIndex]))
        {
            const auto stretchRect = reinterpret_cast<D3DStretchRect_t>(dvt[kD3DStretchRectVtableIndex]);
            const auto getRT = reinterpret_cast<D3DGetRenderTarget_t>(dvt[kD3DGetRenderTargetVtableIndex]);
            const auto getDS = reinterpret_cast<D3DGetDepthStencilSurface_t>(dvt[kD3DGetDepthStencilSurfaceVtableIndex]);
            const auto setVS = reinterpret_cast<D3DSetVertexShader_t>(dvt[kD3DSetVertexShaderVtableIndex]);
            const auto getVS = reinterpret_cast<D3DGetVertexShader_t>(dvt[kD3DGetVertexShaderVtableIndex]);
            const auto setVSC0 = reinterpret_cast<D3DSetVertexShaderConstantF_t>(dvt[kD3DSetVertexShaderConstantFVtableIndex]);
            const auto getVSC0 = reinterpret_cast<D3DGetVertexShaderConstantF_t>(dvt[kD3DGetVertexShaderConstantFVtableIndex]);
            const auto setPS = reinterpret_cast<D3DSetPixelShader_t>(dvt[kD3DSetPixelShaderVtableIndex]);
            const auto getPS = reinterpret_cast<D3DGetPixelShader_t>(dvt[kD3DGetPixelShaderVtableIndex]);
            const auto setTexture = reinterpret_cast<D3DSetTexture_t>(dvt[kD3DSetTextureVtableIndex]);
            const auto getTexture = reinterpret_cast<D3DGetTexture_t>(dvt[kD3DGetTextureVtableIndex]);
            const auto setSampler = reinterpret_cast<D3DSetSamplerState_t>(dvt[kD3DSetSamplerStateVtableIndex]);
            const auto getSampler = reinterpret_cast<D3DGetSamplerState_t>(dvt[kD3DGetSamplerStateVtableIndex]);
            const auto setRender = reinterpret_cast<D3DSetRenderState_t>(dvt[kD3DSetRenderStateVtableIndex]);
            const auto getRender = reinterpret_cast<D3DGetRenderState_t>(dvt[kD3DGetRenderStateVtableIndex]);
            apiReady = true;

            getRtBeforeHr = getRT(device, 0u, &targetBefore);
            getDsBeforeHr = getDS(device, &dsBefore);
            sceneTargetVerified = SUCCEEDED(getRtBeforeHr) && targetBefore && sceneSurface >= 0x10000u &&
                static_cast<uint32_t>(reinterpret_cast<uintptr_t>(targetBefore)) == sceneSurface;
            if (targetBefore)
                targetContainerTex = RawTextureFromSurface(
                    static_cast<uint32_t>(reinterpret_cast<uintptr_t>(targetBefore)), &targetContainerHr);

            sourceCreated = SUCCEEDED(getRtBeforeHr) && targetBefore &&
                EnsureRetailXboxImageZoomSafeSource(device, targetBefore);
            if (sourceCreated && s_retailXboxImageZoomSourceSurface && s_retailXboxImageZoomSourceTexture)
            {
                stretchHr = stretchRect(device, targetBefore, nullptr,
                    s_retailXboxImageZoomSourceSurface, nullptr, kD3DFilterNone);
                sourceCopied = SUCCEEDED(stretchHr);
                const uint32_t sourceSurfaceValue = static_cast<uint32_t>(
                    reinterpret_cast<uintptr_t>(s_retailXboxImageZoomSourceSurface));
                const uint32_t sourceTextureValue = static_cast<uint32_t>(
                    reinterpret_cast<uintptr_t>(s_retailXboxImageZoomSourceTexture));
                const bool surfaceDistinct = sourceSurfaceValue != 0u &&
                    sourceSurfaceValue != static_cast<uint32_t>(reinterpret_cast<uintptr_t>(targetBefore));
                const bool textureDistinct = targetContainerTex == 0u || sourceTextureValue != targetContainerTex;
                sourceTargetAlias = !(surfaceDistinct && textureDistinct);
            }

            getVsHr=getVS(device,&beforeVS); getPsHr=getPS(device,&beforePS);
            getVsC0Hr=getVSC0(device,0u,beforeVsC0,1u); getTexHr=getTexture(device,0u,&beforeTex0);
            for (uint32_t n=0;n<kSamplerStateCount;++n)
                if (SUCCEEDED(getSampler(device,0u,samplerTypes[n],&samplerBefore[n]))) samplerCaptureMask|=(1u<<n);
            for (uint32_t n=0;n<kRenderStateCount;++n)
                if (SUCCEEDED(getRender(device,renderTypes[n],&renderBefore[n]))) renderCaptureMask|=(1u<<n);

            const bool preflight = sourceCopied && !sourceTargetAlias && sceneTargetVerified && shaderIdentityReady &&
                vsValue >= 0x10000u && psValue >= 0x10000u && SUCCEEDED(getVsHr) && SUCCEEDED(getPsHr) &&
                SUCCEEDED(getVsC0Hr) && SUCCEEDED(getTexHr) && samplerCaptureMask==kSamplerAllMask &&
                renderCaptureMask==kRenderAllMask;

            if (preflight)
            {
                setVsHr=setVS(device,reinterpret_cast<void*>(static_cast<uintptr_t>(vsValue)));
                setPsHr=setPS(device,reinterpret_cast<void*>(static_cast<uintptr_t>(psValue)));
                setVsC0Hr=setVSC0(device,0u,zoomC0,1u);
                setTexHr=setTexture(device,0u,s_retailXboxImageZoomSourceTexture);
                if (SUCCEEDED(setVsHr)&&SUCCEEDED(setPsHr)&&SUCCEEDED(setVsC0Hr)&&SUCCEEDED(setTexHr))
                {
                    for (uint32_t n=0;n<kSamplerStateCount;++n)
                        if (SUCCEEDED(setSampler(device,0u,samplerTypes[n],samplerTargets[n]))) samplerSetMask|=(1u<<n);
                    for (uint32_t n=0;n<kRenderStateCount;++n)
                        if (SUCCEEDED(setRender(device,renderTypes[n],renderTargets[n]))) renderSetMask|=(1u<<n);

                    getBoundVsHr=getVS(device,&boundVS); getBoundPsHr=getPS(device,&boundPS);
                    getBoundVsC0Hr=getVSC0(device,0u,boundVsC0,1u); getBoundTexHr=getTexture(device,0u,&boundTex0);
                    for (uint32_t n=0;n<kSamplerStateCount;++n)
                        if (SUCCEEDED(getSampler(device,0u,samplerTypes[n],&samplerBound[n])) && samplerBound[n]==samplerTargets[n]) samplerBoundMask|=(1u<<n);
                    for (uint32_t n=0;n<kRenderStateCount;++n)
                        if (SUCCEEDED(getRender(device,renderTypes[n],&renderBound[n])) && renderBound[n]==renderTargets[n]) renderBoundMask|=(1u<<n);

                    shaderBindVerified=SUCCEEDED(getBoundVsHr)&&SUCCEEDED(getBoundPsHr)&&
                        static_cast<uint32_t>(reinterpret_cast<uintptr_t>(boundVS))==vsValue &&
                        static_cast<uint32_t>(reinterpret_cast<uintptr_t>(boundPS))==psValue;
                    vsC0Verified=SUCCEEDED(getBoundVsC0Hr)&&memcmp(boundVsC0,zoomC0,sizeof(zoomC0))==0;
                    textureBindVerified=SUCCEEDED(getBoundTexHr)&&boundTex0==s_retailXboxImageZoomSourceTexture;
                    samplerBindVerified=samplerSetMask==kSamplerAllMask&&samplerBoundMask==kSamplerAllMask;
                    renderBindVerified=renderSetMask==kRenderAllMask&&renderBoundMask==kRenderAllMask;

                    if (shaderBindVerified && vsC0Verified && textureBindVerified &&
                        samplerBindVerified && renderBindVerified)
                    {
                        drawHr=s_rawDrawPrimitive(device,primitiveType,startVertex,primitiveCount);
                        drew=true;
                        InterlockedIncrement(&s_retailXboxImageZoomDrawCount);

                        getHeldVsHr=getVS(device,&heldVS); getHeldPsHr=getPS(device,&heldPS);
                        getHeldVsC0Hr=getVSC0(device,0u,heldVsC0,1u); getHeldTexHr=getTexture(device,0u,&heldTex0);
                        for (uint32_t n=0;n<kSamplerStateCount;++n)
                            if (SUCCEEDED(getSampler(device,0u,samplerTypes[n],&samplerHeld[n])) && samplerHeld[n]==samplerTargets[n]) samplerHeldMask|=(1u<<n);
                        for (uint32_t n=0;n<kRenderStateCount;++n)
                            if (SUCCEEDED(getRender(device,renderTypes[n],&renderHeld[n])) && renderHeld[n]==renderTargets[n]) renderHeldMask|=(1u<<n);
                        drawHeldShaders=SUCCEEDED(getHeldVsHr)&&SUCCEEDED(getHeldPsHr)&&heldVS==boundVS&&heldPS==boundPS;
                        drawHeldVsC0=SUCCEEDED(getHeldVsC0Hr)&&memcmp(heldVsC0,zoomC0,sizeof(zoomC0))==0;
                        drawHeldTexture=SUCCEEDED(getHeldTexHr)&&heldTex0==s_retailXboxImageZoomSourceTexture;
                        drawHeldSampler=samplerHeldMask==kSamplerAllMask;
                        drawHeldRender=renderHeldMask==kRenderAllMask;
                    }
                }

                // Restore exact state after the one optional ImageZoom draw.
                for (uint32_t n=0;n<kRenderStateCount;++n)
                    if (SUCCEEDED(setRender(device,renderTypes[n],renderBefore[n]))) renderRestoreSetMask|=(1u<<n);
                for (uint32_t n=0;n<kSamplerStateCount;++n)
                    if (SUCCEEDED(setSampler(device,0u,samplerTypes[n],samplerBefore[n]))) samplerRestoreSetMask|=(1u<<n);
                restoreTexHr=setTexture(device,0u,beforeTex0);
                restoreVsC0Hr=setVSC0(device,0u,beforeVsC0,1u);
                restorePsHr=setPS(device,beforePS);
                restoreVsHr=setVS(device,beforeVS);

                getRestoredVsHr=getVS(device,&restoredVS); getRestoredPsHr=getPS(device,&restoredPS);
                getRestoredVsC0Hr=getVSC0(device,0u,restoredVsC0,1u); getRestoredTexHr=getTexture(device,0u,&restoredTex0);
                for (uint32_t n=0;n<kSamplerStateCount;++n)
                    if (SUCCEEDED(getSampler(device,0u,samplerTypes[n],&samplerRestored[n])) && samplerRestored[n]==samplerBefore[n]) samplerRestoreMask|=(1u<<n);
                for (uint32_t n=0;n<kRenderStateCount;++n)
                    if (SUCCEEDED(getRender(device,renderTypes[n],&renderRestored[n])) && renderRestored[n]==renderBefore[n]) renderRestoreMask|=(1u<<n);

                shaderRestoreVerified=SUCCEEDED(restoreVsHr)&&SUCCEEDED(restorePsHr)&&
                    SUCCEEDED(getRestoredVsHr)&&SUCCEEDED(getRestoredPsHr)&&restoredVS==beforeVS&&restoredPS==beforePS;
                vsC0RestoreVerified=SUCCEEDED(restoreVsC0Hr)&&SUCCEEDED(getRestoredVsC0Hr)&&
                    memcmp(restoredVsC0,beforeVsC0,sizeof(beforeVsC0))==0;
                textureRestoreVerified=SUCCEEDED(restoreTexHr)&&SUCCEEDED(getRestoredTexHr)&&restoredTex0==beforeTex0;
                samplerRestoreVerified=samplerRestoreSetMask==kSamplerAllMask&&samplerRestoreMask==kSamplerAllMask;
                renderRestoreVerified=renderRestoreSetMask==kRenderAllMask&&renderRestoreMask==kRenderAllMask;
            }

            getRtAfterHr=getRT(device,0u,&targetAfter); getDsAfterHr=getDS(device,&dsAfter);
            rtStable=SUCCEEDED(getRtBeforeHr)&&SUCCEEDED(getRtAfterHr)&&targetAfter==targetBefore;
            dsStable=getDsAfterHr==getDsBeforeHr&&dsAfter==dsBefore;
        }

        const bool pass = drew && SUCCEEDED(drawHr) && apiReady && shaderIdentityReady && sourceCopied &&
            !sourceTargetAlias && sceneTargetVerified && shaderBindVerified && vsC0Verified && textureBindVerified &&
            samplerBindVerified && renderBindVerified && drawHeldShaders && drawHeldVsC0 && drawHeldTexture &&
            drawHeldSampler && drawHeldRender && shaderRestoreVerified && vsC0RestoreVerified &&
            textureRestoreVerified && samplerRestoreVerified && renderRestoreVerified && rtStable && dsStable;

        if (pass)
            InterlockedIncrement(&s_retailXboxImageZoomDrawPassCount);
        else if (drew)
        {
            InterlockedIncrement(&s_retailXboxImageZoomDrawFailCount);
            InterlockedExchange(&s_retailXboxImageZoomDrawDisabled,1);
        }
        else
            InterlockedIncrement(&s_retailXboxImageZoomDrawFailCount);

        if (ShouldLogDebugXboxIntegrationDetail(attempt) || !pass)
        {
            Event* event=ReserveEvent(EventType::RetailXboxImageZoomNativeDraw);
            if (event)
            {
                event->a=s_presentCount; event->b=static_cast<uint32_t>(attempt); event->c=gate?1u:0u;
                event->d=pass?1u:0u; event->e=static_cast<uint32_t>(reinterpret_cast<uintptr_t>(s_retailXboxImageZoomSourceTexture));
                event->f=static_cast<uint32_t>(reinterpret_cast<uintptr_t>(s_retailXboxImageZoomSourceSurface));
                event->g=static_cast<uint32_t>(reinterpret_cast<uintptr_t>(targetBefore)); event->h=sceneSurface;
                event->i=sourceTargetAlias?1u:0u; event->j=drew?1u:0u;
                event->extra[0]=static_cast<uint32_t>(stretchHr); event->extra[1]=vsValue; event->extra[2]=psValue;
                event->extra[3]=s_retailXboxZBlurVsHash; event->extra[4]=s_retailXboxZBlurPsHash;
                event->extra[5]=FloatBits(s_cameraMotionZBlurLastMotion); event->extra[6]=FloatBits(s_cameraMotionZBlurLastBlurPixels);
                event->extra[7]=FloatBits(zoomScale); event->extra[8]=shaderBindVerified?1u:0u; event->extra[9]=vsC0Verified?1u:0u;
                event->extra[10]=textureBindVerified?1u:0u; event->extra[11]=samplerBindVerified?1u:0u; event->extra[12]=renderBindVerified?1u:0u;
                event->extra[13]=static_cast<uint32_t>(drawHr); event->extra[14]=drawHeldShaders?1u:0u; event->extra[15]=drawHeldVsC0?1u:0u;
                event->extra[16]=drawHeldTexture?1u:0u; event->extra[17]=drawHeldSampler?1u:0u; event->extra[18]=drawHeldRender?1u:0u;
                event->extra[19]=shaderRestoreVerified?1u:0u; event->extra[20]=vsC0RestoreVerified?1u:0u; event->extra[21]=textureRestoreVerified?1u:0u;
                event->extra[22]=samplerRestoreVerified?1u:0u; event->extra[23]=renderRestoreVerified?1u:0u; event->extra[24]=rtStable?1u:0u;
                event->extra[25]=dsStable?1u:0u; event->extra[26]=static_cast<uint32_t>(s_retailXboxImageZoomDrawPassCount);
                event->extra[27]=static_cast<uint32_t>(s_retailXboxImageZoomDrawFailCount);
            }
        }

        SafeReleaseCom(targetBefore); SafeReleaseCom(targetAfter); SafeReleaseCom(dsBefore); SafeReleaseCom(dsAfter);
        SafeReleaseCom(beforeVS); SafeReleaseCom(beforePS); SafeReleaseCom(beforeTex0);
        SafeReleaseCom(boundVS); SafeReleaseCom(boundPS); SafeReleaseCom(boundTex0);
        SafeReleaseCom(heldVS); SafeReleaseCom(heldPS); SafeReleaseCom(heldTex0);
        SafeReleaseCom(restoredVS); SafeReleaseCom(restoredPS); SafeReleaseCom(restoredTex0);
        return drew;
    }

    void RunRetailXboxDepthFeedProbe(uintptr_t returnAddress)
    {
        if (!IsXboxPostProcessRoute() || returnAddress != kCompositeFinalQuadReturn)
            return;

        const LONG attempt = InterlockedIncrement(&s_retailXboxDepthFeedAttemptCount);
        const uint32_t expectedFinal = ReadU32(kCompositePSFinal);
        const uint32_t dofShaderValue = ReadU32(kCompositePSDofFinal);
        const uint32_t depthTextureValue = s_provenIntzDofDepthTexture;
        const uint32_t depthAge = (s_provenIntzDofPresent <= s_presentCount)
            ? (s_presentCount - s_provenIntzDofPresent) : 0xFFFFFFFFu;
        const bool depthReady =
            s_mode4ReszDepthValidated &&
            s_reszDepthResolveState == 3 &&
            s_provenIntzDofState == 1 &&
            s_provenIntzDofPresent == s_presentCount &&
            depthTextureValue >= 0x10000u &&
            depthTextureValue == static_cast<uint32_t>(
                reinterpret_cast<uintptr_t>(s_mode4PackedDepthTexture));

        HRESULT queryHr = E_FAIL;
        HRESULT fetchHr = E_PENDING;
        HRESULT beforeGetHr = E_FAIL;
        HRESULT tex0Hr = E_FAIL;
        HRESULT tex1Hr = E_FAIL;
        HRESULT c0Hr = E_FAIL;
        HRESULT bindDepthHr = E_FAIL;
        HRESULT boundDepthGetHr = E_FAIL;
        HRESULT bindDofHr = E_FAIL;
        HRESULT boundPsGetHr = E_FAIL;
        HRESULT restorePsHr = E_FAIL;
        HRESULT restoreDepthHr = E_FAIL;
        HRESULT restoredDepthGetHr = E_FAIL;

        void* device = nullptr;
        D3DCreatePixelShader_t createPixelShader = nullptr;
        D3DSetPixelShader_t setPixelShader = nullptr;
        D3DGetPixelShader_t getPixelShader = nullptr;
        void* beforePS = nullptr;
        void* boundPS = nullptr;
        void* texture0 = nullptr;
        void* originalTexture1 = nullptr;
        void* boundTexture1 = nullptr;
        void* restoredTexture1 = nullptr;
        float c0[4] = {};
        uint32_t dofBytes = 0u;
        uint32_t dofHash = 0u;

        bool apiReady = ResolveMode5FinalShaderDeviceApis(
            device, createPixelShader, setPixelShader, getPixelShader);
        bool dofValid = false;
        bool beforeVerified = false;
        bool depthBindVerified = false;
        bool dofBindVerified = false;
        bool shaderRestoreVerified = false;
        bool depthRestoreVerified = false;
        bool sourceColorPresent = false;
        bool c0Readable = false;

        D3DGetTexture_t getTexture = nullptr;
        D3DSetTexture_t setTexture = nullptr;
        D3DGetPixelShaderConstantF_t getPSC0 = nullptr;

        if (apiReady && dofShaderValue >= 0x10000u)
        {
            void* dofShader = reinterpret_cast<void*>(static_cast<uintptr_t>(dofShaderValue));
            dofValid = QueryPixelShaderIdentity(
                dofShader, dofBytes, dofHash, queryHr, fetchHr);
            if (dofValid)
            {
                s_retailXboxDofShaderBytes = dofBytes;
                s_retailXboxDofShaderHash = dofHash;
            }

            void** dvt = *reinterpret_cast<void***>(device);
            if (dvt && IsReadableMemory(dvt,
                    (kD3DGetPixelShaderConstantFVtableIndex + 1u) * sizeof(void*)) &&
                IsExecutableMemory(dvt[kD3DGetTextureVtableIndex]) &&
                IsExecutableMemory(dvt[kD3DSetTextureVtableIndex]) &&
                IsExecutableMemory(dvt[kD3DGetPixelShaderConstantFVtableIndex]))
            {
                getTexture = reinterpret_cast<D3DGetTexture_t>(dvt[kD3DGetTextureVtableIndex]);
                setTexture = reinterpret_cast<D3DSetTexture_t>(dvt[kD3DSetTextureVtableIndex]);
                getPSC0 = reinterpret_cast<D3DGetPixelShaderConstantF_t>(
                    dvt[kD3DGetPixelShaderConstantFVtableIndex]);
                tex0Hr = getTexture(device, 0u, &texture0);
                tex1Hr = getTexture(device, 1u, &originalTexture1);
                c0Hr = getPSC0(device, 0u, c0, 1u);
                sourceColorPresent = SUCCEEDED(tex0Hr) && texture0 != nullptr;
                c0Readable = SUCCEEDED(c0Hr);
            }

            beforeGetHr = getPixelShader(device, &beforePS);
            beforeVerified = SUCCEEDED(beforeGetHr) && beforePS &&
                reinterpret_cast<uintptr_t>(beforePS) == expectedFinal;

            if (depthReady && dofValid && beforeVerified && getTexture && setTexture && c0Readable)
            {
                void* expectedDepth = reinterpret_cast<void*>(
                    static_cast<uintptr_t>(depthTextureValue));
                bindDepthHr = setTexture(device, 1u, expectedDepth);
                if (SUCCEEDED(bindDepthHr))
                {
                    boundDepthGetHr = getTexture(device, 1u, &boundTexture1);
                    depthBindVerified = SUCCEEDED(boundDepthGetHr) &&
                        boundTexture1 == expectedDepth;
                }

                if (depthBindVerified)
                {
                    bindDofHr = setPixelShader(device, dofShader);
                    if (SUCCEEDED(bindDofHr))
                    {
                        boundPsGetHr = getPixelShader(device, &boundPS);
                        dofBindVerified = SUCCEEDED(boundPsGetHr) && boundPS == dofShader;
                    }
                }

                restorePsHr = setPixelShader(device, beforePS);
                if (SUCCEEDED(restorePsHr))
                {
                    void* restoredPS = nullptr;
                    const HRESULT restoredPsGetHr = getPixelShader(device, &restoredPS);
                    shaderRestoreVerified = SUCCEEDED(restoredPsGetHr) && restoredPS == beforePS;
                    SafeReleaseCom(restoredPS);
                }

                restoreDepthHr = setTexture(device, 1u, originalTexture1);
                if (SUCCEEDED(restoreDepthHr))
                {
                    restoredDepthGetHr = getTexture(device, 1u, &restoredTexture1);
                    depthRestoreVerified = SUCCEEDED(restoredDepthGetHr) &&
                        restoredTexture1 == originalTexture1;
                }
            }
        }

        const bool pass = apiReady && depthReady && dofValid && beforeVerified &&
            sourceColorPresent && c0Readable && depthBindVerified && dofBindVerified &&
            shaderRestoreVerified && depthRestoreVerified;
        if (pass)
        {
            InterlockedIncrement(&s_retailXboxDepthFeedPassCount);
            // R2 consumes the same-present depth only as a verified input probe.
            // No F18 pixels are drawn; mark the one-frame state restored/closed.
            InterlockedExchange(&s_provenIntzDofState, 3);
        }
        else
        {
            InterlockedIncrement(&s_retailXboxDepthFeedFailCount);
        }

        if (ShouldLogSparse(attempt) || !pass)
        {
            Event* event = ReserveEvent(EventType::RetailXboxDepthFeedProbe);
            if (event)
            {
                event->a = s_presentCount;
                event->b = static_cast<uint32_t>(attempt);
                event->c = static_cast<uint32_t>(returnAddress);
                event->d = depthTextureValue;
                event->e = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(texture0));
                event->f = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(originalTexture1));
                event->g = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(boundTexture1));
                event->h = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(restoredTexture1));
                event->i = dofShaderValue;
                event->j = pass ? 1u : 0u;
                event->extra[0] = depthReady ? 1u : 0u;
                event->extra[1] = static_cast<uint32_t>(s_reszDepthResolveState);
                event->extra[2] = s_mode4ReszDepthValidated ? 1u : 0u;
                event->extra[3] = s_provenIntzDofPresent;
                event->extra[4] = depthAge;
                event->extra[5] = static_cast<uint32_t>(bindDepthHr);
                event->extra[6] = static_cast<uint32_t>(boundDepthGetHr);
                event->extra[7] = static_cast<uint32_t>(bindDofHr);
                event->extra[8] = static_cast<uint32_t>(boundPsGetHr);
                event->extra[9] = static_cast<uint32_t>(restorePsHr);
                event->extra[10] = static_cast<uint32_t>(restoreDepthHr);
                event->extra[11] = static_cast<uint32_t>(restoredDepthGetHr);
                event->extra[12] = depthBindVerified ? 1u : 0u;
                event->extra[13] = dofBindVerified ? 1u : 0u;
                event->extra[14] = shaderRestoreVerified ? 1u : 0u;
                event->extra[15] = depthRestoreVerified ? 1u : 0u;
                event->extra[16] = static_cast<uint32_t>(c0Hr);
                memcpy(&event->extra[17], &c0[0], sizeof(uint32_t));
                memcpy(&event->extra[18], &c0[1], sizeof(uint32_t));
                memcpy(&event->extra[19], &c0[2], sizeof(uint32_t));
                memcpy(&event->extra[20], &c0[3], sizeof(uint32_t));
                event->extra[21] = static_cast<uint32_t>(s_retailXboxDepthFeedPassCount);
                event->extra[22] = static_cast<uint32_t>(s_retailXboxDepthFeedFailCount);
                event->extra[23] = dofValid ? 1u : 0u;
                event->extra[24] = sourceColorPresent ? 1u : 0u;
                event->extra[25] = c0Readable ? 1u : 0u;
                event->extra[26] = 0u; // drawWithDof: deliberately OFF in R2.
                event->extra[27] = s_postProcessRoute;
            }
        }

        SafeReleaseCom(texture0);
        SafeReleaseCom(originalTexture1);
        SafeReleaseCom(boundTexture1);
        SafeReleaseCom(restoredTexture1);
        SafeReleaseCom(beforePS);
        SafeReleaseCom(boundPS);
    }

    bool RunRetailXboxDofNativeDraw(
        void* a1, void* a2, unsigned int a3, int a4,
        uintptr_t returnAddress, int& resultOut)
    {
        resultOut = 0;
        if (!IsXboxPostProcessRoute() || returnAddress != kCompositeFinalQuadReturn)
            return false;

        const LONG attempt = InterlockedIncrement(&s_retailXboxDofDrawAttemptCount);
        if (s_retailXboxDofDrawDisabled != 0)
        {
            InterlockedIncrement(&s_retailXboxDofDrawFallbackCount);
            return false;
        }

        const uint32_t expectedFinal = ReadU32(kCompositePSFinal);
        const uint32_t dofShaderValue = ReadU32(kCompositePSDofFinal);
        const uint32_t depthTextureValue = s_provenIntzDofDepthTexture;
        const uint32_t depthAge = (s_provenIntzDofPresent <= s_presentCount)
            ? (s_presentCount - s_provenIntzDofPresent) : 0xFFFFFFFFu;
        const bool depthReady =
            s_mode4ReszDepthValidated &&
            s_reszDepthResolveState == 3 &&
            s_provenIntzDofState == 1 &&
            s_provenIntzDofPresent == s_presentCount &&
            depthTextureValue >= 0x10000u &&
            depthTextureValue == static_cast<uint32_t>(
                reinterpret_cast<uintptr_t>(s_mode4PackedDepthTexture));

        HRESULT queryHr = E_FAIL;
        HRESULT fetchHr = E_PENDING;
        HRESULT beforeGetHr = E_FAIL;
        HRESULT tex0Hr = E_FAIL;
        HRESULT tex1Hr = E_FAIL;
        HRESULT oldC0Hr = E_FAIL;
        HRESULT bindDepthHr = E_FAIL;
        HRESULT boundDepthGetHr = E_FAIL;
        HRESULT setC0Hr = E_FAIL;
        HRESULT boundC0Hr = E_FAIL;
        HRESULT bindDofHr = E_FAIL;
        HRESULT boundPsGetHr = E_FAIL;
        HRESULT afterDrawPsGetHr = E_PENDING;
        HRESULT afterDrawDepthGetHr = E_PENDING;
        HRESULT restorePsHr = E_PENDING;
        HRESULT restoreDepthHr = E_PENDING;
        HRESULT restoreC0Hr = E_PENDING;
        HRESULT restoredPsGetHr = E_PENDING;
        HRESULT restoredDepthGetHr = E_PENDING;
        HRESULT restoredC0GetHr = E_PENDING;

        void* device = nullptr;
        D3DCreatePixelShader_t createPixelShader = nullptr;
        D3DSetPixelShader_t setPixelShader = nullptr;
        D3DGetPixelShader_t getPixelShader = nullptr;
        D3DGetTexture_t getTexture = nullptr;
        D3DSetTexture_t setTexture = nullptr;
        D3DGetPixelShaderConstantF_t getPSC0 = nullptr;
        D3DSetPixelShaderConstantF_t setPSC0 = nullptr;

        void* beforePS = nullptr;
        void* boundPS = nullptr;
        void* afterDrawPS = nullptr;
        void* restoredPS = nullptr;
        void* texture0 = nullptr;
        void* originalTexture1 = nullptr;
        void* boundTexture1 = nullptr;
        void* afterDrawTexture1 = nullptr;
        void* restoredTexture1 = nullptr;
        float oldC0[4] = {};
        float dofC0[4] = {};
        float boundC0[4] = {};
        float restoredC0[4] = {};
        uint32_t dofBytes = 0u;
        uint32_t dofHash = 0u;

        bool apiReady = ResolveMode5FinalShaderDeviceApis(
            device, createPixelShader, setPixelShader, getPixelShader);
        bool dofValid = false;
        bool beforeVerified = false;
        bool sourceColorPresent = false;
        bool oldC0Readable = false;
        bool dynamicC0Valid = false;
        bool depthBindVerified = false;
        bool c0BindVerified = false;
        bool dofBindVerified = false;
        bool drawHeldDof = false;
        bool drawHeldDepth = false;
        bool shaderRestoreVerified = false;
        bool depthRestoreVerified = false;
        bool c0RestoreVerified = false;
        bool drewDof = false;

        if (apiReady && dofShaderValue >= 0x10000u)
        {
            void* dofShader = reinterpret_cast<void*>(static_cast<uintptr_t>(dofShaderValue));
            dofValid = QueryPixelShaderIdentity(
                dofShader, dofBytes, dofHash, queryHr, fetchHr);
            if (dofValid)
            {
                s_retailXboxDofShaderBytes = dofBytes;
                s_retailXboxDofShaderHash = dofHash;
            }

            void** dvt = *reinterpret_cast<void***>(device);
            if (dvt && IsReadableMemory(dvt,
                    (kD3DGetPixelShaderConstantFVtableIndex + 1u) * sizeof(void*)) &&
                IsExecutableMemory(dvt[kD3DGetTextureVtableIndex]) &&
                IsExecutableMemory(dvt[kD3DSetTextureVtableIndex]) &&
                IsExecutableMemory(dvt[kD3DGetPixelShaderConstantFVtableIndex]) &&
                IsExecutableMemory(dvt[kD3DSetPixelShaderConstantFVtableIndex]))
            {
                getTexture = reinterpret_cast<D3DGetTexture_t>(dvt[kD3DGetTextureVtableIndex]);
                setTexture = reinterpret_cast<D3DSetTexture_t>(dvt[kD3DSetTextureVtableIndex]);
                getPSC0 = reinterpret_cast<D3DGetPixelShaderConstantF_t>(
                    dvt[kD3DGetPixelShaderConstantFVtableIndex]);
                setPSC0 = reinterpret_cast<D3DSetPixelShaderConstantF_t>(
                    dvt[kD3DSetPixelShaderConstantFVtableIndex]);

                tex0Hr = getTexture(device, 0u, &texture0);
                tex1Hr = getTexture(device, 1u, &originalTexture1);
                oldC0Hr = getPSC0(device, 0u, oldC0, 1u);
                sourceColorPresent = SUCCEEDED(tex0Hr) && texture0 != nullptr;
                oldC0Readable = SUCCEEDED(oldC0Hr);
            }

            beforeGetHr = getPixelShader(device, &beforePS);
            beforeVerified = SUCCEEDED(beforeGetHr) && beforePS &&
                reinterpret_cast<uintptr_t>(beforePS) == expectedFinal;

            // R3 deliberately requires the live near/far bridge to be valid.
            // Menus/non-world frames fail closed to the proven clamp rather than
            // drawing F18 with guessed/fallback c0 values.
            dynamicC0Valid = BuildMode4BloomDepthControl(dofC0);

            const bool preflight = depthReady && dofValid && beforeVerified &&
                sourceColorPresent && oldC0Readable && dynamicC0Valid &&
                getTexture && setTexture && getPSC0 && setPSC0;

            if (preflight)
            {
                void* expectedDepth = reinterpret_cast<void*>(
                    static_cast<uintptr_t>(depthTextureValue));
                bindDepthHr = setTexture(device, 1u, expectedDepth);
                if (SUCCEEDED(bindDepthHr))
                {
                    boundDepthGetHr = getTexture(device, 1u, &boundTexture1);
                    depthBindVerified = SUCCEEDED(boundDepthGetHr) &&
                        boundTexture1 == expectedDepth;
                }

                if (depthBindVerified)
                {
                    setC0Hr = setPSC0(device, 0u, dofC0, 1u);
                    if (SUCCEEDED(setC0Hr))
                    {
                        boundC0Hr = getPSC0(device, 0u, boundC0, 1u);
                        c0BindVerified = SUCCEEDED(boundC0Hr) &&
                            memcmp(boundC0, dofC0, sizeof(dofC0)) == 0;
                    }
                }

                if (c0BindVerified)
                {
                    bindDofHr = setPixelShader(device, dofShader);
                    if (SUCCEEDED(bindDofHr))
                    {
                        boundPsGetHr = getPixelShader(device, &boundPS);
                        dofBindVerified = SUCCEEDED(boundPsGetHr) && boundPS == dofShader;
                    }
                }

                if (dofBindVerified)
                {
                    // This is the first Retail Xbox F18 pixel-producing test.
                    // Crucially, it calls the game's EXISTING fullscreen helper
                    // exactly once; xeSM3 still does not issue its own draw.
                    resultOut = s_originalFullscreenQuad(a1, a2, a3, a4);
                    drewDof = true;
                    InterlockedIncrement(&s_retailXboxDofDrawCount);

                    afterDrawPsGetHr = getPixelShader(device, &afterDrawPS);
                    drawHeldDof = SUCCEEDED(afterDrawPsGetHr) && afterDrawPS == dofShader;
                    afterDrawDepthGetHr = getTexture(device, 1u, &afterDrawTexture1);
                    drawHeldDepth = SUCCEEDED(afterDrawDepthGetHr) &&
                        afterDrawTexture1 == expectedDepth;
                }

                // Restore in reverse dependency order before returning to SM3.
                restorePsHr = setPixelShader(device, beforePS);
                if (SUCCEEDED(restorePsHr))
                {
                    restoredPsGetHr = getPixelShader(device, &restoredPS);
                    shaderRestoreVerified = SUCCEEDED(restoredPsGetHr) && restoredPS == beforePS;
                }

                restoreDepthHr = setTexture(device, 1u, originalTexture1);
                if (SUCCEEDED(restoreDepthHr))
                {
                    restoredDepthGetHr = getTexture(device, 1u, &restoredTexture1);
                    depthRestoreVerified = SUCCEEDED(restoredDepthGetHr) &&
                        restoredTexture1 == originalTexture1;
                }

                restoreC0Hr = setPSC0(device, 0u, oldC0, 1u);
                if (SUCCEEDED(restoreC0Hr))
                {
                    restoredC0GetHr = getPSC0(device, 0u, restoredC0, 1u);
                    c0RestoreVerified = SUCCEEDED(restoredC0GetHr) &&
                        memcmp(restoredC0, oldC0, sizeof(oldC0)) == 0;
                }
            }
        }

        const bool pass = drewDof && depthReady && dofValid && beforeVerified &&
            sourceColorPresent && dynamicC0Valid && depthBindVerified && c0BindVerified &&
            dofBindVerified && drawHeldDof && drawHeldDepth &&
            shaderRestoreVerified && depthRestoreVerified && c0RestoreVerified;

        if (pass)
        {
            InterlockedIncrement(&s_retailXboxDofDrawPassCount);
            InterlockedIncrement(&s_retailXboxDepthFeedPassCount);
            InterlockedExchange(&s_provenIntzDofState, 3);

            // R5 probe happens only after the proven F18 final combine completed
            // and all R3 state was restored. It copies the finished target into a
            // dedicated source, binds/verifies ImageZoom state, restores it, and
            // performs ZERO ImageZoom draws / target writes.
            RunRetailXboxImageZoomRouteBindProbe();
        }
        else if (drewDof)
        {
            // Never issue a second fullscreen draw in the same frame. If an
            // integrity check fails after F18 has already drawn, disable future
            // F18 draws and let subsequent frames use the proven clamp fallback.
            InterlockedIncrement(&s_retailXboxDofDrawFailCount);
            InterlockedIncrement(&s_retailXboxDepthFeedFailCount);
            InterlockedExchange(&s_retailXboxDofDrawDisabled, 1);
        }
        else
        {
            InterlockedIncrement(&s_retailXboxDofDrawFallbackCount);
        }

        if (ShouldLogSparse(attempt) || drewDof || (!pass && depthReady))
        {
            Event* event = ReserveEvent(EventType::RetailXboxDofNativeDraw);
            if (event)
            {
                event->a = s_presentCount;
                event->b = static_cast<uint32_t>(attempt);
                event->c = static_cast<uint32_t>(returnAddress);
                event->d = depthTextureValue;
                event->e = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(texture0));
                event->f = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(originalTexture1));
                event->g = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(boundTexture1));
                event->h = dofShaderValue;
                event->i = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(afterDrawPS));
                event->j = pass ? 1u : 0u;
                event->extra[0] = depthReady ? 1u : 0u;
                event->extra[1] = static_cast<uint32_t>(s_reszDepthResolveState);
                event->extra[2] = s_mode4ReszDepthValidated ? 1u : 0u;
                event->extra[3] = s_provenIntzDofPresent;
                event->extra[4] = depthAge;
                event->extra[5] = dynamicC0Valid ? 1u : 0u;
                event->extra[6] = depthBindVerified ? 1u : 0u;
                event->extra[7] = c0BindVerified ? 1u : 0u;
                event->extra[8] = dofBindVerified ? 1u : 0u;
                event->extra[9] = drewDof ? 1u : 0u;
                event->extra[10] = drawHeldDof ? 1u : 0u;
                event->extra[11] = drawHeldDepth ? 1u : 0u;
                event->extra[12] = shaderRestoreVerified ? 1u : 0u;
                event->extra[13] = depthRestoreVerified ? 1u : 0u;
                event->extra[14] = c0RestoreVerified ? 1u : 0u;
                event->extra[15] = static_cast<uint32_t>(s_retailXboxDofDrawCount);
                event->extra[16] = static_cast<uint32_t>(s_retailXboxDofDrawPassCount);
                event->extra[17] = static_cast<uint32_t>(s_retailXboxDofDrawFallbackCount);
                event->extra[18] = static_cast<uint32_t>(s_retailXboxDofDrawFailCount);
                event->extra[19] = static_cast<uint32_t>(s_retailXboxDofDrawDisabled);
                event->extra[20] = FloatBits(dofC0[0]);
                event->extra[21] = FloatBits(dofC0[1]);
                event->extra[22] = FloatBits(dofC0[2]);
                event->extra[23] = FloatBits(dofC0[3]);
                event->extra[24] = static_cast<uint32_t>(bindDepthHr);
                event->extra[25] = static_cast<uint32_t>(setC0Hr);
                event->extra[26] = static_cast<uint32_t>(bindDofHr);
                event->extra[27] = s_postProcessRoute;
            }
        }

        SafeReleaseCom(texture0);
        SafeReleaseCom(originalTexture1);
        SafeReleaseCom(boundTexture1);
        SafeReleaseCom(afterDrawTexture1);
        SafeReleaseCom(restoredTexture1);
        SafeReleaseCom(beforePS);
        SafeReleaseCom(boundPS);
        SafeReleaseCom(afterDrawPS);
        SafeReleaseCom(restoredPS);

        return drewDof;
    }

    void QueueMode5FinalShaderCapture(uint32_t shaderObject)
    {
        if (s_v10Mode != 5 || shaderObject < 0x10000u)
            return;
        if (InterlockedCompareExchange(&s_mode5FinalShaderCaptureState, 1, 0) != 0)
            return;

        HRESULT queryHr = E_FAIL;
        HRESULT fetchHr = E_PENDING;
        UINT byteCount = 0u;
        uint32_t versionToken = 0u;
        uint32_t lastToken = 0u;
        uint32_t hash = 0u;
        uint32_t chunkCount = 0u;

        void* shader = reinterpret_cast<void*>(static_cast<uintptr_t>(shaderObject));
        if (IsReadableMemory(shader, sizeof(void*)))
        {
            void** vtable = *reinterpret_cast<void***>(shader);
            if (vtable && IsReadableMemory(vtable,
                    (kD3DPixelShaderGetFunctionVtableIndex + 1u) * sizeof(void*)) &&
                IsExecutableMemory(vtable[kD3DPixelShaderGetFunctionVtableIndex]))
            {
                const D3DPixelShaderGetFunction_t getFunction =
                    reinterpret_cast<D3DPixelShaderGetFunction_t>(
                        vtable[kD3DPixelShaderGetFunctionVtableIndex]);
                queryHr = getFunction(shader, nullptr, &byteCount);
                if (SUCCEEDED(queryHr) && byteCount >= sizeof(uint32_t) &&
                    byteCount <= kMode5FinalShaderCaptureMaxBytes)
                {
                    UINT fetched = byteCount;
                    memset(s_mode5FinalShaderBytecode, 0, sizeof(s_mode5FinalShaderBytecode));
                    fetchHr = getFunction(shader, s_mode5FinalShaderBytecode, &fetched);
                    if (SUCCEEDED(fetchHr) && fetched >= sizeof(uint32_t) &&
                        fetched <= kMode5FinalShaderCaptureMaxBytes)
                    {
                        byteCount = fetched;
                        hash = Fnv1a32(s_mode5FinalShaderBytecode, byteCount);
                        memcpy(&versionToken, s_mode5FinalShaderBytecode, sizeof(versionToken));
                        const uint32_t alignedDwords = (byteCount + 3u) / 4u;
                        if (alignedDwords != 0u)
                        {
                            memcpy(&lastToken,
                                s_mode5FinalShaderBytecode + ((alignedDwords - 1u) * 4u),
                                (byteCount - ((alignedDwords - 1u) * 4u) >= 4u) ? 4u :
                                (byteCount - ((alignedDwords - 1u) * 4u)));
                        }
                        chunkCount = (alignedDwords + 27u) / 28u;
                        s_mode5FinalShaderBytecodeSize = byteCount;
                        s_mode5FinalShaderBytecodeHash = hash;

                        for (uint32_t chunk = 0; chunk < chunkCount; ++chunk)
                        {
                            Event* event = ReserveEvent(EventType::NativeFinalShaderBytecodeChunk);
                            if (!event)
                                break;
                            const uint32_t tokenOffset = chunk * 28u;
                            const uint32_t remaining = alignedDwords - tokenOffset;
                            const uint32_t tokenCount = remaining > 28u ? 28u : remaining;
                            event->a = chunk;
                            event->b = chunkCount;
                            event->c = tokenOffset;
                            event->d = tokenCount;
                            for (uint32_t n = 0; n < tokenCount; ++n)
                            {
                                uint32_t token = 0u;
                                const uint32_t byteOffset = (tokenOffset + n) * 4u;
                                const uint32_t bytesLeft = byteCount > byteOffset ? byteCount - byteOffset : 0u;
                                const uint32_t copyBytes = bytesLeft >= 4u ? 4u : bytesLeft;
                                if (copyBytes)
                                    memcpy(&token, s_mode5FinalShaderBytecode + byteOffset, copyBytes);
                                event->extra[n] = token;
                            }
                        }
                        InterlockedExchange(&s_mode5FinalShaderCaptureState, 2);
                    }
                }
            }
        }

        if (s_mode5FinalShaderCaptureState != 2)
            InterlockedExchange(&s_mode5FinalShaderCaptureState, -1);

        Event* header = ReserveEvent(EventType::NativeFinalShaderBytecodeHeader);
        if (header)
        {
            header->a = s_presentCount;
            header->b = shaderObject;
            header->c = byteCount;
            header->d = hash;
            header->e = static_cast<uint32_t>(queryHr);
            header->f = static_cast<uint32_t>(fetchHr);
            header->g = versionToken;
            header->h = lastToken;
            header->i = chunkCount;
            header->j = static_cast<uint32_t>(s_mode5FinalShaderCaptureState);
        }
    }

    void QueueMode5FinalShaderBindProof(uintptr_t returnAddress)
    {
        const LONG count = InterlockedIncrement(&s_mode5FinalShaderBindCount);
        const uint32_t expectedFinal = ReadU32(kCompositePSFinal);
        const uint32_t activeCached = ReadU32(kActivePSCache);
        if (ShouldLogSparse(count))
        {
            Event* event = ReserveEvent(EventType::NativeFinalShaderBind);
            if (event)
            {
                event->a = s_presentCount;
                event->b = static_cast<uint32_t>(count);
                event->c = static_cast<uint32_t>(s_currentCompositeCall);
                event->d = static_cast<uint32_t>(returnAddress);
                event->e = expectedFinal;
                event->f = activeCached;
                event->g = ReadU32(kRtQuarterA);
                event->h = CurrentSceneRT();
                event->i = (expectedFinal >= 0x10000u && expectedFinal == activeCached) ? 1u : 0u;
                event->j = static_cast<uint32_t>(s_mode5FinalShaderCaptureState);
            }
        }
        QueueMode5FinalShaderCapture(expectedFinal);
    }

    bool ResolveMode5FinalShaderDeviceApis(
        void*& device,
        D3DCreatePixelShader_t& createPixelShader,
        D3DSetPixelShader_t& setPixelShader,
        D3DGetPixelShader_t& getPixelShader)
    {
        device = nullptr;
        createPixelShader = nullptr;
        setPixelShader = nullptr;
        getPixelShader = nullptr;

        const uint32_t deviceValue = ReadU32(kD3DDeviceGlobal);
        if (deviceValue < 0x10000u)
            return false;

        device = reinterpret_cast<void*>(static_cast<uintptr_t>(deviceValue));
        if (!IsReadableMemory(device, sizeof(void*)))
            return false;
        void** vtable = *reinterpret_cast<void***>(device);
        if (!vtable || !IsReadableMemory(vtable, (kD3DGetPixelShaderVtableIndex + 1u) * sizeof(void*)))
            return false;

        if (!IsExecutableMemory(vtable[kD3DCreatePixelShaderVtableIndex]) ||
            !IsExecutableMemory(vtable[kD3DSetPixelShaderVtableIndex]) ||
            !IsExecutableMemory(vtable[kD3DGetPixelShaderVtableIndex]))
            return false;

        createPixelShader = reinterpret_cast<D3DCreatePixelShader_t>(vtable[kD3DCreatePixelShaderVtableIndex]);
        setPixelShader = reinterpret_cast<D3DSetPixelShader_t>(vtable[kD3DSetPixelShaderVtableIndex]);
        getPixelShader = reinterpret_cast<D3DGetPixelShader_t>(vtable[kD3DGetPixelShaderVtableIndex]);
        return true;
    }

    bool EnsureMode5FinalCloneShader(HRESULT& createHr, HRESULT& verifyQueryHr, HRESULT& verifyFetchHr)
    {
        createHr = E_PENDING;
        verifyQueryHr = E_PENDING;
        verifyFetchHr = E_PENDING;

        void* device = nullptr;
        D3DCreatePixelShader_t createPixelShader = nullptr;
        D3DSetPixelShader_t setPixelShader = nullptr;
        D3DGetPixelShader_t getPixelShader = nullptr;
        if (!ResolveMode5FinalShaderDeviceApis(device, createPixelShader, setPixelShader, getPixelShader))
            return false;

        const uint32_t deviceValue = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(device));
        if (s_mode5FinalCloneShader && s_mode5FinalCloneDevice != deviceValue)
        {
            SafeReleaseCom(s_mode5FinalCloneShader);
            s_mode5FinalCloneDevice = 0u;
            s_mode5FinalCloneBytecodeHash = 0u;
            InterlockedExchange(&s_mode5FinalCloneState, 0);
        }

        if (s_mode5FinalCloneState == 2 && s_mode5FinalCloneShader)
        {
            createHr = S_OK;
            verifyQueryHr = S_OK;
            verifyFetchHr = S_OK;
            return true;
        }
        if (s_mode5FinalCloneState == -1)
            return false;
        if (InterlockedCompareExchange(&s_mode5FinalCloneState, 1, 0) != 0)
            return s_mode5FinalCloneState == 2 && s_mode5FinalCloneShader;

        const uint32_t expectedFinal = ReadU32(kCompositePSFinal);
        QueueMode5FinalShaderCapture(expectedFinal);
        if (s_mode5FinalShaderCaptureState != 2 ||
            s_mode5FinalShaderBytecodeSize < sizeof(uint32_t) ||
            s_mode5FinalShaderBytecodeSize > kMode5FinalShaderCaptureMaxBytes)
        {
            InterlockedExchange(&s_mode5FinalCloneState, -1);
            return false;
        }

        // V10.5.55 PostProcessFix: fail closed unless the captured shader is the exact
        // proven retail BE64 bytecode. .54 showed that adding _sat directly to
        // TEXLD is rejected by D3D9 (D3DERR_INVALIDCALL), so build a legal
        // two-instruction tail while preserving every declaration/input/sampler:
        //     texld   r0,  v0, s0
        //     mov_sat oC0, r0
        if (s_mode5FinalShaderBytecodeSize != kMode5FinalRetailBytecodeBytes ||
            s_mode5FinalShaderBytecodeHash != kMode5FinalRetailBytecodeHash ||
            (kMode5FinalClampEndDwordIndex + 1u) * sizeof(uint32_t) != s_mode5FinalShaderBytecodeSize ||
            kMode5FinalClampPatchedBytes > kMode5FinalShaderCaptureMaxBytes)
        {
            InterlockedExchange(&s_mode5FinalClampPatchState, -1);
            InterlockedExchange(&s_mode5FinalCloneState, -1);
            return false;
        }

        const uint32_t* sourceDwords =
            reinterpret_cast<const uint32_t*>(s_mode5FinalShaderBytecode);
        const uint32_t sourceWord = sourceDwords[kMode5FinalClampDwordIndex];
        s_mode5FinalClampSourceWord = sourceWord;
        if (sourceDwords[kMode5FinalClampTexldOpcodeIndex] != kMode5FinalClampOriginalTexldOpcode ||
            sourceWord != kMode5FinalClampOriginalDword ||
            sourceDwords[kMode5FinalClampEndDwordIndex] != kMode5FinalClampOriginalEnd)
        {
            InterlockedExchange(&s_mode5FinalClampPatchState, -1);
            InterlockedExchange(&s_mode5FinalCloneState, -1);
            return false;
        }

        memset(s_mode5FinalPatchedBytecode, 0, sizeof(s_mode5FinalPatchedBytecode));
        memcpy(s_mode5FinalPatchedBytecode,
            s_mode5FinalShaderBytecode,
            kMode5FinalRetailBytecodeBytes);

        uint32_t* patchedDwords =
            reinterpret_cast<uint32_t*>(s_mode5FinalPatchedBytecode);
        patchedDwords[kMode5FinalClampDwordIndex] = kMode5FinalClampPatchedDword;
        patchedDwords[kMode5FinalClampEndDwordIndex + 0u] = kMode5FinalClampMovOpcode;
        patchedDwords[kMode5FinalClampEndDwordIndex + 1u] = kMode5FinalClampMovDest;
        patchedDwords[kMode5FinalClampEndDwordIndex + 2u] = kMode5FinalClampMovSource;
        patchedDwords[kMode5FinalClampEndDwordIndex + 3u] = kMode5FinalClampOriginalEnd;

        const uint32_t patchedHash =
            Fnv1a32(s_mode5FinalPatchedBytecode, kMode5FinalClampPatchedBytes);
        s_mode5FinalClampPatchedWord = patchedDwords[kMode5FinalClampDwordIndex];
        s_mode5FinalClampPatchedHash = patchedHash;
        s_mode5FinalClampPatchedBytes = kMode5FinalClampPatchedBytes;

        if (patchedHash != kMode5FinalClampExpectedHash)
        {
            InterlockedExchange(&s_mode5FinalClampPatchState, -1);
            InterlockedExchange(&s_mode5FinalCloneState, -1);
            return false;
        }
        InterlockedExchange(&s_mode5FinalClampPatchState, 1);

        void* clone = nullptr;
        createHr = createPixelShader(
            device,
            reinterpret_cast<const DWORD*>(s_mode5FinalPatchedBytecode),
            &clone);
        if (FAILED(createHr) || !clone)
        {
            InterlockedExchange(&s_mode5FinalCloneState, -1);
            return false;
        }

        uint32_t cloneHash = 0u;
        UINT cloneBytes = 0u;
        void** cloneVtable = *reinterpret_cast<void***>(clone);
        if (cloneVtable && IsReadableMemory(cloneVtable,
                (kD3DPixelShaderGetFunctionVtableIndex + 1u) * sizeof(void*)) &&
            IsExecutableMemory(cloneVtable[kD3DPixelShaderGetFunctionVtableIndex]))
        {
            const D3DPixelShaderGetFunction_t getFunction =
                reinterpret_cast<D3DPixelShaderGetFunction_t>(
                    cloneVtable[kD3DPixelShaderGetFunctionVtableIndex]);
            verifyQueryHr = getFunction(clone, nullptr, &cloneBytes);
            if (SUCCEEDED(verifyQueryHr) && cloneBytes == kMode5FinalClampPatchedBytes &&
                cloneBytes <= kMode5FinalShaderCaptureMaxBytes)
            {
                UINT fetched = cloneBytes;
                memset(s_mode5FinalCloneVerifyBytecode, 0, sizeof(s_mode5FinalCloneVerifyBytecode));
                verifyFetchHr = getFunction(clone, s_mode5FinalCloneVerifyBytecode, &fetched);
                if (SUCCEEDED(verifyFetchHr) && fetched == kMode5FinalClampPatchedBytes)
                    cloneHash = Fnv1a32(s_mode5FinalCloneVerifyBytecode, fetched);
            }
        }

        if (cloneHash != s_mode5FinalClampPatchedHash ||
            cloneHash != kMode5FinalClampExpectedHash ||
            cloneHash == 0u)
        {
            SafeReleaseCom(clone);
            InterlockedExchange(&s_mode5FinalCloneState, -1);
            return false;
        }

        s_mode5FinalCloneShader = clone;
        s_mode5FinalCloneDevice = deviceValue;
        s_mode5FinalCloneBytecodeHash = cloneHash;
        InterlockedExchange(&s_mode5FinalCloneState, 2);
        return true;
    }

    int RunMode5FinalCloneOverride(void* a1, void* a2, unsigned int a3, int a4, uintptr_t returnAddress)
    {
        HRESULT createHr = E_PENDING;
        HRESULT verifyQueryHr = E_PENDING;
        HRESULT verifyFetchHr = E_PENDING;
        HRESULT beforeGetHr = E_PENDING;
        HRESULT bindHr = E_PENDING;
        HRESULT boundGetHr = E_PENDING;
        HRESULT afterDrawGetHr = E_PENDING;
        HRESULT restoreHr = E_PENDING;
        HRESULT restoredGetHr = E_PENDING;

        void* device = nullptr;
        D3DCreatePixelShader_t createPixelShader = nullptr;
        D3DSetPixelShader_t setPixelShader = nullptr;
        D3DGetPixelShader_t getPixelShader = nullptr;
        void* beforePS = nullptr;
        void* boundPS = nullptr;
        void* afterDrawPS = nullptr;
        void* restoredPS = nullptr;
        const uint32_t expectedFinal = ReadU32(kCompositePSFinal);

        const LONG drawCount = InterlockedIncrement(&s_mode5FinalCloneDrawCount);
        bool cloneReady = EnsureMode5FinalCloneShader(createHr, verifyQueryHr, verifyFetchHr);
        bool beforeVerified = false;
        bool bindVerified = false;
        bool drawHeldClone = false;
        bool restoreVerified = false;
        int result = 0;

        if (cloneReady && ResolveMode5FinalShaderDeviceApis(
                device, createPixelShader, setPixelShader, getPixelShader))
        {
            beforeGetHr = getPixelShader(device, &beforePS);
            beforeVerified = SUCCEEDED(beforeGetHr) && beforePS &&
                reinterpret_cast<uintptr_t>(beforePS) == expectedFinal;

            if (beforeVerified)
            {
                bindHr = setPixelShader(device, s_mode5FinalCloneShader);
                if (SUCCEEDED(bindHr))
                {
                    boundGetHr = getPixelShader(device, &boundPS);
                    bindVerified = SUCCEEDED(boundGetHr) && boundPS == s_mode5FinalCloneShader;
                }
            }

            if (bindVerified)
            {
                result = s_originalFullscreenQuad(a1, a2, a3, a4);
                afterDrawGetHr = getPixelShader(device, &afterDrawPS);
                drawHeldClone = SUCCEEDED(afterDrawGetHr) && afterDrawPS == s_mode5FinalCloneShader;

                restoreHr = setPixelShader(device, beforePS);
                if (SUCCEEDED(restoreHr))
                {
                    restoredGetHr = getPixelShader(device, &restoredPS);
                    restoreVerified = SUCCEEDED(restoredGetHr) && restoredPS == beforePS;
                }
            }
            else
            {
                if (beforePS)
                    setPixelShader(device, beforePS);
                result = s_originalFullscreenQuad(a1, a2, a3, a4);
            }
        }
        else
        {
            result = s_originalFullscreenQuad(a1, a2, a3, a4);
        }

        const bool pass = cloneReady && beforeVerified && bindVerified && drawHeldClone && restoreVerified;
        if (pass)
            InterlockedIncrement(&s_mode5FinalClonePassCount);
        else
            InterlockedIncrement(&s_mode5FinalCloneFailCount);

        if (ShouldLogSparse(drawCount) || !pass)
        {
            Event* event = ReserveEvent(EventType::NativeFinalShaderCloneOverride);
            if (event)
            {
                event->a = s_presentCount;
                event->b = static_cast<uint32_t>(drawCount);
                event->c = static_cast<uint32_t>(returnAddress);
                event->d = expectedFinal;
                event->e = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(s_mode5FinalCloneShader));
                event->f = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(beforePS));
                event->g = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(boundPS));
                event->h = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(afterDrawPS));
                event->i = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(restoredPS));
                event->j = pass ? 1u : 0u;
                event->extra[0] = static_cast<uint32_t>(s_mode5FinalCloneState);
                event->extra[1] = s_mode5FinalShaderBytecodeHash;
                event->extra[2] = s_mode5FinalCloneBytecodeHash;
                event->extra[3] = static_cast<uint32_t>(createHr);
                event->extra[4] = static_cast<uint32_t>(verifyQueryHr);
                event->extra[5] = static_cast<uint32_t>(verifyFetchHr);
                event->extra[6] = static_cast<uint32_t>(beforeGetHr);
                event->extra[7] = static_cast<uint32_t>(bindHr);
                event->extra[8] = static_cast<uint32_t>(boundGetHr);
                event->extra[9] = static_cast<uint32_t>(afterDrawGetHr);
                event->extra[10] = static_cast<uint32_t>(restoreHr);
                event->extra[11] = static_cast<uint32_t>(restoredGetHr);
                event->extra[12] = beforeVerified ? 1u : 0u;
                event->extra[13] = bindVerified ? 1u : 0u;
                event->extra[14] = drawHeldClone ? 1u : 0u;
                event->extra[15] = restoreVerified ? 1u : 0u;
                event->extra[16] = static_cast<uint32_t>(s_mode5FinalClonePassCount);
                event->extra[17] = static_cast<uint32_t>(s_mode5FinalCloneFailCount);
                event->extra[18] = ReadU32(kActivePSCache);
                event->extra[19] = static_cast<uint32_t>(s_mode5FinalClampPatchState);
                event->extra[20] = s_mode5FinalClampSourceWord;
                event->extra[21] = s_mode5FinalClampPatchedWord;
                event->extra[22] = s_mode5FinalClampPatchedHash;
                event->extra[23] = s_mode5FinalClampPatchedBytes;
                event->extra[24] = kMode5FinalClampMovOpcode;
                event->extra[25] = kMode5FinalClampMovDest;
                event->extra[26] = kMode5FinalClampMovSource;
            }
        }

        SafeReleaseCom(beforePS);
        SafeReleaseCom(boundPS);
        SafeReleaseCom(afterDrawPS);
        SafeReleaseCom(restoredPS);
        return result;
    }

    int __cdecl FullscreenQuad_Hook(void* a1, void* a2, unsigned int a3, int a4)
    {
        const uintptr_t returnAddress = reinterpret_cast<uintptr_t>(_ReturnAddress());

        // V10.5.61 correction: 0x00470900 is the fullscreen quad/stream SETUP
        // helper. The actual GPU draw follows at retail PC 0x00797D25. Keep this
        // helper completely native and move every visible shader override to the
        // exact DrawPrimitive detour (return 0x00797D2B).
        if (s_v10Mode == 5 && s_insideComposite && returnAddress == kCompositeFinalQuadReturn)
        {
            QueueMode5FinalShaderBindProof(returnAddress);
            return s_originalFullscreenQuad(a1, a2, a3, a4);
        }

        const int result = s_originalFullscreenQuad(a1, a2, a3, a4);

        if (s_insideComposite && returnAddress == kCompositeBlur2QuadReturn)
        {
            const uint32_t quarterA = ReadU32(kRtQuarterA);
            const uint32_t quarterB = ReadU32(kRtQuarterB);
            if (s_thisCompositeBlur1Routed && quarterA && quarterB)
            {
                // Stock has just uploaded Gaussian #2's c0 and built its quad.
                // Keep the proven B->A routing, but in Mode 4 replace this draw
                // with PC's dormant blurlevel1offset shader. V10.5.49.10.1 uses
                // the live retail-PC c0 proved in x32: {-6/W,-6/H,+6/W,-6/H}.
                const int rtResult = CallOriginalSetRTTracked(static_cast<int>(quarterA));
                s_originalBindTexture(0, static_cast<int>(quarterB + 0x14u));
                if (s_v10Mode == 4 && s_mode4MidProbeOnly)
                    s_thisCompositeNativeTailLevel1 = ApplyMode4NativeBlurLevel1State();
                QueueRouteEvent(EventType::RouteBlur2, returnAddress, quarterB, quarterA, rtResult);
            }
            else
            {
                QueueMismatch(2, returnAddress, quarterB, quarterA);
            }
        }
        else if (s_insideComposite && returnAddress == kCompositeBlur1QuadReturn)
        {
            // Observation anchor only: the first blur draw immediately follows.
            // No mutation is required here because blur1 RT routing is moved in
            // BindTexture_Hook before quarterA is sampled.
        }

        return result;
    }

    bool EnsureDirectory(const char* path)
    {
        const DWORD attrs = GetFileAttributesA(path);
        if (attrs != INVALID_FILE_ATTRIBUTES)
            return (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (CreateDirectoryA(path, nullptr))
            return true;
        return GetLastError() == ERROR_ALREADY_EXISTS;
    }

    void EnsureLogOpen()
    {
#if defined(XESM3_PUBLIC_BUILD)
        // Public xeSM3 keeps the proven PostFX validation and fail-closed
        // behavior but intentionally creates no persistent research log.
        return;
#else
        if (s_logFile != INVALID_HANDLE_VALUE)
            return;

        char localAppData[MAX_PATH] = {};
        if (!GetEnvironmentVariableA("LOCALAPPDATA", localAppData, MAX_PATH))
            return;

        char root[MAX_PATH] = {};
        char gameDir[MAX_PATH] = {};
        char logPath[MAX_PATH] = {};
        sprintf_s(root, "%s\\RaimiHook", localAppData);
        sprintf_s(gameDir, "%s\\Spider-Man 3", root);
        sprintf_s(logPath, "%s\\RaimiHook_PostFXResearchLog.txt", gameDir);
        EnsureDirectory(root);
        EnsureDirectory(gameDir);

        s_logFile = CreateFileA(logPath, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (s_logFile == INVALID_HANDLE_VALUE)
            return;

        const char* header =
            "xeSM3 v0.1.0 / PostFX V10.5.72 ALT_TAB_D3D9_DEVICE_RESET_FIX\r\n"
            "GOAL: production/release cleanup of the qualified Debug Xbox PostFX route: adaptive weighted bloom + F18 + ImageZoom/ZBlur + xeSM3-owned GodRay; pixels frozen from V10.5.71; V10.5.72 changes device lifecycle only.\r\n"
            "POSTPROCESSFIX: eight native FP16 PostFX targets + corrected A->B->A routing + valid BE64 clamp; final overrides remain at exact DrawPrimitive return 00797D2B.\r\n"
            "SHARED XBOX BASE: same-present packed depth -> F18 s1, live bloomDepthControl -> PS c0, actual F18 draw, then guarded ImageZoom VS B856422D / PS 3FA172A3 with controller 0.5/1.25/10.5.\r\n"
            "DEBUG D2 BLOOM APPLICATION: source-proven defaults darkBrightness=28 lightBrightness=96 darkBloom=0 lightBloom=1.5; sparse 16x12 readback drives the native weighted Scene->QuarterA pass only on same-present world frames.\r\n"
            "GODRAY LOCKED: selector dword_EE5E90 remains read-only; G1 validates stock 0079849C -> 007984A2, G2 verifies exact binds/state, G3 issues the proven xeSM3 draw, and G4 suppresses the duplicate stock draw only after G3 pass + exact restoration; otherwise stock draw falls through.\r\n"
            "PRODUCTION LOGGING: successful research-detail events are OFF by default; selector transitions and failures remain visible; the 300-present XBOX_SHARED_HEARTBEAT is the primary health record.\r\n"
            "SAFETY: G3 has no RT/DS/viewport setter and no selector write or 00797D50 call; draw is permitted only after full G2 verification, post-draw state is read back, exact pre-probe state is restored, and any G3 failure disables subsequent xeSM3 GodRay draws fail-closed.\r\n"
            "EXPECTED CREATE_RT: the same eight 15->71 overrides from V10.5.50.\r\n"
            "EXPECTED ROUTE events during gameplay: ROUTE_BLUR1 source=QuarterA target=QuarterB; ROUTE_BLUR2 source=QuarterB target=QuarterA; ROUTE_FINAL source=QuarterA target=Scene.\r\n"
            "FAIL CLOSED: if the expected Quarter A/B identities do not match, the hook logs a mismatch and leaves the stock bind path in control.\r\n"
            "============================================================\r\n";
        DWORD written = 0;
        WriteFile(s_logFile, header, static_cast<DWORD>(strlen(header)), &written, nullptr);

        char configLine[768] = {};
        sprintf_s(configLine,
            "[POSTFX:V10572_CONFIG] ini=\"%s\" PostProcessFix=%u PostProcessRetailXbox=%u PostProcessDebugXbox=%u active=%u route=%s xboxSharedActualDrawActive=%u xboxSharedImageZoomActive=%u debugD2AdaptiveBloom=%u quietIntegration=%u productionSuccessDetail=%u shaderChunkDump=%u\r\n",
            s_postProcessIniPath,
            s_postProcessFixEnabled ? 1u : 0u,
            s_postProcessRetailXboxEnabled ? 1u : 0u,
            s_postProcessDebugXboxEnabled ? 1u : 0u,
            s_postProcessNativeRepairActive ? 1u : 0u,
            PostProcessRouteName(),
            IsXboxPostProcessRoute() ? 1u : 0u,
            IsXboxPostProcessRoute() ? 1u : 0u,
            s_postProcessRoute == 3u ? 1u : 0u,
            (s_postProcessRoute == 3u ? kDebugXboxIntegrationLockdownQuietLogging : kRetailXboxIntegrationQuietLogging) ? 1u : 0u,
            kDebugXboxProductionSuccessDetailLogging ? 1u : 0u,
            kRetailXboxLogShaderBytecodeChunks ? 1u : 0u);
        written = 0;
        WriteFile(s_logFile, configLine, static_cast<DWORD>(strlen(configLine)), &written, nullptr);
#endif
    }

    void AppendFormatted(char* output, size_t capacity, size_t& offset, const char* fmt, ...)
    {
        if (offset >= capacity)
            return;
        va_list args;
        va_start(args, fmt);
        const int wrote = _vsnprintf_s(output + offset, capacity - offset, _TRUNCATE, fmt, args);
        va_end(args);
        if (wrote > 0)
            offset += static_cast<size_t>(wrote);
        else
            offset = capacity;
    }

    void FlushEvents()
    {
#if defined(XESM3_PUBLIC_BUILD)
        // Release keeps counters/validation alive while discarding file-only
        // telemetry. Drain the ring to prevent long-session event saturation.
        const LONG write = InterlockedCompareExchange(&s_eventWrite, 0, 0);
        InterlockedExchange(&s_eventRead, write);
        return;
#else
        EnsureLogOpen();
        if (s_logFile == INVALID_HANDLE_VALUE)
            return;

        char output[32768] = {};
        size_t offset = 0;

        while (s_eventRead < s_eventWrite && offset < sizeof(output) - 1024)
        {
            const LONG read = s_eventRead;
            const Event event = s_events[static_cast<uint32_t>(read) % kEventCapacity];
            InterlockedIncrement(&s_eventRead);

            switch (event.type)
            {
            case EventType::CreateRT:
                AppendFormatted(output, sizeof(output), offset,
                    "[POSTFX:CREATE_RT] return=%08X flags=%08X requestedFormat=%08X effectiveFormat=%08X override=%u width=%u height=%u result=%08X\r\n",
                    event.a, event.b, event.c, event.d, event.e, event.f, event.g, event.h);
                break;
            case EventType::BloomCallback:
                AppendFormatted(output, sizeof(output), offset,
                    "[POSTFX:BLOOM_CB] call=%u autoExp=%u quarterA=%08X quarterB=%08X rt160=%08X E8FB04=%08X E8FBE8=%08X sceneRT=%08X\r\n",
                    event.a, event.b, event.c, event.d, event.e, event.f, event.g, event.h);
                break;
            case EventType::CompositeEnter:
                AppendFormatted(output, sizeof(output), offset,
                    "[POSTFX:V7_COMPOSITE] call=%u quarterA=%08X quarterB=%08X sceneRT=%08X psInitial=%08X psBlur=%08X psFinal=%08X vs=%08X activePS=%08X activeVS=%08X\r\n",
                    event.a, event.b, event.c, event.d, event.e, event.f, event.g, event.h, event.i, event.j);
                break;
            case EventType::NativeSetRT:
                AppendFormatted(output, sizeof(output), offset,
                    "[POSTFX:V7_NATIVE_SET_RT] call=%u return=%08X target=%08X quarterA=%08X quarterB=%08X sceneRT=%08X\r\n",
                    event.a, event.b, event.c, event.d, event.e, event.f);
                break;
            case EventType::RouteBlur1:
                AppendFormatted(output, sizeof(output), offset,
                    "[POSTFX:V7_ROUTE] phase=blur1 count=%u call=%u return=%08X sourceRT=%08X targetRT=%08X setRTResult=%08X scene=%08X sceneRT=%08X activePS=%08X activeVS=%08X\r\n",
                    event.a, event.b, event.c, event.d, event.e, event.f, event.g, event.h, event.i, event.j);
                break;
            case EventType::RouteBlur2:
                AppendFormatted(output, sizeof(output), offset,
                    "[POSTFX:V7_ROUTE] phase=blur2 count=%u call=%u return=%08X sourceRT=%08X targetRT=%08X setRTResult=%08X scene=%08X sceneRT=%08X activePS=%08X activeVS=%08X\r\n",
                    event.a, event.b, event.c, event.d, event.e, event.f, event.g, event.h, event.i, event.j);
                break;
            case EventType::RouteFinal:
                AppendFormatted(output, sizeof(output), offset,
                    "[POSTFX:V7_ROUTE] phase=final count=%u call=%u return=%08X sourceRT=%08X targetRT=%08X setRTResult=%08X scene=%08X sceneRT=%08X activePS=%08X activeVS=%08X\r\n",
                    event.a, event.b, event.c, event.d, event.e, event.f, event.g, event.h, event.i, event.j);
                break;
            case EventType::RouteMismatch:
                AppendFormatted(output, sizeof(output), offset,
                    "[POSTFX:V7_MISMATCH] count=%u call=%u phase=%u return=%08X observed=%08X expected=%08X quarterA=%08X quarterB=%08X sceneRT=%08X\r\n",
                    event.a, event.b, event.c, event.d, event.e, event.f, event.g, event.h, event.i);
                break;
            case EventType::CompositeExit:
                AppendFormatted(output, sizeof(output), offset,
                    "[POSTFX:V7_EXIT] call=%u activePS=%08X activeVS=%08X sceneRT=%08X result=%08X\r\n",
                    event.a, event.b, event.c, event.d, event.e);
                break;
            case EventType::Summary:
                AppendFormatted(output, sizeof(output), offset,
                    "[POSTFX:V8_V7_SUMMARY] present=%u composite=%u blur1=%u blur2=%u final=%u mismatch=%u autoExp=%u bindE8FB04=%u bindE8FBE8=%u setRTE8FB04=%u setRTE8FBE8=%u dropped=%u\r\n",
                    event.a, event.b, event.c, event.d, event.e, event.f, event.g, event.h, event.i, event.j,
                    static_cast<uint32_t>(s_setRTSecond16Count), static_cast<uint32_t>(s_droppedEvents));
                break;
            case EventType::RawHookStatus:
                AppendFormatted(output, sizeof(output), offset,
                    "[POSTFX:V8_D3D_HOOK] status=%08X device=%08X SetRT=%08X SetTexture=%08X SetPS=%08X SetPSConstF=%08X\r\n",
                    event.a, event.b, event.c, event.d, event.e, event.f);
                break;
            case EventType::DeviceReset:
                AppendFormatted(output, sizeof(output), offset,
                    "[POSTFX:DEVICE_RESET] count=%u device=%08X requested=%ux%u format=%08X hr=%08X releasedZBlur=%u releasedINTZ=%u releasedPackedDepth=%u mode=%u deviceReady=%u\r\n",
                    event.a, event.b, event.c, event.d, event.e, event.f, event.g, event.h, event.i, event.j, event.extra[0]);
                break;
            case EventType::RawTextureExact:
                AppendFormatted(output, sizeof(output), offset,
                    "[POSTFX:V8_RAW_16X12_TEXTURE] count=%u which=%s return=%08X stage=%u texture=%08X hr=%08X activePS=%08X rt0=%08X scene=%08X name=\"%s\"\r\n",
                    event.a, event.b == 1 ? "E8FB04" : "E8FBE8", event.c, event.d, event.e, event.f, event.g, event.h, event.i, event.text);
                break;
            case EventType::RawRenderTargetExact:
                AppendFormatted(output, sizeof(output), offset,
                    "[POSTFX:V8_RAW_16X12_RT] count=%u which=%s return=%08X index=%u surface=%08X nglWrapper=%08X hr=%08X activePS=%08X scene=%08X name=\"%s\"\r\n",
                    event.a, event.b == 1 ? "E8FB04" : "E8FBE8", event.c, event.d, event.e, event.f, event.g, event.h, event.i, event.text);
                break;
            case EventType::RawSetTexture:
                AppendFormatted(output, sizeof(output), offset,
                    "[POSTFX:V8_D3D_TEXTURE] present=%u return=%08X stage=%u texture=%08X source=%s hr=%08X activePS=%08X rt0=%08X scene=%08X name=\"%s\"\r\n",
                    event.a, event.b, event.c, event.d, WrapperKindName(event.e), event.f, event.g, event.h, event.i, event.text);
                break;
            case EventType::RawSetRenderTarget:
                AppendFormatted(output, sizeof(output), offset,
                    "[POSTFX:V8_D3D_RT] present=%u return=%08X index=%u surface=%08X target=%s nglWrapper=%08X hr=%08X activePS=%08X scene=%08X name=\"%s\"\r\n",
                    event.a, event.b, event.c, event.d, WrapperKindName(event.e), event.f, event.g, event.h, event.i, event.text);
                break;
            case EventType::RawDepthStencilCreate:
                AppendFormatted(output, sizeof(output), offset,
                    "[POSTFX:DEPTH_CREATE] present=%u count=%u return=%08X hr=%08X surface=%08X requestedFormat=%08X size=%ux%u msType=%u msQuality=%u discard=%u descHr=%08X containerHr=%08X containerTex=%08X actualFormat=%08X usage=%08X pool=%08X\r\n",
                    event.a, event.b, event.c, event.d, event.e, event.f, event.g, event.h,
                    event.i, event.j, event.extra[0], event.extra[1], event.extra[2],
                    event.extra[3], event.extra[4], event.extra[5], event.extra[6]);
                break;
            case EventType::RawDepthStencilBind:
                AppendFormatted(output, sizeof(output), offset,
                    "[POSTFX:DEPTH_BIND] present=%u count=%u return=%08X surface=%08X hr=%08X descHr=%08X containerHr=%08X containerTex=%08X format=%08X usage=%08X msType=%u msQuality=%u size=%ux%u rt0=%08X activePS=%08X capturedSurface=%08X capturedTex=%08X nglDepthWrapper=%08X wrapperSurfaceOffset=%08X\r\n",
                    event.a, event.b, event.c, event.d, event.e, event.f, event.g, event.h,
                    event.i, event.j, event.extra[0], event.extra[1], event.extra[2],
                    event.extra[3], event.extra[4], event.extra[5], event.extra[6], event.extra[7],
                    event.extra[8], event.extra[9]);
                break;

case EventType::DepthTextureCaps:
    AppendFormatted(output, sizeof(output), offset,
        "[POSTFX:DEPTH_CAPS] present=%u createTextureD24S8=%08X createTextureINTZ=%08X createTextureDF24=%08X createTextureRAWZ=%08X createTextureDF16=%08X nglDepthWrapper=%08X flags=%08X size=%ux%u capturedSurface=%08X wrapperSurfaceOffset=%08X words={%08X,%08X,%08X,%08X,%08X,%08X,%08X,%08X}\r\n",
        event.a, event.b, event.c, event.d, event.e, event.f,
        event.g, event.h, event.i, event.j, event.extra[0], event.extra[1],
        s_mode4NglDepthWrapperWords[0], s_mode4NglDepthWrapperWords[1],
        s_mode4NglDepthWrapperWords[2], s_mode4NglDepthWrapperWords[3],
        s_mode4NglDepthWrapperWords[4], s_mode4NglDepthWrapperWords[5],
        s_mode4NglDepthWrapperWords[6], s_mode4NglDepthWrapperWords[7]);
    break;
            case EventType::DepthBridgeProbe:
                AppendFormatted(output, sizeof(output), offset,
                    "[POSTFX:DEPTH_BRIDGE] present=%u capturedSurface=%08X nglWrapper=%08X backendRoot=%08X directSurfaceOffset=%08X childSlotOffset=%08X child=%08X childSurfaceOffset=%08X "
                    "D24={create=%08X,getSurface=%08X,desc=%08X,container=%08X,match=%u,format=%08X,tex=%08X,surf=%08X} "
                    "INTZ={create=%08X,getSurface=%08X,desc=%08X,container=%08X,match=%u,format=%08X,tex=%08X,surf=%08X} backendWords={%08X,%08X}\r\n",
                    event.a, event.b, event.c, event.d, event.e, event.f, event.g, event.h,
                    event.i, event.j, event.extra[0], event.extra[1], event.extra[2], event.extra[3], event.extra[10], event.extra[11],
                    event.extra[4], event.extra[5], event.extra[6], event.extra[7], event.extra[8], event.extra[9], event.extra[12], event.extra[13],
                    event.extra[14], event.extra[15]);
                break;
            case EventType::DepthReplacementTest:
                AppendFormatted(output, sizeof(output), offset,
                    "[POSTFX:DEPTH_REPLACE] present=%u count=%u return=%08X original=%08X replacementSurface=%08X replacementTex=%08X attempted=%u applied=%u replaceBindHr=%08X fallbackHr=%08X "
                    "createHr=%08X getSurfaceHr=%08X containerHr=%08X containerTex=%08X size=%ux%u sourceFormat=%08X activeDepthHr=%08X activeDepth=%08X finalHr=%08X replacementFormat=%08X\r\n",
                    event.a, event.b, event.c, event.d, event.e, event.f, event.g, event.h, event.i, event.j,
                    event.extra[0], event.extra[1], event.extra[2], event.extra[3], event.extra[4], event.extra[5], event.extra[6],
                    event.extra[7], event.extra[8], event.extra[9], event.extra[10]);
                break;
            case EventType::DepthPackProbe:
                AppendFormatted(output, sizeof(output), offset,
                    "[POSTFX:DEPTH_PACK] present=%u count=%u ok=%u channel=%u createState=%d compileHr=%08X createTexHr=%08X getSurfaceHr=%08X createPSHr=%08X setupHr=%08X drawHr=%08X restoreHr=%08X sourceDepthTex=%08X packedTex=%08X packedSurface=%08X packPS=%08X size=%ux%u apply=%u fail=%u auto=%u block=%u compiler=\"%s\"\r\n",
                    event.a, event.b, event.c, event.d, static_cast<int32_t>(event.e),
                    event.f, event.g, event.h, event.i, event.j, event.extra[0], event.extra[1],
                    event.extra[2], event.extra[3], event.extra[4], event.extra[5], event.extra[6],
                    event.extra[7], event.extra[8], event.extra[9], event.extra[10], event.extra[11], event.text);
                break;
            case EventType::MidSceneCallbackInstall:
                AppendFormatted(output, sizeof(output), offset,
                    "[POSTFX:MID_INSTALL] present=%u requestedType=%u return=%08X scene=%08X existingMid=%08X hook=%08X installed=%u installs=%u callbacks=%u conflicts=%u\r\n",
                    event.a, event.b, event.c, event.d, event.e, event.f, event.g, event.h, event.i, event.j);
                break;
            case EventType::MidSceneDepthProbe:
                AppendFormatted(output, sizeof(output), offset,
                    "[POSTFX:MID_DEPTH] present=%u count=%u scene=%08X device=%08X rt=%08X ds=%08X rtHr=%08X dsHr=%08X nglBackBuffer=%08X nglDepthWrapper=%08X wrapperSurface=%08X wrapperMatch=%u mainDepth=%08X mainMatch=%u rtFmt=%08X rtSize=%ux%u dsFmt=%08X dsSize=%ux%u rawHook=%u midSlot=%08X\r\n",
                    event.a, event.b, event.c, event.d, event.e, event.f, event.g, event.h, event.i, event.j,
                    event.extra[0], event.extra[1], event.extra[10], event.extra[11], event.extra[2], event.extra[3], event.extra[4],
                    event.extra[5], event.extra[6], event.extra[7], event.extra[8], event.extra[9]);
                break;
            case EventType::MidSceneResolveProbe:
                AppendFormatted(output, sizeof(output), offset,
                    "[POSTFX:MID_RESOLVE] present=%u count=%u sourceDS=%08X destSurface=%08X destTexture=%08X createHr=%08X getSurfaceHr=%08X copyHr=%08X sourceFmt=%08X destFmt=%08X sourceSize=%ux%u destSize=%ux%u mainMatch=%u ready=%u attempts=%u success=%u fail=%u containerHr=%08X containerTex=%08X destUsage=%08X\r\n",
                    event.a, event.b, event.c, event.d, event.e, event.f, event.g, event.h, event.i, event.j,
                    event.extra[0], event.extra[1], event.extra[2], event.extra[3], event.extra[4], event.extra[5],
                    event.extra[6], event.extra[7], event.extra[8], event.extra[9], event.extra[10], event.extra[11]);
                break;
            case EventType::ShadowDepthTargetProbe:
                AppendFormatted(output, sizeof(output), offset,
                    "[POSTFX:SHADOW_DEPTH_TARGET] present=%u count=%u ps=%08X rt=%08X rtFmt=%08X rtSize=%ux%u containerHr=%08X rtTex=%08X ds=%08X dsHr=%08X dsFmt=%08X dsSize=%ux%u scene=%08X vs=%08X rtUsage=%08X dsUsage=%08X rtGetHr=%08X name=\"%s\"\r\n",
                    event.a, event.b, event.c, event.d, event.e, event.f, event.g, event.h, event.i, event.j,
                    event.extra[0], event.extra[1], event.extra[2], event.extra[3], event.extra[4], event.extra[5],
                    event.extra[6], event.extra[7], event.extra[8], event.text);
                break;
            case EventType::FullResDepthTargetProbe:
                AppendFormatted(output, sizeof(output), offset,
                    "[POSTFX:FULLRES_DEPTH_TARGET] present=%u count=%u ready=%u tex=%08X rt=%08X ds=%08X createTexHr=%08X getSurfaceHr=%08X createDepthHr=%08X containerHr=%08X containerTex=%08X rtFmt=%08X rtSize=%ux%u dsFmt=%08X dsSize=%ux%u bindRTHr=%08X bindDSHr=%08X restoreDSHr=%08X restoreRTHr=%08X mainMatch=%u\r\n",
                    event.a, event.b, event.c, event.d, event.e, event.f, event.g, event.h, event.i, event.j,
                    event.extra[0], event.extra[1], event.extra[2], event.extra[3], event.extra[4],
                    event.extra[5], event.extra[6], event.extra[7], event.extra[8], event.extra[9],
                    event.extra[10], event.extra[11]);
                break;
            case EventType::MainCameraDepthPrepass:
                if (event.f == 24u || event.f == 25u || event.f == 27u)
                {
                    AppendFormatted(output, sizeof(output), offset,
                        "[POSTFX:PHAT_CALLER_SUMMARY] present=%u calls=%u logged=%u stackFailures=%u scene=%08X stage=%u armPresent=%u endPresent=%u name=\"%s\"\r\n",
                        event.a, event.b, event.c, event.d, event.e, event.f, event.extra[0], event.extra[1], event.text);
                    break;
                }
                AppendFormatted(output, sizeof(output), offset,
                    "[POSTFX:MAIN_DEPTH_PREPASS] present=%u scanned=%u directPhat=%u eligibleShadowWrappers=%u firstCommand=%08X shadowFlagBefore=%u stateBlockHr=%08X captureHr=%08X bindRTHr=%08X bindDSHr=%08X clearHr=%08X replayed=%u restoreDSHr=%08X restoreRTHr=%08X applyHr=%08X cachePSBefore=%08X cacheVSBefore=%08X cachePSAfter=%08X cacheVSAfter=%08X ok=%u firstNode=%08X firstVtable=%08X firstExecute=%08X failStage=%u\r\n",
                    event.a, event.b, event.c, event.d, event.e, event.f, event.g, event.h, event.i, event.j,
                    event.extra[0], event.extra[1], event.extra[2], event.extra[3], event.extra[4],
                    event.extra[5], event.extra[6], event.extra[7], event.extra[8], event.extra[9],
                    event.extra[10], event.extra[11], event.extra[12], event.extra[13]);
                break;
            case EventType::MainCameraDepthReadback:
            {
                float minV = 0.0f, maxV = 0.0f, meanV = 0.0f;
                memcpy(&minV, &event.i, sizeof(float));
                memcpy(&maxV, &event.j, sizeof(float));
                memcpy(&meanV, &event.extra[0], sizeof(float));
                AppendFormatted(output, sizeof(output), offset,
                    "[POSTFX:MAIN_DEPTH_READBACK] present=%u createHr=%08X readbackHr=%08X lockHr=%08X size=%ux%u finite=%u changedFromClear=%u min=%.9g max=%.9g mean=%.9g runState=%d command=%08X materialType=%u\r\n",
                    event.a, event.b, event.c, event.d, event.e, event.f, event.g, event.h,
                    minV, maxV, meanV, static_cast<int32_t>(event.extra[1]), event.extra[2], event.extra[3]);
                break;
            }
            case EventType::ShadowGraphRTPin:
                AppendFormatted(output, sizeof(output), offset,
                    "[POSTFX:SHADOW_GRAPH_RT_PIN] present=%u attempts=%u redirects=%u worldScene=%08X replacementRT=%08X actualRTBefore=%08X actualRTAfter=%08X lastReturn=%08X pinHr=%08X replayed=%u shadowPS=%08X lastRedirectHr=%08X worldRTWrapper=%08X worldRTSurface=%08X\r\n",
                    event.a, event.b, event.c, event.d, event.e, event.f, event.g, event.h, event.i, event.j,
                    event.extra[0], event.extra[1], event.extra[2], event.extra[3]);
                break;
            case EventType::RenderListTopologyProfile:
                AppendFormatted(output, sizeof(output), offset,
                    "[POSTFX:RENDER_LIST_TOPOLOGY] present=%u index=%u count=%u sampleNode=%08X vtable=%08X execute=%08X scanned=%u unique=%u directPhat=%u overflowUnique=%u\r\n",
                    event.a, event.b, event.c, event.d, event.e, event.f, event.g, event.h, event.i, event.j);
                break;
            case EventType::PhatCallerProfile:
                AppendFormatted(output, sizeof(output), offset,
                    "[POSTFX:PHAT_CALLER_PROFILE] present=%u index=%u command=%08X material=%08X materialType=%u return=%08X frames=%u scene=%08X stack={%08X,%08X,%08X,%08X,%08X,%08X,%08X,%08X} name=\"%s\"\r\n",
                    event.a, event.b, event.c, event.d, event.e, event.f, event.g, event.h,
                    event.extra[0], event.extra[1], event.extra[2], event.extra[3],
                    event.extra[4], event.extra[5], event.extra[6], event.extra[7], event.text);
                break;
            case EventType::SceneListProfile:
                AppendFormatted(output, sizeof(output), offset,
                    "[POSTFX:NGL_SCENE_LIST] present=%u index=%u scene=%08X depth=%u parent=%08X rt=%08X zt=%08X size=%ux%u projType=%u opaqueHead=%08X transHead=%08X opaqueCount=%u transCount=%u opaqueScanned=%u transScanned=%u sentinel=%08X name=\"%s\"\r\n",
                    event.a, event.b, event.c, event.d, event.e, event.f, event.g, event.h, event.i, event.j,
                    event.extra[0], event.extra[1], event.extra[2], event.extra[3], event.extra[4], event.extra[5], event.extra[6], event.text);
                break;
            case EventType::SceneNodeClassProfile:
                AppendFormatted(output, sizeof(output), offset,
                    "[POSTFX:NGL_NODE_CLASS] present=%u scene=%08X depth=%u parent=%08X list=%u count=%u sampleNode=%08X vtable=%08X render=%08X scanned=%u declared=%u name=\"%s\"\r\n",
                    event.a, event.b, event.c, event.d, event.e, event.f, event.g, event.h, event.i, event.j,
                    event.extra[0], event.text);
                break;
            case EventType::SceneTreeProfileSummary:
                AppendFormatted(output, sizeof(output), offset,
                    "[POSTFX:NGL_SCENE_TREE_SUMMARY] present=%u scenes=%u classEvents=%u opaqueNodes=%u transNodes=%u readFailures=%u rootScene=%08X rootDepth=%u startedHere=%u result=%u name=\"%s\"\r\n",
                    event.a, event.b, event.c, event.d, event.e, event.f, event.g, event.h, event.i, event.j, event.text);
                break;
            case EventType::OffscreenIntzTargetProbe:
                AppendFormatted(output, sizeof(output), offset,
                    "[POSTFX:OFFSCREEN_INTZ_TARGET] present=%u ready=%u size=%ux%u intzTex=%08X intzDS=%08X intzFmt=%08X containerTex=%08X createHr=%08X getSurfaceHr=%08X descHr=%08X containerHr=%08X containerMatch=%u colorRT=%08X colorFmt=%08X stateBlockHr=%08X captureHr=%08X bindRTHr=%08X bindDSHr=%08X clearHr=%08X actualRT=%08X actualDS=%08X restoreDSHr=%08X restoreRTHr=%08X applyHr=%08X scene=%08X\r\n",
                    event.a, event.b, event.c, event.d, event.e, event.f, event.g, event.h, event.i, event.j,
                    event.extra[0], event.extra[1], event.extra[2], event.extra[3], event.extra[4],
                    event.extra[5], event.extra[6], event.extra[7], event.extra[8], event.extra[9],
                    event.extra[10], event.extra[11], event.extra[12], event.extra[13], event.extra[14], event.extra[15]);
                break;
            case EventType::IntzSampleReadbackProbe:
            {
                float expected = 0.0f, reconstructed = 0.0f;
                memcpy(&expected, &event.extra[1], sizeof(float));
                memcpy(&reconstructed, &event.extra[2], sizeof(float));
                AppendFormatted(output, sizeof(output), offset,
                    "[POSTFX:INTZ_SAMPLE_READBACK] present=%u ok=%u size=%ux%u clearHr=%08X packHr=%08X createReadbackHr=%08X getDataHr=%08X lockHr=%08X unlockHr=%08X packedBGRA=%08X expected=%.9g reconstructed=%.9g intzTex=%08X packedTex=%08X packedSurface=%08X\r\n",
                    event.a, event.b, event.c, event.d, event.e, event.f, event.g, event.h, event.i, event.j,
                    event.extra[0], expected, reconstructed, event.extra[3], event.extra[4], event.extra[5]);
                break;
            }
            case EventType::OpaqueIntzReplayProbe:
            {
                float minV = 1.0f, maxV = 1.0f, meanV = 1.0f;
                memcpy(&minV, &event.extra[0], sizeof(float));
                memcpy(&maxV, &event.extra[1], sizeof(float));
                memcpy(&meanV, &event.extra[2], sizeof(float));
                AppendFormatted(output, sizeof(output), offset,
                    "[POSTFX:OPAQUE_INTZ_REPLAY] present=%u ok=%u scene=%08X declared=%u replayed=%u firstNode=%08X lastNode=%08X badNode=%08X finite=%u changedFromClear=%u min=%.9g max=%.9g mean=%.9g stateBlockHr=%08X captureHr=%08X bindRTHr=%08X bindDSHr=%08X clearHr=%08X restoreDSHr=%08X restoreRTHr=%08X applyHr=%08X packHr=%08X createReadbackHr=%08X getDataHr=%08X lockHr=%08X unlockHr=%08X\r\n",
                    event.a, event.b, event.c, event.d, event.e, event.f, event.g, event.h, event.i, event.j,
                    minV, maxV, meanV, event.extra[3], event.extra[4], event.extra[5], event.extra[6],
                    event.extra[7], event.extra[8], event.extra[9], event.extra[10], event.extra[11],
                    event.extra[12], event.extra[13], event.extra[14], event.extra[15]);
                break;
            }
            case EventType::OpaqueIntzTraversalProof:
                AppendFormatted(output, sizeof(output), offset,
                    "[POSTFX:OPAQUE_INTZ_TRAVERSAL] present=%u scene=%08X declared=%u renderCalls=%u startIndex=%u finalIndex=%u internallyConsumed=%u sentinelReached=%u finalNode=%08X sentinel=%08X\r\n",
                    event.a, event.b, event.c, event.d, event.e, event.f, event.g, event.h, event.i, event.j);
                break;
            case EventType::OpaqueIntzDofSemanticProbe:
            {
                float nearThreshold = 0.0f, farThreshold = 0.0f;
                float minV = 0.0f, maxV = 0.0f, meanV = 0.0f;
                memcpy(&nearThreshold, &event.h, sizeof(float));
                memcpy(&farThreshold, &event.i, sizeof(float));
                memcpy(&minV, &event.extra[2], sizeof(float));
                memcpy(&maxV, &event.extra[3], sizeof(float));
                memcpy(&meanV, &event.extra[4], sizeof(float));
                AppendFormatted(output, sizeof(output), offset,
                    "[POSTFX:INTZ_DOF_SEMANTICS] present=%u scene=%08X finite=%u changed=%u belowNear=%u inBand=%u aboveFar=%u nearThreshold=%.9g farThreshold=%.9g below0_99=%u below0_995=%u below0_999=%u min=%.9g max=%.9g mean=%.9g traversalComplete=%u\r\n",
                    event.a, event.b, event.c, event.d, event.e, event.f, event.g,
                    nearThreshold, farThreshold, event.j, event.extra[0], event.extra[1],
                    minV, maxV, meanV, event.extra[5]);
                break;
            }
            case EventType::ProvenIntzDofActivation:
                AppendFormatted(output, sizeof(output), offset,
                    "[POSTFX:PROVEN_INTZ_DOF_ACTIVATION] present=%u applied=%u colorDescriptor=%08X packedDepth=%08X dofPS=%08X bindHr=%08X c0Hr=%08X psHr=%08X srcBlendHr=%08X dstBlendHr=%08X prevStage1=%08X prevPS=%08X restoreTexHr=%08X restorePsHr=%08X restoreSrcBlendHr=%08X restoreDstBlendHr=%08X depthPresent=%u traversalFinal=%u declared=%u changed=%u windowFrame=%u produced=%u appliedCount=%u failures=%u windowStart=%u windowEnd=%u\r\n",
                    event.a, event.b, event.c, event.d, event.e, event.f, event.g, event.h, event.i, event.j,
                    event.extra[0], event.extra[1], event.extra[2], event.extra[3], event.extra[4], event.extra[5],
                    event.extra[6], event.extra[7], event.extra[8], event.extra[9],
                    event.extra[10], event.extra[11], event.extra[12], event.extra[13], event.extra[14], event.extra[15]);
                break;
            case EventType::DofC0BridgeProbe:
                AppendFormatted(output, sizeof(output), offset,
                    "[POSTFX:DOF_C0_BRIDGE] present=%u dynamic=%u near=%.9g far=%.9g c0={%.9g,%.9g,%.9g,%.9g} builds=%u fallbacks=%u nearBits=%08X farBits=%08X\r\n",
                    event.a, event.b, BitsFloat(event.c), BitsFloat(event.d),
                    BitsFloat(event.e), BitsFloat(event.f), BitsFloat(event.g), BitsFloat(event.h),
                    event.i, event.j, event.extra[0], event.extra[1]);
                break;
            case EventType::FullDofChain:
                AppendFormatted(output, sizeof(output), offset,
                    "[POSTFX:DOF_FULL_CHAIN] present=%u attempt=%u followup=%u final=%u captureHr=%08X downsampleHr=%08X blur1Hr=%08X blur2Hr=%08X restoreHr=%08X finalHr=%08X sceneTex=%08X quarterA=%08X quarterB=%08X sceneSurf=%08X quarterASurf=%08X quarterBSurf=%08X dofPS=%08X pass2PS=%08X blurPS=%08X finalPS=%08X vs=%08X success=%u failures=%u finalDraws=%u prevPS=%08X dofState=%u\r\n",
                    event.a, event.b, event.c, event.d, event.e, event.f, event.g, event.h, event.i, event.j,
                    event.extra[0], event.extra[1], event.extra[2], event.extra[3], event.extra[4], event.extra[5],
                    event.extra[6], event.extra[7], event.extra[8], event.extra[9], event.extra[10], event.extra[11],
                    event.extra[12], event.extra[13], event.extra[14], event.extra[15]);
                break;
            case EventType::NativeBloomTail:
                AppendFormatted(output, sizeof(output), offset,
                    "[POSTFX:NATIVE_BLOOM_TAIL] present=%u call=%u level1=%u level2=%u level1PS=%08X level2PS=%08X level1C0={%.9g,%.9g,%.9g,%.9g} level2C0={%.9g,%.9g,%.9g,%.9g} level1PsHr=%08X level1C0Hr=%08X level1RestorePsHr=%08X level1RestoreC0Hr=%08X level2CaptureHr=%08X level2SetupHr=%08X level2DrawHr=%08X level2RestoreHr=%08X finalSource=%08X successes=%u failures=%u fallbackGaussianHr=%08X\r\n",
                    event.a, event.b, event.c, event.d, event.e, event.f,
                    BitsFloat(event.g), BitsFloat(event.h), BitsFloat(event.i), BitsFloat(event.j),
                    BitsFloat(event.extra[11]), BitsFloat(event.extra[12]),
                    BitsFloat(event.extra[13]), BitsFloat(event.extra[14]),
                    event.extra[0], event.extra[1], event.extra[2], event.extra[3],
                    event.extra[4], event.extra[5], event.extra[6], event.extra[7],
                    event.extra[8], event.extra[9], event.extra[10], event.extra[15]);
                break;
            case EventType::CameraMotionZBlur:
                AppendFormatted(output, sizeof(output), offset,
                    "[POSTFX:CAMERA_MOTION_ZBLUR] present=%u attempt=%u ok=%u vs=%08X ps=%08X c0={%.9g,%.9g,%.9g,%.9g} createHr=%08X captureHr=%08X copySetupHr=%08X copyDrawHr=%08X setupHr=%08X drawHr=%08X restoreHr=%08X copyTex=%08X copySurf=%08X size=%ux%u format=%08X successes=%u failures=%u sceneTex=%08X sceneSurf=%08X dofState=%u controller=%u camera=%08X transform=%08X delta=%.9g avg4=%.9g motion=%.9g blurPixels=%.9g histIndex=%u gate=%u gateReadable=%u gateCalls=%u gateBlocked=%u\r\n",
                    event.a, event.b, event.c, event.d, event.e,
                    BitsFloat(event.f), BitsFloat(event.g), BitsFloat(event.h), BitsFloat(event.i),
                    event.j, event.extra[0], event.extra[1], event.extra[2], event.extra[3], event.extra[4], event.extra[5],
                    event.extra[6], event.extra[7], event.extra[8], event.extra[9], event.extra[10], event.extra[11], event.extra[12], event.extra[13], event.extra[14], event.extra[15],
                    event.extra[16], event.extra[17], event.extra[18], BitsFloat(event.extra[19]), BitsFloat(event.extra[20]), BitsFloat(event.extra[21]), BitsFloat(event.extra[22]), event.extra[23],
                    event.extra[24], event.extra[25], event.extra[26], event.extra[27]);
                break;
            case EventType::CameraMotionZBlurProbe:
                AppendFormatted(output, sizeof(output), offset,
                    "[POSTFX:CAMERA_MOTION_ZBLUR_PROBE] present=%u probe=%u gate=%u wouldRun=%u vs=%08X ps=%08X activeVS=%08X activePS=%08X device=%08X sceneTex=%08X sceneSurf=%08X currentRT=%08X currentDS=%08X scene=%ux%u format=%08X descHr=%08X getRTHr=%08X getDSHr=%08X controller=%u camera=%08X transform=%08X delta=%.9g avg4=%.9g motion=%.9g blurPixels=%.9g scale=%.9g shaderReady=%u sceneReady=%u apiReady=%u dofState=%u gateReadable=%u gateCalls=%u gateBlocked=%u drawEnabled=%u gpuMutations=%u fullscreenQuad=%08X fixedMinScale=%.9g\r\n",
                    event.a, event.b, event.c, event.d, event.e, event.f, event.g, event.h, event.i, event.j,
                    event.extra[0], event.extra[1], event.extra[2], event.extra[3], event.extra[4], event.extra[5],
                    event.extra[6], event.extra[7], event.extra[8], event.extra[9], event.extra[10], event.extra[11],
                    BitsFloat(event.extra[12]), BitsFloat(event.extra[13]), BitsFloat(event.extra[14]), BitsFloat(event.extra[15]),
                    BitsFloat(event.extra[16]), event.extra[17], event.extra[18], event.extra[19], event.extra[20], event.extra[21],
                    event.extra[22], event.extra[23], event.extra[24], event.extra[25], event.extra[26], BitsFloat(event.extra[27]));
                break;
            case EventType::CameraMotionZBlurCopyOnly:
                AppendFormatted(output, sizeof(output), offset,
                    "[POSTFX:CAMERA_MOTION_ZBLUR_COPY_ONLY] present=%u attempt=%u gate=%u copyEligible=%u copyAttempted=%u copyOk=%u createHr=%08X stretchHr=%08X sceneTex=%08X sceneSurf=%08X copyTex=%08X copySurf=%08X size=%ux%u format=%08X beforeRT=%08X afterRT=%08X beforeDS=%08X afterDS=%08X activeVSBefore=%08X activeVSAfter=%08X activePSBefore=%08X activePSAfter=%08X rtStable=%u dsStable=%u shaderStable=%u controller=%u camera=%08X transform=%08X delta=%.9g avg4=%.9g motion=%.9g blurPixels=%.9g scale=%.9g gateReadable=%u gateCalls=%u gateBlocked=%u fullZBlurDraw=0 gpuCopyOps=%u\r\n",
                    event.a, event.b, event.c, event.d, event.e, event.f, event.g, event.h, event.i, event.j,
                    event.extra[0], event.extra[1], event.extra[2], event.extra[3], event.extra[4],
                    event.extra[5], event.extra[6], event.extra[7], event.extra[8],
                    event.extra[9], event.extra[10], event.extra[11], event.extra[12],
                    event.extra[13], event.extra[14], event.extra[15], event.extra[16],
                    event.extra[17], event.extra[18], BitsFloat(event.extra[19]), BitsFloat(event.extra[20]),
                    BitsFloat(event.extra[21]), BitsFloat(event.extra[22]), BitsFloat(event.extra[23]),
                    event.extra[24], event.extra[25], event.extra[26], event.extra[27]);
                break;
            case EventType::CameraMotionZBlurBindOnly:
                AppendFormatted(output, sizeof(output), offset,
                    "[POSTFX:CAMERA_MOTION_ZBLUR_BIND_ONLY] present=%u attempt=%u gate=%u copyOk=%u bindEligible=%u bindAttempted=%u bindOk=%u stretchHr=%08X setVSHr=%08X setPSHr=%08X setC0Hr=%08X restoreVSHr=%08X restorePSHr=%08X restoreC0Hr=%08X getBeforeVSHr=%08X getBeforePSHr=%08X getBeforeC0Hr=%08X targetVS=%08X targetPS=%08X beforeVS=%08X beforePS=%08X boundVS=%08X boundPS=%08X afterVS=%08X afterPS=%08X shaderBindVerified=%u shaderRestoreStable=%u c0BindVerified=%u c0RestoreStable=%u c0={%.9g,%.9g,%.9g,%.9g} rtStable=%u dsStable=%u cacheStable=%u gpuCopyOps=%u createHr=%08X textureBind=0 samplerState=0 renderState=0 rtSwitch=0 dsSwitch=0 viewportChange=0 fullZBlurDraw=0\r\n",
                    event.a, event.b, event.c, event.d, event.e, event.f, event.g, event.h, event.i, event.j,
                    event.extra[0], event.extra[1], event.extra[2], event.extra[3],
                    event.extra[4], event.extra[5], event.extra[6], event.extra[7], event.extra[8],
                    event.extra[9], event.extra[10], event.extra[11], event.extra[12], event.extra[13], event.extra[14],
                    event.extra[15], event.extra[16], event.extra[17], event.extra[18],
                    BitsFloat(event.extra[19]), BitsFloat(event.extra[20]), BitsFloat(event.extra[21]), BitsFloat(event.extra[22]),
                    event.extra[23], event.extra[24], event.extra[25], event.extra[26], event.extra[27]);
                break;
            case EventType::CameraMotionZBlurTextureBindOnly:
                AppendFormatted(output, sizeof(output), offset,
                    "[POSTFX:CAMERA_MOTION_ZBLUR_TEXTURE_BIND_ONLY] present=%u attempt=%u gate=%u copyOk=%u textureEligible=%u textureBindAttempted=%u isolationOk=%u stretchHr=%08X createHr=%08X setTextureHr=%08X restoreTextureHr=%08X getBeforeTextureHr=%08X getBoundTextureHr=%08X getAfterTextureHr=%08X beforeTexture=%08X boundTexture=%08X targetTexture=%08X afterTexture=%08X textureBindVerified=%u textureRestoreStable=%u shaderBindVerified=%u shaderRestoreStable=%u c0BindVerified=%u c0RestoreStable=%u c0={%.9g,%.9g,%.9g,%.9g} rtStable=%u dsStable=%u shaderCacheStable=%u textureCacheStable=%u setVSHr=%08X setPSHr=%08X setC0Hr=%08X restoreVSHr=%08X restorePSHr=%08X restoreC0Hr=%08X samplerState=0 renderState=0 rtSwitch=0 dsSwitch=0 viewportChange=0 fullZBlurDraw=0\r\n",
                    event.a, event.b, event.c, event.d, event.e, event.f, event.g, event.h, event.i, event.j,
                    event.extra[0], event.extra[1], event.extra[2], event.extra[3],
                    event.extra[4], event.extra[5], event.extra[6], event.extra[7],
                    event.extra[8], event.extra[9], event.extra[10], event.extra[11], event.extra[12], event.extra[13],
                    BitsFloat(event.extra[14]), BitsFloat(event.extra[15]), BitsFloat(event.extra[16]), BitsFloat(event.extra[17]),
                    event.extra[18], event.extra[19], event.extra[20], event.extra[21],
                    event.extra[22], event.extra[23], event.extra[24], event.extra[25], event.extra[26], event.extra[27]);
                break;
            case EventType::CameraMotionZBlurSamplerStateOnly:
                AppendFormatted(output, sizeof(output), offset,
                    "[POSTFX:CAMERA_MOTION_ZBLUR_SAMPLER_ONLY] present=%u attempt=%u gate=%u copyOk=%u samplerEligible=%u samplerAttempted=%u isolationOk=%u stretchHr=%08X createHr=%08X setTextureHr=%08X restoreTextureHr=%08X setSamplerHr=%08X restoreSamplerHr=%08X captureMask=%02X setMask=%02X boundMask=%02X restoreSetMask=%02X afterMask=%02X samplerBindVerified=%u samplerRestoreStable=%u textureBindVerified=%u textureRestoreStable=%u shaderBindVerified=%u shaderRestoreStable=%u c0BindVerified=%u c0RestoreStable=%u rtStable=%u dsStable=%u shaderCacheStable=%u textureCacheStable=%u setVSHr=%08X setPSHr=%08X setC0Hr=%08X restoreVSHr=%08X restorePSHr=%08X restoreC0Hr=%08X getBeforeTextureHr=%08X getAfterTextureHr=%08X textureStageState=0 renderState=0 rtSwitch=0 dsSwitch=0 viewportChange=0 fullZBlurDraw=0\r\n",
                    event.a, event.b, event.c, event.d, event.e, event.f, event.g, event.h, event.i, event.j,
                    event.extra[0], event.extra[1], event.extra[2], event.extra[3], event.extra[4],
                    event.extra[5], event.extra[6], event.extra[7], event.extra[8], event.extra[9],
                    event.extra[10], event.extra[11], event.extra[12], event.extra[13], event.extra[14],
                    event.extra[15], event.extra[16], event.extra[17], event.extra[18], event.extra[19],
                    event.extra[20], event.extra[21], event.extra[22], event.extra[23], event.extra[24],
                    event.extra[25], event.extra[26], event.extra[27]);
                break;
            case EventType::CameraMotionZBlurRenderStateOnly:
                AppendFormatted(output, sizeof(output), offset,
                    "[POSTFX:CAMERA_MOTION_ZBLUR_RENDER_STATE_ONLY] present=%u attempt=%u gate=%u copyOk=%u renderEligible=%u renderAttempted=%u isolationOk=%u stretchHr=%08X setRenderHr=%08X restoreRenderHr=%08X captureMask=%03X setMask=%03X boundMask=%03X restoreSetMask=%03X afterMask=%03X renderBindVerified=%u renderRestoreStable=%u samplerBindVerified=%u samplerRestoreStable=%u textureBindVerified=%u textureRestoreStable=%u shaderBindVerified=%u shaderRestoreStable=%u c0BindVerified=%u c0RestoreStable=%u rtStable=%u dsStable=%u shaderCacheStable=%u textureCacheStable=%u setSamplerHr=%08X restoreSamplerHr=%08X setVSHr=%08X setPSHr=%08X setC0Hr=%08X restoreVSHr=%08X restorePSHr=%08X restoreC0Hr=%08X textureStageState=0 rtSwitch=0 dsSwitch=0 viewportChange=0 fullZBlurDraw=0\r\n",
                    event.a, event.b, event.c, event.d, event.e, event.f, event.g, event.h, event.i, event.j,
                    event.extra[0], event.extra[1], event.extra[2], event.extra[3], event.extra[4],
                    event.extra[5], event.extra[6], event.extra[7], event.extra[8], event.extra[9],
                    event.extra[10], event.extra[11], event.extra[12], event.extra[13], event.extra[14],
                    event.extra[15], event.extra[16], event.extra[17], event.extra[18], event.extra[19],
                    event.extra[20], event.extra[21], event.extra[22], event.extra[23], event.extra[24],
                    event.extra[25], event.extra[26]);
                break;
            case EventType::CameraMotionZBlurOutputTargetOnly:
                AppendFormatted(output, sizeof(output), offset,
                    "[POSTFX:CAMERA_MOTION_ZBLUR_OUTPUT_TARGET_ONLY] present=%u attempt=%u gate=%u copyOk=%u outputEligible=%u outputAttempted=%u isolationOk=%u stretchHr=%08X copySetRTHr=%08X copySetDSHr=%08X copySetViewportHr=%08X sceneSetRTHr=%08X sceneSetDSHr=%08X sceneSetViewportHr=%08X restoreRTHr=%08X restoreDSHr=%08X restoreViewportHr=%08X beforeRT=%08X copyRT=%08X boundCopyRT=%08X sceneRT=%08X boundSceneRT=%08X afterRT=%08X beforeDS=%08X boundCopyDS=%08X boundSceneDS=%08X afterDS=%08X beforeVP=%ux%u targetVP=%ux%u copyPhaseVerified=%u scenePhaseVerified=%u outputRestoreStable=%u priorChainVerified=%u rtStable=%u dsStable=%u viewportStable=%u fullZBlurDraw=0\r\n",
                    event.a, event.b, event.c, event.d, event.e, event.f, event.g, event.h, event.i, event.j,
                    event.extra[0], event.extra[1], event.extra[2], event.extra[3], event.extra[4],
                    event.extra[5], event.extra[6], event.extra[7], event.extra[8], event.extra[9],
                    event.extra[10], event.extra[11], event.extra[12], event.extra[13], event.extra[14],
                    event.extra[15], event.extra[16], event.extra[17], event.extra[18], event.extra[19],
                    event.extra[20], event.extra[21], event.extra[22], event.extra[23], event.extra[24],
                    event.extra[25], event.extra[26], event.extra[27]);
                break;
            case EventType::ReszDepthResolveProbe:
            {
                float minV = 1.0f, maxV = 1.0f, meanV = 1.0f;
                memcpy(&minV, &event.extra[0], sizeof(float));
                memcpy(&maxV, &event.extra[1], sizeof(float));
                memcpy(&meanV, &event.extra[2], sizeof(float));
                AppendFormatted(output, sizeof(output), offset,
                    "[POSTFX:RESZ_DEPTH_RESOLVE] present=%u ok=%u scene=%08X liveDS=%08X liveFmt=%08X size=%ux%u intzTex=%08X finite=%u changedFromClear=%u min=%.9g max=%.9g mean=%.9g stateBlockHr=%08X captureHr=%08X clearBindHr=%08X clearHr=%08X restoreLiveDSHr=%08X bindIntzTexHr=%08X dummySetupHr=%08X dummyDrawHr=%08X triggerHr=%08X applyHr=%08X stage0Before=%08X cacheBefore=%08X cacheAfter=%08X packHr=%08X getDataHr=%08X lockHr=%08X unlockHr=%08X\r\n",
                    event.a, event.b, event.c, event.d, event.e, event.f, event.g, event.h, event.i, event.j,
                    minV, maxV, meanV,
                    event.extra[3], event.extra[4], event.extra[5], event.extra[6],
                    event.extra[7], event.extra[8], event.extra[9], event.extra[10],
                    event.extra[11], event.extra[12], event.extra[13], event.extra[14], event.extra[15],
                    static_cast<uint32_t>(s_opaqueIntzPackHr),
                    static_cast<uint32_t>(s_opaqueIntzGetDataHr),
                    static_cast<uint32_t>(s_opaqueIntzLockHr),
                    static_cast<uint32_t>(s_opaqueIntzUnlockHr));
                break;
            }
            case EventType::NativeFinalShaderBind:
                AppendFormatted(output, sizeof(output), offset,
                    "[POSTFX:V10572_FINAL_SHADER_BIND] present=%u count=%u composite=%u return=%08X expectedBE64=%08X activePSCache=%08X quarterA=%08X sceneRT=%08X matched=%u captureState=%d\r\n",
                    event.a, event.b, event.c, event.d, event.e, event.f, event.g, event.h,
                    event.i, static_cast<int32_t>(event.j));
                break;
            case EventType::NativeFinalShaderBytecodeHeader:
                AppendFormatted(output, sizeof(output), offset,
                    "[POSTFX:V10572_FINAL_SHADER_CAPTURE] present=%u shader=%08X size=%u fnv1a=%08X queryHr=%08X fetchHr=%08X version=%08X lastToken=%08X chunks=%u state=%d\r\n",
                    event.a, event.b, event.c, event.d, event.e, event.f, event.g, event.h,
                    event.i, static_cast<int32_t>(event.j));
                break;
            case EventType::NativeFinalShaderBytecodeChunk:
                AppendFormatted(output, sizeof(output), offset,
                    "[POSTFX:V10572_FINAL_SHADER_DWORDS] chunk=%u/%u offset=%u count=%u data=",
                    event.a + 1u, event.b, event.c, event.d);
                for (uint32_t n = 0; n < event.d && n < 28u; ++n)
                    AppendFormatted(output, sizeof(output), offset, "%s%08X", n ? "," : "", event.extra[n]);
                AppendFormatted(output, sizeof(output), offset, "\r\n");
                break;
            case EventType::NativeFinalShaderCloneOverride:
                AppendFormatted(output, sizeof(output), offset,
                    "[POSTFX:V10572_FINAL_SHADER_CLAMP] present=%u draw=%u return=%08X original=%08X override=%08X before=%08X bound=%08X afterDraw=%08X restored=%08X pass=%u state=%d srcHash=%08X overrideHash=%08X createHr=%08X verifyQueryHr=%08X verifyFetchHr=%08X beforeGetHr=%08X bindHr=%08X boundGetHr=%08X afterDrawGetHr=%08X restoreHr=%08X restoredGetHr=%08X beforeVerified=%u bindVerified=%u drawHeldOverride=%u restoreVerified=%u passCount=%u failCount=%u activePSCache=%08X clampState=%d sourceWord=%08X patchedWord=%08X patchedHash=%08X patchedBytes=%u mov={%08X %08X %08X}\r\n",
                    event.a, event.b, event.c, event.d, event.e, event.f, event.g, event.h, event.i, event.j,
                    static_cast<int32_t>(event.extra[0]), event.extra[1], event.extra[2], event.extra[3],
                    event.extra[4], event.extra[5], event.extra[6], event.extra[7], event.extra[8], event.extra[9],
                    event.extra[10], event.extra[11], event.extra[12], event.extra[13], event.extra[14], event.extra[15],
                    event.extra[16], event.extra[17], event.extra[18], static_cast<int32_t>(event.extra[19]),
                    event.extra[20], event.extra[21], event.extra[22], event.extra[23],
                    event.extra[24], event.extra[25], event.extra[26]);
                break;
            case EventType::RetailXboxDofContractProbe:
                break;
            case EventType::RetailXboxDepthFeedProbe:
            {
                float c0[4] = {};
                memcpy(&c0[0], &event.extra[17], sizeof(float));
                memcpy(&c0[1], &event.extra[18], sizeof(float));
                memcpy(&c0[2], &event.extra[19], sizeof(float));
                memcpy(&c0[3], &event.extra[20], sizeof(float));
                AppendFormatted(output, sizeof(output), offset,
                    "[POSTFX:V10572_XBOX_SHARED_DEPTH_FEED] present=%u attempt=%u return=%08X expectedDepth=%08X s0=%08X oldS1=%08X boundS1=%08X restoredS1=%08X dofPS=%08X pass=%u depthReady=%u reszState=%u producerTrusted=%u depthPresent=%u depthAge=%u bindDepthHr=%08X boundDepthGetHr=%08X bindF18Hr=%08X boundPsGetHr=%08X restorePsHr=%08X restoreS1Hr=%08X restoredS1GetHr=%08X s1MatchesDepth=%u f18BindVerified=%u psRestoreVerified=%u s1RestoreVerified=%u c0Hr=%08X c0={%.7g,%.7g,%.7g,%.7g} passCount=%u failCount=%u dofValid=%u s0Present=%u c0Readable=%u drawWithDof=%u route=%u\r\n",
                    event.a, event.b, event.c, event.d, event.e, event.f, event.g, event.h, event.i, event.j,
                    event.extra[0], event.extra[1], event.extra[2], event.extra[3], event.extra[4],
                    event.extra[5], event.extra[6], event.extra[7], event.extra[8], event.extra[9], event.extra[10], event.extra[11],
                    event.extra[12], event.extra[13], event.extra[14], event.extra[15], event.extra[16],
                    c0[0], c0[1], c0[2], c0[3], event.extra[21], event.extra[22], event.extra[23], event.extra[24],
                    event.extra[25], event.extra[26], event.extra[27]);
                break;
            }
            case EventType::RetailXboxDofNativeDraw:
                AppendFormatted(output, sizeof(output), offset,
                    "[POSTFX:V10572_XBOX_SHARED_F18_DRAW] present=%u attempt=%u return=%08X expectedDepth=%08X s0=%08X oldS1=%08X boundS1=%08X dofPS=%08X afterDrawPS=%08X pass=%u depthReady=%u reszState=%u producerTrusted=%u depthPresent=%u depthAge=%u dynamicC0=%u s1MatchesDepth=%u c0Bound=%u f18Bound=%u drewWithDof=%u drawHeldF18=%u drawHeldDepth=%u psRestore=%u s1Restore=%u c0Restore=%u drawCount=%u drawPass=%u fallback=%u drawFail=%u disabled=%u c0={%.7g,%.7g,%.7g,%.7g} bindDepthHr=%08X setC0Hr=%08X bindF18Hr=%08X route=%u\r\n",
                    event.a, event.b, event.c, event.d, event.e, event.f, event.g, event.h, event.i, event.j,
                    event.extra[0], event.extra[1], event.extra[2], event.extra[3], event.extra[4],
                    event.extra[5], event.extra[6], event.extra[7], event.extra[8], event.extra[9],
                    event.extra[10], event.extra[11], event.extra[12], event.extra[13], event.extra[14],
                    event.extra[15], event.extra[16], event.extra[17], event.extra[18], event.extra[19],
                    BitsFloat(event.extra[20]), BitsFloat(event.extra[21]), BitsFloat(event.extra[22]), BitsFloat(event.extra[23]),
                    event.extra[24], event.extra[25], event.extra[26], event.extra[27]);
                break;
            case EventType::RetailXboxZBlurContractProbe:
                AppendFormatted(output, sizeof(output), offset,
                    "[POSTFX:V10572_XBOX_SHARED_ZBLUR_CONTRACT] present=%u probe=%u gate=%u captureState=%d vs=%08X ps=%08X activeVS=%08X activePS=%08X sceneRT=%08X sceneSurface=%08X sceneTexture=%08X shaderPairReady=%u sceneReady=%u gateReadable=%u controllerValid=%u camera=%08X transform=%08X delta=%.7g average=%.7g motion=%.7g blurPixels=%.7g dynamicScale=%.7g vsBytes=%u vsHash=%08X vsVersion=%08X vsLast=%08X psBytes=%u psHash=%08X psVersion=%08X psLast=%08X vsState=%d psState=%d vsQueryHr=%08X vsFetchHr=%08X psQueryHr=%08X psFetchHr=%08X gpuMutationOrDraw=%u route=%u\r\n",
                    event.a, event.b, event.c, static_cast<int32_t>(event.d),
                    event.e, event.f, event.g, event.h, event.i, event.j,
                    event.extra[0], event.extra[1], event.extra[2], event.extra[3], event.extra[4],
                    event.extra[5], event.extra[6], BitsFloat(event.extra[7]), BitsFloat(event.extra[8]),
                    BitsFloat(event.extra[9]), BitsFloat(event.extra[10]), BitsFloat(event.extra[11]),
                    event.extra[12], event.extra[13], event.extra[14], event.extra[15],
                    event.extra[16], event.extra[17], event.extra[18], event.extra[19],
                    static_cast<int32_t>(event.extra[20]), static_cast<int32_t>(event.extra[21]),
                    event.extra[22], event.extra[23], event.extra[24], event.extra[25],
                    event.extra[26], event.extra[27]);
                break;
            case EventType::RetailXboxZBlurShaderChunk:
                AppendFormatted(output, sizeof(output), offset,
                    "[POSTFX:V10572_XBOX_SHARED_ZBLUR_DWORDS] shader=%s chunk=%u/%u offset=%u count=%u object=%08X data=",
                    event.a == 0u ? "VS" : "PS", event.b + 1u, event.c,
                    event.d, event.e, event.f);
                for (uint32_t n = 0u; n < event.e && n < 28u; ++n)
                    AppendFormatted(output, sizeof(output), offset, "%s%08X", n ? "," : "", event.extra[n]);
                AppendFormatted(output, sizeof(output), offset, "\r\n");
                break;
            case EventType::RetailXboxImageZoomRouteBindProbe:
                AppendFormatted(output, sizeof(output), offset,
                    "[POSTFX:V10572_XBOX_SHARED_IMAGEZOOM_ROUTE_BIND] present=%u probe=%u gate=%u pass=%u sourceTex=%08X sourceSurface=%08X targetRT=%08X sceneWrapper=%08X sceneSurface=%08X targetContainerTex=%08X sourceTargetAlias=%u sceneTargetVerified=%u copyOk=%u stretchHr=%08X vs=%08X ps=%08X vsHash=%08X psHash=%08X motion=%.7g blurPixels=%.7g zoomScale=%.7g shaderVerified=%u vsC0Verified=%u textureVerified=%u samplerVerified=%u shaderRestore=%u vsC0Restore=%u textureRestore=%u samplerRestore=%u rtStable=%u dsStable=%u samplerCaptureMask=%02X samplerSetMask=%02X samplerBoundMask=%02X samplerRestoreMask=%02X passCount=%u failCount=%u zblurDraws=%u route=%u\r\n",
                    event.a, event.b, event.c, event.d, event.e, event.f, event.g, event.h, event.i, event.j,
                    event.extra[0], event.extra[1], event.extra[2], event.extra[3], event.extra[4], event.extra[5],
                    event.extra[6], event.extra[7], BitsFloat(event.extra[8]), BitsFloat(event.extra[9]), BitsFloat(event.extra[10]),
                    event.extra[11], event.extra[12], event.extra[13], event.extra[14], event.extra[15], event.extra[16],
                    event.extra[17], event.extra[18], event.extra[19], event.extra[20], event.extra[21], event.extra[22],
                    event.extra[23], event.extra[24], event.extra[25], event.extra[26], event.extra[27], s_postProcessRoute);
                break;
            case EventType::RetailXboxImageZoomNativeDraw:
                AppendFormatted(output, sizeof(output), offset,
                    "[POSTFX:V10572_XBOX_SHARED_IMAGEZOOM_DRAW] present=%u attempt=%u gate=%u pass=%u sourceTex=%08X sourceSurface=%08X targetRT=%08X sceneSurface=%08X sourceTargetAlias=%u drew=%u stretchHr=%08X vs=%08X ps=%08X vsHash=%08X psHash=%08X motion=%.7g blurPixels=%.7g zoomScale=%.7g shaderVerified=%u vsC0Verified=%u textureVerified=%u samplerVerified=%u renderVerified=%u drawHr=%08X drawHeldShaders=%u drawHeldVsC0=%u drawHeldTexture=%u drawHeldSampler=%u drawHeldRender=%u shaderRestore=%u vsC0Restore=%u textureRestore=%u samplerRestore=%u renderRestore=%u rtStable=%u dsStable=%u passCount=%u failCount=%u disabled=%u\r\n",
                    event.a,event.b,event.c,event.d,event.e,event.f,event.g,event.h,event.i,event.j,
                    event.extra[0],event.extra[1],event.extra[2],event.extra[3],event.extra[4],
                    BitsFloat(event.extra[5]),BitsFloat(event.extra[6]),BitsFloat(event.extra[7]),
                    event.extra[8],event.extra[9],event.extra[10],event.extra[11],event.extra[12],event.extra[13],
                    event.extra[14],event.extra[15],event.extra[16],event.extra[17],event.extra[18],
                    event.extra[19],event.extra[20],event.extra[21],event.extra[22],event.extra[23],event.extra[24],event.extra[25],
                    event.extra[26],event.extra[27],static_cast<uint32_t>(s_retailXboxImageZoomDrawDisabled));
                break;
            case EventType::Mode5DedicatedDrawHookStatus:
                AppendFormatted(output, sizeof(output), offset,
                    "[POSTFX:V10572_MODE5_DEVICE_HOOKS] present=%u phase=%08X state=%d device=%08X drawTarget=%08X error=%08X attempts=%u attaches=%u fails=%u drawSlot=%u broadRawHookState=%d route=%u mode=%u resetSlot=%u resetTarget=%08X\r\n",
                    event.a,event.b,static_cast<int32_t>(event.c),event.d,event.e,event.f,
                    event.g,event.h,event.i,event.j,static_cast<int32_t>(event.extra[0]),event.extra[1],event.extra[2],
                    event.extra[3],event.extra[4]);
                break;
            case EventType::RetailXboxActualFinalDraw:
                AppendFormatted(output, sizeof(output), offset,
                    "[POSTFX:V10572_XBOX_SHARED_ACTUAL_F18_DRAW] present=%u attempt=%u return=%08X expectedBE64=%08X f18=%08X depth=%08X beforePS=%08X afterPS=%08X pass=%u drew=%u depthReady=%u depthAge=%u dofValid=%u beforeVerified=%u sourcePresent=%u dynamicC0=%u depthVerified=%u c0Verified=%u f18Verified=%u drawHeldF18=%u drawHeldDepth=%u psRestore=%u depthRestore=%u c0Restore=%u drawHr=%08X primitive=%u start=%u count=%u actualDraws=%u actualPass=%u actualFail=%u disabled=%u c0={%.7g,%.7g,%.7g,%.7g} route=%u imageZoomDraws=%u\r\n",
                    event.a,event.b,event.c,event.d,event.e,event.f,event.g,event.h,event.i,event.j,
                    event.extra[0],event.extra[1],event.extra[2],event.extra[3],event.extra[4],event.extra[5],
                    event.extra[6],event.extra[7],event.extra[8],event.extra[9],event.extra[10],event.extra[11],
                    event.extra[12],event.extra[13],event.extra[14],event.extra[15],event.extra[16],event.extra[17],
                    event.extra[18],event.extra[19],event.extra[20],event.extra[21],
                    BitsFloat(event.extra[22]),BitsFloat(event.extra[23]),BitsFloat(event.extra[24]),BitsFloat(event.extra[25]),
                    event.extra[26],event.extra[27]);
                break;
            case EventType::NativeFinalShaderActualClampDraw:
                AppendFormatted(output, sizeof(output), offset,
                    "[POSTFX:V10572_ACTUAL_CLAMP_DRAW] present=%u attempt=%u return=%08X expectedBE64=%08X override=%08X before=%08X bound=%08X after=%08X restored=%08X pass=%u cloneReady=%u beforeVerified=%u bindVerified=%u drawHeldOverride=%u restoreVerified=%u drawHr=%08X bindHr=%08X afterGetHr=%08X restoreHr=%08X passCount=%u failCount=%u stockFallback=%u primitive=%u start=%u count=%u route=%u\r\n",
                    event.a,event.b,event.c,event.d,event.e,event.f,event.g,event.h,event.i,event.j,
                    event.extra[0],event.extra[1],event.extra[2],event.extra[3],event.extra[4],event.extra[5],
                    event.extra[6],event.extra[7],event.extra[8],event.extra[9],event.extra[10],event.extra[11],
                    event.extra[12],event.extra[13],event.extra[14],event.extra[15]);
                break;
            case EventType::NativeFinalShaderSummary:
                AppendFormatted(output, sizeof(output), offset,
                    "[POSTFX:V10572_FINAL_SHADER_SUMMARY] present=%u binds=%u captureState=%d size=%u srcHash=%08X expectedBE64=%08X activePSCache=%08X mismatches=%u overrideState=%d overrideDraws=%u overridePass=%u overrideFail=%u override=%08X overrideHash=%08X clampState=%d sourceWord=%08X patchedWord=%08X patchedHash=%08X patchedBytes=%u retailDofAttempts=%u retailDofPass=%u retailDofFail=%u retailDofBytes=%u retailDofHash=%08X depthArms=%u depthArmFail=%u depthFeedAttempts=%u depthFeedPass=%u depthFeedFail=%u producerTrusted=%u depthPresent=%u depthTex=%08X f18DrawAttempts=%u f18Draws=%u f18Pass=%u f18Fallback=%u f18Fail=%u disabled=%u\r\n",
                    event.a, event.b, static_cast<int32_t>(event.c), event.d, event.e, event.f, event.g, event.h,
                    static_cast<int32_t>(event.extra[0]), event.extra[1], event.extra[2], event.extra[3], event.extra[4], event.extra[5],
                    static_cast<int32_t>(event.extra[6]), event.extra[7], event.extra[8], event.extra[9], event.extra[10],
                    event.extra[11], event.extra[12], event.extra[13], event.extra[14], event.extra[15], event.extra[16], event.extra[17],
                    event.extra[18], event.extra[19], event.extra[20], event.extra[21], event.extra[22], event.extra[23],
                    event.extra[24], event.extra[25], event.extra[26], event.extra[27], event.i, event.j);
                break;
            case EventType::RetailXboxIntegrationHeartbeat:
                AppendFormatted(output, sizeof(output), offset,
                    "[POSTFX:V10572_XBOX_SHARED_HEARTBEAT] present=%u route=%u drawHookState=%d routeMismatch=%u depthArms=%u depthArmFail=%u depthReady=%u depthPresent=%u depthTex=%08X f18Attempts=%u f18Draws=%u f18Pass=%u f18Fallback=%u f18Fail=%u f18Disabled=%u actualPass=%u actualFail=%u imageZoomAttempts=%u imageZoomDraws=%u imageZoomPass=%u imageZoomFail=%u imageZoomSkip=%u imageZoomDisabled=%u vsHash=%08X psHash=%08X sourceTex=%08X sourceSurface=%08X godRaySelector=%08X godRayTransitions=%u godRayFinalSeen=%u godRayContractPass=%u godRayContractFail=%u godRayG2Attempts=%u godRayG2Pass=%u godRayG2Fail=%u xeSM3GodRayDraws=%u godRayG3Pass=%u godRayG3Fail=%u godRayG4Attempts=%u godRayStockSuppressed=%u godRayStockFallback=%u deviceResets=%u\r\n",
                    event.a,event.b,static_cast<int32_t>(event.c),event.d,event.e,event.f,event.g,event.h,event.i,event.j,
                    event.extra[0],event.extra[1],event.extra[2],event.extra[3],event.extra[4],event.extra[5],event.extra[6],
                    event.extra[7],event.extra[8],event.extra[9],event.extra[10],event.extra[11],event.extra[12],event.extra[13],
                    event.extra[14],event.extra[15],event.extra[16],event.extra[17],event.extra[18],event.extra[19],event.extra[20],event.extra[21],
                    event.extra[22],event.extra[23],event.extra[24],event.extra[25],event.extra[26],event.extra[27],
                    event.extra[28],event.extra[29],event.extra[30],event.extra[31]);
                break;
            case EventType::DebugXboxD2Probe:
                AppendFormatted(output, sizeof(output), offset,
                    "[POSTFX:V10572_DEBUG_XBOX_D2] present=%u probe=%u sampleAttempt=%u samplePass=%u sampleValid=%u sampleAge=%u brightness255=%.7g t=%.7g debugBloom=%.7g retailBloomCompare=%.7g darkBrightness=%.7g lightBrightness=%.7g debugDarkBloom=%.7g debugLightBloom=%.7g retailDarkBloom=%.7g retailLightBloom=%.7g pcGodRaySelectorReadable=%u pcGodRaySelectorRaw=%08X godRayWouldBranch=%u xboxDebugSelectorDefault=%u debugBloomApplied=%u godRayApplied=%u f18Pass=%u imageZoomPass=%u route=%u lumSurf=%08X readbackHr=%08X lockHr=%08X sampleCount=%u samplePresent=%u dofHash=%08X vsHash=%08X psHash=%08X routeMismatch=%u f18Disabled=%u imageZoomDisabled=%u depthReady=%u readbackAttempts=%u readbackPass=%u readbackFail=%u bloomApplyAttempts=%u bloomApplyPass=%u bloomApplyFail=%u bloomApplySkip=%u lastApplyPresent=%u lastAppliedBloom=%.7g\r\n",
                    event.a,event.b,event.c,event.d,event.e,event.f,BitsFloat(event.g),BitsFloat(event.h),BitsFloat(event.i),BitsFloat(event.j),
                    BitsFloat(event.extra[0]),BitsFloat(event.extra[1]),BitsFloat(event.extra[2]),BitsFloat(event.extra[3]),BitsFloat(event.extra[4]),BitsFloat(event.extra[5]),
                    event.extra[6],event.extra[7],event.extra[8],event.extra[9],event.extra[10],event.extra[11],event.extra[12],event.extra[13],event.extra[14],event.extra[15],
                    event.extra[16],event.extra[17],event.extra[18],event.extra[19],event.extra[20],event.extra[21],event.extra[22],event.extra[23],event.extra[24],event.extra[25],
                    event.extra[26],event.extra[27],static_cast<uint32_t>(s_debugXboxBloomReadbackPassCount),static_cast<uint32_t>(s_debugXboxBloomReadbackFailCount),
                    static_cast<uint32_t>(s_debugXboxD2BloomApplyAttemptCount),
                    static_cast<uint32_t>(s_debugXboxD2BloomApplyPassCount),
                    static_cast<uint32_t>(s_debugXboxD2BloomApplyFailCount),
                    static_cast<uint32_t>(s_debugXboxD2BloomApplySkipCount),
                    s_debugXboxD2LastApplyPresent, s_debugXboxD2LastAppliedBloom);
                break;
            case EventType::PermanentReszDofHeartbeat:
                AppendFormatted(output, sizeof(output), offset,
                    "[POSTFX:RESZ_DOF_HEARTBEAT] present=%u produced=%u applied=%u failures=%u dofState=%u reszState=%u rawHookState=%u packedDepth=%08X intzTex=%08X depthPresent=%u depthAge=%u worldFrames=%u liveFmt=%08X size=%ux%u triggerHr=%08X applyHr=%08X dofBindHr=%08X c0Hr=%08X psHr=%08X restoreTexHr=%08X restorePsHr=%08X sceneTex=%08X quarterATex=%08X quarterBTex=%08X rawIdMask=%02X latePatchState=%d lateHits=%u lateReturn=%08X outputAttempts=%u outputPass=%u outputFail=%u callsiteBytes={%02X %02X %02X %02X %02X}\r\n",
                    event.a, event.b, event.c, event.d, event.e, event.f, event.g, event.h, event.i, event.j,
                    event.extra[11], event.extra[0], event.extra[1], event.extra[2], event.extra[3], event.extra[4], event.extra[5],
                    event.extra[6], event.extra[7], event.extra[8], event.extra[9], event.extra[10],
                    event.extra[12], event.extra[13], event.extra[14], event.extra[15],
                    static_cast<int32_t>(event.extra[16]), event.extra[17], event.extra[18],
                    event.extra[19], event.extra[20], event.extra[21],
                    event.extra[22], event.extra[23], event.extra[24], event.extra[25], event.extra[26]);
                break;
            case EventType::RawSetPixelShader:
                AppendFormatted(output, sizeof(output), offset,
                    "[POSTFX:V8_D3D_PS] present=%u return=%08X ps=%08X hr=%08X rt0=%08X tex0=%08X tex1=%08X tex2=%08X tex3=%08X scene=%08X name=\"%s\"\r\n",
                    event.a, event.b, event.c, event.d, event.e, event.f, event.g, event.h, event.i, event.j, event.text);
                break;
            case EventType::RawPixelShaderConstantF:
            {
                float values[16] = {};
                for (uint32_t n = 0; n < event.i * 4u && n < 16u; ++n)
                    memcpy(&values[n], &event.extra[n], sizeof(float));
                AppendFormatted(output, sizeof(output), offset,
                    "[POSTFX:V8_D3D_PSCONST] present=%u return=%08X start=%u vectors=%u captured=%u ps=%08X rt0=%08X hr=%08X scene=%08X name=\"%s\" "
                    "v0={%.7g,%.7g,%.7g,%.7g} v1={%.7g,%.7g,%.7g,%.7g} v2={%.7g,%.7g,%.7g,%.7g} v3={%.7g,%.7g,%.7g,%.7g}\r\n",
                    event.a, event.b, event.c, event.d, event.i, event.e, event.f, event.g, event.h, event.text,
                    values[0], values[1], values[2], values[3], values[4], values[5], values[6], values[7],
                    values[8], values[9], values[10], values[11], values[12], values[13], values[14], values[15]);
                break;
            }
            case EventType::RawIdentityStatus:
                AppendFormatted(output, sizeof(output), offset,
                    "[POSTFX:V8_1_IDENTITY] present=%u which=%s source=%s wrapper=%08X surface=%08X texture=%08X getContainerHr=%08X activePS=%08X rt0=%08X\r\n",
                    event.a, event.b == 1u ? "E8FB04" : "E8FBE8",
                    event.c == 1u ? "SetRT" : (event.c == 2u ? "wrapper+34" : "legacyFallback"),
                    event.d, event.e, event.f, event.g, event.h, event.i);
                break;
            case EventType::RawSummary:
                AppendFormatted(output, sizeof(output), offset,
                    "[POSTFX:V8_RAW_SUMMARY] present=%u hookState=%d rawBindE8FB04=%u rawBindE8FBE8=%u rawSetRTE8FB04=%u rawSetRTE8FBE8=%u rawSetPS=%u rawPSConst=%u lumTex=%08X secondTex=%08X lumSurf=%08X secondSurf=%08X currentPS=%08X currentRT0=%08X lumGetContainerHr=%08X secondGetContainerHr=%08X lumIdentitySource=%u secondIdentitySource=%u\r\n",
                    event.a, static_cast<int32_t>(event.b), event.c, event.d, event.e, event.f, event.g, event.h, event.i, event.j,
                    event.extra[0], event.extra[1], event.extra[2], event.extra[3], event.extra[4], event.extra[5], event.extra[6], event.extra[7]);
                break;
            case EventType::ExposureConfig:
                AppendFormatted(output, sizeof(output), offset,
                    "[POSTFX:V9_CONFIG] present=%u enabled=%u interval=%u warmup=%u min=%.6g max=%.6g darkenAlpha=%.6g brightenAlpha=%.6g\r\n",
                    event.a, event.b, event.c, event.d, BitsFloat(event.e), BitsFloat(event.f), BitsFloat(event.g), BitsFloat(event.h));
                break;
            case EventType::ExposureSample:
                AppendFormatted(output, sizeof(output), offset,
                    "[POSTFX:V9_LUMINANCE] present=%u sample=%u getRTHr=%08X lockHr=%08X avgRGBA={%.7g,%.7g,%.7g,%.7g} lum=%.7g ref=%.7g targetExposure=%.7g adaptedExposure=%.7g validPixels=%u format=%08X size=%ux%u\r\n",
                    event.a, event.b, event.c, event.d,
                    BitsFloat(event.e), BitsFloat(event.f), BitsFloat(event.g), BitsFloat(event.h),
                    BitsFloat(event.i), BitsFloat(event.j), BitsFloat(event.extra[0]), BitsFloat(event.extra[1]),
                    event.extra[2], event.extra[3], event.extra[4], event.extra[5]);
                break;
            case EventType::ExposureApply:
                AppendFormatted(output, sizeof(output), offset,
                    "[POSTFX:V9_APPLY] present=%u apply=%u exposure=%.7g stretchHr=%08X setupHr=%08X drawHr=%08X restoreHr=%08X backbuffer=%08X tempTexture=%08X size=%ux%u format=%08X textureFactor=%08X colorOp=%u getRTHr=%08X getDSHr=%08X beginHr=%08X endHr=%08X\r\n",
                    event.a, event.extra[3], BitsFloat(event.b), event.c, event.d, event.e, event.f, event.g, event.h,
                    event.i, event.j, event.extra[0], event.extra[1], event.extra[2],
                    event.extra[4], event.extra[5], event.extra[6], event.extra[7]);
                break;
            case EventType::ExposureSummary:
                AppendFormatted(output, sizeof(output), offset,
                    "[POSTFX:V9_SUMMARY] present=%u enabled=%u samples=%u applies=%u warmup=%u lum=%.7g ref=%.7g targetExposure=%.7g adaptedExposure=%.7g readbackHr=%08X drawHr=%08X restoreHr=%08X readbackFormat=%08X readbackSize=%ux%u sceneFormat=%08X sceneSize=%ux%u\r\n",
                    event.a, event.b, event.c, event.extra[7], event.d,
                    BitsFloat(event.e), BitsFloat(event.f), BitsFloat(event.g), BitsFloat(event.h),
                    event.i, event.j, event.extra[0], event.extra[1], event.extra[2], event.extra[3],
                    event.extra[4], event.extra[5], event.extra[6]);
                break;
            case EventType::BloomControlConfig:
                AppendFormatted(output, sizeof(output), offset,
                    "[POSTFX:V10_CONFIG] present=%u mode=%u interval=%u darkBrightness=%.7g lightBrightness=%.7g darkBloom=%.7g lightBloom=%.7g weightsRGB={%.7g,%.7g,%.7g}\r\n",
                    event.a, event.b, event.c, BitsFloat(event.d), BitsFloat(event.e),
                    BitsFloat(event.f), BitsFloat(event.g), BitsFloat(event.h), BitsFloat(event.i), BitsFloat(event.j));
                break;
            case EventType::BloomControlSample:
                AppendFormatted(output, sizeof(output), offset,
                    "[POSTFX:V10_BLOOM] present=%u sample=%u validPixels=%u getRTHr=%08X lockHr=%08X weightedRMS=%.7g brightness255=%.7g t=%.7g bloomStrength=%.7g avgRGBA={%.7g,%.7g,%.7g,%.7g} format=%08X size=%ux%u\r\n",
                    event.a, event.b, event.c, event.d, event.e, BitsFloat(event.f), BitsFloat(event.g),
                    BitsFloat(event.h), BitsFloat(event.i), BitsFloat(event.j), BitsFloat(event.extra[0]),
                    BitsFloat(event.extra[1]), BitsFloat(event.extra[2]), event.extra[3], event.extra[4], event.extra[5]);
                break;
            case EventType::BloomScaleApply:
                AppendFormatted(output, sizeof(output), offset,
                    "[POSTFX:V10572_DEBUG_XBOX_D2_WEIGHTED] order=sceneToQuarterA present=%u count=%u ok=%u bloomStrength=%.7g weight0=%.7g weight1=%.7g setupHr=%08X drawHr=%08X restoreHr=%08X sourceTexScene=%08X sourceSurfScene=%08X targetSurfA=%08X targetTexA=%08X size=%ux%u weightedVS=%08X weightedPS=%08X vsNeg5=%.7g vsPos5=%.7g weight2=%.7g mode=%u\r\n",
                    event.a, event.b, event.c, BitsFloat(event.d), BitsFloat(event.e), BitsFloat(event.f), event.g, event.h, event.i,
                    event.j, event.extra[0], event.extra[1], event.extra[2], event.extra[3], event.extra[4],
                    event.extra[5], event.extra[6], BitsFloat(event.extra[7]), BitsFloat(event.extra[8]),
                    BitsFloat(event.extra[9]), event.extra[10]);
                break;
            case EventType::BloomControlSummary:
                AppendFormatted(output, sizeof(output), offset,
                    "[POSTFX:V10_SUMMARY] present=%u mode=%u samples=%u scalePass=%u scaleFail=%u finalSubstitute=%u weightedRMS=%.7g brightness255=%.7g t=%.7g bloomStrength=%.7g sampleValid=%u setupHr=%08X drawHr=%08X restoreHr=%08X texA=%08X texB=%08X surfA=%08X surfB=%08X\r\n",
                    event.a, event.b, event.c, event.d, event.e, event.f, BitsFloat(event.g), BitsFloat(event.h),
                    BitsFloat(event.i), BitsFloat(event.j), event.extra[0], event.extra[1], event.extra[2], event.extra[3],
                    event.extra[4], event.extra[5], event.extra[6], event.extra[7]);
                break;
            case EventType::CalibrationSample:
                AppendFormatted(output, sizeof(output), offset,
                    "[POSTFX:V10_2_CAL] present=%u sample=%u validPixels=%u over1=%u rawRMS=%.7g clampedRMS=%.7g rawMax=%.7g p10_255=%.7g p50_255=%.7g p90_255=%.7g brightness255=%.7g bloomStrength=%.7g lum=%.7g ref=%.7g targetExposure=%.7g adaptedExposure=%.7g avgRGB={%.7g,%.7g,%.7g} mode=%u\r\n",
                    event.a, event.b, event.c, event.d, BitsFloat(event.e), BitsFloat(event.f), BitsFloat(event.g),
                    BitsFloat(event.h), BitsFloat(event.i), BitsFloat(event.j), BitsFloat(event.extra[0]),
                    BitsFloat(event.extra[1]), BitsFloat(event.extra[2]), BitsFloat(event.extra[3]),
                    BitsFloat(event.extra[4]), BitsFloat(event.extra[5]), BitsFloat(event.extra[6]),
                    BitsFloat(event.extra[7]), BitsFloat(event.extra[8]), event.extra[9]);
                break;
            case EventType::XboxExposureStateConfig:
                AppendFormatted(output, sizeof(output), offset,
                    "[POSTFX:V10_3_XBOX_EXPOSURE_CONFIG] present=%u xboxAutoDefault=%u pcTimeGlobal=%08X initial=%.7g min=%.7g max=%.7g targetLow=%.7g targetHigh=%.7g speed=%.7g manual=%.7g fallbackTarget=%.7g fallbackKeep=%.7g fallbackBlend=%.7g deltaScale=%.7g timeOffsets={%u,%u} luminanceInterval=%u nativeMode3Available=1\r\n",
                    event.a, event.b, event.c, BitsFloat(event.d), BitsFloat(event.e), BitsFloat(event.f),
                    BitsFloat(event.g), BitsFloat(event.h), BitsFloat(event.i), BitsFloat(event.j),
                    BitsFloat(event.extra[0]), BitsFloat(event.extra[1]), BitsFloat(event.extra[2]),
                    BitsFloat(event.extra[3]), event.extra[4], event.extra[5], event.extra[6]);
                break;
            case EventType::XboxExposureStateSample:
                AppendFormatted(output, sizeof(output), offset,
                    "[POSTFX:V10_3_XBOX_EXPOSURE] present=%u update=%u timeValid=%u sampleAge=%u gameSeconds=%.7g dayFraction=%.7g cosPhase=%.7g sceneMeanSq=%.7g targetMeanSq=%.7g delta=%.9g state=%.7g shaderExposure=%.7g brightness255=%.7g bloomStrength=%.7g v9Lum=%.7g v9Target=%.7g v9Adapted=%.7g mode=%u world=%08X nativeApplyLoggedSeparately=1\r\n",
                    event.a, event.b, event.c, event.d, BitsFloat(event.e), BitsFloat(event.f),
                    BitsFloat(event.g), BitsFloat(event.h), BitsFloat(event.i), BitsFloat(event.j),
                    BitsFloat(event.extra[0]), BitsFloat(event.extra[1]), BitsFloat(event.extra[2]),
                    BitsFloat(event.extra[3]), BitsFloat(event.extra[4]), BitsFloat(event.extra[5]),
                    BitsFloat(event.extra[6]), event.extra[7], event.extra[8]);
                break;
            case EventType::NativeExposureApply:
                AppendFormatted(output, sizeof(output), offset,
                    "[POSTFX:V10_4_NATIVE_EXPOSURE] present=%u match=%u applied=%u mode=%u return=%08X param=%08X original=%.7g injected=%.7g xboxState=%.7g autoExp=%u sampleValid=%u stateUpdates=%u sampleAge=%u v9Lum=%.7g v9Target=%.7g v9Adapted=%.7g v9VisualEnabled=%u readyMask=%02X result=%08X appliedCount=%u bypassCount=%u\r\n",
                    event.a, event.b, event.c, event.d, event.e, event.f,
                    BitsFloat(event.g), BitsFloat(event.h), BitsFloat(event.i), event.j,
                    event.extra[0], event.extra[1], event.extra[2], BitsFloat(event.extra[3]),
                    BitsFloat(event.extra[4]), BitsFloat(event.extra[5]), event.extra[6],
                    event.extra[7], event.extra[8], event.extra[9], event.extra[10]);
                break;
            case EventType::NativeExposureSummary:
                AppendFormatted(output, sizeof(output), offset,
                    "[POSTFX:V10_4_SUMMARY] present=%u mode=%u matched=%u applied=%u bypass=%u stateUpdates=%u lastApplied=%u autoExp=%u sampleValid=%u v9VisualEnabled=%u lastOriginal=%.7g lastInjected=%.7g xboxState=%.7g lastReturn=%08X lastParam=%08X v9Target=%.7g v9Adapted=%.7g sampleAge=%u\r\n",
                    event.a, event.b, event.c, event.d, event.e, event.f, event.g, event.h, event.i, event.j,
                    BitsFloat(event.extra[0]), BitsFloat(event.extra[1]), BitsFloat(event.extra[2]),
                    event.extra[3], event.extra[4], BitsFloat(event.extra[5]), BitsFloat(event.extra[6]), event.extra[7]);
                break;
            case EventType::NativeExposurePatchStatus:
                AppendFormatted(output, sizeof(output), offset,
                    "[POSTFX:V10_4_1_CALLSITE] status=%d callsite=%08X stockTarget=%08X hook=%08X original={%02X %02X %02X %02X %02X} current={%02X %02X %02X %02X %02X}\r\n",
                    static_cast<int32_t>(event.a), event.b, event.c, event.d,
                    event.e & 0xFFu, event.f & 0xFFu, event.g & 0xFFu, event.h & 0xFFu, event.i & 0xFFu,
                    event.j & 0xFFu, event.extra[0] & 0xFFu, event.extra[1] & 0xFFu, event.extra[2] & 0xFFu, event.extra[3] & 0xFFu);
                break;
            case EventType::BloomCompositeCallsitePatchStatus:
                AppendFormatted(output, sizeof(output), offset,
                    "[POSTFX:PROVEN_LATE_CALLSITE] status=%d callsite=%08X stockTarget=%08X return=%08X hook=%08X original={%02X %02X %02X %02X %02X} current={%02X %02X %02X %02X %02X} hits=%u lastReturn=%08X\r\n",
                    static_cast<int32_t>(event.a), event.b, event.c, event.d, event.e,
                    event.f & 0xFFu, event.g & 0xFFu, event.h & 0xFFu, event.i & 0xFFu, event.j & 0xFFu,
                    event.extra[0] & 0xFFu, event.extra[1] & 0xFFu, event.extra[2] & 0xFFu, event.extra[3] & 0xFFu, event.extra[4] & 0xFFu,
                    event.extra[5], event.extra[6]);
                break;
            case EventType::DofFinalState:
                AppendFormatted(output, sizeof(output), offset,
                    "[POSTFX:DOF_FINAL] present=%u composite=%u attempt=%u mode=%u probeMask=%04X ready=%u colorDesc=%08X colorTex=%08X depthTex=%08X dofPS=%08X simplePS=%08X activeBefore=%08X activeAfter=%08X c0Hr=%08X psHr=%08X c0={%.7g,%.7g,%.7g,%.7g} sceneRT=%08X applyCount=%u fallbackCount=%u depthBindHr=%08X depthCacheBefore=%08X depthCacheAfter=%08X rawStage1After=%08X\r\n",
                    event.a, event.b, event.c, event.d, event.e, event.f, event.g, event.h, event.i, event.j,
                    event.extra[0], event.extra[1], event.extra[2], event.extra[3], event.extra[4],
                    BitsFloat(event.extra[5]), BitsFloat(event.extra[6]), BitsFloat(event.extra[7]), BitsFloat(event.extra[8]),
                    event.extra[9], event.extra[10], event.extra[11], event.extra[12], event.extra[13], event.extra[14], event.extra[15]);
                break;
            case EventType::DofFinalSummary:
                AppendFormatted(output, sizeof(output), offset,
                    "[POSTFX:DOF_FINAL_SUMMARY] present=%u mode=%u attempts=%u applied=%u fallback=%u probeMask=%04X lastReady=%u colorDesc=%08X colorTex=%08X depthTex=%08X dofPS=%08X simplePS=%08X activeBefore=%08X activeAfter=%08X c0Hr=%08X psHr=%08X c0={%.7g,%.7g,%.7g,%.7g} liveDofPS=%08X liveSimplePS=%08X depthBindHr=%08X depthCacheBefore=%08X depthCacheAfter=%08X rawStage1After=%08X\r\n",
                    event.a, event.b, event.c, event.d, event.e, event.f, event.g, event.h, event.i, event.j,
                    event.extra[0], event.extra[1], event.extra[2], event.extra[3], event.extra[4], event.extra[5],
                    BitsFloat(event.extra[6]), BitsFloat(event.extra[7]), BitsFloat(event.extra[8]), BitsFloat(event.extra[9]),
                    event.extra[10], event.extra[11], event.extra[12], event.extra[13], event.extra[14], event.extra[15]);
                break;
            case EventType::DofDepthProbe:
                AppendFormatted(output, sizeof(output), offset,
                    "[POSTFX:MODE4_DEPTH_PROBE] present=%u attempt=%u dsSurface=%08X getDSHr=%08X descHr=%08X containerHr=%08X containerTex=%08X format=%08X usage=%08X msType=%u msQuality=%u size=%ux%u descriptorWords={%08X,%08X,%08X,%08X,%08X,%08X,%08X,%08X} godCacheBefore=%08X godCacheAfter=%08X godRawStage1=%08X capturedSurface=%08X capturedTex=%08X\r\n",
                    event.a, event.b, event.c, event.d, event.e, event.f, event.g, event.h, event.i, event.j,
                    event.extra[0], event.extra[1], event.extra[2], event.extra[3], event.extra[4], event.extra[5],
                    event.extra[6], event.extra[7], event.extra[8], event.extra[9], event.extra[10],
                    event.extra[11], event.extra[12], event.extra[13], event.extra[14], event.extra[15]);
                break;
            case EventType::GodRayG1SelectorTransition:
                AppendFormatted(output, sizeof(output), offset,
                    "[POSTFX:V10572_GODRAY_G1_SELECTOR] present=%u transition=%u old=%08X new=%08X wouldBranch=%u stockFinalSeen=%u contractPass=%u contractFail=%u lastVsHash=%08X lastPsHash=%08X godRayApplied=0\r\n",
                    event.a,event.b,event.c,event.d,event.e,event.f,event.g,event.h,event.i,event.j);
                break;
            case EventType::GodRayG1FinalContract:
                AppendFormatted(output, sizeof(output), offset,
                    "[POSTFX:V10572_GODRAY_G1_FINAL] present=%u seen=%u selector=%08X return=%08X pass=%u activeVS=%08X activePS=%08X vsHash=%08X psHash=%08X s0=%08X s1=%08X s2=%08X expectedVS=%08X expectedPS=%08X expectedS0=%08X expectedS1=%08X expectedS2=%08X rt0=%08X ds=%08X sceneSurface=%08X sourceSelectBits=%08X vsBytes=%u psBytes=%u vsHr=%08X psHr=%08X t0Hr=%08X t1Hr=%08X t2Hr=%08X psConstHr=%08X vsConstHr=%08X rtHr=%08X dsHr=%08X vpHr=%08X captures=%u fails=%u primitive=%u start=%u count=%u xeSM3GodRayDraws=0\r\n",
                    event.a,event.b,event.c,event.d,event.e,event.f,event.g,event.h,event.i,event.j,
                    event.extra[0],event.extra[1],event.extra[2],event.extra[3],event.extra[4],event.extra[5],event.extra[6],
                    event.extra[7],event.extra[8],event.extra[9],event.extra[10],event.extra[11],event.extra[12],event.extra[13],
                    event.extra[14],event.extra[15],event.extra[16],event.extra[17],event.extra[18],event.extra[19],event.extra[20],
                    event.extra[21],event.extra[22],event.extra[23],event.extra[24],event.extra[25],event.extra[26],event.extra[27]);
                break;
            case EventType::GodRayG1FinalConstants:
                AppendFormatted(output, sizeof(output), offset,
                    "[POSTFX:V10572_GODRAY_G1_CONSTANTS] present=%u seen=%u psC0={%.7g,%.7g,%.7g,%.7g} psC1={%.7g,%.7g,%.7g,%.7g} psC2={%.7g,%.7g,%.7g,%.7g} psC3={%.7g,%.7g,%.7g,%.7g} psC4={%.7g,%.7g,%.7g,%.7g} psC5={%.7g,%.7g,%.7g,%.7g} psC6={%.7g,%.7g,%.7g,%.7g} vsC0={%.7g,%.7g,%.7g,%.7g} vsC1={%.7g,%.7g,%.7g,%.7g}\r\n",
                    event.a,event.b,
                    BitsFloat(event.c),BitsFloat(event.d),BitsFloat(event.e),BitsFloat(event.f),
                    BitsFloat(event.extra[0]),BitsFloat(event.extra[1]),BitsFloat(event.extra[2]),BitsFloat(event.extra[3]),
                    BitsFloat(event.extra[4]),BitsFloat(event.extra[5]),BitsFloat(event.extra[6]),BitsFloat(event.extra[7]),
                    BitsFloat(event.extra[8]),BitsFloat(event.extra[9]),BitsFloat(event.extra[10]),BitsFloat(event.extra[11]),
                    BitsFloat(event.extra[12]),BitsFloat(event.extra[13]),BitsFloat(event.extra[14]),BitsFloat(event.extra[15]),
                    BitsFloat(event.extra[16]),BitsFloat(event.extra[17]),BitsFloat(event.extra[18]),BitsFloat(event.extra[19]),
                    BitsFloat(event.extra[20]),BitsFloat(event.extra[21]),BitsFloat(event.extra[22]),BitsFloat(event.extra[23]),
                    BitsFloat(event.g),BitsFloat(event.h),BitsFloat(event.i),BitsFloat(event.j),
                    BitsFloat(event.extra[24]),BitsFloat(event.extra[25]),BitsFloat(event.extra[26]),BitsFloat(event.extra[27]));
                break;
            case EventType::GodRayG1VertexConstants:
                AppendFormatted(output, sizeof(output), offset,
                    "[POSTFX:V10572_GODRAY_G1_VS_CONSTANTS] present=%u seen=%u vsConstHr=%08X activeVS=%08X vsHash=%08X vsC0={%.7g,%.7g,%.7g,%.7g} vsC1={%.7g,%.7g,%.7g,%.7g} vsC2={%.7g,%.7g,%.7g,%.7g} vsC3={%.7g,%.7g,%.7g,%.7g}\r\n",
                    event.a,event.b,event.c,event.d,event.e,
                    BitsFloat(event.extra[0]),BitsFloat(event.extra[1]),BitsFloat(event.extra[2]),BitsFloat(event.extra[3]),
                    BitsFloat(event.extra[4]),BitsFloat(event.extra[5]),BitsFloat(event.extra[6]),BitsFloat(event.extra[7]),
                    BitsFloat(event.extra[8]),BitsFloat(event.extra[9]),BitsFloat(event.extra[10]),BitsFloat(event.extra[11]),
                    BitsFloat(event.extra[12]),BitsFloat(event.extra[13]),BitsFloat(event.extra[14]),BitsFloat(event.extra[15]));
                break;
            case EventType::GodRayG1FinalStates:
                AppendFormatted(output, sizeof(output), offset,
                    "[POSTFX:V10572_GODRAY_G1_STATES] present=%u seen=%u viewport={%u,%u,%u,%u,%.7g,%.7g} srcBlend=%u dstBlend=%u alphaBlend=%u colorWrite=%08X zEnable=%u zWrite=%u s0={addrU=%u addrV=%u min=%u mag=%u} s1={addrU=%u addrV=%u min=%u mag=%u} s2={addrU=%u addrV=%u min=%u mag=%u} renderHr={%08X,%08X,%08X,%08X,%08X,%08X} samplerHrOr={%08X,%08X,%08X} vsC2xyz={%.7g,%.7g,%.7g}\r\n",
                    event.a,event.b,event.c,event.d,event.e,event.f,BitsFloat(event.g),BitsFloat(event.h),event.i,event.j,
                    event.extra[0],event.extra[1],event.extra[2],event.extra[3],
                    event.extra[4],event.extra[5],event.extra[6],event.extra[7],
                    event.extra[8],event.extra[9],event.extra[10],event.extra[11],
                    event.extra[12],event.extra[13],event.extra[14],event.extra[15],
                    event.extra[16],event.extra[17],event.extra[18],event.extra[19],event.extra[20],event.extra[21],
                    event.extra[22],event.extra[23],event.extra[24],BitsFloat(event.extra[25]),BitsFloat(event.extra[26]),BitsFloat(event.extra[27]));
                break;
            case EventType::GodRayG2BindStateProbe:
                AppendFormatted(output, sizeof(output), offset,
                    "[POSTFX:V10572_GODRAY_G2_BIND] present=%u attempt=%u pass=%u selector=%08X expectedVS=%08X expectedPS=%08X expectedS0=%08X expectedS1=%08X expectedS2=%08X sceneSurface=%08X beforeVS=%08X beforePS=%08X boundVS=%08X boundPS=%08X restoredVS=%08X restoredPS=%08X beforeS={%08X,%08X,%08X} boundS={%08X,%08X,%08X} restoredS={%08X,%08X,%08X} vsHash=%08X psHash=%08X setHrOr=%08X boundHrOr=%08X restoreHrOr=%08X restoreGetHrOr=%08X shaderVerified=%u textureVerified=%u constantsVerified=%u stateVerified=%u rtDsVpStable=%u restoreVerified=%u g2InternalDraws=%u g2PassCount=%u g2FailCount=%u\r\n",
                    event.a,event.b,event.c,event.d,event.e,event.f,event.g,event.h,event.i,event.j,
                    event.extra[0],event.extra[1],event.extra[2],event.extra[3],event.extra[4],event.extra[5],
                    event.extra[6],event.extra[7],event.extra[8],event.extra[9],event.extra[10],event.extra[11],
                    event.extra[12],event.extra[13],event.extra[14],event.extra[15],event.extra[16],event.extra[17],
                    event.extra[18],event.extra[19],event.extra[20],event.extra[21],event.extra[22],event.extra[23],
                    event.extra[24],event.extra[25],event.extra[26],event.extra[27],
                    static_cast<uint32_t>(s_godRayG2BindPassCount),static_cast<uint32_t>(s_godRayG2BindFailCount));
                break;
            case EventType::GodRayG3FirstRealDraw:
                AppendFormatted(output, sizeof(output), offset,
                    "[POSTFX:V10572_GODRAY_G3_DRAW] present=%u attempt=%u pass=%u selector=%08X drawHr=%08X preDrawVerified=%u drawHeldShader=%u drawHeldTextures=%u drawHeldConstants=%u drawHeldState=%u drawHeldRtDsVp=%u restoreVerified=%u xeSM3GodRayDraws=%u g3PassCount=%u g3FailCount=%u g3Disabled=%u vsHash=%08X psHash=%08X s0=%08X s1=%08X s2=%08X sceneSurface=%08X heldRT=%08X heldDS=%08X viewport=%ux%u heldHrOr=%08X heldDsHr=%08X restoreHrOr=%08X restoreGetHrOr=%08X beforeContract=%u g2ShaderVerified=%u g2TextureVerified=%u g2ConstantsVerified=%u g2StateVerified=%u g2RtDsVpStable=%u primitive=%u count=%u stockDrawContinues=G4_DECIDES\r\n",
                    event.a,event.b,event.c,event.d,event.e,event.f,event.g,event.h,event.i,event.j,
                    event.extra[0],event.extra[1],event.extra[2],event.extra[3],event.extra[4],event.extra[5],
                    event.extra[6],event.extra[7],event.extra[8],event.extra[9],event.extra[10],event.extra[11],
                    event.extra[12],event.extra[13],event.extra[14],event.extra[15],event.extra[16],event.extra[17],
                    event.extra[18],event.extra[19],event.extra[20],event.extra[21],event.extra[22],event.extra[23],
                    event.extra[24],event.extra[25],event.extra[26],event.extra[27]);
                break;
            case EventType::GodRayG4OwnedDrawSuppress:
                AppendFormatted(output, sizeof(output), offset,
                    "[POSTFX:V10572_GODRAY_G4_OWNED] present=%u attempt=%u pass=%u selector=%08X g2Handled=%u g3PassThisCall=%u xeSM3GodRayDraws=%u g3PassCount=%u g3FailCount=%u g3Disabled=%u stockSuppressCount=%u stockFallbackCount=%u returnedHr=%08X return=%08X primitive=%u start=%u count=%u stockDrawSuppressed=%u\r\n",
                    event.a,event.b,event.c,event.d,event.e,event.f,event.g,event.h,event.i,event.j,
                    event.extra[0],event.extra[1],event.extra[2],event.extra[3],event.extra[4],event.extra[5],event.extra[6],event.c);
                break;
            case EventType::GodRaysDepthBindProbe:
                AppendFormatted(output, sizeof(output), offset,
                    "[POSTFX:GODRAYS_DEPTH_BIND] present=%u count=%u return=%08X descriptor=%08X cacheBefore=%08X cacheAfter=%08X rawStage1=%08X descriptorWords={%08X,%08X,%08X,%08X,%08X,%08X,%08X,%08X}\r\n",
                    event.a, event.b, event.c, event.d, event.e, event.f, event.g,
                    event.extra[0], event.extra[1], event.extra[2], event.extra[3],
                    event.extra[4], event.extra[5], event.extra[6], event.extra[7]);
                break;
            }
        }

        if (offset)
        {
            DWORD written = 0;
            WriteFile(s_logFile, output, static_cast<DWORD>(offset), &written, nullptr);
        }
#endif
    }

    bool InstallBloomCompositeLateCallsitePatch()
    {
        if (InterlockedCompareExchange(&s_bloomCompositeLateCallsitePatchState, 2, 0) != 0)
            return s_bloomCompositeLateCallsitePatchState == 1;

        uint8_t* const site =
            reinterpret_cast<uint8_t*>(kBloomCompositeLateCallsiteAddress);
        uint8_t before[5] = {};
        if (!IsReadableMemory(site, sizeof(before)))
        {
            InterlockedExchange(&s_bloomCompositeLateCallsitePatchState, -1);
        }
        else
        {
            memcpy(before, site, sizeof(before));
            memcpy(s_bloomCompositeLateCallsiteOriginalBytes, before, sizeof(before));

            // Fail closed unless the retail executable still contains the exact
            // runtime-proven CALL 0079618A -> 00797A70 (E8 E1 18 00 00).
            if (memcmp(before, kBloomCompositeLateCallsiteOriginalCall, sizeof(before)) != 0)
            {
                InterlockedExchange(&s_bloomCompositeLateCallsitePatchState, -1);
            }
            else
            {
                DWORD oldProtect = 0;
                if (VirtualProtect(site, sizeof(before), PAGE_EXECUTE_READWRITE, &oldProtect))
                {
                    const uintptr_t next = kBloomCompositeLateCallsiteReturn;
                    const intptr_t delta =
                        reinterpret_cast<intptr_t>(&BloomComposite_Hook) -
                        static_cast<intptr_t>(next);
                    const int32_t rel32 = static_cast<int32_t>(delta);
                    site[0] = 0xE8;
                    memcpy(site + 1, &rel32, sizeof(rel32));
                    FlushInstructionCache(GetCurrentProcess(), site, sizeof(before));
                    DWORD ignored = 0;
                    VirtualProtect(site, sizeof(before), oldProtect, &ignored);
                    InterlockedExchange(&s_bloomCompositeLateCallsitePatchState, 1);
                }
                else
                {
                    InterlockedExchange(&s_bloomCompositeLateCallsitePatchState, -1);
                }
            }
        }

        Event* event = ReserveEvent(EventType::BloomCompositeCallsitePatchStatus);
        if (event)
        {
            uint8_t current[5] = {};
            if (IsReadableMemory(site, sizeof(current)))
                memcpy(current, site, sizeof(current));
            event->a = static_cast<uint32_t>(s_bloomCompositeLateCallsitePatchState);
            event->b = static_cast<uint32_t>(kBloomCompositeLateCallsiteAddress);
            event->c = static_cast<uint32_t>(kBloomBlurCompositeAddress);
            event->d = static_cast<uint32_t>(kBloomCompositeLateCallsiteReturn);
            event->e = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&BloomComposite_Hook));
            event->f = s_bloomCompositeLateCallsiteOriginalBytes[0];
            event->g = s_bloomCompositeLateCallsiteOriginalBytes[1];
            event->h = s_bloomCompositeLateCallsiteOriginalBytes[2];
            event->i = s_bloomCompositeLateCallsiteOriginalBytes[3];
            event->j = s_bloomCompositeLateCallsiteOriginalBytes[4];
            event->extra[0] = current[0];
            event->extra[1] = current[1];
            event->extra[2] = current[2];
            event->extra[3] = current[3];
            event->extra[4] = current[4];
            event->extra[5] = static_cast<uint32_t>(s_bloomCompositeLateCallsiteHits);
            event->extra[6] = s_bloomCompositeLateCallsiteLastReturn;
        }
        return s_bloomCompositeLateCallsitePatchState == 1;
    }

    bool InstallExposureCallsitePatch()
    {
        if (InterlockedCompareExchange(&s_v10_4CallsitePatchState, 2, 0) != 0)
            return s_v10_4CallsitePatchState == 1;

        uint8_t* const site = reinterpret_cast<uint8_t*>(kExposureScalarCallAddress);
        uint8_t before[5] = {};
        if (!IsReadableMemory(site, sizeof(before)))
        {
            InterlockedExchange(&s_v10_4CallsitePatchState, -1);
        }
        else
        {
            memcpy(before, site, sizeof(before));
            memcpy(s_v10_4OriginalCallBytes, before, sizeof(before));

            // Fail closed if another hook/build changed the proven retail call.
            if (memcmp(before, kExposureScalarOriginalCall, sizeof(before)) != 0)
            {
                InterlockedExchange(&s_v10_4CallsitePatchState, -1);
            }
            else
            {
                DWORD oldProtect = 0;
                if (VirtualProtect(site, sizeof(before), PAGE_EXECUTE_READWRITE, &oldProtect))
                {
                    const uintptr_t next = kExposureScalarCallAddress + 5u;
                    const intptr_t delta =
                        reinterpret_cast<intptr_t>(&ExposureScalarCallsite_Hook) - static_cast<intptr_t>(next);
                    const int32_t rel32 = static_cast<int32_t>(delta);
                    site[0] = 0xE8;
                    memcpy(site + 1, &rel32, sizeof(rel32));
                    FlushInstructionCache(GetCurrentProcess(), site, sizeof(before));
                    DWORD ignored = 0;
                    VirtualProtect(site, sizeof(before), oldProtect, &ignored);
                    InterlockedExchange(&s_v10_4CallsitePatchState, 1);
                }
                else
                {
                    InterlockedExchange(&s_v10_4CallsitePatchState, -1);
                }
            }
        }

        Event* event = ReserveEvent(EventType::NativeExposurePatchStatus);
        if (event)
        {
            uint8_t current[5] = {};
            if (IsReadableMemory(site, sizeof(current)))
                memcpy(current, site, sizeof(current));
            event->a = static_cast<uint32_t>(s_v10_4CallsitePatchState);
            event->b = static_cast<uint32_t>(kExposureScalarCallAddress);
            event->c = static_cast<uint32_t>(kShaderScalarSetterAddress);
            event->d = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&ExposureScalarCallsite_Hook));
            event->e = s_v10_4OriginalCallBytes[0];
            event->f = s_v10_4OriginalCallBytes[1];
            event->g = s_v10_4OriginalCallBytes[2];
            event->h = s_v10_4OriginalCallBytes[3];
            event->i = s_v10_4OriginalCallBytes[4];
            event->j = current[0];
            event->extra[0] = current[1];
            event->extra[1] = current[2];
            event->extra[2] = current[3];
            event->extra[3] = current[4];
        }
        return s_v10_4CallsitePatchState == 1;
    }

    void QueueDebugXboxD2Probe(bool sampleAttempted, bool samplePassed)
    {
        if (s_postProcessRoute != 3u)
            return;

        const LONG probe = InterlockedIncrement(&s_debugXboxD2ProbeCount);
        const uint32_t sampleAge = (s_v10BloomSampleValid && s_v10_3LastLuminanceSamplePresent <= s_presentCount)
            ? (s_presentCount - s_v10_3LastLuminanceSamplePresent) : 0xFFFFFFFFu;
        const float brightness = s_v10BloomSampleValid ? s_v10Brightness255 : 0.0f;
        const float range = kDebugXboxLightBrightness - kDebugXboxDarkBrightness;
        const float t = range > 0.000001f
            ? ClampFloat((brightness - kDebugXboxDarkBrightness) / range, 0.0f, 1.0f) : 0.0f;
        const float debugBloom = kDebugXboxDarkBloom +
            (kDebugXboxLightBloom - kDebugXboxDarkBloom) * t;
        const float retailBloom = kRetailXboxDarkBloomCompare +
            (kRetailXboxLightBloomCompare - kRetailXboxDarkBloomCompare) * t;

        const bool selectorReadable = IsReadableMemory(
            reinterpret_cast<void*>(kPcDormantGodRaySelector), sizeof(uint32_t));
        const uint32_t selectorRaw = selectorReadable ? ReadU32(kPcDormantGodRaySelector) : 0u;
        const bool wouldGodRay = selectorReadable && selectorRaw != 0u;

        if (selectorReadable)
        {
            if (!s_godRayG1SelectorInitialized)
            {
                s_godRayG1SelectorInitialized = true;
                s_godRayG1LastSelectorRaw = selectorRaw;
            }
            else if (selectorRaw != s_godRayG1LastSelectorRaw)
            {
                const uint32_t previousRaw = s_godRayG1LastSelectorRaw;
                s_godRayG1LastSelectorRaw = selectorRaw;
                s_godRayG1LastTransitionPresent = s_presentCount;
                const LONG transition = InterlockedIncrement(&s_godRayG1SelectorTransitionCount);
                Event* transitionEvent = ReserveEvent(EventType::GodRayG1SelectorTransition);
                if (transitionEvent)
                {
                    transitionEvent->a = s_presentCount;
                    transitionEvent->b = static_cast<uint32_t>(transition);
                    transitionEvent->c = previousRaw;
                    transitionEvent->d = selectorRaw;
                    transitionEvent->e = selectorRaw != 0u ? 1u : 0u;
                    transitionEvent->f = static_cast<uint32_t>(s_godRayG1FinalSeenCount);
                    transitionEvent->g = static_cast<uint32_t>(s_godRayG1ContractCaptureCount);
                    transitionEvent->h = static_cast<uint32_t>(s_godRayG1ContractFailCount);
                    transitionEvent->i = s_godRayG1LastVsHash;
                    transitionEvent->j = s_godRayG1LastPsHash;
                }
            }
        }

        const bool periodic = s_presentCount == 1u ||
            (s_presentCount % kRetailXboxHeartbeatPresents) == 0u;
        if (kDebugXboxIntegrationLockdownQuietLogging)
        {
            const bool failedAttempt = sampleAttempted && !samplePassed;
            if (!periodic && !failedAttempt)
                return;
        }
        else if (!samplePassed && !periodic && probe > 16)
        {
            return;
        }

        Event* event = ReserveEvent(EventType::DebugXboxD2Probe);
        if (!event)
            return;
        event->a = s_presentCount;
        event->b = static_cast<uint32_t>(probe);
        event->c = sampleAttempted ? 1u : 0u;
        event->d = samplePassed ? 1u : 0u;
        event->e = s_v10BloomSampleValid ? 1u : 0u;
        event->f = sampleAge;
        event->g = FloatBits(brightness);
        event->h = FloatBits(t);
        event->i = FloatBits(debugBloom);
        event->j = FloatBits(retailBloom);
        event->extra[0] = FloatBits(kDebugXboxDarkBrightness);
        event->extra[1] = FloatBits(kDebugXboxLightBrightness);
        event->extra[2] = FloatBits(kDebugXboxDarkBloom);
        event->extra[3] = FloatBits(kDebugXboxLightBloom);
        event->extra[4] = FloatBits(kRetailXboxDarkBloomCompare);
        event->extra[5] = FloatBits(kRetailXboxLightBloomCompare);
        event->extra[6] = selectorReadable ? 1u : 0u;
        event->extra[7] = selectorRaw;
        event->extra[8] = wouldGodRay ? 1u : 0u;
        event->extra[9] = 0u; // Release-2/May-30 Debug selector data default.
                const uint32_t applyAge = (s_debugXboxD2LastApplyPresent <= s_presentCount)
            ? (s_presentCount - s_debugXboxD2LastApplyPresent) : 0xFFFFFFFFu;
        event->extra[10] = (s_debugXboxD2BloomApplyPassCount > 0 && applyAge <= 1u) ? 1u : 0u;
        // debugBloomApplied: set only when the preceding rendered frame completed
        // the guarded weighted Scene->QuarterA pass.
        event->extra[11] = 0u; // godRayApplied: deliberately OFF in D2.
        event->extra[12] = static_cast<uint32_t>(s_actualFinalF18PassCount);
        event->extra[13] = static_cast<uint32_t>(s_retailXboxImageZoomDrawPassCount);
        event->extra[14] = s_postProcessRoute;
        event->extra[15] = s_rawSurfaceLum16;
        event->extra[16] = static_cast<uint32_t>(s_v9LastReadbackHr);
        event->extra[17] = static_cast<uint32_t>(s_v9LastLockHr);
        event->extra[18] = s_v10BloomSampleCount;
        event->extra[19] = s_v10_3LastLuminanceSamplePresent;
        event->extra[20] = s_retailXboxDofShaderHash;
        event->extra[21] = s_retailXboxZBlurVsHash;
        event->extra[22] = s_retailXboxZBlurPsHash;
        event->extra[23] = static_cast<uint32_t>(s_routeMismatchCount);
        event->extra[24] = static_cast<uint32_t>(s_actualFinalF18Disabled);
        event->extra[25] = static_cast<uint32_t>(s_retailXboxImageZoomDrawDisabled);
        event->extra[26] = s_mode4ReszDepthValidated ? 1u : 0u;
        event->extra[27] = static_cast<uint32_t>(s_debugXboxBloomReadbackAttemptCount);
    }

}

void AttachPostFXResearchDetours()
{
    ReadV10ConfigOnce();

    // V10.5.66 Debug D2 + GodRay G1 PostFX route:
    // Retail PC (all three keys 0) installs no PostFX detours at all.
    // Any enabled PostFX route installs only the proven shared native repair.
    if (!s_postProcessNativeRepairActive)
        return;

    DetourAttach(&reinterpret_cast<PVOID&>(s_originalCreateRT), CreateRT_Hook);
    DetourAttach(&reinterpret_cast<PVOID&>(s_originalBloomComposite), BloomComposite_Hook);
    DetourAttach(&reinterpret_cast<PVOID&>(s_originalBindTexture), BindTexture_Hook);
    DetourAttach(&reinterpret_cast<PVOID&>(s_originalFullscreenQuad), FullscreenQuad_Hook);
    if (IsXboxPostProcessRoute())
        DetourAttach(&reinterpret_cast<PVOID&>(s_originalNGLRenderScene), NGLRenderScene_Hook);
}

bool InstallPostFXResearchCallsitePatches()
{
    ReadV10ConfigOnce();

    // V10.5.68 installs no direct game-code patches. The native repair is
    // implemented entirely through the four narrow Detours above.
    return true;
}

void PostFXResearch_OnPresent()
{
    ++s_presentCount;
    InterlockedExchange(&s_rawGenericEventCountThisPresent, 0);
    InterlockedExchange(&s_rawPostFxEventCountThisPresent, 0);

    ReadV10ConfigOnce();
    if (!s_postProcessNativeRepairActive)
        return;

    // Mode 5 avoids the broad raw-D3D research-detour bundle. V10.5.72 keeps two
    // deliberately narrow production exceptions: Reset slot 16 for Alt-Tab/device
    // lifecycle hygiene and DrawPrimitive slot 81 for the proven final-draw path.
    // Present/RT/texture/shader/constant/depth research detours remain unreachable.
    if (s_v10Mode == 5)
    {
        // Attach only IDirect3DDevice9::Reset (slot 16) + DrawPrimitive (slot 81)
        // before the Mode-5 early return. The broad raw-D3D bundle remains below
        // this return and therefore stays unreachable.
        TryAttachMode5DedicatedDeviceDetours();

        // If Reset is currently in flight or the last Reset failed, leave all
        // xeSM3-owned PostFX resources released and wait for a successful Reset.
        if (InterlockedCompareExchange(&s_postFxDeviceReady, 0, 0) == 0)
            return;

        // V10.5.65 Debug D2: read the existing 16x12 luminance target with no broad
        // D3D detours. D2 feeds the resulting 0..1.5 strength into the guarded
        // weighted bloom pass during the subsequent native composite.
        bool debugSampleAttempted = false;
        bool debugSamplePassed = false;

        // V10.5.65 Debug D2 luminance identity path: Mode 5 returns before the legacy raw-identity
        // refresh block below, so the existing 16x12 luminance wrapper never became
        // s_rawSurfaceLum16/s_rawTextureLum16 in .64.  Resolve ONLY that already-created
        // luminance target on the same sparse six-present cadence used by the readback.
        // This is identity discovery only: no RT bind, shader bind, draw, or renderer write.
        if (s_postProcessRoute == 3u && s_presentCount != 0u &&
            (s_presentCount % kV10ReadbackInterval) == 0u &&
            (!s_rawTextureLum16 || !s_rawSurfaceLum16))
        {
            RefreshOneRawIdentity(ReadU32(kRtLuminance16x12), 4u);
        }

        if (s_postProcessRoute == 3u && s_presentCount != 0u &&
            (s_presentCount % kV10ReadbackInterval) == 0u &&
            ResolveRetailXboxDepthApisNoDetours() && s_rawSurfaceLum16 >= 0x10000u)
        {
            debugSampleAttempted = true;
            InterlockedIncrement(&s_debugXboxBloomReadbackAttemptCount);
            void* device = reinterpret_cast<void*>(static_cast<uintptr_t>(s_rawDeviceValue));
            debugSamplePassed = SamplePostFXLuminance(device);
            if (debugSamplePassed)
                InterlockedIncrement(&s_debugXboxBloomReadbackPassCount);
            else
                InterlockedIncrement(&s_debugXboxBloomReadbackFailCount);
        }
        if (s_postProcessRoute == 3u &&
            (debugSampleAttempted || s_presentCount == 1u ||
             (s_presentCount % kRetailXboxHeartbeatPresents) == 0u))
        {
            QueueDebugXboxD2Probe(debugSampleAttempted, debugSamplePassed);
        }

        if (s_presentCount == 1u || (s_presentCount % kRetailXboxHeartbeatPresents) == 0u)
        {
            Event* summary = s_postProcessRoute == 3u
                ? nullptr : ReserveEvent(EventType::NativeFinalShaderSummary);
            if (summary)
            {
                summary->a = s_presentCount;
                summary->b = static_cast<uint32_t>(s_mode5FinalShaderBindCount);
                summary->c = static_cast<uint32_t>(s_mode5FinalShaderCaptureState);
                summary->d = s_mode5FinalShaderBytecodeSize;
                summary->e = s_mode5FinalShaderBytecodeHash;
                summary->f = ReadU32(kCompositePSFinal);
                summary->g = ReadU32(kActivePSCache);
                summary->h = static_cast<uint32_t>(s_routeMismatchCount);
                summary->extra[0] = static_cast<uint32_t>(s_mode5FinalCloneState);
                summary->extra[1] = static_cast<uint32_t>(s_mode5FinalCloneDrawCount);
                summary->extra[2] = static_cast<uint32_t>(s_mode5FinalClonePassCount);
                summary->extra[3] = static_cast<uint32_t>(s_mode5FinalCloneFailCount);
                summary->extra[4] = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(s_mode5FinalCloneShader));
                summary->extra[5] = s_mode5FinalCloneBytecodeHash;
                summary->extra[6] = static_cast<uint32_t>(s_mode5FinalClampPatchState);
                summary->extra[7] = s_mode5FinalClampSourceWord;
                summary->extra[8] = s_mode5FinalClampPatchedWord;
                summary->extra[9] = s_mode5FinalClampPatchedHash;
                summary->extra[10] = s_mode5FinalClampPatchedBytes;
                summary->extra[11] = static_cast<uint32_t>(s_retailXboxDofProbeAttemptCount);
                summary->extra[12] = static_cast<uint32_t>(s_retailXboxDofProbePassCount);
                summary->extra[13] = static_cast<uint32_t>(s_retailXboxDofProbeFailCount);
                summary->extra[14] = s_retailXboxDofShaderBytes;
                summary->extra[15] = s_retailXboxDofShaderHash;
                summary->extra[16] = static_cast<uint32_t>(s_retailXboxDepthArmCount);
                summary->extra[17] = static_cast<uint32_t>(s_retailXboxDepthArmFailCount);
                summary->extra[18] = static_cast<uint32_t>(s_retailXboxDepthFeedAttemptCount);
                summary->extra[19] = static_cast<uint32_t>(s_retailXboxDepthFeedPassCount);
                summary->extra[20] = static_cast<uint32_t>(s_retailXboxDepthFeedFailCount);
                summary->extra[21] = s_mode4ReszDepthValidated ? 1u : 0u;
                summary->extra[22] = s_provenIntzDofPresent;
                summary->extra[23] = s_provenIntzDofDepthTexture;
                summary->extra[24] = static_cast<uint32_t>(s_retailXboxDofDrawAttemptCount);
                summary->extra[25] = static_cast<uint32_t>(s_retailXboxDofDrawCount);
                summary->extra[26] = static_cast<uint32_t>(s_retailXboxDofDrawPassCount);
                summary->extra[27] = static_cast<uint32_t>(s_retailXboxDofDrawFallbackCount);
                summary->i = static_cast<uint32_t>(s_retailXboxDofDrawFailCount);
                summary->j = static_cast<uint32_t>(s_retailXboxDofDrawDisabled);
            }

            if (IsXboxPostProcessRoute())
            {
                Event* heartbeat = ReserveEvent(EventType::RetailXboxIntegrationHeartbeat);
                if (heartbeat)
                {
                    heartbeat->a = s_presentCount;
                    heartbeat->b = s_postProcessRoute;
                    heartbeat->c = static_cast<uint32_t>(s_mode5DrawHookState);
                    heartbeat->d = static_cast<uint32_t>(s_routeMismatchCount);
                    heartbeat->e = static_cast<uint32_t>(s_retailXboxDepthArmCount);
                    heartbeat->f = static_cast<uint32_t>(s_retailXboxDepthArmFailCount);
                    heartbeat->g = s_mode4ReszDepthValidated ? 1u : 0u;
                    heartbeat->h = s_provenIntzDofPresent;
                    heartbeat->i = s_provenIntzDofDepthTexture;
                    heartbeat->j = static_cast<uint32_t>(s_retailXboxDofDrawAttemptCount);
                    heartbeat->extra[0] = static_cast<uint32_t>(s_retailXboxDofDrawCount);
                    heartbeat->extra[1] = static_cast<uint32_t>(s_retailXboxDofDrawPassCount);
                    heartbeat->extra[2] = static_cast<uint32_t>(s_retailXboxDofDrawFallbackCount);
                    heartbeat->extra[3] = static_cast<uint32_t>(s_retailXboxDofDrawFailCount);
                    heartbeat->extra[4] = static_cast<uint32_t>(s_actualFinalF18Disabled);
                    heartbeat->extra[5] = static_cast<uint32_t>(s_actualFinalF18PassCount);
                    heartbeat->extra[6] = static_cast<uint32_t>(s_actualFinalF18FailCount);
                    heartbeat->extra[7] = static_cast<uint32_t>(s_retailXboxImageZoomDrawAttemptCount);
                    heartbeat->extra[8] = static_cast<uint32_t>(s_retailXboxImageZoomDrawCount);
                    heartbeat->extra[9] = static_cast<uint32_t>(s_retailXboxImageZoomDrawPassCount);
                    heartbeat->extra[10] = static_cast<uint32_t>(s_retailXboxImageZoomDrawFailCount);
                    heartbeat->extra[11] = static_cast<uint32_t>(s_retailXboxImageZoomDrawSkipCount);
                    heartbeat->extra[12] = static_cast<uint32_t>(s_retailXboxImageZoomDrawDisabled);
                    heartbeat->extra[13] = s_retailXboxZBlurVsHash;
                    heartbeat->extra[14] = s_retailXboxZBlurPsHash;
                    heartbeat->extra[15] = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(s_retailXboxImageZoomSourceTexture));
                    heartbeat->extra[16] = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(s_retailXboxImageZoomSourceSurface));
                    const bool godRaySelectorReadable = IsReadableMemory(
                        reinterpret_cast<void*>(kPcDormantGodRaySelector), sizeof(uint32_t));
                    // V10.5.66.1: align heartbeat payload with the formatter. In .66
                    // extra[17] incorrectly held final-shader captureState (normally 2),
                    // which printed as godRaySelector=00000002 and shifted every G1 count.
                    heartbeat->extra[17] = godRaySelectorReadable ? ReadU32(kPcDormantGodRaySelector) : 0u;
                    heartbeat->extra[18] = static_cast<uint32_t>(s_godRayG1SelectorTransitionCount);
                    heartbeat->extra[19] = static_cast<uint32_t>(s_godRayG1FinalSeenCount);
                    heartbeat->extra[20] = static_cast<uint32_t>(s_godRayG1ContractCaptureCount);
                    heartbeat->extra[21] = static_cast<uint32_t>(s_godRayG1ContractFailCount);
                    heartbeat->extra[22] = static_cast<uint32_t>(s_godRayG2BindAttemptCount);
                    heartbeat->extra[23] = static_cast<uint32_t>(s_godRayG2BindPassCount);
                    heartbeat->extra[24] = static_cast<uint32_t>(s_godRayG2BindFailCount);
                    heartbeat->extra[25] = static_cast<uint32_t>(s_godRayG3DrawCount);
                    heartbeat->extra[26] = static_cast<uint32_t>(s_godRayG3PassCount);
                    heartbeat->extra[27] = static_cast<uint32_t>(s_godRayG3FailCount);
                    heartbeat->extra[28] = static_cast<uint32_t>(s_godRayG4BoundaryAttemptCount);
                    heartbeat->extra[29] = static_cast<uint32_t>(s_godRayG4StockSuppressCount);
                    heartbeat->extra[30] = static_cast<uint32_t>(s_godRayG4StockFallbackCount);
                    heartbeat->extra[31] = static_cast<uint32_t>(s_deviceResetCount);
                }
            }
        }
        FlushEvents();
        if (s_logFile != INVALID_HANDLE_VALUE &&
            (s_presentCount == 1u || (s_presentCount % kRetailXboxHeartbeatPresents) == 0u))
        {
            FlushFileBuffers(s_logFile);
        }
        return;
    }

    const bool permanentMode4 =
        s_v10Mode == 4 && kMode4PermanentReszDofEnabled;

    // Modes 0-3 preserve all historical research telemetry. Permanent Mode 4
    // keeps only startup/failure/heartbeat diagnostics.
    if (!permanentMode4)
    {
        QueueV10Config();
        QueueV9Config();
        QueueV10_3XboxExposureConfig();
    }

    // V10 keeps the complete V8.1 proof layer, preserves V7 ping-pong, and
    // reconstructs the recovered Xbox scene-brightness-controlled bloom gain.
    TryAttachRawD3DDetours();
    if (s_rawHookState == 2)
    {
        if (s_v10Mode == 4 && !s_mode4MidProbeOnly && s_presentCount <= 4u)
        {
            void* device = reinterpret_cast<void*>(static_cast<uintptr_t>(s_rawDeviceValue));
            ProbeMode4DepthTextureCapabilities(device);
        }
        if (s_v10Mode == 4 && !s_mode4MidProbeOnly && s_presentCount <= 32u)
        {
            void* device = reinterpret_cast<void*>(static_cast<uintptr_t>(s_rawDeviceValue));
            ProbeMode4DepthBridge(device);
        }

        if (permanentMode4)
        {
            // V10.5.49.1 FIX: permanent Mode 4 suppresses the legacy V8/V8.1
            // telemetry path, but the full DOF follow-up preflight still needs
            // authoritative raw D3D identities for Scene, Quarter A and Quarter B.
            // V10.5.49 accidentally skipped ALL identity refresh while permanent
            // Mode 4 was active, so Mode4FullDofFollowupReady() always failed
            // before the DOF shader was ever bound (all activation HRESULTs stayed
            // E_PENDING). Resolve only the three resources required by the native
            // DOF chain; leave luminance/legacy identity telemetry suppressed.
            if (!s_rawTextureQuarterA || !s_rawSurfaceQuarterA)
                RefreshOneRawIdentity(ReadU32(kRtQuarterA), 1u);
            if (!s_rawTextureQuarterB || !s_rawSurfaceQuarterB)
                RefreshOneRawIdentity(ReadU32(kRtQuarterB), 2u);
            if (!s_rawTextureScene || !s_rawSurfaceScene)
                RefreshOneRawIdentity(CurrentSceneRT(), 8u);

            // V10.5.49.5: the recovered Xbox adaptive-bloom controller consumes
            // the already-produced PC 16x12 luminance target.  Keep only this
            // additional identity alive in permanent Mode 4; the second 16x12
            // target and legacy V8 telemetry remain dormant.
            if (!s_rawSurfaceLum16)
                RefreshOneRawIdentity(ReadU32(kRtLuminance16x12), 4u);
        }
        else
        {
            RefreshRawTextureIdentities();
            if (s_presentCount <= 4u || (s_presentCount % 300u) == 0u ||
                s_rawSurfaceLum16 == 0u || s_rawSurfaceSecond16 == 0u)
            {
                RefreshRawSurfaceIdentities();
            }
        }

        // V10.5.49.5 reuses the sparse luminance sampler in permanent Mode 4
        // solely for Xbox adaptive bloom. Exposure/DOF state remains untouched.
    }

    if (permanentMode4)
    {
        if (s_presentCount != 0u &&
            (s_presentCount % kMode4ReleaseHeartbeatPresents) == 0u)
        {
            Event* heartbeat = ReserveEvent(EventType::PermanentReszDofHeartbeat);
            if (heartbeat)
            {
                heartbeat->a = s_presentCount;
                heartbeat->b = s_provenIntzDofWindowProduced;
                heartbeat->c = s_provenIntzDofWindowApplied;
                heartbeat->d = s_provenIntzDofWindowFailures;
                heartbeat->e = static_cast<uint32_t>(s_provenIntzDofState);
                heartbeat->f = static_cast<uint32_t>(s_reszDepthResolveState);
                heartbeat->g = static_cast<uint32_t>(s_rawHookState);
                heartbeat->h = static_cast<uint32_t>(
                    reinterpret_cast<uintptr_t>(s_mode4PackedDepthTexture));
                heartbeat->i = static_cast<uint32_t>(
                    reinterpret_cast<uintptr_t>(s_offscreenIntzTexture));
                heartbeat->j = s_provenIntzDofPresent;
                heartbeat->extra[0] = s_provenIntzDofWindowFrameOrdinal;
                heartbeat->extra[1] = s_reszLiveDepthFormat;
                heartbeat->extra[2] = s_reszLiveDepthWidth;
                heartbeat->extra[3] = s_reszLiveDepthHeight;
                heartbeat->extra[4] = static_cast<uint32_t>(s_reszTriggerHr);
                heartbeat->extra[5] = static_cast<uint32_t>(s_reszApplyHr);
                heartbeat->extra[6] = static_cast<uint32_t>(s_provenIntzDofBindHr);
                heartbeat->extra[7] = static_cast<uint32_t>(s_provenIntzDofConstHr);
                heartbeat->extra[8] = static_cast<uint32_t>(s_provenIntzDofPsHr);
                heartbeat->extra[9] = static_cast<uint32_t>(s_provenIntzDofRestoreTexHr);
                heartbeat->extra[10] = static_cast<uint32_t>(s_provenIntzDofRestorePsHr);
                heartbeat->extra[11] =
                    (s_presentCount >= s_provenIntzDofPresent)
                        ? (s_presentCount - s_provenIntzDofPresent)
                        : 0u;
                // V10.5.49.2 identity proof. Bits: 0 sceneTex, 1 sceneSurf,
                // 2 quarterATex, 3 quarterASurf, 4 quarterBTex, 5 quarterBSurf.
                uint32_t rawIdMask = 0u;
                if (s_rawTextureScene >= 0x10000u) rawIdMask |= 0x01u;
                if (s_rawSurfaceScene >= 0x10000u) rawIdMask |= 0x02u;
                if (s_rawTextureQuarterA >= 0x10000u) rawIdMask |= 0x04u;
                if (s_rawSurfaceQuarterA >= 0x10000u) rawIdMask |= 0x08u;
                if (s_rawTextureQuarterB >= 0x10000u) rawIdMask |= 0x10u;
                if (s_rawSurfaceQuarterB >= 0x10000u) rawIdMask |= 0x20u;
                heartbeat->extra[12] = s_rawTextureScene;
                heartbeat->extra[13] = s_rawTextureQuarterA;
                heartbeat->extra[14] = s_rawTextureQuarterB;
                heartbeat->extra[15] = rawIdMask;
                // V10.5.49.10.14: Present-side proof for the output-target isolation
                // late-callsite wrapper. This heartbeat itself is telemetry only.
                heartbeat->extra[16] = static_cast<uint32_t>(s_bloomCompositeLateCallsitePatchState);
                heartbeat->extra[17] = static_cast<uint32_t>(s_bloomCompositeLateCallsiteHits);
                heartbeat->extra[18] = s_bloomCompositeLateCallsiteLastReturn;
                heartbeat->extra[19] = static_cast<uint32_t>(s_cameraMotionZBlurOutputTargetOnlyAttemptCount);
                heartbeat->extra[20] = static_cast<uint32_t>(s_cameraMotionZBlurOutputTargetOnlySuccessCount);
                heartbeat->extra[21] = static_cast<uint32_t>(s_cameraMotionZBlurOutputTargetOnlyFailureCount);
                uint8_t samplerCallsiteBytes[5] = {};
                const uint8_t* samplerCallsite = reinterpret_cast<const uint8_t*>(
                    kBloomCompositeLateCallsiteAddress);
                if (IsReadableMemory(samplerCallsite, sizeof(samplerCallsiteBytes)))
                    memcpy(samplerCallsiteBytes, samplerCallsite, sizeof(samplerCallsiteBytes));
                heartbeat->extra[22] = samplerCallsiteBytes[0];
                heartbeat->extra[23] = samplerCallsiteBytes[1];
                heartbeat->extra[24] = samplerCallsiteBytes[2];
                heartbeat->extra[25] = samplerCallsiteBytes[3];
                heartbeat->extra[26] = samplerCallsiteBytes[4];
            }
        }
    }
    else if (s_presentCount == 1u || (s_presentCount % 300u) == 0u)
    {
        Event* event = ReserveEvent(EventType::Summary);
        if (event)
        {
            event->a = s_presentCount;
            event->b = static_cast<uint32_t>(s_compositeCount);
            event->c = static_cast<uint32_t>(s_routeBlur1Count);
            event->d = static_cast<uint32_t>(s_routeBlur2Count);
            event->e = static_cast<uint32_t>(s_routeFinalCount);
            event->f = static_cast<uint32_t>(s_routeMismatchCount);
            event->g = ReadU8(kAutoExposureEnabled);
            event->h = static_cast<uint32_t>(s_bindLum16Count);
            event->i = static_cast<uint32_t>(s_bindSecond16Count);
            event->j = static_cast<uint32_t>(s_setRTLum16Count);
        }

        Event* raw = ReserveEvent(EventType::RawSummary);
        if (raw)
        {
            raw->a = s_presentCount;
            raw->b = static_cast<uint32_t>(s_rawHookState);
            raw->c = static_cast<uint32_t>(s_rawLumTextureCount);
            raw->d = static_cast<uint32_t>(s_rawSecondTextureCount);
            raw->e = static_cast<uint32_t>(s_rawLumSetRTCount);
            raw->f = static_cast<uint32_t>(s_rawSecondSetRTCount);
            raw->g = static_cast<uint32_t>(s_rawSetPixelShaderCount);
            raw->h = static_cast<uint32_t>(s_rawPSConstantCount);
            raw->i = s_rawTextureLum16;
            raw->j = s_rawTextureSecond16;
            raw->extra[0] = s_rawSurfaceLum16;
            raw->extra[1] = s_rawSurfaceSecond16;
            raw->extra[2] = s_rawCurrentPixelShader;
            raw->extra[3] = s_rawCurrentRT0;
            raw->extra[4] = s_rawLumGetContainerHr;
            raw->extra[5] = s_rawSecondGetContainerHr;
            raw->extra[6] = s_rawLumIdentitySource;
            raw->extra[7] = s_rawSecondIdentitySource;
        }

        QueueV9Summary();
        QueueV10Summary();
        QueueV10_4NativeExposureSummary();
        QueueMode4DofSummary();
    }

    FlushEvents();
    const uint32_t flushPeriod = permanentMode4 ?
        kMode4ReleaseHeartbeatPresents : 300u;
    if (s_logFile != INVALID_HANDLE_VALUE &&
        s_presentCount != 0u && (s_presentCount % flushPeriod) == 0u)
    {
        FlushFileBuffers(s_logFile);
    }
}



