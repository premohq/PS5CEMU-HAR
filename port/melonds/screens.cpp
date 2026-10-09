// SPDX-License-Identifier: GPL-3.0-or-later
// PS5CEMU-HAR: the DS's two screens on the TV (screens.h). Set up as the UI kit and Cemu's renderer
// are on the console, where both are known to draw: B8G8R8A8_UNORM and FIFO, a render pass that
// clears, host-visible buffers flushed after writing, device-local optimal-tiled images filled by a
// copy from a staging buffer. Each step of the first frame is logged, so a game that shows nothing
// says where it stopped.

#include "screens.h"
#include "shaders.h"
#include "../app/ingame3ds.h"
#include "../ps5/display.h"
#include "../ps5/log.h"
#include "../ps5/vulkan_display.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>
#include <vector>

namespace ps5melonds::screens
{
	namespace
	{
		// The Vulkan functions this calls, loaded through the driver's vkGetInstanceProcAddr into a
		// table of their own (the names stay clear of Cemu's, which are global)
#define DS_VK_INSTANCE_FUNCTIONS(X)                                                                                    \
	X(DestroyInstance)                                                                                                 \
	X(EnumeratePhysicalDevices)                                                                                        \
	X(GetPhysicalDeviceProperties)                                                                                     \
	X(GetPhysicalDeviceQueueFamilyProperties)                                                                          \
	X(GetPhysicalDeviceMemoryProperties)                                                                               \
	X(CreateDevice)                                                                                                    \
	X(GetDeviceProcAddr)                                                                                               \
	X(GetPhysicalDeviceSurfaceCapabilitiesKHR)                                                                         \
	X(GetPhysicalDeviceSurfaceFormatsKHR)                                                                              \
	X(DestroySurfaceKHR)

#define DS_VK_DEVICE_FUNCTIONS(X)                                                                                      \
	X(DestroyDevice)                                                                                                   \
	X(GetDeviceQueue)                                                                                                  \
	X(DeviceWaitIdle)                                                                                                  \
	X(QueueSubmit)                                                                                                     \
	X(CreateSwapchainKHR)                                                                                              \
	X(DestroySwapchainKHR)                                                                                             \
	X(GetSwapchainImagesKHR)                                                                                           \
	X(AcquireNextImageKHR)                                                                                             \
	X(QueuePresentKHR)                                                                                                 \
	X(CreateCommandPool)                                                                                               \
	X(DestroyCommandPool)                                                                                              \
	X(AllocateCommandBuffers)                                                                                          \
	X(ResetCommandBuffer)                                                                                              \
	X(BeginCommandBuffer)                                                                                              \
	X(EndCommandBuffer)                                                                                                \
	X(CreateFence)                                                                                                     \
	X(DestroyFence)                                                                                                    \
	X(WaitForFences)                                                                                                   \
	X(ResetFences)                                                                                                     \
	X(CreateSemaphore)                                                                                                 \
	X(DestroySemaphore)                                                                                                \
	X(CreateRenderPass)                                                                                                \
	X(DestroyRenderPass)                                                                                               \
	X(CreateFramebuffer)                                                                                               \
	X(DestroyFramebuffer)                                                                                              \
	X(CreateImageView)                                                                                                 \
	X(DestroyImageView)                                                                                                \
	X(CreateImage)                                                                                                     \
	X(DestroyImage)                                                                                                    \
	X(GetImageMemoryRequirements)                                                                                      \
	X(BindImageMemory)                                                                                                 \
	X(CreateBuffer)                                                                                                    \
	X(DestroyBuffer)                                                                                                   \
	X(GetBufferMemoryRequirements)                                                                                     \
	X(BindBufferMemory)                                                                                                \
	X(AllocateMemory)                                                                                                  \
	X(FreeMemory)                                                                                                      \
	X(MapMemory)                                                                                                       \
	X(UnmapMemory)                                                                                                     \
	X(FlushMappedMemoryRanges)                                                                                         \
	X(CreateShaderModule)                                                                                              \
	X(DestroyShaderModule)                                                                                             \
	X(CreatePipelineLayout)                                                                                            \
	X(DestroyPipelineLayout)                                                                                           \
	X(CreateGraphicsPipelines)                                                                                         \
	X(DestroyPipeline)                                                                                                 \
	X(CreateDescriptorSetLayout)                                                                                       \
	X(DestroyDescriptorSetLayout)                                                                                      \
	X(CreateDescriptorPool)                                                                                            \
	X(DestroyDescriptorPool)                                                                                           \
	X(AllocateDescriptorSets)                                                                                          \
	X(UpdateDescriptorSets)                                                                                            \
	X(CreateSampler)                                                                                                   \
	X(DestroySampler)                                                                                                  \
	X(CmdBeginRenderPass)                                                                                              \
	X(CmdEndRenderPass)                                                                                                \
	X(CmdBindPipeline)                                                                                                 \
	X(CmdBindDescriptorSets)                                                                                           \
	X(CmdDraw)                                                                                                         \
	X(CmdSetViewport)                                                                                                  \
	X(CmdSetScissor)                                                                                                   \
	X(CmdPushConstants)                                                                                                \
	X(CmdPipelineBarrier)                                                                                              \
	X(CmdCopyBufferToImage)

