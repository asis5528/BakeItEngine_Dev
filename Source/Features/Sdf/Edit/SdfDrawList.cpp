#include "SdfDrawList.h"
#include <stdexcept>

namespace
{
constexpr uint32_t kBlocksPerAxis = 64u;
constexpr uint32_t kTotalBlocks = kBlocksPerAxis * kBlocksPerAxis * kBlocksPerAxis;
constexpr uint32_t kNumBuckets = 256u;
constexpr uint32_t kGroupSize = 256u;
constexpr uint32_t kBlockGroups = (kTotalBlocks + kGroupSize - 1u) / kGroupSize;
// Buckets cover camera distances up to 192 block units (the chunk diagonal is
// ~111; some headroom for the camera sitting outside the chunk).
constexpr float kBucketScale = float(kNumBuckets) / 192.0f;

// Mirrors the SortParams UBO of the count/scatter shaders.
struct alignas(16) SortParamsData
{
	glm::vec4 camAndScale;      // xyz: camera (block units), w: bucket scale
	glm::vec4 frustumPlanes[6]; // block units, normalized, inward-facing
};

void MemoryBarrier(
	VkCommandBuffer cmd,
	VkPipelineStageFlags srcStage, VkAccessFlags srcAccess,
	VkPipelineStageFlags dstStage, VkAccessFlags dstAccess)
{
	VkMemoryBarrier barrier{};
	barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
	barrier.srcAccessMask = srcAccess;
	barrier.dstAccessMask = dstAccess;
	vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 1, &barrier, 0, nullptr, 0, nullptr);
}
}

SdfDrawList::SdfDrawList(
	VulkanBase* base,
	const SBO& blocksSbo,
	VkBuffer instanceIdBuffer,
	uint64_t instanceIdBufferSize,
	VkBuffer indirectBuffer)
{
	this->base = base;
	this->indirectBuffer = indirectBuffer;

	sortParams = base->vbuffer->createUniformBuffers({ sizeof(SortParamsData) });
	bucketSbo = base->vbuffer->createStorageBuffers({ sizeof(uint32_t) * kNumBuckets });

	SBO instanceSbo{};
	instanceSbo.buffer = instanceIdBuffer;
	instanceSbo.size = instanceIdBufferSize;
	SBO indirectSbo{};
	indirectSbo.buffer = indirectBuffer;
	indirectSbo.size = sizeof(VkDrawIndexedIndirectCommand);

	ComputeInput countInput;
	countInput.computeShaderPath = "shaders/Sdf/SdfDrawListCountCS.spv";
	countInput.descriptorTypes = {
		VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, // blocks
		VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, // bucket counts
		VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER  // sort params
	};
	countPipe = new ComputePipeline(base, countInput);
	countPipe->setupDescriptors(sortParams, {}, { blocksSbo, bucketSbo[0] });

	ComputeInput scanInput;
	scanInput.computeShaderPath = "shaders/Sdf/SdfDrawListScanCS.spv";
	scanInput.descriptorTypes = {
		VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, // buckets (counts -> offsets)
		VK_DESCRIPTOR_TYPE_STORAGE_BUFFER  // indirect command (instanceCount)
	};
	scanPipe = new ComputePipeline(base, scanInput);
	scanPipe->setupDescriptors({}, {}, { bucketSbo[0], indirectSbo });

	ComputeInput scatterInput;
	scatterInput.computeShaderPath = "shaders/Sdf/SdfDrawListScatterCS.spv";
	scatterInput.descriptorTypes = {
		VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, // blocks
		VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, // bucket offsets
		VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, // instance IDs
		VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER  // sort params
	};
	scatterPipe = new ComputePipeline(base, scatterInput);
	scatterPipe->setupDescriptors(sortParams, {}, { blocksSbo, bucketSbo[0], instanceSbo });

	VkCommandPoolCreateInfo poolInfo{};
	poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	poolInfo.queueFamilyIndex = base->vulkandevice->queueID;
	poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	if (vkCreateCommandPool(base->device, &poolInfo, nullptr, &pool) != VK_SUCCESS) {
		throw std::runtime_error("failed to create draw list command pool!");
	}

	VkCommandBufferAllocateInfo allocInfo{};
	allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	allocInfo.commandPool = pool;
	allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	allocInfo.commandBufferCount = 1;
	if (vkAllocateCommandBuffers(base->device, &allocInfo, &cmd) != VK_SUCCESS) {
		throw std::runtime_error("failed to allocate draw list command buffer!");
	}

	VkFenceCreateInfo fenceInfo{};
	fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
	if (vkCreateFence(base->device, &fenceInfo, nullptr, &fence) != VK_SUCCESS) {
		throw std::runtime_error("failed to create draw list fence!");
	}

	record();
}

