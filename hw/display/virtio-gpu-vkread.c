/*
 * Windows hosts: read a Venus blob (GPU-only Vulkan memory) back to the CPU.
 *
 * A Venus guest driver scans out of host Vulkan memory that the CPU cannot
 * map. On Linux QEMU shows it through a dmabuf; Windows has none, and
 * importing the memory into OpenGL crashed AMD's GL driver. So QEMU opens its
 * own Vulkan device on the same GPU (matched by device UUID), imports the
 * blob through the opaque Win32 handle virglrenderer exports, and copies the
 * framebuffer into host-visible memory on every flush.
 *
 * Vulkan is loaded at run time (vulkan-1.dll), so this adds no build
 * dependency; without a loader or a matching GPU, readback simply fails.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "virtio-gpu-vkread.h"

#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>

struct VkRead {
    VkDeviceMemory mem;   /* the imported guest blob */
    VkBuffer src;         /* aliases it */
    VkDeviceMemory staging_mem;
    VkBuffer staging;
    void *staging_map;
    uint64_t offset, size;
};

static struct {
    bool tried, ok, logged_import;
    uint8_t device_uuid[VK_UUID_SIZE];
    HMODULE lib;
    VkInstance instance;
    VkPhysicalDevice pdev;
    VkPhysicalDeviceMemoryProperties memprops;
    VkDevice dev;
    VkQueue queue;
    VkCommandPool pool;
    VkCommandBuffer cmd;
    VkFence fence;
#define F(n) PFN_##n n
    F(vkGetInstanceProcAddr);
    F(vkCreateInstance);
    F(vkEnumeratePhysicalDevices);
    F(vkGetPhysicalDeviceProperties2);
    F(vkGetPhysicalDeviceMemoryProperties);
    F(vkGetPhysicalDeviceQueueFamilyProperties);
    F(vkCreateDevice);
    F(vkGetDeviceProcAddr);
    F(vkGetDeviceQueue);
    F(vkCreateCommandPool);
    F(vkAllocateCommandBuffers);
    F(vkCreateFence);
    F(vkAllocateMemory);
    F(vkFreeMemory);
    F(vkCreateBuffer);
    F(vkDestroyBuffer);
    F(vkGetBufferMemoryRequirements);
    F(vkBindBufferMemory);
    F(vkMapMemory);
    F(vkResetFences);
    F(vkWaitForFences);
    F(vkResetCommandBuffer);
    F(vkBeginCommandBuffer);
    F(vkEndCommandBuffer);
    F(vkCmdCopyBuffer);
    F(vkQueueSubmit);
#undef F
} vk;

#define LOAD_I(n) (vk.n = (PFN_##n)vk.vkGetInstanceProcAddr(vk.instance, #n))
#define LOAD_D(n) (vk.n = (PFN_##n)vk.vkGetDeviceProcAddr(vk.dev, #n))

static char *uuid_hex(const uint8_t *u)
{
    GString *str = g_string_new(NULL);
    int i;

    for (i = 0; i < VK_UUID_SIZE; i++) {
        g_string_append_printf(str, "%02x", u[i]);
    }
    return g_string_free(str, false);
}

