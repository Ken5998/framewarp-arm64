// Runs the layer's luma and motion shaders on two frames where a noise
// pattern moved by a known amount, and checks the motion they find. Uses the
// first Vulkan device (Mesa's software driver in CI).

#include <vulkan/vulkan.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "filter_comp.h"
#include "global_comp.h"
#include "luma_comp.h"
#include "motion_comp.h"
#include "pyramid_comp.h"
#include "refine_comp.h"

namespace {

#define VK(expr)                                                               \
    do {                                                                       \
        VkResult _r = (expr);                                                  \
        if (_r != VK_SUCCESS) {                                                \
            fprintf(stderr, "%s failed: %d\n", #expr, (int)_r);                \
            exit(2);                                                           \
        }                                                                      \
    } while (0)

constexpr uint32_t kSize = 512;               // frame
constexpr uint32_t kLuma = kSize / 4;         // luma
constexpr uint32_t kLuma2 = kLuma / 4;        // coarse luma
constexpr uint32_t kMotion = kLuma / 8;       // motion blocks

VkInstance instance;
VkPhysicalDevice gpu;
VkDevice device;
VkQueue queue;
uint32_t family;
VkCommandPool pool;
VkPhysicalDeviceMemoryProperties mem_props;

struct Image {
    VkImage image;
    VkDeviceMemory memory;
    VkImageView view;
};

struct Buffer {
    VkBuffer buffer;
    VkDeviceMemory memory;
    void *data;
};

uint32_t memory_type(uint32_t bits, VkMemoryPropertyFlags want)
{
    for (uint32_t i = 0; i < mem_props.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mem_props.memoryTypes[i].propertyFlags & want) == want)
            return i;
    fprintf(stderr, "no memory type\n");
    exit(2);
}

Image make_image(VkFormat format, uint32_t size, VkImageUsageFlags usage, uint32_t width = 0)
{
    Image img{};
    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = format;
    ici.extent = {width ? width : size, size, 1};
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
    vci.format = format;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VK(vkCreateImageView(device, &vci, nullptr, &img.view));
    return img;
}

Buffer make_buffer(VkDeviceSize size, VkBufferUsageFlags usage)
{
    Buffer b{};
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = size;
    bci.usage = usage;
    VK(vkCreateBuffer(device, &bci, nullptr, &b.buffer));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device, b.buffer, &req);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, req.size,
                             memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                                 VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
    VK(vkAllocateMemory(device, &mai, nullptr, &b.memory));
    VK(vkBindBufferMemory(device, b.buffer, b.memory, 0));
    VK(vkMapMemory(device, b.memory, 0, size, 0, &b.data));
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

void end(VkCommandBuffer cmd)
{
    VK(vkEndCommandBuffer(cmd));
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    VK(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE));
    VK(vkQueueWaitIdle(queue));
    vkFreeCommandBuffers(device, pool, 1, &cmd);
}

void barrier(VkCommandBuffer cmd)
{
    VkMemoryBarrier b{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, VK_ACCESS_MEMORY_WRITE_BIT,
                      VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &b, 0,
                         nullptr, 0, nullptr);
}

void to_general(VkCommandBuffer cmd, VkImage image)
{
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &b);
}

// Smooth value noise, so every block has texture to match.
float noise(float x, float y)
{
    auto hash = [](int i, int j) {
        uint32_t h = (uint32_t)i * 374761393u + (uint32_t)j * 668265263u;
        h = (h ^ (h >> 13)) * 1274126177u;
        return (float)((h ^ (h >> 16)) & 0xffff) / 65535.0f;
    };
    float v = 0, amp = 0.5f;
    for (float cell : {16.0f, 7.0f}) {
        float fx = x / cell, fy = y / cell;
        int ix = (int)floorf(fx), iy = (int)floorf(fy);
        float tx = fx - ix, ty = fy - iy;
        float a = hash(ix, iy), b = hash(ix + 1, iy), c = hash(ix, iy + 1), d = hash(ix + 1, iy + 1);
        v += amp * ((a * (1 - tx) + b * tx) * (1 - ty) + (c * (1 - tx) + d * tx) * ty);
        amp *= 0.5f;
    }
    return v;
}

