#include "ComputeShaderPipelines.h"
#include "Imgui/imgui.h"
#include <chrono>
#include <thread>

struct computeUbo {
    glm::vec4 data;
    float time;
    int Frame;

};


ComputeShaderPipelines::ComputeShaderPipelines(EngineContext& ctx) {
    this->base = ctx.base;
    this->scene = ctx.scene;
    meshtosdf = new MeshToSdf(ctx);
    fluidSim = new GaussianFluidSim(ctx, meshtosdf->computeTexture);
	sdfEditor = new SdfEditor(ctx);
  //  ct3D = fluidSim->computeTexture;
	scene->SBOs.push_back(sdfEditor->computeSBO);
   // scene->ptextures.push_back(fluidSim->computeTexture);
}
ComputeShaderPipelines::~ComputeShaderPipelines() {
    delete meshtosdf;
    delete fluidSim;
    delete sdfEditor;
}

void ComputeShaderPipelines::update(float time) {
    meshtosdf->update(time);
   // glfwSetMouseButtonCallback(base->window, mouse_button_cb);
    //fluidSim->update(time);
 //   if (ButtonPressed ||time<0.01) {
        sdfEditor->update(time);
   // }
}

