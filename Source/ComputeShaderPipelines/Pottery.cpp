#include "Pottery.h"
#include "../ResourceManager.h"
#include <algorithm>
#include <cmath>
#include <vector>

struct alignas(16) potteryUBO {
	glm::vec4 BrushData;
	glm::vec4 BrushColor;
	float BrushAction; // 1.0 add, 0.0 remove
	float time;
	float BrushShape; // 0.0 cube, 1.0 sphere, 2.0 plane
	float BrushEmissive; // 1.0 emissive paint mode on, 0.0 off
	float BrushRecolor; // 1.0 recolor existing blocks
	float _pad0 = 0.0f;
	float _pad1 = 0.0f;
	

};

Pottery::Pottery(VulkanBase* base, Scene* scene)
{
	this->base = base;
	this->scene = scene;
	//int size = 64*8;

	int blocksPerAxis = 64;
	int blockDim = 8;
	int size = blocksPerAxis * blockDim; // 64 * 8 = 512

	computeTexture = base->vbuffer->createTexture3DGeneral(size, size , size );
	ComputeInput input;
	input.computeShaderPath = "shaders/ComputeShaders/sdfCompute _pottery.spv";
	input.descriptorTypes = { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE ,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER };
	comp = new ComputePipeline(base, input);
	computeUBO = base->vbuffer->createUniformBuffers({ sizeof(potteryUBO) });
	
	int numBlocks = blocksPerAxis * blocksPerAxis * blocksPerAxis; // 64^3 = 262144
	int bricksPerBlock = 9 * 9 * 9;                                     // 729
	int totalBrickFloats = numBlocks * bricksPerBlock;
	computeSBO = base->vbuffer->createStorageBuffers({ sizeof(float) *512*512*512, sizeof(int) * numBlocks,sizeof(int),sizeof(glm::vec4) * numBlocks, sizeof(int) });
	int zeroInit = 0;
	computeSBO[4].map(base->device, sizeof(int), &zeroInit);
	comp->setupDescriptors(computeUBO, { ResourceManager::getTexture(computeTexture.id) }, computeSBO);
	int bx = size / 8;
	comp->dispatch(bx, bx, bx);
	//glfwSetMouseButtonCallback(base->window, mouse_button_cb);
}
Pottery::~Pottery() {
	delete comp;
	for (UBO& ubo : computeUBO) {
		vkDestroyBuffer(base->device, ubo.buffer, nullptr);
		vkFreeMemory(base->device, ubo.bufferMemory, nullptr);
	}
	for (SBO& sbo : computeSBO) {
		vkDestroyBuffer(base->device, sbo.buffer, nullptr);
		vkFreeMemory(base->device, sbo.bufferMemory, nullptr);
	}
	//computeTexture.destroy(base->device);
	//vkDestroySampler(base->device, computeTexture.imageSampler, nullptr);
}

static bool KeyPressed = false;
static bool WKeyDown = false;
static bool SKeyDown = false;
static bool AKeyDown = false;
static bool DKeyDown = false;
static bool LeftArrowDown = false;
static bool RightArrowDown = false;
static bool AltKeyDown = false;
static bool ShiftKeyDown = false;
static int BrushShapeMode = 0; // 0 cube, 1 sphere, 2 plane
static bool EmissiveMode = false;
static void key_cb(GLFWwindow* win, int key, int scancode, int action, int mods)
{
	const char* act = (action == GLFW_PRESS) ? "down" :
		(action == GLFW_RELEASE) ? "up" : "repeat";

	std::printf("Key %d %s (mods=%d)\n", key, act, mods);

	// Track general press
	KeyPressed = (action == GLFW_PRESS);
	if (key == GLFW_KEY_LEFT_ALT) {
		AltKeyDown = (action != GLFW_RELEASE);
	}
	if (key == GLFW_KEY_LEFT_SHIFT || key == GLFW_KEY_RIGHT_SHIFT) {
		ShiftKeyDown = (action != GLFW_RELEASE);
	}
	// Track specific keys
	if (key == GLFW_KEY_W)
		WKeyDown = (action != GLFW_RELEASE);
	if (key == GLFW_KEY_S)
		SKeyDown = (action != GLFW_RELEASE);
	if (key == GLFW_KEY_A)
		AKeyDown = (action != GLFW_RELEASE);
	if (key == GLFW_KEY_D)
		DKeyDown = (action != GLFW_RELEASE);
	if (key == GLFW_KEY_LEFT)
		LeftArrowDown = (action != GLFW_RELEASE);
	if (key == GLFW_KEY_RIGHT)
		RightArrowDown = (action != GLFW_RELEASE);
	if (key == GLFW_KEY_U && action == GLFW_PRESS) {
		BrushShapeMode = (BrushShapeMode + 1) % 3;
		const char* shapeName = (BrushShapeMode == 0) ? "Cube" : ((BrushShapeMode == 1) ? "Sphere" : "Plane");
		std::printf("Brush Shape: %s\n", shapeName);
	}
	if (key == GLFW_KEY_I && action == GLFW_PRESS) {
		EmissiveMode = !EmissiveMode;
		std::printf("Emissive Mode: %s\n", EmissiveMode ? "ON" : "OFF");
	}
}

