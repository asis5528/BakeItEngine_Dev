#include "VulkanImage.h"
#include <stdexcept>
#include "VulkanBuffer.h"
#include "VulkanUtils.h"

 VulkanImage::VulkanImage(VkDevice device, VkPhysicalDevice physicalDevice) {
    this->device = device;
    this->physicalDevice = physicalDevice;
    this->image = VK_NULL_HANDLE;
    this->imageView = VK_NULL_HANDLE;
    this->imageMemory = VK_NULL_HANDLE;
}
 void VulkanTexture::destroy(VkDevice device)
 {
     if (imageView != VK_NULL_HANDLE) {
         vkDestroyImageView(device, imageView, nullptr);
         imageView = VK_NULL_HANDLE;
     }
     if (image != VK_NULL_HANDLE) {
         vkDestroyImage(device, image, nullptr);
         image = VK_NULL_HANDLE;
     }
     if (imageMemory != VK_NULL_HANDLE) {
         vkFreeMemory(device, imageMemory, nullptr);
         imageMemory = VK_NULL_HANDLE;
     }
 }

 void VulkanImage::destroy()
 {
     if (imageView != VK_NULL_HANDLE) {
         vkDestroyImageView(device, imageView, nullptr);
         imageView = VK_NULL_HANDLE;
     }
     if (image != VK_NULL_HANDLE) {
         vkDestroyImage(device, image, nullptr);
         image = VK_NULL_HANDLE;
     }
     if (imageMemory != VK_NULL_HANDLE) {
         vkFreeMemory(device, imageMemory, nullptr);
         imageMemory = VK_NULL_HANDLE;
     }
 }
 void VulkanImage::createImage(uint32_t width, uint32_t height, uint32_t depth,VkImageType type ,uint32_t mipLevels, VkSampleCountFlagBits numSamples, VkFormat format, VkImageTiling tiling, VkImageUsageFlags usage, VkMemoryPropertyFlags properties,uint32_t arrayCount) {
     VkImageCreateInfo imageInfo{};
     imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
     imageInfo.imageType = type;
     imageInfo.extent.width = width;
     imageInfo.extent.height = height;
     imageInfo.extent.depth = depth;
     imageInfo.mipLevels = mipLevels;
     imageInfo.arrayLayers = arrayCount;
     imageInfo.format = format;
     imageInfo.tiling = tiling;
     imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
     imageInfo.usage = usage;
     imageInfo.samples = numSamples;
     imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

     if (vkCreateImage(device, &imageInfo, nullptr, &image) != VK_SUCCESS) {
         throw std::runtime_error("failed to create image!");
     }

     VkMemoryRequirements memRequirements;
     vkGetImageMemoryRequirements(device, image, &memRequirements);

     VkMemoryAllocateInfo allocInfo{};
     allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
     allocInfo.allocationSize = memRequirements.size;
     allocInfo.memoryTypeIndex = findMemoryType(physicalDevice, memRequirements.memoryTypeBits, properties);

     if (vkAllocateMemory(device, &allocInfo, nullptr, &imageMemory) != VK_SUCCESS) {
         throw std::runtime_error("failed to allocate image memory!");
     }

     vkBindImageMemory(device, image, imageMemory, 0);
 }

void VulkanImage::createImageView(VkFormat format, VkImageAspectFlags aspectFlags, uint32_t mipLevels,VkImageViewType type,  uint32_t layerCount ) {
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = image;
    viewInfo.viewType = type;
    viewInfo.format = format;
    viewInfo.subresourceRange.aspectMask = aspectFlags;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = mipLevels;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = layerCount;
 

   
    if (vkCreateImageView(device, &viewInfo, nullptr, &imageView) != VK_SUCCESS) {
        throw std::runtime_error("failed to create texture image view!");
    }

   
}


uint32_t VulkanImage::findMemoryType(VkPhysicalDevice physicalDevice, uint32_t typeFilter, VkMemoryPropertyFlags properties) {
    VkPhysicalDeviceMemoryProperties memProperties;
    vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memProperties);

    for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
        if ((typeFilter & (1 << i)) && (memProperties.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }

    throw std::runtime_error("failed to find suitable memory type!");
}



