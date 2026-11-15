/*
 * virtio-fs served inside QEMU by a host file-server library (qorral_fs)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef QEMU_VIRTIO_FS_HOST_H
#define QEMU_VIRTIO_FS_HOST_H

#include "hw/virtio/virtio.h"
#include "qemu/thread.h"
#include "qom/object.h"

#define TYPE_VIRTIO_FS_HOST "virtio-fs-host-device"
OBJECT_DECLARE_SIMPLE_TYPE(VirtIOFSHost, VIRTIO_FS_HOST)

/* Matches the library's QfsIovec (and struct iovec) layout. */
typedef struct QfsIovec {
    void *base;
    size_t len;
} QfsIovec;

typedef struct VirtIOFSHostLib {
    void *module;       /* HMODULE or dlopen handle */
    uint32_t (*abi_version)(void);
    void *(*create)(const char *config, char *err, size_t err_len);
    size_t (*handle)(void *fs, const QfsIovec *in, size_t n_in,
                     const QfsIovec *out, size_t n_out);
    void (*reset)(void *fs);
    void (*destroy)(void *fs);
} VirtIOFSHostLib;

struct VirtIOFSHost {
    VirtIODevice parent_obj;

    /* properties */
    char *tag;
    char *config;       /* path of the share list (JSON) */
    char *library;      /* optional path of the file-server library */
    uint16_t num_request_queues;
    uint16_t queue_size;
    bool inline_requests; /* experiment: answer everything in the notifying thread */
    bool inline_metadata; /* answer quick metadata requests in the notifying thread */
    bool inline_io;       /* also small reads/writes, opens, creates, attribute changes */

    VirtIOFSHostLib lib;
    void *server;
    VirtQueue *hiprio_vq;
    VirtQueue **req_vqs;

    /* Requests whose worker has not finished (guarded by lock). */
    QemuMutex lock;
    QemuCond idle;
    unsigned running;
    /* Requests whose completion has not run yet (main loop only). */
    unsigned pending;
    /* Bumped on reset: older completions are dropped, not pushed. */
    unsigned generation;
};

#endif
