# BakeIT Engine — Architecture Map

**Status:** Current state after the 2026-06 restructure (all 7 phases of `BAKEIT_MIGRATION_PLAN.md` executed). Companion docs: `BAKEIT_ARCHITECTURE_TARGET.md` (the design rationale), `BAKEIT_ENGINE_DIRECTION.md` (product north-star).

---

## 1. Folder layout

```
Source/
├── Core/                      # Vulkan foundation. Knows nothing about the game.
│   ├── Vulkan/                # VulkanBase, VulkanDevice, SwapChain, VulkanUtils, Framebuffer
│   ├── Resources/             # VulkanBuffer, VulkanImage, VulkanDescriptor,
│   │                          # ResourceManager, ResourceTypes
│   ├── Pipelines/             # GraphicsPipeline, ComputePipeline, Shader
│   └── Math/                  # MathUtils
├── Platform/
│   └── Desktop/               # Input: the ONLY place GLFW input callbacks live
├── World/                     # Scene (world data), Object, Mesh, Model, Material,
│   │                          # Animation, AnimationData, Loader
│   └── Camera/                # Camera: proj/view[2], free-fly, viewportResolution
├── Features/                  # Vertical slices: each owns all its GPU stages
│   ├── Sdf/            # ★ the SDF research core
│   │   ├── SdfVolume.h        #   public contract: atlas stats (grows as needed)
│   │   ├── Edit/              #   SdfEditor (brush editor) + BrushState contract
│   │   └── Bake/              #   MeshToSdf
│   └── Fluid/                 # GaussianFluidSim
├── Render/                    # Renderer (resources + frame recording),
│   │                          # ScenePipelines (object pipeline registry)
│   └── PostProcess/           # PostProcessingManager → PostProcessPass
│                              # (passthrough quad; effects plug in here), Quad
├── Editor/                    # EditorGui (ImGui dockspace), VulkanImgui
├── Imgui/                     # vendored third-party — leave alone
├── shaders/                   # mirrors features; compiled by recursive compile.bat
│   ├── World/                 # ObjectShaderVS/SkinnedVS/TexturedFS/BasicFS
│   ├── Sdf/            # SdfBrickVS/FS, SdfBrushCS,
│   │                          # SdfLod1CS, MeshToSdfCS
│   ├── Fluid/                 # GaussianFluidSimCS
│   └── Render/                # PostPassVS/FS
├── EngineContext.h            # root wiring struct passed to subsystem ctors
├── ComputeShaderPipelines.*   # thin per-frame compute fan-out (SdfEditor, MeshToSdf)
└── main.cpp                   # Vulkan class: owns everything below
```

## 2. Ownership & wiring

```
Vulkan (main.cpp)
├── VulkanBase* base            (Core: device/swapchain/commands/sync)
├── DesktopInput input          (Platform: owns ALL GLFW input callbacks,
│                                installed before ImGui init so ImGui chains)
├── Camera camera               (World/Camera)
├── Renderer* renderer          (Render: framebuffer, pipelines, post chain,
│                                descriptor pool, renderPassDirty, recording)
├── Scene* scene                (World data + wiring pointers, see §3)
├── EngineContext ctx           {base, scene, renderer, camera, input}
├── Loader* loader              (ctx)
├── EditorGui* editorGui        (ctx; + SdfVolume*/BrushState* from SdfEditor)
├── VulkanImgui* vulkanImgui
└── ComputeShaderPipelines* clines (ctx)
    ├── SdfEditor* (ctx)          — the volumetric brush editor
    ├── MeshToSdf* (ctx)
    └── GaussianFluidSim* (ctx)
```

**Frame loop:** `glfwPollEvents → input.update() → drawFrame { editorGui->run, vulkanImgui->draw, scene->update (camera->update, UBO writes, renderer->update), base->presentQueue, clines->update }` — compute runs **after** present.

## 3. Scene (world data, no longer a god object)

`World/Scene.h` holds: `objects`, `meshes`, `materials`, `animations`, `rootNode`, `selectedID`, `ptextures`, `SBOs`, a `pipelines` mirror (set by `Renderer::init`), and the wiring pointers `input`/`camera`/`renderer`. Its render methods (`init/renderPass/createSceneDescriptor/cleanDescriptors/recreate`) are **one-line forwarders to Renderer** kept for legacy call sites.

Former responsibilities and where they went:
| Was on Scene | Now lives in |
|---|---|
| camera matrices, fov/near/far, viewport resolution | `World/Camera/Camera` |
| 13 raw input bools | `Platform/Desktop/Input` |
| brush state (9 fields) | `Features/Sdf/Edit/BrushState` (owned by SdfEditor) |
| atlas stats (`sdfAtlas*`, dirty blocks) | `Features/Sdf/SdfVolume` (owned by SdfEditor) |
| framebuffers, pipelines, post chain, descriptor pool, sampler, renderPassDirty | `Render/Renderer` |
| TAA jitter + motion history, floor-blob SSGI | **deleted** (experiments removed) |

## 4. Feature contracts

- `Features/Sdf/SdfVolume.h` — atlas stats today; grid constants/buffers
  migrate here when Physics/Fluid become consumers.
- `Features/Sdf/Edit/BrushState.h` — brush params; SdfEditor writes,
  Editor displays + edits color.
- Cross-feature rule: features share **data through contracts**, never include
  each other's internals.

## 5. Known leftovers / intentional slots

- **Aux-worldpos + motion-vector attachments** in `Core/Vulkan/Framebuffer` have
  no consumer (shaders write zeros) — kept deliberately as future G-buffer
  slots for deferred lighting. Do not remove.
- **Brush indicator** was lost with the TAA/SSGI postprocess removal —
  `REMAINDER.txt` item 4; re-add via `PostProcessPass` quad or the volumetric pass.
- `ComputeShaderPipelines` is a thin fan-out; it dissolves into a feature
  registry when a second feature implements the update/record hook shape.
- Scene's forwarder methods retire when call sites migrate to `ctx.renderer`.
- `Object::update()`, `Scene::cleanTextures/cleanMeshes/cleanQuads` are declared
  but unimplemented legacy stubs.

## 6. Conventions

- Includes are **project-root-relative** (`#include "Core/Vulkan/VulkanBase.h"`);
  `$(ProjectDir)` is on the include path. Same-folder includes stay bare.
- Shaders: `<Feature><What><Stage>` with `VS`/`FS`/`CS` suffix; compile.bat is a
  recursive loop — new shader files need no script edit.
- New features: folder under `Features/` + `shaders/<Feature>/`, constructor
  takes `EngineContext&`, register update in the frame loop.
- File moves must update `VulkanEngine.vcxproj` **and** `.filters`.
