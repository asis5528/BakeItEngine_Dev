#include "Mesh.h"

void Mesh::draw(VkCommandBuffer commandBuffer) {
    if (hasInstanceIdBuffer) {
        VkBuffer vertexBuffers[] = { vertexBuffer, instanceIdBuffer };
        VkDeviceSize offsets[] = { 0, 0 };
        vkCmdBindVertexBuffers(commandBuffer, 0, 2, vertexBuffers, offsets);
    } else {
        VkBuffer vertexBuffers[] = { vertexBuffer };
        VkDeviceSize offsets[] = { 0 };
        vkCmdBindVertexBuffers(commandBuffer, 0, 1, vertexBuffers, offsets);
    }

    vkCmdBindIndexBuffer(commandBuffer, indexBuffer, 0, VK_INDEX_TYPE_UINT32);

  
    if(hasInstanceIdBuffer){
    vkCmdDrawIndexed(commandBuffer, static_cast<uint32_t>(indices.size()), 
        hasInstanceIdBuffer ? instanceCount : (64u * 64u * 64u), 
        //1,
        0, 0, 0);
    }
    else {
        vkCmdDrawIndexed(commandBuffer, static_cast<uint32_t>(indices.size()),
            1,
			0, 0, 0);
    }
}


