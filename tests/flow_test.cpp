// Runs the win-fg flow and synthesis (third_party/win-fg) on two frames
// where a pattern moved by a known amount, under a still strip like a HUD,
// and checks the frames they generate: halfway between the two
// (interpolation) and half a frame past the second (extrapolation) must be
// much closer to the pattern moved by that amount than a plain repeat of the
// second frame, and the strip must stay as it is. (Before framewarp bound
// win-fg's two flow images the right way round, both came out worse than a
// repeat.) Uses the first Vulkan
// device (Mesa's software driver in CI).

#include <vulkan/vulkan.h>

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "framegen.hpp"

void framewarp_winfg_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "win-fg: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
}

namespace {

#define VK(expr)                                                               \
    do {                                                                       \
        VkResult _r = (expr);                                                  \
        if (_r != VK_SUCCESS) {                                                \
            fprintf(stderr, "%s failed: %d\n", #expr, (int)_r);                \
            exit(2);                                                           \
        }                                                                      \
    } while (0)

constexpr uint32_t kW = 640, kH = 384;
constexpr uint32_t kStrip = 48;     // still rows at the bottom
constexpr int kMargin = 40;         // edges left out of the comparison

VkDevice device;
VkQueue queue;
VkCommandPool pool;
VkPhysicalDeviceMemoryProperties mem_props;

struct Image {
    VkImage image;
    VkDeviceMemory memory;
    VkImageView view;
};

uint32_t memory_type(uint32_t bits, VkMemoryPropertyFlags want)
{
    for (uint32_t i = 0; i < mem_props.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mem_props.memoryTypes[i].propertyFlags & want) == want)
            return i;
    fprintf(stderr, "no memory type\n");
    exit(2);
}

Image make_image(VkImageUsageFlags usage)
{
    Image img{};
    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = VK_FORMAT_R8G8B8A8_UNORM;
    ici.extent = {kW, kH, 1};
    ici.mipLevels = ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.usage = usage;
    VK(vkCreateImage(device, &ici, nullptr, &img.image));
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(device, img.image, &req);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, req.size,
                             memory_type(req.memoryTypeBits, 0)};
    VK(vkAllocateMemory(device, &mai, nullptr, &img.memory));
    VK(vkBindImageMemory(device, img.image, img.memory, 0));
    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = img.image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = VK_FORMAT_R8G8B8A8_UNORM;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VK(vkCreateImageView(device, &vci, nullptr, &img.view));
    return img;
}

struct Buffer {
    VkBuffer buffer;
    VkDeviceMemory memory;
    uint8_t *data;
};

Buffer make_buffer(VkDeviceSize size)
{
    Buffer b{};
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = size;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VK(vkCreateBuffer(device, &bci, nullptr, &b.buffer));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device, b.buffer, &req);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, req.size,
                             memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                                 VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
    VK(vkAllocateMemory(device, &mai, nullptr, &b.memory));
    VK(vkBindBufferMemory(device, b.buffer, b.memory, 0));
    VK(vkMapMemory(device, b.memory, 0, size, 0, (void **)&b.data));
    return b;
}

VkCommandBuffer begin()
{
    VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr, pool,
                                     VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    VkCommandBuffer cmd;
    VK(vkAllocateCommandBuffers(device, &cbai, &cmd));
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    VK(vkBeginCommandBuffer(cmd, &bi));
    return cmd;
}

void submit(VkCommandBuffer cmd)
{
    VK(vkEndCommandBuffer(cmd));
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    VK(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE));
    VK(vkQueueWaitIdle(queue));
    vkFreeCommandBuffers(device, pool, 1, &cmd);
}

void barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to)
{
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr,
                         0, nullptr, 1, &b);
}

// A smooth pattern with detail at several scales: value noise on 24, 12 and
// 6 pixel lattices, bilinear, so any shift is well defined.
float lattice(int x, int y, int seed)
{
    uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u + (uint32_t)seed * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return (float)((h ^ (h >> 16)) & 0xffff) / 65535.0f;
}

float noise(float x, float y, float cell, int seed)
{
    float fx = x / cell, fy = y / cell;
    int ix = (int)std::floor(fx), iy = (int)std::floor(fy);
    float tx = fx - ix, ty = fy - iy;
    float a = lattice(ix, iy, seed), b = lattice(ix + 1, iy, seed);
    float c = lattice(ix, iy + 1, seed), d = lattice(ix + 1, iy + 1, seed);
    return (a + (b - a) * tx) * (1 - ty) + (c + (d - c) * tx) * ty;
}

