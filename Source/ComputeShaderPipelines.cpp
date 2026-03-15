#include "ComputeShaderPipelines.h"
#include "Imgui/imgui.h"
#include <chrono>
#include <thread>

struct computeUbo {
    glm::vec4 data;
    float time;
    int Frame;

};


ComputeShaderPipelines::ComputeShaderPipelines(VulkanBase* base,Scene *scene) {
    this->base = base;
    this->scene = scene;
    meshtosdf = new MeshToSdf(base, scene);
    fluidSim = new GaussianFluidSim(base,scene, meshtosdf->computeTexture); 
    pottery = new Pottery(base, scene);
  //  ct3D = fluidSim->computeTexture;
	scene->SBOs.push_back(pottery->computeSBO);
   // scene->ptextures.push_back(fluidSim->computeTexture);
}
ComputeShaderPipelines::~ComputeShaderPipelines() {
    delete meshtosdf;
    delete fluidSim;
    delete pottery;
}

void ComputeShaderPipelines::update(float time) {
    meshtosdf->update(time);
   // glfwSetMouseButtonCallback(base->window, mouse_button_cb);
    //fluidSim->update(time);
 //   if (ButtonPressed ||time<0.01) {
        pottery->update(time);
   // }
}

void ComputeShaderPipelines::createStaticFloorObject() {
    pottery->createStaticFloorObject();
}
