#include "renderer.h"

#include <cstdio>
#include <cstring>
#include <vector>

#if defined(__ANDROID__)
#include <android/log.h>
#include <android/native_window.h>
#else
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>
#endif

// SPIR-V shaders, embedded at build time by CMake (tools/embed_binary.py).
#include "shader_lobby_vert.h"
#include "shader_lobby_frag.h"

#ifdef KSILA_VERBOSE
#define KSILA_LOG(...) do { std::fprintf(stderr, "[ksila] " __VA_ARGS__); std::fprintf(stderr, "\n"); } while (0)
#else
#define KSILA_LOG(...) ((void)0)
#endif

#if defined(__ANDROID__)
#define KSILA_ERROR(...) do { __android_log_print(ANDROID_LOG_ERROR, "Ksila", __VA_ARGS__); } while (0)
#else
#define KSILA_ERROR(...) do { std::fprintf(stderr, "Ksila: " __VA_ARGS__); std::fprintf(stderr, "\n"); } while (0)
#endif

namespace ksila {

static constexpr uint32_t MAX_FRAMES_IN_FLIGHT = 2;

// ------------------------------------------------------------------ helpers --

static VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(
		VkDebugUtilsMessageSeverityFlagBitsEXT p_severity,
		VkDebugUtilsMessageTypeFlagsEXT p_type,
		const VkDebugUtilsMessengerCallbackDataEXT *p_data, void *) {
	if (p_severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
		std::fprintf(stderr, "[vk validation] error: %s\n", p_data->pMessage);
	} else if (p_severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
		std::fprintf(stderr, "[vk validation] warning: %s\n", p_data->pMessage);
	} else {
		KSILA_LOG("validation: %s", p_data->pMessage);
	}
	return VK_FALSE;
}

static uint32_t find_memory_type(VkPhysicalDevice p_device, uint32_t p_type_bits, VkMemoryPropertyFlags p_props) {
	VkPhysicalDeviceMemoryProperties mem;
	vkGetPhysicalDeviceMemoryProperties(p_device, &mem);
	for (uint32_t i = 0; i < mem.memoryTypeCount; i++) {
		if ((p_type_bits & (1u << i)) && (mem.memoryTypes[i].propertyFlags & p_props) == p_props) {
			return i;
		}
	}
	return 0xFFFFFFFF;
}

// Creates a buffer; optionally keeps it persistently mapped.
static bool create_buffer(VkDevice p_device, VkPhysicalDevice p_physical,
		VkDeviceSize p_size, VkBufferUsageFlags p_usage, VkMemoryPropertyFlags p_props,
		VkBuffer &r_buffer, VkDeviceMemory &r_memory, void **r_mapped = nullptr) {
	VkBufferCreateInfo buffer_info{};
	buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	buffer_info.size = p_size;
	buffer_info.usage = p_usage;
	buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	if (vkCreateBuffer(p_device, &buffer_info, nullptr, &r_buffer) != VK_SUCCESS) {
		return false;
	}

	VkMemoryRequirements req{};
	vkGetBufferMemoryRequirements(p_device, r_buffer, &req);
	VkMemoryAllocateInfo alloc{};
	alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	alloc.allocationSize = req.size;
	alloc.memoryTypeIndex = find_memory_type(p_physical, req.memoryTypeBits, p_props);
	if (alloc.memoryTypeIndex == 0xFFFFFFFF ||
			vkAllocateMemory(p_device, &alloc, nullptr, &r_memory) != VK_SUCCESS) {
		vkDestroyBuffer(p_device, r_buffer, nullptr);
		r_buffer = VK_NULL_HANDLE;
		return false;
	}
	if (vkBindBufferMemory(p_device, r_buffer, r_memory, 0) != VK_SUCCESS) {
		vkDestroyBuffer(p_device, r_buffer, nullptr);
		vkFreeMemory(p_device, r_memory, nullptr);
		r_buffer = VK_NULL_HANDLE;
		r_memory = VK_NULL_HANDLE;
		return false;
	}
	if (r_mapped) {
		if (vkMapMemory(p_device, r_memory, 0, p_size, 0, r_mapped) != VK_SUCCESS) {
			return false;
		}
	}
	return true;
}

static VkShaderModule create_shader_module(VkDevice p_device, const uint32_t *p_code, size_t p_word_count) {
	VkShaderModuleCreateInfo info{};
	info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	info.codeSize = p_word_count * sizeof(uint32_t);
	info.pCode = p_code;
	VkShaderModule module = VK_NULL_HANDLE;
	if (vkCreateShaderModule(p_device, &info, nullptr, &module) != VK_SUCCESS) {
		return VK_NULL_HANDLE;
	}
	return module;
}

static void transition_image_layout(VkCommandBuffer p_cmd, VkImage p_image, VkImageLayout p_old, VkImageLayout p_new) {
	VkImageMemoryBarrier barrier{};
	barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	barrier.oldLayout = p_old;
	barrier.newLayout = p_new;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image = p_image;
	barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	barrier.subresourceRange.baseMipLevel = 0;
	barrier.subresourceRange.levelCount = 1;
	barrier.subresourceRange.baseArrayLayer = 0;
	barrier.subresourceRange.layerCount = 1;

	VkPipelineStageFlags src_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
	VkPipelineStageFlags dst_stage = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;

	if (p_new == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
		barrier.srcAccessMask = 0;
		barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		src_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
		dst_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
	} else if (p_old == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL && p_new == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
		barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
		src_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
		dst_stage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
	}

	vkCmdPipelineBarrier(p_cmd, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}

// --------------------------------------------------------------------- init --

bool Renderer::init(SystemWindow p_window, const FontAtlas &p_font, bool p_validation) {
	window_ = p_window;

	if (!create_instance(p_validation)) {
		KSILA_ERROR("failed to create Vulkan instance.");
		return false;
	}
	if (validation_enabled_ && !create_debug_messenger()) {
		KSILA_ERROR("failed to create debug messenger.");
		return false;
	}
	if (!create_surface(p_window)) {
		KSILA_ERROR("failed to create window surface.");
		return false;
	}
	if (!pick_physical_device()) {
		KSILA_ERROR("no suitable Vulkan device found (need graphics + presentation).");
		return false;
	}
	if (!create_logical_device()) {
		KSILA_ERROR("failed to create Vulkan device.");
		return false;
	}
	if (!create_swapchain() || !create_image_views() || !create_render_pass() || !create_msaa_target() ||
			!create_framebuffers()) {
		KSILA_ERROR("failed to set up the swapchain.");
		return false;
	}
	if (!create_descriptor_layout() || !create_command_buffers() ||
			!create_atlas_texture(p_font) || !create_descriptor_pool_and_set() ||
			!create_pipeline()) {
		KSILA_ERROR("failed to set up the pipeline.");
		return false;
	}
	if (!create_sync_objects()) {
		KSILA_ERROR("failed to set up sync objects.");
		return false;
	}
	return true;
}

bool Renderer::create_instance(bool p_validation) {
	VkApplicationInfo app{};
	app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	app.pApplicationName = "Ksila Lobby";
	app.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
	app.pEngineName = "Ksila";
	app.engineVersion = VK_MAKE_VERSION(0, 1, 0);
	app.apiVersion = VK_API_VERSION_1_0;

	uint32_t glfw_extension_count = 0;
	const char **glfw_extensions = nullptr;
#if !defined(__ANDROID__)
	glfw_extensions = glfwGetRequiredInstanceExtensions(&glfw_extension_count);
#endif

	std::vector<const char *> extensions;
#if defined(__ANDROID__)
	// No GLFW on Android: the surface is created via VK_KHR_android_surface.
	extensions.push_back(VK_KHR_SURFACE_EXTENSION_NAME);
	extensions.push_back(VK_KHR_ANDROID_SURFACE_EXTENSION_NAME);
#else
	for (uint32_t i = 0; i < glfw_extension_count; i++) {
		extensions.push_back(glfw_extensions[i]);
	}
#endif

	const char *validation_layer = "VK_LAYER_KHRONOS_validation";
	bool want_validation = p_validation;
	if (want_validation) {
		uint32_t layer_count = 0;
		vkEnumerateInstanceLayerProperties(&layer_count, nullptr);
		std::vector<VkLayerProperties> layers(layer_count);
		vkEnumerateInstanceLayerProperties(&layer_count, layers.data());
		bool found = false;
		for (const VkLayerProperties &layer : layers) {
			if (std::strcmp(layer.layerName, validation_layer) == 0) {
				found = true;
				break;
			}
		}
		if (!found) {
			KSILA_ERROR("validation layer requested but not installed; continuing without it.");
			want_validation = false;
		}
	}
	validation_enabled_ = want_validation;

	VkDebugUtilsMessengerCreateInfoEXT debug_info{};
	debug_info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
	debug_info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
	debug_info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
	debug_info.pfnUserCallback = debug_callback;

	VkInstanceCreateInfo info{};
	info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	info.pApplicationInfo = &app;
	if (validation_enabled_) {
		extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
		info.enabledLayerCount = 1;
		info.ppEnabledLayerNames = &validation_layer;
		info.pNext = &debug_info; // also cover instance creation/destruction
	}
	info.enabledExtensionCount = uint32_t(extensions.size());
	info.ppEnabledExtensionNames = extensions.data();

	VkResult result = vkCreateInstance(&info, nullptr, &instance_);
	if (result != VK_SUCCESS) {
		KSILA_ERROR("vkCreateInstance failed (%d).", int(result));
		return false;
	}
	return true;
}

bool Renderer::create_debug_messenger() {
	PFN_vkCreateDebugUtilsMessengerEXT create =
			(PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance_, "vkCreateDebugUtilsMessengerEXT");
	if (!create) {
		return false;
	}
	VkDebugUtilsMessengerCreateInfoEXT info{};
	info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
	info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
	info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
	info.pfnUserCallback = debug_callback;
	return create(instance_, &info, nullptr, &debug_messenger_) == VK_SUCCESS;
}

bool Renderer::create_surface(SystemWindow p_window) {
#if defined(__ANDROID__)
	VkAndroidSurfaceCreateInfoKHR info{};
	info.sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR;
	info.window = p_window; // struct ANativeWindow *
	return vkCreateAndroidSurfaceKHR(instance_, &info, nullptr, &surface_) == VK_SUCCESS;
#else
	return glfwCreateWindowSurface(instance_, p_window, nullptr, &surface_) == VK_SUCCESS;
#endif
}

bool Renderer::pick_physical_device() {
	uint32_t device_count = 0;
	vkEnumeratePhysicalDevices(instance_, &device_count, nullptr);
	if (device_count == 0) {
		return false;
	}
	std::vector<VkPhysicalDevice> devices(device_count);
	vkEnumeratePhysicalDevices(instance_, &device_count, devices.data());

	int best_score = -1;
	for (VkPhysicalDevice device : devices) {
		VkPhysicalDeviceProperties props{};
		vkGetPhysicalDeviceProperties(device, &props);
		if (props.apiVersion < VK_API_VERSION_1_0) {
			continue;
		}

		uint32_t queue_count = 0;
		vkGetPhysicalDeviceQueueFamilyProperties(device, &queue_count, nullptr);
		std::vector<VkQueueFamilyProperties> queues(queue_count);
		vkGetPhysicalDeviceQueueFamilyProperties(device, &queue_count, queues.data());

		bool has_graphics = false, has_present = false;
		for (uint32_t i = 0; i < queue_count; i++) {
			if (queues[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
				has_graphics = true;
			}
			VkBool32 present = VK_FALSE;
			vkGetPhysicalDeviceSurfaceSupportKHR(device, i, surface_, &present);
			if (present) {
				has_present = true;
			}
		}

		uint32_t extension_count = 0;
		vkEnumerateDeviceExtensionProperties(device, nullptr, &extension_count, nullptr);
		std::vector<VkExtensionProperties> extensions(extension_count);
		vkEnumerateDeviceExtensionProperties(device, nullptr, &extension_count, extensions.data());
		bool has_swapchain = false;
		for (const VkExtensionProperties &ext : extensions) {
			if (std::strcmp(ext.extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0) {
				has_swapchain = true;
				break;
			}
		}

		VkSurfaceCapabilitiesKHR caps{};
		vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device, surface_, &caps);

		if (!has_graphics || !has_present || !has_swapchain || caps.minImageCount == 0) {
			continue;
		}

		int score = 100;
		if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
			score = 1000;
		} else if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) {
			score = 500;
		} else if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU) {
			score = 300; // lavapipe / SwiftShader
		}
		if (score > best_score) {
			best_score = score;
			physical_device_ = device;
		}
	}
	return physical_device_ != VK_NULL_HANDLE;
}

