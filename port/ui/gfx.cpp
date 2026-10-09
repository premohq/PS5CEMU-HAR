// SPDX-License-Identifier: GPL-3.0-or-later
// PS5CEMU-HAR's UI kit: drawing on the GPU (gfx.h).

#include "gfx.h"
#include "shaders.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>

namespace ui
{
	namespace
	{
		std::function<void(const std::string&)> s_log;

		const char* ResultName(VkResult result)
		{
			switch (result)
			{
			case VK_SUCCESS: return "VK_SUCCESS";
			case VK_NOT_READY: return "VK_NOT_READY";
			case VK_TIMEOUT: return "VK_TIMEOUT";
			case VK_SUBOPTIMAL_KHR: return "VK_SUBOPTIMAL_KHR";
			case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
			case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
			case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
			case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
			case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
			case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
			case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
			case VK_ERROR_SURFACE_LOST_KHR: return "VK_ERROR_SURFACE_LOST_KHR";
			case VK_ERROR_NATIVE_WINDOW_IN_USE_KHR: return "VK_ERROR_NATIVE_WINDOW_IN_USE_KHR";
			case VK_ERROR_OUT_OF_DATE_KHR: return "VK_ERROR_OUT_OF_DATE_KHR";
			default: return "VkResult";
			}
		}

		std::string Describe(VkResult result)
		{
			return std::string(ResultName(result)) + " (" + std::to_string((int)result) + ")";
		}
	}

	void SetLog(std::function<void(const std::string&)> log)
	{
		s_log = std::move(log);
	}

	void Log(const std::string& line)
	{
		if (s_log)
			s_log(line);
		else
			std::fprintf(stderr, "%s\n", line.c_str());
	}

	void DrawList::Add(const Instance& instance, TextureId texture)
	{
		if (runs.empty() || runs.back().texture != texture)
			runs.push_back({(uint32_t)instances.size(), 0, texture});
		runs.back().count++;
		instances.push_back(instance);
	}

	void DrawList::Clear()
	{
		instances.clear();
		runs.clear();
	}

	bool Gfx::Fail(std::string& error, const std::string& what, VkResult result)
	{
		error = result == VK_SUCCESS ? what : what + ": " + Describe(result);
		Log("[ui] gfx: " + error);
		Stop();
		return false;
	}

	uint32_t Gfx::MemoryType(uint32_t bits, VkMemoryPropertyFlags properties) const
	{
		for (uint32_t i = 0; i < m_memory.memoryTypeCount; i++)
			if ((bits & (1u << i)) && (m_memory.memoryTypes[i].propertyFlags & properties) == properties)
				return i;
		// any the resource can live in: on the PS5 every type is the same memory
		for (uint32_t i = 0; i < m_memory.memoryTypeCount; i++)
			if (bits & (1u << i))
				return i;
		return 0;
	}

	bool Gfx::MakeBuffer(Buffer& buffer, VkDeviceSize size, VkBufferUsageFlags usage)
	{
		VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
		info.size = size;
		info.usage = usage;
		info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		if (vk::vkCreateBuffer(m_device, &info, nullptr, &buffer.buffer) != VK_SUCCESS)
			return false;
		VkMemoryRequirements requirements;
		vk::vkGetBufferMemoryRequirements(m_device, buffer.buffer, &requirements);
		VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
		allocate.allocationSize = requirements.size;
		allocate.memoryTypeIndex = MemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
		if (vk::vkAllocateMemory(m_device, &allocate, nullptr, &buffer.memory) != VK_SUCCESS ||
			vk::vkBindBufferMemory(m_device, buffer.buffer, buffer.memory, 0) != VK_SUCCESS ||
			vk::vkMapMemory(m_device, buffer.memory, 0, VK_WHOLE_SIZE, 0, &buffer.mapped) != VK_SUCCESS)
		{
			FreeBuffer(buffer);
			return false;
		}
		buffer.size = size;
		buffer.bytes = requirements.size;
		m_memoryBytes += requirements.size;
		return true;
	}

	void Gfx::FreeBuffer(Buffer& buffer)
	{
		if (buffer.mapped)
			vk::vkUnmapMemory(m_device, buffer.memory);
		if (buffer.buffer)
			vk::vkDestroyBuffer(m_device, buffer.buffer, nullptr);
		if (buffer.memory)
			vk::vkFreeMemory(m_device, buffer.memory, nullptr);
		if (buffer.bytes && m_memoryBytes >= buffer.bytes)
			m_memoryBytes -= buffer.bytes;
		buffer = {};
	}

