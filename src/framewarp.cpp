// framewarp: Vulkan layer that shows extra frames between the frames a game
// renders, made by pushing the last real frame forward along its motion
// (extrapolation), so nothing waits for a future frame.
//
// How it works: on every vkQueuePresentKHR of the game the layer copies the
// game's image into its own history image, estimates the motion between it
// and the previous real frame (block matching on a 1/4 size luma copy),
// presents the game's image, then acquires (multiplier - 1) more images from
// the same swapchain, draws the warped history image into each and presents
// them. With FIFO each present takes
// one vblank, so on a 60 Hz panel multiplier 3 shows 60 frames per second
// out of 20 real ones. The generated frames are queued right behind the real
// frame, so they never wait behind the next frame's GPU work.

#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>

#include <algorithm>
#include <cmath>
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

#include "filter_comp.h"
#include "fullscreen_vert.h"
#include "global_comp.h"
#include "luma_comp.h"
#include "motion_comp.h"
#include "pyramid_comp.h"
#include "refine_comp.h"
#include "warp_frag.h"

#define FW_EXPORT extern "C" __attribute__((visibility("default")))

namespace {

constexpr int kMaxMultiplier = 4;
constexpr int kRing = 8;

// ---------------------------------------------------------------- config

struct Config {
    int multiplier = 2;    // with auto: the most frames shown per real frame
    bool auto_mult = false;
    double refresh = 1.0 / 60.0;
    bool force_fifo = true;
    int mode = 1; // 0 = copy, 1 = warp, 2 = warp + motion overlay, 3 = camera, 4 = camera + overlay
    float zero_bias = 0.05f;
    float tint = 0.0f;
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
        if (const char *m = getenv("FRAMEWARP_MULT"); m && !strcmp(m, "auto")) {
            g_cfg.auto_mult = true;
            g_cfg.multiplier = std::clamp(env_int("FRAMEWARP_MAX_MULT", 3), 1, kMaxMultiplier);
        } else {
            g_cfg.multiplier = std::clamp(env_int("FRAMEWARP_MULT", 2), 1, kMaxMultiplier);
        }
        g_cfg.refresh = 1.0 / std::clamp(env_float("FRAMEWARP_REFRESH", 60.0f), 24.0f, 240.0f);
        g_cfg.force_fifo = env_int("FRAMEWARP_FORCE_FIFO", 1) != 0;
        if (const char *m = getenv("FRAMEWARP_MODE"); m && *m)
            g_cfg.mode = !strcmp(m, "copy") ? 0 : !strcmp(m, "debug") ? 2 : !strcmp(m, "camera") ? 3 : !strcmp(m, "camera-debug") ? 4 : 1;
        g_cfg.zero_bias = std::clamp(env_float("FRAMEWARP_ZERO_BIAS", 0.05f), 0.0f, 0.5f);
        g_cfg.tint = std::clamp(env_float("FRAMEWARP_TINT", 0.0f), 0.0f, 1.0f);
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
    X(CmdDispatch)                                                             \
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
    X(CreateBuffer)                                                            \
    X(DestroyBuffer)                                                           \
    X(GetBufferMemoryRequirements)                                             \
    X(BindBufferMemory)                                                        \
    X(MapMemory)                                                               \
    X(CmdCopyImageToBuffer)                                                    \
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
    X(CreateComputePipelines)                                                  \
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
// cmd[0] copies the real frame, cmd[1..] draw the generated frames and
// cmd[kAnalysis] estimates the motion. Timestamp pair i belongs to cmd[i].
constexpr int kAnalysis = kMaxMultiplier;

struct Slot {
    VkCommandBuffer cmd[kMaxMultiplier + 1] = {};
    VkSemaphore copy_done = VK_NULL_HANDLE;
    VkSemaphore acquired[kMaxMultiplier] = {};
    VkSemaphore drawn[kMaxMultiplier] = {};
    VkFence fence = VK_NULL_HANDLE;
    VkQueryPool queries = VK_NULL_HANDLE;
    VkBuffer camera = VK_NULL_HANDLE;       // camera mode: the fitted model, for the log
    VkDeviceMemory camera_memory = VK_NULL_HANDLE;
    const float *camera_data = nullptr;
    bool camera_valid = false;
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
    double copy_ms = 0, gen_ms = 0, gen_ms_max = 0, motion_ms = 0;
    int copy_n = 0, gen_n = 0, motion_n = 0;
    double extra_cpu_ms = 0, extra_cpu_ms_max = 0;
    int generated = 0;
    // Camera mode, per real frame: pan in pixels, zoom and roll in percent
    // and degrees, share of blocks that agree with the camera and that move.
    double pan = 0, pan_max = 0, zoom = 0, roll = 0, agree = 0, moving = 0;
    int camera_n = 0;
};

// FRAMEWARP_MULT=auto: how many frames to generate per real frame. Every
// half second it compares the game's frame interval with what the current
// count allows: a slower game gets one more generated frame. Since the layer
// itself holds the game at that rate, it can't see when the game could go
// faster, so now and then it tries one generated frame less and keeps it if
// the game keeps up; a failed try waits longer before the next one.
struct AutoMultiplier {
    int generated = 0;
    bool probing = false;
    double window_start = 0, next_probe = 0, backoff = 10.0;
    double sum = 0;
    int frames = 0;