float pattern(float x, float y, int channel)
{
    return 0.5f * noise(x, y, 24.0f, channel) + 0.3f * noise(x, y, 12.0f, channel + 7) +
           0.2f * noise(x, y, 6.0f, channel + 13);
}

// The pattern moved by (dx, dy), with the still strip at the bottom.
std::vector<uint8_t> frame(float dx, float dy)
{
    std::vector<uint8_t> px(kW * kH * 4);
    for (uint32_t y = 0; y < kH; y++)
        for (uint32_t x = 0; x < kW; x++)
            for (int c = 0; c < 4; c++) {
                float v = c == 3 ? 1.0f
                          : y >= kH - kStrip ? 0.15f + 0.7f * ((x / 16 + c) % 2)
                                             : pattern(x - dx, y - dy, c);
                px[(y * kW + x) * 4 + c] = (uint8_t)std::lround(std::fmin(std::fmax(v, 0.0f), 1.0f) * 255.0f);
            }
    return px;
}

void upload(const Image &img, const std::vector<uint8_t> &px, Buffer &staging)
{
    memcpy(staging.data, px.data(), px.size());
    VkCommandBuffer cmd = begin();
    barrier(cmd, img.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {kW, kH, 1};
    vkCmdCopyBufferToImage(cmd, staging.buffer, img.image, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
    barrier(cmd, img.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
    submit(cmd);
}

std::vector<uint8_t> download(const Image &img, Buffer &staging)
{
    VkCommandBuffer cmd = begin();
    barrier(cmd, img.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {kW, kH, 1};
    vkCmdCopyImageToBuffer(cmd, img.image, VK_IMAGE_LAYOUT_GENERAL, staging.buffer, 1, &region);
    submit(cmd);
    return std::vector<uint8_t>(staging.data, staging.data + kW * kH * 4);
}

// Mean absolute difference of the rgb channels, 0..255, over the moving
// part (rows above the strip) or the strip.
double difference(const std::vector<uint8_t> &a, const std::vector<uint8_t> &b, bool strip)
{
    double sum = 0;
    long n = 0;
    uint32_t y0 = strip ? kH - kStrip + 2 : kMargin, y1 = strip ? kH - 2 : kH - kStrip - kMargin;
    for (uint32_t y = y0; y < y1; y++)
        for (uint32_t x = kMargin; x < kW - kMargin; x++)
            for (int c = 0; c < 3; c++, n++)
                sum += std::abs((int)a[(y * kW + x) * 4 + c] - (int)b[(y * kW + x) * 4 + c]);
    return sum / n;
}

} // namespace

int main()
{
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion = VK_API_VERSION_1_0;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    VkInstance instance;
    VK(vkCreateInstance(&ici, nullptr, &instance));
    uint32_t n = 1;
    VkPhysicalDevice gpu;
    vkEnumeratePhysicalDevices(instance, &n, &gpu);
    if (!n) {
        fprintf(stderr, "no Vulkan device\n");
        return 2;
    }
    vkGetPhysicalDeviceMemoryProperties(gpu, &mem_props);
    uint32_t nf = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(gpu, &nf, nullptr);
    std::vector<VkQueueFamilyProperties> fams(nf);
    vkGetPhysicalDeviceQueueFamilyProperties(gpu, &nf, fams.data());
    uint32_t family = 0;
    while (family < nf && !(fams[family].queueFlags & VK_QUEUE_COMPUTE_BIT))
        family++;
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, nullptr, 0, family, 1, &prio};
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    VK(vkCreateDevice(gpu, &dci, nullptr, &device));
    vkGetDeviceQueue(device, family, 0, &queue);
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.queueFamilyIndex = family;
    VK(vkCreateCommandPool(device, &pci, nullptr, &pool));

    winfg::DeviceDispatch dd;
    winfg::InstanceDispatch id;
#define LOAD(name) dd.name = (PFN_vk##name)vkGetDeviceProcAddr(device, "vk" #name);
    LOAD(DeviceWaitIdle) LOAD(CreateImage) LOAD(DestroyImage) LOAD(CreateImageView) LOAD(DestroyImageView)
    LOAD(AllocateMemory) LOAD(FreeMemory) LOAD(BindImageMemory) LOAD(GetImageMemoryRequirements) LOAD(CreateBuffer)
    LOAD(DestroyBuffer) LOAD(GetBufferMemoryRequirements) LOAD(BindBufferMemory) LOAD(MapMemory) LOAD(UnmapMemory)
    LOAD(CreateSampler) LOAD(DestroySampler) LOAD(CreateShaderModule) LOAD(DestroyShaderModule)
    LOAD(CreateDescriptorSetLayout) LOAD(DestroyDescriptorSetLayout) LOAD(CreatePipelineLayout)
    LOAD(DestroyPipelineLayout) LOAD(CreateComputePipelines) LOAD(DestroyPipeline) LOAD(CreateDescriptorPool)
    LOAD(DestroyDescriptorPool) LOAD(ResetDescriptorPool) LOAD(AllocateDescriptorSets) LOAD(UpdateDescriptorSets)
    LOAD(CmdBindPipeline) LOAD(CmdBindDescriptorSets) LOAD(CmdDispatch) LOAD(CmdPipelineBarrier) LOAD(CmdCopyImage)
    LOAD(CmdClearColorImage) LOAD(CmdPushConstants)
#undef LOAD
    id.GetPhysicalDeviceMemoryProperties = vkGetPhysicalDeviceMemoryProperties;

    const VkImageUsageFlags in_usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    Image prev = make_image(in_usage), curr = make_image(in_usage);
    Image out = make_image(VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    Buffer staging = make_buffer(kW * kH * 4);
    VkCommandBuffer cmd = begin();
    barrier(cmd, out.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    submit(cmd);

    const float dx = 10.0f, dy = 6.0f;
    upload(prev, frame(0, 0), staging);
    upload(curr, frame(dx, dy), staging);
    const std::vector<uint8_t> repeat = frame(dx, dy);

    int failures = 0;
    for (int model : {3, 4}) {
        winfg::FrameGen fg;
        winfg::Config c;
        c.model = model;
        fg.configure(c);
        if (!fg.init(&dd, &id, gpu, device, family, queue) || !fg.onResize({kW, kH}, VK_FORMAT_R8G8B8A8_UNORM)) {
            fprintf(stderr, "engine failed to start\n");
            return 2;
        }
        // Several frames, so the global motion estimate settles as in a game.
        for (int round = 0; round < 6; round++) {
            for (float t : {0.25f, 0.5f, 1.5f}) {
                cmd = begin();
                fg.recordFlow(cmd, prev.view, curr.view, out.view, round % winfg::FrameGen::kGmSlots);
                fg.recordSynth(cmd, t, t > 1.0f ? 1.0f : 0.0f);
                submit(cmd);
                if (round < 5)
                    continue;
                std::vector<uint8_t> got = download(out, staging);
                std::vector<uint8_t> want = frame(dx * t, dy * t);
                double err = difference(got, want, false), base = difference(repeat, want, false);
                double strip = difference(got, repeat, true);
                bool ok = err < 0.5 * base && strip < 1.0;
                failures += !ok;
                printf("model %d, %s at %.1f: off by %.2f (repeating the frame: %.2f), still strip off by %.2f %s\n",
                       model, t > 1.0f ? "extrapolated" : "interpolated", t, err, base, strip, ok ? "ok" : "FAIL");
            }
        }
        // The flow view (framewarp flow-debug): a move right and a bit down
        // reads orange in the moving part, the still strip stays grey.
        cmd = begin();
        fg.recordFlow(cmd, prev.view, curr.view, out.view, 0);
        fg.recordSynth(cmd, 0.0f, 2.0f);
        submit(cmd);
        std::vector<uint8_t> view = download(out, staging);
        double moving[3] = {}, still = 0;
        long nm = 0, ns = 0;
        for (uint32_t y = 140; y < kH - 2; y++)
            for (uint32_t x = kMargin; x < kW - kMargin; x++) {
                const uint8_t *p = &view[(y * kW + x) * 4];
                if (y >= kH - kStrip + 2) {
                    still += std::abs(p[0] - p[1]) + std::abs(p[1] - p[2]);
                    ns++;
                } else if (y < kH - kStrip - kMargin) {
                    for (int c = 0; c < 3; c++)
                        moving[c] += p[c];
                    nm++;
                }
            }
        for (double &m : moving)
            m /= nm;
        still /= ns;
        bool ok = moving[0] > moving[1] + 8 && moving[1] > moving[2] + 8 && still < 1.0;
        failures += !ok;
        printf("model %d, flow view: moving part rgb %.0f %.0f %.0f, still strip colour %.2f %s\n", model,
               moving[0], moving[1], moving[2], still, ok ? "ok" : "FAIL");
        fg.destroy();
    }
    return failures ? 1 : 0;
}
