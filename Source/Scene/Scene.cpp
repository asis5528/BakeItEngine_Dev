#include "Scene.h"
#include <chrono>
#include <iostream>
#include "Core/Vulkan/VulkanUtils.h"
#include "Core/Resources/ResourceManager.h"

namespace
{
float Halton(uint32_t index, uint32_t base)
{
    float result = 0.0f;
    float invBase = 1.0f / float(base);
    float fraction = invBase;

    while (index > 0) {
        result += float(index % base) * fraction;
        index /= base;
        fraction *= invBase;
    }

    return result;
}

glm::vec2 NextTaaJitter(uint32_t frameIndex, float width, float height)
{
    if (width <= 0.0f || height <= 0.0f) {
        return glm::vec2(0.0f);
    }

    const glm::vec2 halton = glm::vec2(
        Halton((frameIndex % 8u) + 1u, 2u),
        Halton((frameIndex % 8u) + 1u, 3u));
    const float jitterScalePixels = 0.20f;
    const glm::vec2 jitterPixels = (halton - glm::vec2(0.5f)) * jitterScalePixels;

    return glm::vec2(
        (2.0f * jitterPixels.x) / width,
        (2.0f * jitterPixels.y) / height);
}

void ApplyProjectionJitter(glm::mat4& projection, const glm::vec2& jitterNdc)
{
    projection[2][0] += jitterNdc.x*10.;
    projection[2][1] += jitterNdc.y*10.;
}
}

Scene::Scene(VulkanBase* vbase) {
	this->base = vbase;
   
}




void Scene::init()
{
    printf("Setting mouse callback to my function\n");
    //glfwSetWindowUserPointer(base->window, this);
    //glfwSetMouseButtonCallback(base->window, mouse_button_cbb);
    standardSampler = base->vbuffer->createTextureSampler(1);
    
   
    Framebuffer* frameBuffer = new Framebuffer(base->vulkandevice, base->swapChain->swapChainExtent.width, base->swapChain->swapChainExtent.height, base->vulkandevice->getMaxUsableSampleCount(), base->swapChain->swapChainImageFormat, utils::findDepthFormat(base->vulkandevice->physicalDevice),2);
    framebuffers.push_back(frameBuffer);
    postprocessingManager = new PostProcessingManager(base,this,framebuffers[0]);
    scenePipelines = new ScenePipelines(base, framebuffers[0]->renderPass);
    pipelines = scenePipelines->pipelines;
}

void Scene::renderPass() {


    for (size_t i = 0; i < base->commandBuffers.size(); i++) {
        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;

        if (vkBeginCommandBuffer(base->commandBuffers[i], &beginInfo) != VK_SUCCESS) {
            throw std::runtime_error("failed to begin recording command buffer!");
        }

        // Image memory barrier to make sure that compute shader writes are finished before sampling from the texture
        //dont need this if compute isnt rendering realtime,its only for syncing the work between compute and graphics
        /*
        VkImageMemoryBarrier imageMemoryBarrier = {};
        imageMemoryBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        imageMemoryBarrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        imageMemoryBarrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        imageMemoryBarrier.image = computeTexture.image;
        imageMemoryBarrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 1, 1, 0, 1 };
        imageMemoryBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        imageMemoryBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(
            base->commandBuffers[i],
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            0,
            0, nullptr,
            0, nullptr,
            1, &imageMemoryBarrier);
*/
        framebuffers[0]->beginRenderPass(base->commandBuffers[i]);
        



        for (unsigned int j = 0; j < objects.size(); j++) {
            if (objects[j].hasMesh) {
                for (int meshI = 0; meshI < objects[j].meshID.size(); meshI++) {
                    Mesh mesh = meshes[objects[j].meshID[meshI]];
                    vkCmdBindPipeline(base->commandBuffers[i], VK_PIPELINE_BIND_POINT_GRAPHICS, objects[j].shader[meshI].graphicsPipeline->graphicsPipeline);
                    vkCmdBindDescriptorSets(base->commandBuffers[i], VK_PIPELINE_BIND_POINT_GRAPHICS, objects[j].shader[meshI].graphicsPipeline->pipelineLayout, 0, 1, &objects[j].shader[meshI].descriptorSet, 0, nullptr);
                    mesh.draw(base->commandBuffers[i]);
                }


            }
        }

        vkCmdEndRenderPass(base->commandBuffers[i]);

        /*
        framebuffers[1]->beginRenderPass(base->commandBuffers[i]);
        quads[0]->draw(base->commandBuffers[i]);
        vkCmdEndRenderPass(base->commandBuffers[i]);
        */
        postprocessingManager->renderPass(base->commandBuffers[i]);


        base->swapChain->beginRenderPass(base->commandBuffers[i], i);
         //quads[0]->draw(base->commandBuffers[i]);
        vkCmdEndRenderPass(base->commandBuffers[i]);



        if (vkEndCommandBuffer(base->commandBuffers[i]) != VK_SUCCESS) {
            throw std::runtime_error("failed to record command buffer!");
        }
    }

   
}

