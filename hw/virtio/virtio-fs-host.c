/*
 * virtio-fs served inside QEMU by a host file-server library
 *
 * The standard virtio-fs device (virtio ID 26, so the guest drivers that
 * exist work unchanged), but instead of a vhost-user daemon, which QEMU on
 * Windows cannot use, requests go to a library loaded from QEMU's own
 * folder (qorral_fs): it answers each FUSE request from the configured
 * host folders. Requests run on the thread pool, never on the main loop or
 * a vCPU thread; completions are pushed from the main loop.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/main-loop.h"
#include "qemu/module.h"
#include "qemu/aio.h"
#include "block/thread-pool.h"
#include "hw/core/qdev-properties.h"
#include "hw/virtio/virtio-fs-host.h"
#include "hw/virtio/virtio-access.h"
#include "migration/vmstate.h"
#include "standard-headers/linux/virtio_fs.h"
#include "standard-headers/linux/virtio_ids.h"
#ifndef _WIN32
#include <dlfcn.h>
#endif

#define QFS_ABI_VERSION 1

#ifdef _WIN32
#define QFS_LIBRARY_NAME "qorral_fs.dll"
#elif defined(__APPLE__)
#define QFS_LIBRARY_NAME "libqorral_fs.dylib"
#else
#define QFS_LIBRARY_NAME "libqorral_fs.so"
#endif

typedef struct VirtIOFSHostReq {
    VirtQueueElement elem;  /* first: virtqueue_pop allocates the request */
    VirtIOFSHost *fs;
    VirtQueue *vq;
    unsigned generation;
    size_t reply_len;
} VirtIOFSHostReq;

/* The library next to the QEMU executable, never a search-path lookup. */
static char *fsh_default_library_path(void)
{
#ifdef _WIN32
    wchar_t buf[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, buf, MAX_PATH);
    g_autofree char *exe = NULL;
    g_autofree char *dir = NULL;

    if (n == 0 || n >= MAX_PATH) {
        return NULL;
    }
    exe = g_utf16_to_utf8(buf, n, NULL, NULL, NULL);
    if (!exe) {
        return NULL;
    }
    dir = g_path_get_dirname(exe);
    return g_build_filename(dir, QFS_LIBRARY_NAME, NULL);
#else
    g_autofree char *exe = g_file_read_link("/proc/self/exe", NULL);
    g_autofree char *dir = NULL;

    if (!exe) {
        return NULL;
    }
    dir = g_path_get_dirname(exe);
    return g_build_filename(dir, QFS_LIBRARY_NAME, NULL);
#endif
}

static void *fsh_open(const char *path, char **why)
{
#ifdef _WIN32
    g_autofree wchar_t *wpath = g_utf8_to_utf16(path, -1, NULL, NULL, NULL);
    /* Its dependencies come from its own folder and System32 only. */
    HMODULE h = wpath ? LoadLibraryExW(wpath, NULL,
                                       LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
                                       LOAD_LIBRARY_SEARCH_SYSTEM32) : NULL;
    if (!h) {
        *why = g_win32_error_message(GetLastError());
    }
    return h;
#else
    void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        *why = g_strdup(dlerror());
    }
    return h;
#endif
}

static void *fsh_sym(void *module, const char *name)
{
#ifdef _WIN32
    return (void *)GetProcAddress(module, name);
#else
    return dlsym(module, name);
#endif
}

static void fsh_close(void *module)
{
#ifdef _WIN32
    FreeLibrary(module);
#else
    dlclose(module);
#endif
}