static bool vkread_init(const uint8_t *device_uuid)
{
    VkPhysicalDevice pdevs[16];
    uint32_t n = 16, i, qcount = 0, qfam = UINT32_MAX;
    VkQueueFamilyProperties qprops[16];
    float prio = 1.0f;

    if (vk.tried) {
        return vk.ok && !memcmp(vk.device_uuid, device_uuid, VK_UUID_SIZE);
    }
    vk.tried = true;
    memcpy(vk.device_uuid, device_uuid, VK_UUID_SIZE);

    vk.lib = LoadLibraryA("vulkan-1.dll");
    if (!vk.lib) {
        error_report("virtio-gpu: no Vulkan loader for blob readback");
        return false;
    }
    vk.vkGetInstanceProcAddr =
        (PFN_vkGetInstanceProcAddr)(void *)GetProcAddress(vk.lib, "vkGetInstanceProcAddr");
    LOAD_I(vkCreateInstance);
    if (!vk.vkCreateInstance) {
        return false;
    }
    VkApplicationInfo app = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "qemu-blob-readback",
        .apiVersion = VK_API_VERSION_1_1,
    };
    VkInstanceCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app,
    };
    if (vk.vkCreateInstance(&ici, NULL, &vk.instance) != VK_SUCCESS) {
        error_report("virtio-gpu: vkCreateInstance failed (blob readback)");
        return false;
    }
    LOAD_I(vkEnumeratePhysicalDevices);
    LOAD_I(vkGetPhysicalDeviceProperties2);
    LOAD_I(vkGetPhysicalDeviceMemoryProperties);
    LOAD_I(vkGetPhysicalDeviceQueueFamilyProperties);
    LOAD_I(vkCreateDevice);
    LOAD_I(vkGetDeviceProcAddr);

    if (vk.vkEnumeratePhysicalDevices(vk.instance, &n, pdevs) < 0) {
        return false;
    }
    for (i = 0; i < n && !vk.pdev; i++) {
        VkPhysicalDeviceIDProperties id = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES,
        };
        VkPhysicalDeviceProperties2 p2 = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
            .pNext = &id,
        };
        vk.vkGetPhysicalDeviceProperties2(pdevs[i], &p2);
        if (!memcmp(id.deviceUUID, device_uuid, VK_UUID_SIZE)) {
            vk.pdev = pdevs[i];
        }
    }
    if (!vk.pdev) {
        g_autofree char *want = uuid_hex(device_uuid);
        error_report("virtio-gpu: blob readback: no GPU with the renderer's device UUID "
                     "(%s, %u GPUs)", want, n);
        for (i = 0; i < n; i++) {
            VkPhysicalDeviceIDProperties id = {
                .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES,
            };
            VkPhysicalDeviceProperties2 p2 = {
                .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
                .pNext = &id,
            };
            vk.vkGetPhysicalDeviceProperties2(pdevs[i], &p2);
            g_autofree char *have = uuid_hex(id.deviceUUID);
            error_report("virtio-gpu:   GPU %u %s: %s", i, p2.properties.deviceName, have);
        }
        return false;
    }
    vk.vkGetPhysicalDeviceMemoryProperties(vk.pdev, &vk.memprops);
    qcount = 16;
    vk.vkGetPhysicalDeviceQueueFamilyProperties(vk.pdev, &qcount, qprops);
    /* A transfer-only queue keeps out of the guest's graphics work. */
    for (i = 0; i < qcount; i++) {
        if ((qprops[i].queueFlags & VK_QUEUE_TRANSFER_BIT) &&
            !(qprops[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT))) {
            qfam = i;
            break;
        }
    }
    for (i = 0; i < qcount && qfam == UINT32_MAX; i++) {
        if (qprops[i].queueFlags & (VK_QUEUE_TRANSFER_BIT | VK_QUEUE_GRAPHICS_BIT |
                                    VK_QUEUE_COMPUTE_BIT)) {
            qfam = i;
        }
    }
    if (qfam == UINT32_MAX) {
        return false;
    }
    const char *exts[] = {
        VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
        VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME,
    };
    VkDeviceQueueCreateInfo qci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = qfam,
        .queueCount = 1,
        .pQueuePriorities = &prio,
    };
    VkDeviceCreateInfo dci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &qci,
        .enabledExtensionCount = ARRAY_SIZE(exts),
        .ppEnabledExtensionNames = exts,
    };
    if (vk.vkCreateDevice(vk.pdev, &dci, NULL, &vk.dev) != VK_SUCCESS) {
        error_report("virtio-gpu: vkCreateDevice failed (blob readback)");
        return false;
    }
    LOAD_D(vkGetDeviceQueue);
    LOAD_D(vkCreateCommandPool);
    LOAD_D(vkAllocateCommandBuffers);
    LOAD_D(vkCreateFence);
    LOAD_D(vkAllocateMemory);
    LOAD_D(vkFreeMemory);
    LOAD_D(vkCreateBuffer);
    LOAD_D(vkDestroyBuffer);
    LOAD_D(vkGetBufferMemoryRequirements);
    LOAD_D(vkBindBufferMemory);
    LOAD_D(vkMapMemory);
    LOAD_D(vkResetFences);
    LOAD_D(vkWaitForFences);
    LOAD_D(vkResetCommandBuffer);
    LOAD_D(vkBeginCommandBuffer);
    LOAD_D(vkEndCommandBuffer);
    LOAD_D(vkCmdCopyBuffer);
    LOAD_D(vkQueueSubmit);

    vk.vkGetDeviceQueue(vk.dev, qfam, 0, &vk.queue);
    VkCommandPoolCreateInfo pci = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = qfam,
    };
    VkCommandBufferAllocateInfo cai = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandBufferCount = 1,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
    };
    VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    if (vk.vkCreateCommandPool(vk.dev, &pci, NULL, &vk.pool) != VK_SUCCESS) {
        return false;
    }
    cai.commandPool = vk.pool;
    if (vk.vkAllocateCommandBuffers(vk.dev, &cai, &vk.cmd) != VK_SUCCESS ||
        vk.vkCreateFence(vk.dev, &fci, NULL, &vk.fence) != VK_SUCCESS) {
        return false;
    }
    vk.ok = true;
    info_report("virtio-gpu: blob scanouts read back through Vulkan (queue family %u)", qfam);
    return true;
}

