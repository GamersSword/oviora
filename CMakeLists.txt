cmake_minimum_required(VERSION 3.24)

project(oviora VERSION 2026.11.14 LANGUAGES C CXX RC)
# Moonlight/Xbox have C sources independently of the optional PS5 backend.
# Shown in the EXE's version resource and in the package name. CMake versions
# must be numeric, so the beta marker lives here instead.
set(OVIORA_DISPLAY_VERSION "2026.11.14" CACHE STRING "Human-readable version string")
# The cache keeps the first value it ever saw, so a release that bumps
# project(VERSION) used to ship an EXE still stamped with the previous display
# string. Re-align it whenever the cached label does not start with the project
# version; an explicit matching label (e.g. "1.4.0beta") is still honoured.
if(NOT OVIORA_DISPLAY_VERSION MATCHES "^${PROJECT_VERSION}")
  message(STATUS "OVIORA_DISPLAY_VERSION ${OVIORA_DISPLAY_VERSION} does not match project ${PROJECT_VERSION}; using ${PROJECT_VERSION}")
  set(OVIORA_DISPLAY_VERSION "${PROJECT_VERSION}" CACHE STRING "Human-readable version string" FORCE)
endif()

# Match the compiler's actual bytes, including on localized Windows installs.
# CMake's cached, decoded prefix can leave Ninja with zero header dependencies.
if(MSVC AND CMAKE_GENERATOR MATCHES "Ninja")
  set(OVIORA_INCLUDE_PROBE "${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/oviora-include-probe")
  file(WRITE "${OVIORA_INCLUDE_PROBE}.h" "// Dependency parser probe\n")
  file(WRITE "${OVIORA_INCLUDE_PROBE}.cpp" "#include \"oviora-include-probe.h\"\n")
  execute_process(COMMAND "${CMAKE_CXX_COMPILER}" /nologo /utf-8 /showIncludes /c /TP "${OVIORA_INCLUDE_PROBE}.cpp" "/Fo${OVIORA_INCLUDE_PROBE}.obj"
    OUTPUT_VARIABLE OVIORA_INCLUDE_OUTPUT ERROR_VARIABLE OVIORA_INCLUDE_OUTPUT
    RESULT_VARIABLE OVIORA_INCLUDE_RESULT ENCODING NONE)
  if(NOT OVIORA_INCLUDE_RESULT EQUAL 0 OR NOT OVIORA_INCLUDE_OUTPUT MATCHES "(^|[\r\n])([^\r\n]*: )[ ]*[A-Za-z]:[^\r\n]*oviora-include-probe.h")
    message(FATAL_ERROR "Unable to verify MSVC/Ninja header dependency prefix")
  endif()
  set(CMAKE_CL_SHOWINCLUDES_PREFIX "${CMAKE_MATCH_2}")
endif()

# ---------------------------------------------------------------------------
# Oviora cache variables and feature switches (Playbook section 5.1).
# Machine-specific absolute paths are resolved by scripts/build.ps1 at
# configure time; CMakePresets.json stays free of local paths.
# ---------------------------------------------------------------------------
set(OVIORA_DLSS_SDK_ROOT "" CACHE PATH "NVIDIA DLSS SDK root")
set(OVIORA_NVOF_SDK_ROOT "" CACHE PATH "NVIDIA Optical Flow SDK root")
if(NOT OVIORA_NVOF_SDK_ROOT)
  set(OVIORA_NVOF_CANDIDATE "${CMAKE_CURRENT_SOURCE_DIR}/third_party_local/nvidia/Optical_Flow_SDK_5.0.7")
  if(EXISTS "${OVIORA_NVOF_CANDIDATE}/NvOFInterface/nvOpticalFlowCommon.h")
    set(OVIORA_NVOF_SDK_ROOT "${OVIORA_NVOF_CANDIDATE}")
  endif()
endif()
set(OVIORA_RUNTIME_ROOT "" CACHE PATH "Local NVIDIA runtime root")
set(OVIORA_FFMPEG_ROOT "" CACHE PATH "vcpkg-installed x64-windows root with FFmpeg (player deps)")
option(OVIORA_ENABLE_DEINTERLACE "Enable optional LGPL FFmpeg deinterlacing module" ON)
set(OVIORA_CLIP_TOOLS_ROOT "" CACHE PATH "vcpkg-installed x64-windows root with openh264-enabled FFmpeg (clip tooling only)")

option(OVIORA_ENABLE_EXPERIMENTAL_DLSSNR
  "Enable local-only experimental Feature 18 adapter" OFF)