static float an;

struct FreeCam
{
    glm::vec3 pos = glm::vec3(0, 0, 0);
    float yaw = -90.0f;
    float pitch = 0.0f;
    float roll = 0.0f;
};

void UpdateFreeCamera(
    FreeCam& cam,
    glm::mat4& outView,
    float dx, float dy,            // mouse delta (pixels)
    float moveF, float moveR, float moveU, // movement intent (-1..1)
    float rollInput,               // roll intent (-1..1)
    float dt,
    float mouseSens = 0.1f,
    float moveSpeed = 5.0f,
    float rollSpeed = 90.0f
)
{
    // --- mouse look ---
    cam.yaw += dx * mouseSens;
    cam.pitch += dy * mouseSens;
    cam.pitch = glm::clamp(cam.pitch, -89.0f, 89.0f);
    cam.roll += rollInput * rollSpeed * dt;

    // --- forward vector ---
    glm::vec3 forward;
    forward.x = cos(glm::radians(cam.yaw)) * cos(glm::radians(cam.pitch));
    forward.y = sin(glm::radians(cam.pitch));
    forward.z = sin(glm::radians(cam.yaw)) * cos(glm::radians(cam.pitch));
    forward = glm::normalize(forward);

    // --- basis ---
    glm::vec3 worldUp = glm::vec3(0, 1, 0);
    glm::vec3 right = glm::normalize(glm::cross(forward, worldUp));
    glm::vec3 up = glm::normalize(glm::cross(right, forward));

    if (glm::abs(cam.roll) > 0.0001f) {
        glm::mat4 rollRot = glm::rotate(glm::mat4(1.0f), glm::radians(cam.roll), forward);
        up = glm::normalize(glm::vec3(rollRot * glm::vec4(up, 0.0f)));
        right = glm::normalize(glm::cross(forward, up));
    }

    // --- movement (camera space) ---
    cam.pos += forward * moveF * moveSpeed * dt;
    cam.pos += right * moveR * moveSpeed * dt;
    cam.pos += up * moveU * moveSpeed * dt;

    // --- view matrix ---
    outView = glm::lookAt(cam.pos, cam.pos + forward, up);
}
void Scene::update(float iTime) {
    if (renderPassDirty) {
        renderPass();
        renderPassDirty = false;
    }
   

    static float tmi = 0.0;

    float delTime = iTime - tmi;
    tmi = iTime;

    float time = iTime;
    
  
    for (Animation& animation : animations) {
        animation.boneTransform(time);
    }
#ifndef VR
    ///for non vr////////

    const float viewportWidth = glm::max(viewport_resolution.x, 1.0f);
    const float viewportHeight = glm::max(viewport_resolution.y, 1.0f);
    static uint32_t taaFrameIndex = 0;

    projNoJitter[0] = glm::perspective(glm::radians(camFovDegrees), viewportWidth / viewportHeight, camNearPlane,camFarPlane);
    proj[0] = projNoJitter[0];
    taaJitter = NextTaaJitter(taaFrameIndex++, viewportWidth, viewportHeight);
    ApplyProjectionJitter(proj[0], taaJitter);
    // offset =1.5;
    
    ///for non vr////////
    
    float angle = (sin(iTime/3.)*3.1415)*0.;
 //   float an = iTime*0.;
    double mouseX, mouseY;
    glfwGetCursorPos(base->window, &mouseX, &mouseY);

    float msx = mouseX / base->swapChain->swapChainExtent.width;
    float msy = mouseY / base->swapChain->swapChainExtent.height;
    static float msxold = 0.0;
	static float msyold = 0.0;
	float msxdelta = msx - msxold;
	float msydelta = msy - msyold;
	msxold = msx;
	msyold = msy;
    

    //std::cout << msx<<"\n";
    if(cameraMoveMode){
       
        an += (msx-0.5)*0.03;
        //std::cout << an << "\n";
	}
   // angle = 1.4;
   //camPos = glm::vec3(glm::sin(an) * offset,  glm::cos(an)*0., glm::cos(an ) * offset);
   if (cameraMoveMode) {
	   std::cout << "camPos: " << camPos.x << " " << camPos.y << " " << camPos.z << "\n";
   }
 //  view[0] = glm::lookAt(camPos, glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f));

   float dx = msxdelta* base->swapChain->swapChainExtent.width*4.;
   float dy = -msydelta* base->swapChain->swapChainExtent.height*4.;
   static float forwardKey = 0;
   static float backwardKey = 0;
   static float rightKey = 0;
   static float leftKey = 0;
   float cameraStep = 0.005;
   if (slowZoom) cameraStep = 0.0001;
   forwardKey += WKeyDown ? cameraStep : 0.0;
   backwardKey += SKeyDown ? cameraStep : 0.0;
   forwardKey *= WKeyDown ? 1.0 : 0.0;
   backwardKey *= SKeyDown ? 1.0 : 0.0;
   rightKey += DKeyDown ? cameraStep : 0.0;
   leftKey += AKeyDown ? cameraStep : 0.0;
   rightKey *= DKeyDown ? 1.0 : 0.0;
   leftKey *= AKeyDown ? 1.0 : 0.0;

   float upKey = 0;
   float downKey = 0;
   float moveF = forwardKey - backwardKey; // W/S
   float moveR = rightKey - leftKey;      // D/A
   float moveU = upKey - downKey;      // Space/Shift
   float rollInput = 0.0f;
   rollInput += LeftArrowDown ? 1.0f : 0.0f;
   rollInput -= RightArrowDown ? 1.0f : 0.0f;
   static FreeCam cam;
   if (cameraMoveMode || LeftArrowDown || RightArrowDown) {
       float mouseDx = cameraMoveMode ? dx : 0.0f;
       float mouseDy = cameraMoveMode ? dy : 0.0f;
       UpdateFreeCamera(cam, view[0], mouseDx, mouseDy, moveF, moveR, moveU, rollInput, delTime);
       camPos = cam.pos;
   }

#endif
  //  glm::vec3 camPos;
    UniformBufferObject ubo{};
    glm::mat4 k;

    ubo.view[0] = view[0];
    ubo.proj[0] = proj[0];
    ubo.proj[0][1][1] *= -1;
    ubo.prevProj[0] = motionHistoryValid ? prevProjForMotion[0] : ubo.proj[0];
    ubo.projNoJitter[0] = projNoJitter[0];
    ubo.projNoJitter[0][1][1] *= -1;
    ubo.prevView[0] = motionHistoryValid ? prevViewForMotion[0] : view[0];
    ubo.prevProjNoJitter[0] = motionHistoryValid ? prevProjNoJitterForMotion[0] : ubo.projNoJitter[0];

    ubo.view[1] = view[1];
    ubo.proj[1] = proj[1];
    ubo.proj[1][1][1] *= -1;
    ubo.prevProj[1] = motionHistoryValid ? prevProjForMotion[1] : ubo.proj[1];
    ubo.projNoJitter[1] = projNoJitter[1];
    ubo.projNoJitter[1][1][1] *= -1;
    ubo.prevView[1] = motionHistoryValid ? prevViewForMotion[1] : view[1];
    ubo.prevProjNoJitter[1] = motionHistoryValid ? prevProjNoJitterForMotion[1] : ubo.projNoJitter[1];

    

    ubo1 u{};
    u.time1 = iTime;
    u.time2 = 0;
    u.time3 = 0;
  //  objects[0].finalMatrix = glm::mat4(1.);



    ///for non vr////////
    //objects[0].finalMatrix = glm::translate(objects[0].finalMatrix, glm::vec3(0., 0,2.));
    for (Object& object : objects) {

        if(object.hasMesh){
            if (object.hasAnimation) {

                for (int meshID = 0; meshID < object.meshID.size(); meshID++) {


                    Mesh* mesh = &meshes[object.meshID[meshID]];
                    float k = 1.;
                   
                    SkinnedUniformBufferObject subo{};
                    subo.view[0] = ubo.view[0];
                    subo.proj[0] = ubo.proj[0];
                    subo.prevProj[0] = ubo.prevProj[0];
                    subo.projNoJitter[0] = ubo.projNoJitter[0];
                    subo.prevView[0] = ubo.prevView[0];
                    subo.prevProjNoJitter[0] = ubo.prevProjNoJitter[0];

                    subo.view[1] = ubo.view[1];
                    subo.proj[1] = ubo.proj[1];
                    subo.prevProj[1] = ubo.prevProj[1];
                    subo.projNoJitter[1] = ubo.projNoJitter[1];
                    subo.prevView[1] = ubo.prevView[1];
                    subo.prevProjNoJitter[1] = ubo.prevProjNoJitter[1];
                    
                    subo.model = object.finalMatrix;
                    subo.prevModel = object.PreviousModelMatrix;
                    
                    Animation* animation = &animations[object.animationID];
                    unsigned int meshBoneIndex = 0;



                    for (int i = 0; i < mesh->boneInfo.size(); i++) {
                        subo.boneMatrices[i] = mesh->boneInfo[i].BoneOffset;
                    }
                    for (int i = 0; i < mesh->boneInfo.size(); i++) {
                        std::string boneName = mesh->boneInfo[i].name;
                        for (int j = 0; j < animation->processedMatrices.size(); j++) {
                            if (boneName == animation->processedMatrices[j].name) {
                                glm::mat4 finalMat = animation->processedMatrices[j].transformedBone * mesh->boneInfo[i].BoneOffset;
                                glm::mat4 f(1.0);
                                subo.boneMatrices[i] = finalMat;
                            }
                        }

                    }
                    
                  //  for (int i = 0; i < object.meshID.size(); i++) {
                   
                    subo.model = object.finalMatrix;
                    subo.model = glm::scale(subo.model, glm::vec3(3, 3, 3));
                        object.shader[meshID].ubos[0].map(base->device, sizeof(subo), &subo);
                        if (mesh->texID >= 0) {
                            object.shader[meshID].ubos[1].map(base->device, sizeof(u), &u);
                        }
                        else {
                            BasicUbo bubo{};
                            bubo.color = mesh->color;
                            object.shader[meshID].ubos[1].map(base->device, sizeof(bubo), &bubo);
                        }
                        
                   // }
                }
            }
            else {
                //for (int meshID = 0; meshID < object.meshID.size(); meshID++) {
                   
                    ubo.model = object.ModelMatrix;
                    ubo.prevModel = object.PreviousModelMatrix;

                    for (int i = 0; i < object.meshID.size(); i++) {
                        Mesh* mesh = &meshes[object.meshID[i]];
                        if (object.shader[i].graphicsPipeline != pipelines[4]) {
                            if (mesh->vertices.size() > 100.) {
                                ubo.model = glm::translate(ubo.model, glm::vec3(1., -0.3, 0.));
                            }
                            else {
                                ubo.model = glm::rotate(ubo.model, angle, glm::vec3(0., 1., 0.));
                            }
                        }
                        object.shader[i].ubos[0].map(base->device, sizeof(ubo), &ubo);
                        if (mesh->texID >= 0) {
                            object.shader[i].ubos[1].map(base->device, sizeof(u), &u);
                        }
                        else {
                            BasicUbo bubo{};
                            if (object.shader[i].graphicsPipeline == pipelines[4]) {
                                // Volumetric object base color; per-block emissive comes from block metadata.
                                bubo.color = glm::vec4(1.0f, 0.35f, 0.75f, iTime);
                            } else {
                                // Legacy behavior for non-volumetric basic materials.
                                bubo.color = glm::inverse(ubo.model) * glm::vec4(camPos, 1.);
                                bubo.color.a = iTime;
                            }
                            object.shader[i].ubos[1].map(base->device, sizeof(bubo), &bubo);
                        }
                    }
                }
    }

    /*
    for (int i = 0; i < objects.size(); i++) {
        if (objects[i].hasAnimation) {
            subo.model = objects[i].ModelMatrix;

            for (int i = 0; i < animations[objects[i].animationID].processedMatrices.size(); i++) {
            
            }

            objects[i].shader.ubos[0].map(&base->device, sizeof(subo), &subo);
            objects[i].shader.ubos[1].map(&base->device, sizeof(u), &u);
        }else{
        ubo.model = objects[i].ModelMatrix;
        objects[i].shader.ubos[0].map(&base->device, sizeof(ubo), &ubo);
        objects[i].shader.ubos[1].map(&base->device, sizeof(u), &u);
        }
    }*/
    }

	postprocessingManager->update();
    for (Object& object : objects) {
        object.PreviousModelMatrix = object.ModelMatrix;
    }
    prevViewForMotion[0] = view[0];
    prevViewForMotion[1] = view[1];
    prevProjForMotion[0] = ubo.proj[0];
    prevProjForMotion[1] = ubo.proj[1];
    prevProjNoJitterForMotion[0] = ubo.projNoJitter[0];
    prevProjNoJitterForMotion[1] = ubo.projNoJitter[1];
    motionHistoryValid = true;

}

