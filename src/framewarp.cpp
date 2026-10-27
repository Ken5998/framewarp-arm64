// framewarp: Vulkan layer that shows extra frames between the frames a game
// renders. Stage 1 is a probe: the extra frames are copies of the last real
// frame, and the layer measures what they cost on the GPU and whether they
// reach the display on time.
//
// How it works: on every vkQueuePresentKHR of the game the layer copies the
// game's image into its own history image, presents the game's image, then
// acquires (multiplier - 1) more images from the same swapchain, draws the
// history image into each and presents them. With FIFO each present takes
// one vblank, so on a 60 Hz panel multiplier 3 shows 60 frames per second
// out of 20 real ones. The generated frames are queued right behind the real
// frame, so they never wait behind the next frame's GPU work.

#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>

#include <algorithm>
#include <cinttypes>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "fullscreen_vert.h"
#include "probe_frag.h"

#define FW_EXPORT extern "C" __attribute__((visibility("default")))

namespace {

constexpr int kMaxMultiplier = 4;
constexpr int kRing = 8;

// ---------------------------------------------------------------- config

struct Config {
    int multiplier = 2;
    bool force_fifo = true;
    int taps = 1;
    float tint = 0.0f;
    float shift = 0.0f;
    double log_every = 2.0;
    std::string log_path;
};

Config g_cfg;
FILE *g_log = nullptr;
std::mutex g_log_mutex;

int env_int(const char *name, int def)
{
    const char *v = getenv(name);
    return v && *v ? atoi(v) : def;
}

float env_float(const char *name, float def)
{
    const char *v = getenv(name);
    return v && *v ? strtof(v, nullptr) : def;
}

void load_config()
{
    static std::once_flag once;
    std::call_once(once, [] {
        g_cfg.multiplier = std::clamp(env_int("FRAMEWARP_MULT", 2), 1, kMaxMultiplier);
        g_cfg.force_fifo = env_int("FRAMEWARP_FORCE_FIFO", 1) != 0;
        g_cfg.taps = env_int("FRAMEWARP_TAPS", 1) > 1 ? 9 : 1;
        g_cfg.tint = std::clamp(env_float("FRAMEWARP_TINT", 0.0f), 0.0f, 1.0f);
        g_cfg.shift = env_float("FRAMEWARP_SHIFT", 0.0f);
        g_cfg.log_every = std::max(0.5f, env_float("FRAMEWARP_LOG_EVERY", 2.0f));
        if (const char *p = getenv("FRAMEWARP_LOG"); p && *p)
            g_cfg.log_path = p;
        else if (const char *home = getenv("HOME"); home && *home)
            g_cfg.log_path = std::string(home) + "/framewarp.log";
        if (!g_cfg.log_path.empty())
            g_log = fopen(g_cfg.log_path.c_str(), "a");
    });
}

void logf(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    std::lock_guard<std::mutex> lock(g_log_mutex);
    fprintf(stderr, "framewarp: %s\n", buf);
    if (g_log) {
        fprintf(g_log, "%s\n", buf);
        fflush(g_log);
    }
}

double now_s()
{
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

// ---------------------------------------------------------------- dispatch

template <typename T> void *key(T obj) { return *reinterpret_cast<void **>(obj); }

#define INSTANCE_FUNCS(X)                                                      \
    X(DestroyInstance)                                                         \
    X(GetPhysicalDeviceMemoryProperties)                                       \
    X(GetPhysicalDeviceQueueFamilyProperties)                                  \
    X(GetPhysicalDeviceProperties)                                             \
    X(GetPhysicalDeviceSurfaceCapabilitiesKHR)

#define DEVICE_FUNCS(X)                                                        \
    X(DestroyDevice)                                                           \
    X(GetDeviceQueue)                                                          \
    X(GetDeviceQueue2)                                                         \
    X(CreateSwapchainKHR)                                                      \
    X(DestroySwapchainKHR)                                                     \
    X(GetSwapchainImagesKHR)                                                   \
    X(AcquireNextImageKHR)                                                     \
    X(QueuePresentKHR)                                                         \
    X(QueueSubmit)                                                             \
    X(DeviceWaitIdle)                                                          \
    X(CreateCommandPool)                                                       \
    X(DestroyCommandPool)                                                      \
    X(AllocateCommandBuffers)                                                  \
    X(BeginCommandBuffer)                                                      \
    X(EndCommandBuffer)                                                        \
    X(ResetCommandBuffer)                                                      \
    X(CmdPipelineBarrier)                                                      \
    X(CmdCopyImage)                                                            \
    X(CmdBeginRenderPass)                                                      \
    X(CmdEndRenderPass)                                                        \
    X(CmdBindPipeline)                                                         \
    X(CmdBindDescriptorSets)                                                   \
    X(CmdPushConstants)                                                        \
    X(CmdDraw)                                                                 \
    X(CmdSetViewport)                                                          \
    X(CmdSetScissor)                                                           \
    X(CmdWriteTimestamp)                                                       \
    X(CmdResetQueryPool)                                                       \
    X(CreateQueryPool)                                                         \
    X(DestroyQueryPool)                                                        \
    X(GetQueryPoolResults)                                                     \
    X(CreateImage)                                                             \
    X(DestroyImage)                                                            \
    X(GetImageMemoryRequirements)                                              \
    X(AllocateMemory)                                                          \
    X(FreeMemory)                                                              \
    X(BindImageMemory)                                                         \
    X(CreateImageView)                                                         \
    X(DestroyImageView)                                                        \
    X(CreateSampler)                                                           \
    X(DestroySampler)                                                          \
    X(CreateDescriptorSetLayout)                                               \
    X(DestroyDescriptorSetLayout)                                              \
    X(CreateDescriptorPool)                                                    \
    X(DestroyDescriptorPool)                                                   \
    X(AllocateDescriptorSets)                                                  \
    X(UpdateDescriptorSets)                                                    \
    X(CreatePipelineLayout)                                                    \
    X(DestroyPipelineLayout)                                                   \
    X(CreateShaderModule)                                                      \
    X(DestroyShaderModule)                                                     \
    X(CreateGraphicsPipelines)                                                 \
    X(DestroyPipeline)                                                         \
    X(CreateRenderPass)                                                        \
    X(DestroyRenderPass)                                                       \
    X(CreateFramebuffer)                                                       \
    X(DestroyFramebuffer)                                                      \
    X(CreateSemaphore)                                                         \
    X(DestroySemaphore)                                                        \
    X(CreateFence)                                                             \
    X(DestroyFence)                                                            \
    X(WaitForFences)                                                           \
    X(ResetFences)

#define DECLARE(name) PFN_vk##name name = nullptr;

struct InstanceData {
    VkInstance instance;
    PFN_vkGetInstanceProcAddr gipa;
    INSTANCE_FUNCS(DECLARE)
};

struct SwapchainData;

struct DeviceData {
    VkDevice device;
    VkPhysicalDevice physical;
    InstanceData *inst;
    PFN_vkGetDeviceProcAddr gdpa;
    PFN_vkSetDeviceLoaderData set_loader_data;
    DEVICE_FUNCS(DECLARE)
    std::vector<VkQueueFamilyProperties> families;
    VkPhysicalDeviceMemoryProperties mem;
    float timestamp_period;
};

struct QueueData {
    DeviceData *dev;
    uint32_t family;
};

// One set of objects per in-flight present cycle.
struct Slot {
    VkCommandBuffer cmd[kMaxMultiplier] = {};
    VkSemaphore copy_done = VK_NULL_HANDLE;
    VkSemaphore acquired[kMaxMultiplier] = {};
    VkSemaphore drawn[kMaxMultiplier] = {};
    VkFence fence = VK_NULL_HANDLE;
    VkQueryPool queries = VK_NULL_HANDLE;
    int generated = 0; // generated frames recorded in the last use
    bool used = false;
};

struct Stats {
    double window_start = 0;
    double last_real = 0;
    int real = 0;
    int shown = 0;
    int missed = 0;
    double interval_max = 0;
    double copy_ms = 0, gen_ms = 0, gen_ms_max = 0;
    int copy_n = 0, gen_n = 0;
    double extra_cpu_ms = 0, extra_cpu_ms_max = 0;
};

struct SwapchainData {
    DeviceData *dev;
    VkSwapchainKHR swapchain;
    VkFormat format;
    VkExtent2D extent;
    VkPresentModeKHR mode;
    std::vector<VkImage> images;
    std::vector<VkImageView> views;
    std::vector<VkFramebuffer> framebuffers;

    bool ready = false;
    bool broken = false;
    uint32_t family = 0;

    VkImage history = VK_NULL_HANDLE;
    VkDeviceMemory history_mem = VK_NULL_HANDLE;
    VkImageView history_view = VK_NULL_HANDLE;
    bool history_valid = false;
    VkSampler sampler = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkDescriptorSet set = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkRenderPass pass = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkCommandPool cmd_pool = VK_NULL_HANDLE;
    Slot slots[kRing];
    int next_slot = 0;
    bool timestamps = false;

    Stats stats;
};

std::mutex g_mutex;
std::unordered_map<void *, std::unique_ptr<InstanceData>> g_instances; // by dispatch key
std::unordered_map<void *, std::unique_ptr<DeviceData>> g_devices;     // by dispatch key
std::unordered_map<VkQueue, QueueData> g_queues;
std::unordered_map<VkSwapchainKHR, std::unique_ptr<SwapchainData>> g_swapchains;

InstanceData *instance_of(void *k)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_instances.find(k);
    return it == g_instances.end() ? nullptr : it->second.get();
}

DeviceData *device_of(void *k)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_devices.find(k);
    return it == g_devices.end() ? nullptr : it->second.get();
}