option(OVIORA_ENABLE_REMOTEPLAY "Build PS5 Remote Play with pinned Chiaki source" OFF)
option(OVIORA_ENABLE_DLSS_SR "Enable public DLSS SR backend" OFF)
option(OVIORA_ENABLE_DLSS_FG "Enable public DLSSG backend" OFF)
option(OVIORA_ENABLE_NVOF "Enable NVIDIA Optical Flow guidance" OFF)
option(OVIORA_ENABLE_D3D12_DEBUG "Enable D3D12 debug layer" ON)
set(OVIORA_FIDELITYFX_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party_local/amd/FidelityFX-SDK/sdk" CACHE PATH "Local official FidelityFX SDK 1.1.4")
set(OVIORA_XESS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party_local/intel/xess-3.0.2" CACHE PATH "Local Intel XeSS SDK 3.0.2")
set(OVIORA_FIDELITYFX2_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party_local/amd/FidelityFX-SDK-2.3.0/Kits/FidelityFX" CACHE PATH "Local AMD FidelityFX SDK 2.3.0 Kits/FidelityFX tree")

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(OvioraWarnings)
include(OvioraShaders)

# NVIDIA's static NGX core shim (nvsdk_ngx_s*.lib) is built against the
# static CRT (MT/MTd); match it for the diagnostic tools that link it.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Moonlight targets come after the runtime setting: they link into the same
# executables as the rest of the engine, so they must use the static CRT too.

# ---------------------------------------------------------------------------
# Targets grow phase by phase; only what actually compiles is declared.
# ---------------------------------------------------------------------------
add_library(oviora_base STATIC
  src/base/Log.cpp
  src/base/NgxResult.cpp
  src/base/FileIdentity.cpp
  src/base/RuntimePaths.cpp
  src/base/ThunkHook.cpp
  # .cube parsing, the runtime LUT store and named colour looks: low level, no
  # D3D12, shared by the ingest pipeline and the settings/import paths.
  src/engine/ColorLut.cpp
  src/engine/ColorLookStore.cpp
)
oviora_apply_warnings(oviora_base)
target_compile_options(oviora_base PUBLIC /utf-8)
target_compile_features(oviora_base PUBLIC cxx_std_20)
target_compile_definitions(oviora_base PUBLIC
  UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_include_directories(oviora_base PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(oviora_base PUBLIC bcrypt wintrust crypt32 version)


add_library(oviora_gfx STATIC
  src/gfx/GpuSchedulingPriority.cpp
  src/gfx/NvidiaAppProfile.cpp
  src/gfx/D3D12DeviceContext.cpp
  src/gfx/CommandSlotRing.cpp
  src/gfx/PresentSink.cpp
  src/gfx/XessPresenter.cpp
  src/gfx/XessMfgUnlock.cpp
  src/gfx/XessPacing.cpp
  src/gfx/FsrFgPresenter.cpp
  src/gfx/FsrSrBackend.cpp
)
oviora_apply_warnings(oviora_gfx)
target_compile_features(oviora_gfx PUBLIC cxx_std_20)
target_link_libraries(oviora_gfx PUBLIC oviora_base d3d12 dxgi dxguid)
if(EXISTS "${OVIORA_XESS_ROOT}/inc/xess_fg/xefg_swapchain_d3d12.h")
  target_include_directories(oviora_gfx PRIVATE "${OVIORA_XESS_ROOT}/inc")
  target_compile_definitions(oviora_gfx PRIVATE OVIORA_HAS_XESS=1)
endif()
if(EXISTS "${OVIORA_FIDELITYFX2_ROOT}/framegeneration/include/dx12/ffx_api_framegeneration_dx12.h")
  target_include_directories(oviora_gfx PRIVATE
    "${OVIORA_FIDELITYFX2_ROOT}/api/include"
    "${OVIORA_FIDELITYFX2_ROOT}/framegeneration/include"
    "${OVIORA_FIDELITYFX2_ROOT}/upscalers/include")
  target_compile_definitions(oviora_gfx PRIVATE OVIORA_HAS_FSR=1)
endif()
if(OVIORA_ENABLE_D3D12_DEBUG)
  target_compile_definitions(oviora_gfx PUBLIC OVIORA_D3D12_DEBUG=1)
endif()

# ---------------------------------------------------------------------------
# oviora_ngx: NGX core host + local experimental DLSSNR adapter.
# Compiled only with the explicit experimental switch; missing SDK paths are
# a configure-time failure, never a runtime surprise (Playbook 5.1/8.1).
# ---------------------------------------------------------------------------
if(OVIORA_ENABLE_EXPERIMENTAL_DLSSNR)
  if(NOT OVIORA_DLSS_SDK_ROOT OR NOT EXISTS "${OVIORA_DLSS_SDK_ROOT}/include/nvsdk_ngx.h")
    message(FATAL_ERROR "OVIORA_ENABLE_EXPERIMENTAL_DLSSNR requires OVIORA_DLSS_SDK_ROOT pointing at NVIDIA DLSS SDK 310.7 (include/nvsdk_ngx.h missing)")
  endif()
add_library(oviora_ngx STATIC
  src/ngx/NgxCoreHost.cpp
  src/ngx/NgxCoreCache.cpp
  src/ngx/DlssNrRuntimeAdapter.cpp
  src/ngx/NvapiLifetime.cpp
  src/ngx/DlssSrBackend.cpp
    src/ngx/VideoSrBackend.cpp
    src/ngx/TrueHdrBackend.cpp
  src/ngx/AnrProvider.cpp
  src/ngx/DlssFgBackend.cpp
  src/ngx/FgCompatibilitySession.cpp
  src/ngx/compat/hde/hde64.c
  src/ngx/AdaMfgUnlock.cpp
  src/ngx/AmpereMfgUnlock.cpp
  src/ngx/NvapiArchSpoof.cpp
  src/ngx/DlssgTransfusion.cpp
)
  set_source_files_properties(src/ngx/compat/hde/hde64.c PROPERTIES LANGUAGE CXX)
  oviora_apply_warnings(oviora_ngx)
  target_compile_features(oviora_ngx PUBLIC cxx_std_20)
  target_link_libraries(oviora_ngx PUBLIC oviora_base oviora_gfx d3d11)
  target_include_directories(oviora_ngx PUBLIC
    "${OVIORA_DLSS_SDK_ROOT}/include")
  # NVIDIA ships the static core shim as MT-Release; the _dbg variants match
  # the MDd debug runtime. Pick per configuration to avoid LNK2038.
  target_link_libraries(oviora_ngx PRIVATE
    "$<IF:$<CONFIG:Debug>,${OVIORA_DLSS_SDK_ROOT}/lib/Windows_x86_64/x64/nvsdk_ngx_s_dbg.lib,${OVIORA_DLSS_SDK_ROOT}/lib/Windows_x86_64/x64/nvsdk_ngx_s.lib>")
  target_compile_definitions(oviora_ngx PUBLIC
    OVIORA_EXPERIMENTAL_LOCAL_DLSSNR=1 OVIORA_HAS_NGX_CORE_CACHE=1 OVIORA_HAS_PAUSED_NR_RESIDUAL_REUSE=1)

  # Diagnostic: exercises the Ada multi-frame unlock against the local DLSS-G
  # runtime image without involving NGX (works on any NVIDIA adapter).

  # Diagnostic: exercises the RTX 30 (sm_86) Ampere unlock against the local
  # DLSS-G runtime image without involving NGX (works on any NVIDIA adapter).
endif()


# Dolby/DTS endpoint passthrough capability probe (diagnostic, never shipped).

# Isolated descriptor/Present diagnosis probe (user directive 2026-09-04 s3).
# No NGX/FFmpeg/NVOF/WASAPI. Uses PresentBlit shaders for cases F/G.

oviora_add_compute_shader(oviora_shader_parity_encode ParityEncode)
oviora_add_compute_shader(oviora_shader_parity_decode ParityDecode)
oviora_add_compute_shader(oviora_shader_rdna3_nr_decode Rdna3NrDecode)
oviora_add_compute_shader(oviora_shader_hdr_video_sr HdrVideoSrRestore)
oviora_add_compute_shader(oviora_shader_sr_resolve SrResolve)
oviora_add_compute_shader(oviora_shader_hdr_amd_nr_encode HdrAmdNrEncode)
oviora_add_compute_shader(oviora_shader_hdr_amd_nr_restore HdrAmdNrRestore)
if(OVIORA_ENABLE_EXPERIMENTAL_DLSSNR)
  oviora_add_compute_shader(oviora_shader_test_pattern GenerateTestPattern)

  # NR stacking probe: several Feature-18 handles on one snippet session.

  # Phase 6: DLSSG truth harness + WASAPI audio probe.
endif()

# YUV→RGB shader is shared between the media pipeline and the harness.
oviora_add_compute_shader(oviora_shader_yuv_to_rgb YuvToLinearRgb)
oviora_add_compute_shader(oviora_shader_rgb_to_linear RgbToLinear)
oviora_add_compute_shader(oviora_shader_color_grade ColorGradePass)
# 纹理 / 清晰度 / 去朦胧: block statistics, blur, apply (ColorDetailPass).
oviora_add_compute_shader(oviora_shader_color_detail_reduce ColorDetailReduce)
oviora_add_compute_shader(oviora_shader_color_detail_blur ColorDetailBlur)
oviora_add_compute_shader(oviora_shader_color_detail_apply ColorDetailApply)
oviora_add_compute_shader(oviora_shader_yuy2_to_linear Yuy2ToLinear)
oviora_add_compute_shader(oviora_shader_packed_capture PackedCaptureToLinear)
add_dependencies(oviora_shader_rgb_to_linear oviora_shader_yuy2_to_linear)
# ScaleBlit: working frame -> swapchain backbuffer (Phase 6 present path).
oviora_add_compute_shader(oviora_shader_scale_blit ScaleBlit)
oviora_add_compute_shader(oviora_shader_effect_exclusion EffectExclusion)
# Nv12Upload: byte buffer -> luma/chroma planes (copy-free upload path).
oviora_add_compute_shader(oviora_shader_nv12_upload Nv12Upload)
# PresentBlit: fullscreen-triangle pixel blit onto flip-model back buffers.
oviora_add_graphics_shader_pair(oviora_shader_present_blit PresentBlit)
oviora_add_compute_shader(oviora_shader_present_motion PresentMotion)
oviora_add_compute_shader(oviora_shader_hdr_coverage HdrCurveCoverage)
# NvofDensify: SHORT2 S10.5 raw flow -> workingExtent float2 + confidence.
oviora_add_compute_shader(oviora_shader_nvof_densify NvofDensify)
oviora_add_compute_shader(oviora_shader_nr_downsample NrDownsample)
oviora_add_compute_shader(oviora_shader_nr_residual NrResidualComposite)
oviora_add_compute_shader(oviora_shader_nr_temporal NrTemporal)
# Stage-5 output stabiliser (anti-flicker, default off).
oviora_add_compute_shader(oviora_shader_nr_hold NrHold)
# NR inference reuse (realtime preview only, default off).
oviora_add_compute_shader(oviora_shader_nr_reuse_compose NrReuseCompose)
oviora_add_compute_shader(oviora_shader_nr_reuse_warp NrReuseWarp)
oviora_add_compute_shader(oviora_shader_flow_adapt FlowAdapt)
oviora_add_compute_shader(oviora_shader_flow_refine FlowRefine)
oviora_add_compute_shader(oviora_shader_flow_fill FlowFill)
oviora_add_compute_shader(oviora_shader_flow_claim FlowClaim)
# VfgShotChange: per-pair shot-change test for VFG (replaces the SDK's automatic one).
oviora_add_compute_shader(oviora_shader_vfg_shot_change VfgShotChange)

# ---------------------------------------------------------------------------
# oviora_parity: RenoDX-equivalent codec reference (CPU golden + shaders later).
# The CPU reference has no D3D12 dependency, so it builds unconditionally.
# ---------------------------------------------------------------------------



# Guidance product library (Playbook R3/R4): providers produce the unified
# GuidanceFrame. First real member: ZeroGuidanceProvider (honest fallback).
# NvofGuidanceProvider moves in from player_probe during R3.2.


# Pipeline product library (Playbook R2/R3): data contracts + reset coordinator.
# Grows in R3 as the real GPU graph migrates out of the probes.
add_library(oviora_pipeline STATIC
  src/pipeline/ResetCoordinator.cpp
  src/pipeline/ColorGradeTables.cpp
  src/pipeline/ColorGradeInstance.cpp
  src/pipeline/ColorDetailPass.cpp
  src/pipeline/AntiAliasPass.cpp
  src/pipeline/SmaaLookupTables.cpp
  src/pipeline/NrInstance.cpp
  src/pipeline/AmdHipPreflight.cpp
  src/pipeline/LmxxfNrBackend.cpp
  src/pipeline/Rdna3NrBackend.cpp
  src/pipeline/VfgBackend.cpp
  src/pipeline/VfgAmpereCompat.cpp
  src/pipeline/VfxSrBackend.cpp
)
oviora_apply_warnings(oviora_pipeline)
# FFmpeg/NVOF headers are third-party: /WX stays off for this target.
target_compile_options(oviora_pipeline PRIVATE /WX-)
target_compile_features(oviora_pipeline PRIVATE cxx_std_20)
target_compile_definitions(oviora_pipeline PUBLIC
  UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
  OVIORA_SHADER_DIR="${CMAKE_BINARY_DIR}/shaders")
target_include_directories(oviora_pipeline PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
set(OVIORA_CUDA_DRIVER_INCLUDE_DIR "" CACHE PATH "External CUDA driver headers for native VFG (never vendored)")
if(EXISTS "${OVIORA_CUDA_DRIVER_INCLUDE_DIR}/cuda.h")
  target_include_directories(oviora_pipeline PRIVATE "${OVIORA_CUDA_DRIVER_INCLUDE_DIR}")
  target_compile_definitions(oviora_pipeline PRIVATE OVIORA_HAVE_CUDA_DRIVER_HEADERS=1)
endif()
target_link_libraries(oviora_pipeline PUBLIC oviora_base oviora_gfx d3d12 dxgi dxguid d3dcompiler)
add_dependencies(oviora_pipeline oviora_shader_rdna3_nr_decode)

# Optional independent RX7000 network worker. All third-party source and runtime
# assets remain external; only our host and reproducible preparation script live here.
set(OVIORA_RDNA3_NETWORK_SOURCE_DIR "" CACHE PATH "Prepared external RedDuke network source (MIT)")
if(EXISTS "${OVIORA_RDNA3_NETWORK_SOURCE_DIR}/runtime/core/network.cpp")
endif()
# Issue 88: the AMD HIP self-test child, its class ladder and its result lines.
add_dependencies(oviora_pipeline oviora_shader_color_grade oviora_shader_color_detail_reduce oviora_shader_color_detail_blur oviora_shader_color_detail_apply)
# Spatial anti-aliasing (#53): FXAA / SMAA / MLAA ported from SAOG0721/Magpie
# d687997 (sources and notices: THIRD_PARTY_NOTICES.md). Quality tiers of one
# source are separate .dxil variants.
oviora_add_compute_shader(oviora_shader_aa_luma AaLuma)
oviora_add_compute_shader_variant(oviora_shader_aa_fxaa3 AaFxaa AaFxaa3 FXAA_PRESET=3)
oviora_add_compute_shader_variant(oviora_shader_aa_fxaa4 AaFxaa AaFxaa4 FXAA_PRESET=4)
oviora_add_compute_shader_variant(oviora_shader_aa_fxaa5 AaFxaa AaFxaa5 FXAA_PRESET=5)
oviora_add_compute_shader(oviora_shader_aa_smaa_edges AaSmaaEdges)
oviora_add_compute_shader_variant(oviora_shader_aa_smaa_weights_low AaSmaaWeights AaSmaaWeightsLow SMAA_PRESET_LOW)
oviora_add_compute_shader_variant(oviora_shader_aa_smaa_weights_medium AaSmaaWeights AaSmaaWeightsMedium SMAA_PRESET_MEDIUM)
oviora_add_compute_shader_variant(oviora_shader_aa_smaa_weights_high AaSmaaWeights AaSmaaWeightsHigh SMAA_PRESET_HIGH)
oviora_add_compute_shader_variant(oviora_shader_aa_smaa_weights_ultra AaSmaaWeights AaSmaaWeightsUltra SMAA_PRESET_ULTRA)
oviora_add_compute_shader(oviora_shader_aa_smaa_blend AaSmaaBlend)
oviora_add_compute_shader(oviora_shader_aa_temporal AaTemporalResolve)
oviora_add_compute_shader(oviora_shader_aa_mlaa_edges AaMlaaEdges)
oviora_add_compute_shader(oviora_shader_aa_mlaa_counts AaMlaaCounts)
oviora_add_compute_shader(oviora_shader_aa_mlaa_blend AaMlaaBlend)
# DLAA in HDR: adds the input's out-of-BT.709 part back to the DLSS result.
oviora_add_compute_shader(oviora_shader_dlaa_hdr_restore DlaaHdrRestore)
add_dependencies(oviora_pipeline oviora_shader_aa_luma oviora_shader_aa_fxaa3 oviora_shader_aa_fxaa4 oviora_shader_aa_fxaa5
  oviora_shader_aa_smaa_edges oviora_shader_aa_smaa_weights_low oviora_shader_aa_smaa_weights_medium
  oviora_shader_aa_smaa_weights_high oviora_shader_aa_smaa_weights_ultra oviora_shader_aa_smaa_blend oviora_shader_aa_temporal
  oviora_shader_aa_mlaa_edges oviora_shader_aa_mlaa_counts oviora_shader_aa_mlaa_blend oviora_shader_dlaa_hdr_restore)



# Standalone scene analyzer tool (phase5 gate expects oviora_scene_analyzer.exe).
if(OVIORA_FFMPEG_ROOT AND EXISTS "${OVIORA_FFMPEG_ROOT}/include/libavformat/avformat.h")
endif()

# NVOF probe tool (phase5 gate expects oviora_nvof_probe.exe).
set(OVIORA_NVOF_SDK_ROOT "" CACHE PATH "NVIDIA Optical Flow SDK root")
if(OVIORA_NVOF_SDK_ROOT AND EXISTS "${OVIORA_NVOF_SDK_ROOT}/NvOFInterface/nvOpticalFlowCommon.h")
  # Reusable NVOF D3D12 session (Phase 6 motion guidance for DLSSG).
  add_library(oviora_nvof STATIC src/ngx/NvOfSession.cpp)
  oviora_apply_warnings(oviora_nvof)
  target_compile_features(oviora_nvof PUBLIC cxx_std_20)
  target_include_directories(oviora_nvof PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    "${OVIORA_NVOF_SDK_ROOT}/NvOFInterface"
    "${OVIORA_NVOF_SDK_ROOT}/Common/NvOFBase")
  target_link_libraries(oviora_nvof PUBLIC oviora_base oviora_gfx)


  # s9-A3: table-driven null-argument fault injection for NvOfSession.
endif()

# Resolve FFmpeg before any consumer, including the shared enhancement graph.
if(OVIORA_FFMPEG_ROOT AND EXISTS "${OVIORA_FFMPEG_ROOT}/include/libavformat/avformat.h")
  list(APPEND CMAKE_MODULE_PATH "${OVIORA_FFMPEG_ROOT}/share/ffmpeg")
  find_package(FFMPEG REQUIRED)
endif()

# EnhanceGraph (the real GPU chain) needs the NGX + NVOF backends; wire it
# into oviora_pipeline only when both exist so bare contract builds still work.
if(TARGET oviora_ngx AND TARGET oviora_nvof AND OVIORA_FFMPEG_ROOT)
  target_sources(oviora_pipeline PRIVATE src/pipeline/EnhanceGraph.cpp src/pipeline/NrTemporalPass.cpp src/pipeline/NrHoldPass.cpp src/pipeline/NrReusePass.cpp src/core/SceneCadenceAnalyzer.cpp)
  target_include_directories(oviora_pipeline PRIVATE ${FFMPEG_INCLUDE_DIRS})
  target_link_libraries(oviora_pipeline PRIVATE oviora_ngx oviora_nvof ${FFMPEG_LIBRARIES})
  add_dependencies(oviora_pipeline
    oviora_shader_yuv_to_rgb oviora_shader_rgb_to_linear oviora_shader_parity_encode
    oviora_shader_parity_decode oviora_shader_scale_blit
    oviora_shader_hdr_amd_nr_encode oviora_shader_hdr_amd_nr_restore
    oviora_shader_nv12_upload oviora_shader_nvof_densify oviora_shader_packed_capture)
endif()

# ---------------------------------------------------------------------------
# oviora_media: FFmpeg demux/decode (Phase 3). Links only the Playbook 3.4
# dependency set from the pinned vcpkg install.
# ---------------------------------------------------------------------------
if(OVIORA_FFMPEG_ROOT AND EXISTS "${OVIORA_FFMPEG_ROOT}/include/libavformat/avformat.h")
  add_library(oviora_media STATIC
    src/media/FFmpegDemuxer.cpp
    src/media/DemuxSession.cpp
    src/media/FFmpegVideoDecoder.cpp
    src/media/FFmpegDeinterlacer.cpp
  )
  oviora_apply_warnings(oviora_media)
  # FFmpeg headers are third-party; /WX stays off for this target
  # (Playbook 5.3: never let third-party headers block the build).
  target_compile_options(oviora_media PRIVATE /WX-)
  target_compile_features(oviora_media PUBLIC cxx_std_20)
  # d3d11: the HEVC hardware path decodes on a private D3D11VA device and shares
  # the surfaces with the D3D12 graph as NT handles.
  target_link_libraries(oviora_media PUBLIC oviora_base oviora_gfx d3d11)
  if(OVIORA_ENABLE_DEINTERLACE)
    if(NOT EXISTS "${OVIORA_FFMPEG_ROOT}/include/libavfilter/avfilter.h")
      message(FATAL_ERROR "Deinterlacing requires the filter prefix; run scripts/ffmpeg/build-deinterlace-module.py and set OVIORA_FFMPEG_ROOT to its prefix (or explicitly disable OVIORA_ENABLE_DEINTERLACE).")
    endif()
    target_compile_definitions(oviora_media PRIVATE OVIORA_ENABLE_DEINTERLACE=1)
    # A reused build tree may retain FFMPEG_INCLUDE_DIRS from the pre-filter
    # prefix. Compile against the prefix whose filter headers we just checked.
    target_include_directories(oviora_media BEFORE PRIVATE "${OVIORA_FFMPEG_ROOT}/include")
  endif()
  target_include_directories(oviora_media PUBLIC ${FFMPEG_INCLUDE_DIRS})
  target_link_directories(oviora_media PUBLIC ${FFMPEG_LIBRARY_DIRS})
  target_link_libraries(oviora_media PUBLIC ${FFMPEG_LIBRARIES})


  # oviora_sinks: product output sinks (Playbook R3). First member is the
  # WASAPI audio sink extracted verbatim from player_probe; D3D12PresentSink
  # currently lives in oviora_gfx and moves here when the migration completes.
  add_library(oviora_sinks STATIC
    src/sink/WasapiAudioSink.cpp
    src/sink/CaptureAudioSession.cpp
    src/sink/CaptureAudioDsp.cpp
    src/sink/BitstreamAudio.cpp
    src/sink/BitstreamAudioSink.cpp
    src/sink/ImageExportSink.cpp
  )
  oviora_apply_warnings(oviora_sinks)
  target_compile_options(oviora_sinks PRIVATE /WX-)
  target_compile_features(oviora_sinks PRIVATE cxx_std_20)
  target_compile_definitions(oviora_sinks PRIVATE
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
  target_include_directories(oviora_sinks PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
  target_include_directories(oviora_sinks PRIVATE ${FFMPEG_INCLUDE_DIRS})
  target_compile_definitions(oviora_sinks PRIVATE OVIORA_SHADER_DIR="${CMAKE_BINARY_DIR}/shaders")
  # oviora_media: AudioPipeline reads packets from the shared network demux session (#48).
  target_link_libraries(oviora_sinks PUBLIC oviora_base oviora_gfx oviora_media ole32 windowscodecs mmdevapi avrt mfplat mfuuid mf)
  add_subdirectory(third_party/soundtouch EXCLUDE_FROM_ALL)
  target_link_libraries(oviora_sinks PRIVATE ${FFMPEG_LIBRARIES} oviora_soundtouch)
  if(FFMPEG_INCLUDE_DIRS)
    target_include_directories(oviora_sinks PRIVATE ${FFMPEG_INCLUDE_DIRS})
  endif()

  # oviora_sources: product input sources (Playbook R3/R7). MediaFileSource
  # composes oviora_media; no second decode implementation exists.
  add_dependencies(oviora_pipeline oviora_shader_effect_exclusion oviora_shader_sr_resolve oviora_shader_nr_downsample oviora_shader_nr_residual oviora_shader_nr_temporal oviora_shader_nr_hold oviora_shader_nr_reuse_compose oviora_shader_nr_reuse_warp oviora_shader_flow_adapt oviora_shader_flow_refine oviora_shader_flow_fill oviora_shader_flow_claim oviora_shader_vfg_shot_change)
  add_library(oviora_sources STATIC
    src/source/MediaFileSource.cpp
  )
  oviora_apply_warnings(oviora_sources)
  target_compile_options(oviora_sources PRIVATE /WX-)
  target_compile_features(oviora_sources PRIVATE cxx_std_20)
  target_link_libraries(oviora_sources PUBLIC oviora_sinks)
  target_compile_definitions(oviora_sources PRIVATE
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
  target_include_directories(oviora_sources PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
  target_link_libraries(oviora_sources PUBLIC oviora_pipeline oviora_media oviora_base)
  target_link_libraries(oviora_sources PRIVATE ${FFMPEG_LIBRARIES} ole32 oleaut32 d3d12 dxgi d3d11)

  if(OVIORA_ENABLE_REMOTEPLAY)
    set(OVIORA_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
    include(cmake/OvioraRemotePlay.cmake)
    target_compile_definitions(oviora_sources PUBLIC OVIORA_ENABLE_REMOTEPLAY=1)
    target_sources(oviora_sources PRIVATE src/source/RemotePlaySource.cpp src/source/RemotePlaySessionSource.cpp)
    target_link_libraries(oviora_sources PUBLIC oviora_remoteplay_chiaki)
  endif()




  # #48: shared demux session, reconnect, buffering, damaged-data tolerance (tests/network).


  # oviora_engine is linked for the subtitle engine (SRT/ASS/VTT parsing +
  # indexed cue lookup); FFmpeg import libraries come from its public surface.
  # main() holds hundreds of 8 KB EnhancementSettings copies; the 1 MB default stack overflowed partway (2.1.0).

  # Its headers carry UTF-8 Chinese literals (CaptureBuffer.h, EnhancementSettings.h).
  # wmain holds many PlayerSnapshot locals; the default 1 MiB stack overflowed at entry.

  if(OVIORA_FFMPEG_ROOT AND EXISTS "${OVIORA_FFMPEG_ROOT}/include/libavcodec/avcodec.h")
  endif()
  # Qt 6.8.3 is staged under deps/ (the user allowed the latest Qt). The probe is
# opt-in so a normal product build never depends on Qt being present.
option(OVIORA_BUILD_QT_PROBE "Build the Qt + D3D12 host probe" OFF)
option(OVIORA_BUILD_QML_UI "Build the QML front end" OFF)
if(OVIORA_BUILD_QT_PROBE OR OVIORA_BUILD_QML_UI)
  find_package(Qt6 6.8 QUIET COMPONENTS Core Gui Qml Quick QuickControls2 Widgets Network Concurrent)
endif()

# The QML bridge. It is engine code (no QML files), so it compiles into a
# separate small library rather than into oviora_engine: the product build must
# never depend on Qt being installed.
if(OVIORA_BUILD_QML_UI AND Qt6_FOUND)
  add_library(oviora_qml_bridge STATIC src/ui/QmlPlayerBridge.cpp
    include/oviora/ui/QmlPlayerBridge.h
    # P4-c: the existing subtitle renderer, reused as-is (no second renderer).
    apps/oviora/ui/SubtitleOverlay.cpp)
  set_source_files_properties(apps/oviora/ui/SubtitleOverlay.cpp PROPERTIES COMPILE_OPTIONS /utf-8)
  # AUTOMOC: QmlPlayerBridge declares Q_OBJECT, so its meta-object has to be
  # generated. Without this the library links but every signal is unresolved.
  # Keep bridge domains explicit: no source glob or duplicate runtime state.
  target_sources(oviora_qml_bridge PRIVATE
    src/ui/bridge/Hardware.cpp
    src/ui/bridge/Status.cpp
    src/ui/bridge/FrameGeneration.cpp
    src/ui/bridge/Colour.cpp
    src/ui/bridge/Preferences.cpp
    src/ui/bridge/RemotePlay.cpp
    src/ui/bridge/GameInput.cpp
    src/ui/bridge/MouseAim.cpp
    src/ui/bridge/StreamSession.cpp
    src/ui/bridge/Output.cpp
    src/ui/bridge/Subtitles.cpp
    src/ui/bridge/Playback.cpp
    src/ui/MenuBackdropStore.cpp include/oviora/ui/MenuBackdropStore.h
    src/ui/bridge/Enhancement.cpp
    src/ui/bridge/Chain.cpp
    src/ui/bridge/View.cpp
    src/ui/VideoOverlays.cpp
    src/ui/VideoOverlays.h
    src/ui/QmlPlayerBridge_p.h)
  set_target_properties(oviora_qml_bridge PROPERTIES AUTOMOC ON)
  # Interface language (UiLanguage.h): its own small library so the streaming models,
  # which are tested without the player, translate their messages too.
  add_library(oviora_ui_i18n STATIC src/ui/UiLanguage.cpp include/oviora/ui/UiLanguage.h)
  target_compile_features(oviora_ui_i18n PUBLIC cxx_std_20)
  target_compile_options(oviora_ui_i18n PRIVATE /utf-8)
  target_include_directories(oviora_ui_i18n PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
  target_link_libraries(oviora_ui_i18n PUBLIC oviora_base Qt6::Core)
  target_link_libraries(oviora_qml_bridge PUBLIC oviora_ui_i18n)
  target_compile_features(oviora_qml_bridge PRIVATE cxx_std_20)
  target_include_directories(oviora_qml_bridge PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/src ${FFMPEG_INCLUDE_DIRS}
    ${CMAKE_CURRENT_SOURCE_DIR}/apps/oviora/ui)
  # Widgets is needed only for QFileDialog: the native file picker is the one
  # piece of the old shell worth keeping, and its own dialog is what users
  # expect from a desktop app. No QWidget UI is created.
  target_link_libraries(oviora_qml_bridge PUBLIC oviora_engine Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick Qt6::QuickControls2 Qt6::Widgets PRIVATE shell32)
  find_package(Qt6 COMPONENTS Test)
  if(TARGET Qt6::Test)
  endif()
  # OVIORA_DISPLAY_VERSION is the human-readable version the app reports; it is
  # the same value the resource file uses, so the UI cannot drift from it.
  target_compile_definitions(oviora_qml_bridge PRIVATE OVIORA_DISPLAY_VERSION="${OVIORA_DISPLAY_VERSION}")
  # libass (ISC) + FreeType/FriBidi/HarfBuzz, static, for full ASS/SSA effect
  # subtitles. Local prebuilt tree only (vcpkg x64-windows-static-md); never in
  # source Git. Without it ASS tracks fall back to the built-in styled renderer.
  set(OVIORA_LIBASS_ROOT "" CACHE PATH "libass static install prefix (include/ass/ass.h, lib/ass.lib)")
  if(OVIORA_LIBASS_ROOT AND EXISTS "${OVIORA_LIBASS_ROOT}/include/ass/ass.h" AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/apps/oviora/ui/AssSubtitleRenderer.cpp")
    target_sources(oviora_qml_bridge PRIVATE apps/oviora/ui/AssSubtitleRenderer.cpp apps/oviora/ui/AssSubtitleRenderer.h)
    set_source_files_properties(apps/oviora/ui/AssSubtitleRenderer.cpp PROPERTIES COMPILE_OPTIONS /utf-8)
    target_include_directories(oviora_qml_bridge PRIVATE "${OVIORA_LIBASS_ROOT}/include")
    target_compile_definitions(oviora_qml_bridge PRIVATE OVIORA_HAS_LIBASS=1)
    foreach(lib ass harfbuzz fribidi freetype libpng16 zs bz2 brotlidec brotlicommon)
      target_link_libraries(oviora_qml_bridge PRIVATE "${OVIORA_LIBASS_ROOT}/lib/${lib}.lib")
    endforeach()
    target_link_libraries(oviora_qml_bridge PRIVATE gdi32 dwrite)
    message(STATUS "libass: ${OVIORA_LIBASS_ROOT}")
  else()
    message(STATUS "libass: not configured; ASS effects use the built-in styled renderer")
  endif()

  # The QML module is deployed as plain files next to the executable rather
  # than compiled into a resource: the qmldir-based module keeps the .qml
  # editable in a build tree, which matters while the interface is still being
  # worked on. A packaged release can switch to compiling them in.
  # WIN32: a GUI subsystem binary. Without it a console window follows the app
  # around, which is not what a player should look like. Diagnostics still reach
  # the log file through the logger.
  add_executable(oviora_qml_ui WIN32 apps/oviora-qml/main.cpp src/ui/QmlDataDirectory.cpp src/ui/QmlDiskCache.cpp)
  # The QML copy is its own always-run step rather than a POST_BUILD of the link:
  # its two long non-ASCII build paths on the link statement pushed that command past
  # CMake's response-file threshold, and link.exe reads response files in the
  # ANSI code page (the UTF-8 Qt library paths then fail to open).
  add_custom_target(oviora_qml_files ALL
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/qml ${CMAKE_CURRENT_BINARY_DIR}/qml
    COMMENT "Copying QML")
  add_dependencies(oviora_qml_ui oviora_qml_files)
  # Interface translations (设置 → 界面语言): one catalog, compiled in so a package
  # cannot ship without it. scripts/i18n/ builds it from the source strings.
  qt_add_resources(oviora_qml_ui "oviora_i18n" PREFIX "/i18n" BASE "${CMAKE_CURRENT_SOURCE_DIR}/i18n" FILES i18n/catalog.json)
  set(OVIORA_VB_CABLE_PACKAGE "" CACHE FILEPATH "Original VB-Audio Pack46 ZIP to include for optional offline installation")
  if(OVIORA_VB_CABLE_PACKAGE)
    file(SHA256 "${OVIORA_VB_CABLE_PACKAGE}" oviora_vb_cable_sha256)
    if(NOT oviora_vb_cable_sha256 STREQUAL "bc44e8974191a5a2427d6c979e2dab08dbb09925f3f435dfa2f0ba3e45b6eb90")
      message(FATAL_ERROR "OVIORA_VB_CABLE_PACKAGE must be the original VB-Audio Pack46 ZIP")
    endif()
    set_source_files_properties("${OVIORA_VB_CABLE_PACKAGE}" PROPERTIES QT_RESOURCE_ALIAS "VBCABLE_Driver_Pack46.zip")
    qt_add_resources(oviora_qml_ui "oviora_optional_vb_cable" PREFIX "/optional-drivers"
      FILES "${OVIORA_VB_CABLE_PACKAGE}" OPTIONS -no-compress)
  endif()
  set_target_properties(oviora_qml_ui PROPERTIES OUTPUT_NAME "Oviora")
  target_compile_features(oviora_qml_ui PRIVATE cxx_std_20)
  # Every thread without an explicit size (UI, engine, export worker) reserves the
  # image default. The 32-node EffectChain is 45 KB and the bridge, preset and
  # engine frames hold several (2026-10-18: bridge constructor 400 KB,
  # EngineController::run 385 KB), so 1 MB no longer leaves a safe margin.
  # Reserve only: pages are committed as they are touched.
  target_link_options(oviora_qml_ui PRIVATE /STACK:8388608)
  # Qt6::CorePrivate only for QUnifiedTimer's slow mode behind --slow-animations.
  target_link_libraries(oviora_qml_ui PRIVATE oviora_qml_bridge oviora_engine Qt6::CorePrivate Qt6::GuiPrivate dbghelp ole32)

  # Oviora platform: Oviora server account, friends, PSN sign-in (WebView2) and the PSN relay
  # for a friend's PS5. The server and its protocol are shared with the old Oviora.
  # Updates download and install only with OVIORA_PLATFORM_UPDATES (stage 6, the release build).
  option(OVIORA_PLATFORM_UPDATES "Download and install the Oviora server's signed releases" OFF)
  find_package(OpenSSL REQUIRED)
  add_library(oviora_platform STATIC
    src/platform/OvioraServer.cpp include/oviora/platform/OvioraServer.h
    src/platform/PsnRelayBridge.cpp include/oviora/platform/PsnRelayBridge.h
    src/platform/PsnWebView.cpp include/oviora/platform/PsnWebView.h src/platform/PsnAccountLookup.cpp include/oviora/platform/PsnAccountLookup.h
    src/platform/SecretStore.cpp include/oviora/platform/SecretStore.h
    src/platform/MessageCenter.cpp include/oviora/platform/MessageCenter.h
    src/platform/DiscordPresence.cpp include/oviora/platform/DiscordPresence.h
    src/platform/SubtitleScreen.cpp include/oviora/platform/SubtitleScreen.h
    third_party/qrcodegen/qrcodegen.cpp)
  set_target_properties(oviora_platform PROPERTIES AUTOMOC ON)
  target_compile_features(oviora_platform PRIVATE cxx_std_20)
  target_compile_options(oviora_platform PRIVATE /utf-8)
  target_compile_definitions(oviora_platform PRIVATE NOMINMAX)
  if(OVIORA_PLATFORM_UPDATES)
    target_compile_definitions(oviora_platform PRIVATE OVIORA_PLATFORM_UPDATES)
  endif()
  target_include_directories(oviora_platform PUBLIC include PRIVATE third_party/qrcodegen third_party/webview2/include)
  target_link_libraries(oviora_platform PUBLIC Qt6::Core Qt6::Gui Qt6::Quick Qt6::Widgets Qt6::Network Qt6::Concurrent
    PRIVATE OpenSSL::Crypto crypt32 ole32 shell32 advapi32 uuid)
  # Messages: end-to-end encryption from the vendored chiaki library (chiaki/e2e.h)
  if(NOT TARGET chiaki-lib)
    message(FATAL_ERROR "oviora_platform needs chiaki-lib (OVIORA_ENABLE_REMOTEPLAY) for the message encryption")
  endif()
  target_link_libraries(oviora_platform PRIVATE chiaki-lib)
  if(TARGET oviora_remoteplay_core)
    target_link_libraries(oviora_platform PRIVATE oviora_remoteplay_core)
  endif()
  target_link_libraries(oviora_qml_ui PRIVATE oviora_platform)
  # Oviora: in-game subtitle translation (src/platform/subtitle). PaddleOCR runs on an ONNX Runtime
  # loaded at run time from runtime/altyazi/ocr (headers only: third_party/onnxruntime); the shared
  # translation memory goes through the Oviora server; the local AI (Ollama) downloads on request.
  find_package(ZLIB REQUIRED)
  add_library(oviora_subtitle STATIC
    src/platform/subtitle/SubtitleTranslator.cpp src/platform/subtitle/SubtitleTranslator.h
    src/platform/subtitle/SubtitleText.cpp src/platform/subtitle/SubtitleText.h src/platform/subtitle/GameGlossary.cpp src/platform/subtitle/GameGlossary.h
    src/platform/subtitle/PaddleOcr.cpp src/platform/subtitle/PaddleOcr.h
    src/platform/subtitle/TranslationMemory.cpp src/platform/subtitle/TranslationMemory.h
    src/platform/subtitle/LocalAi.cpp src/platform/subtitle/LocalAi.h
    src/platform/subtitle/TranslationOverlay.cpp src/platform/subtitle/TranslationOverlay.h)
  set_target_properties(oviora_subtitle PROPERTIES AUTOMOC ON)
  target_compile_features(oviora_subtitle PRIVATE cxx_std_20)
  target_compile_options(oviora_subtitle PRIVATE /utf-8)
  target_compile_definitions(oviora_subtitle PRIVATE NOMINMAX)
  target_include_directories(oviora_subtitle PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/platform/subtitle
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/third_party/onnxruntime/include ${FFMPEG_INCLUDE_DIRS})
  target_link_libraries(oviora_subtitle PUBLIC oviora_platform oviora_engine Qt6::Core Qt6::Gui Qt6::Qml Qt6::Network
    PRIVATE ZLIB::ZLIB gdiplus dxgi)
  qt_add_resources(oviora_subtitle "oviora_altyazi" PREFIX "/altyazi" BASE "${CMAKE_CURRENT_SOURCE_DIR}/assets/altyazi"
    FILES assets/altyazi/ingilizce.txt)
  target_link_libraries(oviora_qml_ui PRIVATE oviora_subtitle)
  # WebView2Loader.dll is loaded at run time (official Microsoft.Web.WebView2 NuGet package)
  add_custom_command(TARGET oviora_qml_ui POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/webview2/x64/WebView2Loader.dll $<TARGET_FILE_DIR:oviora_qml_ui>/WebView2Loader.dll)

  # The per-build QML disk cache folder (QmlDiskCache.h).

  # G0.6: Theme easing curves read back through QML against the design's CSS curves.
  # Loads qml/ from the source tree; runs offscreen with the Qt bin directory on PATH.

  # G1.6: component smoke tests (Qt Quick Test), tests/qml/quick/tst_*.qml.
  find_package(Qt6 COMPONENTS QuickTest)
  if(TARGET Qt6::QuickTest)
  endif()
endif()

# Qt + D3D12 host probe (S3.0): answers whether Qt can host the native video
# swapchain window without costing present latency. Diagnostic only.
if(OVIORA_BUILD_QT_PROBE AND Qt6_FOUND)
endif()

  # Stage-4 defect-audit assertions: GPU-free checks on the architecture-spoof
  # scope and the Ampere re-label policy (see docs/NR_FG_OPTIMIZATION_PLAN_2026-09-29.md).
  # Stage-6 anti-flicker tier assertions (GPU-free: enum narrowing, flat/list
  # agreement, validation, chain-node carry).
  # Stage-5 output-stabiliser acceptance (anti-flicker, default off).
  # NR inference reuse: frame selection, Auto controller and settings (GPU-free),
  # and the reuse pass on real D3D12 dispatches.
  # NR inference reuse against full inference on a real video (offline readbacks).


  # Headless quality-core runner (Playbook R3.3): the phase5 gate's quality
  add_library(oviora_engine STATIC src/engine/EngineController.cpp src/engine/EngineWatchdog.cpp src/engine/PreviewGpuSession.cpp src/engine/EngineControllerImage.cpp src/engine/PosterFrame.cpp src/engine/EffectChain.cpp src/engine/PresetLibrary.cpp src/ui/PlayerUiFacade.cpp src/engine/TiledImageProcessor.cpp src/engine/PresetStore.cpp src/engine/VideoPresenter.cpp src/engine/DisplayStateService.cpp src/engine/Subtitles.cpp src/engine/SubtitleTap.cpp src/engine/FgCompatibilityProbe.cpp)
  target_sources(oviora_pipeline PRIVATE src/guidance/AmdOpticalFlow.cpp
    src/guidance/GpuDisOpticalFlow.cpp third_party/gpu-dis/src/gpu/native_strict_dis_provider.cpp)
  target_include_directories(oviora_pipeline PRIVATE third_party/gpu-dis/src/gpu)
  include(cmake/OvioraGpuDis.cmake)
  oviora_add_compute_shader(oviora_shader_gpu_dis_luma GpuDisLuma)
  oviora_add_compute_shader(oviora_shader_gpu_dis_validate GpuDisValidate)
  add_dependencies(oviora_pipeline oviora_shader_gpu_dis oviora_shader_gpu_dis_luma oviora_shader_gpu_dis_validate)
  if(EXISTS "${OVIORA_FIDELITYFX_ROOT}/bin/ffx_sdk/ffx_opticalflow_x64.lib")
    target_compile_definitions(oviora_pipeline PRIVATE OVIORA_HAS_AMD_OF=1)
    target_include_directories(oviora_pipeline PRIVATE "${OVIORA_FIDELITYFX_ROOT}/include")
    target_link_libraries(oviora_pipeline PRIVATE
      "${OVIORA_FIDELITYFX_ROOT}/bin/ffx_sdk/ffx_opticalflow_x64.lib"
      "${OVIORA_FIDELITYFX_ROOT}/bin/ffx_sdk/ffx_backend_dx12_x64.lib")
  endif()
  target_compile_features(oviora_engine PUBLIC cxx_std_20)
  target_compile_options(oviora_engine PRIVATE /utf-8)
  target_compile_definitions(oviora_engine PUBLIC OVIORA_PROJECT_ROOT="${CMAKE_CURRENT_SOURCE_DIR}" OVIORA_SHADER_DIR="${CMAKE_BINARY_DIR}/shaders")
  target_link_libraries(oviora_engine PUBLIC oviora_pipeline oviora_sources oviora_sinks oviora_ngx oviora_nvof)
 # Preset apply recurses over 21 KB ChainConfiguration frames; the default 1 MB stack is too close to the edge.
# oviora_base carries /utf-8 and NOMINMAX (PresetLibrary.cpp has Chinese literals and uses std::max).

  # Large independent settings/descriptor fixtures, like the preset contracts.

  # CPU bake of the colour grade: no GPU, no engine link - the tables the ingest
  # shader reads are pure maths and must be verifiable without a device.
  # GPU contract for the v4 colour chain: identity when off/neutral, exposure,
  # curve, saturation and hue, all through a real graph and its ingest pass.
  # Byte-exact regression for the effect insertion slots (colour today, more
  # effects later): one frame per configuration through the real graph, printed
  # as SHA-256 so a build from before a change can be compared with one after it.
  # GPU quality / HDR / temporal / slot / tiled / timing checks of the anti-aliasing effect (#53).
  # Adobe .cube parsing + the runtime LUT store (CPU only).
  # Named colour looks + .vpcolor export/import (CPU only).
  add_dependencies(oviora_engine oviora_shader_hdr_video_sr oviora_shader_present_motion oviora_shader_present_blit oviora_shader_hdr_coverage)
  # Headless export harness: verifies the frame-timestamp policy (tail snap,
  # dropped-sample gap fill, hard refusal) without the GUI.
 # same 32-node chain frames as oviora_qml_ui
  if(OVIORA_ENABLE_REMOTEPLAY)
    find_package(SDL3 3.4.14 EXACT CONFIG REQUIRED)
    # P4-e: the QML front end forwards the PC controller to the PS5 the same way.
    # Only the SDL library file and headers: SDL3::SDL3's own system-library list
    # pushed the oviora_qml_ui link line over CMake's response-file threshold, and
    # link.exe reads response files in the ANSI code page, which breaks the UTF-8
    # Qt paths under a non-ASCII folder. Those system libraries come from #pragma comment in
    # the private QmlPlayerBridge_p.h instead.
    if(TARGET oviora_qml_bridge)
      target_sources(oviora_qml_bridge PRIVATE src/remoteplay/ControllerInput.cpp src/remoteplay/Microphone.cpp)
      target_link_libraries(oviora_qml_bridge PUBLIC $<TARGET_LINKER_FILE:SDL3::SDL3-static>)
      target_link_libraries(oviora_qml_bridge PRIVATE SDL3::Headers)
    endif()
  endif()

  # matrix executes through the shared product libraries only.

  # UI migration guard: per-frame output hashes for fixed stage configurations.

  # Headless Remote Play probe: drives the product session stack with a saved
  # profile so codec/bitrate variants can be compared without the GUI.
  if(OVIORA_ENABLE_REMOTEPLAY)
  endif()

  # Phase 6 realtime engine probe: decode -> SR -> NR -> NVOF -> DLSSG -> present.
  if(OVIORA_ENABLE_EXPERIMENTAL_DLSSNR AND EXISTS "${OVIORA_NVOF_SDK_ROOT}/NvOFInterface/nvOpticalFlowCommon.h")
  endif()
endif()

# Clip generator: links the openh264-enabled toolchain FFmpeg ONLY.
if(OVIORA_CLIP_TOOLS_ROOT AND EXISTS "${OVIORA_CLIP_TOOLS_ROOT}/include/libavcodec/avcodec.h")
  target_link_directories(oviora_clip_gen PRIVATE "${OVIORA_CLIP_TOOLS_ROOT}/lib")
endif()

# Layered child subtitle windows require a modern-Windows compatibility manifest.
set(OVIORA_RESOURCE_FILENAME "Oviora.exe")
configure_file(apps/oviora/version.rc.in ${CMAKE_CURRENT_BINARY_DIR}/oviora-version.rc @ONLY)
if(TARGET oviora_qml_ui)
  set(OVIORA_RESOURCE_FILENAME "Oviora.exe")
  configure_file(apps/oviora/version.rc.in ${CMAKE_CURRENT_BINARY_DIR}/oviora-qml-version.rc @ONLY)
  target_sources(oviora_qml_ui PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/oviora-qml-version.rc)
endif()
configure_file(apps/oviora/resource.h ${CMAKE_CURRENT_BINARY_DIR}/oviora-resource.h COPYONLY)
configure_file(assets/oviora.ico ${CMAKE_CURRENT_BINARY_DIR}/oviora.ico COPYONLY)
set_source_files_properties(${CMAKE_CURRENT_BINARY_DIR}/oviora-version.rc PROPERTIES
  OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/assets/oviora.ico;${CMAKE_CURRENT_SOURCE_DIR}/apps/oviora/resource.h")
if(TARGET oviora_qml_ui)
  set_source_files_properties(${CMAKE_CURRENT_BINARY_DIR}/oviora-qml-version.rc PROPERTIES
    OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/assets/oviora.ico;${CMAKE_CURRENT_SOURCE_DIR}/apps/oviora/resource.h")
endif()
# P4-c: the QML front end hosts the same layered child subtitle window.
if(TARGET oviora_qml_ui)
  # Copied next to the link step and named relatively: a short link line keeps
  # link.exe off the (ANSI-decoded) response file.
  configure_file(apps/oviora/oviora.manifest ${CMAKE_CURRENT_BINARY_DIR}/oviora-qml.manifest COPYONLY)
  target_link_options(oviora_qml_ui PRIVATE "/MANIFESTINPUT:oviora-qml.manifest")
endif()

# ---------------------------------------------------------------------------
# Headless WebRTC gateway (oviora-gecit.exe). Off by default; never alters the
# desktop application build.
# ---------------------------------------------------------------------------
option(OVIORA_BUILD_GECIT "Build the headless WebRTC gateway (oviora-gecit.exe)" OFF)
if(OVIORA_BUILD_GECIT)
  set(OVIORA_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
  include(cmake/OvioraRemotePlay.cmake)

  find_package(Qt6 6.8 REQUIRED COMPONENTS Core Network)
  find_package(LibDataChannel CONFIG REQUIRED)
  find_package(nlohmann_json CONFIG REQUIRED)

  add_library(oviora_gecit_core STATIC
    apps/oviora-gecit/GatewayConfig.cpp
    apps/oviora-gecit/GatewayConfig.h
    apps/oviora-gecit/DataChannelProtocol.cpp
    apps/oviora-gecit/DataChannelProtocol.h
    apps/oviora-gecit/H264RelayCache.cpp
    apps/oviora-gecit/H264RelayCache.h
    apps/oviora-gecit/IzletMediaCache.cpp
    apps/oviora-gecit/IzletMediaCache.h
    apps/oviora-gecit/IzletBroadcastSession.cpp
    apps/oviora-gecit/IzletBroadcastSession.h
    apps/oviora-gecit/WebRtcGatewaySession.cpp
    apps/oviora-gecit/WebRtcGatewaySession.h
    apps/oviora-gecit/PhpGatewayClient.cpp
    apps/oviora-gecit/PhpGatewayClient.h
    apps/oviora-gecit/GatewaySessionManager.cpp
    apps/oviora-gecit/GatewaySessionManager.h
  )
  set_target_properties(oviora_gecit_core PROPERTIES AUTOMOC ON)
  oviora_apply_warnings(oviora_gecit_core)
  target_compile_options(oviora_gecit_core PRIVATE /utf-8)
  target_compile_features(oviora_gecit_core PUBLIC cxx_std_20)
  target_include_directories(oviora_gecit_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/apps/oviora-gecit
    ${CMAKE_CURRENT_SOURCE_DIR}/include)
  target_link_libraries(oviora_gecit_core PUBLIC
    oviora_base
    oviora_remoteplay_chiaki
    Qt6::Core
    Qt6::Network
    LibDataChannel::LibDataChannel
    nlohmann_json::nlohmann_json
    ws2_32)

  set(OVIORA_RESOURCE_FILENAME "oviora-gecit.exe")
  configure_file(apps/oviora/version.rc.in ${CMAKE_CURRENT_BINARY_DIR}/oviora-gecit-version.rc @ONLY)
  set_source_files_properties(${CMAKE_CURRENT_BINARY_DIR}/oviora-gecit-version.rc PROPERTIES
    OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/assets/oviora.ico;${CMAKE_CURRENT_SOURCE_DIR}/apps/oviora/resource.h")

  add_executable(oviora_gecit
    apps/oviora-gecit/main.cpp
    ${CMAKE_CURRENT_BINARY_DIR}/oviora-gecit-version.rc
  )
  set_target_properties(oviora_gecit PROPERTIES OUTPUT_NAME "oviora-gecit")
  oviora_apply_warnings(oviora_gecit)
  target_compile_options(oviora_gecit PRIVATE /utf-8)
  target_compile_features(oviora_gecit PRIVATE cxx_std_20)
  target_include_directories(oviora_gecit PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
  target_link_libraries(oviora_gecit PRIVATE oviora_gecit_core advapi32)

  add_executable(oviora_gecit_tests
    apps/oviora-gecit/tests_main.cpp
  )
  oviora_apply_warnings(oviora_gecit_tests)
  target_compile_options(oviora_gecit_tests PRIVATE /utf-8)
  target_compile_features(oviora_gecit_tests PRIVATE cxx_std_20)
  target_link_libraries(oviora_gecit_tests PRIVATE oviora_gecit_core)

endif()
