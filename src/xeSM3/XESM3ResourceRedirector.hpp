// xeSM3 - Spider-Man 3 PC Loose Resource Mod Loader
// Created by TSGAMING264
// Frozen proven resource-loader core

#pragma once
#include <Windows.h>

// XESM3 frozen resource redirector API
void AttachApkfHandlerRegistrationTraceDetours();
void AttachGenericApkfDispatchProbeDetour();
void AttachNativeTexRedirectorDetour();
void AttachNativeMatRedirectorDetour();
void AttachNativeMeshRedirectorDetour();
void AttachNativeAnimRedirectorDetour();
void AttachNativeAnimRuntimeDiagnosticDetours();
void AttachMeshGpuProbeDetour();
void AttachRenderMeshProbeDetour();
void AttachNativeSkelRedirectorDetour();
void InitResourceRedirector();
void EnsureResourceRedirectorInstalled();
void ReloadMods();
void ClearResourceLog();
void ArmNativeAnimTrackStateSnapshot();
void ArmNativeAnimResidualDecoderCapture();
void RestartNativeAnimIndependentDecoderValidation();
void RestartNativeAnimInitialDecoderValidation();
void RestartNativeAnimPoseReconstructionValidation();
LONG GetGenericApkfDispatchCallCount();
LONG GetGenericApkfRangeDispatchCallCount();
LONG GetGenericApkfResourceSeenCount();
LONG GetGenericApkfExactLooseMatchCount();
LONG GetNativeApkfOverlayCandidateCount();
LONG GetNativeApkfMeshHandoffCount();
LONG GetNativeApkfMatHandoffCount();
LONG GetNativeApkfAnimHandoffCount();
LONG GetNativeApkfTexHandoffCount();
LONG GetNativeAsklShadowPreparedCount();
LONG GetNativeAsklShadowFailureCount();
LONG GetNativeAsklResolverSubstituteCount();
LONG GetNativeAsklShadowReuseCount();
LONG GetNativeAsklOwnerRetireCount();
LONG GetNativeSkelContextResetCount();
LONG GetResourceRequestCount();
LONG GetUniqueResourceCount();
LONG GetLooseResourceMatchCount();
LONG GetIndexedModCount();
LONG GetNativeTexOverrideSuccessCount();
LONG GetNativeTexOverrideFailureCount();
LONG GetNativeTexHighResOverrideCount();
LONG GetEnabledModPackageCount();
LONG GetDisabledModPackageCount();
LONG GetModConflictCount();
LONG GetScopedNativeTexCount();
LONG GetScopedNativeTexApplyCount();
LONG GetNativeMatOverrideSuccessCount();
LONG GetNativeMatOverrideFailureCount();
LONG GetScopedNativeMatCount();
LONG GetScopedNativeMatApplyCount();
LONG GetNativeMeshOverrideSuccessCount();
LONG GetNativeMeshOverrideFailureCount();
LONG GetScopedNativeMeshCount();
LONG GetScopedNativeMeshApplyCount();
LONG GetNativeAnimOverrideSuccessCount();
LONG GetNativeAnimOverrideFailureCount();
LONG GetScopedNativeAnimCount();
LONG GetScopedNativeAnimApplyCount();
LONG GetNativeAnimReapplyCount();
LONG GetNativeAnimPersistenceSkipCount();
LONG GetNativeAnimShadowPostHandlerVerifyCount();
LONG GetNativeAnimShadowPostHandlerFailureCount();
LONG GetNativeAnimKnownStockNormalizationCount();
LONG GetNativeAnimUnexpectedPostHandlerMutationCount();
LONG GetNativeAnimShadowOwnerPeakCount();
LONG GetNativeSkelOverrideSuccessCount();
LONG GetNativeSkelOverrideFailureCount();
LONG GetScopedNativeSkelCount();
LONG GetScopedNativeSkelApplyCount();
LONG GetArchiveCatalogRowCount();
LONG GetResolvedArchiveContextCount();
LONG GetUnresolvedScopedRequestCount();
bool IsGenericApkfDispatchProbeInstalled();
bool IsNativeTexRedirectorInstalled();
bool IsNativeMatRedirectorInstalled();
bool IsNativeMeshRedirectorInstalled();
bool IsNativeAnimRedirectorInstalled();
bool IsNativeSkelRedirectorInstalled();
bool IsResourceRedirectorInstalled();
const char* GetResourceStorageMode();

// Silent public-build test diagnostics (no persistent log file).
long XESM3_GetEnabledModPackageCountForDiagnostics();
long XESM3_GetIndexedResourceCountForDiagnostics();
