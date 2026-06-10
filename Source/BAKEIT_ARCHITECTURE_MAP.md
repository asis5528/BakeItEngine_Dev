# BakeIT Engine — Architecture Map

**Status:** Step 1 of an ongoing cleanup/restructure effort.
**Purpose:** Honest map of the engine as it exists *today*. No opinions baked in beyond §9 ("pain pointers"). Future steps (target structure, migration plan, execution) will reference this.

---

## 1. Folder layout

```
Source/
├── (top-level loose files: ~20)
├── Imgui/                        # vendored 3rd-party ImGui — leave alone
├── Graphics/
│   └── IMGUI/                    # OUR ImGui wrapper (VulkanImgui)
├── Scene/                        # 2 files: Scene, ScenePipelines
├── PostProcessing/               # PostProcessingManager + VolumeRendering
├── ComputeShaderPipelines/       # GaussianFluidSim, MeshToSdf, Pottery
├── GUI/                          # only EditorGui
└── shaders/
    ├── ObjectShaders/            # rasterization VS/FS
    ├── ComputeShaders/           # .comp files
    └── PostProcessing/           # screen-space VS/FS
```

---

## 2. Class ownership tree

```
Vulkan (main.cpp)                    ← root
├── VulkanBase* base
│   ├── VulkanDevice*
│   ├── SwapChain*
│   ├── VBuffer*
│   └── VulkanDescriptor*
├── Scene* scene  ─── GOD OBJECT (see §3)
├── Loader*
├── VulkanImgui*
├── EditorGui*    (← Scene*, VulkanBase*)
├── ComputeShaderPipelines* clines
│   ├── (← VulkanBase*, Scene*)
│   ├── GaussianFluidSim* (← VulkanBase*, Scene*)
│   ├── MeshToSdf*        (← VulkanBase*, Scene*)
│   └── Pottery*          (← VulkanBase*, Scene*)
└── Scene-owned:
    ├── PostProcessingManager* (← VulkanBase*, Scene*)
    │   └── VolumeRendering*
    └── ScenePipelines*
```

---

## 3. What Scene currently holds (the god-object)

From `Scene.h`:

| Category | Members |
|---|---|
| **Render resources** | pipelines, ptextures, framebuffers, quads, descriptorPool, standardSampler, computeTexture, postprocessingManager, scenePipelines |
| **Scene graph** | objects, meshes, materials, animations, SBOs, rootNode, selectedID |
| **Camera (multiview)** | proj[2], view[2], projNoJitter[2], prevProjForMotion[2], prevViewForMotion[2], prevProjNoJitterForMotion[2], taaJitter, camFovDegrees, camNearPlane, camFarPlane, camPos, motionHistoryValid |
| **Input state** | ButtonPressed, Left/Middle/RightButtonDown, Shift/W/S/A/D/LeftArrow/RightArrowDown, cameraMoveMode (11 bools) |
| **Brush state** | brushData, brushColor, brushUseSphere, brushShapeMode, brushRadius, brushSmoothness, brushZAxis, brushAction, emissiveMode |
| **Render flags** | taaEnabled, renderPassDirty, dispatch, slowZoom, offset |
| **Stats/HUD** | dirtyBlockCount, sdfAtlasUsedBricks/Capacity/Tail/Free/Usage, fps |
| **Lighting** | floorBlobShadows[64], floorBlobBounceColors[64], floorBlobShadowCount |
| **Connection** | base*, viewport_resolution |

**8 unrelated responsibility groups in one class.**

---

## 4. Shader inventory

**`ObjectShaders/`** (rasterization):
- `ObjectShaderVS.vert` + `ObjectBasicFS.frag`
- `ObjectSkinnedVS.vert`
- `ObjectTexturedFS.frag`
- `ObjectVolumetricVS.vert` + `ObjectVolumetricFS.frag` ← SDF cubes (Pottery)
- `ObjectFluidPrensentation.frag` ← **typo: Prensentation**

**`ComputeShaders/`**:
- `MeshtoSdf.comp` ← **lowercase 't', cpp is `MeshToSdf`**
- `sdfCompute.comp`
- `sdfCompute2.comp` ← unclear vs `sdfCompute.comp`
- `sdfCompute _pottery.comp` ← **literal space in filename**
- `sdfCompute_lod1.comp`
- `GaussianFluidSim.comp`
- `GaussianFluidSim - Copy.comp` ← **Windows accidental copy**
- `simple.comp` ← unclear purpose

