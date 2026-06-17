#pragma once

// ShaderHotReload - desktop-only live reload of RENDER shaders.
//
// Watches the .vert/.frag SOURCE of every registered graphics pipeline. When a
// source's modification time changes, it recompiles just that file with glslc
// (the same tool/output layout as compile.bat), rebuilds the affected
// pipeline(s) and reports back so the caller can re-record command buffers. A
// failed compile is printed and the previous pipeline is kept running - no
// crash, no half-built pipeline.
//
// Limitation: this is a shader-MODULE swap only; the pipeline layout and
// descriptor set layout are reused. Editing a shader's resource *bindings*
// (adding a UBO/SSBO, changing a binding number or stage) needs a restart, not
// a reload. Compute shaders (SdfBrushCS etc.) are intentionally out of scope.

#include <vulkan/vulkan.h>
#include <filesystem>
#include <string>
#include <vector>

class GraphicsPipeline;

class ShaderHotReload
{
public:
    explicit ShaderHotReload(VkDevice device);

    // Watch a pipeline's vertex + fragment sources. Call once after the
    // pipeline is set up; the pointer must stay valid for the watcher's life
    // (the engine's resize path rebuilds pipelines in place, so this holds).
    void registerPipeline(GraphicsPipeline* pipeline);

    // Poll watched sources once per frame. Returns true if at least one
    // pipeline was reloaded, in which case the caller must flag the render
    // command buffers dirty (renderPassDirty) so they re-record with the new
    // VkPipeline handles.
    bool poll();

private:
    struct Watch
    {
        std::string source;                       // shaders/.../Name.frag
        std::string spv;                          // shaders/.../Name.spv
        std::filesystem::file_time_type stamp{};
        bool stampValid = false;                  // first sighting seeds the baseline
        std::vector<GraphicsPipeline*> pipelines; // every pipeline using this source
    };

    VkDevice device;
    std::vector<Watch> watches;

    void watchSource(const std::string& spvPath, const char* sourceExt, GraphicsPipeline* pipeline);
};