static bool fsh_load_library(VirtIOFSHost *fs, Error **errp)
{
    VirtIOFSHostLib *lib = &fs->lib;
    g_autofree char *path = fs->library ? g_strdup(fs->library)
                                        : fsh_default_library_path();
    g_autofree char *why = NULL;

    if (!path) {
        error_setg(errp, "cannot locate %s", QFS_LIBRARY_NAME);
        return false;
    }
    lib->module = fsh_open(path, &why);
    if (!lib->module) {
        error_setg(errp, "cannot load the file-server library %s: %s",
                   path, why ? why : "unknown error");
        return false;
    }
    lib->abi_version = fsh_sym(lib->module, "qfs_abi_version");
    lib->create = fsh_sym(lib->module, "qfs_create");
    lib->handle = fsh_sym(lib->module, "qfs_handle");
    lib->reset = fsh_sym(lib->module, "qfs_reset");
    lib->destroy = fsh_sym(lib->module, "qfs_destroy");
    if (!lib->abi_version || !lib->create || !lib->handle || !lib->reset ||
        !lib->destroy) {
        error_setg(errp, "%s is not a file-server library", path);
        goto fail;
    }
    if (lib->abi_version() != QFS_ABI_VERSION) {
        error_setg(errp, "%s has interface version %u, this QEMU needs %u",
                   path, lib->abi_version(), QFS_ABI_VERSION);
        goto fail;
    }
    return true;

fail:
    fsh_close(lib->module);
    lib->module = NULL;
    return false;
}

/* Thread pool: answer one request into the guest's buffers. */
static int fsh_worker(void *opaque)
{
    VirtIOFSHostReq *r = opaque;
    VirtIOFSHost *fs = r->fs;

    r->reply_len = fs->lib.handle(fs->server,
                                  (const QfsIovec *)r->elem.out_sg,
                                  r->elem.out_num,
                                  (const QfsIovec *)r->elem.in_sg,
                                  r->elem.in_num);
    qemu_mutex_lock(&fs->lock);
    if (--fs->running == 0) {
        qemu_cond_broadcast(&fs->idle);
    }
    qemu_mutex_unlock(&fs->lock);
    return 0;
}

/* Main loop: hand the reply to the guest. */
static void fsh_done(void *opaque, int ret)
{
    VirtIOFSHostReq *r = opaque;
    VirtIOFSHost *fs = r->fs;
    VirtIODevice *vdev = VIRTIO_DEVICE(fs);

    if (r->generation == fs->generation) {
        virtqueue_push(r->vq, &r->elem, r->reply_len);
        virtio_notify(vdev, r->vq);
    } else {
        /* The device was reset meanwhile: the queue no longer owns it. */
        virtqueue_detach_element(r->vq, &r->elem, 0);
    }
    fs->pending--;
    g_free(r);
}

/*
 * Requests answered right away in the notifying thread. The thread-pool hop
 * plus the main-loop wakeup costs about 185 us per request on Windows hosts,
 * more than most requests take on the host (30-160 us for lookups, opens,
 * creates, small reads and writes, attribute changes). Only requests that
 * can take long stay on worker threads: syncs, fallocate, copy ranges, and
 * reads or writes of more than 64 KiB.
 */
#define FSH_INLINE_IO_MAX (64 * 1024)

static bool fsh_is_quick(const VirtIOFSHost *fs, const VirtQueueElement *elem)
{
    const uint8_t *hdr;
    uint32_t opcode, size;

    if (elem->out_num < 1 || elem->out_sg[0].iov_len < 8) {
        return false;
    }
    hdr = elem->out_sg[0].iov_base;
    memcpy(&opcode, hdr + 4, sizeof(opcode));
    if (!fs->inline_io) {
        /* Metadata only: lookups, attributes, releases, flushes, statfs. */
        switch (le32_to_cpu(opcode)) {
        case 1: case 2: case 3: case 5: case 17: case 18: case 22: case 23:
        case 25: case 29: case 34: case 36: case 42: case 52:
            return true;
        default:
            return false;
        }
    }
    switch (le32_to_cpu(opcode)) {
    case 15:    /* READ */
    case 16:    /* WRITE */
        /* fuse_in_header (40) + fh (8) + offset (8), then the size */
        if (elem->out_sg[0].iov_len < 60) {
            return false;
        }
        memcpy(&size, hdr + 56, sizeof(size));
        return le32_to_cpu(size) <= FSH_INLINE_IO_MAX;
    case 20:    /* FSYNC */
    case 30:    /* FSYNCDIR */
    case 43:    /* FALLOCATE */
    case 47:    /* COPY_FILE_RANGE */
    case 50:    /* SYNCFS */
        return false;
    default:
        return true;
    }
}

