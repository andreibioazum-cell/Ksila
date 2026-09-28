// Ksila — Vulkan 1.0 renderer for the lobby UI.
//
// Renders a DrawList (2D textured/colored triangles) with a single graphics
// pipeline, a single combined image sampler (the font atlas) and 16 bytes of
// push constants. Supports window resizing and optional 4x MSAA.
#pragma once

// The NDK's vulkan.h only declares VK_KHR_android_surface (and the
// vkCreateAndroidSurfaceKHR entry point) when this is defined.
#if defined(__ANDROID__)
#define VK_USE_PLATFORM_ANDROID_KHR 1
#endif

#include <vulkan/vulkan.h>

#include "ui.h"

// Platform window handle: GLFWwindow on desktop, ANativeWindow on Android.
#if defined(__ANDROID__)
struct ANativeWindow;
using SystemWindow = struct ANativeWindow *;
#else
struct GLFWwindow;
using SystemWindow = GLFWwindow *;
#endif

namespace ksila {

class Renderer {
public:
	// p_validation enables the VK_LAYER_KHRONOS_validation layer + debug
	// messenger when available.
	bool init(SystemWindow p_window, const FontAtlas &p_font, bool p_validation);
	void shutdown();

	// Notifies the renderer that the framebuffer size changed (safe to call
	// from a window callback).
	void notify_resize() { framebuffer_resized_ = true; }

	// The application layer feeds the current drawable size each frame
	// (GLFW: glfwGetFramebufferSize; Android: ANativeWindow_getWidth/Height).
	void set_framebuffer_size(int p_w, int p_h) {
		fb_width_ = p_w;
		fb_height_ = p_h;
	}

	// Uploads the draw list geometry and presents one frame.
	// Returns false only on fatal errors (device lost etc.).
	bool draw_frame(const DrawList &p_list);

private:
	bool create_instance(bool p_validation);
	bool create_debug_messenger();
	bool create_surface(GLFWwindow *p_window);
	bool pick_physical_device();
	bool create_logical_device();
	bool create_swapchain();
	bool create_image_views();
	bool create_render_pass();
	bool create_msaa_target();
	bool create_framebuffers();
	bool create_descriptor_layout();
	bool create_atlas_texture(const FontAtlas &p_font);
	bool create_descriptor_pool_and_set();
	bool create_pipeline();
	bool create_command_buffers();
	bool create_sync_objects();
	bool create_geometry_buffers(size_t p_vertex_bytes, size_t p_index_bytes);

	void destroy_swapchain_objects();
	void destroy_geometry_buffers();
	void recreate_swapchain();

	SystemWindow window_ = nullptr;
	int fb_width_ = 0;
	int fb_height_ = 0;

	// Core objects.
	VkInstance instance_ = VK_NULL_HANDLE;
	VkDebugUtilsMessengerEXT debug_messenger_ = VK_NULL_HANDLE;
	VkSurfaceKHR surface_ = VK_NULL_HANDLE;
	VkPhysicalDevice physical_device_ = VK_NULL_HANDLE;
	VkDevice device_ = VK_NULL_HANDLE;
	uint32_t graphics_family_ = 0;
	uint32_t present_family_ = 0;
	VkQueue graphics_queue_ = VK_NULL_HANDLE;
	VkQueue present_queue_ = VK_NULL_HANDLE;

	// Swapchain.
	VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
	VkFormat swapchain_format_ = VK_FORMAT_UNDEFINED;
	VkExtent2D swapchain_extent_{};
	std::vector<VkImage> swapchain_images_;
	std::vector<VkImageView> swapchain_image_views_;
	std::vector<VkFramebuffer> framebuffers_;
	VkRenderPass render_pass_ = VK_NULL_HANDLE;

	// Optional MSAA target (used when the device supports 4x).
	VkSampleCountFlagBits sample_count_ = VK_SAMPLE_COUNT_1_BIT;
	VkImage msaa_image_ = VK_NULL_HANDLE;
	VkDeviceMemory msaa_memory_ = VK_NULL_HANDLE;
	VkImageView msaa_view_ = VK_NULL_HANDLE;

	// Pipeline & resources.
	VkDescriptorSetLayout descriptor_layout_ = VK_NULL_HANDLE;
	VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;
	VkDescriptorSet descriptor_set_ = VK_NULL_HANDLE;
	VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
	VkPipeline pipeline_ = VK_NULL_HANDLE;
	VkImage atlas_image_ = VK_NULL_HANDLE;
	VkDeviceMemory atlas_memory_ = VK_NULL_HANDLE;
	VkImageView atlas_view_ = VK_NULL_HANDLE;
	VkSampler atlas_sampler_ = VK_NULL_HANDLE;
	VkBuffer atlas_staging_ = VK_NULL_HANDLE;
	VkDeviceMemory atlas_staging_memory_ = VK_NULL_HANDLE;

	// Geometry buffers (grown on demand, persistent mapping).
	VkBuffer vertex_buffer_ = VK_NULL_HANDLE;
	VkDeviceMemory vertex_memory_ = VK_NULL_HANDLE;
	void *vertex_mapped_ = nullptr;
	size_t vertex_capacity_ = 0;
	VkBuffer index_buffer_ = VK_NULL_HANDLE;
	VkDeviceMemory index_memory_ = VK_NULL_HANDLE;
	void *index_mapped_ = nullptr;
	size_t index_capacity_ = 0;

	// Commands & sync.
	VkCommandPool command_pool_ = VK_NULL_HANDLE;
	VkCommandBuffer command_buffers_[2] = { VK_NULL_HANDLE, VK_NULL_HANDLE };
	VkSemaphore image_available_[2] = { VK_NULL_HANDLE, VK_NULL_HANDLE };
	VkSemaphore render_finished_[2] = { VK_NULL_HANDLE, VK_NULL_HANDLE };
	VkFence in_flight_[2] = { VK_NULL_HANDLE, VK_NULL_HANDLE };
	uint32_t current_frame_ = 0;
	bool framebuffer_resized_ = false;
	bool validation_enabled_ = false;
};

} // namespace ksila