    void update(double t, double interval, int most)
    {
        if (window_start == 0) {
            window_start = t;
            next_probe = t + backoff;
            return;
        }
        sum += interval;
        frames++;
        if (t - window_start < 0.5 || frames < 4)
            return;
        double avg = sum / frames;
        double cap = (generated + 1) * g_cfg.refresh;
        if (probing) {
            probing = false;
            if (avg <= cap * 1.1) {
                backoff = 10.0;
            } else {
                generated++;
                backoff = std::min(backoff * 2.0, 60.0);
            }
            next_probe = t + backoff;
        } else if (avg > cap * 1.15 && generated < most) {
            generated++;
        } else if (generated > 0 && t >= next_probe) {
            generated--;
            probing = true;
        }
        window_start = t;
        sum = 0;
        frames = 0;
    }
};

struct Image {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
};

// Must match Params in shaders/common.glsl.
struct PushConstants {
    float tint[4];
    float phase;
    int32_t mode;
    int32_t has_prev;
    float zero_bias;
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

    // All in VK_IMAGE_LAYOUT_GENERAL once initialized.
    Image history[2];    // real frames, by parity
    Image luma[2];       // their luma, 1/4 size
    Image luma2[2];      // and 1/16 size
    Image motion_raw;    // block motion, one texel per 32x32 frame pixels
    Image motion;        // the same after the median filter
    Image pixel_motion;  // per luma pixel, picked among the block vectors
    Image camera;        // one affine motion for the whole frame, 2x1
    VkExtent2D luma_extent{}, luma2_extent{}, motion_extent{};
    bool initialized = false;
    int cur = 0;         // history slot of the last real frame
    bool has_prev = false;
    VkSampler sampler = VK_NULL_HANDLE;         // linear
    VkSampler sampler_nearest = VK_NULL_HANDLE; // for formats without linear filtering
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkDescriptorSet sets[2] = {};
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkRenderPass pass = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkPipeline luma_pipeline = VK_NULL_HANDLE;
    VkPipeline pyramid_pipeline = VK_NULL_HANDLE;
    VkPipeline motion_pipeline = VK_NULL_HANDLE;
    VkPipeline filter_pipeline = VK_NULL_HANDLE;
    VkPipeline refine_pipeline = VK_NULL_HANDLE;
    VkPipeline global_pipeline = VK_NULL_HANDLE;
    VkCommandPool cmd_pool = VK_NULL_HANDLE;
    Slot slots[kRing];
    int next_slot = 0;
    bool timestamps = false;