static int find_type(uint32_t bits, VkMemoryPropertyFlags want)
{
    uint32_t i;

    for (i = 0; i < vk.memprops.memoryTypeCount; i++) {
        if ((bits & (1u << i)) &&
            (vk.memprops.memoryTypes[i].propertyFlags & want) == want) {
            return i;
        }
    }
    return -1;
}

void vkread_free(VkRead *r)
{
    if (!r) {
        return;
    }
    if (vk.dev) {
        if (r->staging) {
            vk.vkDestroyBuffer(vk.dev, r->staging, NULL);
        }
        if (r->staging_mem) {
            vk.vkFreeMemory(vk.dev, r->staging_mem, NULL);
        }
        if (r->src) {
            vk.vkDestroyBuffer(vk.dev, r->src, NULL);
        }
        if (r->mem) {
            vk.vkFreeMemory(vk.dev, r->mem, NULL);
        }
    }
    g_free(r);
}

VkRead *vkread_import(const struct virgl_renderer_win32_export *e,
                      uint64_t offset, uint64_t size)
{
    VkRead *r;
    VkMemoryRequirements req;
    int type;

    if (!vkread_init(e->device_uuid)) {
        CloseHandle(e->handle);
        return NULL;
    }
    /*
     * The guest's scanouts are dedicated image allocations. Vulkan wants those
     * imported together with a matching image, which QEMU cannot recreate (it
     * does not know the guest's image parameters). Reading the memory through
     * a buffer works on AMD's driver with linear scanouts; set
     * QEMU_BLOB_STRICT_DEDICATED=1 to refuse it instead.
     */
    if (offset + size > e->allocation_size ||
        ((e->flags & VIRGL_RENDERER_WIN32_HANDLE_DEDICATED) &&
         g_getenv("QEMU_BLOB_STRICT_DEDICATED"))) {
        error_report("virtio-gpu: blob not readable: %" PRIu64 "+%" PRIu64 " of %" PRIu64
                     " bytes, %s", offset, size, e->allocation_size,
                     (e->flags & VIRGL_RENDERER_WIN32_HANDLE_DEDICATED) ? "dedicated" : "shared");
        CloseHandle(e->handle);
        return NULL;
    }
    r = g_new0(VkRead, 1);
    r->offset = offset;
    r->size = size;

    VkImportMemoryWin32HandleInfoKHR import = {
        .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT,
        .handle = e->handle,
    };
    VkMemoryAllocateInfo mai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &import,
        .allocationSize = e->allocation_size,
        .memoryTypeIndex = e->memory_type_index,
    };
    VkResult res = vk.vkAllocateMemory(vk.dev, &mai, NULL, &r->mem);
    /* Importing an NT handle does not take ownership of it. */
    CloseHandle(e->handle);
    if (res != VK_SUCCESS) {
        error_report("virtio-gpu: blob import failed (%d)", res);
        goto fail;
    }

    VkExternalMemoryBufferCreateInfo ext = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT,
    };
    VkBufferCreateInfo bci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .pNext = &ext,
        .size = offset + size,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    if (vk.vkCreateBuffer(vk.dev, &bci, NULL, &r->src) != VK_SUCCESS) {
        error_report("virtio-gpu: blob readback: source buffer failed");
        goto fail;
    }
    vk.vkGetBufferMemoryRequirements(vk.dev, r->src, &req);
    if (!(req.memoryTypeBits & (1u << e->memory_type_index)) ||
        req.size > e->allocation_size) {
        error_report("virtio-gpu: blob memory cannot back a copy buffer "
                     "(types 0x%x, type %u, need %" PRIu64 " of %" PRIu64 ")",
                     req.memoryTypeBits, e->memory_type_index,
                     (uint64_t)req.size, e->allocation_size);
        goto fail;
    }
    if (vk.vkBindBufferMemory(vk.dev, r->src, r->mem, 0) != VK_SUCCESS) {
        error_report("virtio-gpu: blob readback: binding the blob failed");
        goto fail;
    }

    bci.pNext = NULL;
    bci.size = size;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (vk.vkCreateBuffer(vk.dev, &bci, NULL, &r->staging) != VK_SUCCESS) {
        goto fail;
    }
    vk.vkGetBufferMemoryRequirements(vk.dev, r->staging, &req);
    type = find_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                     VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                     VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
    if (type < 0) {
        type = find_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    }
    if (type < 0) {
        error_report("virtio-gpu: blob readback: no host-visible memory type");
        goto fail;
    }
    VkMemoryAllocateInfo sai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = req.size,
        .memoryTypeIndex = type,
    };
    if (vk.vkAllocateMemory(vk.dev, &sai, NULL, &r->staging_mem) != VK_SUCCESS ||
        vk.vkBindBufferMemory(vk.dev, r->staging, r->staging_mem, 0) != VK_SUCCESS ||
        vk.vkMapMemory(vk.dev, r->staging_mem, 0, VK_WHOLE_SIZE, 0,
                       &r->staging_map) != VK_SUCCESS) {
        error_report("virtio-gpu: blob readback: staging memory failed");
        goto fail;
    }
    if (!vk.logged_import) {
        vk.logged_import = true;
        info_report("virtio-gpu: blob imported (type %u, %" PRIu64 " bytes, %s)",
                    e->memory_type_index, e->allocation_size,
                    (e->flags & VIRGL_RENDERER_WIN32_HANDLE_DEDICATED) ?
                    "dedicated, read as a buffer" : "shared");
    }
    return r;

