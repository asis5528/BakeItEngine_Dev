# BakeIT Engine — Target Architecture

**Status:** Step 3 of the cleanup/restructure effort. Companion to `BAKEIT_ARCHITECTURE_MAP.md` (current state) and `BAKEIT_ENGINE_DIRECTION.md` (product north-star).

---

## 1. What this engine must support

From the owner's stated goals:

- **Research playground now**: sparse SDF rendering is the active research thrust; structure must make experiments cheap to start and cheap to throw away.
- **Real game later (Quest)**: BakeIT — bake cakes, cut them smoothly, icing/frosting; cut physics (severed half falls as its own body, still SDF-rendered); fluid simulation (cocktail mixing, egg → cooked omelette solidification); possibly gaussian splatting.
- **Features inherently cooperate across GPU stages**: e.g. compute writes sparse voxel bricks → rasterized cube proxies → fragment-shader raymarch reads the same bricks. Compute and fragment are *one feature*, not two.
- **OpenXR/Quest port** must be a platform swap, not an engine rewrite.

## 2. Core principles

### P1 — Vertical features, not technology layers
A feature folder contains **everything** that feature needs: its compute shaders, its raster shaders, its C++ pipeline objects, its GPU buffers. Never split a feature across "compute land" and "raster land". Pottery already proves this shape: brush compute + cube raymarch FS are one system.

### P2 — Features share DATA, never internals
Cross-feature cooperation happens through **owned data with a public contract**:

- `Volumetric` *owns* the sparse brick storage (atlas texture/SSBO, block table, free lists, extents) and exposes it via one small header (`SdfVolume.h`): buffer handles, formats, grid constants, and a few mutation entry points.
- `Fluid` includes `SdfVolume.h` to solidify an omelette into the SDF. It never includes Pottery's edit internals.
- `Physics` includes `SdfVolume.h` to query distance/cut the field. The two cake halves after a cut are just two chunk-sets, each attached to a rigid body, still rendered by Volumetric's renderer.
- `Splatting` owns its own splat storage; render hooks compose it into the frame.

Dependency arrows always point **feature → another feature's public data header**, never into another feature's `Edit/`/internals. If two features need to touch the same data, the owner exposes it; no third "shared dumping ground".

### P3 — Core knows nothing about the game
`Core/` is Vulkan plumbing: device, swapchain, command buffers, sync, buffer/texture/sampler creation, descriptor helpers, pipeline builders, math. It must compile without any mention of SDF, brushes, food, or scenes. This is the slow-changing foundation; everything above it is allowed to churn.

### P4 — Platform is swappable from day one
All GLFW / window / mouse-keyboard code lives in `Platform/Desktop/`. The rest of the engine consumes an input/window interface, not `<GLFW/glfw3.h>`. The existing multiview (`proj[2]/view[2]`) camera stays — it is the seed of the XR path. Quest port = implement `Platform/XR/` (OpenXR session, swapchain images, per-eye views), leave features untouched.

### P5 — Adding a feature is additive
A new research direction (cloth, smoke, splatting, …) requires:
1. New folder `Features/<Name>/` + `shaders/<Name>/`.
2. Implement the feature hook points (see §4).
3. Register it in the frame loop + compile.bat.
Nothing else in the engine is edited. If the experiment dies, delete the folder.

### P6 — Sandbox is allowed to be filthy
`Sandbox/` is for prototypes with zero structural obligations. Graduation path: Sandbox → `Features/<Name>/` when it earns it. Nothing in `Features/` or `Core/` may include from `Sandbox/`.

### P7 — The interaction rule still reigns
From `BAKEIT_ENGINE_DIRECTION.md`: drag = immediate, release = async phased settle, idle = heavy cleanup. The restructure must never trade brush responsiveness for cleanliness. Migration steps that touch Pottery must preserve the phased-pipeline flags and the drag/release/idle split exactly.

## 3. Target folder layout

