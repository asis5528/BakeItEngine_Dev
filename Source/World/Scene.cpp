#include "Scene.h"
#include <chrono>
#include <iostream>
#include "Core/Vulkan/VulkanUtils.h"
#include "Core/Resources/ResourceManager.h"
#include "Render/Renderer.h"

Scene::Scene(VulkanBase* vbase) {
	this->base = vbase;

}

// Legacy forwarders: render resources and recording live in Render/Renderer.
void Scene::init()
{
    renderer->init(this);
}

void Scene::renderPass() {
    renderer->recordCommandBuffers(*this);
}

void Scene::update(float iTime) {
    if (renderer->renderPassDirty) {
        renderPass();
        renderer->renderPassDirty = false;
    }
   

    static float tmi = 0.0;

    float delTime = iTime - tmi;
    tmi = iTime;

    float time = iTime;
    
  
    for (Animation& animation : animations) {
        animation.boneTransform(time);
    }
    float angle = (sin(iTime/3.)*3.1415)*0.;
#ifndef VR
    camera->update(*input, delTime,
        float(base->presentExtent().width),
        float(base->presentExtent().height));
#endif
    UniformBufferObject ubo{};
    glm::mat4 k;

    ubo.view[0] = camera->view[0];
    ubo.proj[0] = camera->proj[0];
    ubo.proj[0][1][1] *= -1;

    ubo.view[1] = camera->view[1];
    ubo.proj[1] = camera->proj[1];
    ubo.proj[1][1][1] *= -1;

    

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

                    subo.view[1] = ubo.view[1];
                    subo.proj[1] = ubo.proj[1];

                    subo.model = object.finalMatrix;

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
                                bubo.color = glm::inverse(ubo.model) * glm::vec4(camera->position, 1.);
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

	renderer->update();
}

void Scene::createSceneDescriptor()
{
    renderer->createSceneDescriptor(*this);
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
        if (mesh.indirectDrawBuffer != VK_NULL_HANDLE) {
            vkDestroyBuffer(base->device, mesh.indirectDrawBuffer, nullptr);
            vkFreeMemory(base->device, mesh.indirectDrawBufferMemory, nullptr);
        }
    }

    renderer->clean();

    for (Material& mat : materials) {
        if (mat.materialData!=NULL) {
            delete mat.materialData;
        }
        
    }
   // vkDestroySampler(base->device, standardSampler, nullptr);
}
void Scene::recreate()
{
    renderer->recreate(*this);
}
void Scene::cleanDescriptors() {
    renderer->cleanDescriptors();
}