		struct Functions
		{
			PFN_vkGetInstanceProcAddr GetInstanceProcAddr = nullptr;
			PFN_vkCreateInstance CreateInstance = nullptr;
#define DS_VK_MEMBER(name) PFN_vk##name name = nullptr;
			DS_VK_INSTANCE_FUNCTIONS(DS_VK_MEMBER)
			DS_VK_DEVICE_FUNCTIONS(DS_VK_MEMBER)
#undef DS_VK_MEMBER
		};
		Functions vk;
		const char* s_missing = "";

		bool LoadInstance(VkInstance instance)
		{
#define DS_VK_LOAD(name)                                                                                               \
	vk.name = (PFN_vk##name)vk.GetInstanceProcAddr(instance, "vk" #name);                                              \
	if (!vk.name)                                                                                                      \
	{                                                                                                                  \
		s_missing = "vk" #name;                                                                                        \
		return false;                                                                                                  \
	}
			DS_VK_INSTANCE_FUNCTIONS(DS_VK_LOAD)
#undef DS_VK_LOAD
			return true;
		}

		bool LoadDevice(VkDevice device)
		{
#define DS_VK_LOAD(name)                                                                                               \
	vk.name = (PFN_vk##name)vk.GetDeviceProcAddr(device, "vk" #name);                                                  \
	if (!vk.name)                                                                                                      \
	{                                                                                                                  \
		s_missing = "vk" #name;                                                                                        \
		return false;                                                                                                  \
	}
			DS_VK_DEVICE_FUNCTIONS(DS_VK_LOAD)
#undef DS_VK_LOAD
			return true;
		}

		struct Buffer
		{
			VkBuffer buffer = VK_NULL_HANDLE;
			VkDeviceMemory memory = VK_NULL_HANDLE;
			void* mapped = nullptr;
		};

		struct Image
		{
			VkImage image = VK_NULL_HANDLE;
			VkDeviceMemory memory = VK_NULL_HANDLE;
			VkImageView view = VK_NULL_HANDLE;
			VkDescriptorSet set = VK_NULL_HANDLE;
			bool filled = false; // a copy has been made into it (its layout is no longer UNDEFINED)
		};

		struct Slot
		{
			VkCommandBuffer commands = VK_NULL_HANDLE;
			VkFence done = VK_NULL_HANDLE;
			VkSemaphore acquired = VK_NULL_HANDLE;
			Buffer staging; // the two screens' pixels
		};

		// What the shaders take (shaders/screens.vert): the quad, the screen's size, the filter
		struct Constants
		{
			float rect[4];
			float size[2];
			float mode;
			float padding;
		};
		static_assert(sizeof(Constants) == 32);

		constexpr VkDeviceSize kScreenBytes = (VkDeviceSize)kScreenWidth * kScreenHeight * 4;
		constexpr int kSlots = 2;

		struct State
		{
			VkInstance instance = VK_NULL_HANDLE;
			VkPhysicalDevice physical = VK_NULL_HANDLE;
			VkPhysicalDeviceMemoryProperties memory{};
			VkDevice device = VK_NULL_HANDLE;
			uint32_t queueFamily = 0;
			VkQueue queue = VK_NULL_HANDLE;
			VkSurfaceKHR surface = VK_NULL_HANDLE;
			VkSwapchainKHR swapchain = VK_NULL_HANDLE;
			VkFormat format = VK_FORMAT_B8G8R8A8_UNORM;
			uint32_t width = 0, height = 0;
			std::vector<VkImage> images;
			std::vector<VkImageView> views;
			std::vector<VkFramebuffer> framebuffers;
			std::vector<VkSemaphore> rendered; // one per swapchain image
			VkRenderPass renderPass = VK_NULL_HANDLE;
			VkCommandPool pool = VK_NULL_HANDLE;
			VkSampler sampler = VK_NULL_HANDLE;
			VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
			VkDescriptorPool descriptors = VK_NULL_HANDLE;
			VkPipelineLayout layout = VK_NULL_HANDLE;
			VkPipeline screenPipeline = VK_NULL_HANDLE;
			VkPipeline cursorPipeline = VK_NULL_HANDLE;
			Image screens[2]; // top, bottom
			Slot slots[kSlots];
			int slot = 0;
			uint64_t frames = 0;
		};
		State s;

		const char* ResultName(VkResult result)
		{
			switch (result)
			{
			case VK_SUCCESS: return "VK_SUCCESS";
			case VK_TIMEOUT: return "VK_TIMEOUT";
			case VK_SUBOPTIMAL_KHR: return "VK_SUBOPTIMAL_KHR";
			case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
			case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
			case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
			case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
			case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
			case VK_ERROR_SURFACE_LOST_KHR: return "VK_ERROR_SURFACE_LOST_KHR";
			case VK_ERROR_NATIVE_WINDOW_IN_USE_KHR: return "VK_ERROR_NATIVE_WINDOW_IN_USE_KHR";
			case VK_ERROR_OUT_OF_DATE_KHR: return "VK_ERROR_OUT_OF_DATE_KHR";
			default: return "VkResult";
			}
		}

		std::string Describe(VkResult result)
		{
			return fmt::format("{} ({})", ResultName(result), (int)result);
		}

		bool Fail(std::string& error, const std::string& what, VkResult result = VK_SUCCESS)
		{
			error = result == VK_SUCCESS ? what : what + ": " + Describe(result);
			ps5log::Line("[ds] screens: {}", error);
			Stop();
			return false;
		}

		uint32_t MemoryType(uint32_t bits, VkMemoryPropertyFlags properties)
		{
			for (uint32_t i = 0; i < s.memory.memoryTypeCount; i++)
				if ((bits & (1u << i)) && (s.memory.memoryTypes[i].propertyFlags & properties) == properties)
					return i;
			// any the resource can live in: on the PS5 every type is the same memory
			for (uint32_t i = 0; i < s.memory.memoryTypeCount; i++)
				if (bits & (1u << i))
					return i;
			return 0;
		}

		bool MakeBuffer(Buffer& buffer, VkDeviceSize size)
		{
			VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
			info.size = size;
			info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
			info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
			if (vk.CreateBuffer(s.device, &info, nullptr, &buffer.buffer) != VK_SUCCESS)
				return false;
			VkMemoryRequirements requirements;
			vk.GetBufferMemoryRequirements(s.device, buffer.buffer, &requirements);
			VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
			allocate.allocationSize = requirements.size;
			allocate.memoryTypeIndex =
				MemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
			return vk.AllocateMemory(s.device, &allocate, nullptr, &buffer.memory) == VK_SUCCESS &&
				vk.BindBufferMemory(s.device, buffer.buffer, buffer.memory, 0) == VK_SUCCESS &&
				vk.MapMemory(s.device, buffer.memory, 0, VK_WHOLE_SIZE, 0, &buffer.mapped) == VK_SUCCESS;
		}

		void FreeBuffer(Buffer& buffer)
		{
			if (buffer.mapped)
				vk.UnmapMemory(s.device, buffer.memory);
			if (buffer.buffer)
				vk.DestroyBuffer(s.device, buffer.buffer, nullptr);
			if (buffer.memory)
				vk.FreeMemory(s.device, buffer.memory, nullptr);
			buffer = {};
		}

		bool MakeScreen(Image& image)
		{
			VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
			info.imageType = VK_IMAGE_TYPE_2D;
			info.format = VK_FORMAT_B8G8R8A8_UNORM; // melonDS's pixels, as they are in memory
			info.extent = {(uint32_t)kScreenWidth, (uint32_t)kScreenHeight, 1};
			info.mipLevels = 1;
			info.arrayLayers = 1;
			info.samples = VK_SAMPLE_COUNT_1_BIT;
			info.tiling = VK_IMAGE_TILING_OPTIMAL;
			info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
			info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
			info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
			if (vk.CreateImage(s.device, &info, nullptr, &image.image) != VK_SUCCESS)
				return false;
			VkMemoryRequirements requirements;
			vk.GetImageMemoryRequirements(s.device, image.image, &requirements);
			VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
			allocate.allocationSize = requirements.size;
			allocate.memoryTypeIndex = MemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
			if (vk.AllocateMemory(s.device, &allocate, nullptr, &image.memory) != VK_SUCCESS ||
				vk.BindImageMemory(s.device, image.image, image.memory, 0) != VK_SUCCESS)
				return false;
			VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
			view.image = image.image;
			view.viewType = VK_IMAGE_VIEW_TYPE_2D;
			view.format = VK_FORMAT_B8G8R8A8_UNORM;
			view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
			if (vk.CreateImageView(s.device, &view, nullptr, &image.view) != VK_SUCCESS)
				return false;
			VkDescriptorSetAllocateInfo set{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
			set.descriptorPool = s.descriptors;
			set.descriptorSetCount = 1;
			set.pSetLayouts = &s.setLayout;
			if (vk.AllocateDescriptorSets(s.device, &set, &image.set) != VK_SUCCESS)
				return false;
			VkDescriptorImageInfo described{s.sampler, image.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
			VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
			write.dstSet = image.set;
			write.dstBinding = 0;
			write.descriptorCount = 1;
			write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
			write.pImageInfo = &described;
			vk.UpdateDescriptorSets(s.device, 1, &write, 0, nullptr);
			return true;
		}

		void FreeScreen(Image& image)
		{
			if (image.view)
				vk.DestroyImageView(s.device, image.view, nullptr);
			if (image.image)
				vk.DestroyImage(s.device, image.image, nullptr);
			if (image.memory)
				vk.FreeMemory(s.device, image.memory, nullptr);
			image = {}; // its set goes with the pool
		}

		bool MakeSwapchain(std::string& error)
		{
			VkSurfaceCapabilitiesKHR caps{};
			VkResult result = vk.GetPhysicalDeviceSurfaceCapabilitiesKHR(s.physical, s.surface, &caps);
			if (result != VK_SUCCESS)
				return Fail(error, "the surface's capabilities", result);
			uint32_t formats = 0;
			vk.GetPhysicalDeviceSurfaceFormatsKHR(s.physical, s.surface, &formats, nullptr);
			std::vector<VkSurfaceFormatKHR> list(formats);
			vk.GetPhysicalDeviceSurfaceFormatsKHR(s.physical, s.surface, &formats, list.data());
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
			s.format = chosen.format;
			s.width = caps.currentExtent.width != UINT32_MAX ? caps.currentExtent.width : ps5display::kWidth;
			s.height = caps.currentExtent.height != UINT32_MAX ? caps.currentExtent.height : ps5display::kHeight;
			VkSwapchainCreateInfoKHR info{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
			info.surface = s.surface;
			info.minImageCount = std::max(caps.minImageCount, 3u);
			if (caps.maxImageCount)
				info.minImageCount = std::min(info.minImageCount, caps.maxImageCount);
			info.imageFormat = chosen.format;
			info.imageColorSpace = chosen.colorSpace;
			info.imageExtent = {s.width, s.height};
			info.imageArrayLayers = 1;
			info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
			info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
			info.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
			info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
			info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
			info.clipped = VK_TRUE;
			result = vk.CreateSwapchainKHR(s.device, &info, nullptr, &s.swapchain);
			if (result != VK_SUCCESS)
				return Fail(error, "vkCreateSwapchainKHR", result);
			uint32_t images = 0;
			vk.GetSwapchainImagesKHR(s.device, s.swapchain, &images, nullptr);
			s.images.resize(images);
			vk.GetSwapchainImagesKHR(s.device, s.swapchain, &images, s.images.data());
			ps5log::Line("[ds] screens: swapchain {}x{}, {} images, format {}", s.width, s.height, images, (int)s.format);

			// the render pass: cleared (black, as Azahar's screens are on), drawn, then shown
			VkAttachmentDescription attachment{};
			attachment.format = s.format;
			attachment.samples = VK_SAMPLE_COUNT_1_BIT;
			attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
			attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
			attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
			attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
			attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
			attachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
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
			result = vk.CreateRenderPass(s.device, &pass, nullptr, &s.renderPass);
			if (result != VK_SUCCESS)
				return Fail(error, "vkCreateRenderPass", result);

			for (VkImage image : s.images)
			{
				VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
				viewInfo.image = image;
				viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
				viewInfo.format = s.format;
				viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
				VkImageView view = VK_NULL_HANDLE;
				result = vk.CreateImageView(s.device, &viewInfo, nullptr, &view);
				if (result != VK_SUCCESS)
					return Fail(error, "a swapchain image's view", result);
				s.views.push_back(view);
				VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
				VkSemaphore rendered = VK_NULL_HANDLE;
				if (vk.CreateSemaphore(s.device, &semaphore, nullptr, &rendered) != VK_SUCCESS)
					return Fail(error, "a semaphore");
				s.rendered.push_back(rendered);
				VkFramebufferCreateInfo framebuffer{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
				framebuffer.renderPass = s.renderPass;
				framebuffer.attachmentCount = 1;
				framebuffer.pAttachments = &view;
				framebuffer.width = s.width;
				framebuffer.height = s.height;
				framebuffer.layers = 1;
				VkFramebuffer made = VK_NULL_HANDLE;
				result = vk.CreateFramebuffer(s.device, &framebuffer, nullptr, &made);
				if (result != VK_SUCCESS)
					return Fail(error, "a framebuffer", result);
				s.framebuffers.push_back(made);
			}
			return true;
		}

		bool MakePipelines(std::string& error)
		{
			VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
			sampler.magFilter = VK_FILTER_LINEAR;
			sampler.minFilter = VK_FILTER_LINEAR;
			sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
			sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
			sampler.maxLod = 0.0f;
			VkResult result = vk.CreateSampler(s.device, &sampler, nullptr, &s.sampler);
			if (result != VK_SUCCESS)
				return Fail(error, "vkCreateSampler", result);

			VkDescriptorSetLayoutBinding binding{};
			binding.binding = 0;
			binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
			binding.descriptorCount = 1;
			binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
			binding.pImmutableSamplers = &s.sampler;
			VkDescriptorSetLayoutCreateInfo setLayout{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
			setLayout.bindingCount = 1;
			setLayout.pBindings = &binding;
			result = vk.CreateDescriptorSetLayout(s.device, &setLayout, nullptr, &s.setLayout);
			if (result != VK_SUCCESS)
				return Fail(error, "vkCreateDescriptorSetLayout", result);

			VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2};
			VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
			poolInfo.maxSets = 2;
			poolInfo.poolSizeCount = 1;
			poolInfo.pPoolSizes = &poolSize;
			result = vk.CreateDescriptorPool(s.device, &poolInfo, nullptr, &s.descriptors);
			if (result != VK_SUCCESS)
				return Fail(error, "vkCreateDescriptorPool", result);

			VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(Constants)};
			VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
			layoutInfo.setLayoutCount = 1;
			layoutInfo.pSetLayouts = &s.setLayout;
			layoutInfo.pushConstantRangeCount = 1;
			layoutInfo.pPushConstantRanges = &push;
			result = vk.CreatePipelineLayout(s.device, &layoutInfo, nullptr, &s.layout);
			if (result != VK_SUCCESS)
				return Fail(error, "vkCreatePipelineLayout", result);

			VkShaderModule modules[2]{};
			const std::pair<const uint32_t*, size_t> code[2] = {{shaders::k_screens_vert, sizeof(shaders::k_screens_vert)},
				{shaders::k_screens_frag, sizeof(shaders::k_screens_frag)}};
			for (int i = 0; i < 2; i++)
			{
				VkShaderModuleCreateInfo module{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
				module.codeSize = code[i].second;
				module.pCode = code[i].first;
				result = vk.CreateShaderModule(s.device, &module, nullptr, &modules[i]);
				if (result != VK_SUCCESS)
				{
					if (modules[0])
						vk.DestroyShaderModule(s.device, modules[0], nullptr);
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
			VkPipelineVertexInputStateCreateInfo vertexInput{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
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
			// the screens replace what is under them; the cursor inverts it, as Azahar's crosshair does
			VkPipelineColorBlendAttachmentState blend{};
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
			pipeline.layout = s.layout;
			pipeline.renderPass = s.renderPass;
			pipeline.subpass = 0;
			result = vk.CreateGraphicsPipelines(s.device, VK_NULL_HANDLE, 1, &pipeline, nullptr, &s.screenPipeline);
			if (result == VK_SUCCESS)
			{
				blend.blendEnable = VK_TRUE;
				blend.srcColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
				blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
				blend.colorBlendOp = VK_BLEND_OP_ADD;
				blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
				blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
				blend.alphaBlendOp = VK_BLEND_OP_ADD;
				result = vk.CreateGraphicsPipelines(s.device, VK_NULL_HANDLE, 1, &pipeline, nullptr, &s.cursorPipeline);
			}
			vk.DestroyShaderModule(s.device, modules[0], nullptr);
			vk.DestroyShaderModule(s.device, modules[1], nullptr);
			if (result != VK_SUCCESS)
				return Fail(error, "vkCreateGraphicsPipelines", result);
			return true;
		}

		// A quad on the picture, in clip space
		void Draw(VkCommandBuffer commands, const Rect& rect, float mode)
		{
			Constants constants{};
			constants.rect[0] = rect.left / s.width * 2.0f - 1.0f;
			constants.rect[1] = rect.top / s.height * 2.0f - 1.0f;
			constants.rect[2] = rect.right / s.width * 2.0f - 1.0f;
			constants.rect[3] = rect.bottom / s.height * 2.0f - 1.0f;
			constants.size[0] = kScreenWidth;
			constants.size[1] = kScreenHeight;
			constants.mode = mode;
			vk.CmdPushConstants(commands, s.layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(constants), &constants);
			vk.CmdDraw(commands, 6, 1, 0, 0);
		}

		// The new pixels into the screens' images, from this slot's staging buffer
		void Upload(Slot& slot, VkCommandBuffer commands, const uint32_t* const pixels[2])
		{
			for (int i = 0; i < 2; i++)
			{
				if (!pixels[i])
					continue;
				Image& image = s.screens[i];
				std::memcpy(static_cast<uint8_t*>(slot.staging.mapped) + i * kScreenBytes, pixels[i], kScreenBytes);
				VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
				barrier.oldLayout = image.filled ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
				barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
				barrier.srcAccessMask = image.filled ? VK_ACCESS_SHADER_READ_BIT : 0;
				barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
				barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
				barrier.image = image.image;
				barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
				vk.CmdPipelineBarrier(commands, image.filled ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
					VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
				VkBufferImageCopy region{};
				region.bufferOffset = i * kScreenBytes;
				region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
				region.imageExtent = {(uint32_t)kScreenWidth, (uint32_t)kScreenHeight, 1};
				vk.CmdCopyBufferToImage(commands, slot.staging.buffer, image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
				barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
				barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
				barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
				barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
				vk.CmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr,
					1, &barrier);
				image.filled = true;
			}
			if (pixels[0] || pixels[1])
			{
				VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
				range.memory = slot.staging.memory;
				range.size = VK_WHOLE_SIZE;
				vk.FlushMappedMemoryRanges(s.device, 1, &range);
			}
		}

		// The in-game menu's frame (app/ingame3ds.h): before the render pass for its uploads, in it to draw
		void Overlay(VkCommandBuffer commands, bool inside)
		{
			ps5ingame3ds::Record({s.instance, s.physical, s.device, s.queueFamily, s.queue, s.renderPass, (uint32_t)s.images.size(), commands,
				s.width, s.height, inside, 1});
		}
	}

	void Place(int layout, bool swapped, float width, float height, Rect& top, Rect& bottom)
	{
		top = bottom = {};
		Rect* first = swapped ? &bottom : &top;	 // the main screen: the top one, or the bottom one swapped
		Rect* second = swapped ? &top : &bottom;
		constexpr float w = kScreenWidth, h = kScreenHeight;
		// the screens' box fitted to the picture and centred, as Azahar's MaxRectangle fits its own
		auto fit = [&](float boxWidth, float boxHeight, float& scale, float& x, float& y) {
			scale = std::min(width / boxWidth, height / boxHeight);
			x = std::floor((width - boxWidth * scale) / 2);
			y = std::floor((height - boxHeight * scale) / 2);
		};
		auto rect = [](float x, float y, float rectWidth, float rectHeight) {
			return Rect{x, y, std::round(x + rectWidth), std::round(y + rectHeight)};
		};
		float scale, x, y;
		switch (layout)
		{
		case 1: // the main screen alone
			fit(w, h, scale, x, y);
			*first = rect(x, y, w * scale, h * scale);
			break;
		case 2:
		{
			// the main screen large, the other a quarter of its size at its bottom right (Azahar's
			// large_screen_proportion 4, small_screen_position BottomRight)
			constexpr float kProportion = 4.0f;
			fit(w + w / kProportion, h, scale, x, y);
			*first = rect(x, y, w * scale, h * scale);
			const float smallWidth = w / kProportion * scale, smallHeight = h / kProportion * scale;
			*second = rect(first->right, first->bottom - std::round(smallHeight), smallWidth, smallHeight);
			break;
		}
		case 3: // side by side, the main one on the left
			fit(w * 2, h, scale, x, y);
			*first = rect(x, y, w * scale, h * scale);
			*second = rect(first->right, y, w * scale, h * scale);
			break;
		default: // stacked, the main one above
			fit(w, h * 2, scale, x, y);
			*first = rect(x, y, w * scale, h * scale);
			*second = rect(x, first->bottom, w * scale, h * scale);
			break;
		}
	}

	bool Start(std::string& error)
	{
		if (s.device)
			return true;
		vk.GetInstanceProcAddr = ps5vk::GetInstanceProcAddr();
		vk.CreateInstance = vk.GetInstanceProcAddr ? (PFN_vkCreateInstance)vk.GetInstanceProcAddr(VK_NULL_HANDLE, "vkCreateInstance") : nullptr;
		if (!vk.CreateInstance)
			return Fail(error, "the driver has no vkCreateInstance");
		VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
		app.pApplicationName = "PS5CEMU-HAR";
		app.pEngineName = "PS5CEMU-HAR DS";
		app.apiVersion = VK_API_VERSION_1_1;
		VkInstanceCreateInfo instanceInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
		instanceInfo.pApplicationInfo = &app;
		instanceInfo.enabledExtensionCount = (uint32_t)std::size(ps5vk::kSurfaceExtensions);
		instanceInfo.ppEnabledExtensionNames = ps5vk::kSurfaceExtensions;
		VkResult result = vk.CreateInstance(&instanceInfo, nullptr, &s.instance);
		if (result != VK_SUCCESS)
			return Fail(error, "vkCreateInstance", result);
		if (!LoadInstance(s.instance))
			return Fail(error, std::string("the driver has no ") + s_missing);

		uint32_t count = 0;
		vk.EnumeratePhysicalDevices(s.instance, &count, nullptr);
		if (count == 0)
			return Fail(error, "the driver reports no GPU");
		std::vector<VkPhysicalDevice> physicals(count);
		vk.EnumeratePhysicalDevices(s.instance, &count, physicals.data());
		s.physical = physicals[0];
		VkPhysicalDeviceProperties properties;
		vk.GetPhysicalDeviceProperties(s.physical, &properties);
		vk.GetPhysicalDeviceMemoryProperties(s.physical, &s.memory);
		count = 0;
		vk.GetPhysicalDeviceQueueFamilyProperties(s.physical, &count, nullptr);
		std::vector<VkQueueFamilyProperties> families(count);
		vk.GetPhysicalDeviceQueueFamilyProperties(s.physical, &count, families.data());
		s.queueFamily = UINT32_MAX;
		for (uint32_t i = 0; i < count && s.queueFamily == UINT32_MAX; i++)
			if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)
				s.queueFamily = i;
		if (s.queueFamily == UINT32_MAX)
			return Fail(error, "the GPU has no graphics queue");

		s.surface = ps5vk::CreateDisplaySurface(s.instance, error);
		if (!s.surface)
			return Fail(error, error.empty() ? "no display surface" : error);
		ps5log::Line("[ds] screens: VideoOut surface");

		const float priority = 1.0f;
		VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
		queueInfo.queueFamilyIndex = s.queueFamily;
		queueInfo.queueCount = 1;
		queueInfo.pQueuePriorities = &priority;
		const char* swapchain = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
		VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
		deviceInfo.queueCreateInfoCount = 1;
		deviceInfo.pQueueCreateInfos = &queueInfo;
		deviceInfo.enabledExtensionCount = 1;
		deviceInfo.ppEnabledExtensionNames = &swapchain;
		result = vk.CreateDevice(s.physical, &deviceInfo, nullptr, &s.device);
		if (result != VK_SUCCESS)
			return Fail(error, "vkCreateDevice", result);
		if (!LoadDevice(s.device))
			return Fail(error, std::string("the device has no ") + s_missing);
		vk.GetDeviceQueue(s.device, s.queueFamily, 0, &s.queue);
		ps5log::Line("[ds] screens: device on {}", properties.deviceName);

		VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
		poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
		poolInfo.queueFamilyIndex = s.queueFamily;
		result = vk.CreateCommandPool(s.device, &poolInfo, nullptr, &s.pool);
		if (result != VK_SUCCESS)
			return Fail(error, "vkCreateCommandPool", result);
		for (Slot& slot : s.slots)
		{
			VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
			allocate.commandPool = s.pool;
			allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
			allocate.commandBufferCount = 1;
			VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
			fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;
			VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
			if (vk.AllocateCommandBuffers(s.device, &allocate, &slot.commands) != VK_SUCCESS ||
				vk.CreateFence(s.device, &fence, nullptr, &slot.done) != VK_SUCCESS ||
				vk.CreateSemaphore(s.device, &semaphore, nullptr, &slot.acquired) != VK_SUCCESS || !MakeBuffer(slot.staging, kScreenBytes * 2))
				return Fail(error, "the frames' command buffers");
		}
		if (!MakeSwapchain(error) || !MakePipelines(error))
			return false;
		for (Image& screen : s.screens)
			if (!MakeScreen(screen))
				return Fail(error, "the screens' images");
		ps5log::Line("[ds] screens: ready, {}x{}", s.width, s.height);
		return true;
	}

	bool Present(const Frame& frame)
	{
		if (!s.device)
			return false;
		const bool first = s.frames == 0;
		Slot& slot = s.slots[s.slot];
		VkResult result = vk.WaitForFences(s.device, 1, &slot.done, VK_TRUE, UINT64_MAX);
		if (result != VK_SUCCESS)
		{
			ps5log::Line("[ds] screens: waiting for a frame: {}", Describe(result));
			return false;
		}
		uint32_t image = 0;
		result = vk.AcquireNextImageKHR(s.device, s.swapchain, UINT64_MAX, slot.acquired, VK_NULL_HANDLE, &image);
		if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
		{
			ps5log::Line("[ds] screens: vkAcquireNextImageKHR: {}", Describe(result));
			return false;
		}
		vk.ResetFences(s.device, 1, &slot.done);

		VkCommandBuffer commands = slot.commands;
		vk.ResetCommandBuffer(commands, 0);
		VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
		begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
		vk.BeginCommandBuffer(commands, &begin);
		const uint32_t* const pixels[2] = {frame.top, frame.bottom};
		Upload(slot, commands, pixels);
		Overlay(commands, false);

		VkClearValue clear{};
		clear.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
		VkRenderPassBeginInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
		pass.renderPass = s.renderPass;
		pass.framebuffer = s.framebuffers[image];
		pass.renderArea = {{0, 0}, {s.width, s.height}};
		pass.clearValueCount = 1;
		pass.pClearValues = &clear;
		vk.CmdBeginRenderPass(commands, &pass, VK_SUBPASS_CONTENTS_INLINE);
		VkViewport viewport{0.0f, 0.0f, (float)s.width, (float)s.height, 0.0f, 1.0f};
		VkRect2D scissor{{0, 0}, {s.width, s.height}};
		vk.CmdSetViewport(commands, 0, 1, &viewport);
		vk.CmdSetScissor(commands, 0, 1, &scissor);
		vk.CmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, s.screenPipeline);
		const Rect* rects[2] = {&frame.topRect, &frame.bottomRect};
		const float mode = (float)std::clamp(frame.filter, 0, kFilterCount - 1);
		for (int i = 0; i < 2; i++)
			if (rects[i]->Shown() && s.screens[i].filled)
			{
				vk.CmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, s.layout, 0, 1, &s.screens[i].set, 0, nullptr);
				Draw(commands, *rects[i], mode);
			}
		const Rect& bottom = frame.bottomRect;
		if (frame.cursor && bottom.Shown() && s.screens[1].filled)
		{
			// Azahar's crosshair: two bars, a sixtieth of the screen's height long each way and a fifth of
			// that thick, kept on the bottom screen
			const float x = bottom.left + std::clamp(frame.cursorX, 0.0f, 1.0f) * bottom.Width();
			const float y = bottom.top + std::clamp(frame.cursorY, 0.0f, 1.0f) * bottom.Height();
			const float arm = bottom.Height() / 60.0f, thick = arm / 5.0f;
			auto clip = [&](Rect r) {
				return Rect{std::max(r.left, bottom.left), std::max(r.top, bottom.top), std::min(r.right, bottom.right),
					std::min(r.bottom, bottom.bottom)};
			};
			vk.CmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, s.cursorPipeline);
			Draw(commands, clip({x - thick, y - arm, x + thick, y + arm}), 3.0f);
			Draw(commands, clip({x - arm, y - thick, x - thick, y + thick}), 3.0f);
			Draw(commands, clip({x + thick, y - thick, x + arm, y + thick}), 3.0f);
		}
		Overlay(commands, true);
		vk.CmdEndRenderPass(commands);
		vk.EndCommandBuffer(commands);

		const VkPipelineStageFlags wait = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
		VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
		submit.commandBufferCount = 1;
		submit.pCommandBuffers = &commands;
		submit.waitSemaphoreCount = 1;
		submit.pWaitSemaphores = &slot.acquired;
		submit.pWaitDstStageMask = &wait;
		submit.signalSemaphoreCount = 1;
		submit.pSignalSemaphores = &s.rendered[image];
		result = vk.QueueSubmit(s.queue, 1, &submit, slot.done);
		if (result != VK_SUCCESS)
		{
			ps5log::Line("[ds] screens: vkQueueSubmit: {}", Describe(result));
			return false;
		}
		VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
		present.waitSemaphoreCount = 1;
		present.pWaitSemaphores = &s.rendered[image];
		present.swapchainCount = 1;
		present.pSwapchains = &s.swapchain;
		present.pImageIndices = &image;
		result = vk.QueuePresentKHR(s.queue, &present);
		if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
		{
			ps5log::Line("[ds] screens: vkQueuePresentKHR: {}", Describe(result));
			return false;
		}
		if (first)
			ps5log::Line("[ds] screens: first frame presented");
		s.slot = (s.slot + 1) % kSlots;
		s.frames++;
		return true;
	}

	void Stop()
	{
		if (s.instance == VK_NULL_HANDLE)
			return;
		if (s.device)
		{
			vk.DeviceWaitIdle(s.device);
			for (Slot& slot : s.slots)
			{
				FreeBuffer(slot.staging);
				if (slot.done)
					vk.DestroyFence(s.device, slot.done, nullptr);
				if (slot.acquired)
					vk.DestroySemaphore(s.device, slot.acquired, nullptr);
				slot = {};
			}
			for (Image& screen : s.screens)
				FreeScreen(screen);
			if (s.screenPipeline)
				vk.DestroyPipeline(s.device, s.screenPipeline, nullptr);
			if (s.cursorPipeline)
				vk.DestroyPipeline(s.device, s.cursorPipeline, nullptr);
			if (s.layout)
				vk.DestroyPipelineLayout(s.device, s.layout, nullptr);
			if (s.descriptors)
				vk.DestroyDescriptorPool(s.device, s.descriptors, nullptr);
			if (s.setLayout)
				vk.DestroyDescriptorSetLayout(s.device, s.setLayout, nullptr);
			if (s.sampler)
				vk.DestroySampler(s.device, s.sampler, nullptr);
			for (VkFramebuffer framebuffer : s.framebuffers)
				vk.DestroyFramebuffer(s.device, framebuffer, nullptr);
			for (VkImageView view : s.views)
				vk.DestroyImageView(s.device, view, nullptr);
			for (VkSemaphore semaphore : s.rendered)
				vk.DestroySemaphore(s.device, semaphore, nullptr);
			if (s.renderPass)
				vk.DestroyRenderPass(s.device, s.renderPass, nullptr);
			if (s.swapchain)
				vk.DestroySwapchainKHR(s.device, s.swapchain, nullptr);
			if (s.pool)
				vk.DestroyCommandPool(s.device, s.pool, nullptr);
			vk.DestroyDevice(s.device, nullptr);
		}
		if (s.surface && vk.DestroySurfaceKHR)
			vk.DestroySurfaceKHR(s.instance, s.surface, nullptr);
		if (vk.DestroyInstance)
			vk.DestroyInstance(s.instance, nullptr);
		const bool logged = s.frames > 0;
		s = State();
		if (logged)
			ps5log::Line("[ds] screens: stopped");
	}

	uint32_t Width()
	{
		return s.width;
	}

	uint32_t Height()
	{
		return s.height;
	}
}