    Stats stats;
    AutoMultiplier auto_mult;
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
    logf("device %s, multiplier %s%d, mode %d, zero bias %.2f, force_fifo %d", props.deviceName,
         g_cfg.auto_mult ? "auto up to " : "", g_cfg.multiplier, g_cfg.mode, g_cfg.zero_bias, g_cfg.force_fifo);

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
        if (s.camera) d->DestroyBuffer(dv, s.camera, nullptr);
        if (s.camera_memory) d->FreeMemory(dv, s.camera_memory, nullptr);
        s = Slot{};
    }
    if (sc->cmd_pool) d->DestroyCommandPool(dv, sc->cmd_pool, nullptr);
    for (auto fb : sc->framebuffers) if (fb) d->DestroyFramebuffer(dv, fb, nullptr);
    for (auto v : sc->views) if (v) d->DestroyImageView(dv, v, nullptr);
    for (VkPipeline p : {sc->pipeline, sc->luma_pipeline, sc->pyramid_pipeline, sc->motion_pipeline,
                         sc->filter_pipeline, sc->refine_pipeline, sc->global_pipeline})
        if (p) d->DestroyPipeline(dv, p, nullptr);
    if (sc->pass) d->DestroyRenderPass(dv, sc->pass, nullptr);
    if (sc->layout) d->DestroyPipelineLayout(dv, sc->layout, nullptr);
    if (sc->pool) d->DestroyDescriptorPool(dv, sc->pool, nullptr);
    if (sc->set_layout) d->DestroyDescriptorSetLayout(dv, sc->set_layout, nullptr);
    if (sc->sampler) d->DestroySampler(dv, sc->sampler, nullptr);
    if (sc->sampler_nearest) d->DestroySampler(dv, sc->sampler_nearest, nullptr);
    for (Image *img : {&sc->history[0], &sc->history[1], &sc->luma[0], &sc->luma[1], &sc->luma2[0], &sc->luma2[1],
                        &sc->motion_raw, &sc->motion, &sc->pixel_motion, &sc->camera}) {
        if (img->view) d->DestroyImageView(dv, img->view, nullptr);
        if (img->image) d->DestroyImage(dv, img->image, nullptr);
        if (img->memory) d->FreeMemory(dv, img->memory, nullptr);
        *img = Image{};
    }
    sc->pipeline = sc->luma_pipeline = sc->pyramid_pipeline = sc->motion_pipeline = sc->filter_pipeline =
        sc->refine_pipeline = sc->global_pipeline = VK_NULL_HANDLE;
    sc->pass = VK_NULL_HANDLE;
    sc->layout = VK_NULL_HANDLE;
    sc->pool = VK_NULL_HANDLE;
    sc->set_layout = VK_NULL_HANDLE;
    sc->sampler = sc->sampler_nearest = VK_NULL_HANDLE;
    sc->initialized = sc->has_prev = false;
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

bool create_image(DeviceData *d, VkFormat format, VkExtent2D extent, VkImageUsageFlags usage, Image &img)
{
    VkDevice dv = d->device;
    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = format;
    ici.extent = {extent.width, extent.height, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = usage;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    CHECK(d->CreateImage(dv, &ici, nullptr, &img.image));
    VkMemoryRequirements req;
    d->GetImageMemoryRequirements(dv, img.image, &req);
    int type = find_memory(d, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type < 0)
        type = find_memory(d, req.memoryTypeBits, 0);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = (uint32_t)type;
    CHECK(d->AllocateMemory(dv, &mai, nullptr, &img.memory));
    CHECK(d->BindImageMemory(dv, img.image, img.memory, 0));
    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = format;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vci.image = img.image;
    CHECK(d->CreateImageView(dv, &vci, nullptr, &img.view));
    return true;
}

bool create_shader(DeviceData *d, const uint32_t *code, size_t size, VkShaderModule *out)
{
    VkShaderModuleCreateInfo smci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    smci.codeSize = size;
    smci.pCode = code;
    CHECK(d->CreateShaderModule(d->device, &smci, nullptr, out));
    return true;
}

bool create_compute(SwapchainData *sc, const uint32_t *code, size_t size, VkPipeline *out)
{
    DeviceData *d = sc->dev;
    VkShaderModule cs;
    if (!create_shader(d, code, size, &cs))
        return false;
    VkComputePipelineCreateInfo cpci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    cpci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT,
                  cs, "main", nullptr};
    cpci.layout = sc->layout;
    VkResult r = d->CreateComputePipelines(d->device, VK_NULL_HANDLE, 1, &cpci, nullptr, out);
    d->DestroyShaderModule(d->device, cs, nullptr);
    CHECK(r);
    return true;
}

bool create_resources(SwapchainData *sc, uint32_t family)
{
    DeviceData *d = sc->dev;
    VkDevice dv = d->device;
    sc->family = family;

    sc->luma_extent = {(sc->extent.width + 3) / 4, (sc->extent.height + 3) / 4};
    sc->luma2_extent = {(sc->luma_extent.width + 3) / 4, (sc->luma_extent.height + 3) / 4};
    sc->motion_extent = {(sc->luma_extent.width + 7) / 8, (sc->luma_extent.height + 7) / 8};
    const VkImageUsageFlags storage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    for (int i = 0; i < 2; i++) {
        if (!create_image(d, sc->format, sc->extent, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                          sc->history[i]) ||
            !create_image(d, VK_FORMAT_R32_SFLOAT, sc->luma_extent, storage, sc->luma[i]) ||
            !create_image(d, VK_FORMAT_R32_SFLOAT, sc->luma2_extent, storage, sc->luma2[i]))
            return false;
    }
    if (!create_image(d, VK_FORMAT_R16G16B16A16_SFLOAT, sc->motion_extent, storage, sc->motion_raw) ||
        !create_image(d, VK_FORMAT_R16G16B16A16_SFLOAT, sc->motion_extent, storage, sc->motion) ||
        !create_image(d, VK_FORMAT_R16G16B16A16_SFLOAT, sc->luma_extent, storage, sc->pixel_motion) ||
        !create_image(d, VK_FORMAT_R32G32B32A32_SFLOAT, {2, 1}, storage | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, sc->camera))
        return false;

    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    CHECK(d->CreateSampler(dv, &sci, nullptr, &sc->sampler));
    sci.magFilter = sci.minFilter = VK_FILTER_NEAREST;
    CHECK(d->CreateSampler(dv, &sci, nullptr, &sc->sampler_nearest));

    // The bindings of shaders/common.glsl.
    const VkDescriptorType sampled = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    const VkDescriptorType image = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    const VkDescriptorType types[] = {sampled, sampled, sampled, image, image, image, sampled,
                                      sampled, sampled, image, sampled, image, sampled, image, sampled};
    constexpr uint32_t kBindings = sizeof types / sizeof types[0];
    VkDescriptorSetLayoutBinding b[kBindings];
    for (uint32_t i = 0; i < kBindings; i++)
        b[i] = {i, types[i], 1, VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo dlci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dlci.bindingCount = kBindings;
    dlci.pBindings = b;
    CHECK(d->CreateDescriptorSetLayout(dv, &dlci, nullptr, &sc->set_layout));
    VkDescriptorPoolSize ps[2] = {{sampled, 18}, {image, 12}};
    VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpci.maxSets = 2;
    dpci.poolSizeCount = 2;
    dpci.pPoolSizes = ps;
    CHECK(d->CreateDescriptorPool(dv, &dpci, nullptr, &sc->pool));
    VkDescriptorSetLayout layouts[2] = {sc->set_layout, sc->set_layout};
    VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dsai.descriptorPool = sc->pool;
    dsai.descriptorSetCount = 2;
    dsai.pSetLayouts = layouts;
    CHECK(d->AllocateDescriptorSets(dv, &dsai, sc->sets));
    for (int c = 0; c < 2; c++) {
        const VkImageView views[kBindings] = {sc->history[c].view, sc->luma[c].view, sc->luma[1 - c].view,
                                              sc->luma[c].view, sc->motion_raw.view, sc->motion.view,
                                              sc->motion.view, sc->luma2[c].view, sc->luma2[1 - c].view,
                                              sc->luma2[c].view, sc->history[1 - c].view, sc->pixel_motion.view,
                                              sc->pixel_motion.view, sc->camera.view, sc->camera.view};
        VkDescriptorImageInfo info[kBindings];
        VkWriteDescriptorSet w[kBindings];
        for (uint32_t i = 0; i < kBindings; i++) {
            VkSampler sampler = types[i] != sampled ? VK_NULL_HANDLE : (i == 1 || i == 2 || i == 7 || i == 8 || i == 12 || i == 14)
                                                                                           ? sc->sampler_nearest
                                                                                           : sc->sampler;
            info[i] = {sampler, views[i], VK_IMAGE_LAYOUT_GENERAL};
            w[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            w[i].dstSet = sc->sets[c];
            w[i].dstBinding = i;
            w[i].descriptorCount = 1;
            w[i].descriptorType = types[i];
            w[i].pImageInfo = &info[i];
        }
        d->UpdateDescriptorSets(dv, kBindings, w, 0, nullptr);
    }

    VkPushConstantRange pcr{VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(PushConstants)};
    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &sc->set_layout;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges = &pcr;
    CHECK(d->CreatePipelineLayout(dv, &plci, nullptr, &sc->layout));

    if (!create_compute(sc, luma_comp, sizeof luma_comp, &sc->luma_pipeline) ||
        !create_compute(sc, pyramid_comp, sizeof pyramid_comp, &sc->pyramid_pipeline) ||
        !create_compute(sc, motion_comp, sizeof motion_comp, &sc->motion_pipeline) ||
        !create_compute(sc, filter_comp, sizeof filter_comp, &sc->filter_pipeline) ||
        !create_compute(sc, refine_comp, sizeof refine_comp, &sc->refine_pipeline) ||
        !create_compute(sc, global_comp, sizeof global_comp, &sc->global_pipeline))
        return false;

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

    VkShaderModule vs, fs;
    if (!create_shader(d, fullscreen_vert, sizeof fullscreen_vert, &vs) ||
        !create_shader(d, warp_frag, sizeof warp_frag, &fs))
        return false;
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
        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = sc->format;
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
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
        cbai.commandBufferCount = kMaxMultiplier + 1;
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
            qci.queryCount = 2 * (kMaxMultiplier + 1);
            CHECK(d->CreateQueryPool(dv, &qci, nullptr, &s.queries));
        }
        if (g_cfg.mode >= 3) {
            VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
            bci.size = 2 * 4 * sizeof(float);
            bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            CHECK(d->CreateBuffer(dv, &bci, nullptr, &s.camera));
            VkMemoryRequirements req;
            d->GetBufferMemoryRequirements(dv, s.camera, &req);
            int type = find_memory(d, req.memoryTypeBits,
                                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            if (type < 0)
                return false;
            VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            mai.allocationSize = req.size;
            mai.memoryTypeIndex = (uint32_t)type;
            CHECK(d->AllocateMemory(dv, &mai, nullptr, &s.camera_memory));
            CHECK(d->BindBufferMemory(dv, s.camera, s.camera_memory, 0));
            void *data;
            CHECK(d->MapMemory(dv, s.camera_memory, 0, VK_WHOLE_SIZE, 0, &data));
            s.camera_data = static_cast<const float *>(data);
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
    uint64_t v[2 * (kMaxMultiplier + 1)][2] = {};
    VkResult r = d->GetQueryPoolResults(d->device, s.queries, 0, 2 * (kMaxMultiplier + 1), sizeof v, v, sizeof v[0],
                                        VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
    if (r != VK_SUCCESS && r != VK_NOT_READY)
        return;
    double scale = d->timestamp_period * 1e-6;
    if (v[2 * kAnalysis][1] && v[2 * kAnalysis + 1][1]) {
        sc->stats.motion_ms += (double)(v[2 * kAnalysis + 1][0] - v[2 * kAnalysis][0]) * scale;
        sc->stats.motion_n++;
    }
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

void collect_camera(SwapchainData *sc, Slot &s)
{
    if (!s.camera_valid)
        return;
    s.camera_valid = false;
    const float *v = s.camera_data; // a11 a12 bx agree, a21 a22 by moving
    double h = sc->extent.height;
    Stats &st = sc->stats;
    double pan = std::hypot(v[2], v[6]);
    st.pan += pan;
    st.pan_max = std::max(st.pan_max, pan);
    st.zoom += (v[0] + v[5]) * 0.5 / h * 100.0;
    st.roll += (v[4] - v[1]) * 0.5 / h * 57.29578;
    st.agree += v[3];
    st.moving += v[7];
    st.camera_n++;
}

void report(SwapchainData *sc, double t)
{
    Stats &st = sc->stats;
    double dt = t - st.window_start;
    if (dt < g_cfg.log_every)
        return;
    logf("real %.1f fps (worst gap %.1f ms), shown %.1f fps, missed %d | GPU copy %.2f ms, motion %.2f ms, "
         "generated frame %.2f ms (max %.2f) | CPU in extra presents %.2f ms (max %.2f)%s",
         st.real / dt, st.interval_max * 1e3, st.shown / dt, st.missed,
         st.copy_n ? st.copy_ms / st.copy_n : 0.0, st.motion_n ? st.motion_ms / st.motion_n : 0.0,
         st.gen_n ? st.gen_ms / st.gen_n : 0.0,
         st.gen_ms_max, st.real ? st.extra_cpu_ms / st.real : 0.0, st.extra_cpu_ms_max,
         g_cfg.auto_mult ? (" | generating " + std::to_string(sc->auto_mult.generated) + " per frame").c_str() : "");
    if (st.camera_n) {
        double n = st.camera_n;
        logf("  camera per real frame: pan %.1f px (max %.1f), zoom %+.2f%%, roll %+.2f deg | blocks that move "
             "%.0f%%, that agree with the camera %.0f%%",
             st.pan / n, st.pan_max, st.zoom / n, st.roll / n, st.moving / n * 100.0, st.agree / n * 100.0);
    }
    double last = st.last_real;
    st = Stats{};
    st.window_start = t;
    st.last_real = last;
}

PushConstants push_constants(SwapchainData *sc, float phase)
{
    return PushConstants{{1.0f, 0.0f, 1.0f, g_cfg.tint}, phase, g_cfg.mode, sc->has_prev ? 1 : 0, g_cfg.zero_bias};
}

void memory_barrier(DeviceData *d, VkCommandBuffer cmd, VkPipelineStageFlags src, VkAccessFlags src_access,
                    VkPipelineStageFlags dst, VkAccessFlags dst_access)
{
    VkMemoryBarrier b{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    b.srcAccessMask = src_access;
    b.dstAccessMask = dst_access;
    d->CmdPipelineBarrier(cmd, src, dst, 0, 1, &b, 0, nullptr, 0, nullptr);
}

// Luma pyramid of the new real frame, block motion against the previous
// one, median filter, then motion per luma pixel. Runs right after the real frame is presented.
VkResult submit_analysis(SwapchainData *sc, VkQueue queue, Slot &s)
{
    DeviceData *d = sc->dev;
    VkCommandBuffer cmd = s.cmd[kAnalysis];
    d->ResetCommandBuffer(cmd, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    d->BeginCommandBuffer(cmd, &bi);
    if (sc->timestamps)
        d->CmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, s.queries, 2 * kAnalysis);
    d->CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, sc->layout, 0, 1, &sc->sets[sc->cur], 0, nullptr);
    PushConstants pc = push_constants(sc, 0.0f);
    d->CmdPushConstants(cmd, sc->layout, VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc,
                        &pc);
    const VkPipelineStageFlags shaders = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    const VkAccessFlags rw = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;

    d->CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, sc->luma_pipeline);
    d->CmdDispatch(cmd, (sc->luma_extent.width + 7) / 8, (sc->luma_extent.height + 7) / 8, 1);
    memory_barrier(d, cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, shaders, rw);
    d->CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, sc->pyramid_pipeline);
    d->CmdDispatch(cmd, (sc->luma2_extent.width + 7) / 8, (sc->luma2_extent.height + 7) / 8, 1);
    memory_barrier(d, cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, shaders, rw);
    d->CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, sc->motion_pipeline);
    d->CmdDispatch(cmd, sc->motion_extent.width, sc->motion_extent.height, 1);
    memory_barrier(d, cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, shaders, rw);
    d->CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, sc->filter_pipeline);
    d->CmdDispatch(cmd, (sc->motion_extent.width + 7) / 8, (sc->motion_extent.height + 7) / 8, 1);
    memory_barrier(d, cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, shaders, rw);
    if (g_cfg.mode >= 3) {
        d->CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, sc->global_pipeline);
        d->CmdDispatch(cmd, 1, 1, 1);
        memory_barrier(d, cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                       shaders | VK_PIPELINE_STAGE_TRANSFER_BIT, rw | VK_ACCESS_TRANSFER_READ_BIT);
        if (sc->has_prev && s.camera) {
            VkBufferImageCopy region{};
            region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.imageExtent = {2, 1, 1};
            d->CmdCopyImageToBuffer(cmd, sc->camera.image, VK_IMAGE_LAYOUT_GENERAL, s.camera, 1, &region);
            memory_barrier(d, cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                           VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
            s.camera_valid = true;
        }
    }
    d->CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, sc->refine_pipeline);
    d->CmdDispatch(cmd, (sc->luma_extent.width + 7) / 8, (sc->luma_extent.height + 7) / 8, 1);
    memory_barrier(d, cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, shaders, rw);

    if (sc->timestamps)
        d->CmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, s.queries, 2 * kAnalysis + 1);
    d->EndCommandBuffer(cmd);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    return d->QueueSubmit(queue, 1, &si, VK_NULL_HANDLE);
}

VkResult record_and_submit(SwapchainData *sc, VkQueue queue, Slot &s, int k, int shown_per_real, uint32_t image,
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
    d->CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, sc->layout, 0, 1, &sc->sets[sc->cur], 0,
                             nullptr);
    PushConstants pc = push_constants(sc, (float)k / (float)shown_per_real);
    d->CmdPushConstants(cmd, sc->layout, VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc,
                        &pc);
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
    if (st.last_real > 0) {
        st.interval_max = std::max(st.interval_max, t0 - st.last_real);
        if (g_cfg.auto_mult)
            sc->auto_mult.update(t0, t0 - st.last_real, g_cfg.multiplier - 1);
    }
    const int shown_per_real = g_cfg.auto_mult ? sc->auto_mult.generated + 1 : g_cfg.multiplier;
    st.last_real = t0;
    st.real++;

