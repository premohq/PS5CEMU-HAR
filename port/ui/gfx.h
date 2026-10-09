// SPDX-License-Identifier: GPL-3.0-or-later
// PS5CEMU-HAR's UI kit: drawing on the GPU (docs/UI-REDESIGN.md, 9.1 and 9.4). A Vulkan device of the
// kit's own on the driver the app links, presenting to VideoOut's display surface at 3840 x 2160, or
// rendering into an image read back each frame (the PC preview). Everything is one instanced pipeline:
// each instance is a quad whose fragment shader draws a shape as a signed distance (shaders/ui.frag),
// in the 1920 x 1080 layout space the viewport scales to the output.
//
// It is set up the way Cemu's renderer and its ImGui backend are on the console, where both are known
// to draw: B8G8R8A8_UNORM and FIFO, a render pass that clears, host-visible buffers flushed after
// writing, device-local optimal-tiled images filled by a copy from a staging buffer. Each step of the
// first frame is logged, so a launcher that shows nothing says where it stopped.
//
// Everything is made and destroyed on the thread that calls Start, Frame and Stop.

#pragma once

#include "vkfn.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace ui
{
	// What the kit logs (the boot log on the console, stderr on the PC).
	void SetLog(std::function<void(const std::string&)> log);
	void Log(const std::string& line);

	// Where the kit draws.
	struct Target
	{
		uint32_t width = 3840, height = 2160;
		// Instance extensions the surface needs, and the surface: VideoOut's (ps5vk::CreateDisplaySurface).
		// Without one, the kit draws into an image and hands each frame to readback.
		std::vector<const char*> instanceExtensions;
		std::function<VkSurfaceKHR(VkInstance, std::string& error)> createSurface;
		std::function<void(const uint8_t* bgra, uint32_t width, uint32_t height, size_t pitch)> readback;
		// RADV's pipeline cache, kept between sessions (empty: none)
		std::string pipelineCache;
	};

	// One quad, as the shaders take it (shaders/ui.vert's attributes).
	struct Instance
	{
		float rect[4];	  // the quad drawn, in layout units
		float box[4];	  // the shape's box
		float shape[4];	  // corner radius, ring width, softness, kind
		uint32_t colour0; // RGBA, 8 bits each, R in the lowest byte
		uint32_t colour1;
		float gradient[4];
		float uv[4];
		float clip[4];
		float extra[4]; // clip's corner radius, gradient kind, glyph weight, gradient start
	};
	static_assert(sizeof(Instance) == 120);

	enum Kind
	{
		kFill = 0,
		kRing = 1,
		kShadow = 2,
		kImage = 3,
		kGlyph = 4,
		kGrain = 5,
		kLine = 6,
		kTriangle = 7,
		kWave = 8,
	};

	using TextureId = uint32_t; // 0: none (a white pixel)

	// A frame's instances, in runs that share a texture.
	struct DrawList
	{
		struct Run
		{
			uint32_t first, count;
			TextureId texture;
		};
		std::vector<Instance> instances;
		std::vector<Run> runs;

		void Add(const Instance& instance, TextureId texture);
		void Clear();
	};

	class Gfx
	{
	public:
		~Gfx() { Stop(); }

		// The instance, device, swapchain (or the image read back) and the pipeline. False, with the
		// reason, when something cannot be made; what was made is destroyed again.
		bool Start(PFN_vkGetInstanceProcAddr gipa, const Target& target, std::string& error);
		// Everything Start made, once the GPU has finished with it. The driver keeps VideoOut open
		// (it opens it once a process), so another renderer's swapchain can take it at once.
		void Stop();
		bool Running() const { return m_device != VK_NULL_HANDLE; }

		// Draws list and shows it: returns once the frame is queued, waiting first for the one two
		// frames back (VideoOut's flips pace it). False when the device was lost.
		bool Frame(const DrawList& list, float time);

		// A picture's pixels (RGBA, straight alpha, rows top down): made at once, filled before the next
		// frame draws. Destroyed once the frames using it are done.
		TextureId CreateTexture(uint32_t width, uint32_t height, const uint8_t* rgba);
		void DestroyTexture(TextureId texture);
		// The glyph atlas (one byte a texel: text.h), again whenever it grows, and the rows of it that
		// changed.
		void SetAtlas(uint32_t width, uint32_t height, const uint8_t* pixels);
		void AtlasChanged(uint32_t firstRow, uint32_t rows);

		uint32_t Width() const { return m_width; }
		uint32_t Height() const { return m_height; }
		uint64_t Frames() const { return m_frames; }
		// GPU memory the kit holds, in bytes (textures, buffers, the target), for the log
		uint64_t MemoryBytes() const { return m_memoryBytes; }

	private:
		struct Buffer
		{
			VkBuffer buffer = VK_NULL_HANDLE;
			VkDeviceMemory memory = VK_NULL_HANDLE;
			void* mapped = nullptr;
			VkDeviceSize size = 0;	// what it holds
			VkDeviceSize bytes = 0; // the memory it took
		};
		struct Image
		{
			VkImage image = VK_NULL_HANDLE;
			VkDeviceMemory memory = VK_NULL_HANDLE;
			VkImageView view = VK_NULL_HANDLE;
			VkDeviceSize bytes = 0;
		};
		struct Texture
		{
			Image image;
			uint32_t width = 0, height = 0;
			VkDescriptorSet set = VK_NULL_HANDLE;
			std::vector<uint8_t> pending; // pixels still to upload
			bool live = false;
		};
		struct FrameSlot
		{
			VkCommandBuffer commands = VK_NULL_HANDLE;
			VkFence done = VK_NULL_HANDLE;
			VkSemaphore acquired = VK_NULL_HANDLE;
			Buffer instances;
			Buffer staging;
			std::vector<std::function<void()>> garbage; // destroyed once this slot's fence passes
		};

		bool Fail(std::string& error, const std::string& what, VkResult result = VK_SUCCESS);
		uint32_t MemoryType(uint32_t bits, VkMemoryPropertyFlags properties) const;
		bool MakeBuffer(Buffer& buffer, VkDeviceSize size, VkBufferUsageFlags usage);
		void FreeBuffer(Buffer& buffer);
		bool MakeImage(Image& image, uint32_t width, uint32_t height, VkFormat format, VkImageUsageFlags usage);
		void FreeImage(Image& image);
		bool MakeTarget(std::string& error);
		bool MakePipeline(std::string& error);
		VkDescriptorSet MakeSet(VkImageView image);
		void RecordUploads(FrameSlot& slot, VkCommandBuffer commands);
		void SavePipelineCache();

		Target m_target;
		uint32_t m_width = 0, m_height = 0;
		VkInstance m_instance = VK_NULL_HANDLE;
		VkPhysicalDevice m_physical = VK_NULL_HANDLE;
		VkPhysicalDeviceMemoryProperties m_memory{};
		VkDevice m_device = VK_NULL_HANDLE;
		uint32_t m_queueFamily = 0;
		VkQueue m_queue = VK_NULL_HANDLE;
		VkSurfaceKHR m_surface = VK_NULL_HANDLE;
		VkSwapchainKHR m_swapchain = VK_NULL_HANDLE;
		VkFormat m_format = VK_FORMAT_B8G8R8A8_UNORM;
		std::vector<VkImage> m_images;
		std::vector<VkImageView> m_views;
		std::vector<VkFramebuffer> m_framebuffers;
		std::vector<VkSemaphore> m_rendered; // one per swapchain image
		Image m_offscreen;
		Buffer m_readback;
		VkRenderPass m_renderPass = VK_NULL_HANDLE;
		VkCommandPool m_pool = VK_NULL_HANDLE;
		VkPipelineCache m_cache = VK_NULL_HANDLE;
		VkDescriptorSetLayout m_setLayout = VK_NULL_HANDLE;
		VkPipelineLayout m_layout = VK_NULL_HANDLE;
		VkPipeline m_pipeline = VK_NULL_HANDLE;
		VkDescriptorPool m_descriptors = VK_NULL_HANDLE;
		VkSampler m_sampler = VK_NULL_HANDLE;
		static constexpr int kSlots = 2;
		FrameSlot m_slots[kSlots];
		int m_slot = 0;

		std::vector<Texture> m_textures; // [0]: the white pixel
		std::vector<TextureId> m_freeTextures;
		Image m_atlas;
		uint32_t m_atlasWidth = 0, m_atlasHeight = 0;
		const uint8_t* m_atlasPixels = nullptr;
		uint32_t m_atlasDirtyFirst = 0, m_atlasDirtyEnd = 0;
		bool m_atlasFresh = false; // its layout still UNDEFINED

		uint64_t m_frames = 0;
		uint64_t m_memoryBytes = 0;
	};
}
