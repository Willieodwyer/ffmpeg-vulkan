#ifndef VULKAN_IMAGE_SCALING_H
#define VULKAN_IMAGE_SCALING_H

VkInstance instance;
VkPhysicalDevice physical_device;
VkDevice device;

bool init_vulkan()
{
  // 1. Initialize Vulkan Instance
  VkApplicationInfo app_info = {};
  app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
  app_info.pApplicationName = "Vulkan Application";
  app_info.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
  app_info.pEngineName = "No Engine";
  app_info.engineVersion = VK_MAKE_VERSION(1, 0, 0);
  app_info.apiVersion = VK_API_VERSION_1_3;

  VkInstanceCreateInfo create_info = {};
  create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  create_info.pApplicationInfo = &app_info;

  if (vkCreateInstance(&create_info, nullptr, &instance) != VK_SUCCESS) {
    std::cerr << "Failed to create Vulkan instance!" << std::endl;
    return false;
  }

  // 2. Enumerate Physical Devices
  uint32_t device_count = 0;
  vkEnumeratePhysicalDevices(instance, &device_count, nullptr);
  if (device_count == 0) {
    std::cerr << "No Vulkan-compatible devices found!" << std::endl;
    return false;
  }

  std::vector<VkPhysicalDevice> devices(device_count);
  vkEnumeratePhysicalDevices(instance, &device_count, devices.data());

  // 3. Select the first available physical device (simplified selection)
  physical_device = devices[0];

  VkPhysicalDeviceProperties device_properties;
  vkGetPhysicalDeviceProperties(physical_device, &device_properties);

  // Print the name of the physical device
  std::cout << "Physical Device Name: " << device_properties.deviceName << std::endl;

  // 4. Create Logical Device
  VkDeviceCreateInfo device_create_info = {};
  device_create_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;

  // Add queue info (basic, no additional queues)
  float queue_priority = 1.0f;
  VkDeviceQueueCreateInfo queue_create_info = {};
  queue_create_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
  queue_create_info.queueFamilyIndex = 0; // Assume index 0 for simplicity
  queue_create_info.queueCount = 1;
  queue_create_info.pQueuePriorities = &queue_priority;

  device_create_info.pQueueCreateInfos = &queue_create_info;
  device_create_info.queueCreateInfoCount = 1;

  // Create the logical device
  if (vkCreateDevice(physical_device, &device_create_info, nullptr, &device) != VK_SUCCESS) {
    std::cerr << "Failed to create Vulkan device!" << std::endl;
    return false;
  }

  return true;
}

void cleanup()
{
  vkDestroyDevice(device, nullptr);
  vkDestroyInstance(instance, nullptr);
}