bool Renderer::create_logical_device() {
	uint32_t queue_count = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(physical_device_, &queue_count, nullptr);
	std::vector<VkQueueFamilyProperties> queues(queue_count);
	vkGetPhysicalDeviceQueueFamilyProperties(physical_device_, &queue_count, queues.data());

	graphics_family_ = 0xFFFFFFFF;
	present_family_ = 0xFFFFFFFF;
	for (uint32_t i = 0; i < queue_count; i++) {
		if ((queues[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && graphics_family_ == 0xFFFFFFFF) {
			graphics_family_ = i;
		}
		VkBool32 present = VK_FALSE;
		vkGetPhysicalDeviceSurfaceSupportKHR(physical_device_, i, surface_, &present);
		if (present && present_family_ == 0xFFFFFFFF) {
			present_family_ = i;
		}
	}
	if (graphics_family_ == 0xFFFFFFFF || present_family_ == 0xFFFFFFFF) {
		return false;
	}

	float priority = 1.f;
	VkDeviceQueueCreateInfo queue_infos[2]{};
	uint32_t queue_info_count = 1;
	queue_infos[0].sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
	queue_infos[0].queueFamilyIndex = graphics_family_;
	queue_infos[0].queueCount = 1;
	queue_infos[0].pQueuePriorities = &priority;
	if (present_family_ != graphics_family_) {
		queue_infos[1].sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
		queue_infos[1].queueFamilyIndex = present_family_;
		queue_infos[1].queueCount = 1;
		queue_infos[1].pQueuePriorities = &priority;
		queue_info_count = 2;
	}

	const char *device_extensions[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };

	VkPhysicalDeviceFeatures features{};

	VkDeviceCreateInfo info{};
	info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
	info.queueCreateInfoCount = queue_info_count;
	info.pQueueCreateInfos = queue_infos;
	info.enabledExtensionCount = 1;
	info.ppEnabledExtensionNames = device_extensions;
	info.pEnabledFeatures = &features;

	if (vkCreateDevice(physical_device_, &info, nullptr, &device_) != VK_SUCCESS) {
		return false;
	}
	vkGetDeviceQueue(device_, graphics_family_, 0, &graphics_queue_);
	vkGetDeviceQueue(device_, present_family_, 0, &present_queue_);
	return true;
}

bool Renderer::create_swapchain() {
	VkSurfaceCapabilitiesKHR caps{};
	vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_device_, surface_, &caps);

	uint32_t format_count = 0;
	vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device_, surface_, &format_count, nullptr);
	std::vector<VkSurfaceFormatKHR> formats(format_count);
	vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device_, surface_, &format_count, formats.data());

	VkSurfaceFormatKHR chosen = formats[0];
	for (const VkSurfaceFormatKHR &format : formats) {
		if ((format.format == VK_FORMAT_B8G8R8A8_UNORM || format.format == VK_FORMAT_B8G8R8A8_SRGB) &&
				format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
			chosen = format;
			break;
		}
	}
	swapchain_format_ = chosen.format;

	// FIFO is guaranteed by the spec and gives us vsync.
	VkPresentModeKHR present_mode = VK_PRESENT_MODE_FIFO_KHR;

	VkExtent2D extent = caps.currentExtent;
	if (extent.width == 0xFFFFFFFF || extent.height == 0xFFFFFFFF) {
#if defined(__ANDROID__)
		// ANativeWindow always reports an exact size.
		extent.width = uint32_t(ANativeWindow_getWidth(window_));
		extent.height = uint32_t(ANativeWindow_getHeight(window_));
#else
		int w = 0, h = 0;
		glfwGetFramebufferSize(window_, &w, &h);
		extent.width = uint32_t(w);
		extent.height = uint32_t(h);
#endif
	}
	extent.width = extent.width < caps.minImageExtent.width ? caps.minImageExtent.width : extent.width;
	extent.height = extent.height < caps.minImageExtent.height ? caps.minImageExtent.height : extent.height;
	extent.width = extent.width > caps.maxImageExtent.width ? caps.maxImageExtent.width : extent.width;
	extent.height = extent.height > caps.maxImageExtent.height ? caps.maxImageExtent.height : extent.height;
	if (extent.width == 0 || extent.height == 0) {
		return false; // minimized
	}
	swapchain_extent_ = extent;

	uint32_t image_count = caps.minImageCount + 1;
	if (caps.maxImageCount > 0 && image_count > caps.maxImageCount) {
		image_count = caps.maxImageCount;
	}

	VkSwapchainCreateInfoKHR info{};
	info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
	info.surface = surface_;
	info.minImageCount = image_count;
	info.imageFormat = swapchain_format_;
	info.imageColorSpace = chosen.colorSpace;
	info.imageExtent = swapchain_extent_;
	info.imageArrayLayers = 1;
	info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
	info.preTransform = caps.currentTransform;
	info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
	info.presentMode = present_mode;
	info.clipped = VK_TRUE;
	info.oldSwapchain = VK_NULL_HANDLE;

	uint32_t families[2] = { graphics_family_, present_family_ };
	if (graphics_family_ != present_family_) {
		info.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
		info.queueFamilyIndexCount = 2;
		info.pQueueFamilyIndices = families;
	}

	if (vkCreateSwapchainKHR(device_, &info, nullptr, &swapchain_) != VK_SUCCESS) {
		return false;
	}

	uint32_t count = 0;
	vkGetSwapchainImagesKHR(device_, swapchain_, &count, nullptr);
	swapchain_images_.resize(count);
	vkGetSwapchainImagesKHR(device_, swapchain_, &count, swapchain_images_.data());
	return true;
}