void Scene::createSceneDescriptor()
{
    unsigned int objectWithMeshSize = objects.size();
    for (Object&object:objects) {
        if (!object.hasMesh) {
            objectWithMeshSize -=1;
        }
        else {
            objectWithMeshSize += object.meshID.size()-1;
        }
    }
    base->vulkandescriptor->createDescriptorPool(objectWithMeshSize,
        {
            VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER
        },
        objectWithMeshSize + 10, &descriptorPool);

    //std::vector<VkDescriptorSetLayout> layouts(objects.size(), base->objectPipeline->descriptorSetLayout);
    std::vector<VkDescriptorSetLayout> layouts;
    for (int i = 0; i < objects.size(); i++) {
        if(objects[i].hasMesh){
            for (int meshID = 0; meshID < objects[i].meshID.size(); meshID++) {
                layouts.push_back(objects[i].shader[meshID].graphicsPipeline->descriptorSetLayout);
            }
            
        }
    }
    std::vector<VkDescriptorSet> descriptorSets = base->vulkandescriptor->createDescriptorSets(objectWithMeshSize, layouts, descriptorPool);
   
    int j = 0;
    for (int i = 0; i < objects.size(); i++) {
        if (objects[i].hasMesh) {
            for (int meshID = 0; meshID < objects[i].meshID.size(); meshID++) {
                objects[i].shader[meshID].descriptorSet = descriptorSets[j];
                j += 1;
            }
        }
    }


    for (size_t i = 0; i < objects.size(); i++) {
        if (objects[i].hasMesh){
            Object* object = &objects[i];
            VulkanTexture tex;
         
          
           for (int meshID = 0; meshID < objects[i].meshID.size(); meshID++) {

             //  int texID = meshes[object->meshID[meshID]].texID;
        
               std::vector<VulkanTexture> descriptorsTextures{};
               Material mat = materials[meshes[object->meshID[meshID]].matID];
               if (mat.type == TexturedMaterial) {
                   MaterialTexturedData *dat = (struct MaterialTexturedData*)mat.materialData;
                  // MaterialTexturedData k = *dat;
                   descriptorsTextures.push_back(ResourceManager::getTexture(ptextures[dat->DiffuseTexture].id));
               }
               base->vulkandescriptor->updateDescriptorSets(object->shader[meshID].descriptorSet, { object->shader[meshID].ubos }, descriptorsTextures, object->shader[meshID].sbos);
           }
        }
    }
	postprocessingManager->createDescriptors();

   rootNode.updateTransoformation(glm::mat4(1.0),objects);

}

