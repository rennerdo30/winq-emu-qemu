/*
 * Windows hosts: read a Venus blob (GPU-only Vulkan memory) back to the CPU.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_DISPLAY_VIRTIO_GPU_VKREAD_H
#define HW_DISPLAY_VIRTIO_GPU_VKREAD_H

#include <virglrenderer.h>

typedef struct VkRead VkRead;

/*
 * Imports the exported blob memory and prepares a copy of size bytes from
 * offset. Always closes e->handle. NULL when Vulkan, the GPU or the memory
 * does not allow it.
 */
VkRead *vkread_import(const struct virgl_renderer_win32_export *e,
                      uint64_t offset, uint64_t size);

/* Copies the current contents to dst (size bytes); waits for the GPU. */
bool vkread_copy(VkRead *r, void *dst);

void vkread_free(VkRead *r);

#endif