// ---------------------------------------------------------------- instance

VKAPI_ATTR VkResult VKAPI_CALL CreateInstance(const VkInstanceCreateInfo *ci,
                                              const VkAllocationCallbacks *alloc,
                                              VkInstance *out)
{
    auto *link = (VkLayerInstanceCreateInfo *)ci->pNext;
    while (link && !(link->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO &&
                     link->function == VK_LAYER_LINK_INFO))
        link = (VkLayerInstanceCreateInfo *)link->pNext;
    if (!link)
        return VK_ERROR_INITIALIZATION_FAILED;

    PFN_vkGetInstanceProcAddr gipa = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    link->u.pLayerInfo = link->u.pLayerInfo->pNext;
    auto create = (PFN_vkCreateInstance)gipa(VK_NULL_HANDLE, "vkCreateInstance");
    VkResult r = create(ci, alloc, out);
    if (r != VK_SUCCESS)
        return r;

    load_config();
    auto data = std::make_unique<InstanceData>();
    data->instance = *out;
    data->gipa = gipa;
#define LOAD(name) data->name = (PFN_vk##name)gipa(*out, "vk" #name);
    INSTANCE_FUNCS(LOAD)
#undef LOAD
    std::lock_guard<std::mutex> lock(g_mutex);
    g_instances[key(*out)] = std::move(data);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL DestroyInstance(VkInstance instance, const VkAllocationCallbacks *alloc)
{
    InstanceData *data = instance_of(key(instance));
    if (!data)
        return;
    data->DestroyInstance(instance, alloc);
    std::lock_guard<std::mutex> lock(g_mutex);
    g_instances.erase(key(instance));
}

// ---------------------------------------------------------------- device

VKAPI_ATTR VkResult VKAPI_CALL CreateDevice(VkPhysicalDevice physical, const VkDeviceCreateInfo *ci,
                                            const VkAllocationCallbacks *alloc, VkDevice *out)
{
    InstanceData *inst = instance_of(key(physical));
    auto *link = (VkLayerDeviceCreateInfo *)ci->pNext;
    while (link && !(link->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO &&
                     link->function == VK_LAYER_LINK_INFO))
        link = (VkLayerDeviceCreateInfo *)link->pNext;
    if (!inst || !link)
        return VK_ERROR_INITIALIZATION_FAILED;

    PFN_vkGetInstanceProcAddr gipa = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    PFN_vkGetDeviceProcAddr gdpa = link->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    link->u.pLayerInfo = link->u.pLayerInfo->pNext;
    auto create = (PFN_vkCreateDevice)gipa(inst->instance, "vkCreateDevice");
    VkResult r = create(physical, ci, alloc, out);
    if (r != VK_SUCCESS)
        return r;

    auto data = std::make_unique<DeviceData>();
    data->device = *out;
    data->physical = physical;
    data->inst = inst;
    data->gdpa = gdpa;
    data->set_loader_data = nullptr;
    for (auto *l = (VkLayerDeviceCreateInfo *)ci->pNext; l; l = (VkLayerDeviceCreateInfo *)l->pNext)
        if (l->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO &&
            l->function == VK_LOADER_DATA_CALLBACK)
            data->set_loader_data = l->u.pfnSetDeviceLoaderData;
#define LOAD(name) data->name = (PFN_vk##name)gdpa(*out, "vk" #name);
    DEVICE_FUNCS(LOAD)
#undef LOAD

    uint32_t n = 0;
    inst->GetPhysicalDeviceQueueFamilyProperties(physical, &n, nullptr);
    data->families.resize(n);
    inst->GetPhysicalDeviceQueueFamilyProperties(physical, &n, data->families.data());
    inst->GetPhysicalDeviceMemoryProperties(physical, &data->mem);
    VkPhysicalDeviceProperties props;
    inst->GetPhysicalDeviceProperties(physical, &props);
    data->timestamp_period = props.limits.timestampPeriod;
    logf("device %s, multiplier %d, taps %d, force_fifo %d", props.deviceName,
         g_cfg.multiplier, g_cfg.taps, g_cfg.force_fifo);

    std::lock_guard<std::mutex> lock(g_mutex);
    g_devices[key(*out)] = std::move(data);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL DestroyDevice(VkDevice device, const VkAllocationCallbacks *alloc)
{
    DeviceData *dev = device_of(key(device));
    if (!dev)
        return;
    dev->DestroyDevice(device, alloc);
    std::lock_guard<std::mutex> lock(g_mutex);
    for (auto it = g_queues.begin(); it != g_queues.end();)
        it = it->second.dev == dev ? g_queues.erase(it) : std::next(it);
    g_devices.erase(key(device));
}

void remember_queue(DeviceData *dev, VkQueue queue, uint32_t family)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_queues[queue] = QueueData{dev, family};
}

VKAPI_ATTR void VKAPI_CALL GetDeviceQueue(VkDevice device, uint32_t family, uint32_t index, VkQueue *queue)
{
    DeviceData *dev = device_of(key(device));
    dev->GetDeviceQueue(device, family, index, queue);
    if (*queue)
        remember_queue(dev, *queue, family);
}

VKAPI_ATTR void VKAPI_CALL GetDeviceQueue2(VkDevice device, const VkDeviceQueueInfo2 *info, VkQueue *queue)
{
    DeviceData *dev = device_of(key(device));
    dev->GetDeviceQueue2(device, info, queue);
    if (*queue)
        remember_queue(dev, *queue, info->queueFamilyIndex);
}

// ---------------------------------------------------------------- swapchain resources

int find_memory(DeviceData *dev, uint32_t bits, VkMemoryPropertyFlags want)
{
    for (uint32_t i = 0; i < dev->mem.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (dev->mem.memoryTypes[i].propertyFlags & want) == want)
            return (int)i;
    return -1;
}

void destroy_resources(SwapchainData *sc)
{
    DeviceData *d = sc->dev;
    VkDevice dv = d->device;
    if (sc->ready || sc->cmd_pool)
        d->DeviceWaitIdle(dv);
    for (auto &s : sc->slots) {
        if (s.copy_done) d->DestroySemaphore(dv, s.copy_done, nullptr);
        for (int k = 0; k < kMaxMultiplier; k++) {
            if (s.acquired[k]) d->DestroySemaphore(dv, s.acquired[k], nullptr);
            if (s.drawn[k]) d->DestroySemaphore(dv, s.drawn[k], nullptr);
        }
        if (s.fence) d->DestroyFence(dv, s.fence, nullptr);
        if (s.queries) d->DestroyQueryPool(dv, s.queries, nullptr);
        s = Slot{};
    }
    if (sc->cmd_pool) d->DestroyCommandPool(dv, sc->cmd_pool, nullptr);
    for (auto fb : sc->framebuffers) if (fb) d->DestroyFramebuffer(dv, fb, nullptr);
    for (auto v : sc->views) if (v) d->DestroyImageView(dv, v, nullptr);
    if (sc->pipeline) d->DestroyPipeline(dv, sc->pipeline, nullptr);
    if (sc->pass) d->DestroyRenderPass(dv, sc->pass, nullptr);
    if (sc->layout) d->DestroyPipelineLayout(dv, sc->layout, nullptr);
    if (sc->pool) d->DestroyDescriptorPool(dv, sc->pool, nullptr);
    if (sc->set_layout) d->DestroyDescriptorSetLayout(dv, sc->set_layout, nullptr);
    if (sc->sampler) d->DestroySampler(dv, sc->sampler, nullptr);
    if (sc->history_view) d->DestroyImageView(dv, sc->history_view, nullptr);
    if (sc->history) d->DestroyImage(dv, sc->history, nullptr);
    if (sc->history_mem) d->FreeMemory(dv, sc->history_mem, nullptr);
    sc->framebuffers.clear();
    sc->views.clear();
    sc->cmd_pool = VK_NULL_HANDLE;
    sc->ready = false;
}

#define CHECK(expr)                                                            \
    do {                                                                       \
        VkResult _r = (expr);                                                  \
        if (_r != VK_SUCCESS) {                                                \
            logf("%s failed: %d", #expr, (int)_r);                             \
            return false;                                                      \
        }                                                                      \
    } while (0)

bool create_resources(SwapchainData *sc, uint32_t family)
{
    DeviceData *d = sc->dev;
    VkDevice dv = d->device;
    sc->family = family;

    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = sc->format;
    ici.extent = {sc->extent.width, sc->extent.height, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    CHECK(d->CreateImage(dv, &ici, nullptr, &sc->history));
    VkMemoryRequirements req;
    d->GetImageMemoryRequirements(dv, sc->history, &req);
    int type = find_memory(d, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type < 0)
        type = find_memory(d, req.memoryTypeBits, 0);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = (uint32_t)type;
    CHECK(d->AllocateMemory(dv, &mai, nullptr, &sc->history_mem));
    CHECK(d->BindImageMemory(dv, sc->history, sc->history_mem, 0));

    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = sc->format;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vci.image = sc->history;
    CHECK(d->CreateImageView(dv, &vci, nullptr, &sc->history_view));

    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    CHECK(d->CreateSampler(dv, &sci, nullptr, &sc->sampler));

    VkDescriptorSetLayoutBinding b{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                                   VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo dlci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dlci.bindingCount = 1;
    dlci.pBindings = &b;
    CHECK(d->CreateDescriptorSetLayout(dv, &dlci, nullptr, &sc->set_layout));
    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
    VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpci.maxSets = 1;
    dpci.poolSizeCount = 1;
    dpci.pPoolSizes = &ps;
    CHECK(d->CreateDescriptorPool(dv, &dpci, nullptr, &sc->pool));
    VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dsai.descriptorPool = sc->pool;
    dsai.descriptorSetCount = 1;
    dsai.pSetLayouts = &sc->set_layout;
    CHECK(d->AllocateDescriptorSets(dv, &dsai, &sc->set));
    VkDescriptorImageInfo dii{sc->sampler, sc->history_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = sc->set;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.pImageInfo = &dii;
    d->UpdateDescriptorSets(dv, 1, &w, 0, nullptr);

    VkPushConstantRange pcr{VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32};
    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &sc->set_layout;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges = &pcr;
    CHECK(d->CreatePipelineLayout(dv, &plci, nullptr, &sc->layout));

    VkAttachmentDescription att{};
    att.format = sc->format;
    att.samples = VK_SAMPLE_COUNT_1_BIT;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    att.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &ref;
    // Wait for the acquire semaphore (waited at COLOR_ATTACHMENT_OUTPUT)
    // before the layout transition.
    VkSubpassDependency dep{};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    VkRenderPassCreateInfo rpci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rpci.attachmentCount = 1;
    rpci.pAttachments = &att;
    rpci.subpassCount = 1;
    rpci.pSubpasses = &sub;
    rpci.dependencyCount = 1;
    rpci.pDependencies = &dep;
    CHECK(d->CreateRenderPass(dv, &rpci, nullptr, &sc->pass));

    VkShaderModuleCreateInfo smci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    VkShaderModule vs, fs;
    smci.codeSize = sizeof fullscreen_vert;
    smci.pCode = fullscreen_vert;
    CHECK(d->CreateShaderModule(dv, &smci, nullptr, &vs));
    smci.codeSize = sizeof probe_frag;
    smci.pCode = probe_frag;
    CHECK(d->CreateShaderModule(dv, &smci, nullptr, &fs));
    VkPipelineShaderStageCreateInfo stages[2] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main", nullptr},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main", nullptr},
    };
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState cba{};
    cba.colorWriteMask = 0xf;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &cba;
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    ds.dynamicStateCount = 2;
    ds.pDynamicStates = dyn;
    VkGraphicsPipelineCreateInfo gpci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gpci.stageCount = 2;
    gpci.pStages = stages;
    gpci.pVertexInputState = &vi;
    gpci.pInputAssemblyState = &ia;
    gpci.pViewportState = &vp;
    gpci.pRasterizationState = &rs;
    gpci.pMultisampleState = &ms;
    gpci.pColorBlendState = &cb;
    gpci.pDynamicState = &ds;
    gpci.layout = sc->layout;
    gpci.renderPass = sc->pass;
    VkResult pr = d->CreateGraphicsPipelines(dv, VK_NULL_HANDLE, 1, &gpci, nullptr, &sc->pipeline);
    d->DestroyShaderModule(dv, vs, nullptr);
    d->DestroyShaderModule(dv, fs, nullptr);
    CHECK(pr);

    sc->views.assign(sc->images.size(), VK_NULL_HANDLE);
    sc->framebuffers.assign(sc->images.size(), VK_NULL_HANDLE);
    for (size_t i = 0; i < sc->images.size(); i++) {
        vci.image = sc->images[i];
        CHECK(d->CreateImageView(dv, &vci, nullptr, &sc->views[i]));
        VkFramebufferCreateInfo fci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fci.renderPass = sc->pass;
        fci.attachmentCount = 1;
        fci.pAttachments = &sc->views[i];
        fci.width = sc->extent.width;
        fci.height = sc->extent.height;
        fci.layers = 1;
        CHECK(d->CreateFramebuffer(dv, &fci, nullptr, &sc->framebuffers[i]));
    }

    VkCommandPoolCreateInfo cpci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpci.queueFamilyIndex = family;
    CHECK(d->CreateCommandPool(dv, &cpci, nullptr, &sc->cmd_pool));
    sc->timestamps = d->families[family].timestampValidBits > 0;
    for (auto &s : sc->slots) {
        VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cbai.commandPool = sc->cmd_pool;
        cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cbai.commandBufferCount = kMaxMultiplier;
        CHECK(d->AllocateCommandBuffers(dv, &cbai, s.cmd));
        for (auto c : s.cmd) {
            // Command buffers made by a layer need the loader's dispatch table.
            if (d->set_loader_data)
                d->set_loader_data(dv, c);
            else
                *reinterpret_cast<void **>(c) = key(dv);
        }
        VkSemaphoreCreateInfo semci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        CHECK(d->CreateSemaphore(dv, &semci, nullptr, &s.copy_done));
        for (int k = 0; k < kMaxMultiplier; k++) {
            CHECK(d->CreateSemaphore(dv, &semci, nullptr, &s.acquired[k]));
            CHECK(d->CreateSemaphore(dv, &semci, nullptr, &s.drawn[k]));
        }
        VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        CHECK(d->CreateFence(dv, &fci, nullptr, &s.fence));
        if (sc->timestamps) {
            VkQueryPoolCreateInfo qci{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
            qci.queryType = VK_QUERY_TYPE_TIMESTAMP;
            qci.queryCount = 2 * kMaxMultiplier;
            CHECK(d->CreateQueryPool(dv, &qci, nullptr, &s.queries));
        }
    }
    sc->ready = true;
    logf("swapchain %ux%u format %d, %zu images, present mode %d, timestamps %d",
         sc->extent.width, sc->extent.height, (int)sc->format, sc->images.size(), (int)sc->mode,
         (int)sc->timestamps);
    return true;
}

VKAPI_ATTR VkResult VKAPI_CALL CreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR *ci,
                                                  const VkAllocationCallbacks *alloc, VkSwapchainKHR *out)
{
    DeviceData *dev = device_of(key(device));
    VkSwapchainCreateInfoKHR info = *ci;
    if (g_cfg.multiplier > 1) {
        VkSurfaceCapabilitiesKHR caps{};
        dev->inst->GetPhysicalDeviceSurfaceCapabilitiesKHR(dev->physical, ci->surface, &caps);
        // Room for the extra frames this layer keeps in flight.
        info.minImageCount += (uint32_t)g_cfg.multiplier - 1;
        if (caps.maxImageCount && info.minImageCount > caps.maxImageCount)
            info.minImageCount = caps.maxImageCount;
        info.imageUsage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        if (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT)
            info.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        if (g_cfg.force_fifo)
            info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    }
    VkResult r = dev->CreateSwapchainKHR(device, &info, alloc, out);
    if (r != VK_SUCCESS || g_cfg.multiplier <= 1)
        return r;

    auto sc = std::make_unique<SwapchainData>();
    sc->dev = dev;
    sc->swapchain = *out;
    sc->format = info.imageFormat;
    sc->extent = info.imageExtent;
    sc->mode = info.presentMode;
    if (!(info.imageUsage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) || info.imageArrayLayers != 1) {
        logf("swapchain can't be copied from, passing it through");
        sc->broken = true;
    }
    uint32_t n = 0;
    dev->GetSwapchainImagesKHR(device, *out, &n, nullptr);
    sc->images.resize(n);
    dev->GetSwapchainImagesKHR(device, *out, &n, sc->images.data());
    std::lock_guard<std::mutex> lock(g_mutex);
    g_swapchains[*out] = std::move(sc);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL DestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain,
                                               const VkAllocationCallbacks *alloc)
{
    DeviceData *dev = device_of(key(device));
    std::unique_ptr<SwapchainData> sc;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_swapchains.find(swapchain);
        if (it != g_swapchains.end()) {
            sc = std::move(it->second);
            g_swapchains.erase(it);
        }
    }
    if (sc)
        destroy_resources(sc.get());
    dev->DestroySwapchainKHR(device, swapchain, alloc);
}

// ---------------------------------------------------------------- present

void image_barrier(DeviceData *d, VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to,
                   VkAccessFlags src_access, VkAccessFlags dst_access, VkPipelineStageFlags src_stage,
                   VkPipelineStageFlags dst_stage)
{
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = src_access;
    b.dstAccessMask = dst_access;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    d->CmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &b);
}

void collect_timestamps(SwapchainData *sc, Slot &s)
{
    if (!sc->timestamps || !s.used)
        return;
    DeviceData *d = sc->dev;
    uint64_t v[2 * kMaxMultiplier][2];
    int n = 2 * (1 + s.generated);
    VkResult r = d->GetQueryPoolResults(d->device, s.queries, 0, n, sizeof v, v, sizeof v[0],
                                        VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
    if (r != VK_SUCCESS && r != VK_NOT_READY)
        return;
    double scale = d->timestamp_period * 1e-6;
    for (int k = 0; k <= s.generated; k++) {
        if (!v[2 * k][1] || !v[2 * k + 1][1])
            continue;
        double ms = (double)(v[2 * k + 1][0] - v[2 * k][0]) * scale;
        if (k == 0) {
            sc->stats.copy_ms += ms;
            sc->stats.copy_n++;
        } else {
            sc->stats.gen_ms += ms;
            sc->stats.gen_n++;
            sc->stats.gen_ms_max = std::max(sc->stats.gen_ms_max, ms);
        }
    }
}

void report(SwapchainData *sc, double t)
{
    Stats &st = sc->stats;
    double dt = t - st.window_start;
    if (dt < g_cfg.log_every)
        return;
    logf("real %.1f fps (worst gap %.1f ms), shown %.1f fps, missed %d | GPU copy %.2f ms, "
         "generated frame %.2f ms (max %.2f) | CPU in extra presents %.2f ms (max %.2f)",
         st.real / dt, st.interval_max * 1e3, st.shown / dt, st.missed,
         st.copy_n ? st.copy_ms / st.copy_n : 0.0, st.gen_n ? st.gen_ms / st.gen_n : 0.0,
         st.gen_ms_max, st.real ? st.extra_cpu_ms / st.real : 0.0, st.extra_cpu_ms_max);
    double last = st.last_real;
    st = Stats{};
    st.window_start = t;
    st.last_real = last;
}

VkResult record_and_submit(SwapchainData *sc, VkQueue queue, Slot &s, int k, uint32_t image,
                           VkSemaphore wait, VkSemaphore signal)
{
    DeviceData *d = sc->dev;
    VkCommandBuffer cmd = s.cmd[k];
    d->ResetCommandBuffer(cmd, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    d->BeginCommandBuffer(cmd, &bi);
    if (sc->timestamps)
        d->CmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, s.queries, 2 * k);

    VkRenderPassBeginInfo rbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rbi.renderPass = sc->pass;
    rbi.framebuffer = sc->framebuffers[image];
    rbi.renderArea = {{0, 0}, sc->extent};
    d->CmdBeginRenderPass(cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport vp{0, 0, (float)sc->extent.width, (float)sc->extent.height, 0, 1};
    VkRect2D sci{{0, 0}, sc->extent};
    d->CmdSetViewport(cmd, 0, 1, &vp);
    d->CmdSetScissor(cmd, 0, 1, &sci);
    d->CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, sc->pipeline);
    d->CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, sc->layout, 0, 1, &sc->set, 0, nullptr);
    struct {
        float tint[4];
        float shift[2];
        int32_t taps;
        int32_t pad;
    } pc = {{1.0f, 0.0f, 1.0f, g_cfg.tint}, {g_cfg.shift * k, 0.0f}, g_cfg.taps, 0};
    d->CmdPushConstants(cmd, sc->layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, &pc);
    d->CmdDraw(cmd, 3, 1, 0, 0);
    d->CmdEndRenderPass(cmd);

    if (sc->timestamps)
        d->CmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, s.queries, 2 * k + 1);
    d->EndCommandBuffer(cmd);

    VkPipelineStageFlags stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &wait;
    si.pWaitDstStageMask = &stage;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &signal;
    return d->QueueSubmit(queue, 1, &si, VK_NULL_HANDLE);
}

