


#pragma once
#include "Platform/Platform.h"
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>


#define GLM_FORCE_RIGHT_HANDED
#define GLM_FORCE_RADIANS
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>


#define STB_IMAGE_IMPLEMENTATION
#include <stb/stb_image.h>



#include <openvr.h>

#include <iostream>
#include <fstream>
#include <stdexcept>
#include <algorithm>
#include <chrono>
#include <vector>
#include <cstring>
#include <cstdlib>
#include <cstdint>
#include <array>
#include <optional>
#include <set>
#include <unordered_map>
#include "Core/Math/MathUtils.h"
const uint32_t WIDTH = 800;
const uint32_t HEIGHT = 601;
//for quad to render compute texture
//const std::string MODEL_PATH = "models/nerd.glb";

const std::string MODEL_PATH = "models/ube.glb";

const std::string TEXTURE_PATH = "textures/bounds3.png";
const std::string TEXTURE_PATH2 = "textures/viking_room.png";
const int MAX_FRAMES_IN_FLIGHT = 2;


const std::vector<const char*> validationLayers = {
    "VK_LAYER_KHRONOS_validation"
};

const std::vector<const char*> deviceExtensions = {
    VK_KHR_SWAPCHAIN_EXTENSION_NAME,VK_KHR_MULTIVIEW_EXTENSION_NAME,VK_KHR_MAINTENANCE2_EXTENSION_NAME,VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME,VK_KHR_DEPTH_STENCIL_RESOLVE_EXTENSION_NAME
};

#ifdef NDEBUG
const bool enableValidationLayers = false;
#else
const bool enableValidationLayers = true;
#endif
#include "World/Mesh.h"
#include "Core/Vulkan/VulkanBase.h"
#include "Editor/VulkanImgui.h"

#include "World/Object.h"
#include "Render/PostProcess/Quad.h"
//uncomment to enable VR
 //#define VR
#include "Core/Pipelines/ComputePipeline.h"
#include "World/Scene.h"
#include "World/Loader.h"
#include "Editor/EditorGui.h"
#include <glm/gtx/matrix_decompose.hpp>
#include "Core/Vulkan/VulkanUtils.h"
#include "ComputeShaderPipelines.h"
#include "Core/Resources/ResourceManager.h"
#include "Platform/Desktop/Input.h"
#include "Platform/Desktop/DesktopBrushInput.h"
#include "Render/Renderer.h"
#include "Core/Pipelines/ShaderHotReload.h"
#include "EngineContext.h"



class Vulkan {
public:
    void run() {
        initWindow();
        initVulkan();
        mainLoop();
        cleanup();
    }

private:
    GLFWwindow* window;

    VulkanBase *base;

    Scene* scene;
    bool framebufferResized = false;
    Loader* loader;

    VulkanImgui *vulkanImgui;

    EditorGui *editorGui;

    VkDescriptorPool descriptorPool;

    
    VulkanTexture ct;
    VulkanTexture3D ct3D;
    vr::IVRSystem *sym;

    clock_t current_ticks, delta_ticks;
    clock_t fps = 0;
    std::vector<UBO> computeUBO;
  //  Quad *quad;
    int computeDimension[3] = { 128,128,128 };

    ComputeShaderPipelines *clines;
    DesktopInput input;
    Camera camera;
    Renderer* renderer;
    EngineContext ctx;
    ShaderHotReload* shaderReload = nullptr; // live render-shader reload (desktop)
    void initWindow() {
        glfwInit();

        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
       // glfwWindowHint(GLFW_DECORATED, false);
     //   glfwWindowHint(GLFW_TRANSPARENT_FRAMEBUFFER, 1);
        window = glfwCreateWindow(WIDTH, HEIGHT, "BakeIt Engine", nullptr, nullptr);
        glfwSetWindowUserPointer(window, this);
        glfwSetFramebufferSizeCallback(window, framebufferResizeCallback);
        glfwSetDropCallback(window, dropCallback);
        // All key/mouse-button/scroll callbacks live in Platform/Desktop.
        // Installed before ImGui init so ImGui chain-calls them.
        input.install(window);

    }
    static void dropCallback(GLFWwindow* window, int path_count, const char* paths[]) {

        auto app = reinterpret_cast<Vulkan*>(glfwGetWindowUserPointer(window));

        vkDeviceWaitIdle(app->base->device);
      
     //   for (int i = 0; i < path_count; i++) {
       //     const char* path = paths[i];
       //     app->loadModel(path);
     //   }
        app->loader->load(path_count, paths);
        app->scene->cleanDescriptors();
        app->scene->createSceneDescriptor();
        app->createCommandBuffers();
       // app->modelTraverse(app->scene->rootNode, glm::mat4(1.0));

    }