float half_to_float(uint16_t h)
{
    int e = (h >> 10) & 31, m = h & 1023;
    float v = e == 0 ? ldexpf((float)m, -24) : ldexpf((float)(m | 1024), e - 25);
    return (h & 0x8000) ? -v : v;
}

} // namespace

int main()
{
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    VK(vkCreateInstance(&ici, nullptr, &instance));
    uint32_t n = 1;
    VkResult r = vkEnumeratePhysicalDevices(instance, &n, &gpu);
    if ((r != VK_SUCCESS && r != VK_INCOMPLETE) || n == 0) {
        fprintf(stderr, "no Vulkan device\n");
        return 2;
    }
    vkGetPhysicalDeviceMemoryProperties(gpu, &mem_props);
    uint32_t nf = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(gpu, &nf, nullptr);
    std::vector<VkQueueFamilyProperties> fams(nf);
    vkGetPhysicalDeviceQueueFamilyProperties(gpu, &nf, fams.data());
    for (family = 0; family < nf && !(fams[family].queueFlags & VK_QUEUE_COMPUTE_BIT); family++) {}
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, nullptr, 0, family, 1, &prio};
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    VK(vkCreateDevice(gpu, &dci, nullptr, &device));
    vkGetDeviceQueue(device, family, 0, &queue);
    VkCommandPoolCreateInfo cpci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, nullptr, 0, family};
    VK(vkCreateCommandPool(device, &cpci, nullptr, &pool));

    const VkImageUsageFlags storage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    Image frame = make_image(VK_FORMAT_R8G8B8A8_UNORM, kSize, VK_IMAGE_USAGE_SAMPLED_BIT |
                                                                 VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    Image luma[2] = {make_image(VK_FORMAT_R32_SFLOAT, kLuma, storage),
                     make_image(VK_FORMAT_R32_SFLOAT, kLuma, storage)};
    Image luma2[2] = {make_image(VK_FORMAT_R32_SFLOAT, kLuma2, storage),
                      make_image(VK_FORMAT_R32_SFLOAT, kLuma2, storage)};
    Image motion_raw = make_image(VK_FORMAT_R16G16B16A16_SFLOAT, kMotion, storage);
    Image motion = make_image(VK_FORMAT_R16G16B16A16_SFLOAT, kMotion, storage | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    Image pixel_motion = make_image(VK_FORMAT_R16G16B16A16_SFLOAT, kLuma, storage | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    Image camera = make_image(VK_FORMAT_R32G32B32A32_SFLOAT, 1, storage | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, 4);
    Buffer upload = make_buffer(kSize * kSize * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    Buffer readback = make_buffer(kMotion * kMotion * 8, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    Buffer readback_px = make_buffer(kLuma * kLuma * 8, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    Buffer readback_cam = make_buffer(4 * 16, VK_BUFFER_USAGE_TRANSFER_DST_BIT);

    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VkSampler linear, nearest;
    VK(vkCreateSampler(device, &sci, nullptr, &linear));
    sci.magFilter = sci.minFilter = VK_FILTER_NEAREST;
    VK(vkCreateSampler(device, &sci, nullptr, &nearest));

    // Same bindings as shaders/common.glsl and the layer.
    const VkDescriptorType S = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, I = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    constexpr uint32_t kBindings = 15;
    const VkDescriptorType types[kBindings] = {S, S, S, I, I, I, S, S, S, I, S, I, S, I, S};
    VkDescriptorSetLayoutBinding b[kBindings];
    for (uint32_t i = 0; i < kBindings; i++)
        b[i] = {i, types[i], 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo dlci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, nullptr, 0, kBindings, b};
    VkDescriptorSetLayout set_layout;
    VK(vkCreateDescriptorSetLayout(device, &dlci, nullptr, &set_layout));
    VkDescriptorPoolSize ps[2] = {{S, 18}, {I, 12}};
    VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, nullptr, 0, 2, 2, ps};
    VkDescriptorPool dpool;
    VK(vkCreateDescriptorPool(device, &dpci, nullptr, &dpool));
    VkDescriptorSetLayout layouts[2] = {set_layout, set_layout};
    VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, nullptr, dpool, 2, layouts};
    VkDescriptorSet sets[2];
    VK(vkAllocateDescriptorSets(device, &dsai, sets));
    for (int c = 0; c < 2; c++) {
        VkImageView views[kBindings] = {frame.view, luma[c].view, luma[1 - c].view, luma[c].view,
                                        motion_raw.view, motion.view, motion.view,
                                        luma2[c].view, luma2[1 - c].view, luma2[c].view,
                                        frame.view, pixel_motion.view, pixel_motion.view, camera.view, camera.view};
        VkDescriptorImageInfo info[kBindings];
        VkWriteDescriptorSet w[kBindings];
        for (uint32_t i = 0; i < kBindings; i++) {
            bool exact = i == 1 || i == 2 || i == 7 || i == 8 || i == 12 || i == 14;
            info[i] = {types[i] != S ? VK_NULL_HANDLE : exact ? nearest : linear, views[i], VK_IMAGE_LAYOUT_GENERAL};
            w[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, sets[c], i, 0, 1, types[i], &info[i]};
        }
        vkUpdateDescriptorSets(device, kBindings, w, 0, nullptr);
    }

    struct {
        float tint[4];
        float phase;
        int32_t mode, has_prev;
        float zero_bias;
    } pc = {{0, 0, 0, 0}, 0.0f, 1, 1, 0.05f};
    VkPushConstantRange pcr{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc};
    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, nullptr, 0, 1, &set_layout, 1, &pcr};
    VkPipelineLayout layout;
    VK(vkCreatePipelineLayout(device, &plci, nullptr, &layout));
    auto pipeline = [&](const uint32_t *code, size_t size) {
        VkShaderModuleCreateInfo smci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, nullptr, 0, size, code};
        VkShaderModule m;
        VK(vkCreateShaderModule(device, &smci, nullptr, &m));
        VkComputePipelineCreateInfo cpci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        cpci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, m,
                      "main", nullptr};
        cpci.layout = layout;
        VkPipeline p;
        VK(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &cpci, nullptr, &p));
        vkDestroyShaderModule(device, m, nullptr);
        return p;
    };
    VkPipeline luma_p = pipeline(luma_comp, sizeof luma_comp);
    VkPipeline pyramid_p = pipeline(pyramid_comp, sizeof pyramid_comp);
    VkPipeline motion_p = pipeline(motion_comp, sizeof motion_comp);
    VkPipeline filter_p = pipeline(filter_comp, sizeof filter_comp);
    VkPipeline refine_p = pipeline(refine_comp, sizeof refine_comp);
    VkPipeline global_p = pipeline(global_comp, sizeof global_comp);

    VkCommandBuffer cmd = begin();
    for (VkImage img : {frame.image, luma[0].image, luma[1].image, luma2[0].image, luma2[1].image,
                        motion_raw.image, motion.image, pixel_motion.image, camera.image})
        to_general(cmd, img);
    end(cmd);

    // Each case: the pattern moves by (dx, dy) frame pixels between the two
    // frames, everywhere or (split) only in the left half while the right
    // half stays still. The blocks should find that motion to the 4 pixel
    // grid, and the per pixel motion should follow the split. Some cases
    // also zoom the pattern about the centre, add perspective (the bottom
    // moving faster and spreading out, like the road when driving forward),
    // or keep a still strip at the bottom like a HUD; those only check the
    // camera model, which should give the motion of the pattern everywhere.
    struct Case {
        int dx, dy;
        bool split;
        float zoom = 1.0f;
        bool hud = false;
        float perspective = 0.0f;
    };
    const Case cases[] = {{0, 0, false},      {12, -8, false},       {-20, 4, false},       {30, 18, false},
                          {-60, -40, false},  {100, -60, false},     {24, 8, true},         {-36, 0, true},
                          {0, 0, false, 1.05f}, {8, -4, false, 0.96f}, {12, 6, false, 1.0f, true},
                          {-6, 10, false, 1.03f, true}, {0, 4, false, 1.0f, false, 0.12f},
                          {4, 6, false, 1.02f, true, 0.1f}};
    int failures = 0;
    for (const Case &c : cases) {
        float dx = (float)c.dx, dy = (float)c.dy;
        auto moving = [&](float x) { return !c.split || x < kSize / 2; };
        const float half = kSize / 2.0f;
        // Where the pattern at frame 1 pixel (x, y) was in frame 0.
        auto source = [&](float x, float y, float &sx, float &sy) {
            float k = c.perspective * (y - half) / kSize;
            sx = half + (x - half) / c.zoom - dx - k * (x - half);
            sy = half + (y - half) / c.zoom - dy - k * (y - half);
        };
        const bool camera_only = c.zoom != 1.0f || c.hud || c.perspective != 0.0f;
        pc.mode = camera_only ? 3 : 1;
        for (int f = 0; f < 2; f++) {
            auto *px = (uint8_t *)upload.data;
            for (uint32_t y = 0; y < kSize; y++)
                for (uint32_t x = 0; x < kSize; x++) {
                    // Frame 1 shows the pattern moved by (dx, dy).
                    float sx = (float)x, sy = (float)y;
                    if (f && moving((float)x) && !(c.hud && y >= kSize * 3 / 4))
                        source((float)x, (float)y, sx, sy);
                    float v = noise(sx + 1000, sy + 1000);
                    uint8_t g = (uint8_t)(v * 255.0f);
                    px[0] = px[1] = px[2] = g;
                    px[3] = 255;
                    px += 4;
                }
            cmd = begin();
            VkBufferImageCopy region{};
            region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.imageExtent = {kSize, kSize, 1};
            vkCmdCopyBufferToImage(cmd, upload.buffer, frame.image, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
            barrier(cmd);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &sets[f], 0, nullptr);
            vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, &pc);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, luma_p);
            vkCmdDispatch(cmd, kLuma / 8, kLuma / 8, 1);
            barrier(cmd);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pyramid_p);
            vkCmdDispatch(cmd, (kLuma2 + 7) / 8, (kLuma2 + 7) / 8, 1);
            barrier(cmd);
            if (f == 1) {
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, motion_p);
                vkCmdDispatch(cmd, kMotion, kMotion, 1);
                barrier(cmd);
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, filter_p);
                vkCmdDispatch(cmd, (kMotion + 7) / 8, (kMotion + 7) / 8, 1);
                barrier(cmd);
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, global_p);
                vkCmdDispatch(cmd, 1, 1, 1);
                barrier(cmd);
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, refine_p);
                vkCmdDispatch(cmd, kLuma / 8, kLuma / 8, 1);
                barrier(cmd);
                VkBufferImageCopy rb{};
                rb.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                rb.imageExtent = {kMotion, kMotion, 1};
                vkCmdCopyImageToBuffer(cmd, motion.image, VK_IMAGE_LAYOUT_GENERAL, readback.buffer, 1, &rb);
                rb.imageExtent = {kLuma, kLuma, 1};
                vkCmdCopyImageToBuffer(cmd, pixel_motion.image, VK_IMAGE_LAYOUT_GENERAL, readback_px.buffer, 1, &rb);
                rb.imageExtent = {4, 1, 1};
                vkCmdCopyImageToBuffer(cmd, camera.image, VK_IMAGE_LAYOUT_GENERAL, readback_cam.buffer, 1, &rb);
            }
            end(cmd);
        }

        // The camera model against the true motion of the pattern, inside
        // the frame and above the still strip, where camera mode keeps the
        // picture still anyway (split cases have no single motion).
        bool camera_ok = true;
        if (!c.split) {
            auto *v = (float *)readback_cam.data; // cx[0..3], cx[4..5], .., cy[0..3], cy[4..5]
            float worst = 0;
            const float bottom = c.hud ? kSize * 3 / 4 - 32 : kSize - 64;
            for (float y = 64; y <= bottom; y += 32)
                for (float x = 64; x <= kSize - 64; x += 32) {
                    float qx = (x - half) / kSize, qy = (y - half) / kSize;
                    const float f[6] = {1, qx, qy, qx * qx, qx * qy, qy * qy};
                    float mx = 0, my = 0;
                    for (int i = 0; i < 6; i++) {
                        mx += v[i] * f[i];
                        my += v[8 + i] * f[i];
                    }
                    // The pattern point now at (x, y) moved there from source.
                    float sx, sy;
                    source(x, y, sx, sy);
                    worst = fmaxf(worst, hypotf(mx - (x - sx), my - (y - sy)));
                }
            camera_ok = worst <= 3.0f; // block vectors are on a 4 px grid
            printf("move (%+.0f, %+.0f) zoom %.2f perspective %.2f%s: camera model off by %.2f px at most %s\n", dx,
                   dy, c.zoom, c.perspective, c.hud ? " with a still strip" : "", worst, camera_ok ? "ok" : "FAIL");
            failures += !camera_ok;
        }
        if (camera_only) {
            // Per pixel, camera mode should tell the still strip from the
            // rest.
            auto *pm = (uint16_t *)readback_px.data;
            const int strip = (int)kLuma * 3 / 4;
            int good = 0, total = 0;
            for (int y = 8; y < (int)kLuma - 8; y++)
                for (int x = 8; x < (int)kLuma - 8; x++) {
                    if (c.hud && abs(y - strip) < 2)
                        continue;
                    bool still = c.hud && y >= strip;
                    total++;
                    good += (half_to_float(pm[4 * (y * kLuma + x) + 3]) == 0.0f) == still;
                }
            bool split_ok = good >= total * 95 / 100;
            printf("move (%+.0f, %+.0f) zoom %.2f perspective %.2f%s: %d/%d pixels rightly still or moving %s\n", dx,
                   dy, c.zoom, c.perspective, c.hud ? " with a still strip" : "", good, total, split_ok ? "ok" : "FAIL");
            failures += !split_ok;
            continue;
        }

        // Blocks away from the borders, where the pattern entered the frame.
        bool ok = true;
        if (!c.split) {
            auto *m = (uint16_t *)readback.data;
            int good = 0, total = 0;
            float sx = 0, sy = 0;
            for (uint32_t y = 3; y < kMotion - 3; y++)
                for (uint32_t x = 3; x < kMotion - 3; x++) {
                    uint16_t *t = m + 4 * (y * kMotion + x);
                    float mx = half_to_float(t[0]), my = half_to_float(t[1]);
                    sx += mx;
                    sy += my;
                    total++;
                    good += fabsf(mx - dx) <= 2.0f && fabsf(my - dy) <= 2.0f;
                }
            ok = good >= total * 95 / 100;
            printf("move (%+.0f, %+.0f): mean block motion (%+.1f, %+.1f), %d/%d blocks within 2 px %s\n", dx, dy,
                   sx / total, sy / total, good, total, ok ? "ok" : "FAIL");
        }

        // Pixels away from the borders and the split.
        auto *pm = (uint16_t *)readback_px.data;
        int margin = 3 + (abs(c.dx) > abs(c.dy) ? abs(c.dx) : abs(c.dy)) / 4;
        int good = 0, total = 0;
        for (int y = margin; y < (int)kLuma - margin; y++)
            for (int x = margin; x < (int)kLuma - margin; x++) {
                if (c.split && abs(x - (int)kLuma / 2) < 2)
                    continue;
                uint16_t *t = pm + 4 * (y * kLuma + x);
                float mx = half_to_float(t[0]), my = half_to_float(t[1]);
                bool moves = moving((float)(x * 4));
                float ex = moves ? dx : 0, ey = moves ? dy : 0;
                total++;
                good += fabsf(mx - ex) <= 2.0f && fabsf(my - ey) <= 2.0f;
            }
        bool pixels_ok = good >= total * 95 / 100;
        printf("move (%+.0f, %+.0f)%s: %d/%d pixels within 2 px %s\n", dx, dy, c.split ? " left half only" : "",
               good, total, pixels_ok ? "ok" : "FAIL");
        failures += !ok + !pixels_ok;
    }
    vkDeviceWaitIdle(device);
    return failures ? 1 : 0;
}