```
Source/
├── Core/                       # P3. Vulkan foundation. Slow-changing.
│   ├── Vulkan/                 # instance, device, swapchain, sync, command buffers
│   │                           #   (today: VulkanBase, VulkanDevice, SwapChain)
│   ├── Resources/              # buffer/texture/sampler creation + ResourceManager
│   │                           #   (today: VulkanBuffer, VulkanImage, PGEResourceTypes)
│   ├── Pipelines/              # GraphicsPipeline / ComputePipeline / descriptor helpers
│   └── Math/                   # MathUtils etc.
│
├── Platform/                   # P4. OS / window / input / XR.
│   ├── Desktop/                # GLFW window, mouse/kb, drop callback
│   └── XR/                     # (future) OpenXR session, Quest swapchain, controllers
│
├── World/                      # CPU-side scene: what exists.
│   ├── Object.h / Node         # scene graph
│   ├── Mesh / Material / Animation / Loader (Assimp)
│   └── Camera/                 # multiview proj/view, TAA jitter, orbit controls
│
├── Features/                   # P1/P2. Vertical slices. The playground.
│   ├── Volumetric/             # ★ active research core: sparse SDF
│   │   ├── SdfVolume.h/.cpp    #   PUBLIC CONTRACT: brick atlas, block tables,
│   │   │                       #   free lists, grid constants, mutation API
│   │   ├── Bake/               #   MeshToSdf (mesh → field)
│   │   ├── Edit/               #   Pottery brush; phased resolve/scan/LOD-rebuild;
│   │   │                       #   snapshot chunks; drag/release/idle policies
│   │   └── Render/             #   cube-proxy instancing, raymarch pipelines,
│   │                           #   LOD selection, normals
│   ├── Fluid/                  # GaussianFluidSim today; cocktails/egg sims later.
│   │                           #   Writes into SdfVolume to "solidify" (omelette).
│   ├── Physics/                # (future) rigid + soft + SDF cutting/fracture.
│   │                           #   Reads SdfVolume to cut; produces bodies whose
│   │                           #   chunks Volumetric/Render draws.
│   └── Splatting/              # (future) gaussian splatting: own storage + hooks.
│
├── Render/                     # Frame composition (NOT a feature).
│   ├── FrameGraph-lite:        #   pass ordering, offscreen → post → present
│   ├── PostProcess/            #   PostProcessingManager, Quad, Framebuffer, TAA
│   └── VolumePass/             #   screen-space volume raymarch (VolumeRendering)
│
├── Editor/                     # ImGui dockspace, panels, viewport, stats HUD
│   └── (today: GUI/EditorGui + Graphics/IMGUI/VulkanImgui merged here)
│
├── Sandbox/                    # P6. Messy experiments. May be empty.
│
├── shaders/                    # mirrors Features/ — ALL stages co-located
│   ├── Core/                   #   shared .glsl includes (sampling, noise, math)
│   ├── World/                  #   basic / skinned / textured raster shaders
│   ├── Volumetric/             #   pottery .comp + lod .comp + cube VS/FS together
│   ├── Fluid/
│   └── Render/                 #   quad/post/TAA/volume-pass shaders
│
├── Imgui/                      # vendored third-party — untouched
└── main.cpp                    # Engine root: owns Core, Platform, World,
                                # feature registry, Render, Editor; frame loop
```

## 4. How things connect (replacing the `base* + scene*` bus)

### EngineContext instead of Scene-as-bulletin-board
A slim per-frame context struct passed to features:

```
EngineContext {
    CoreContext&   core;      // device, allocator, descriptor helpers
    Camera&        camera;    // proj/view[2], jitter, camPos
    Input&         input;     // buttons/keys (from Platform)
    FrameInfo      frame;     // time, dt, frameIndex, viewport extent
    Flags&         flags;     // renderPassDirty etc. (shrinks over time)
}
```

`Scene` stops being the god object: scene-graph data moves to `World/`, camera to `World/Camera`, input to `Platform/`, brush state into `Features/Volumetric/Edit`, atlas stats into `SdfVolume`, render resources into `Render/`. **This happens gradually** — Scene shrinks field-by-field across migration commits; it is never deleted in one step.

### Feature hook points
Each feature implements (informally at first — a common base class only if it proves useful):

- `update(ctx)` — CPU-side per-frame logic
- `recordCompute(cmd, ctx)` — compute dispatches (today: after present, keep that)
- `recordDraw(cmd, ctx)` — draws into the offscreen pass
- `onResize(extent)` — plug into the existing recreate path
- `drawEditorUI()` — its own ImGui panel section (stats, toggles)

The frame loop in `main.cpp` iterates registered features. This is exactly how `ComputeShaderPipelines::update` already fans out to Pottery/Fluid/MeshToSdf — formalized and extended to draws.

### Mixed rasterized + SDF world (one world, per-object representation)

There is **one** world, not a "raster world" and an "SDF world". Every entity is a
`World` Object with a transform plus *optional* representation handles:

```
Object {
    transform / scene-graph parent
    meshID       // present → drawn by raster mesh pipeline   (table, spoon)
    sdfChunkID   // present → drawn by Volumetric/Render       (cake, dough)
    bodyID       // present → simulated by Features/Physics    (either kind)
}
```

Three integration contracts — and only these — connect the representations:

1. **World transforms.** Identity + placement is shared; renderers and physics are
   consumers keyed by the optional handles. A cut-off cake half = new Object with a
   new `sdfChunkID` (chunk subset) + new `bodyID`.