    static void framebufferResizeCallback(GLFWwindow* window, int width, int height) {
        if (!window) return;

        auto app = reinterpret_cast<Vulkan*>(glfwGetWindowUserPointer(window));
        if (!app) return;
        if (app->base == nullptr) return;
        if (app->base->swapChain == nullptr) return;
        app->framebufferResized = true;
        //if (app->base->swapChain->framebufferResized == nullptr) return;
        //app->base->swapChain->framebufferResized = true;
    }

 
    void initVulkan() {

#ifdef VR
        vr::EVRInitError eError = vr::VRInitError_None;
        sym = vr::VR_Init(&eError, vr::VRApplication_Scene);
        vr::Eye_Left;
#endif

        base = new VulkanBase();

        base->init(window);
        ResourceManager::setVulkanBase(base);
        // ResourceManager& resourceManager = ResourceManager::getInstance();
        scene = new Scene(base);
        scene->input = &input.state;
        scene->camera = &camera;
        renderer = new Renderer(base);
        scene->renderer = renderer;
        ctx.base = base;
        ctx.scene = scene;
        ctx.renderer = renderer;
        ctx.camera = &camera;
        ctx.input = &input.state;
        loader = new Loader(ctx);
        editorGui = new EditorGui(ctx);

        //ResourceManager::addTexture(createTexture(TEXTURE_PATH.c_str()));
        scene->ptextures.push_back(createTexture(TEXTURE_PATH.c_str()));
        //  ResourceManager::addTexture(createTexture(TEXTURE_PATH2.c_str()));
        scene->ptextures.push_back(createTexture(TEXTURE_PATH2.c_str()));
        //scene->textures.push_back(createTexture3D());

        scene->init();

        vulkanImgui = new VulkanImgui(base, window);
        const char* p[] = { MODEL_PATH.c_str() };
        //  loadModel(MODEL_PATH.c_str());
        loader->load(1, p);
        //   scene->materials[0].skinnedPipeline = scene->pipelines[1];
       //    scene->materials[0].type = TexturedMaterial;
          // MaterialTexturedData dat;
         //  dat.DiffuseTexture = 0;
          // scene->materials[0].materialData = &dat;


        // The volumetric pipeline must be assigned BEFORE ComputeShaderPipelines:
        // SdfEditor's constructor locates the editable mesh by this pipeline to
        // create the indirect draw buffer and the GPU draw-list builder.
        scene->objects[0].shader[0].graphicsPipeline = scene->pipelines[4];
        scene->objects[0].ModelMatrix = glm::mat4(1.0f);
        scene->objects[0].finalMatrix = glm::mat4(1.0f);

        clines = new ComputeShaderPipelines(ctx);
        editorGui->sdfVolume = &clines->sdfEditor->volume;
        editorGui->brush = &clines->sdfEditor->brush;
        editorGui->sdfEditor = clines->sdfEditor; // chunks panel (move/weld)
        scene->brush = &clines->sdfEditor->brush; // brush indicator overlay (post pass)
        scene->sdfVolume = &clines->sdfEditor->volume; // post pass reads blockSizeWS for deferred normals
        // Platform brush source: desktop = cursor ray + mouse/keyboard; the
        // Quest entry point wires an OpenXR controller source here instead.
        clines->sdfEditor->brushInput = new DesktopBrushInput(ctx);
        // Object 0 (the SDF brick object) samples a 3D-texture field through its
        // material/diffuse slot - binding 2 (sampler3D texSampler) in SdfBrickFS.
        // BAKEIT_SDF_TEXTURE_ATLAS: the sparse tile pool (texture-native field).
        // Otherwise: the dense fine atlas for the SSBO-vs-texture A/B test
        // (ubo2.color.w selects the FS fetch path). Both are R16F volumes SdfEditor
        // created + registered in scene->ptextures.
        Material material;
        material.type = TexturedMaterial;
        MaterialTexturedData* matData = new MaterialTexturedData();
#ifdef BAKEIT_SDF_TEXTURE_ATLAS
        matData->DiffuseTexture = clines->sdfEditor->tilePoolTextureIndex;
#else
        matData->DiffuseTexture = clines->sdfEditor->fineAtlasTextureIndex;
#endif
        // 2nd texture (binding 3): the SHARED coarse tile pool (all LOD levels) sampled
        // by the brick FS's coarse-LOD path. One pool for the whole pyramid - no
        // per-level re-point; the per-instance level picks its table region in the shader.
        matData->SecondaryTexture = clines->sdfEditor->coarseSharedPoolIndex;
        material.materialData = matData;
        scene->materials.push_back(material);
        scene->meshes[scene->objects[0].meshID[0]].matID = scene->materials.size() - 1;

        scene->objects[0].shader[scene->objects[0].meshID[0]].sbos = {
            scene->SBOs[0][0],
            scene->SBOs[0][1],
            scene->SBOs[0][2],
            scene->SBOs[0][3],
            scene->SBOs[0][5],
            clines->sdfEditor->coarseSharedSBO[1], // binding 9: shared coarse block table
            clines->sdfEditor->coarseSharedSBO[0]  // binding 10: coarse dummy bricks
        };
        base->vbuffer->updateIndirectInstanceList(scene->meshes[scene->objects[0].meshID[0]], {});

        // Second SDF chunk (multi-chunk stage 3): spawned beside the primary
        // chunk with a debug sphere stamp. Must run after the tile-pool
        // material is wired (the cloned proxy inherits its matID) and before
        // the scene descriptor / command buffers are built.
        clines->sdfEditor->spawnChunk(glm::vec3(1.2f, 0.0f, 0.0f),
            /*debugStamp=*/true, /*rebuildScene=*/false); // init-time: descriptors are built below

        scene->createSceneDescriptor();

        

        //implementing compute texture to first object mainly for plane object
        /*
        scene->computeTexture = ct;
        scene->textures.push_back(ct);

        MaterialTexturedData* dat = (struct MaterialTexturedData*)scene->materials[scene->meshes[0].matID].materialData;
        dat->DiffuseTexture = scene->textures.size()-1;
        */

        

        //edit object 0 for 3D texture raymarching,you can comment while doing other stuffs



        createCommandBuffers();
        vulkanImgui->init();

        VulkanTexture guitex;
        guitex.imageSampler = renderer->standardSampler;
        guitex.imageView = renderer->postprocessingManager->framebuffers[renderer->postprocessingManager->bufferSize - 1]->displayImageView;
        guitex.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        editorGui->createdescriptor(guitex);

#ifdef BAKEIT_PLATFORM_DESKTOP
        // Live render-shader reload: edit a .vert/.frag under shaders/ and it
        // recompiles + swaps into the running engine on save (see ShaderHotReload).
        // Pipeline pointers are stable across resize (rebuilt in place), so a
        // one-time registration holds. Scene pipelines + the post-process quad
        // (SdfBrickVS/FS, the object shaders, PostPassVS/FS) are all covered.
        shaderReload = new ShaderHotReload(base->device);
        for (GraphicsPipeline* p : renderer->scenePipelines->pipelines) {
            shaderReload->registerPipeline(p);
        }
        if (renderer->postprocessingManager && renderer->postprocessingManager->postPass) {
            for (Quad* q : renderer->postprocessingManager->postPass->quads) {
                shaderReload->registerPipeline(q->graphicsPipeline);
            }
        }
#endif
    }