static float leftBut = 1.0;
static float BrushRadius = 0.01;
static float BrushDistanceFromCamera = 0.2f;

static bool intersectAabb(const glm::vec3& ro, const glm::vec3& rd, const glm::vec3& bmin, const glm::vec3& bmax, float& tEnter, float& tExit)
{
	tEnter = -1e30f;
	tExit = 1e30f;

	for (int axis = 0; axis < 3; ++axis) {
		float o = ro[axis];
		float d = rd[axis];
		float mn = bmin[axis];
		float mx = bmax[axis];

		if (std::fabs(d) < 1e-6f) {
			if (o < mn || o > mx) return false;
			continue;
		}

		float t0 = (mn - o) / d;
		float t1 = (mx - o) / d;
		if (t0 > t1) std::swap(t0, t1);

		tEnter = std::max(tEnter, t0);
		tExit = std::min(tExit, t1);
		if (tExit < tEnter) return false;
	}
	return true;
}

void Pottery::update(float time)
{

	
	double mouseX, mouseY;
	glfwGetCursorPos(base->window, &mouseX, &mouseY);
	
	glfwSetKeyCallback(base->window, key_cb);
	scene->slowZoom = AltKeyDown;
	float msx = mouseX / base->swapChain->swapChainExtent.width;
	float msy = mouseY / base->swapChain->swapChainExtent.height;
	// Example: print to console
	//std::printf("Mouse: (%.1f, %.1f)\n", msx-0.5, msy-0.5);
	potteryUBO pubo;
	pubo.time = time*0.05;
	
	scene->WKeyDown = WKeyDown;
	scene->AKeyDown = AKeyDown;
	scene->SKeyDown = SKeyDown;
	scene->DKeyDown = DKeyDown;
	scene->LeftArrowDown = LeftArrowDown;
	scene->RightArrowDown = RightArrowDown;
	if(!scene->cameraMoveMode){
		 if (scene->RightButtonDown) leftBut = 0.0;
		if (scene->LeftButtonDown) leftBut = 1.0;
		if (AKeyDown) { BrushRadius += 0.0001f; std::cout << BrushRadius << "\n"; }
		if (DKeyDown) { BrushRadius -= 0.0001f; std::cout << BrushRadius << "\n"; }
		if (WKeyDown) BrushDistanceFromCamera += 0.005f;
		if (SKeyDown) BrushDistanceFromCamera -= 0.005f;

		if (BrushRadius < 0.001f) BrushRadius = 0.001f;
		if (BrushDistanceFromCamera < 0.02f) BrushDistanceFromCamera = 0.02f;
		if (BrushDistanceFromCamera > 3.0f) BrushDistanceFromCamera = 3.0f;
	}
	glm::mat4 invProj = glm::inverse(scene->proj[0]);
	glm::mat4 invView = glm::inverse(scene->view[0]);
	float ndcX = msx * 2.0f - 1.0f;
	float ndcY = 1.0f - msy * 2.0f;

	glm::vec4 rayClip(ndcX, ndcY, -1.0f, 1.0f);
	glm::vec4 rayView = invProj * rayClip;
	rayView = glm::vec4(rayView.x, rayView.y, -1.0f, 0.0f);
	glm::vec3 rayDir = glm::normalize(glm::vec3(invView * rayView));
	glm::vec3 rayOrigin = scene->camPos;

	float tEnter = 0.0f, tExit = 0.0f;
	glm::vec3 brushCenter = rayOrigin + rayDir * BrushDistanceFromCamera; // fallback
	if (intersectAabb(rayOrigin, rayDir, glm::vec3(-0.5f), glm::vec3(0.5f), tEnter, tExit)) {
		float tStart = std::max(tEnter, 0.0f);
		float tCenter = std::min(tStart + BrushDistanceFromCamera, tExit - 0.001f);
		if (tCenter >= tStart) {
			brushCenter = rayOrigin + rayDir * tCenter;
		}
	}
	pubo.BrushData = glm::vec4(-brushCenter, BrushRadius);
	pubo.BrushColor = scene->brushColor;
	pubo.BrushAction = leftBut; // 1.0 add, 0.0 remove
	pubo.BrushShape = static_cast<float>(BrushShapeMode);
	pubo.BrushEmissive = EmissiveMode ? 1.0f : 0.0f;
	pubo.BrushRecolor = (ShiftKeyDown && scene->LeftButtonDown && !scene->cameraMoveMode) ? 1.0f : 0.0f;
	scene->brushData = pubo.BrushData;
	scene->brushUseSphere = (BrushShapeMode == 1);
	scene->brushShapeMode = BrushShapeMode;
	scene->brushRadius = BrushRadius;
	scene->brushZAxis = BrushDistanceFromCamera;
	scene->brushAction = leftBut;
	scene->ShiftDown = ShiftKeyDown;
	scene->emissiveMode = EmissiveMode;
	computeUBO[0].map(base->device, sizeof(potteryUBO), &pubo);


	int gpuValue;
	//computeSBO[2].read(base->device, sizeof(int), &gpuValue);
	//std::cout << "gpuValue: " << gpuValue/(9*9*9) << std::endl;
	//gpuValue = 0;
	//computeSBO[2].map(base->device, sizeof(int), &gpuValue);

	const int dim = 8;
	const int total = dim * dim * dim; // 512

	// CPU-side storage
	std::vector<int> gpuArray(total);

	// Read from GPU buffer
	//computeSBO[1].read(base->device, sizeof(int) * gpuArray.size(), gpuArray.data());

	
	//std::cout << "gpuArray[0]: " << gpuArray[10] << std::endl;

	int zero = 0;

	// write to GPU
	//computeSBO[0].map(base->device, sizeof(int), &zero);
	
	if (!scene->cameraMoveMode && (scene->LeftButtonDown|| scene->RightButtonDown)) {
		int dirtyZero = 0;
		computeSBO[4].map(base->device, sizeof(int), &dirtyZero);
		comp->submit();
		vkWaitForFences(base->device, 1, &comp->computeFence, VK_TRUE, UINT64_MAX);
		int dirtyCount = 0;
		computeSBO[4].read(base->device, sizeof(int), &dirtyCount);
		scene->dirtyBlockCount = dirtyCount;

		Mesh* targetMesh = nullptr;
		for (Object& obj : scene->objects) {
			if (!obj.hasMesh) continue;
			for (size_t i = 0; i < obj.meshID.size(); ++i) {
				if (obj.shader.size() <= i) continue;
				if (obj.shader[i].graphicsPipeline == scene->pipelines[4]) {
					targetMesh = &scene->meshes[obj.meshID[i]];
					break;
				}
			}
			if (targetMesh) break;
		}
		if (targetMesh && targetMesh->hasInstanceIdBuffer) {
			const uint32_t totalBlocks = 64u * 64u * 64u;
			std::vector<int32_t> blockState(totalBlocks, -1);
			computeSBO[1].read(base->device, sizeof(int32_t) * totalBlocks, blockState.data());

			std::vector<int32_t> activeIds;
			activeIds.reserve(static_cast<size_t>(dirtyCount > 0 ? dirtyCount : 0));
			for (uint32_t i = 0; i < totalBlocks; ++i) {
				if (blockState[i] >= 0) {
					activeIds.push_back(static_cast<int32_t>(i));
				}
			}

			uint32_t oldCount = targetMesh->instanceCount;
			base->vbuffer->updateInstanceIdBuffer(*targetMesh, activeIds);
			uint32_t newCount = targetMesh->instanceCount;
			scene->dirtyBlockCount = static_cast<int>(newCount);

			if (newCount != oldCount) {
				vkDeviceWaitIdle(base->device);
				scene->renderPass();
			}
		}
	}
}
