# BakeIt Engine

BakeIt Engine is an experimental Vulkan-based rendering and baking engine. It focuses on GPU-driven workflows such as volume rendering, mesh-to-SDF conversion, and fluid simulation, wrapped in a small editor-like UI built with ImGui.

The project is Windows-centric and targets Vulkan-capable GPUs.

## Features

- **Vulkan rendering backend**
  - Swapchain management, MSAA framebuffers, and graphics pipelines (`Graphics/`)
  - Support for multi-sampled offscreen rendering and presentation
- **Scene system**
  - `Scene` manages meshes, objects, materials, animations, framebuffers, and descriptor sets
  - Camera with simple orbit controls and adjustable near/far planes
  - Skeletal animation support and per-object materials
- **GPU compute pipelines** (`ComputeShaderPipelines`)
  - Gaussian fluid simulation
  - Mesh-to-SDF baking
  - Pottery-style experimentation pipeline
  - Compute results are written into textures/buffers and fed back into the rendering/post-processing stack
- **Post-processing & volume rendering**
  - `PostProcessingManager` chains post-process passes
  - `VolumeRendering` performs volume raymarching via screen-space quads
- **ImGui-based editor UI**
  - Docking layout with a resizable **Viewport** that displays the rendered scene
  - **Scene** panel with hierarchy tree and FPS readout
  - Tools such as a "MeshToSDF Dispatch" button to trigger compute workflows
- **Hot-reload-ish shader compilation**
  - On startup, `main` runs `compile.bat` to compile GLSL shaders to SPIR-V

## High-level architecture

### Entry point (`Source/main.cpp`)

- Defines the `Vulkan` application class.
- On `run()` it:
  1. Creates a GLFW window titled **"BakeIt Engine"**.
  2. Sets input callbacks (framebuffer resize, mouse buttons, scroll, drag-and-drop).
  3. Initializes the Vulkan backend (`VulkanBase`).
  4. Creates the `Scene`, `Loader`, `EditorGui`, `VulkanImgui`, and `ComputeShaderPipelines` instances.
  5. Compiles shaders via `compile.bat` (from the repository root) before the main loop.
- The main loop:
  - Polls events
  - Records and submits command buffers via `Scene::renderPass()` and post-processing
  - Updates ImGui and the editor UI
  - Presents the final image and (optionally) submits textures to OpenVR if VR is enabled at compile time

### Rendering core (`Source/Graphics/`)

Key components:

- `VulkanBase`
  - Creates the Vulkan instance, device, surface, and swapchain
  - Manages command buffers, synchronization primitives, and presentation
  - Owns helper classes like `VulkanDevice`, `SwapChain`, `VulkanBuffer`, `VulkanDescriptor`, and `Framebuffer`
- `GraphicsPipeline`
  - Wraps the creation of Vulkan graphics pipelines for different material/shader combinations
- `ComputePipeline`
  - Encapsulates compute pipelines used by `ComputeShaderPipelines`

### Scene system (`Source/Scene/Scene.*`)

The `Scene` class is responsible for tying the rendering pieces together:

- Holds collections of:
  - `meshes`, `materials`, `objects`, `animations`
  - post-processing framebuffers
  - structured buffer objects (`SBOs`) and textures (`ptextures`)
- Manages:
  - Camera state (`proj`, `view`, `camPos`, `offset`, near/far planes)
  - A scene graph root node (`rootNode`) and selected object ID
  - Descriptor pools and descriptor sets for all renderable objects
- Main methods:
  - `init()`
    - Creates the main offscreen framebuffer
    - Initializes `PostProcessingManager` and `ScenePipelines` and wires up default graphics pipelines
  - `renderPass()`
    - Records command buffers for each swapchain image
    - Begins the offscreen render pass and draws all objects
    - Runs the post-processing chain (including `VolumeRendering`)
    - Finally, draws into the swapchain render pass for presentation
  - `update(float time)`
    - Updates animation bones
    - Computes camera matrices (perspective + orbit camera for non-VR builds)
    - Writes per-object uniform buffers (model/view/projection, material parameters, etc.)
    - Updates post-processing state

### Post-processing (`Source/PostProcessing/`)

- `PostProcessingManager`
  - Owns a list of full-screen `Quad` objects and intermediate `Framebuffer` instances
  - Receives the main scene framebuffer and sets up additional passes
  - Provides `renderPass`, `createDescriptors`, `update`, and `recreate` for when the viewport size changes
- `VolumeRendering`
  - Handles 3D volume rendering using screen-space quads
  - Maintains its own UBOs and framebuffers
  - Is driven by `PostProcessingManager` and fed by textures/SBOs from the rest of the engine

### Compute pipelines (`Source/ComputeShaderPipelines.*`)