	bool Gfx::MakeImage(Image& image, uint32_t width, uint32_t height, VkFormat format, VkImageUsageFlags usage)
	{
		VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
		info.imageType = VK_IMAGE_TYPE_2D;
		info.format = format;
		info.extent = {width, height, 1};
		info.mipLevels = 1;
		info.arrayLayers = 1;
		info.samples = VK_SAMPLE_COUNT_1_BIT;
		info.tiling = VK_IMAGE_TILING_OPTIMAL;
		info.usage = usage;
		info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		if (vk::vkCreateImage(m_device, &info, nullptr, &image.image) != VK_SUCCESS)
			return false;
		VkMemoryRequirements requirements;
		vk::vkGetImageMemoryRequirements(m_device, image.image, &requirements);
		VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
		allocate.allocationSize = requirements.size;
		allocate.memoryTypeIndex = MemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
		if (vk::vkAllocateMemory(m_device, &allocate, nullptr, &image.memory) != VK_SUCCESS ||
			vk::vkBindImageMemory(m_device, image.image, image.memory, 0) != VK_SUCCESS)
		{
			FreeImage(image);
			return false;
		}
		VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
		view.image = image.image;
		view.viewType = VK_IMAGE_VIEW_TYPE_2D;
		view.format = format;
		view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
		if (vk::vkCreateImageView(m_device, &view, nullptr, &image.view) != VK_SUCCESS)
		{
			FreeImage(image);
			return false;
		}
		image.bytes = requirements.size;
		m_memoryBytes += requirements.size;
		return true;
	}

	void Gfx::FreeImage(Image& image)
	{
		if (image.view)
			vk::vkDestroyImageView(m_device, image.view, nullptr);
		if (image.image)
			vk::vkDestroyImage(m_device, image.image, nullptr);
		if (image.memory)
			vk::vkFreeMemory(m_device, image.memory, nullptr);
		if (image.bytes && m_memoryBytes >= image.bytes)
			m_memoryBytes -= image.bytes;
		image = {};
	}

	bool Gfx::Start(PFN_vkGetInstanceProcAddr gipa, const Target& target, std::string& error)
	{
		if (m_device)
			return true;
		m_target = target;
		const bool display = (bool)target.createSurface;

		// the instance
		if (!vk::LoadGlobal(gipa))
			return Fail(error, "no vkCreateInstance");
		VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
		app.pApplicationName = "PS5CEMU-HAR";
		app.pEngineName = "PS5CEMU-HAR UI";
		app.apiVersion = VK_API_VERSION_1_1;
		VkInstanceCreateInfo instanceInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
		instanceInfo.pApplicationInfo = &app;
		instanceInfo.enabledExtensionCount = (uint32_t)target.instanceExtensions.size();
		instanceInfo.ppEnabledExtensionNames = target.instanceExtensions.data();
		VkResult result = vk::vkCreateInstance(&instanceInfo, nullptr, &m_instance);
		if (result != VK_SUCCESS)
			return Fail(error, "vkCreateInstance", result);
		if (!vk::LoadInstance(m_instance, display))
			return Fail(error, std::string("the driver has no ") + vk::Missing());
		Log("[ui] gfx: instance");

		// the GPU and its one graphics queue
		uint32_t count = 0;
		vk::vkEnumeratePhysicalDevices(m_instance, &count, nullptr);
		if (count == 0)
			return Fail(error, "the driver reports no GPU");
		std::vector<VkPhysicalDevice> physicals(count);
		vk::vkEnumeratePhysicalDevices(m_instance, &count, physicals.data());
		m_physical = physicals[0];
		VkPhysicalDeviceProperties properties;
		vk::vkGetPhysicalDeviceProperties(m_physical, &properties);
		vk::vkGetPhysicalDeviceMemoryProperties(m_physical, &m_memory);
		count = 0;
		vk::vkGetPhysicalDeviceQueueFamilyProperties(m_physical, &count, nullptr);
		std::vector<VkQueueFamilyProperties> families(count);
		vk::vkGetPhysicalDeviceQueueFamilyProperties(m_physical, &count, families.data());
		m_queueFamily = UINT32_MAX;
		for (uint32_t i = 0; i < count && m_queueFamily == UINT32_MAX; i++)
			if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)
				m_queueFamily = i;
		if (m_queueFamily == UINT32_MAX)
			return Fail(error, "the GPU has no graphics queue");

		if (display)
		{
			m_surface = target.createSurface(m_instance, error);
			if (!m_surface)
				return Fail(error, error.empty() ? "no display surface" : error);
			Log("[ui] gfx: VideoOut surface");
		}

		const float priority = 1.0f;
		VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
		queueInfo.queueFamilyIndex = m_queueFamily;
		queueInfo.queueCount = 1;
		queueInfo.pQueuePriorities = &priority;
		const char* swapchain = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
		VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
		deviceInfo.queueCreateInfoCount = 1;
		deviceInfo.pQueueCreateInfos = &queueInfo;
		deviceInfo.enabledExtensionCount = display ? 1 : 0;
		deviceInfo.ppEnabledExtensionNames = &swapchain;
		result = vk::vkCreateDevice(m_physical, &deviceInfo, nullptr, &m_device);
		if (result != VK_SUCCESS)
			return Fail(error, "vkCreateDevice", result);
		if (!vk::LoadDevice(m_device))
			return Fail(error, std::string("the device has no ") + vk::Missing());
		vk::vkGetDeviceQueue(m_device, m_queueFamily, 0, &m_queue);
		Log(std::string("[ui] gfx: device on ") + properties.deviceName);

		VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
		poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
		poolInfo.queueFamilyIndex = m_queueFamily;
		result = vk::vkCreateCommandPool(m_device, &poolInfo, nullptr, &m_pool);
		if (result != VK_SUCCESS)
			return Fail(error, "vkCreateCommandPool", result);
		for (FrameSlot& slot : m_slots)
		{
			VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
			allocate.commandPool = m_pool;
			allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
			allocate.commandBufferCount = 1;
			VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
			fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;
			VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
			if (vk::vkAllocateCommandBuffers(m_device, &allocate, &slot.commands) != VK_SUCCESS ||
				vk::vkCreateFence(m_device, &fence, nullptr, &slot.done) != VK_SUCCESS ||
				vk::vkCreateSemaphore(m_device, &semaphore, nullptr, &slot.acquired) != VK_SUCCESS)
				return Fail(error, "the frames' command buffers");
		}

		if (!MakeTarget(error) || !MakePipeline(error))
			return false;

		// texture 0: one white pixel, for everything drawn without a picture
		m_textures.clear();
		m_freeTextures.clear();
		m_textures.emplace_back();
		const uint8_t white[4] = {255, 255, 255, 255};
		if (!MakeImage(m_textures[0].image, 1, 1, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT))
			return Fail(error, "the white texture");
		m_textures[0].width = m_textures[0].height = 1;
		m_textures[0].pending.assign(white, white + 4);
		m_textures[0].live = true;
		Log("[ui] gfx: ready, " + std::to_string(m_width) + "x" + std::to_string(m_height));
		return true;
	}

	bool Gfx::MakeTarget(std::string& error)
	{
		VkResult result;
		if (m_surface)
		{
			VkSurfaceCapabilitiesKHR caps{};
			result = vk::vkGetPhysicalDeviceSurfaceCapabilitiesKHR(m_physical, m_surface, &caps);
			if (result != VK_SUCCESS)
				return Fail(error, "the surface's capabilities", result);
			uint32_t formats = 0;
			vk::vkGetPhysicalDeviceSurfaceFormatsKHR(m_physical, m_surface, &formats, nullptr);
			std::vector<VkSurfaceFormatKHR> list(formats);
			vk::vkGetPhysicalDeviceSurfaceFormatsKHR(m_physical, m_surface, &formats, list.data());
			VkSurfaceFormatKHR chosen{VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
			bool found = false;
			for (const auto& format : list)
				if (format.format == VK_FORMAT_B8G8R8A8_UNORM)
				{
					chosen = format;
					found = true;
				}
			if (!found && !list.empty())
				chosen = list[0];
			m_format = chosen.format;
			m_width = caps.currentExtent.width != UINT32_MAX ? caps.currentExtent.width : m_target.width;
			m_height = caps.currentExtent.height != UINT32_MAX ? caps.currentExtent.height : m_target.height;
			VkSwapchainCreateInfoKHR info{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
			info.surface = m_surface;
			info.minImageCount = std::max(caps.minImageCount, 3u);
			if (caps.maxImageCount)
				info.minImageCount = std::min(info.minImageCount, caps.maxImageCount);
			info.imageFormat = chosen.format;
			info.imageColorSpace = chosen.colorSpace;
			info.imageExtent = {m_width, m_height};
			info.imageArrayLayers = 1;
			info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
			info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
			info.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
			info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
			info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
			info.clipped = VK_TRUE;
			result = vk::vkCreateSwapchainKHR(m_device, &info, nullptr, &m_swapchain);
			if (result != VK_SUCCESS)
				return Fail(error, "vkCreateSwapchainKHR", result);
			uint32_t images = 0;
			vk::vkGetSwapchainImagesKHR(m_device, m_swapchain, &images, nullptr);
			m_images.resize(images);
			vk::vkGetSwapchainImagesKHR(m_device, m_swapchain, &images, m_images.data());
			Log("[ui] gfx: swapchain " + std::to_string(m_width) + "x" + std::to_string(m_height) + ", " + std::to_string(images) +
				" images, format " + std::to_string((int)m_format));
		}
		else
		{
			m_width = m_target.width;
			m_height = m_target.height;
			m_format = VK_FORMAT_B8G8R8A8_UNORM;
			if (!MakeImage(m_offscreen, m_width, m_height, m_format, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT))
				return Fail(error, "the offscreen image");
			if (!MakeBuffer(m_readback, (VkDeviceSize)m_width * m_height * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT))
				return Fail(error, "the readback buffer");
			m_images = {m_offscreen.image};
		}

		// the render pass: cleared, drawn, then shown (or copied out)
		VkAttachmentDescription attachment{};
		attachment.format = m_format;
		attachment.samples = VK_SAMPLE_COUNT_1_BIT;
		attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
		attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		attachment.finalLayout = m_surface ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
		VkAttachmentReference colour{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
		VkSubpassDescription subpass{};
		subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
		subpass.colorAttachmentCount = 1;
		subpass.pColorAttachments = &colour;
		VkSubpassDependency dependency{};
		dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
		dependency.dstSubpass = 0;
		dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
		dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
		dependency.srcAccessMask = 0;
		dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		VkRenderPassCreateInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
		pass.attachmentCount = 1;
		pass.pAttachments = &attachment;
		pass.subpassCount = 1;
		pass.pSubpasses = &subpass;
		pass.dependencyCount = 1;
		pass.pDependencies = &dependency;
		result = vk::vkCreateRenderPass(m_device, &pass, nullptr, &m_renderPass);
		if (result != VK_SUCCESS)
			return Fail(error, "vkCreateRenderPass", result);

		for (size_t i = 0; i < m_images.size(); i++)
		{
			VkImageView view = m_offscreen.view;
			if (m_surface)
			{
				VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
				viewInfo.image = m_images[i];
				viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
				viewInfo.format = m_format;
				viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
				result = vk::vkCreateImageView(m_device, &viewInfo, nullptr, &view);
				if (result != VK_SUCCESS)
					return Fail(error, "a swapchain image's view", result);
				m_views.push_back(view);
				VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
				VkSemaphore rendered = VK_NULL_HANDLE;
				if (vk::vkCreateSemaphore(m_device, &semaphore, nullptr, &rendered) != VK_SUCCESS)
					return Fail(error, "a semaphore");
				m_rendered.push_back(rendered);
			}
			VkFramebufferCreateInfo framebuffer{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
			framebuffer.renderPass = m_renderPass;
			framebuffer.attachmentCount = 1;
			framebuffer.pAttachments = &view;
			framebuffer.width = m_width;
			framebuffer.height = m_height;
			framebuffer.layers = 1;
			VkFramebuffer made = VK_NULL_HANDLE;
			result = vk::vkCreateFramebuffer(m_device, &framebuffer, nullptr, &made);
			if (result != VK_SUCCESS)
				return Fail(error, "a framebuffer", result);
			m_framebuffers.push_back(made);
		}
		return true;
	}

	bool Gfx::MakePipeline(std::string& error)
	{
		VkResult result;
		// the pipeline cache from the last session, when there is one
		std::vector<char> cached;
		if (!m_target.pipelineCache.empty())
		{
			std::ifstream in(m_target.pipelineCache, std::ios::binary);
			cached.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
		}
		VkPipelineCacheCreateInfo cacheInfo{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
		cacheInfo.initialDataSize = cached.size();
		cacheInfo.pInitialData = cached.empty() ? nullptr : cached.data();
		if (vk::vkCreatePipelineCache(m_device, &cacheInfo, nullptr, &m_cache) != VK_SUCCESS)
		{
			cacheInfo.initialDataSize = 0;
			cacheInfo.pInitialData = nullptr;
			vk::vkCreatePipelineCache(m_device, &cacheInfo, nullptr, &m_cache);
		}

		VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
		sampler.magFilter = VK_FILTER_LINEAR;
		sampler.minFilter = VK_FILTER_LINEAR;
		sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
		sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		sampler.maxLod = 0.0f;
		result = vk::vkCreateSampler(m_device, &sampler, nullptr, &m_sampler);
		if (result != VK_SUCCESS)
			return Fail(error, "vkCreateSampler", result);

		VkDescriptorSetLayoutBinding bindings[2]{};
		for (uint32_t i = 0; i < 2; i++)
		{
			bindings[i].binding = i;
			bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
			bindings[i].descriptorCount = 1;
			bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
			bindings[i].pImmutableSamplers = &m_sampler;
		}
		VkDescriptorSetLayoutCreateInfo setLayout{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
		setLayout.bindingCount = 2;
		setLayout.pBindings = bindings;
		result = vk::vkCreateDescriptorSetLayout(m_device, &setLayout, nullptr, &m_setLayout);
		if (result != VK_SUCCESS)
			return Fail(error, "vkCreateDescriptorSetLayout", result);

		VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2048};
		VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
		poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
		poolInfo.maxSets = 1024;
		poolInfo.poolSizeCount = 1;
		poolInfo.pPoolSizes = &poolSize;
		result = vk::vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &m_descriptors);
		if (result != VK_SUCCESS)
			return Fail(error, "vkCreateDescriptorPool", result);

		VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16};
		VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
		layoutInfo.setLayoutCount = 1;
		layoutInfo.pSetLayouts = &m_setLayout;
		layoutInfo.pushConstantRangeCount = 1;
		layoutInfo.pPushConstantRanges = &push;
		result = vk::vkCreatePipelineLayout(m_device, &layoutInfo, nullptr, &m_layout);
		if (result != VK_SUCCESS)
			return Fail(error, "vkCreatePipelineLayout", result);

		VkShaderModule modules[2]{};
		const std::pair<const uint32_t*, size_t> code[2] = {{shaders::k_ui_vert, sizeof(shaders::k_ui_vert)}, {shaders::k_ui_frag, sizeof(shaders::k_ui_frag)}};
		for (int i = 0; i < 2; i++)
		{
			VkShaderModuleCreateInfo module{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
			module.codeSize = code[i].second;
			module.pCode = code[i].first;
			result = vk::vkCreateShaderModule(m_device, &module, nullptr, &modules[i]);
			if (result != VK_SUCCESS)
			{
				if (modules[0])
					vk::vkDestroyShaderModule(m_device, modules[0], nullptr);
				return Fail(error, "vkCreateShaderModule", result);
			}
		}
		VkPipelineShaderStageCreateInfo stages[2]{};
		for (int i = 0; i < 2; i++)
		{
			stages[i].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
			stages[i].stage = i == 0 ? VK_SHADER_STAGE_VERTEX_BIT : VK_SHADER_STAGE_FRAGMENT_BIT;
			stages[i].module = modules[i];
			stages[i].pName = "main";
		}

		VkVertexInputBindingDescription binding{0, sizeof(Instance), VK_VERTEX_INPUT_RATE_INSTANCE};
		const VkVertexInputAttributeDescription attributes[] = {
			{0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Instance, rect)},
			{1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Instance, box)},
			{2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Instance, shape)},
			{3, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(Instance, colour0)},
			{4, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(Instance, colour1)},
			{5, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Instance, gradient)},
			{6, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Instance, uv)},
			{7, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Instance, clip)},
			{8, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Instance, extra)},
		};
		VkPipelineVertexInputStateCreateInfo vertexInput{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
		vertexInput.vertexBindingDescriptionCount = 1;
		vertexInput.pVertexBindingDescriptions = &binding;
		vertexInput.vertexAttributeDescriptionCount = (uint32_t)std::size(attributes);
		vertexInput.pVertexAttributeDescriptions = attributes;
		VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
		assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
		VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
		viewport.viewportCount = 1;
		viewport.scissorCount = 1;
		VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
		raster.polygonMode = VK_POLYGON_MODE_FILL;
		raster.cullMode = VK_CULL_MODE_NONE;
		raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
		raster.lineWidth = 1.0f;
		VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
		multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
		VkPipelineColorBlendAttachmentState blend{};
		blend.blendEnable = VK_TRUE;
		blend.srcColorBlendFactor = VK_BLEND_FACTOR_ONE; // premultiplied
		blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
		blend.colorBlendOp = VK_BLEND_OP_ADD;
		blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
		blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
		blend.alphaBlendOp = VK_BLEND_OP_ADD;
		blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
		VkPipelineColorBlendStateCreateInfo blendState{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
		blendState.attachmentCount = 1;
		blendState.pAttachments = &blend;
		const VkDynamicState dynamics[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
		VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
		dynamic.dynamicStateCount = 2;
		dynamic.pDynamicStates = dynamics;

		VkGraphicsPipelineCreateInfo pipeline{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
		pipeline.stageCount = 2;
		pipeline.pStages = stages;
		pipeline.pVertexInputState = &vertexInput;
		pipeline.pInputAssemblyState = &assembly;
		pipeline.pViewportState = &viewport;
		pipeline.pRasterizationState = &raster;
		pipeline.pMultisampleState = &multisample;
		pipeline.pColorBlendState = &blendState;
		pipeline.pDynamicState = &dynamic;
		pipeline.layout = m_layout;
		pipeline.renderPass = m_renderPass;
		pipeline.subpass = 0;
		result = vk::vkCreateGraphicsPipelines(m_device, m_cache, 1, &pipeline, nullptr, &m_pipeline);
		vk::vkDestroyShaderModule(m_device, modules[0], nullptr);
		vk::vkDestroyShaderModule(m_device, modules[1], nullptr);
		if (result != VK_SUCCESS)
			return Fail(error, "vkCreateGraphicsPipelines", result);
		Log(std::string("[ui] gfx: pipeline") + (cached.empty() ? "" : " (pipeline cache read)"));
		return true;
	}

	VkDescriptorSet Gfx::MakeSet(VkImageView image)
	{
		VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
		allocate.descriptorPool = m_descriptors;
		allocate.descriptorSetCount = 1;
		allocate.pSetLayouts = &m_setLayout;
		VkDescriptorSet set = VK_NULL_HANDLE;
		if (vk::vkAllocateDescriptorSets(m_device, &allocate, &set) != VK_SUCCESS)
			return VK_NULL_HANDLE;
		VkDescriptorImageInfo images[2]{};
		images[0] = {m_sampler, m_atlas.view ? m_atlas.view : image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
		images[1] = {m_sampler, image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
		VkWriteDescriptorSet writes[2]{};
		for (uint32_t i = 0; i < 2; i++)
		{
			writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
			writes[i].dstSet = set;
			writes[i].dstBinding = i;
			writes[i].descriptorCount = 1;
			writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
			writes[i].pImageInfo = &images[i];
		}
		vk::vkUpdateDescriptorSets(m_device, 2, writes, 0, nullptr);
		return set;
	}

	TextureId Gfx::CreateTexture(uint32_t width, uint32_t height, const uint8_t* rgba)
	{
		if (!m_device || width == 0 || height == 0)
			return 0;
		TextureId id;
		if (!m_freeTextures.empty())
		{
			id = m_freeTextures.back();
			m_freeTextures.pop_back();
		}
		else
		{
			id = (TextureId)m_textures.size();
			m_textures.emplace_back();
		}
		Texture& texture = m_textures[id];
		texture = {};
		if (!MakeImage(texture.image, width, height, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT))
		{
			m_freeTextures.push_back(id);
			return 0;
		}
		texture.width = width;
		texture.height = height;
		texture.pending.assign(rgba, rgba + (size_t)width * height * 4);
		texture.live = true;
		return id;
	}

	void Gfx::DestroyTexture(TextureId id)
	{
		if (!m_device || id == 0 || id >= m_textures.size() || !m_textures[id].live)
			return;
		Texture texture = std::move(m_textures[id]);
		m_textures[id] = {};
		// the frames still drawing with it finish first
		m_slots[(m_slot + kSlots - 1) % kSlots].garbage.push_back([this, texture]() mutable {
			if (texture.set)
				vk::vkFreeDescriptorSets(m_device, m_descriptors, 1, &texture.set);
			FreeImage(texture.image);
		});
		m_freeTextures.push_back(id);
	}

	void Gfx::SetAtlas(uint32_t width, uint32_t height, const uint8_t* pixels)
	{
		if (!m_device)
			return;
		if (m_atlas.image)
		{
			// a taller atlas (text.h grows it as a language's characters fill it): the old image goes
			// once the frames drawing with it are done
			m_slots[(m_slot + kSlots - 1) % kSlots].garbage.push_back([this, old = m_atlas]() mutable { FreeImage(old); });
			m_atlas = {};
		}
		if (!MakeImage(m_atlas, width, height, VK_FORMAT_R8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT))
		{
			Log("[ui] gfx: the glyph atlas could not be made");
			return;
		}
		m_atlasWidth = width;
		m_atlasHeight = height;
		m_atlasPixels = pixels;
		m_atlasDirtyFirst = 0;
		m_atlasDirtyEnd = height;
		m_atlasFresh = true;
		// sets made before it point at their own image for binding 0: made again, the old ones freed once
		// the frames using them are done
		for (Texture& texture : m_textures)
			if (texture.set)
			{
				m_slots[(m_slot + kSlots - 1) % kSlots].garbage.push_back([this, set = texture.set]() mutable {
					vk::vkFreeDescriptorSets(m_device, m_descriptors, 1, &set);
				});
				texture.set = VK_NULL_HANDLE;
			}
	}

	void Gfx::AtlasChanged(uint32_t firstRow, uint32_t rows)
	{
		if (!m_atlas.image || rows == 0)
			return;
		const uint32_t end = std::min(firstRow + rows, m_atlasHeight);
		if (m_atlasDirtyEnd <= m_atlasDirtyFirst)
		{
			m_atlasDirtyFirst = firstRow;
			m_atlasDirtyEnd = end;
		}
		else
		{
			m_atlasDirtyFirst = std::min(m_atlasDirtyFirst, firstRow);
			m_atlasDirtyEnd = std::max(m_atlasDirtyEnd, end);
		}
	}

	// Copies what changed into the GPU's images before the frame draws: the atlas's dirty rows, and the
	// textures made since the last frame, as many as the staging buffer's budget takes.
	void Gfx::RecordUploads(FrameSlot& slot, VkCommandBuffer commands)
	{
		constexpr VkDeviceSize kBudget = 48ull << 20;
		struct Copy
		{
			VkImage image;
			VkDeviceSize offset;
			uint32_t width, height, rowOffset;
			bool fresh;
		};
		std::vector<Copy> copies;
		std::vector<TextureId> uploaded;
		VkDeviceSize needed = 0;
		const bool atlas = m_atlas.image && m_atlasDirtyEnd > m_atlasDirtyFirst;
		const VkDeviceSize atlasBytes = atlas ? (VkDeviceSize)(m_atlasDirtyEnd - m_atlasDirtyFirst) * m_atlasWidth : 0;
		needed += (atlasBytes + 15) & ~VkDeviceSize(15);
		for (TextureId id = 0; id < m_textures.size(); id++)
		{
			const Texture& texture = m_textures[id];
			if (!texture.live || texture.pending.empty())
				continue;
			const VkDeviceSize bytes = (texture.pending.size() + 15) & ~size_t(15);
			if (!uploaded.empty() && needed + bytes > kBudget)
				break;
			needed += bytes;
			uploaded.push_back(id);
		}
		if (needed == 0)
			return;
		if (slot.staging.size < needed)
		{
			// the old one is this slot's, whose frame has finished
			FreeBuffer(slot.staging);
			VkDeviceSize size = 4ull << 20;
			while (size < needed)
				size *= 2;
			if (!MakeBuffer(slot.staging, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT))
			{
				Log("[ui] gfx: no staging buffer for the uploads");
				return;
			}
		}
		auto* staging = static_cast<uint8_t*>(slot.staging.mapped);
		VkDeviceSize at = 0;
		if (atlas)
		{
			std::memcpy(staging, m_atlasPixels + (size_t)m_atlasDirtyFirst * m_atlasWidth, atlasBytes);
			copies.push_back({m_atlas.image, 0, m_atlasWidth, m_atlasDirtyEnd - m_atlasDirtyFirst, m_atlasDirtyFirst, m_atlasFresh});
			at = (atlasBytes + 15) & ~VkDeviceSize(15);
			m_atlasFresh = false;
			m_atlasDirtyFirst = m_atlasDirtyEnd = 0;
		}
		for (TextureId id : uploaded)
		{
			Texture& texture = m_textures[id];
			std::memcpy(staging + at, texture.pending.data(), texture.pending.size());
			copies.push_back({texture.image.image, at, texture.width, texture.height, 0, true});
			at += (texture.pending.size() + 15) & ~size_t(15);
			texture.pending.clear();
			texture.pending.shrink_to_fit();
		}
		VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
		range.memory = slot.staging.memory;
		range.offset = 0;
		range.size = VK_WHOLE_SIZE;
		vk::vkFlushMappedMemoryRanges(m_device, 1, &range);

		for (const Copy& copy : copies)
		{
			VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
			barrier.oldLayout = copy.fresh ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
			barrier.srcAccessMask = copy.fresh ? 0 : VK_ACCESS_SHADER_READ_BIT;
			barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
			barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			barrier.image = copy.image;
			barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
			vk::vkCmdPipelineBarrier(commands, copy.fresh ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
				VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
			VkBufferImageCopy region{};
			region.bufferOffset = copy.offset;
			region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
			region.imageOffset = {0, (int32_t)copy.rowOffset, 0};
			region.imageExtent = {copy.width, copy.height, 1};
			vk::vkCmdCopyBufferToImage(commands, slot.staging.buffer, copy.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
			barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
			barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
			barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
			vk::vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr,
				1, &barrier);
		}
	}

	bool Gfx::Frame(const DrawList& list, float time)
	{
		if (!m_device)
			return false;
		const bool first = m_frames == 0;
		FrameSlot& slot = m_slots[m_slot];
		VkResult result = vk::vkWaitForFences(m_device, 1, &slot.done, VK_TRUE, UINT64_MAX);
		if (result != VK_SUCCESS)
		{
			Log("[ui] gfx: waiting for a frame: " + Describe(result));
			return false;
		}
		for (auto& destroy : slot.garbage)
			destroy();
		slot.garbage.clear();

		uint32_t image = 0;
		if (m_swapchain)
		{
			result = vk::vkAcquireNextImageKHR(m_device, m_swapchain, UINT64_MAX, slot.acquired, VK_NULL_HANDLE, &image);
			if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
			{
				Log("[ui] gfx: vkAcquireNextImageKHR: " + Describe(result));
				return false;
			}
			if (first)
				Log("[ui] gfx: first frame: image acquired");
		}
		vk::vkResetFences(m_device, 1, &slot.done);

		// the instances, into this slot's buffer (grown when the frame has more)
		const VkDeviceSize bytes = std::max<VkDeviceSize>(list.instances.size() * sizeof(Instance), sizeof(Instance));
		if (slot.instances.size < bytes)
		{
			FreeBuffer(slot.instances);
			VkDeviceSize size = 256 * 1024;
			while (size < bytes)
				size *= 2;
			if (!MakeBuffer(slot.instances, size, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT))
			{
				Log("[ui] gfx: no buffer for the frame's instances");
				return false;
			}
		}
		if (!list.instances.empty())
		{
			std::memcpy(slot.instances.mapped, list.instances.data(), list.instances.size() * sizeof(Instance));
			VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
			range.memory = slot.instances.memory;
			range.offset = 0;
			range.size = VK_WHOLE_SIZE;
			vk::vkFlushMappedMemoryRanges(m_device, 1, &range);
		}

		VkCommandBuffer commands = slot.commands;
		vk::vkResetCommandBuffer(commands, 0);
		VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
		begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
		vk::vkBeginCommandBuffer(commands, &begin);
		RecordUploads(slot, commands);
		// every texture drawn has its descriptor set
		for (const auto& run : list.runs)
			if (run.texture < m_textures.size())
			{
				Texture& texture = m_textures[run.texture];
				if (texture.live && !texture.set)
					texture.set = MakeSet(texture.image.view);
			}

		VkClearValue clear{};
		clear.color = {{0.0196f, 0.0275f, 0.051f, 1.0f}}; // ink-0, #05070d
		VkRenderPassBeginInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
		pass.renderPass = m_renderPass;
		pass.framebuffer = m_framebuffers[m_swapchain ? image : 0];
		pass.renderArea = {{0, 0}, {m_width, m_height}};
		pass.clearValueCount = 1;
		pass.pClearValues = &clear;
		vk::vkCmdBeginRenderPass(commands, &pass, VK_SUBPASS_CONTENTS_INLINE);
		VkViewport viewport{0.0f, 0.0f, (float)m_width, (float)m_height, 0.0f, 1.0f};
		VkRect2D scissor{{0, 0}, {m_width, m_height}};
		vk::vkCmdSetViewport(commands, 0, 1, &viewport);
		vk::vkCmdSetScissor(commands, 0, 1, &scissor);
		vk::vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);
		const float constants[4] = {1920.0f, 1080.0f, m_width / 1920.0f, time};
		vk::vkCmdPushConstants(commands, m_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(constants), constants);
		const VkDeviceSize offset = 0;
		vk::vkCmdBindVertexBuffers(commands, 0, 1, &slot.instances.buffer, &offset);
		VkDescriptorSet bound = VK_NULL_HANDLE;
		for (const auto& run : list.runs)
		{
			// a picture still waiting for its upload is not drawn yet (the screens show its placeholder)
			if (run.texture >= m_textures.size() || !m_textures[run.texture].live || !m_textures[run.texture].pending.empty())
				continue;
			const VkDescriptorSet set = m_textures[run.texture].set;
			if (!set)
				continue;
			if (set != bound)
			{
				vk::vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, m_layout, 0, 1, &set, 0, nullptr);
				bound = set;
			}
			vk::vkCmdDraw(commands, 6, run.count, 0, run.first);
		}
		vk::vkCmdEndRenderPass(commands);
		if (!m_swapchain)
		{
			VkBufferImageCopy region{};
			region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
			region.imageExtent = {m_width, m_height, 1};
			vk::vkCmdCopyImageToBuffer(commands, m_offscreen.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, m_readback.buffer, 1, &region);
		}
		vk::vkEndCommandBuffer(commands);

		const VkPipelineStageFlags wait = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
		VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
		submit.commandBufferCount = 1;
		submit.pCommandBuffers = &commands;
		if (m_swapchain)
		{
			submit.waitSemaphoreCount = 1;
			submit.pWaitSemaphores = &slot.acquired;
			submit.pWaitDstStageMask = &wait;
			submit.signalSemaphoreCount = 1;
			submit.pSignalSemaphores = &m_rendered[image];
		}
		result = vk::vkQueueSubmit(m_queue, 1, &submit, slot.done);
		if (result != VK_SUCCESS)
		{
			Log("[ui] gfx: vkQueueSubmit: " + Describe(result));
			return false;
		}
		if (first)
			Log("[ui] gfx: first frame: " + std::to_string(list.instances.size()) + " instances in " + std::to_string(list.runs.size()) +
				" runs submitted");
		if (m_swapchain)
		{
			VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
			present.waitSemaphoreCount = 1;
			present.pWaitSemaphores = &m_rendered[image];
			present.swapchainCount = 1;
			present.pSwapchains = &m_swapchain;
			present.pImageIndices = &image;
			result = vk::vkQueuePresentKHR(m_queue, &present);
			if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
			{
				Log("[ui] gfx: vkQueuePresentKHR: " + Describe(result));
				return false;
			}
			if (first)
				Log("[ui] gfx: first frame presented");
		}
		else
		{
			vk::vkWaitForFences(m_device, 1, &slot.done, VK_TRUE, UINT64_MAX);
			VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
			range.memory = m_readback.memory;
			range.size = VK_WHOLE_SIZE;
			vk::vkInvalidateMappedMemoryRanges(m_device, 1, &range);
			if (m_target.readback)
				m_target.readback(static_cast<const uint8_t*>(m_readback.mapped), m_width, m_height, (size_t)m_width * 4);
		}
		m_slot = (m_slot + 1) % kSlots;
		m_frames++;
		return true;
	}

	void Gfx::SavePipelineCache()
	{
		if (!m_cache || m_target.pipelineCache.empty())
			return;
		size_t size = 0;
		if (vk::vkGetPipelineCacheData(m_device, m_cache, &size, nullptr) != VK_SUCCESS || size == 0)
			return;
		std::vector<char> data(size);
		if (vk::vkGetPipelineCacheData(m_device, m_cache, &size, data.data()) != VK_SUCCESS)
			return;
		std::ofstream out(m_target.pipelineCache, std::ios::binary | std::ios::trunc);
		out.write(data.data(), (std::streamsize)size);
	}

	void Gfx::Stop()
	{
		if (m_instance == VK_NULL_HANDLE)
			return;
		if (m_device)
		{
			vk::vkDeviceWaitIdle(m_device);
			SavePipelineCache();
			for (FrameSlot& slot : m_slots)
			{
				for (auto& destroy : slot.garbage)
					destroy();
				slot.garbage.clear();
				FreeBuffer(slot.instances);
				FreeBuffer(slot.staging);
				if (slot.done)
					vk::vkDestroyFence(m_device, slot.done, nullptr);
				if (slot.acquired)
					vk::vkDestroySemaphore(m_device, slot.acquired, nullptr);
				slot = {};
			}
			for (Texture& texture : m_textures)
				FreeImage(texture.image);
			m_textures.clear();
			m_freeTextures.clear();
			FreeImage(m_atlas);
			m_atlasPixels = nullptr;
			if (m_pipeline)
				vk::vkDestroyPipeline(m_device, m_pipeline, nullptr);
			if (m_layout)
				vk::vkDestroyPipelineLayout(m_device, m_layout, nullptr);
			if (m_descriptors)
				vk::vkDestroyDescriptorPool(m_device, m_descriptors, nullptr);
			if (m_setLayout)
				vk::vkDestroyDescriptorSetLayout(m_device, m_setLayout, nullptr);
			if (m_sampler)
				vk::vkDestroySampler(m_device, m_sampler, nullptr);
			if (m_cache)
				vk::vkDestroyPipelineCache(m_device, m_cache, nullptr);
			for (VkFramebuffer framebuffer : m_framebuffers)
				vk::vkDestroyFramebuffer(m_device, framebuffer, nullptr);
			for (VkImageView view : m_views)
				vk::vkDestroyImageView(m_device, view, nullptr);
			for (VkSemaphore semaphore : m_rendered)
				vk::vkDestroySemaphore(m_device, semaphore, nullptr);
			if (m_renderPass)
				vk::vkDestroyRenderPass(m_device, m_renderPass, nullptr);
			if (m_swapchain)
				vk::vkDestroySwapchainKHR(m_device, m_swapchain, nullptr);
			FreeImage(m_offscreen);
			FreeBuffer(m_readback);
			if (m_pool)
				vk::vkDestroyCommandPool(m_device, m_pool, nullptr);
			vk::vkDestroyDevice(m_device, nullptr);
		}
		if (m_surface && vk::vkDestroySurfaceKHR)
			vk::vkDestroySurfaceKHR(m_instance, m_surface, nullptr);
		vk::vkDestroyInstance(m_instance, nullptr);
		const bool logged = m_frames > 0;
		*this = Gfx();
		if (logged)
			Log("[ui] gfx: stopped");
	}
}