/* Answers or dispatches every request available on the queue now. */
static void fsh_drain(VirtIOFSHost *fs, VirtQueue *vq, bool *notify)
{
    VirtIOFSHostReq *r;

    while ((r = virtqueue_pop(vq, sizeof(VirtIOFSHostReq)))) {
        if (fs->inline_requests || (fs->inline_metadata && fsh_is_quick(fs, &r->elem))) {
            size_t len = fs->lib.handle(fs->server,
                                        (const QfsIovec *)r->elem.out_sg,
                                        r->elem.out_num,
                                        (const QfsIovec *)r->elem.in_sg,
                                        r->elem.in_num);
            virtqueue_push(vq, &r->elem, len);
            g_free(r);
            *notify = true;
            continue;
        }
        r->fs = fs;
        r->vq = vq;
        r->generation = fs->generation;
        r->reply_len = 0;
        qemu_mutex_lock(&fs->lock);
        fs->running++;
        qemu_mutex_unlock(&fs->lock);
        fs->pending++;
        thread_pool_submit_aio(fsh_worker, r, fsh_done, r);
    }
}

static void fsh_handle_output(VirtIODevice *vdev, VirtQueue *vq)
{
    VirtIOFSHost *fs = VIRTIO_FS_HOST(vdev);
    bool notify = false;

    if (!fs->server) {
        return;
    }
    /*
     * The usual virtio loop: no guest kicks while the queue is drained,
     * then notifications back on (with a full barrier) and one more look.
     * Draining until empty alone can miss a request: the "notify me" index
     * written by virtqueue_pop may become visible to the guest only after
     * the device's last emptiness check, so both sides assume the other
     * one acts, and the request waits for the next unrelated kick (seen as
     * 5 s stalls: the guest's periodic writeback).
     */
    do {
        virtio_queue_set_notification(vq, 0);
        fsh_drain(fs, vq, &notify);
        virtio_queue_set_notification(vq, 1);
    } while (!virtio_queue_empty(vq));
    if (notify) {
        virtio_notify(vdev, vq);
    }
}

/* Waits until no worker touches guest memory any more. */
static void fsh_wait_workers(VirtIOFSHost *fs)
{
    qemu_mutex_lock(&fs->lock);
    while (fs->running) {
        qemu_cond_wait(&fs->idle, &fs->lock);
    }
    qemu_mutex_unlock(&fs->lock);
}

static void fsh_reset(VirtIODevice *vdev)
{
    VirtIOFSHost *fs = VIRTIO_FS_HOST(vdev);

    fsh_wait_workers(fs);
    fs->generation++;
    if (fs->server) {
        /* The next request may be a new FUSE_INIT. */
        fs->lib.reset(fs->server);
    }
}

static void fsh_get_config(VirtIODevice *vdev, uint8_t *config)
{
    VirtIOFSHost *fs = VIRTIO_FS_HOST(vdev);
    struct virtio_fs_config cfg = {};

    memcpy(cfg.tag, fs->tag, MIN(strlen(fs->tag), sizeof(cfg.tag)));
    virtio_stl_p(vdev, &cfg.num_request_queues, fs->num_request_queues);
    memcpy(config, &cfg, sizeof(cfg));
}

static uint64_t fsh_get_features(VirtIODevice *vdev, uint64_t features,
                                 Error **errp)
{
    return features;
}

