#include "EditorGui.h"

#include "Imgui/imgui.h"
#include "Imgui/imgui_impl_glfw.h"
#include "Imgui/imgui_impl_vulkan.h"
#include "Imgui/imgui_internal.h"
#include "Render/Renderer.h"
#include "Features/Sdf/Edit/SdfEditor.h"
#include <cstdio>

namespace
{
const Mesh* FindFirstVolumetricMesh(const Scene* scene, int* outMeshId = nullptr)
{
    if (!scene || scene->pipelines.size() <= 4) {
        return nullptr;
    }

    for (const Object& obj : scene->objects) {
        if (!obj.hasMesh) {
            continue;
        }

        for (size_t i = 0; i < obj.meshID.size(); ++i) {
            if (i >= obj.shader.size()) {
                continue;
            }
            if (obj.shader[i].graphicsPipeline != scene->pipelines[4]) {
                continue;
            }

            const int meshId = obj.meshID[i];
            if (meshId < 0 || meshId >= static_cast<int>(scene->meshes.size())) {
                continue;
            }

            if (outMeshId) {
                *outMeshId = meshId;
            }
            return &scene->meshes[meshId];
        }
    }

    return nullptr;
}

void DrawSdfAtlasUsageBar(const char* label, float fraction, int usedBricks, int capacityBricks)
{
    fraction = ImClamp(fraction, 0.0f, 1.0f);

    ImGui::Text("%s", label);
    ImVec2 pos = ImGui::GetCursorScreenPos();
    float width = ImGui::GetContentRegionAvail().x;
    float height = 20.0f;
    ImVec2 size(width, height);

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const ImU32 bgColor = IM_COL32(52, 52, 52, 255);
    const ImU32 borderColor = IM_COL32(96, 96, 96, 255);
    const ImVec4 lowColor = ImVec4(0.19f, 0.49f, 0.95f, 1.0f);
    const ImVec4 highColor = ImVec4(0.88f, 0.20f, 0.16f, 1.0f);
    const ImVec4 mixColor = ImLerp(lowColor, highColor, fraction);
    const ImU32 fillColor = ImGui::ColorConvertFloat4ToU32(mixColor);
    const float rounding = height * 0.5f;

    drawList->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), bgColor, rounding);

    float fillWidth = size.x * fraction;
    if (fillWidth > 0.0f) {
        drawList->AddRectFilled(
            pos,
            ImVec2(pos.x + fillWidth, pos.y + size.y),
            fillColor,
            rounding);
    }

    drawList->AddRect(pos, ImVec2(pos.x + size.x, pos.y + size.y), borderColor, rounding, 0, 1.0f);

    char overlay[128];
    std::snprintf(
        overlay,
        sizeof(overlay),
        "%d / %d bricks  (%.1f%%)",
        usedBricks,
        capacityBricks,
        fraction * 100.0f);
    ImVec2 textSize = ImGui::CalcTextSize(overlay);
    ImVec2 textPos(
        pos.x + (size.x - textSize.x) * 0.5f,
        pos.y + (size.y - textSize.y) * 0.5f);
    drawList->AddText(textPos, IM_COL32(255, 255, 255, 255), overlay);

    ImGui::Dummy(size);
}
}


 EditorGui::EditorGui(EngineContext& ctx)
     : m_scene(ctx.scene),
     m_base(ctx.base){
     renderer = ctx.renderer;
}
 void EditorGui::createdescriptor(VulkanTexture &tex) {
     ds.push_back(ImGui_ImplVulkan_AddTexture(tex.imageSampler,tex.imageView,tex.imageLayout));

 }
 void EditorGui::run() {
	 setupStyle();
     setupGui();

     frameID += 1;
 }

 void EditorGui::tree(Node& node) {
     for (int i = 0; i < node.children.size(); i++) {
         auto c = IM_COL32(0, 255, 0, 255);
         if (i & 1) {
             c = IM_COL32(255, 255, 0, 255);
         }
         else {
             c = IM_COL32(0, 255, 0, 255);
         }

         ImGui::PushStyleColor(ImGuiCol_FrameBg, c);
         if (node.children[i].children.size()) {
            
             if (ImGui::TreeNode(m_scene->objects[node.children[i].ObjectID].name.c_str())) {

                 ImGui::TreePop();
                 ImGui::Indent();
                 
                 tree(node.children[i]);
                 ImGui::Unindent();

             }
         }
         else {
             //static bool k = 0;
             ImGui::Indent();
           
             ImGui::Selectable(m_scene->objects[node.children[i].ObjectID].name.c_str(), node.children[i].ObjectID== m_scene->selectedID);
             if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(0))
             {
                 m_scene->selectedID = node.children[i].ObjectID;
             }
             ImGui::Unindent();
         }

         ImGui::PopStyleColor();
     }
 }
 void EditorGui::setupGui() {
     /*
    if(frameID<3){
   m_scene->camera->viewportResolution.x = 512;
   m_scene->camera->viewportResolution.y = 512;
   vkDeviceWaitIdle(m_base->device);
   m_scene->recreate();
   m_scene->renderPass();
   }
   ImGui::Begin("Demo window");
   ImGui::Button("Hello!");
   ImGui::End();


   */
     ImGui::Begin("DockSpace", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoTitleBar);

     ImGui::SetWindowSize(ImVec2(m_base->presentExtent().width, m_base->presentExtent().height));
     ImGui::SetWindowPos(ImVec2(0, 0));

     ImGuiIO& io = ImGui::GetIO();
     ImGuiID dockspace_id = ImGui::GetID("DockSpace");
     ImGui::DockSpace(dockspace_id, ImVec2(0.0f, 0.0f));

     if (frameID == 1) {
         io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;

         ImGui::DockBuilderRemoveNode(dockspace_id); // clear any previous layout
         ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
         ImGui::DockBuilderSetNodeSize(dockspace_id, ImVec2(m_base->presentExtent().width, m_base->presentExtent().height));

         auto dock_id_up = ImGui::DockBuilderSplitNode(dockspace_id, ImGuiDir_Up, 0.2f, nullptr, &dockspace_id);
         auto dock_id_down = ImGui::DockBuilderSplitNode(dockspace_id, ImGuiDir_Down, 0.25f, nullptr, &dockspace_id);

         auto dock_id_left = ImGui::DockBuilderSplitNode(dockspace_id, ImGuiDir_Left, 0.2f, nullptr, &dockspace_id);

         auto dock_id_center = ImGui::DockBuilderGetCentralNode(dockspace_id);

         auto dock_id_right = ImGui::DockBuilderSplitNode(dockspace_id, ImGuiDir_Right, 0.25f, nullptr, &dockspace_id);
         auto dock_id_right2 = ImGui::DockBuilderSplitNode(dockspace_id, ImGuiDir_Right, 0.25f, nullptr, &dockspace_id);


       //  ImGui::DockBuilderDockWindow("tools", dock_id_down);
         ImGui::DockBuilderDockWindow("scene", dock_id_left);
       //  ImGui::DockBuilderDockWindow("scene3", dock_id_right);
       //  ImGui::DockBuilderDockWindow("inspector", dock_id_right2);
       //  ImGui::DockBuilderDockWindow("Demo window", dock_id_up);
         ImGui::DockBuilderDockWindow("Viewport", dock_id_center->ID);
         ImGui::DockBuilderFinish(dockspace_id);
     }
    
     ImGui::End();
     //  ImGui::Begin("damn",0, ImGuiWindowFlags_NoResize|ImGuiWindowFlags_NoMove| ImGuiWindowFlags_NoBringToFrontOnFocus);
      // ImGui::SetWindowSize(ImVec2(base->swapChain->swapChainExtent.width, base->swapChain->swapChainExtent.height));
      // ImGui::SetWindowSize(ImVec2(100,100));
      // ImGui::SetWindowPos(ImVec2(0, 0));
      // ImGui::End();
    // ImGui::dockbu

     ImGuiID id = ImGui::GetID("Viewport");

     ImGui::SetWindowCollapsed(1);
     bool k1 = 1;
     ImGui::Begin("Viewport", &k1, ImGuiWindowFlags_NoTitleBar);
     ImVec2 viewport_size = ImGui::GetWindowSize();

     if (viewport_size.x != m_scene->camera->viewportResolution.x || viewport_size.y != m_scene->camera->viewportResolution.y) {
         m_scene->camera->viewportResolution.x = viewport_size.x;
         m_scene->camera->viewportResolution.y = viewport_size.y;
         vkDeviceWaitIdle(m_base->device);
         m_scene->recreate();
         m_scene->renderPass();
         ds[0] = ImGui_ImplVulkan_AddTexture(renderer->standardSampler, renderer->postprocessingManager->framebuffers[renderer->postprocessingManager->bufferSize-1]->displayImageView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
         //  vkDeviceWaitIdle(base->device);
           //cleanCommandBuffers();
         //  scene->framebuffers[0]->clean();
           //scene->framebuffers[0]->width = k.x;
           //scene->framebuffers[0]->height = k.y;
         //  scene->framebuffers[0]->recreate();


         //  scene->quads[0]->textures[0].imageView = scene->framebuffers[0]->MultisampledColorImage->imageView;
           //createCommandBuffers();

           //vkDeviceWaitIdle(base->device);
           //vkDeviceWaitIdle(base->device);
     }
     auto viewport_node = ImGui::DockBuilderGetCentralNode(dockspace_id);
     if (frameID == 2) {
         viewport_node->WantHiddenTabBarToggle = true;
     }
     ImVec2 viewportPanelSize = ImGui::GetContentRegionAvail();
     //ImGui_ImplVulkan_AddTexture
  //   ImGui::Image(m_scene->quads[0]->descriptorSet, ImVec2{ viewportPanelSize.x, viewportPanelSize.y });
     ImGui::Image(ds[0], ImVec2{ viewportPanelSize.x, viewportPanelSize.y });
     ImGui::End();

   
     
     ImGui::Begin("scene");
	 ImGui::Text("FPS  : %.1f  (%.2f ms avg)", ImGui::GetIO().Framerate, 1000.0f / ImGui::GetIO().Framerate);
	 ImGui::Text("frame: %.2f ms", ImGui::GetIO().DeltaTime * 1000.0f);
     ImGui::Text("FPS2 : %.1f", m_scene->fps);
     tree(m_scene->rootNode);

     ImGui::End();


     ImGui::Begin("scene");
     ImGui::Text("this is for testing");
     if (ImGui::Button("MeshToSDF Dispatch")) {
         m_scene->dispatch = true;
     }
     ImGui::Separator();
     if (brush) {
         ImGui::Text("Brush");
         ImGui::Text("Shape   : %s (U to cycle)", BrushShapeName(brush->shapeMode));
         ImGui::Text("Radius  : %.4f (A/D)", brush->radius);
         ImGui::Text("Smooth  : %.4f (Q/E)", brush->smoothness);
         ImGui::ColorEdit3("Color", &brush->color.x);
         ImGui::Text("Z Offset: %.4f (W/S)", brush->zAxis);
         ImGui::Text("Action  : %s", (brush->action < 0.5f) ? "Remove (RMB)" : "Add (LMB)");
         ImGui::Text("Emissive Paint: %s (I)", brush->emissive ? "On" : "Off");
     }
     if (sdfVolume) {
         const SdfVolumeStats& atlas = sdfVolume->stats;
         ImGui::Text("Dirty Blocks: %d", atlas.dirtyBlockCount);
         DrawSdfAtlasUsageBar(
             "SDF Atlas",
             atlas.usage,
             atlas.usedBricks,
             atlas.capacityBricks);
         const int atlasLeftBricks =
             (atlas.capacityBricks > atlas.usedBricks)
             ? (atlas.capacityBricks - atlas.usedBricks)
             : 0;
         ImGui::Text("Atlas Left: %d bricks", atlasLeftBricks);
         ImGui::Text("Atlas Tail: %d bricks", atlas.tailBricks);
         ImGui::Text("Free Stack: %d bricks", atlas.freeBricks);
     }
     int volumetricMeshId = -1;
     if (const Mesh* volumetricMesh = FindFirstVolumetricMesh(m_scene, &volumetricMeshId)) {
         ImGui::Separator();
         ImGui::Text("Vol Mesh ID: %d", volumetricMeshId);
         ImGui::Text("Vol Indices: %d", static_cast<int>(volumetricMesh->indices.size()));
         ImGui::Text("Vol Instances: %u / %u", volumetricMesh->instanceCount, volumetricMesh->instanceCapacity);
     }
     ImGui::Text("Mode: %s (MMB toggle)", (m_scene->input && m_scene->input->cameraMoveMode) ? "Camera" : "Edit");
     ImGui::Text("Shift+LMB: Recolor existing blocks");
     ImGui::Checkbox("Block Wireframe", &m_scene->showBlockWireframe);
     ImGui::Checkbox("Deferred Shading (test)", &m_scene->deferredLighting);
     {
         // Max coarse level: Off / fine->2 / fine->2->4 (concentric distance bands).
         static const char* coarseItems[] = { "Off", "2x2x2", "4x4x4" };
         const int coarseMap[3] = { 0, 2, 4 };
         int curIdx = 0;
         for (int i = 0; i < 3; ++i) { if (coarseMap[i] == m_scene->coarseLodLevel) { curIdx = i; } }
         if (ImGui::Combo("Coarse LOD (test)", &curIdx, coarseItems, 3)) {
             m_scene->coarseLodLevel = coarseMap[curIdx];
         }
         // Band boundaries (camera distance, block units): fine -> 2 at the first, 2 -> 4
         // at the second (kept >= the first). Nearer than the first stays fine.
         if (m_scene->coarseLodLevel >= 2) {
             ImGui::SliderFloat("LOD dist fine->2", &m_scene->coarseLodDistance, 0.0f, 120.0f);
         }
         if (m_scene->coarseLodLevel >= 4) {
             ImGui::SliderFloat("LOD dist 2->4", &m_scene->coarseLodDistance2, 0.0f, 120.0f);
             if (m_scene->coarseLodDistance2 < m_scene->coarseLodDistance) {
                 m_scene->coarseLodDistance2 = m_scene->coarseLodDistance;
             }
         }
     }
     // Fine field fetch source: SSBO brick atlas vs dense 3D texture (perf A/B).
     // The dense texture re-converts on idle frames while this is on, so flip it
     // and watch the FPS on the same sculpt.
     ImGui::Checkbox("Fine: 3D texture fetch (test)", &m_scene->fineTexFetch);
     // Sculptable-bounds cage (the block region's blue box) - on by default to
     // match the Quest build.
     ImGui::Checkbox("Show Bounds", &m_scene->showChunkBounds);
     ImGui::End();
    
     // Chunk volumes: move a chunk (its proxy object's ModelMatrix
     // translation) and weld it into the primary chunk's field - the desktop
     // stand-in for the future VR grab/place mechanic.
     if (sdfEditor) {
         ImGui::Begin("Chunks");
         for (size_t ci = 0; ci < sdfEditor->chunks.size(); ++ci) {
             SdfChunk& c = sdfEditor->chunks[ci];
             ImGui::PushID(static_cast<int>(ci));
             ImGui::Text("Chunk %d%s%s", static_cast<int>(ci),
                 (static_cast<int>(ci) == sdfEditor->editableChunkIndex) ? " [target]" : "",
                 c.active ? "" : " (absorbed)");
             if (c.active && c.objectIndex >= 0) {
                 glm::mat4& m = m_scene->objects[c.objectIndex].ModelMatrix;
                 float pos[3] = { m[3].x, m[3].y, m[3].z };
                 if (ImGui::DragFloat3("pos", pos, 0.005f)) {
                     m[3] = glm::vec4(pos[0], pos[1], pos[2], 1.0f);
                 }
                 if (ci != 0) {
                     if (ImGui::Button("Weld into chunk 0")) {
                         sdfEditor->weldChunkInto(static_cast<int>(ci), 0);
                     }
                 }
             }
             ImGui::PopID();
             ImGui::Separator();
         }
         ImGui::End();
     }

     // One-click Quest build + deploy (runs on a worker thread; the log
     // streams in while the editor stays interactive).
     ImGui::Begin("Quest");
     const bool deploying = questDeploy.isRunning();
     if (deploying) {
         ImGui::BeginDisabled();
     }
     if (ImGui::Button("Build & Deploy to Quest", ImVec2(-1.0f, 0.0f))) {
         questDeploy.start();
     }
     if (deploying) {
         ImGui::EndDisabled();
     }
     ImGui::Text("Status: %s", questDeploy.status().c_str());
     ImGui::Separator();
     ImGui::BeginChild("questlog", ImVec2(0, 150), true);
     for (const std::string& line : questDeploy.logTail(40)) {
         ImGui::TextUnformatted(line.c_str());
     }
     if (deploying) {
         ImGui::SetScrollHereY(1.0f);
     }
     ImGui::EndChild();
     ImGui::End();

     /*
     ImGui::Begin("inspector");
    if( ImGui::Button("MeshToSDF Dispatch")) {
         m_scene->dispatch = true;
     }
   //  m_scene->objects[m_scene->selectedID].
     //ImGui::DragFloat3("position", &scene.objects[scene.selectionIndex].position[0], 0.1, 0.0, 0.0, "%3f");
     ImGui::End();*/

     /*
     ImGui::Begin("Demo window");
     ImGui::BeginTabBar("List", 1);
     if (ImGui::BeginTabItem("Mesh List")) {
         for (int i = 0; i < m_scene->meshes.size(); i++) {
             ImGui::Selectable(m_scene->meshes[i].name.c_str(), false);
             //  ImGui::Text(scene->meshes[i].name.c_str());
         }
     }
     ImGui::EndTabItem();
     ImGui::EndTabBar();
     ImGui::End();*/

     ImGui::Render();
 }


 void EditorGui::setupStyle() {
     ImGuiStyle* style = &ImGui::GetStyle();
     ImVec4* colors = style->Colors;
     ImFont* font = ImGui::GetFont();
     font->FontSize = 19.;


     colors[ImGuiCol_Text] = ImVec4(1.000f, 1.000f, 1.000f, 1.000f);
     colors[ImGuiCol_TextDisabled] = ImVec4(0.500f, 0.500f, 0.500f, 1.000f);
     colors[ImGuiCol_WindowBg] = ImVec4(0.180f, 0.180f, 0.180f, 1.000f);
     // colors[ImGuiCol_ChildBg] = ImVec4(0.280f, 0.280f, 0.280f, 0.000f);
     colors[ImGuiCol_PopupBg] = ImVec4(0.313f, 0.313f, 0.313f, 1.000f);
     colors[ImGuiCol_Border] = ImVec4(0.266f, 0.266f, 0.266f, 1.000f);
     colors[ImGuiCol_BorderShadow] = ImVec4(0.000f, 0.000f, 0.000f, 0.000f);
     colors[ImGuiCol_FrameBg] = ImVec4(0.160f, 0.160f, 0.160f, 1.000f);
     colors[ImGuiCol_FrameBgHovered] = ImVec4(0.200f, 0.200f, 0.200f, 1.000f);
     colors[ImGuiCol_FrameBgActive] = ImVec4(0.280f, 0.280f, 0.280f, 1.000f);
     colors[ImGuiCol_TitleBg] = ImVec4(0.2, 0.2, 0.2, 1.);
     colors[ImGuiCol_TitleBgActive] = ImVec4(0.4, 0.4, 0.4, 1.);
     colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.4, 0.4, 0.4, 1.);
     colors[ImGuiCol_MenuBarBg] = ImVec4(0.995f, 0.195f, 0.195f, 1.000f);
     colors[ImGuiCol_ScrollbarBg] = ImVec4(0.160f, 0.160f, 0.160f, 1.000f);
     colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.277f, 0.277f, 0.277f, 1.000f);
     colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.300f, 0.300f, 0.300f, 1.000f);
     colors[ImGuiCol_ScrollbarGrabActive] = ImVec4(1.000f, 0.391f, 0.000f, 1.000f);
     colors[ImGuiCol_CheckMark] = ImVec4(1.000f, 1.000f, 1.000f, 1.000f);
     colors[ImGuiCol_SliderGrab] = ImVec4(0.391f, 0.391f, 0.391f, 1.000f);
     colors[ImGuiCol_SliderGrabActive] = ImVec4(1.000f, 0.391f, 0.000f, 1.000f);
     colors[ImGuiCol_Button] = ImVec4(1.000f, 1.000f, 1.000f, 0.000f);
     colors[ImGuiCol_ButtonHovered] = ImVec4(1.000f, 1.000f, 1.000f, 0.156f);
     colors[ImGuiCol_ButtonActive] = ImVec4(1.000f, 1.000f, 1.000f, 0.391f);
     colors[ImGuiCol_Header] = ImVec4(0.313f, 0.313f, 0.313f, 1.000f);
     colors[ImGuiCol_HeaderHovered] = ImVec4(0.469f, 0.469f, 0.469f, 1.000f);
     colors[ImGuiCol_HeaderActive] = ImVec4(0.469f, 0.469f, 0.469f, 1.000f);
     colors[ImGuiCol_Separator] = colors[ImGuiCol_Border];
     colors[ImGuiCol_SeparatorHovered] = ImVec4(0.391f, 0.391f, 0.391f, 1.000f);
     colors[ImGuiCol_SeparatorActive] = ImVec4(0.5f, 0.5f, 0.5f, 1.000f);
     colors[ImGuiCol_ResizeGrip] = ImVec4(1.000f, 1.000f, 1.000f, 0.250f);
     colors[ImGuiCol_ResizeGripHovered] = ImVec4(1.000f, 1.000f, 1.000f, 0.670f);
     colors[ImGuiCol_ResizeGripActive] = ImVec4(0.5f, 0.5f, 0.000f, 1.000f);
     colors[ImGuiCol_Tab] = ImVec4(0.098f, 0.098f, 0.098f, 1.000f);
     colors[ImGuiCol_TabHovered] = ImVec4(0.352f, 0.352f, 0.352f, 1.000f);
     colors[ImGuiCol_TabActive] = ImVec4(0.195f, 0.195f, 0.195f, 1.000f);
     colors[ImGuiCol_TabUnfocused] = ImVec4(0.098f, 0.098f, 0.098f, 1.000f);
     colors[ImGuiCol_TabUnfocusedActive] = ImVec4(0.195f, 0.195f, 0.195f, 1.000f);
     colors[ImGuiCol_DockingPreview] = ImVec4(0.5f, 0.5f, 0.500f, 0.781f);
     colors[ImGuiCol_DockingEmptyBg] = ImVec4(0.180f, 0.180f, 0.180f, 1.000f);
     colors[ImGuiCol_PlotLines] = ImVec4(0.469f, 0.469f, 0.469f, 1.000f);
     colors[ImGuiCol_PlotLinesHovered] = ImVec4(0.5f, 0.5f, 0.500f, 1.000f);
     colors[ImGuiCol_PlotHistogram] = ImVec4(0.586f, 0.586f, 0.586f, 1.000f);
     colors[ImGuiCol_PlotHistogramHovered] = ImVec4(0.5f, 0.5f, 0.000f, 1.000f);
     colors[ImGuiCol_TextSelectedBg] = ImVec4(1.000f, 1.000f, 1.000f, 0.156f);
     colors[ImGuiCol_DragDropTarget] = ImVec4(0.5f, 0.5f, 0.5f, 1.000f);
     colors[ImGuiCol_NavHighlight] = ImVec4(0.5f, 0.5f, 0.500f, 1.000f);
     colors[ImGuiCol_NavWindowingHighlight] = ImVec4(0.5f, 0.5f, 0.500f, 1.000f);
     colors[ImGuiCol_NavWindowingDimBg] = ImVec4(0.000f, 0.000f, 0.000f, 0.586f);
     colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0.000f, 0.000f, 0.000f, 0.586f);

     // style->ChildRounding = 4.0f;
     style->FrameBorderSize = 1.0f;
     // style->FrameRounding = 2.0f;
     style->GrabMinSize = 7.0f;
     // style->PopupRounding = 2.0f;
     // style->ScrollbarRounding = 12.0f;
     style->ScrollbarSize = 13.0f;
     style->TabBorderSize = 1.0f;
     // style->TabRounding = 0.0f;
     // style->WindowRounding = 4.0f;
     style->WindowTitleAlign = ImVec2(0.5, 0.5);

     style->FramePadding = ImVec2(8, 6);
     //  ImGuiID dockspace_id = ImGui::GetID("MyDockSpace");
      // ImGui::DockSpace(dockspace_id, ImVec2(0.0f, 0.0f), ImGuiDockNodeFlags_None);
      //ImGui::DockBuilderAddNode()


 }