    void mainLoop() {
        while (!glfwWindowShouldClose(window)) {
            current_ticks = clock();
            glfwPollEvents();
            input.update();

#ifdef BAKEIT_PLATFORM_DESKTOP
            // Pick up any saved shader edits before recording this frame.
            if (shaderReload && shaderReload->poll()) {
                renderer->renderPassDirty = true;
            }
#endif

            drawFrame();
            delta_ticks = clock() - current_ticks; //the time, in ms, that took to render the scene
            if (delta_ticks > 0)
                fps = CLOCKS_PER_SEC / delta_ticks;
          //  std::cout << fps << std::endl;
        }
        
        vkDeviceWaitIdle(base->device);
    }
    
    void cleanDescriptors() {
        vkDestroyDescriptorPool(base->device, descriptorPool, nullptr);
   }
    void cleanCommandBuffers() {
        vkFreeCommandBuffers(base->device, base->vulkandevice->commandPool, static_cast<uint32_t>(base->commandBuffers.size()), base->commandBuffers.data());
    }

    void cleanupSwapChain() {
      
        
        base->swapChain->cleanSwapChain();
      
       // base->offscreenbuffer->clean();
        //scene->framebuffers[0]->clean();
        vulkanImgui->cleanFrameBuffers();
        cleanCommandBuffers();


        //vkDestroySwapchainKHR(base->device, base->swapChain->swapChain, nullptr);


    }