2. **Shared depth buffer.** Raster meshes and SDF cube-proxies draw into the *same*
   offscreen pass; the raymarch FS writes `gl_FragDepth` at the hit point, so
   occlusion between a rasterized spoon and an SDF cake "just works". No renderer-
   to-renderer data exchange exists or is needed.
3. **Physics colliders.** Physics defines bodies + collider variants:
   `Box/Plane/ConvexMesh` (table, walls) and `SdfCollider` (wraps `SdfVolume.h`:
   distance = penetration, gradient = contact normal — SDFs are an ideal collision
   representation). Raster-table-vs-SDF-cake collision is box-vs-SDF; physics
   writes resolved transforms back to World via `bodyID`. Rendering uninvolved.

Rule of thumb: objects that never need boolean/deformation (table) stay mesh-only;
objects that get sculpted/cut/cooked are SDF; both can carry physics bodies.

### Input routing (GLFW single-callback problem)

GLFW provides exactly one callback per event type per window, so **only
`Platform/Desktop` registers GLFW callbacks**. Everything else consumes a routed
`Input` state:

```
GLFW callbacks (Platform/Desktop only)
   ↓ record into
Input { keys/buttons pressed-held-released, mouse pos/delta, scroll }
   ↓ routed each frame, in priority order
1. ImGui        — io.WantCaptureMouse/WantCaptureKeyboard → consumed, stop
2. Editor       — computes viewport rect: stamps input.viewportHovered +
                  viewport-local mouse coords onto Input
3. Features     — camera orbit + brush read input.viewportMouse via
                  EngineContext, act only when viewportHovered
```

This replaces Scene's 11 raw input bools, fixes window-space-vs-viewport-space
mouse coordinate bugs, and means "input for a specific area" is provided by the
routing layer (the docking UI knows the rects), not by GLFW. The XR port replaces
stage 1–2 with controller poses feeding the same `Input`/context shape.

### Worked examples (the game scenarios)

| Scenario | Data flow |
|---|---|
| Sculpt/icing a cake | `Volumetric/Edit` (brush compute) writes bricks → `Volumetric/Render` raymarches them. One feature, all internal. |
| Cut cake, half falls | `Physics` reads `SdfVolume`, splits chunk ownership into two bodies; per-body transform feeds `Volumetric/Render` instancing. Physics never touches render code. |
| Egg → omelette | `Fluid` simulates particles/density; on "cooked", writes solidified field through `SdfVolume` mutation API; from then on it's ordinary volumetric content. |
| Cocktail mixing | Pure `Fluid` + its own render hook (or volume pass). No other feature edited. |
| Gaussian splatting | New `Features/Splatting/` with own storage + `recordDraw`. Additive (P5). |
| Quest port | Implement `Platform/XR/`; camera already multiview; features unchanged (P4). |

## 5. Naming conventions (target)

- **C++ files**: PascalCase, one class per file, file = class name.
- **Shader files**: `<Feature><What><Stage>.<ext>` with stage suffix — `VolumetricCubeVS.vert`, `VolumetricCubeFS.frag`, `VolumetricBrushCS.comp`, `PostTaaResolveFS.frag`. Fix during migration: `sdfCompute _pottery.comp` (space), `MeshtoSdf.comp` (case), `ObjectFluidPrensentation.frag` (typo), `quad.frag` (no suffix), `GaussianFluidSim - Copy.comp` (delete).
- **compile.bat**: one canonical script that compiles `shaders/**` by folder glob, so new shaders never need a manual list entry; keep the root/Source pair in sync or collapse to one.
- **Namespaces**: retire `PGE` → `bakeit` (or none); decide before mass-renaming.
- **Dead files**: delete `Header.h`, `SceneObjects.h/.cpp`, `Imgui/imgui_impl_glut - Copy.cpp` early — zero-risk wins.

## 6. What we are deliberately NOT building

- No render graph framework, ECS, DI container, plugin loader, or message bus. Solo research engine; structs and direct calls win.
- No premature interface/ABC hierarchy for features — start with convention, formalize only when a third feature repeats the pattern.
- No big-bang rewrite. Engine stays runnable after every migration commit (Step 4 defines the commit sequence).

## 7. Next: Step 4 — migration plan

Ordered, each commit leaves the engine running:
1. Zero-risk deletions + shader renames + compile.bat consolidation.
2. Create folder skeleton; move `Core/` files (pure mechanical moves + include fixes).
3. Carve `Platform/Desktop` out of main.cpp/Scene input state.
4. Extract `SdfVolume.h` contract from Pottery/Scene atlas state.
5. Move features into `Features/`; split shaders into mirrored folders.
6. Shrink Scene field-group by field-group into World/Camera/EngineContext.
7. Formalize feature hooks once two features use them.

(Detailed commit-by-commit plan to be written as Step 4.)