void SdfDrawList::record()
{
	VkCommandBufferBeginInfo beginInfo{};
	beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	if (vkBeginCommandBuffer(cmd, &beginInfo) != VK_SUCCESS) {
		throw std::runtime_error("failed to begin draw list command buffer!");
	}

	// Earlier submissions may still draw from the previous list (indirect +
	// per-instance reads) or write blocks[] (brush compute); order them
	// before the bucket reset and the count pass reads.
	MemoryBarrier(cmd,
		VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
		VK_ACCESS_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
		VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
		VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT);

	vkCmdFillBuffer(cmd, bucketSbo[0].buffer, 0, sizeof(uint32_t) * kNumBuckets, 0);

	MemoryBarrier(cmd,
		VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
		VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);

	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, countPipe->computePipeline);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, countPipe->pipelineLayout, 0, 1, &countPipe->descriptorSet, 0, 0);
	vkCmdDispatch(cmd, kBlockGroups, 1, 1);

	MemoryBarrier(cmd,
		VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
		VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);

	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, scanPipe->computePipeline);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, scanPipe->pipelineLayout, 0, 1, &scanPipe->descriptorSet, 0, 0);
	vkCmdDispatch(cmd, 1, 1, 1);

	MemoryBarrier(cmd,
		VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
		VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);

	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, scatterPipe->computePipeline);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, scatterPipe->pipelineLayout, 0, 1, &scatterPipe->descriptorSet, 0, 0);
	vkCmdDispatch(cmd, kBlockGroups, 1, 1);

	// Hand the fresh list to later submissions: the indirect draw command
	// read and the per-instance vertex attribute fetch.
	MemoryBarrier(cmd,
		VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
		VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_VERTEX_INPUT_BIT,
		VK_ACCESS_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT);

	if (vkEndCommandBuffer(cmd) != VK_SUCCESS) {
		throw std::runtime_error("failed to record draw list command buffer!");
	}
}

void SdfDrawList::build(const glm::vec3& camBlockUnits, const glm::mat4& blockToClip)
{
	vkWaitForFences(base->device, 1, &fence, VK_TRUE, UINT64_MAX);
	vkResetFences(base->device, 1, &fence);

	SortParamsData params{};
	params.camAndScale = glm::vec4(camBlockUnits, kBucketScale);

	// Gribb-Hartmann frustum planes from the block-units -> clip matrix.
	// GLM_FORCE_DEPTH_ZERO_TO_ONE: the near plane is row2 alone (z >= 0),
	// not row3 + row2. Normalized so the shaders' sphere-radius test works.
	const glm::mat4& m = blockToClip;
	const glm::vec4 r0(m[0][0], m[1][0], m[2][0], m[3][0]);
	const glm::vec4 r1(m[0][1], m[1][1], m[2][1], m[3][1]);
	const glm::vec4 r2(m[0][2], m[1][2], m[2][2], m[3][2]);
	const glm::vec4 r3(m[0][3], m[1][3], m[2][3], m[3][3]);
	const glm::vec4 raw[6] = { r3 + r0, r3 - r0, r3 + r1, r3 - r1, r2, r3 - r2 };
	for (int i = 0; i < 6; ++i) {
		const float len = glm::length(glm::vec3(raw[i]));
		params.frustumPlanes[i] = raw[i] / glm::max(len, 1e-12f);
	}

	sortParams[0].map(base->device, sizeof(SortParamsData), &params);

	VkSubmitInfo submitInfo{};
	submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submitInfo.commandBufferCount = 1;
	submitInfo.pCommandBuffers = &cmd;
	if (vkQueueSubmit(base->queue, 1, &submitInfo, fence) != VK_SUCCESS) {
		throw std::runtime_error("failed to submit draw list build!");
	}
}

SdfDrawList::~SdfDrawList()
{
	vkWaitForFences(base->device, 1, &fence, VK_TRUE, UINT64_MAX);
	delete countPipe;
	delete scanPipe;
	delete scatterPipe;
	for (UBO& ubo : sortParams) {
		vkDestroyBuffer(base->device, ubo.buffer, nullptr);
		vkFreeMemory(base->device, ubo.bufferMemory, nullptr);
	}
	for (SBO& sbo : bucketSbo) {
		vkDestroyBuffer(base->device, sbo.buffer, nullptr);
		vkFreeMemory(base->device, sbo.bufferMemory, nullptr);
	}
	vkDestroyFence(base->device, fence, nullptr);
	vkDestroyCommandPool(base->device, pool, nullptr);
}
