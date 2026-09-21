// V10.5.72 RELEASE-INTEGRATED — DEBUG XBOX POSTFX + D3D9 RESET RECOVERY
//   Public priority: DebugXbox > RetailXbox > PostProcessFix > RetailPC.
//   Proven Debug adaptive bloom + F18 + ImageZoom route remains unchanged.
//   GodRay G1/G1.1 contract and G2 bind/state reproduction remain active.
//   G3 runs only at stock 0079849C -> return 007984A2 with selector != 0.
//   After full G2 verification, xeSM3 issues one original DrawPrimitive
//   TRIANGLESTRIP/0/2, verifies draw-held state, and restores exact pre-probe state.
//   G4 suppresses the duplicate stock draw only after that same G3 invocation passes;
//   otherwise the original stock DrawPrimitive executes as the fail-closed fallback.
//   No RT/DS/viewport setter, no selector write, no direct 00797D50 call.
//
#pragma once

// Spider-Man 3 PC native post-processing restoration / research hooks.
//
// Preserved foundation:
//   Mode 0 = V7 baseline / FP16 + quarter A/B bloom ping-pong
//   Mode 1 = V10 Xbox-style dynamic bloom
//   Mode 2 = V10.1 frozen visual reference: Mode 1 + V9 full-frame exposure POC
//   Mode 3 = V10.4.1 SpeedTree exposure research (not global post-FX exposure)
//   Mode 4 = V10.5.49.10.14 proven late callsite + .10.6 depth fail-closed DOF + native bloom tail + ZBlur output-target isolation; full ZBlur draw OFF [DEFAULT]
//
// Locked depth producer (V10.5.36/37 + V10.5.48):
//   1. Stock SM3 opaque world renders once into the live full-resolution D24S8.
//   2. Retail NGL MID callback runs after opaque and before translucent rendering.
//   3. Legacy D3D9 RESZ resolves that already-rendered D24S8 into xeSM3's
//      full-resolution texture-backed INTZ resource.
//   4. INTZ is GPU-packed into the exact A/R/G 24-bit representation decoded by
//      the dormant PC BloomFinalCombineDOF shader at 010FBF18:
//        depth = A*(255/256) + R*(255/65536) + G*(255/16777216)
//   5. c0 uses the recovered Xbox bloomDepthControl algebra with live PC near/far.
//
// V10.5.49.10.14 pass topology (render path frozen from .10.6):
//   BLOOM WEIGHT: Xbox Function_82F31348 proves the weighted bloom stage samples
//           Scene directly, writes the weighted result into a working bloom RT,
//           resolves it, then runs Gaussian. PC maps this as Scene -> Quarter A
//           through dormant VS 010FBEB4 / PS 010FBE48 with Xbox adaptive c6.
//   NATIVE BLOOM TAIL: Gaussian A -> B, then PC dormant blurlevel1offset
//           (010FBF04 / blob 00AC2718) B -> A, then blurlevel2offset
//           (010FBF38 / blob 00AB61C8) A -> B. Live retail-PC x32 proof
//           shows dedicated c0 vectors: Level1={-6/W,-6/H,+6/W,-6/H},
//           Level2={-10/W,-10/H,+10/W,-10/H}, using full-scene dimensions.
//           Final/F18 samples B. If the tail cannot complete, the hook redraws
//           stock Gaussian #2 B -> A and, when that succeeds, falls back to
//           the .49.9 final source A.
//   PASS 1: stock final-draw location is replaced with 010FBF18 using
//           s0 = native bloom/color, s1 = same-present packed depth, c0 = live
//           bloomDepthControl.  PC-compat translation keeps stock 7/8 RGB
//           while alpha is independently replaced (2/1) with the depth mask.
//           This avoids the V10.5.49.2 additive-bloom blowout while preserving
//           the mask consumed by PASS 4.
//   PASS 2: after the stock composite returns, reuse PC's already-working
//           neutral scene -> quarter-A copy for the DOF follow-up.
//   PASS 3: DOF follow-up remains the frozen PC Gaussian A -> B -> A ping-pong.
//   PASS 4: restore PC's simple final shader and 7/8 blend, then draw blurred
//           quarter A back to Scene.  7/8 is DstAlpha/InvDstAlpha, so the mask
//           written by PASS 1 controls the visible depth-dependent blur.
//
// Important:
//   - Mode 4 insertion is the runtime-proven 0079618A CALL to 00797A70,
//     returning to 0079618F. 00797A70 itself is not entry-detoured in Mode 4.
//   - DAT_00EE5E90 remains OFF and 00797D50 is NOT called by xeSM3.
//   - No duplicate city render / no nglRenderNode replay.
//   - Historical Mode 4 retained CPU depth validation; Retail Xbox R2 does NOT use CPU depth readback. It reuses the already-proven GPU RESZ->INTZ->packed-depth producer and verifies same-present s1 identity.
//   - The dormant native weighted-bloom pair 00AC20F8/00ABCEC0 is activated
//     only for the fail-closed Scene -> QuarterA bloom-weight stage.
//   - The .49.8 preliminary-pass experiment is NOT part of this path; Xbox
//     Function_82F31348 does not place DAT_8438AAA8 before adaptive weighting.
//
// Safety fallback:
//   XESM3_V10_MODE=2
//
// Public xeSM3 build: persistent PostFX research logging is disabled.
// Primary V10.5.49.10.14 proof/health tags:
//   [POSTFX:RESZ_DEPTH_RESOLVE]
//   [POSTFX:DOF_C0_BRIDGE]
//   [POSTFX:PROVEN_INTZ_DOF_ACTIVATION]
//   [POSTFX:DOF_FULL_CHAIN]
//   [POSTFX:NATIVE_BLOOM_TAIL]
//   [POSTFX:CAMERA_MOTION_ZBLUR_OUTPUT_TARGET_ONLY] // prior clean stages + RT0/DS/viewport bind/readback/restore; fullZBlurDraw=0
//   [POSTFX:PROVEN_LATE_CALLSITE]
//   [POSTFX:RESZ_DOF_HEARTBEAT]
void AttachPostFXResearchDetours();
bool InstallPostFXResearchCallsitePatches();
void PostFXResearch_OnPresent();

// V10.5.49.10.14 NATIVE WEIGHTED SCENE-SOURCE + XBOX-ORDER BLOOM TAIL + PC C0
//   Scene -> QuarterA through retail dormant VS 010FBEB4 / PS 010FBE48
//   immediately before the recovered Gaussian -> BlurLevel1 -> BlurLevel2 tail.
//   VS c0 = {-5*E8FAF8,0,+5*E8FAF8,0}; PS c0-c5 = native PC offset blocks;
//   PS c6 = {adaptiveBloom*.375, adaptiveBloom*.25, adaptiveBloom*.0625, 0}.
//   Tail c0: L1={-6/W,-6/H,+6/W,-6/H}; L2={-10/W,-10/H,+10/W,-10/H}.
//   Tail success: Gaussian A->B, BlurLevel1 B->A, BlurLevel2 A->B, final B.
//   Tail failure: redraw stock Gaussian #2 B->A and use .49.9 final A.