static void fsh_realize(DeviceState *dev, Error **errp)
{
    VirtIODevice *vdev = VIRTIO_DEVICE(dev);
    VirtIOFSHost *fs = VIRTIO_FS_HOST(dev);
    g_autofree char *config = NULL;
    g_autoptr(GError) gerr = NULL;
    char err[512] = "";
    unsigned i;

    if (!fs->tag || !*fs->tag) {
        error_setg(errp, "missing tag property");
        return;
    }
    if (strlen(fs->tag) > sizeof_field(struct virtio_fs_config, tag)) {
        error_setg(errp, "tag must be %zu bytes or less",
                   sizeof_field(struct virtio_fs_config, tag));
        return;
    }
    if (!fs->config) {
        error_setg(errp, "missing config property (the share list)");
        return;
    }
    if (fs->num_request_queues == 0) {
        error_setg(errp, "num-request-queues must be at least 1");
        return;
    }
    if (!is_power_of_2(fs->queue_size) || fs->queue_size > VIRTQUEUE_MAX_SIZE) {
        error_setg(errp, "queue-size must be a power of 2 up to %d",
                   VIRTQUEUE_MAX_SIZE);
        return;
    }
    if (!g_file_get_contents(fs->config, &config, NULL, &gerr)) {
        error_setg(errp, "cannot read the share list %s: %s", fs->config,
                   gerr->message);
        return;
    }
    if (!fsh_load_library(fs, errp)) {
        return;
    }
    fs->server = fs->lib.create(config, err, sizeof(err));
    if (!fs->server) {
        error_setg(errp, "file server: %s", err);
        fsh_close(fs->lib.module);
        fs->lib.module = NULL;
        return;
    }

    qemu_mutex_init(&fs->lock);
    qemu_cond_init(&fs->idle);
    virtio_init(vdev, VIRTIO_ID_FS, sizeof(struct virtio_fs_config));
    fs->hiprio_vq = virtio_add_queue(vdev, fs->queue_size, fsh_handle_output);
    fs->req_vqs = g_new(VirtQueue *, fs->num_request_queues);
    for (i = 0; i < fs->num_request_queues; i++) {
        fs->req_vqs[i] = virtio_add_queue(vdev, fs->queue_size,
                                          fsh_handle_output);
    }
}

static void fsh_unrealize(DeviceState *dev)
{
    VirtIODevice *vdev = VIRTIO_DEVICE(dev);
    VirtIOFSHost *fs = VIRTIO_FS_HOST(dev);
    unsigned i;

    fsh_wait_workers(fs);
    /* Let the queued completions run before the queues go away. */
    while (fs->pending) {
        aio_poll(qemu_get_aio_context(), true);
    }
    virtio_delete_queue(fs->hiprio_vq);
    for (i = 0; i < fs->num_request_queues; i++) {
        virtio_delete_queue(fs->req_vqs[i]);
    }
    g_free(fs->req_vqs);
    virtio_cleanup(vdev);
    if (fs->server) {
        fs->lib.destroy(fs->server);
        fs->server = NULL;
    }
    if (fs->lib.module) {
        fsh_close(fs->lib.module);
        fs->lib.module = NULL;
    }
    qemu_cond_destroy(&fs->idle);
    qemu_mutex_destroy(&fs->lock);
}

static const VMStateDescription vmstate_fsh = {
    .name = "virtio-fs-host",
    .unmigratable = 1,
};

static const Property fsh_properties[] = {
    DEFINE_PROP_STRING("tag", VirtIOFSHost, tag),
    DEFINE_PROP_STRING("config", VirtIOFSHost, config),
    DEFINE_PROP_STRING("library", VirtIOFSHost, library),
    DEFINE_PROP_UINT16("num-request-queues", VirtIOFSHost,
                       num_request_queues, 1),
    DEFINE_PROP_UINT16("queue-size", VirtIOFSHost, queue_size, 1024),
    DEFINE_PROP_BOOL("inline", VirtIOFSHost, inline_requests, false),
    DEFINE_PROP_BOOL("inline-metadata", VirtIOFSHost, inline_metadata, true),
    DEFINE_PROP_BOOL("inline-io", VirtIOFSHost, inline_io, true),
};

static void fsh_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    VirtioDeviceClass *vdc = VIRTIO_DEVICE_CLASS(klass);

    device_class_set_props(dc, fsh_properties);
    dc->vmsd = &vmstate_fsh;
    set_bit(DEVICE_CATEGORY_STORAGE, dc->categories);
    vdc->realize = fsh_realize;
    vdc->unrealize = fsh_unrealize;
    vdc->get_features = fsh_get_features;
    vdc->get_config = fsh_get_config;
    vdc->reset = fsh_reset;
}

static const TypeInfo fsh_info = {
    .name = TYPE_VIRTIO_FS_HOST,
    .parent = TYPE_VIRTIO_DEVICE,
    .instance_size = sizeof(VirtIOFSHost),
    .class_init = fsh_class_init,
};

static void fsh_register_types(void)
{
    type_register_static(&fsh_info);
}

type_init(fsh_register_types);