**`PostProcessing/`**:
- `quad.frag`, `quad.vert` ← **lowercase, no stage suffix**
- `VolumePassVS.vert`, `VolumePassFS.frag`
- `TaaHistoryCopyFS.frag`

---

## 5. `compile.bat` sync issues

**Live shaders NOT being compiled by `compile.bat`:**
- `sdfCompute_lod1.comp` (referenced by Pottery — bug if it ever needs recompile)
- `ObjectFluidPrensentation.frag`
- `simple.comp`
- `quad.frag` / `quad.vert`

Plus the dead `GaussianFluidSim - Copy.comp`.

**Two `compile.bat` files** (root + `Source/`) must stay in sync; easy to drift.

---

## 6. Connection pattern (de facto)

Every subsystem constructor takes `(VulkanBase* base, Scene* scene)`. Inside, code reaches through both:
- `base->vbuffer->createTexturefromBuffer(...)`
- `base->vulkandescriptor->createX(...)`
- `base->swapChain->...`
- `scene->brushData`, `scene->brushAction`, `scene->camPos`, `scene->renderPassDirty`, `scene->dirtyBlockCount`...

Scene is the bulletin board for shared state. No per-subsystem context, no service layer, no event/message bus.

---

## 7. Likely-dead or anachronistic files

| File | Status |
|---|---|
| `SceneObjects.h/.cpp` | Defines a class duplicating Scene's fields. Only self-referenced + project files. **Dead.** |
| `Header.h` | Just `#pragma once`. **Dead.** |
| `PGEResourceTypes.h` | `PGE` prefix from a previous project name. Namespace is live, but the name is anachronistic. |
| `GaussianFluidSim - Copy.comp` | Windows accident. **Dead.** |
| `Imgui/imgui_impl_glut - Copy.cpp` | Windows accident. **Dead.** |
| Dead helpers inside `ObjectVolumetricFS.frag` | See separate cleanup plan (1A–1E). |

---

## 8. Naming inconsistencies (raw list)

| Issue | Examples |
|---|---|
| File case | `quad.cpp`/`quad.h` lowercase vs everything else PascalCase |
| Class purpose ambiguous | `Object` vs `SceneObjects`; `Scene` vs `ScenePipelines`; `ComputeShaderPipelines` class vs same-name folder |
| Shader stage suffix | `ObjectBasicFS.frag` (suffix) vs `quad.frag` (none) |
| Shader file case | `MeshtoSdf.comp` (cpp `MeshToSdf.cpp`) |
| Literal space in filename | `sdfCompute _pottery.comp` |
| Anachronistic prefix | `PGE` namespace throughout |
| Numeric versioning | `sdfCompute.comp` / `sdfCompute2.comp` (which is live?) |
| Folder casing | `Imgui/` (vendored) vs `Graphics/IMGUI/` (ours) |

---

## 9. Pain pointers (observations only)

1. **Scene is doing 8 unrelated jobs.** Camera, input, brush, render resources, gameplay graph, stats, render flags, lighting. The de facto "shared state" mechanism.
2. **`base + scene` as global bus.** Every subsystem reaches through them. No isolation, no scoping, no clear contracts.
3. **`ComputeShaderPipelines` is both a class and a folder with the same name.** Awkward; the class is a thin wrapper that mostly dispatches to its members.
4. **`GUI/` has one file.** Disproportionate folder.
5. **No render-graph / pass abstraction.** `Scene::renderPass` records command buffers; `PostProcessingManager::renderPass` records its own; subsystems each have ad-hoc dispatch.
6. **Shader/file naming has at least 4 conventions in use simultaneously.** No enforced format.
7. **At least 3 dead C++ files**, plus literal "Copy" files, plus inactive shaders not in `compile.bat`.

---

## Next steps in this effort

- **Step 2** (conversation): pick top pain points from §9. Decide principles for the target architecture.
- **Step 3**: draft target structure doc (`BAKEIT_ARCHITECTURE_TARGET.md` or similar).
- **Step 4**: migration plan, broken into small commits.
- **Step 5**: execute, one phase at a time, with engine always runnable.