VKAPI_ATTR VkResult VKAPI_CALL QueuePresentKHR(VkQueue queue, const VkPresentInfoKHR *pi)
{
    QueueData q{};
    SwapchainData *sc = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto qi = g_queues.find(queue);
        if (qi != g_queues.end())
            q = qi->second;
        if (pi->swapchainCount == 1) {
            auto it = g_swapchains.find(pi->pSwapchains[0]);
            if (it != g_swapchains.end())
                sc = it->second.get();
        }
    }
    DeviceData *d = q.dev ? q.dev : device_of(key(queue));
    if (!sc || sc->broken || !q.dev)
        return d->QueuePresentKHR(queue, pi);
    if (!sc->ready) {
        if (!(d->families[q.family].queueFlags & VK_QUEUE_GRAPHICS_BIT) || !create_resources(sc, q.family)) {
            logf("can't draw on the present queue, passing frames through");
            destroy_resources(sc);
            sc->broken = true;
            return d->QueuePresentKHR(queue, pi);
        }
    }
    if (q.family != sc->family)
        return d->QueuePresentKHR(queue, pi);

    double t0 = now_s();
    Stats &st = sc->stats;
    if (st.window_start == 0)
        st.window_start = t0;
    if (st.last_real > 0)
        st.interval_max = std::max(st.interval_max, t0 - st.last_real);
    st.last_real = t0;
    st.real++;

    Slot &s = sc->slots[sc->next_slot];
    sc->next_slot = (sc->next_slot + 1) % kRing;
    d->WaitForFences(d->device, 1, &s.fence, VK_TRUE, UINT64_MAX);
    collect_timestamps(sc, s);
    d->ResetFences(d->device, 1, &s.fence);
    s.used = true;
    s.generated = 0;

    // 1. Copy the game's image into the history image, after the game's
    //    rendering (the semaphores it gave to present).
    uint32_t image = pi->pImageIndices[0];
    VkCommandBuffer cmd = s.cmd[0];
    d->ResetCommandBuffer(cmd, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    d->BeginCommandBuffer(cmd, &bi);
    if (sc->timestamps) {
        d->CmdResetQueryPool(cmd, s.queries, 0, 2 * kMaxMultiplier);
        d->CmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, s.queries, 0);
    }
    image_barrier(d, cmd, sc->images[image], VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                  0, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    image_barrier(d, cmd, sc->history, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                  VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkImageCopy region{};
    region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.dstSubresource = region.srcSubresource;
    region.extent = {sc->extent.width, sc->extent.height, 1};
    d->CmdCopyImage(cmd, sc->images[image], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, sc->history,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    image_barrier(d, cmd, sc->images[image], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                  VK_ACCESS_TRANSFER_READ_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT,
                  VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    image_barrier(d, cmd, sc->history, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                  VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    if (sc->timestamps)
        d->CmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, s.queries, 1);
    d->EndCommandBuffer(cmd);

    std::vector<VkPipelineStageFlags> stages(pi->waitSemaphoreCount, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount = pi->waitSemaphoreCount;
    si.pWaitSemaphores = pi->pWaitSemaphores;
    si.pWaitDstStageMask = stages.data();
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &s.copy_done;
    VkResult r = d->QueueSubmit(queue, 1, &si, VK_NULL_HANDLE);
    if (r != VK_SUCCESS) {
        d->QueueSubmit(queue, 0, nullptr, s.fence);
        return r;
    }

    // 2. Present the real frame, keeping whatever the game chained to it.
    VkPresentInfoKHR real = *pi;
    real.waitSemaphoreCount = 1;
    real.pWaitSemaphores = &s.copy_done;
    VkResult result = d->QueuePresentKHR(queue, &real);
    st.shown += result >= 0;

    // 3. Draw and present the generated frames.
    double t1 = now_s();
    if (result >= 0) {
        for (int k = 1; k < g_cfg.multiplier; k++) {
            uint32_t idx;
            VkResult ar = d->AcquireNextImageKHR(d->device, sc->swapchain, 100000000ull, s.acquired[k],
                                                 VK_NULL_HANDLE, &idx);
            if (ar != VK_SUCCESS && ar != VK_SUBOPTIMAL_KHR) {
                st.missed++;
                if (ar == VK_ERROR_OUT_OF_DATE_KHR || ar == VK_ERROR_SURFACE_LOST_KHR)
                    result = ar;
                break;
            }
            if (record_and_submit(sc, queue, s, k, idx, s.acquired[k], s.drawn[k]) != VK_SUCCESS) {
                st.missed++;
                break;
            }
            s.generated = k;
            VkPresentInfoKHR extra{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
            extra.waitSemaphoreCount = 1;
            extra.pWaitSemaphores = &s.drawn[k];
            extra.swapchainCount = 1;
            extra.pSwapchains = &sc->swapchain;
            extra.pImageIndices = &idx;
            VkResult er = d->QueuePresentKHR(queue, &extra);
            if (er < 0) {
                st.missed++;
                result = er;
                break;
            }
            st.shown++;
            if (er == VK_SUBOPTIMAL_KHR && result == VK_SUCCESS)
                result = er;
        }
    }
    d->QueueSubmit(queue, 0, nullptr, s.fence);

    double t2 = now_s();
    st.extra_cpu_ms += (t2 - t1) * 1e3;
    st.extra_cpu_ms_max = std::max(st.extra_cpu_ms_max, (t2 - t1) * 1e3);
    report(sc, t2);
    return result;
}

// ---------------------------------------------------------------- proc addr

PFN_vkVoidFunction hooked(const char *name)
{
#define HOOK(fn) if (!strcmp(name, "vk" #fn)) return (PFN_vkVoidFunction)fn;
    HOOK(GetDeviceQueue)
    HOOK(GetDeviceQueue2)
    HOOK(CreateSwapchainKHR)
    HOOK(DestroySwapchainKHR)
    HOOK(QueuePresentKHR)
    HOOK(DestroyDevice)
#undef HOOK
    return nullptr;
}

} // namespace

FW_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL framewarp_GetDeviceProcAddr(VkDevice device, const char *name);

FW_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL framewarp_GetInstanceProcAddr(VkInstance instance, const char *name)
{
    if (!strcmp(name, "vkGetInstanceProcAddr"))
        return (PFN_vkVoidFunction)framewarp_GetInstanceProcAddr;
    if (!strcmp(name, "vkGetDeviceProcAddr"))
        return (PFN_vkVoidFunction)framewarp_GetDeviceProcAddr;
    if (!strcmp(name, "vkCreateInstance"))
        return (PFN_vkVoidFunction)CreateInstance;
    if (!strcmp(name, "vkDestroyInstance"))
        return (PFN_vkVoidFunction)DestroyInstance;
    if (!strcmp(name, "vkCreateDevice"))
        return (PFN_vkVoidFunction)CreateDevice;
    if (instance)
        if (auto fn = hooked(name))
            return fn;
    InstanceData *inst = instance ? instance_of(key(instance)) : nullptr;
    return inst ? inst->gipa(instance, name) : nullptr;
}

FW_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL framewarp_GetDeviceProcAddr(VkDevice device, const char *name)
{
    if (!strcmp(name, "vkGetDeviceProcAddr"))
        return (PFN_vkVoidFunction)framewarp_GetDeviceProcAddr;
    DeviceData *dev = device_of(key(device));
    if (!dev)
        return nullptr;
    if (auto fn = hooked(name)) {
        // Only hook what the device really has.
        return dev->gdpa(device, name) ? fn : nullptr;
    }
    return dev->gdpa(device, name);
}