    void cleanup() {

        vr::VR_Shutdown();
     //   vkDestroyImageView(base->device,ct3D.imageView, nullptr);
       // vkDestroyImage(base->device, ct3D.image, nullptr);
        //vkFreeMemory(base->device, ct3D.imageMemory, nullptr);
        delete shaderReload;
        delete clines;
        ResourceManager::cleanTextures();
        ResourceManager::cleanTextureSamplers();
        scene->clean();;
        delete vulkanImgui;
        delete base;
        
        glfwDestroyWindow(window);

        glfwTerminate();
    }

    void recreateSwapChain() {
        int width = 0, height = 0;
        glfwGetFramebufferSize(window, &width, &height);
        while (width == 0 || height == 0) {
            glfwGetFramebufferSize(window, &width, &height);
            glfwWaitEvents();
        }

        vkDeviceWaitIdle(base->device);

        cleanupSwapChain();
        //base->recreate();
       
        base->recreate();
      
     
        vulkanImgui->recreate();
         scene->recreate();
   
        createCommandBuffers();

        
    }

    ResourceTypes::Texture createTexture(const char* path) {
        VulkanTexture texture;
        int texWidth, texHeight, texChannels;
        stbi_uc* pixels = stbi_load(path, &texWidth, &texHeight, &texChannels, STBI_rgb_alpha);
      
        VkDeviceSize imageSize = texWidth * texHeight * 4;
        VkFormat format = VK_FORMAT_R8G8B8A8_SRGB;
        texture.mipLevels = static_cast<uint32_t>(std::floor(std::log2(std::max(texWidth, texHeight)))) + 1;
        texture.width = texWidth;
        texture.height = texHeight;
        if (!pixels) {
            throw std::runtime_error("failed to load texture image!");
        }
        ResourceTypes::Texture t = base->vbuffer->createTexturefromBuffer(texture, pixels);
        stbi_image_free(pixels);
        return t;

    }

    //VulkanTexture3D createComputeTexture3D() {
    //    VulkanTexture3D texture;
    //    int width = computeDimension[0];
    //    int height = computeDimension[1];
    //    int depth = computeDimension[2];
    //    texture.width = width;
    //    texture.height = height;
    //    texture.depth = depth;
    //    texture.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    //    texture.imageType = VK_IMAGE_TYPE_3D;
    //    texture.imageViewType = VK_IMAGE_VIEW_TYPE_3D;

    //    const uint32_t memSize = width * height * depth * 4;

    //    VulkanImage image = VulkanImage(base->device, base->vulkandevice->physicalDevice);

    //    image.createImage(width, height, depth, VK_IMAGE_TYPE_3D,1, VK_SAMPLE_COUNT_1_BIT, VK_FORMAT_R32G32B32A32_SFLOAT, VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    //   
    //    base->vbuffer->transitionImageLayout(image.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,1);

    //    image.createImageView(VK_FORMAT_R32G32B32A32_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT, 1, VK_IMAGE_VIEW_TYPE_3D);

    //    texture.image = image.image;
    //    texture.imageView = image.imageView;
    //    texture.imageMemory = image.imageMemory;
    //    //MAKE OWN SAMPLER NEXT TIME AIGHT?
    //    texture.imageSampler = scene->textures[0].imageSampler;
    //    return texture;
    //}
    //VulkanTexture createComputeTexture() {
    //    VulkanTexture texture;
    //    int width = 512;
    //    int height = 512;
    //    texture.width = width;
    //    texture.height = height;
    //    texture.imageLayout = VK_IMAGE_LAYOUT_GENERAL;


    //    const uint32_t memSize = width * height  * 4;
    //    uint8_t* pixels = new uint8_t[memSize];
    //    int32_t index = 0;
    //    for (int32_t y = 0; y < height; y++)
    //    {
    //        for (int32_t x = 0; x < width; x++)
    //        {
    //            uint8_t dat = 255;
    //            pixels[index + 0] = 255;
    //            pixels[index + 1] = 0;
    //            pixels[index + 2] = 0;
    //            pixels[index + 3] = 255;
    //            index += 4;
    //        }
    //    }

    //    VkBuffer stagingBuffer;
    //    VkDeviceMemory stagingBufferMemory;