- `ComputeShaderPipelines`
  - Central place for configuring and updating all compute workloads
  - Owns instances of:
    - `GaussianFluidSim`
    - `MeshToSdf`
    - `Pottery`
  - Its `update(float time)` method is called every frame from `main.cpp` to advance simulations and baking
- Compute shaders typically:
  - Write into 2D/3D textures or buffers
  - Those resources are then bound in graphics/post-processing passes (e.g., for volume rendering or surface shading)

### Editor GUI (`Source/GUI/EditorGui.*`)

- Integrates ImGui with Vulkan (via `ImGui_ImplVulkan` and `ImGui_ImplGlfw`).
- Sets up a dockspace layout with:
  - A central **Viewport** window that shows the last post-processed framebuffer as an ImGui image
  - A **scene** window that:
    - Displays current FPS (both from ImGui IO and the engine's own measurement)
    - Shows a hierarchical tree of `Scene::objects` using `rootNode`
    - Lets you select objects in the hierarchy
  - A small tool area with buttons, e.g. **"MeshToSDF Dispatch"** to trigger compute work in the scene
- Responds to viewport size changes by:
  - Updating `Scene::viewport_resolution`
  - Recreating framebuffers and pipelines
  - Rebuilding ImGui descriptor sets pointing at the updated framebuffer

### Loader (`Source/Loader.*`)

- Wraps loading of external assets into the scene.
- The GLFW drag-and-drop callback in `main.cpp` forwards dropped file paths to `Loader::load`.
- After loading, the scene refreshes its descriptors and command buffers so the new content is visible.

## Controls & interaction

These are the default, engine-side controls (some may be adjusted in code):

- **Camera orbit**
  - Click and hold the **middle mouse button** in the viewport to orbit the camera around the origin.
- **Zoom**
  - Use the **mouse wheel** to zoom in and out; the scroll callback modifies the scene's `offset` (with an optional slow zoom mode).
- **Selection**
  - Use the **scene** panel to select objects in the hierarchy; clicking an item sets `Scene::selectedID`.
- **MeshToSDF Dispatch**
  - In the tools panel, press **"MeshToSDF Dispatch"** to trigger the mesh-to-SDF compute pipeline for the current scene state.
- **Drag & drop**
  - Drag supported 3D model files (e.g. `.glb`) into the window to load them via `Loader`.

## Building & running

### Prerequisites

- Windows
- A Vulkan-capable GPU and recent drivers
- [Vulkan SDK](https://vulkan.lunarg.com/) installed (with `glslc.exe` available)
- Visual Studio 2019 or later (solution file is `BakeItEngine.sln`)

### Shader compilation

There are two batch scripts for shaders:

- `compile.bat` (repository root)
  - Calls `C:/VulkanSDK/1.2.189.2/Bin/glslc.exe` on several top-level shaders and outputs SPIR-V into `shaders/*.spv`.
  - **Note:** edit this batch file if your Vulkan SDK is installed in a different location.
- `Source/compile.bat`
  - Uses `glslc.exe` from your `PATH`.
  - Recursively compiles all `.vert`, `.frag`, and `.comp` files under `Source/shaders` into `.spv` files next to the originals.

On startup, `main.cpp` runs `system("compile.bat")`, so by default the root script is used.

### Steps

1. Install the Vulkan SDK and ensure `glslc.exe` is either:
   - At the path expected by the root `compile.bat`, or
   - In your `PATH` if you prefer using `Source/compile.bat` manually.
2. Open `BakeItEngine.sln` in Visual Studio.
3. Set the configuration to `Debug|x64` or `Release|x64`.
4. Build the solution; the main project is `VulkanProject`.
5. Run the application from Visual Studio.

You should see the ImGui-based editor layout, the viewport rendering the default scene, and the FPS counter updating.

## Project structure (selected)

- `BakeItEngine.sln` – Visual Studio solution.
- `Source/`
  - `main.cpp` – entry point and main loop.
  - `Graphics/` – Vulkan backend (device, swapchain, framebuffers, pipelines, buffers, descriptors).
  - `Scene/` – scene graph, objects, materials, pipelines, and rendering logic.
  - `PostProcessing/` – post-processing chain and volume rendering.
  - `ComputeShaderPipelines/` – GPU compute simulations/baking.
  - `GUI/` – ImGui-based editor UI.
  - `Imgui/` – Dear ImGui sources and Vulkan/GLFW backends.
  - Other engine components (`Loader`, `Material`, `Object`, `SceneObjects`, etc.).
- `shaders/` and `Source/shaders/` – GLSL shaders compiled to SPIR-V.

## Notes

- VR support via OpenXR exists behind a compile-time `#define VR` in `main.cpp`; it is disabled by default.
- Many features are experimental; expect rough edges and incomplete tools.
- This README (`readme2.md`) is meant as a high-level guide; consult the source for implementation details and current behavior.
