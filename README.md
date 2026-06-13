# BakeIt Engine

BakeIt is a **gameplay-first volumetric food-interaction engine** — a real-time
signed-distance-field (SDF) sculpting core built for a mixed-reality cooking /
cake-decorating game. The player cuts, adds, smooths, and decorates food
(bread, cake, dough, icing, toppings) by dragging a brush through a volumetric
field, with immediate visual feedback and no hitching during the interaction.

It runs cross-platform from one engine core:

- **Windows desktop** — a Vulkan editor with an ImGui dockspace (viewport, scene
  tree, brush/atlas panels) for fast iteration.
- **Meta Quest 3 standalone** — an OpenXR + Vulkan VR app: sculpt with the touch
  controllers, with camera **passthrough** as the mixed-reality background.

> The guiding rule (`BAKEIT_ENGINE_DIRECTION.md`): the interaction path must
> never block on optimization. **Drag = immediate. Release = async settle.
> Idle = heavy cleanup.** If a change makes the brush feel laggier in exchange
> for cleaner data structures, it's the wrong change.

## The SDF sculpting core

The one persistent scene SDF lives in a **sparse brick atlas**:

- The chunk is a 64³ grid of blocks; each surface block owns a **brick** of 9³
  samples (8³ cells + 1 shared padding sample for cross-brick trilinear).
  Fully-interior / empty blocks store a sentinel and no brick.
- Every brush stamp is a complete boolean update — smooth union (`smin`) for
  add, subtraction for remove — applied by a compute shader (`SdfBrushCS`) over
  only the blocks the brush touches. Stamps are final; there is no resolve pass.
- Bricks are **rasterized cube proxies** that the fragment shader sphere-traces
  per pixel (`SdfBrickVS/FS`), writing real depth so volumetric surfaces compose
  with each other. Smooth normals come from cross-brick field sampling.
- Visibility is **fully GPU-driven**: `SdfDrawList` counting-sorts active blocks
  front-to-back and frustum-culls them into an indirect draw list right behind
  each stamp — no CPU readback in the drag path.

## Architecture (`Source/`)

The engine root (`main.cpp`'s `Vulkan` class) owns an `EngineContext`
(`base/scene/renderer/camera/input`); every subsystem takes it and picks what it
needs. See `BAKEIT_ARCHITECTURE_MAP.md` for the detailed current-state map.

- **`Core/`** — Vulkan plumbing: `VulkanBase` (device/swapchain/commands/sync),
  `Resources/` (buffers, images, descriptors), `Pipelines/` (graphics + compute
  wrappers). Platform-agnostic.
- **`Platform/Desktop/Input`** — the only place GLFW input callbacks live;
  consumers read the routed `Input` struct.
- **`World/`** — `Scene` (world data + wiring), `Camera` (multiview proj/view),
  Object/Mesh/Model/Material/Animation, and the Assimp `Loader`.
- **`Features/Sdf/`** — the research core: `Edit/SdfEditor` (the brush editor —
  the heart of BakeIt) + `BrushState`, `Bake/MeshToSdf`, and the `SdfVolume.h`
  data contract. `Features/Fluid/` — a Gaussian fluid sim.
- **`Render/`** — `Renderer` (offscreen object pass → post-process → swapchain),
  `ScenePipelines`, and the `PostProcess/` chain (brush-indicator overlay, HUD).
- **`Editor/`** — `EditorGui` (ImGui dockspace) + `VulkanImgui`.

`Android/` holds the Quest standalone app (Gradle/NDK, OpenXR entry point, XR
action sets for controller sculpting).

## Building & running

### Desktop (Windows)

Two equivalent ways:

- **Visual Studio 2022** (`PlatformToolset v143`): open `BakeItEngine.sln`,
  build `Debug|x64` or `Release|x64`, and run. The debugger working directory
  must be `Source/` (the project sets this) — at startup `main()` runs
  `Source/compile.bat` and loads assets via paths relative to `Source/`.
- **CMake** (portable, all deps bundled in `ThirdParty/`):
  ```
  cmake -S . -B build -G "Visual Studio 17 2022" -A x64
  cmake --build build --config Debug
  ```
  or open `build/BakeItEngine.sln`. Sources are globbed, so new files only need
  a regenerate. The debugger working dir is preset to the repo root.

**Prerequisites:** a Vulkan-capable GPU + recent drivers, the
[Vulkan SDK](https://vulkan.lunarg.com/) with `glslc.exe` on `PATH`, and (for the
VS path) Visual Studio 2022.

### Shaders

`compile.bat` recursively compiles every `*.vert/*.frag/*.comp` under `shaders/`
with `glslc.exe` to SPIR-V next to the source — adding a shader needs no script
edit. `main()` runs it automatically on startup (working dir `Source/`).

### Quest 3

The desktop editor has a one-click **Build & Deploy to Quest** button (shaders →
Gradle `assembleDebug` → `adb install` → launch). It needs the Android NDK,
Gradle, and a Quest in developer mode connected over `adb` (toolchain paths come
from `JAVA_HOME` / `ANDROID_HOME` / `BAKEIT_GRADLE_BAT`).

## Controls

**Desktop:** LMB draws, RMB erases; MMB toggles camera/edit mode; the brush
follows the cursor ray. Brush params are keyed (radius, smoothness, shape,
color), and the brush/atlas panels show live stats.

**Quest:** the right controller is the brush (trigger to stamp); the left stick
drives world zoom / smoothness; grip toggles a block wireframe; passthrough is
the background.

## Status

This is an active research engine — expect rough edges. `BAKEIT_ENGINE_DIRECTION.md`
is the product/architecture north-star; `BAKEIT_ARCHITECTURE_MAP.md` and
`BAKEIT_ARCHITECTURE_TARGET.md` document the current and intended design.

## License

MIT — see [`LICENSE`](LICENSE).