bool Renderer::create_image_views() {
	swapchain_image_views_.resize(swapchain_images_.size());
	for (size_t i = 0; i < swapchain_images_.size(); i++) {
		VkImageViewCreateInfo info{};
		info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
		info.image = swapchain_images_[i];
		info.viewType = VK_IMAGE_VIEW_TYPE_2D;
		info.format = swapchain_format_;
		info.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
		info.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
		info.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
		info.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
		info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		info.subresourceRange.levelCount = 1;
		info.subresourceRange.layerCount = 1;
		if (vkCreateImageView(device_, &info, nullptr, &swapchain_image_views_[i]) != VK_SUCCESS) {
			return false;
		}
	}
	return true;
}

bool Renderer::create_render_pass() {
	// Prefer 4x MSAA if the device supports it (smooths rounded corners).
	VkPhysicalDeviceProperties props{};
	vkGetPhysicalDeviceProperties(physical_device_, &props);
	VkSampleCountFlags supported = props.limits.framebufferColorSampleCounts;
	if (supported & VK_SAMPLE_COUNT_4_BIT) {
		sample_count_ = VK_SAMPLE_COUNT_4_BIT;
	} else if (supported & VK_SAMPLE_COUNT_2_BIT) {
		sample_count_ = VK_SAMPLE_COUNT_2_BIT;
	}

	VkAttachmentDescription attachments[2]{};
	uint32_t attachment_count = 1;

	if (sample_count_ > VK_SAMPLE_COUNT_1_BIT) {
		// MSAA color target.
		attachments[0].format = swapchain_format_;
		attachments[0].samples = sample_count_;
		attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
		attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		attachments[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		attachments[0].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		// Resolve target = swapchain image.
		attachments[1].format = swapchain_format_;
		attachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
		attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		attachments[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		attachments[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		attachments[1].finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
		attachment_count = 2;
	} else {
		attachments[0].format = swapchain_format_;
		attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
		attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
		attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		attachments[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		attachments[0].finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
	}

	VkAttachmentReference color_ref{};
	color_ref.attachment = 0;
	color_ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

	VkAttachmentReference resolve_ref{};
	resolve_ref.attachment = 1;
	resolve_ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

	VkSubpassDescription subpass{};
	subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	subpass.colorAttachmentCount = 1;
	subpass.pColorAttachments = &color_ref;
	if (sample_count_ > VK_SAMPLE_COUNT_1_BIT) {
		subpass.pResolveAttachments = &resolve_ref;
	}

	VkSubpassDependency dependency{};
	dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
	dependency.dstSubpass = 0;
	dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	dependency.srcAccessMask = 0;
	dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

	VkRenderPassCreateInfo info{};
	info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
	info.attachmentCount = attachment_count;
	info.pAttachments = attachments;
	info.subpassCount = 1;
	info.pSubpasses = &subpass;
	info.dependencyCount = 1;
	info.pDependencies = &dependency;

	return vkCreateRenderPass(device_, &info, nullptr, &render_pass_) == VK_SUCCESS;
}

bool Renderer::create_msaa_target() {
	if (sample_count_ == VK_SAMPLE_COUNT_1_BIT) {
		return true;
	}
	VkImageCreateInfo image{};
	image.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	image.imageType = VK_IMAGE_TYPE_2D;
	image.format = swapchain_format_;
	image.extent = { swapchain_extent_.width, swapchain_extent_.height, 1 };
	image.mipLevels = 1;
	image.arrayLayers = 1;
	image.samples = sample_count_;
	image.tiling = VK_IMAGE_TILING_OPTIMAL;
	image.usage = VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
	if (vkCreateImage(device_, &image, nullptr, &msaa_image_) != VK_SUCCESS) {
		return false;
	}

	VkMemoryRequirements req{};
	vkGetImageMemoryRequirements(device_, msaa_image_, &req);
	VkMemoryAllocateInfo alloc{};
	alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	alloc.allocationSize = req.size;
	alloc.memoryTypeIndex = find_memory_type(physical_device_, req.memoryTypeBits,
			VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT);
	if (alloc.memoryTypeIndex == 0xFFFFFFFF) {
		alloc.memoryTypeIndex = find_memory_type(physical_device_, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	}
	if (alloc.memoryTypeIndex == 0xFFFFFFFF ||
			vkAllocateMemory(device_, &alloc, nullptr, &msaa_memory_) != VK_SUCCESS ||
			vkBindImageMemory(device_, msaa_image_, msaa_memory_, 0) != VK_SUCCESS) {
		return false;
	}

	VkImageViewCreateInfo view{};
	view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	view.image = msaa_image_;
	view.viewType = VK_IMAGE_VIEW_TYPE_2D;
	view.format = swapchain_format_;
	view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	view.subresourceRange.levelCount = 1;
	view.subresourceRange.layerCount = 1;
	return vkCreateImageView(device_, &view, nullptr, &msaa_view_) == VK_SUCCESS;
}

bool Renderer::create_framebuffers() {
	framebuffers_.resize(swapchain_image_views_.size());
	for (size_t i = 0; i < swapchain_image_views_.size(); i++) {
		VkImageView views[2] = { msaa_view_, swapchain_image_views_[i] };
		VkFramebufferCreateInfo info{};
		info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
		info.renderPass = render_pass_;
		info.attachmentCount = sample_count_ > VK_SAMPLE_COUNT_1_BIT ? 2 : 1;
		info.pAttachments = views;
		info.width = swapchain_extent_.width;
		info.height = swapchain_extent_.height;
		info.layers = 1;
		if (vkCreateFramebuffer(device_, &info, nullptr, &framebuffers_[i]) != VK_SUCCESS) {
			return false;
		}
	}
	return true;
}

bool Renderer::create_descriptor_layout() {
	VkDescriptorSetLayoutBinding binding{};
	binding.binding = 0;
	binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	binding.descriptorCount = 1;
	binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	binding.pImmutableSamplers = nullptr;

	VkDescriptorSetLayoutCreateInfo info{};
	info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	info.bindingCount = 1;
	info.pBindings = &binding;
	return vkCreateDescriptorSetLayout(device_, &info, nullptr, &descriptor_layout_) == VK_SUCCESS;
}

bool Renderer::create_atlas_texture(const FontAtlas &p_font) {
	const int w = p_font.width();
	const int h = p_font.height();
	const size_t data_size = size_t(w) * size_t(h);

	if (!create_buffer(device_, physical_device_, data_size,
				VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
				VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
				atlas_staging_, atlas_staging_memory_)) {
		return false;
	}
	void *mapped = nullptr;
	if (vkMapMemory(device_, atlas_staging_memory_, 0, data_size, 0, &mapped) != VK_SUCCESS) {
		return false;
	}
	std::memcpy(mapped, p_font.pixels(), data_size);
	vkUnmapMemory(device_, atlas_staging_memory_);

	VkImageCreateInfo image{};
	image.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	image.imageType = VK_IMAGE_TYPE_2D;
	image.format = VK_FORMAT_R8_UNORM;
	image.extent = { uint32_t(w), uint32_t(h), 1 };
	image.mipLevels = 1;
	image.arrayLayers = 1;
	image.samples = VK_SAMPLE_COUNT_1_BIT;
	image.tiling = VK_IMAGE_TILING_OPTIMAL;
	image.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
	if (vkCreateImage(device_, &image, nullptr, &atlas_image_) != VK_SUCCESS) {
		return false;
	}

	VkMemoryRequirements req{};
	vkGetImageMemoryRequirements(device_, atlas_image_, &req);
	VkMemoryAllocateInfo alloc{};
	alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	alloc.allocationSize = req.size;
	alloc.memoryTypeIndex = find_memory_type(physical_device_, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	if (alloc.memoryTypeIndex == 0xFFFFFFFF ||
			vkAllocateMemory(device_, &alloc, nullptr, &atlas_memory_) != VK_SUCCESS ||
			vkBindImageMemory(device_, atlas_image_, atlas_memory_, 0) != VK_SUCCESS) {
		return false;
	}

	VkImageViewCreateInfo view{};
	view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	view.image = atlas_image_;
	view.viewType = VK_IMAGE_VIEW_TYPE_2D;
	view.format = VK_FORMAT_R8_UNORM;
	view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	view.subresourceRange.levelCount = 1;
	view.subresourceRange.layerCount = 1;
	if (vkCreateImageView(device_, &view, nullptr, &atlas_view_) != VK_SUCCESS) {
		return false;
	}

	VkSamplerCreateInfo sampler{};
	sampler.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
	sampler.magFilter = VK_FILTER_LINEAR;
	sampler.minFilter = VK_FILTER_LINEAR;
	sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
	sampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	sampler.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	sampler.mipLodBias = 0.f;
	sampler.anisotropyEnable = VK_FALSE;
	sampler.maxAnisotropy = 1.f;
	sampler.compareEnable = VK_FALSE;
	sampler.minLod = 0.f;
	sampler.maxLod = 0.f;
	sampler.borderColor = VK_BORDER_COLOR_INT_OPAQUE_WHITE;
	sampler.unnormalizedCoordinates = VK_FALSE;
	if (vkCreateSampler(device_, &sampler, nullptr, &atlas_sampler_) != VK_SUCCESS) {
		return false;
	}

	// One-time upload: staging -> optimal image (uses the main command pool).
	VkCommandBuffer cmd{};
	VkCommandBufferAllocateInfo cmd_alloc{};
	cmd_alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	cmd_alloc.commandPool = command_pool_;
	cmd_alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	cmd_alloc.commandBufferCount = 1;
	if (vkAllocateCommandBuffers(device_, &cmd_alloc, &cmd) != VK_SUCCESS) {
		return false;
	}

	VkCommandBufferBeginInfo begin{};
	begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkBeginCommandBuffer(cmd, &begin);

	transition_image_layout(cmd, atlas_image_, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
	VkBufferImageCopy region{};
	region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	region.imageSubresource.mipLevel = 0;
	region.imageSubresource.baseArrayLayer = 0;
	region.imageSubresource.layerCount = 1;
	region.imageExtent = { uint32_t(w), uint32_t(h), 1 };
	vkCmdCopyBufferToImage(cmd, atlas_staging_, atlas_image_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
	transition_image_layout(cmd, atlas_image_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

	vkEndCommandBuffer(cmd);

	VkSubmitInfo submit{};
	submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submit.commandBufferCount = 1;
	submit.pCommandBuffers = &cmd;
	vkQueueSubmit(graphics_queue_, 1, &submit, VK_NULL_HANDLE);
	vkQueueWaitIdle(graphics_queue_);

	vkFreeCommandBuffers(device_, command_pool_, 1, &cmd);

	vkDestroyBuffer(device_, atlas_staging_, nullptr);
	vkFreeMemory(device_, atlas_staging_memory_, nullptr);
	atlas_staging_ = VK_NULL_HANDLE;
	atlas_staging_memory_ = VK_NULL_HANDLE;
	return true;
}

bool Renderer::create_descriptor_pool_and_set() {
	VkDescriptorPoolSize pool_size{};
	pool_size.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	pool_size.descriptorCount = 1;

	VkDescriptorPoolCreateInfo pool{};
	pool.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	pool.maxSets = 1;
	pool.poolSizeCount = 1;
	pool.pPoolSizes = &pool_size;
	if (vkCreateDescriptorPool(device_, &pool, nullptr, &descriptor_pool_) != VK_SUCCESS) {
		return false;
	}

	VkDescriptorSetAllocateInfo alloc{};
	alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	alloc.descriptorPool = descriptor_pool_;
	alloc.descriptorSetCount = 1;
	alloc.pSetLayouts = &descriptor_layout_;
	if (vkAllocateDescriptorSets(device_, &alloc, &descriptor_set_) != VK_SUCCESS) {
		return false;
	}

	VkDescriptorImageInfo image_info{};
	image_info.sampler = atlas_sampler_;
	image_info.imageView = atlas_view_;
	image_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

	VkWriteDescriptorSet write{};
	write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	write.dstSet = descriptor_set_;
	write.dstBinding = 0;
	write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	write.descriptorCount = 1;
	write.pImageInfo = &image_info;
	vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
	return true;
}

bool Renderer::create_pipeline() {
	VkShaderModule vert = create_shader_module(device_, LOBBY_VERT_SPV, LOBBY_VERT_SPV_SIZE);
	VkShaderModule frag = create_shader_module(device_, LOBBY_FRAG_SPV, LOBBY_FRAG_SPV_SIZE);
	if (!vert || !frag) {
		KSILA_ERROR("failed to create shader modules.");
		return false;
	}

	VkPipelineShaderStageCreateInfo stages[2]{};
	stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
	stages[0].module = vert;
	stages[0].pName = "main";
	stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	stages[1].module = frag;
	stages[1].pName = "main";

	VkVertexInputBindingDescription binding{};
	binding.binding = 0;
	binding.stride = sizeof(Vertex);
	binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

	VkVertexInputAttributeDescription attributes[3]{};
	attributes[0].location = 0; // vec2 position
	attributes[0].binding = 0;
	attributes[0].format = VK_FORMAT_R32G32_SFLOAT;
	attributes[0].offset = offsetof(Vertex, x);
	attributes[1].location = 1; // vec2 uv
	attributes[1].binding = 0;
	attributes[1].format = VK_FORMAT_R32G32_SFLOAT;
	attributes[1].offset = offsetof(Vertex, u);
	attributes[2].location = 2; // rgba8 color
	attributes[2].binding = 0;
	attributes[2].format = VK_FORMAT_R8G8B8A8_UNORM;
	attributes[2].offset = offsetof(Vertex, color);

	VkPipelineVertexInputStateCreateInfo vertex_input{};
	vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
	vertex_input.vertexBindingDescriptionCount = 1;
	vertex_input.pVertexBindingDescriptions = &binding;
	vertex_input.vertexAttributeDescriptionCount = 3;
	vertex_input.pVertexAttributeDescriptions = attributes;

	VkPipelineInputAssemblyStateCreateInfo input_assembly{};
	input_assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
	input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

	VkPipelineViewportStateCreateInfo viewport_state{};
	viewport_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
	viewport_state.viewportCount = 1;
	viewport_state.scissorCount = 1;

	VkPipelineRasterizationStateCreateInfo raster{};
	raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
	raster.polygonMode = VK_POLYGON_MODE_FILL;
	raster.cullMode = VK_CULL_MODE_NONE;
	raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
	raster.lineWidth = 1.f;

	VkPipelineMultisampleStateCreateInfo multisample{};
	multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
	multisample.rasterizationSamples = sample_count_;
	multisample.minSampleShading = 1.f;

	VkPipelineColorBlendAttachmentState blend_attachment{};
	blend_attachment.blendEnable = VK_TRUE;
	blend_attachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
	blend_attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
	blend_attachment.colorBlendOp = VK_BLEND_OP_ADD;
	blend_attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
	blend_attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
	blend_attachment.alphaBlendOp = VK_BLEND_OP_ADD;
	blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

	VkPipelineColorBlendStateCreateInfo blend{};
	blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
	blend.attachmentCount = 1;
	blend.pAttachments = &blend_attachment;

	VkDynamicState dynamic_states[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
	VkPipelineDynamicStateCreateInfo dynamic{};
	dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
	dynamic.dynamicStateCount = 2;
	dynamic.pDynamicStates = dynamic_states;

	VkPushConstantRange push{};
	push.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
	push.offset = 0;
	push.size = 16;

	VkPipelineLayoutCreateInfo layout{};
	layout.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	layout.setLayoutCount = 1;
	layout.pSetLayouts = &descriptor_layout_;
	layout.pushConstantRangeCount = 1;
	layout.pPushConstantRanges = &push;
	if (vkCreatePipelineLayout(device_, &layout, nullptr, &pipeline_layout_) != VK_SUCCESS) {
		return false;
	}

	VkGraphicsPipelineCreateInfo pipeline{};
	pipeline.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
	pipeline.stageCount = 2;
	pipeline.pStages = stages;
	pipeline.pVertexInputState = &vertex_input;
	pipeline.pInputAssemblyState = &input_assembly;
	pipeline.pViewportState = &viewport_state;
	pipeline.pRasterizationState = &raster;
	pipeline.pMultisampleState = &multisample;
	pipeline.pColorBlendState = &blend;
	pipeline.pDynamicState = &dynamic;
	pipeline.layout = pipeline_layout_;
	pipeline.renderPass = render_pass_;
	pipeline.subpass = 0;

	VkResult result = vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &pipeline, nullptr, &pipeline_);
	vkDestroyShaderModule(device_, vert, nullptr);
	vkDestroyShaderModule(device_, frag, nullptr);
	return result == VK_SUCCESS;
}

bool Renderer::create_command_buffers() {
	VkCommandPoolCreateInfo pool{};
	pool.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	pool.queueFamilyIndex = graphics_family_;
	if (vkCreateCommandPool(device_, &pool, nullptr, &command_pool_) != VK_SUCCESS) {
		return false;
	}

	VkCommandBufferAllocateInfo alloc{};
	alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	alloc.commandPool = command_pool_;
	alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	alloc.commandBufferCount = MAX_FRAMES_IN_FLIGHT;
	return vkAllocateCommandBuffers(device_, &alloc, command_buffers_) == VK_SUCCESS;
}

bool Renderer::create_sync_objects() {
	VkSemaphoreCreateInfo semaphore{};
	semaphore.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
	VkFenceCreateInfo fence{};
	fence.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;
	for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
		if (vkCreateSemaphore(device_, &semaphore, nullptr, &image_available_[i]) != VK_SUCCESS ||
				vkCreateSemaphore(device_, &semaphore, nullptr, &render_finished_[i]) != VK_SUCCESS ||
				vkCreateFence(device_, &fence, nullptr, &in_flight_[i]) != VK_SUCCESS) {
			return false;
		}
	}
	return true;
}

bool Renderer::create_geometry_buffers(size_t p_vertex_bytes, size_t p_index_bytes) {
	if (!create_buffer(device_, physical_device_, p_vertex_bytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
				VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
				vertex_buffer_, vertex_memory_, &vertex_mapped_)) {
		return false;
	}
	if (!create_buffer(device_, physical_device_, p_index_bytes, VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
				VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
				index_buffer_, index_memory_, &index_mapped_)) {
		return false;
	}
	vertex_capacity_ = p_vertex_bytes;
	index_capacity_ = p_index_bytes;
	return true;
}

// ------------------------------------------------------------------- frame --

void Renderer::destroy_geometry_buffers() {
	if (vertex_buffer_) {
		vkDestroyBuffer(device_, vertex_buffer_, nullptr);
	}
	if (vertex_memory_) {
		vkFreeMemory(device_, vertex_memory_, nullptr);
	}
	if (index_buffer_) {
		vkDestroyBuffer(device_, index_buffer_, nullptr);
	}
	if (index_memory_) {
		vkFreeMemory(device_, index_memory_, nullptr);
	}
	vertex_buffer_ = VK_NULL_HANDLE;
	vertex_memory_ = VK_NULL_HANDLE;
	index_buffer_ = VK_NULL_HANDLE;
	index_memory_ = VK_NULL_HANDLE;
	vertex_mapped_ = nullptr;
	index_mapped_ = nullptr;
	vertex_capacity_ = 0;
	index_capacity_ = 0;
}

void Renderer::destroy_swapchain_objects() {
	for (VkFramebuffer fb : framebuffers_) {
		vkDestroyFramebuffer(device_, fb, nullptr);
	}
	framebuffers_.clear();
	if (msaa_view_) {
		vkDestroyImageView(device_, msaa_view_, nullptr);
		msaa_view_ = VK_NULL_HANDLE;
	}
	if (msaa_image_) {
		vkDestroyImage(device_, msaa_image_, nullptr);
		msaa_image_ = VK_NULL_HANDLE;
	}
	if (msaa_memory_) {
		vkFreeMemory(device_, msaa_memory_, nullptr);
		msaa_memory_ = VK_NULL_HANDLE;
	}
	for (VkImageView view : swapchain_image_views_) {
		vkDestroyImageView(device_, view, nullptr);
	}
	swapchain_image_views_.clear();
	if (swapchain_) {
		vkDestroySwapchainKHR(device_, swapchain_, nullptr);
		swapchain_ = VK_NULL_HANDLE;
	}
}

void Renderer::recreate_swapchain() {
	vkDeviceWaitIdle(device_);
	destroy_swapchain_objects();
	if (!create_swapchain()) {
		return; // minimized; will retry on next resize
	}
	if (!create_image_views() || !create_msaa_target() || !create_framebuffers()) {
		KSILA_ERROR("failed to recreate swapchain.");
	}
}

bool Renderer::draw_frame(const DrawList &p_list) {
	// Skip frames while minimized.
	int fbw = fb_width_;
	int fbh = fb_height_;
	if (fbw <= 0 || fbh <= 0) {
		return true;
	}

	// Recreate the swapchain when the framebuffer size changed (before
	// acquiring — the acquired image must belong to the current swapchain).
	if (framebuffer_resized_ ||
			int(swapchain_extent_.width) != fbw || int(swapchain_extent_.height) != fbh) {
		framebuffer_resized_ = false;
		recreate_swapchain();
		if (framebuffers_.empty()) {
			return true; // still resizing/minimized
		}
	}

	VkResult result = vkWaitForFences(device_, 1, &in_flight_[current_frame_], VK_TRUE, UINT64_MAX);
	if (result != VK_SUCCESS) {
		return false;
	}

	uint32_t image_index = 0;
	result = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX, image_available_[current_frame_], VK_NULL_HANDLE, &image_index);
	if (result == VK_ERROR_OUT_OF_DATE_KHR) {
		recreate_swapchain();
		return true;
	}
	if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
		return false;
	}

	// Upload geometry (grow buffers when needed).
	size_t vertex_bytes = p_list.vertices.size() * sizeof(Vertex);
	size_t index_bytes = p_list.indices.size() * sizeof(uint32_t);
	if (vertex_bytes > vertex_capacity_ || index_bytes > index_capacity_) {
		destroy_geometry_buffers();
		size_t new_vcap = vertex_capacity_ ? vertex_capacity_ : 256 * 1024;
		size_t new_icap = index_capacity_ ? index_capacity_ : 64 * 1024;
		while (new_vcap < vertex_bytes) {
			new_vcap *= 2;
		}
		while (new_icap < index_bytes) {
			new_icap *= 2;
		}
		if (!create_geometry_buffers(new_vcap, new_icap)) {
			KSILA_ERROR("out of memory for geometry buffers.");
			return false;
		}
	}
	if (!p_list.vertices.empty()) {
		std::memcpy(vertex_mapped_, p_list.vertices.data(), vertex_bytes);
		std::memcpy(index_mapped_, p_list.indices.data(), index_bytes);
	}

	VkCommandBuffer cmd = command_buffers_[current_frame_];
	vkResetCommandBuffer(cmd, 0);

	VkCommandBufferBeginInfo begin{};
	begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	if (vkBeginCommandBuffer(cmd, &begin) != VK_SUCCESS) {
		return false;
	}

	VkClearValue clear{};
	clear.color = { { 0.078f, 0.086f, 0.106f, 1.0f } };

	VkRenderPassBeginInfo render_pass{};
	render_pass.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
	render_pass.renderPass = render_pass_;
	render_pass.framebuffer = framebuffers_[image_index];
	render_pass.renderArea.extent = swapchain_extent_;
	render_pass.clearValueCount = 1;
	render_pass.pClearValues = &clear;

	vkCmdBeginRenderPass(cmd, &render_pass, VK_SUBPASS_CONTENTS_INLINE);

	VkViewport viewport{};
	viewport.width = float(swapchain_extent_.width);
	viewport.height = float(swapchain_extent_.height);
	viewport.minDepth = 0.f;
	viewport.maxDepth = 1.f;
	vkCmdSetViewport(cmd, 0, 1, &viewport);

	VkRect2D scissor{};
	scissor.extent = swapchain_extent_;
	vkCmdSetScissor(cmd, 0, 1, &scissor);

	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);

	VkDeviceSize offset = 0;
	vkCmdBindVertexBuffers(cmd, 0, 1, &vertex_buffer_, &offset);
	vkCmdBindIndexBuffer(cmd, index_buffer_, 0, VK_INDEX_TYPE_UINT32);

	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_, 0, 1, &descriptor_set_, 0, nullptr);

	struct PushConstants {
		float scale[2];
		float translate[2];
	} push;
	push.scale[0] = 2.f / float(swapchain_extent_.width);
	push.scale[1] = -2.f / float(swapchain_extent_.height);
	push.translate[0] = -1.f;
	push.translate[1] = 1.f;
	vkCmdPushConstants(cmd, pipeline_layout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(PushConstants), &push);

	if (!p_list.indices.empty()) {
		vkCmdDrawIndexed(cmd, uint32_t(p_list.indices.size()), 1, 0, 0, 0);
	}

	vkCmdEndRenderPass(cmd);
	if (vkEndCommandBuffer(cmd) != VK_SUCCESS) {
		return false;
	}

	VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	VkSubmitInfo submit{};
	submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submit.waitSemaphoreCount = 1;
	submit.pWaitSemaphores = &image_available_[current_frame_];
	submit.pWaitDstStageMask = &wait_stage;
	submit.commandBufferCount = 1;
	submit.pCommandBuffers = &cmd;
	submit.signalSemaphoreCount = 1;
	submit.pSignalSemaphores = &render_finished_[current_frame_];

	vkResetFences(device_, 1, &in_flight_[current_frame_]);
	if (vkQueueSubmit(graphics_queue_, 1, &submit, in_flight_[current_frame_]) != VK_SUCCESS) {
		return false;
	}

	VkPresentInfoKHR present{};
	present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
	present.waitSemaphoreCount = 1;
	present.pWaitSemaphores = &render_finished_[current_frame_];
	present.swapchainCount = 1;
	present.pSwapchains = &swapchain_;
	present.pImageIndices = &image_index;

	result = vkQueuePresentKHR(present_queue_, &present);
	if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
		recreate_swapchain();
	} else if (result != VK_SUCCESS) {
		return false;
	}

	current_frame_ = (current_frame_ + 1) % MAX_FRAMES_IN_FLIGHT;
	return true;
}

// ---------------------------------------------------------------- shutdown --

void Renderer::shutdown() {
	if (device_ == VK_NULL_HANDLE) {
		return;
	}
	vkDeviceWaitIdle(device_);

	destroy_geometry_buffers();
	destroy_swapchain_objects();

	if (pipeline_) {
		vkDestroyPipeline(device_, pipeline_, nullptr);
	}
	if (pipeline_layout_) {
		vkDestroyPipelineLayout(device_, pipeline_layout_, nullptr);
	}
	if (descriptor_set_ != VK_NULL_HANDLE) {
		// Freed with the pool.
		descriptor_set_ = VK_NULL_HANDLE;
	}
	if (descriptor_pool_) {
		vkDestroyDescriptorPool(device_, descriptor_pool_, nullptr);
	}
	if (descriptor_layout_) {
		vkDestroyDescriptorSetLayout(device_, descriptor_layout_, nullptr);
	}
	if (atlas_sampler_) {
		vkDestroySampler(device_, atlas_sampler_, nullptr);
	}
	if (atlas_view_) {
		vkDestroyImageView(device_, atlas_view_, nullptr);
	}
	if (atlas_image_) {
		vkDestroyImage(device_, atlas_image_, nullptr);
	}
	if (atlas_memory_) {
		vkFreeMemory(device_, atlas_memory_, nullptr);
	}
	if (render_pass_) {
		vkDestroyRenderPass(device_, render_pass_, nullptr);
	}
	for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
		if (image_available_[i]) {
			vkDestroySemaphore(device_, image_available_[i], nullptr);
		}
		if (render_finished_[i]) {
			vkDestroySemaphore(device_, render_finished_[i], nullptr);
		}
		if (in_flight_[i]) {
			vkDestroyFence(device_, in_flight_[i], nullptr);
		}
	}
	if (command_pool_) {
		vkDestroyCommandPool(device_, command_pool_, nullptr);
	}
	vkDestroyDevice(device_, nullptr);
	device_ = VK_NULL_HANDLE;

	if (debug_messenger_) {
		PFN_vkDestroyDebugUtilsMessengerEXT destroy =
				(PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance_, "vkDestroyDebugUtilsMessengerEXT");
		if (destroy) {
			destroy(instance_, debug_messenger_, nullptr);
		}
	}
	if (surface_) {
		vkDestroySurfaceKHR(instance_, surface_, nullptr);
		surface_ = VK_NULL_HANDLE;
	}
	if (instance_) {
		vkDestroyInstance(instance_, nullptr);
		instance_ = VK_NULL_HANDLE;
	}
}

} // namespace ksila
