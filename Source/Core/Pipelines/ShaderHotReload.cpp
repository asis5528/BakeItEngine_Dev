#include "ShaderHotReload.h"
#include "GraphicsPipeline.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>

namespace
{
// Replace the extension after the file name's last '.' (ext includes the
// leading dot, e.g. ".vert"). Slashes are respected so a dotted directory is
// never mistaken for an extension.
std::string withExtension(const std::string& path, const char* ext)
{
    size_t slash = path.find_last_of("/\\");
    size_t dot = path.find_last_of('.');
    bool hasExt = dot != std::string::npos && (slash == std::string::npos || dot > slash);
    std::string base = hasExt ? path.substr(0, dot) : path;
    return base + ext;
}
}

ShaderHotReload::ShaderHotReload(VkDevice device)
    : device(device)
{
}

void ShaderHotReload::watchSource(const std::string& spvPath, const char* sourceExt, GraphicsPipeline* pipeline)
{
    std::string source = withExtension(spvPath, sourceExt);
    for (Watch& w : watches) {
        if (w.source == source) {
            // Shared source (e.g. ObjectShaderVS.vert feeds several pipelines):
            // reload all of them on one edit.
            w.pipelines.push_back(pipeline);
            return;
        }
    }
    Watch w;
    w.source = source;
    w.spv = withExtension(spvPath, ".spv"); // spvPath is already .spv; normalize anyway
    w.pipelines.push_back(pipeline);
    watches.push_back(std::move(w));
}

void ShaderHotReload::registerPipeline(GraphicsPipeline* pipeline)
{
    if (!pipeline) return;
    if (!pipeline->pipelineInput.vertexShaderpath.empty()) {
        watchSource(pipeline->pipelineInput.vertexShaderpath, ".vert", pipeline);
    }
    if (!pipeline->pipelineInput.fragmentShaderpath.empty()) {
        watchSource(pipeline->pipelineInput.fragmentShaderpath, ".frag", pipeline);
    }
}

bool ShaderHotReload::poll()
{
    std::vector<GraphicsPipeline*> toReload;

    for (Watch& w : watches) {
        std::error_code ec;
        auto t = std::filesystem::last_write_time(w.source, ec);
        if (ec) continue; // source not on disk (unreadable / mid-write) - skip this poll

        if (!w.stampValid) {
            // First sighting: seed the baseline so the startup compile.bat
            // build is not treated as an edit.
            w.stamp = t;
            w.stampValid = true;
            continue;
        }
        if (t == w.stamp) continue;

        // Advance the stamp now so a file that keeps failing to compile is
        // retried on the next *save*, not re-attempted every frame.
        w.stamp = t;

        // glslc.exe is on PATH (same assumption as compile.bat); the engine's
        // working directory is Source/, so the relative paths resolve and the
        // .spv lands next to its source - exactly where the loader reads it.
        std::string cmd = "glslc.exe \"" + w.source + "\" -o \"" + w.spv + "\"";
        int rc = std::system(cmd.c_str());
        if (rc != 0) {
            std::cerr << "[shader hot-reload] compile FAILED: " << w.source
                      << " - keeping previous shader\n";
            continue;
        }
        std::cout << "[shader hot-reload] reloaded " << w.source << "\n";
        for (GraphicsPipeline* p : w.pipelines) {
            if (std::find(toReload.begin(), toReload.end(), p) == toReload.end()) {
                toReload.push_back(p);
            }
        }
    }

    if (toReload.empty()) return false;

    // A reloaded pipeline may still be bound by in-flight command buffers;
    // wait for the GPU to drain before destroying the old VkPipeline.
    vkDeviceWaitIdle(device);
    for (GraphicsPipeline* p : toReload) {
        p->reloadShaders();
    }
    return true;
}