void Scene::cleanUniforms()
{
    for (size_t i = 0; i < objects.size(); i++) {
        for (int meshID = 0; meshID < objects[i].meshID.size(); meshID++) {
            for (int j = 0; j < objects[i].shader[meshID].ubos.size(); j++)
            {
                vkDestroyBuffer(base->device, objects[i].shader[meshID].ubos[j].buffer, nullptr);
                vkFreeMemory(base->device, objects[i].shader[meshID].ubos[j].bufferMemory, nullptr);
            }
        }
    }

    cleanDescriptors();
}
void Scene::clean() {
    cleanUniforms();
    
  //  for (VulkanTexture& texture : textures) {
       // texture.destroy(base->device);
        //vkDestroyImageView(base->device, texture.imageView, nullptr);
      //  vkDestroyImage(base->device, texture.image, nullptr);
       // vkFreeMemory(base->device, texture.imageMemory, nullptr);
        //vkDestroySampler(base->device, texture.imageSampler, nullptr);
  //  }

    for (Mesh& mesh : meshes) {
        
        vkDestroyBuffer(base->device, mesh.indexBuffer, nullptr);
        vkFreeMemory(base->device, mesh.indexBufferMemory, nullptr);

        vkDestroyBuffer(base->device, mesh.vertexBuffer, nullptr);
        vkFreeMemory(base->device, mesh.vertexBufferMemory, nullptr);
        if (mesh.hasInstanceIdBuffer && mesh.instanceIdBuffer != VK_NULL_HANDLE) {
            vkDestroyBuffer(base->device, mesh.instanceIdBuffer, nullptr);
            vkFreeMemory(base->device, mesh.instanceIdBufferMemory, nullptr);
        }
    }
  /*
    for (GraphicsPipeline*& pipeline : pipelines) {
        delete pipeline;
    }
    */
    delete scenePipelines;
    /*
    for (Quad*& quad : quads) {
        delete quad->graphicsPipeline;
        delete quad;
    }
    */
    delete postprocessingManager;
    for (Framebuffer*& fb : framebuffers) {
        delete fb;
    }


    for (Material& mat : materials) {
        if (mat.materialData!=NULL) {
            delete mat.materialData;
        }
        
    }
   // vkDestroySampler(base->device, standardSampler, nullptr);
}
void Scene::recreate()
{
   
    VkExtent2D ex;
    ex.width = viewport_resolution.x;
    ex.height = viewport_resolution.y;

    for (Framebuffer*& framebuffer : framebuffers) {

        framebuffer->clean();
        framebuffer->width = ex.width;
        framebuffer->height = ex.height;
        framebuffer->recreate();
    }
    /*
    framebuffers[0]->clean();
    framebuffers[0]->width = viewport_resolution.x;
    framebuffers[0]->height = viewport_resolution.y;
    framebuffers[0]->recreate();
    //framebuffers[0]->colorFormat = VK_FORMAT_B8G8R8A8_SRGB;


    framebuffers[1]->clean();
    framebuffers[1]->width = viewport_resolution.x;
    framebuffers[1]->height = viewport_resolution.y;
    framebuffers[1]->recreate();
    //framebuffers[1]->colorFormat = VK_FORMAT_B8G8R8A8_SRGB;
    */

    scenePipelines->recreate(framebuffers[0]->renderPass, ex);
    /*
    for (GraphicsPipeline* &pipeline : pipelines) {
        pipeline->clean();
        pipeline->recreatePipeline(ex, framebuffers[0]->renderPass);
    }
    */
    //uncomment this when needs actual updating the frame texture
   // quads[0]->textures[0].imageView = quads[0]->fr2->MultisampledColorImage->imageView;
    /*
    for (Quad* &quad : quads) {
        quad->textures[0].imageView = quad->fr2->MultisampledColorImage->imageView;
        quad->graphicsPipeline->clean();
        quad->recreateGraphicsPipeline(ex);
        quad->cleandescriptors();
        quad->createdescriptors();
    }
    */
    postprocessingManager->recreate(ex);
 

    cleanDescriptors();
    createSceneDescriptor();

}
void Scene::cleanDescriptors() {
    
    vkDestroyDescriptorPool(base->device, descriptorPool, nullptr);
}