    //    VulkanImage image =  VulkanImage(base->device,base->vulkandevice->physicalDevice);


    //    image.createImage(width, height, 1, VK_IMAGE_TYPE_2D, 1, VK_SAMPLE_COUNT_1_BIT, VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    // 
    //    base->vbuffer->transitionImageLayout(image.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 1);


    //    
    //    image.createImageView(VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT, 1, VK_IMAGE_VIEW_TYPE_2D);

    //    texture.image = image.image;
    //    texture.imageView = image.imageView;
    //    texture.imageMemory = image.imageMemory;
    //    //MAKE OWN SAMPLER NEXT TIME AIGHT?
    //    texture.imageSampler = scene->textures[0].imageSampler;
    //    return texture;
    //}
    
   
    void createCommandBuffers() {

        scene->renderPass();
    }

   
    void updateUniformBuffer(uint32_t currentImage) {
       

    }
   
    static glm::mat4 ConvertSteamVRMatrixToMatrix4(const vr::HmdMatrix34_t& matPose)
    {
        glm::mat4 matrixObj(
            matPose.m[0][0], matPose.m[1][0], matPose.m[2][0], 0.0,
            matPose.m[0][1], matPose.m[1][1], matPose.m[2][1], 0.0,
            matPose.m[0][2], matPose.m[1][2], matPose.m[2][2], 0.0,
           matPose.m[0][3], matPose.m[1][3], matPose.m[2][3], 1.0f
        );
        return matrixObj;
    }
   
