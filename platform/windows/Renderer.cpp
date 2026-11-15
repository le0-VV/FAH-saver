#define VK_USE_PLATFORM_WIN32_KHR
#include "Renderer.hpp"
#include <vulkan/vulkan.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <vector>

namespace fah::windows {
namespace {
void require(VkResult result, const char* operation) {
    if (result != VK_SUCCESS)
        throw std::runtime_error(std::string(operation) + " failed (Vulkan " + std::to_string(result) + ")");
}
struct Buffer { VkBuffer buffer = VK_NULL_HANDLE; VkDeviceMemory memory = VK_NULL_HANDLE; std::size_t capacity = 0; };
struct Geometry { Buffer vertices, indices, instances; std::uint32_t indexCount = 0, instanceCount = 0; };
struct Parameters { fah::Matrix vp; float background[4]; float options[4]; };
static_assert(sizeof(Parameters) == 96 && sizeof(fah::Instance) == 80 && sizeof(fah::Vertex) == 24);

std::vector<std::uint32_t> shaderBytes(const wchar_t* name) {
    std::wstring executable(32768, L'\0');
    const auto length = GetModuleFileNameW(nullptr, executable.data(), DWORD(executable.size()));
    if (!length || length == executable.size()) throw std::runtime_error("Cannot locate shader directory");
    executable.resize(length);
    auto path = std::filesystem::path(executable).parent_path() / L"shaders" / name;
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) throw std::runtime_error("Cannot open compiled Vulkan shader");
    const auto bytes = file.tellg();
    if (bytes <= 0 || bytes % 4 != 0 || bytes > 4 * 1024 * 1024)
        throw std::runtime_error("Invalid compiled Vulkan shader size");
    std::vector<std::uint32_t> result(std::size_t(bytes) / 4);
    file.seekg(0);
    if (!file.read(reinterpret_cast<char*>(result.data()), bytes))
        throw std::runtime_error("Cannot read compiled Vulkan shader");
    return result;
}
} // namespace

struct Renderer::Impl {
    HWND window = nullptr;
    VkInstance instance = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    std::uint32_t family = 0;
    VkPhysicalDeviceMemoryProperties memory{};
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkExtent2D extent{};
    VkFormat colourFormat = VK_FORMAT_UNDEFINED;
    std::vector<VkImageView> views;
    std::vector<VkFramebuffer> framebuffers;
    std::vector<VkSemaphore> finished;
    VkImage depth = VK_NULL_HANDLE;
    VkDeviceMemory depthMemory = VK_NULL_HANDLE;
    VkImageView depthView = VK_NULL_HANDLE;
    VkRenderPass renderPass = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline modelPipeline = VK_NULL_HANDLE, backgroundPipeline = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer command = VK_NULL_HANDLE;
    VkSemaphore acquired = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkQueryPool queries = VK_NULL_HANDLE;
    bool queryWritten = false;
    std::uint32_t timestampBits = 0;
    float timestampPeriod = 0;
    std::string adapterName;
    Geometry atoms, bonds;
    float radius = 1;

    ~Impl() {
        if (device) {
            vkDeviceWaitIdle(device);
            destroySwapchain();
            destroyGeometry(atoms); destroyGeometry(bonds);
            if (queries) vkDestroyQueryPool(device, queries, nullptr);
            if (fence) vkDestroyFence(device, fence, nullptr);
            if (acquired) vkDestroySemaphore(device, acquired, nullptr);
            if (pool) vkDestroyCommandPool(device, pool, nullptr);
            if (layout) vkDestroyPipelineLayout(device, layout, nullptr);
            vkDestroyDevice(device, nullptr);
        }
        if (surface) vkDestroySurfaceKHR(instance, surface, nullptr);
        if (instance) vkDestroyInstance(instance, nullptr);
    }

    std::uint32_t memoryType(std::uint32_t types, VkMemoryPropertyFlags flags) {
        for (std::uint32_t i = 0; i < memory.memoryTypeCount; ++i)
            if ((types & (1u << i)) && (memory.memoryTypes[i].propertyFlags & flags) == flags) return i;
        throw std::runtime_error("Required Vulkan memory type unavailable");
    }

