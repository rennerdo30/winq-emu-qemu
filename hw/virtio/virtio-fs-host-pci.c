/*
 * virtio-fs served inside QEMU: PCI transport
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/core/qdev-properties.h"
#include "hw/virtio/virtio-fs-host.h"
#include "hw/virtio/virtio-pci.h"
#include "qom/object.h"

typedef struct VirtIOFSHostPCI {
    VirtIOPCIProxy parent_obj;
    VirtIOFSHost vdev;
} VirtIOFSHostPCI;

#define TYPE_VIRTIO_FS_HOST_PCI "virtio-fs-host-pci-base"
DECLARE_INSTANCE_CHECKER(VirtIOFSHostPCI, VIRTIO_FS_HOST_PCI,
                         TYPE_VIRTIO_FS_HOST_PCI)

static const Property fsh_pci_properties[] = {
    DEFINE_PROP_UINT32("vectors", VirtIOPCIProxy, nvectors,
                       DEV_NVECTORS_UNSPECIFIED),
};

static void fsh_pci_realize(VirtIOPCIProxy *vpci_dev, Error **errp)
{
    VirtIOFSHostPCI *dev = VIRTIO_FS_HOST_PCI(vpci_dev);
    DeviceState *vdev = DEVICE(&dev->vdev);

    if (vpci_dev->nvectors == DEV_NVECTORS_UNSPECIFIED) {
        /* config change + hiprio + one per request queue */
        vpci_dev->nvectors = dev->vdev.num_request_queues + 2;
    }
    qdev_realize(vdev, BUS(&vpci_dev->bus), errp);
}

static void fsh_pci_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    VirtioPCIClass *k = VIRTIO_PCI_CLASS(klass);
    PCIDeviceClass *pcidev_k = PCI_DEVICE_CLASS(klass);

    k->realize = fsh_pci_realize;
    set_bit(DEVICE_CATEGORY_STORAGE, dc->categories);
    device_class_set_props(dc, fsh_pci_properties);
    pcidev_k->vendor_id = PCI_VENDOR_ID_REDHAT_QUMRANET;
    pcidev_k->device_id = 0; /* set by virtio-pci from the virtio ID */
    pcidev_k->revision = 0x00;
    pcidev_k->class_id = PCI_CLASS_STORAGE_OTHER;
}

static void fsh_pci_instance_init(Object *obj)
{
    VirtIOFSHostPCI *dev = VIRTIO_FS_HOST_PCI(obj);

    virtio_instance_init_common(obj, &dev->vdev, sizeof(dev->vdev),
                                TYPE_VIRTIO_FS_HOST);
}

static const VirtioPCIDeviceTypeInfo fsh_pci_info = {
    .base_name             = TYPE_VIRTIO_FS_HOST_PCI,
    /* virtio-fs is a modern-only device */
    .non_transitional_name = "virtio-fs-host-pci",
    .instance_size = sizeof(VirtIOFSHostPCI),
    .instance_init = fsh_pci_instance_init,
    .class_init    = fsh_pci_class_init,
};

static void fsh_pci_register(void)
{
    virtio_pci_types_register(&fsh_pci_info);
}

type_init(fsh_pci_register);