    void VRsetup() {
        vr::TrackedDevicePose_t m_rTrackedDevicePose[vr::k_unMaxTrackedDeviceCount];

        vr::VRCompositor()->WaitGetPoses(m_rTrackedDevicePose, vr::k_unMaxTrackedDeviceCount, NULL, 0);



        /////////////////UNCOMMENT FOR VR stuff TEST///////////////////////////
        vr::HmdMatrix44_t mat = sym->GetProjectionMatrix(vr::Eye_Left, 0.001, 10.);


        //   vr::HmdMatrix44_t mat;
        glm::mat4 proj[2];
        proj[0] = glm::mat4(mat.m[0][0], mat.m[1][0], mat.m[2][0], mat.m[3][0],
            mat.m[0][1], mat.m[1][1], mat.m[2][1], mat.m[3][1],
            mat.m[0][2], mat.m[1][2], mat.m[2][2], mat.m[3][2],
            mat.m[0][3], mat.m[1][3], mat.m[2][3], mat.m[3][3]);
        /////////////////UNCOMMENT FOR VR stuff TEST///////////////////////////
        mat = sym->GetProjectionMatrix(vr::Eye_Right, 0.001, 10.);

        proj[1] = glm::mat4(mat.m[0][0], mat.m[1][0], mat.m[2][0], mat.m[3][0],
            mat.m[0][1], mat.m[1][1], mat.m[2][1], mat.m[3][1],
            mat.m[0][2], mat.m[1][2], mat.m[2][2], mat.m[3][2],
            mat.m[0][3], mat.m[1][3], mat.m[2][3], mat.m[3][3]);

        int m_iValidPoseCount = 0;
        std::string m_strPoseClasses = "";
        glm::mat4 m_rmat4DevicePose[vr::k_unMaxTrackedDeviceCount];
        char m_rDevClassChar[vr::k_unMaxTrackedDeviceCount];
        glm::mat4 viewpos;
        for (int nDevice = 0; nDevice < vr::k_unMaxTrackedDeviceCount; ++nDevice)
        {
            if (m_rTrackedDevicePose[nDevice].bPoseIsValid)
            {
                m_iValidPoseCount++;
                m_rmat4DevicePose[nDevice] = ConvertSteamVRMatrixToMatrix4(m_rTrackedDevicePose[nDevice].mDeviceToAbsoluteTracking);
                if (m_rDevClassChar[nDevice] == 0)
                {
                    switch (sym->GetTrackedDeviceClass(nDevice))
                    {
                    case vr::TrackedDeviceClass_Controller:        m_rDevClassChar[nDevice] = 'C'; break;
                    case vr::TrackedDeviceClass_HMD:               m_rDevClassChar[nDevice] = 'H'; break;
                    case vr::TrackedDeviceClass_Invalid:           m_rDevClassChar[nDevice] = 'I'; break;
                    case vr::TrackedDeviceClass_GenericTracker:    m_rDevClassChar[nDevice] = 'G'; break;
                    case vr::TrackedDeviceClass_TrackingReference: m_rDevClassChar[nDevice] = 'T'; break;
                    default:                                       m_rDevClassChar[nDevice] = '?'; break;
                    }
                }
                m_strPoseClasses += m_rDevClassChar[nDevice];
            }
        }

        if (m_rTrackedDevicePose[vr::k_unTrackedDeviceIndex_Hmd].bPoseIsValid)
        {
            viewpos = m_rmat4DevicePose[vr::k_unTrackedDeviceIndex_Hmd];
            glm::mat4 pos = m_rmat4DevicePose[vr::k_unTrackedDeviceIndex_Hmd];
            pos = glm::rotate(pos, -3.1415f / 2.f, glm::vec3(1, 0, 0));

            viewpos = glm::inverse(viewpos);
             viewpos = glm::translate(viewpos, glm::vec3(0., 0, -1.5));

            glm::vec3 position, scale, skew;
            glm::quat rot;
            glm::vec4 pers;
            glm::mat4 k = glm::inverse(viewpos);

            glm::decompose(viewpos, scale, rot, position, skew, pers);
            //std::cout << position.x << " " << position.y << " " << position.z << "\n";
            scene->camera->position = glm::vec3(position.x,position.z,-position.y);
            //  std::cout << position.x << " " << position.y << " " << position.z << "\n";
             // scene->objects[0].finalMatrix = glm::translate(glm::mat4(1.),glm::vec3(position.x,position.y, position.z));

        }
        if (m_rTrackedDevicePose[2].bPoseIsValid) {

            glm::mat4 con = m_rmat4DevicePose[2];
            con = glm::inverse(con);
            con = glm::rotate(con, -3.1415f / 2.f, glm::vec3(1, 0, 0));
            con = glm::inverse(con);
            con = glm::rotate(con, -3.1415f, glm::vec3(1, 0, 0));
            glm::vec3 position, scale, skew;
            glm::quat rot;
            glm::vec4 pers;
            // std::cout<<con
            glm::mat4 mat(1.0);
            mat = glm::translate(mat, glm::vec3(0., 0.2, -4.));
            //mat = glm::translate(mat, glm::vec3(position.x,position.z,position.y));
            //mat = glm::inverse(viewpos);
          //  for (unsigned int j = 0; j < scene->objects.size(); j++) {
           //     if (scene->objects[j].hasMesh) {
            //        scene->objects[j].finalMatrix = con * glm::scale( scene->objects[0].ModelMatrix,glm::vec3(0.2,0.2,0.2));
             //   }
           // }
           // scene->objects[1].finalMatrix = con * glm::scale(scene->objects[0].ModelMatrix, glm::vec3(0.2, 0.2, 0.2));
            for (Object& obj : scene->objects) {


                if (obj.hasMesh) {

                    if (scene->meshes[obj.meshID[0]].bones.size() > 1) {
                     //   obj.finalMatrix = con * obj.ModelMatrix;
                     //   obj.finalMatrix = glm::rotate(obj.finalMatrix, -3.1415f / 1.f, glm::vec3(1, 0, 0));
                       // obj.finalMatrix = glm::scale(obj.finalMatrix, glm::vec3(0.0025, 0.0025, 0.0025));
                    }
                }
            }
            // scene->objects[0].finalMatrix = con *scene->objects[0].ModelMatrix;
             // scene->objects[0].finalMatrix = glm::mat4(0.0);

        }


        glm::mat4 view[2];

        ///////////vr stuff/////////////
        vr::HmdMatrix34_t matEyeRight = sym->GetEyeToHeadTransform(vr::Eye_Left);
        //vr::HmdMatrix34_t matEyeRight;
        view[0] = glm::mat4(matEyeRight.m[0][0], matEyeRight.m[1][0], matEyeRight.m[2][0], 0.0,
            matEyeRight.m[0][1], matEyeRight.m[1][1], matEyeRight.m[2][1], 0.0,
            matEyeRight.m[0][2], matEyeRight.m[1][2], matEyeRight.m[2][2], 0.0,
            matEyeRight.m[0][3], matEyeRight.m[1][3], matEyeRight.m[2][3], 1.0f);

        glm::mat4 seperation = glm::mat4(1.0);
        seperation == glm::translate(seperation, glm::vec3(0.05, 0., 0.));

        view[0] = glm::inverse(view[0]);

        view[0] = view[0] * viewpos;
        view[0] = glm::rotate(view[0], -3.1415f / 2.f, glm::vec3(1, 0, 0));
        // view[0] 

         ///////vr stuff////
        matEyeRight = sym->GetEyeToHeadTransform(vr::Eye_Right);

        view[1] = glm::mat4(matEyeRight.m[0][0], matEyeRight.m[1][0], matEyeRight.m[2][0], 0.0,
            matEyeRight.m[0][1], matEyeRight.m[1][1], matEyeRight.m[2][1], 0.0,
            matEyeRight.m[0][2], matEyeRight.m[1][2], matEyeRight.m[2][2], 0.0,
            matEyeRight.m[0][3], matEyeRight.m[1][3], matEyeRight.m[2][3], 1.0f);
        seperation = glm::mat4(1.0);
        seperation == glm::translate(seperation, glm::vec3(-0.05, 0., 0.));
        view[1] = glm::inverse(view[1]);
        view[1] = view[1] * viewpos;
        view[1] = glm::rotate(view[1], -3.1415f / 2.f, glm::vec3(1, 0, 0));
        //glm::translate(seperation, glm::vec3(-0.05, 0., 0.));

        glm::mat4 transformation; // your transformation matrix.
        glm::vec3 scale;
        glm::quat rotation;
        glm::vec3 translation;
        glm::vec3 skew;
        glm::vec4 perspective;





        scene->camera->proj[0] = proj[0];
        scene->camera->view[0] = view[0];

        scene->camera->proj[1] = proj[1];
        scene->camera->view[1] = view[1];
    }