fail:
    vkread_free(r);
    return NULL;
}

bool vkread_copy(VkRead *r, void *dst)
{
    return vkread_copy_range(r, dst, 0, r->size);
}

bool vkread_copy_range(VkRead *r, void *dst, uint64_t off, uint64_t len)
{
    VkCommandBufferBeginInfo cbi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    VkBufferCopy region = { .srcOffset = r->offset + off, .dstOffset = off, .size = len };
    if (off > r->size || len > r->size - off) {
        return false;
    }
    VkSubmitInfo si = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &vk.cmd,
    };

    vk.vkResetCommandBuffer(vk.cmd, 0);
    vk.vkBeginCommandBuffer(vk.cmd, &cbi);
    vk.vkCmdCopyBuffer(vk.cmd, r->src, r->staging, 1, &region);
    vk.vkEndCommandBuffer(vk.cmd);
    vk.vkResetFences(vk.dev, 1, &vk.fence);
    if (vk.vkQueueSubmit(vk.queue, 1, &si, vk.fence) != VK_SUCCESS) {
        return false;
    }
    /* 4 MB on the GPU: well under a millisecond; give up after 100 ms. */
    if (vk.vkWaitForFences(vk.dev, 1, &vk.fence, VK_TRUE, 100 * 1000 * 1000) != VK_SUCCESS) {
        return false;
    }
    memcpy(dst, (const uint8_t *)r->staging_map + off, len);
    return true;
}
