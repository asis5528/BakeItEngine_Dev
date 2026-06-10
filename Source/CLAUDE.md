# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build & run

- **Build**: open `../BakeItEngine.sln` in Visual Studio 2022 (PlatformToolset `v143`) and build the `BakeItEngine` project (`Source/VulkanEngine.vcxproj`). Configurations are `Debug|x64` / `Release|x64`. Command-line build works too: `MSBuild.exe ..\BakeItEngine.sln /p:Configuration=Debug /p:Platform=x64` (MSBuild from VS2022 Community, locatable via vswhere).
- **Run**: launch from Visual Studio. The exe's working directory must be the repo root (parent of `Source/`) — at startup `main()` calls `system("compile.bat")` and the engine loads assets via relative paths like `models/ube.glb`, `textures/...`, `shaders/.../*.spv`.
- **Tests**: there are no automated tests.

## Shader compilation

Both batch scripts recursively compile every `*.vert/*.frag/*.comp` under `shaders/` with `glslc.exe` — **adding a new shader file requires no compile.bat edit**:

- `../compile.bat` (repo root) is canonical; `main()` runs it automatically at startup.
- `Source/compile.bat` is the same loop for invoking manually from inside `Source/`.

`glslc.exe` is expected on `PATH`. SPIR-V outputs sit next to the `.vert/.frag/.comp` sources (same basename, `.spv` extension, gitignored); the C++ code loads the `.spv` files directly. Shader naming convention: `<Feature><What><Stage>` with stage suffix `VS`/`FS`/`CS` (e.g. `VolumetricBrushCS.comp`, `ObjectVolumetricFS.frag`).

## Engine direction (read this before changing the edit/render path)

`BAKEIT_ENGINE_DIRECTION.md` is the product/architecture north-star. The engine is a **gameplay-first volumetric food-interaction game**, not a sculpting/CAD/SDF-baker tool. The single hardest rule:

> The interaction (brush drag) path must never block on optimization. Drag = immediate, possibly approximate. Release = async settle. Idle = heavy cleanup.

Practical consequences when editing the Pottery / SDF / LOD code paths:

- Drag path: only local compute dispatch around the brush, broad/preview instance coverage, shader-side block validity. No full-chunk CPU rebuild, no strict surface scan, no full `lod1` rebuild, no compact-instance rebuild.
- Release path: full resolve, strict surface scan, fine-active-id rebuild, lod1 rebuild — but **phased over multiple frames**, never one blocking step.
- Idle path: atlas/brick compaction, free-list cleanup, lod refinement.
- "Recompute extent", "storage extent", and "render extent" are different sets — don't conflate them.

If a change makes the brush feel laggier in exchange for cleaner data structures, it's the wrong change.

## Architecture

Entry point is `Source/main.cpp`'s `Vulkan` class. `run()` does `initWindow → initVulkan → mainLoop → cleanup`. `mainLoop` per frame: `glfwPollEvents → drawFrame`. `drawFrame` orchestrates `editorGui->run()`, `vulkanImgui->draw()`, `scene->update(iTime)`, `base->presentQueue()`, then `clines->update(iTime)` (compute pipelines run **after** present).

Top-level systems owned by `Vulkan`:

- `VulkanBase` (`Core/Vulkan/VulkanBase.*`) — instance, device, surface, swapchain, command buffers, sync. Owns `VulkanDevice`, `SwapChain`, `VBuffer` (`VulkanBuffer`), `VulkanDescriptor`. `recreate()` handles swapchain rebuild on resize.
- `Scene` (`Scene/Scene.*`) — the central state: `objects`, `meshes`, `materials`, `animations`, `ptextures`, `SBOs`, `framebuffers`, `quads`, descriptor pool, camera (`proj[2]/view[2]` — array-of-2 because the renderer is multiview-capable for VR), TAA jitter state, brush state (`brushData`, `brushColor`, `brushShapeMode`, `brushAction`, etc.), and gameplay flags (`renderPassDirty`, `dirtyBlockCount`, `sdfAtlas*`).
  - `Scene::renderPass()` records command buffers per swapchain image: offscreen pass draws all objects → post-process chain → swapchain pass for present.
  - `Scene::update(time)` updates animation bones, builds camera matrices (orbit camera in non-VR), writes per-object UBOs, applies TAA jitter via Halton sequence, updates post-processing.