    void destroyBuffer(Buffer& buffer) {
        if (buffer.buffer) vkDestroyBuffer(device, buffer.buffer, nullptr);
        if (buffer.memory) vkFreeMemory(device, buffer.memory, nullptr);
        buffer = {};
    }
    void destroyGeometry(Geometry& geometry) {
        destroyBuffer(geometry.vertices); destroyBuffer(geometry.indices); destroyBuffer(geometry.instances);
    }

    void upload(Buffer& destination, const void* data, std::size_t size, VkBufferUsageFlags usage) {
        if (!size) return;
        if (size > destination.capacity) {
            destroyBuffer(destination);
            VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
            info.size = size; info.usage = usage; info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            require(vkCreateBuffer(device, &info, nullptr, &destination.buffer), "Create buffer");
            VkMemoryRequirements requirements;
            vkGetBufferMemoryRequirements(device, destination.buffer, &requirements);
            VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            allocation.allocationSize = requirements.size;
            // shortcut: host-visible geometry buffers; add device-local staging if measurements justify it.
            allocation.memoryTypeIndex = memoryType(requirements.memoryTypeBits,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            require(vkAllocateMemory(device, &allocation, nullptr, &destination.memory), "Allocate buffer memory");
            require(vkBindBufferMemory(device, destination.buffer, destination.memory, 0), "Bind buffer memory");
            destination.capacity = size;
        }
        void* mapped = nullptr;
        require(vkMapMemory(device, destination.memory, 0, size, 0, &mapped), "Map buffer");
        std::memcpy(mapped, data, size);
        vkUnmapMemory(device, destination.memory);
    }

    void initialise(HWND target) {
        window = target;
        VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        application.pApplicationName = "FAH Saver"; application.apiVersion = VK_API_VERSION_1_0;
        const char* instanceExtensions[]{VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME};
        VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        info.pApplicationInfo = &application;
        info.enabledExtensionCount = 2; info.ppEnabledExtensionNames = instanceExtensions;
        require(vkCreateInstance(&info, nullptr, &instance), "Create Vulkan instance");
        VkWin32SurfaceCreateInfoKHR surfaceInfo{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
        surfaceInfo.hinstance = GetModuleHandleW(nullptr); surfaceInfo.hwnd = window;
        require(vkCreateWin32SurfaceKHR(instance, &surfaceInfo, nullptr, &surface), "Create Win32 surface");
        std::uint32_t count = 0;
        require(vkEnumeratePhysicalDevices(instance, &count, nullptr), "Enumerate adapters");
        std::vector<VkPhysicalDevice> devices(count);
        require(vkEnumeratePhysicalDevices(instance, &count, devices.data()), "Enumerate adapters");
        int bestScore = -1;
        for (const auto candidate : devices) {
            std::uint32_t extensionsCount = 0;
            require(vkEnumerateDeviceExtensionProperties(candidate, nullptr, &extensionsCount, nullptr), "Enumerate device extensions");
            std::vector<VkExtensionProperties> extensions(extensionsCount);
            require(vkEnumerateDeviceExtensionProperties(candidate, nullptr, &extensionsCount, extensions.data()), "Enumerate device extensions");
            if (std::none_of(extensions.begin(), extensions.end(), [](const auto& extension) {
                return std::strcmp(extension.extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0;
            })) continue;
            std::uint32_t families = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &families, nullptr);
            std::vector<VkQueueFamilyProperties> queues(families);
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &families, queues.data());
            VkPhysicalDeviceProperties properties;
            vkGetPhysicalDeviceProperties(candidate, &properties);
            const int score = properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 2 : 1;
            for (std::uint32_t index = 0; index < families; ++index) {
                VkBool32 supportsPresent = false;
                require(vkGetPhysicalDeviceSurfaceSupportKHR(candidate, index, surface, &supportsPresent), "Check presentation support");
                if ((queues[index].queueFlags & VK_QUEUE_GRAPHICS_BIT) && supportsPresent && score > bestScore) {
                    physical = candidate; family = index; bestScore = score;
                    timestampBits = queues[index].timestampValidBits;
                    timestampPeriod = properties.limits.timestampPeriod;
                    adapterName = properties.deviceName;
                }
            }
        }
        if (!physical) throw std::runtime_error("No Vulkan adapter supports graphics and window presentation");
        vkGetPhysicalDeviceMemoryProperties(physical, &memory);
        const float priority = 1;
        VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        queueInfo.queueFamilyIndex = family; queueInfo.queueCount = 1; queueInfo.pQueuePriorities = &priority;
        const char* deviceExtensions[]{VK_KHR_SWAPCHAIN_EXTENSION_NAME};
        VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        deviceInfo.queueCreateInfoCount = 1; deviceInfo.pQueueCreateInfos = &queueInfo;
        deviceInfo.enabledExtensionCount = 1; deviceInfo.ppEnabledExtensionNames = deviceExtensions;
        require(vkCreateDevice(physical, &deviceInfo, nullptr, &device), "Create Vulkan device");
        vkGetDeviceQueue(device, family, 0, &queue);
        VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        poolInfo.queueFamilyIndex = family; poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        require(vkCreateCommandPool(device, &poolInfo, nullptr, &pool), "Create command pool");
        VkCommandBufferAllocateInfo commandInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        commandInfo.commandPool = pool; commandInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; commandInfo.commandBufferCount = 1;
        require(vkAllocateCommandBuffers(device, &commandInfo, &command), "Allocate command buffer");
        VkSemaphoreCreateInfo semaphoreInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        require(vkCreateSemaphore(device, &semaphoreInfo, nullptr, &acquired), "Create acquisition semaphore");
        VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        require(vkCreateFence(device, &fenceInfo, nullptr, &fence), "Create submission fence");
        VkPushConstantRange range{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(Parameters)};
        VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        layoutInfo.pushConstantRangeCount = 1; layoutInfo.pPushConstantRanges = &range;
        require(vkCreatePipelineLayout(device, &layoutInfo, nullptr, &layout), "Create pipeline layout");
        if (timestampBits) {
            VkQueryPoolCreateInfo queryInfo{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
            queryInfo.queryType = VK_QUERY_TYPE_TIMESTAMP; queryInfo.queryCount = 2;
            require(vkCreateQueryPool(device, &queryInfo, nullptr, &queries), "Create timestamp queries");
        }
        const auto sphere = fah::sphereMesh(); const auto cylinder = fah::cylinderMesh();
        upload(atoms.vertices, sphere.vertices.data(), sphere.vertices.size() * sizeof(fah::Vertex), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
        upload(atoms.indices, sphere.indices.data(), sphere.indices.size() * sizeof(std::uint32_t), VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
        atoms.indexCount = std::uint32_t(sphere.indices.size());
        upload(bonds.vertices, cylinder.vertices.data(), cylinder.vertices.size() * sizeof(fah::Vertex), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
        upload(bonds.indices, cylinder.indices.data(), cylinder.indices.size() * sizeof(std::uint32_t), VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
        bonds.indexCount = std::uint32_t(cylinder.indices.size());
        createSwapchain();
    }

    VkPipeline pipeline(bool background) {
        const auto vertexBytes = shaderBytes(background ? L"background.vert.spv" : L"model.vert.spv");
        const auto fragmentBytes = shaderBytes(background ? L"background.frag.spv" : L"model.frag.spv");
        VkShaderModule vertex = VK_NULL_HANDLE, fragment = VK_NULL_HANDLE;
        VkShaderModuleCreateInfo module{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        module.codeSize = vertexBytes.size() * 4; module.pCode = vertexBytes.data();
        require(vkCreateShaderModule(device, &module, nullptr, &vertex), "Create vertex shader");
        module.codeSize = fragmentBytes.size() * 4; module.pCode = fragmentBytes.data();
        auto result = vkCreateShaderModule(device, &module, nullptr, &fragment);
        if (result != VK_SUCCESS) { vkDestroyShaderModule(device, vertex, nullptr); require(result, "Create fragment shader"); }
        VkPipelineShaderStageCreateInfo stages[2]{};
        for (auto& stage : stages) { stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO; stage.pName = "main"; }
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT; stages[0].module = vertex;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = fragment;
        const VkVertexInputBindingDescription bindings[]{{0, sizeof(fah::Vertex), VK_VERTEX_INPUT_RATE_VERTEX},
            {1, sizeof(fah::Instance), VK_VERTEX_INPUT_RATE_INSTANCE}};
        const VkVertexInputAttributeDescription attributes[]{
            {0,0,VK_FORMAT_R32G32B32_SFLOAT,0}, {1,0,VK_FORMAT_R32G32B32_SFLOAT,12},
            {2,1,VK_FORMAT_R32G32B32A32_SFLOAT,0}, {3,1,VK_FORMAT_R32G32B32A32_SFLOAT,16},
            {4,1,VK_FORMAT_R32G32B32A32_SFLOAT,32}, {5,1,VK_FORMAT_R32G32B32A32_SFLOAT,48},
            {6,1,VK_FORMAT_R32G32B32A32_SFLOAT,64}};
        VkPipelineVertexInputStateCreateInfo vertexInput{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        if (!background) {
            vertexInput.vertexBindingDescriptionCount = 2; vertexInput.pVertexBindingDescriptions = bindings;
            vertexInput.vertexAttributeDescriptionCount = 7; vertexInput.pVertexAttributeDescriptions = attributes;
        }
        VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        viewport.viewportCount = 1; viewport.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        raster.polygonMode = VK_POLYGON_MODE_FILL; raster.cullMode = VK_CULL_MODE_NONE; raster.lineWidth = 1;
        VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo depthState{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        depthState.depthTestEnable = !background; depthState.depthWriteEnable = !background; depthState.depthCompareOp = VK_COMPARE_OP_LESS;
        VkPipelineColorBlendAttachmentState attachment{};
        attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        blend.attachmentCount = 1; blend.pAttachments = &attachment;
        const VkDynamicState states[]{VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dynamic.dynamicStateCount = 2; dynamic.pDynamicStates = states;
        VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        info.stageCount = 2; info.pStages = stages; info.pVertexInputState = &vertexInput;
        info.pInputAssemblyState = &assembly; info.pViewportState = &viewport; info.pRasterizationState = &raster;
        info.pMultisampleState = &multisample; info.pDepthStencilState = &depthState; info.pColorBlendState = &blend;
        info.pDynamicState = &dynamic; info.layout = layout; info.renderPass = renderPass;
        VkPipeline output = VK_NULL_HANDLE;
        result = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &info, nullptr, &output);
        vkDestroyShaderModule(device, fragment, nullptr); vkDestroyShaderModule(device, vertex, nullptr);
        require(result, "Create graphics pipeline");
        return output;
    }

    void destroySwapchain() {
        for (auto framebuffer : framebuffers) vkDestroyFramebuffer(device, framebuffer, nullptr);
        framebuffers.clear();
        if (modelPipeline) vkDestroyPipeline(device, modelPipeline, nullptr);
        if (backgroundPipeline) vkDestroyPipeline(device, backgroundPipeline, nullptr);
        modelPipeline = backgroundPipeline = VK_NULL_HANDLE;
        if (renderPass) vkDestroyRenderPass(device, renderPass, nullptr); renderPass = VK_NULL_HANDLE;
        if (depthView) vkDestroyImageView(device, depthView, nullptr); depthView = VK_NULL_HANDLE;
        if (depth) vkDestroyImage(device, depth, nullptr); depth = VK_NULL_HANDLE;
        if (depthMemory) vkFreeMemory(device, depthMemory, nullptr); depthMemory = VK_NULL_HANDLE;
        for (auto view : views) vkDestroyImageView(device, view, nullptr);
        views.clear();
        for (auto semaphore : finished) vkDestroySemaphore(device, semaphore, nullptr);
        finished.clear();
        if (swapchain) vkDestroySwapchainKHR(device, swapchain, nullptr); swapchain = VK_NULL_HANDLE;
        extent = {};
    }

    void createSwapchain() {
        RECT rectangle; GetClientRect(window, &rectangle);
        if (rectangle.right <= 0 || rectangle.bottom <= 0) return;
        VkSurfaceCapabilitiesKHR capabilities;
        require(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical, surface, &capabilities), "Read surface capabilities");
        std::uint32_t count = 0;
        require(vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &count, nullptr), "Read surface formats");
        if (!count) throw std::runtime_error("Vulkan surface has no colour formats");
        std::vector<VkSurfaceFormatKHR> formats(count);
        require(vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &count, formats.data()), "Read surface formats");
        auto format = formats.front();
        for (auto candidate : formats)
            if (candidate.format == VK_FORMAT_B8G8R8A8_UNORM && candidate.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) format = candidate;
        if (format.format == VK_FORMAT_UNDEFINED) format.format = VK_FORMAT_B8G8R8A8_UNORM;
        colourFormat = format.format;
        extent = capabilities.currentExtent;
        if (extent.width == std::numeric_limits<std::uint32_t>::max()) {
            extent.width = std::clamp(std::uint32_t(rectangle.right), capabilities.minImageExtent.width, capabilities.maxImageExtent.width);
            extent.height = std::clamp(std::uint32_t(rectangle.bottom), capabilities.minImageExtent.height, capabilities.maxImageExtent.height);
        }
        count = capabilities.minImageCount + 1;
        if (capabilities.maxImageCount) count = std::min(count, capabilities.maxImageCount);
        VkSwapchainCreateInfoKHR info{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
        info.surface = surface; info.minImageCount = count; info.imageFormat = format.format; info.imageColorSpace = format.colorSpace;
        info.imageExtent = extent; info.imageArrayLayers = 1; info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE; info.preTransform = capabilities.currentTransform;
        if (!(capabilities.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR))
            throw std::runtime_error("Vulkan surface does not support an opaque screensaver");
        info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR; info.presentMode = VK_PRESENT_MODE_FIFO_KHR; info.clipped = VK_TRUE;
        require(vkCreateSwapchainKHR(device, &info, nullptr, &swapchain), "Create swapchain");
        require(vkGetSwapchainImagesKHR(device, swapchain, &count, nullptr), "Read swapchain images");
        std::vector<VkImage> images(count);
        require(vkGetSwapchainImagesKHR(device, swapchain, &count, images.data()), "Read swapchain images");
        for (auto image : images) {
            VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            view.image = image; view.viewType = VK_IMAGE_VIEW_TYPE_2D; view.format = colourFormat;
            view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; view.subresourceRange.levelCount = 1; view.subresourceRange.layerCount = 1;
            VkImageView handle = VK_NULL_HANDLE;
            require(vkCreateImageView(device, &view, nullptr, &handle), "Create colour image view"); views.push_back(handle);
            VkSemaphoreCreateInfo semaphoreInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            VkSemaphore semaphore = VK_NULL_HANDLE;
            require(vkCreateSemaphore(device, &semaphoreInfo, nullptr, &semaphore), "Create presentation semaphore"); finished.push_back(semaphore);
        }
        VkFormatProperties depthProperties;
        vkGetPhysicalDeviceFormatProperties(physical, VK_FORMAT_D32_SFLOAT, &depthProperties);
        if (!(depthProperties.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT))
            throw std::runtime_error("Vulkan D32 depth attachments are unavailable");
        VkImageCreateInfo depthInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        depthInfo.imageType = VK_IMAGE_TYPE_2D; depthInfo.format = VK_FORMAT_D32_SFLOAT;
        depthInfo.extent = {extent.width, extent.height,1}; depthInfo.mipLevels = 1; depthInfo.arrayLayers = 1;
        depthInfo.samples = VK_SAMPLE_COUNT_1_BIT; depthInfo.tiling = VK_IMAGE_TILING_OPTIMAL; depthInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        require(vkCreateImage(device, &depthInfo, nullptr, &depth), "Create depth image");
        VkMemoryRequirements requirements; vkGetImageMemoryRequirements(device, depth, &requirements);
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocation.allocationSize = requirements.size; allocation.memoryTypeIndex = memoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        require(vkAllocateMemory(device, &allocation, nullptr, &depthMemory), "Allocate depth memory");
        require(vkBindImageMemory(device, depth, depthMemory, 0), "Bind depth image");
        VkImageViewCreateInfo depthViewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        depthViewInfo.image = depth; depthViewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D; depthViewInfo.format = VK_FORMAT_D32_SFLOAT;
        depthViewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        depthViewInfo.subresourceRange.levelCount = 1; depthViewInfo.subresourceRange.layerCount = 1;
        require(vkCreateImageView(device, &depthViewInfo, nullptr, &depthView), "Create depth image view");
        VkAttachmentDescription attachments[2]{};
        attachments[0].format = colourFormat; attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
        attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachments[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED; attachments[0].finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        attachments[1].format = VK_FORMAT_D32_SFLOAT; attachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
        attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachments[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED; attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        const VkAttachmentReference colour{0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}, depthReference{1,VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
        VkSubpassDescription subpass{}; subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1; subpass.pColorAttachments = &colour; subpass.pDepthStencilAttachment = &depthReference;
        VkSubpassDependency dependency{}; dependency.srcSubpass = VK_SUBPASS_EXTERNAL; dependency.dstSubpass = 0;
        dependency.srcStageMask = dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        VkRenderPassCreateInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        pass.attachmentCount = 2; pass.pAttachments = attachments; pass.subpassCount = 1; pass.pSubpasses = &subpass;
        pass.dependencyCount = 1; pass.pDependencies = &dependency;
        require(vkCreateRenderPass(device, &pass, nullptr, &renderPass), "Create render pass");
        modelPipeline = pipeline(false); backgroundPipeline = pipeline(true);
        for (auto view : views) {
            VkImageView attachmentsForFrame[]{view,depthView};
            VkFramebufferCreateInfo framebufferInfo{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
            framebufferInfo.renderPass = renderPass; framebufferInfo.attachmentCount = 2; framebufferInfo.pAttachments = attachmentsForFrame;
            framebufferInfo.width = extent.width; framebufferInfo.height = extent.height; framebufferInfo.layers = 1;
            VkFramebuffer framebuffer = VK_NULL_HANDLE;
            require(vkCreateFramebuffer(device, &framebufferInfo, nullptr, &framebuffer), "Create framebuffer"); framebuffers.push_back(framebuffer);
        }
    }

    void resize() {
        require(vkDeviceWaitIdle(device), "Wait before resizing");
        destroySwapchain(); createSwapchain();
    }

    void geometry(const Geometry& geometry) {
        if (!geometry.instanceCount) return;
        VkBuffer buffers[]{geometry.vertices.buffer,geometry.instances.buffer};
        const VkDeviceSize offsets[]{0,0};
        vkCmdBindVertexBuffers(command,0,2,buffers,offsets);
        vkCmdBindIndexBuffer(command,geometry.indices.buffer,0,VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(command,geometry.indexCount,geometry.instanceCount,0,0,0);
    }

    DrawTiming draw(float angle, bool sky, const float colour[3]) {
        RECT rectangle; GetClientRect(window,&rectangle);
        if (rectangle.right <= 0 || rectangle.bottom <= 0) return {};
        if (!swapchain) createSwapchain();
        require(vkWaitForFences(device,1,&fence,VK_TRUE,UINT64_MAX), "Wait for previous frame");
        DrawTiming timing;
        if (queries && queryWritten) {
            std::uint64_t ticks[2]{};
            require(vkGetQueryPoolResults(device,queries,0,2,sizeof(ticks),ticks,sizeof(std::uint64_t),VK_QUERY_RESULT_64_BIT), "Read GPU timestamps");
            const std::uint64_t mask = timestampBits >= 64 ? UINT64_MAX : (std::uint64_t(1) << timestampBits) - 1;
            timing.gpuMilliseconds = double((ticks[1] - ticks[0]) & mask) * timestampPeriod / 1e6;
        }
        std::uint32_t image = 0;
        const auto acquire = vkAcquireNextImageKHR(device,swapchain,UINT64_MAX,acquired,VK_NULL_HANDLE,&image);
        if (acquire == VK_ERROR_OUT_OF_DATE_KHR) { resize(); return {}; }
        if (acquire != VK_SUBOPTIMAL_KHR) require(acquire, "Acquire image");
        const auto start = std::chrono::steady_clock::now();
        require(vkResetCommandBuffer(command,0), "Reset command buffer");
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        require(vkBeginCommandBuffer(command,&begin), "Begin command buffer");
        if (queries) { vkCmdResetQueryPool(command,queries,0,2); vkCmdWriteTimestamp(command,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,queries,0); }
        VkClearValue clears[2]{}; clears[0].color = {{0,0,0,1}}; clears[1].depthStencil = {1,0};
        VkRenderPassBeginInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        pass.renderPass = renderPass; pass.framebuffer = framebuffers[image]; pass.renderArea.extent = extent;
        pass.clearValueCount = 2; pass.pClearValues = clears;
        vkCmdBeginRenderPass(command,&pass,VK_SUBPASS_CONTENTS_INLINE);
        const VkViewport viewport{0,0,float(extent.width),float(extent.height),0,1};
        const VkRect2D scissor{{0,0},extent};
        vkCmdSetViewport(command,0,1,&viewport); vkCmdSetScissor(command,0,1,&scissor);
        Parameters parameters{fah::viewProjection(angle,float(extent.width)/extent.height,radius),
            {colour[0],colour[1],colour[2],1},{sky ? 1.f : 0.f,0,0,0}};
        vkCmdPushConstants(command,layout,VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof(parameters),&parameters);
        vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_GRAPHICS,backgroundPipeline); vkCmdDraw(command,3,1,0,0);
        vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_GRAPHICS,modelPipeline); geometry(atoms); geometry(bonds);
        vkCmdEndRenderPass(command);
        if (queries) vkCmdWriteTimestamp(command,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,queries,1);
        require(vkEndCommandBuffer(command), "End command buffer");
        const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.waitSemaphoreCount = 1; submit.pWaitSemaphores = &acquired; submit.pWaitDstStageMask = &waitStage;
        submit.commandBufferCount = 1; submit.pCommandBuffers = &command;
        submit.signalSemaphoreCount = 1; submit.pSignalSemaphores = &finished[image];
        require(vkResetFences(device,1,&fence), "Reset submission fence");
        require(vkQueueSubmit(queue,1,&submit,fence), "Submit frame");
        queryWritten = bool(queries);
        VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        present.waitSemaphoreCount = 1; present.pWaitSemaphores = &finished[image];
        present.swapchainCount = 1; present.pSwapchains = &swapchain; present.pImageIndices = &image;
        const auto result = vkQueuePresentKHR(queue,&present);
        if (result != VK_SUBOPTIMAL_KHR && result != VK_ERROR_OUT_OF_DATE_KHR) require(result, "Present frame");
        timing.cpuMilliseconds = std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
        timing.rendered = true;
        if (result == VK_SUBOPTIMAL_KHR || result == VK_ERROR_OUT_OF_DATE_KHR) resize();
        return timing;
    }
};

Renderer::Renderer(HWND window) : impl(std::make_unique<Impl>()) { impl->initialise(window); }
Renderer::~Renderer() = default;
void Renderer::setScene(const fah::Scene& scene) {
    require(vkWaitForFences(impl->device,1,&impl->fence,VK_TRUE,UINT64_MAX), "Wait before scene update");
    if (scene.atoms.empty()) {
        impl->destroyBuffer(impl->atoms.instances); impl->destroyBuffer(impl->bonds.instances);
    }
    impl->upload(impl->atoms.instances,scene.atoms.data(),scene.atoms.size()*sizeof(fah::Instance),VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    impl->upload(impl->bonds.instances,scene.bonds.data(),scene.bonds.size()*sizeof(fah::Instance),VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    impl->atoms.instanceCount = std::uint32_t(scene.atoms.size()); impl->bonds.instanceCount = std::uint32_t(scene.bonds.size());
    impl->radius = scene.radius;
}
DrawTiming Renderer::draw(float angle,bool sky,const float colour[3]) { return impl->draw(angle,sky,colour); }
void Renderer::resize() { impl->resize(); }
const std::string& Renderer::adapter() const { return impl->adapterName; }

} // namespace fah::windows