    void renderVR() {
        vr::VRVulkanTextureArrayData_t vData;

        vData.m_nImage = (uint64_t)renderer->framebuffers[0]->MultisampledColorImage->image;
        vData.m_pDevice = (VkDevice_T*)base->device;
        vData.m_pPhysicalDevice = (VkPhysicalDevice_T*)base->vulkandevice->physicalDevice;
        vData.m_pInstance = (VkInstance_T*)base->vkinstance;
        vData.m_pQueue = (VkQueue_T*)base->queue;
        vData.m_nQueueFamilyIndex = base->vulkandevice->queueID;
        vData.m_nWidth = scene->camera->viewportResolution.x;
        vData.m_nHeight = scene->camera->viewportResolution.y;
        vData.m_nFormat = VK_FORMAT_R8G8B8A8_SRGB;
        vData.m_nSampleCount = 1;
        vData.m_unArraySize = 2;
        vData.m_unArrayIndex = 0;

        /*
        vr::VRVulkanTextureData_t vulkanData;
        vulkanData.m_nImage = (uint64_t)renderer->framebuffers[0]->MultisampledColorImage->image;
        vulkanData.m_pDevice = (VkDevice_T*)base->device;
        vulkanData.m_pPhysicalDevice = (VkPhysicalDevice_T*)base->vulkandevice->physicalDevice;
        vulkanData.m_pInstance = (VkInstance_T*)base->vkinstance;
        vulkanData.m_pQueue = (VkQueue_T*)base->queue;
        vulkanData.m_nQueueFamilyIndex = base->vulkandevice->queueID;
        vulkanData.m_nWidth = scene->camera->viewportResolution.x;
        vulkanData.m_nHeight = scene->camera->viewportResolution.y;
        vulkanData.m_nFormat = base->swapChain->swapChainImageFormat;
        vulkanData.m_nSampleCount = 1;

        */
        vr::Texture_t texture = { &vData, vr::TextureType_Vulkan, vr::ColorSpace_Auto };
        vr::EVRSubmitFlags::Submit_GlArrayTexture;
        vr::VRTextureBounds_t bounds;
        bounds.uMin = 0.0f;
        bounds.uMax = 1.0f;
        bounds.vMin = 0.0f;
        bounds.vMax = 1.0f;
        vr::EVRCompositorError cErr;

        //vr stuff
        cErr = vr::VRCompositor()->Submit(vr::Eye_Left, &texture, &bounds, vr::Submit_VulkanTextureWithArrayData);




        vr::VRVulkanTextureArrayData_t vData_right;

        vData_right.m_nImage = (uint64_t)renderer->framebuffers[0]->MultisampledColorImage->image;
        vData_right.m_pDevice = (VkDevice_T*)base->device;
        vData_right.m_pPhysicalDevice = (VkPhysicalDevice_T*)base->vulkandevice->physicalDevice;
        vData_right.m_pInstance = (VkInstance_T*)base->vkinstance;
        vData_right.m_pQueue = (VkQueue_T*)base->queue;
        vData_right.m_nQueueFamilyIndex = base->vulkandevice->queueID;
        vData_right.m_nWidth = scene->camera->viewportResolution.x;
        vData_right.m_nHeight = scene->camera->viewportResolution.y;
        vData_right.m_nFormat = base->swapChain->swapChainImageFormat;
        vData_right.m_nSampleCount = 1;
        vData_right.m_unArraySize = 2;
        vData_right.m_unArrayIndex = 1;

        vr::Texture_t  texture_right = { &vData_right, vr::TextureType_Vulkan, vr::ColorSpace_Auto };
        vr::EVRSubmitFlags::Submit_GlArrayTexture;
        // vr::VRTextureBounds_t bounds;
        // bounds.uMin = 0.0f;
        // bounds.uMax = 1.0f;
         //bounds.vMin = 0.0f;
        // bounds.vMax = 1.0f;
        // 
        // 
 //         vr STUFF
      //   vr::EVRCompositorError cErr;

        cErr = vr::VRCompositor()->Submit(vr::Eye_Right, &texture_right, &bounds, vr::Submit_VulkanTextureWithArrayData);

        vr::VRCompositor()->PostPresentHandoff();

    }
    void drawFrame() {


        static auto startTime = std::chrono::high_resolution_clock::now();

        auto currentTime = std::chrono::high_resolution_clock::now();
        static float oldTime = 0.;
        float iTime = std::chrono::duration<float, std::chrono::seconds::period>(currentTime - startTime).count();
        float deltaTime = iTime - oldTime;
        oldTime = iTime;

        float fps = 0.0f;
        if (deltaTime > 0.0f) {
            fps = 1.0f / deltaTime;
        }
         static float fpsTimer = 0.0f;
    static int frames = 0;
    frames++;
    scene->fps = frames / fpsTimer;
    fpsTimer += deltaTime;
    if (fpsTimer >= 1.0f) {
        // Console stays quiet: fps lives in the editor panel (FPS2) and the
        // Quest HUD - printing it every second drowns the edit/split logs.
        frames = 0;
        fpsTimer = 0.0f;
    }

       // static float iTime = 0.;
       // iTime += 0.2;
       // std::cout << delta << "\n";
        static int frameid = 0;
    
      //  if (frameid == 0) {
       

      //  vkWaitForFences(base->device, 1, &base->inFlightFences[base->currentFrame],
        //    VK_TRUE, UINT64_MAX);


        uint32_t imageIndex = base->getFrameIndex();
        vulkanImgui->newFrame();
      


        editorGui->run();
        // GUI mouse capture: clicks/drags on ImGui panels must not paint into
        // the scene behind them. The 3D scene itself is an ImGui window (the
        // Viewport image), so "GUI owns the mouse" means WantCaptureMouse AND
        // not hovering the Viewport. SdfEditor polls this via BrushInput and
        // runs AFTER this (clines->update), so the value is current-frame.
        input.state.uiWantsMouse = ImGui::GetIO().WantCaptureMouse && !editorGui->viewportHovered;
        vr::VREvent_t event;
        //sym->PollNextEvent(&event, sizeof(event));


        //IMGUI/////////////
  
        vulkanImgui->draw(imageIndex);

#ifdef VR
        VRsetup();
#endif
        scene->update(iTime);
        //ResourceManager::base;

        updateUniformBuffer(0);

        base->submitCommandBuffers.push_back(vulkanImgui->imguiCommandBuffer[imageIndex]);

      
        VkResult result;
        base->presentQueue(result);
#ifdef VR

        renderVR();
#endif       
        
        if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR || framebufferResized) {
            framebufferResized = false;
            recreateSwapChain();
            editorGui->refreshViewportTexture(); // reuses ds[0] - AddTexture here leaked a set per resize
        }
        else if (result != VK_SUCCESS) {
            throw std::runtime_error("failed to present swap chain image!");
        }
   
         clines->update(iTime);

    }
    
   
};



int main() {
    Vulkan app;
#ifdef BAKEIT_PLATFORM_DESKTOP
    // Dev convenience: recompile every shader at startup (needs glslc on
    // PATH). The Quest build has no shell and no glslc - it ships the
    // precompiled .spv files as APK assets instead.
    system("compile.bat");
#endif
    try {
        app.run();
    }
    catch (const std::exception& e) {
        std::cerr << e.what() << std::endl;
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