    Slot &s = sc->slots[sc->next_slot];
    sc->next_slot = (sc->next_slot + 1) % kRing;
    d->WaitForFences(d->device, 1, &s.fence, VK_TRUE, UINT64_MAX);
    collect_timestamps(sc, s);
    collect_camera(sc, s);
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
        d->CmdResetQueryPool(cmd, s.queries, 0, 2 * (kMaxMultiplier + 1));
        d->CmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, s.queries, 0);
    }
    const VkPipelineStageFlags shaders = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    if (!sc->initialized) {
        for (Image *img : {&sc->history[0], &sc->history[1], &sc->luma[0], &sc->luma[1], &sc->luma2[0], &sc->luma2[1],
                        &sc->motion_raw, &sc->motion, &sc->pixel_motion, &sc->camera})
            image_barrier(d, cmd, img->image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0,
                          VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
                          VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, shaders | VK_PIPELINE_STAGE_TRANSFER_BIT);
        sc->initialized = true;
        sc->has_prev = false;
    }
    // The new real frame goes into the other history slot; the frames drawn
    // from it two presents ago must be done reading it.
    sc->cur = 1 - sc->cur;
    Image &history = sc->history[sc->cur];
    image_barrier(d, cmd, sc->images[image], VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                  0, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    memory_barrier(d, cmd, shaders, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT | shaders,
                   VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    VkImageCopy region{};
    region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.dstSubresource = region.srcSubresource;
    region.extent = {sc->extent.width, sc->extent.height, 1};
    d->CmdCopyImage(cmd, sc->images[image], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, history.image,
                    VK_IMAGE_LAYOUT_GENERAL, 1, &region);
    image_barrier(d, cmd, sc->images[image], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                  VK_ACCESS_TRANSFER_READ_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT,
                  VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    memory_barrier(d, cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, shaders,
                   VK_ACCESS_SHADER_READ_BIT);
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

    // 3. Estimate the motion, then draw and present the generated frames.
    double t1 = now_s();
    if (result >= 0 && g_cfg.mode != 0 && submit_analysis(sc, queue, s) != VK_SUCCESS)
        result = VK_ERROR_DEVICE_LOST;
    sc->has_prev = result >= 0;
    if (result >= 0) {
        for (int k = 1; k < shown_per_real; k++) {
            uint32_t idx;
            VkResult ar = d->AcquireNextImageKHR(d->device, sc->swapchain, 100000000ull, s.acquired[k],
                                                 VK_NULL_HANDLE, &idx);
            if (ar != VK_SUCCESS && ar != VK_SUBOPTIMAL_KHR) {
                st.missed++;
                if (ar == VK_ERROR_OUT_OF_DATE_KHR || ar == VK_ERROR_SURFACE_LOST_KHR)
                    result = ar;
                break;
            }
            if (record_and_submit(sc, queue, s, k, shown_per_real, idx, s.acquired[k], s.drawn[k]) != VK_SUCCESS) {
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