- `PostProcessingManager` (`Render/PostProcess/`) — chain of full-screen `Quad`s + intermediate `Framebuffer`s. Owns `VolumeRendering` which raymarches the SDF/volume textures via screen-space quads. `recreate()` on viewport resize.
- `ComputeShaderPipelines` (`Source/ComputeShaderPipelines.*`) — owns `GaussianFluidSim`, `MeshToSdf`, and **`Pottery`** (the volumetric brush editor — the heart of BakeIT). Its `update(time)` is dispatched once per frame after present.
- `EditorGui` + `VulkanImgui` (both in `Editor/`) — ImGui dockspace with a Viewport that displays the last post-process framebuffer as an `ImGui_ImplVulkan` texture, a Scene hierarchy panel (drives `Scene::selectedID` via `rootNode` tree), FPS, and tool buttons (e.g. "MeshToSDF Dispatch"). On viewport resize it triggers framebuffer + pipeline recreation and rebuilds the ImGui descriptor pointing at the new image.
- `Loader` (`World/Loader.*`) — Assimp-backed model loader (`assimp-vc143-mtd.dll`). The GLFW drop callback in `main.cpp` forwards dropped paths to `Loader::load`, then calls `cleanDescriptors → createSceneDescriptor → createCommandBuffers` to refresh the scene.

### Pottery (volumetric SDF editor)

`Features/Volumetric/Edit/Pottery.*` is where almost all SDF/volumetric edit logic lives. Key concepts to keep straight:

- **Editable chunk** vs **snapshot (settled) chunks** — different performance policies. The editable chunk prioritizes drag responsiveness; snapshots can be compacted aggressively and LOD'd. `snapshotChunks[]` and `snapshotChunkSBOs[]` hold settled work; `chunkBlocksPerAxis`, `chunkBlockDim`, `chunkBlockSizeWS` describe the live chunk grid.
- **Brick atlas** — fine-resolution SDF storage. Tracked via `sdfAtlasUsedBricks/CapacityBricks/TailBricks/FreeBricks/Usage` on `Scene` (and surfaced in the GUI).
- **Block sets** — `strictScanBlockState`, `strictScanRenderableIds`, `fineActiveIds`, `lod1ActiveIds`. These are *different sets* (recompute / store / render); do not unify them.
- **Phased pipeline flags** — `pendingFullChunkResolve`, `pendingStrictSurfaceScan`, `pendingLod1Rebuild`, `lod1RebuildSubmitted`, `strictSurfaceScanCursor`. The brush release path sets these and `Pottery::update` chips at them across frames. Anything new that mutates the chunk should plug into this phasing rather than stalling.
- **LOD** — `rebuildLod1`, `updateLodSelection`, `coarseLodSelected`, `lod1ObjectIndex/MeshIndex`. LOD selection lives in compute; rebuild must not run in the drag path.
- Compute shaders: `shaders/Volumetric/VolumetricBrushCS.comp` and `VolumetricLod1CS.comp` are Pottery's GPU side.

## Conventions & gotchas

- **Coordinate system & matrix flags**: GLM is configured globally in `main.cpp` with `GLM_FORCE_RIGHT_HANDED`, `GLM_FORCE_DEPTH_ZERO_TO_ONE`, `GLM_FORCE_RADIANS`. Don't fight these.
- **Multiview / VR**: camera matrices are `glm::mat4 proj[2]/view[2]`. Index 0 is used for non-VR; index 1 is for the right eye. VR is compiled out unless `#define VR` is uncommented in `main.cpp`. `VR_KHR_multiview` is enabled at the device level.
- **Resize path**: any change that affects framebuffers / pipelines / descriptors must work through `Scene::recreate` and `PostProcessingManager::recreate`, plus rebuild the ImGui viewport descriptor in `editorGui->ds[0]`. The drop callback uses a similar but *not identical* refresh sequence — both must stay coherent.
- **Texture loading**: `stb_image` is single-translation-unit (`STB_IMAGE_IMPLEMENTATION` in `main.cpp`); don't redefine.
- **ResourceManager** is a static-style global (`ResourceManager::setVulkanBase`, `ResourceManager::cleanTextures`). Created textures and samplers tracked here are cleaned at shutdown — keep new long-lived GPU resources registered there or clean them yourself.
- **`REMAINDER.txt`** holds informal author TODOs (postprocessing quad refactor, sampler destruction, viewport texture refresh unification). Treat as low-priority hints, not specs.