void copy_and_convert_to_yuv(VkImage image, int width, int height)
{
  // 1. Allocate memory for the image on the CPU
  VkMemoryRequirements mem_reqs;
  vkGetImageMemoryRequirements(device, image, &mem_reqs);

  VkMemoryAllocateInfo mem_alloc_info = {};
  mem_alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  mem_alloc_info.allocationSize = mem_reqs.size;

  VkPhysicalDeviceMemoryProperties mem_props;
  vkGetPhysicalDeviceMemoryProperties(physical_device, &mem_props);

  for (uint32_t i = 0; i < mem_props.memoryTypeCount; i++) {
    if ((mem_reqs.memoryTypeBits & (1 << i)) &&
        (mem_props.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) &&
        (mem_props.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
      mem_alloc_info.memoryTypeIndex = i;
      break;
    }
  }

  VkDeviceMemory memory;
  if (vkAllocateMemory(device, &mem_alloc_info, nullptr, &memory) != VK_SUCCESS) {
    std::cerr << "Failed to allocate memory!" << std::endl;
    return;
  }

  vkBindImageMemory(device, image, memory, 0);

  // 2. Map the memory and copy the image data to the CPU
  void* data;
  vkMapMemory(device, memory, 0, mem_reqs.size, 0, &data);

  VkImageSubresource subresource = {};
  subresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  subresource.mipLevel = 0;
  subresource.arrayLayer = 0;

  VkSubresourceLayout layout;
  vkGetImageSubresourceLayout(device, image, &subresource, &layout);

  uint8_t* pixel_data = static_cast<uint8_t*>(data) + layout.offset;

  // 3. Convert RGB to YUV
  std::vector<uint8_t> yuv_data(width * height * 3 / 2); // Assuming YUV 4:2:0 format
  uint8_t* y_plane = yuv_data.data();
  uint8_t* u_plane = yuv_data.data() + width * height;
  uint8_t* v_plane = u_plane + (width * height / 4);

  for (int y = 0; y < height; y++) {
    for (int x = 0; x < width; x++) {
      uint8_t r = pixel_data[(y * width + x) * 4 + 0];
      uint8_t g = pixel_data[(y * width + x) * 4 + 1];
      uint8_t b = pixel_data[(y * width + x) * 4 + 2];

      uint8_t y_val = static_cast<uint8_t>(0.299 * r + 0.587 * g + 0.114 * b);
      uint8_t u_val = static_cast<uint8_t>((b - y_val) * 0.565 + 128);
      uint8_t v_val = static_cast<uint8_t>((r - y_val) * 0.713 + 128);

      y_plane[y * width + x] = y_val;
      if (y % 2 == 0 && x % 2 == 0) {
        u_plane[(y / 2) * (width / 2) + (x / 2)] = u_val;
        v_plane[(y / 2) * (width / 2) + (x / 2)] = v_val;
      }
    }
  }

  std::ofstream yuv_file("yuv.yuv", std::ios::binary);
  yuv_file.write(reinterpret_cast<char*>(yuv_data.data()), yuv_data.size());
  yuv_file.close();

  // 4. Unmap and free the memory
  vkUnmapMemory(device, memory);
  vkFreeMemory(device, memory, nullptr);

  // Now yuv_data contains the YUV 4:2:0 data of the image
}

bool vk_scale_image(AVFrame* hw_frame, int target_width, int target_height)
{
  VkImage vulkan_image = (VkImage)hw_frame->data[0];

  // 1. Create the target image (scaled image)
  VkImageCreateInfo image_create_info = {};
  image_create_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  image_create_info.imageType = VK_IMAGE_TYPE_2D;
  image_create_info.format = VK_FORMAT_R8G8B8A8_UNORM; // Assuming RGBA format, change if necessary
  image_create_info.extent.width = target_width;
  image_create_info.extent.height = target_height;
  image_create_info.extent.depth = 1;
  image_create_info.mipLevels = 1;
  image_create_info.arrayLayers = 1;
  image_create_info.samples = VK_SAMPLE_COUNT_1_BIT;
  image_create_info.tiling = VK_IMAGE_TILING_OPTIMAL;
  image_create_info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

  VkImage scaled_image;
  vkCreateImage(device, &image_create_info, nullptr, &scaled_image);

  // 2. Create image views for both source and target images
  VkImageViewCreateInfo image_view_create_info = {};
  image_view_create_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  image_view_create_info.image = vulkan_image;
  image_view_create_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
  image_view_create_info.format = VK_FORMAT_R8G8B8A8_UNORM;
  image_view_create_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  image_view_create_info.subresourceRange.levelCount = 1;
  image_view_create_info.subresourceRange.layerCount = 1;

  VkImageView source_image_view;
  vkCreateImageView(device, &image_view_create_info, nullptr, &source_image_view);

  image_view_create_info.image = scaled_image;
  VkImageView target_image_view;
  vkCreateImageView(device, &image_view_create_info, nullptr, &target_image_view);

  // 3. Query queue family index that supports graphics or transfer operations
  uint32_t queue_family_count = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &queue_family_count, nullptr);

  VkQueueFamilyProperties* queue_families = new VkQueueFamilyProperties[queue_family_count];
  vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &queue_family_count, queue_families);

  uint32_t queue_family_index = UINT32_MAX;
  for (uint32_t i = 0; i < queue_family_count; ++i) {
    if (queue_families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
      queue_family_index = i;
      break;
    }
  }

  delete[] queue_families;

  if (queue_family_index == UINT32_MAX) {
    // Handle error: No suitable queue family found
    return true;
  }

  // 4. Create a queue
  VkQueue queue;
  vkGetDeviceQueue(device, queue_family_index, 0, &queue); // Get the first queue in the selected family

  // 5. Create a command pool to allocate command buffers
  VkCommandPoolCreateInfo cmd_pool_create_info = {};
  cmd_pool_create_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  cmd_pool_create_info.queueFamilyIndex = queue_family_index;
  cmd_pool_create_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;

  VkCommandPool command_pool;
  vkCreateCommandPool(device, &cmd_pool_create_info, nullptr, &command_pool);

  // 6. Create a command buffer for scaling operation
  VkCommandBufferAllocateInfo cmd_buffer_allocate_info = {};
  cmd_buffer_allocate_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  cmd_buffer_allocate_info.commandPool = command_pool;
  cmd_buffer_allocate_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cmd_buffer_allocate_info.commandBufferCount = 1;

  VkCommandBuffer command_buffer;
  vkAllocateCommandBuffers(device, &cmd_buffer_allocate_info, &command_buffer);

  // 7. Begin command buffer recording
  VkCommandBufferBeginInfo cmd_buffer_begin_info = {};
  cmd_buffer_begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  vkBeginCommandBuffer(command_buffer, &cmd_buffer_begin_info);

  // 8. Set up the Blit Image operation (scaling)
  VkImageSubresourceRange subresource_range = {};
  subresource_range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  subresource_range.levelCount = 1;
  subresource_range.layerCount = 1;

  VkImageBlit blit_region = {};
  blit_region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  blit_region.srcOffsets[0] = {0, 0, 0};
  blit_region.srcOffsets[1] = {hw_frame->width, hw_frame->height, 1}; // Source image dimensions

  blit_region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  blit_region.dstOffsets[0] = {0, 0, 0};
  blit_region.dstOffsets[1] = {target_width, target_height, 1}; // Target image dimensions

  vkCmdBlitImage(command_buffer, vulkan_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, scaled_image,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit_region,
                 VK_FILTER_LINEAR // Use linear filter for scaling
  );

  // 9. End command buffer recording
  vkEndCommandBuffer(command_buffer);

  // 10. Create a fence to wait for the command buffer completion
  VkFenceCreateInfo fence_create_info = {};
  fence_create_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  VkFence fence;
  vkCreateFence(device, &fence_create_info, nullptr, &fence);

  // 11. Submit the command buffer for execution
  VkSubmitInfo submit_info = {};
  submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  submit_info.commandBufferCount = 1;
  submit_info.pCommandBuffers = &command_buffer;

  vkQueueSubmit(queue, 1, &submit_info, fence);

  // 12. Wait for the fence to signal
  VkResult result = vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX);

  if (result == VK_SUCCESS) {
    // Command buffer completed successfully
    printf("Command executed successfully!\n");

    // VkImage to YUV Here
    copy_and_convert_to_yuv(scaled_image, target_width, target_height);


  }
  else {
    // There was an error during command execution
    printf("Error occurred during command execution!\n");
  }

  // 13. Clean up Vulkan resources (you should do this after the scaling operation is complete)
  vkDestroyImageView(device, source_image_view, nullptr);
  vkDestroyImageView(device, target_image_view, nullptr);
  vkDestroyImage(device, scaled_image, nullptr);
  vkDestroyFence(device, fence, nullptr);

  // 14. Clean up the command pool
  vkDestroyCommandPool(device, command_pool, nullptr);
  return true;
}

#endif //VULKAN_IMAGE_SCALING_H
