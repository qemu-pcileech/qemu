/*
 * QEMU Virtual PCILeech Device
 *
 * Copyright (c) 2024 Zero Tang
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "hw/pci/pci.h"
#include "hw/hw.h"
#include "hw/pci/msi.h"
#include "qemu/timer.h"
#include "hw/qdev-properties.h"
#include "hw/qdev-properties-system.h"
#include "qom/object.h"
#include "qemu/main-loop.h" /* iothread mutex */
#include "qemu/module.h"
#include "chardev/char-fe.h"
#include "qapi/visitor.h"

#define TYPE_PCILEECH_DEVICE "pcileech"

#define PCILEECH_REQUEST_READ   0
#define PCILEECH_REQUEST_WRITE  1

#define PCILEECH_BUFFER_SIZE    1024

struct LeechRequestHeader {
    uint8_t command;    /* 0 - Read, 1 - Write */
    uint8_t reserved[7];
    /* Little-Endian */
    uint64_t address;
    uint64_t length;
};

struct LeechResponseHeader {
    /* Little-Endian */
    uint32_t result;
    uint8_t reserved[4];
    uint64_t length;    /* Indicates length of data followed by header */
};

/* Verify the header length */
static_assert(sizeof(struct LeechRequestHeader) == 24);
static_assert(sizeof(struct LeechResponseHeader) == 16);

struct PciLeechState {
    /* Internal State */
    PCIDevice device;
    struct LeechRequestHeader request;
    bool write_pending;
    uint64_t written_length;
    int pos;
    /* Communication */
    CharBackend chardev;
};

typedef struct LeechRequestHeader LeechRequestHeader;
typedef struct PciLeechState PciLeechState;

DECLARE_INSTANCE_CHECKER(PciLeechState, PCILEECH, TYPE_PCILEECH_DEVICE)

static void pci_leech_process_write_request(PciLeechState *state,
                                            const uint8_t *buf, int size)
{
    const uint64_t address = state->request.address + state->written_length;
    struct LeechResponseHeader response = { 0 };
    MemTxResult result = pci_dma_write(&state->device, address, buf, size);
    if (result != MEMTX_OK) {
        printf("PCILeech: Address 0x%lX Write Error! MemTxResult: 0x%X\n",
                                    address, result);
    }
    response.result = cpu_to_le32(result);
    response.length = 0;
    qemu_chr_fe_write_all(&state->chardev, (uint8_t *)&response,
                            sizeof(response));
    /* Increment written length counter. */
    state->written_length += size;
    /* Check if write-operation is fulfilled. */
    if (state->written_length == state->request.length) {
        state->written_length = 0;
        state->write_pending = false;
    }
}

static void pci_leech_process_read_request(PciLeechState *state)
{
    uint8_t buff[PCILEECH_BUFFER_SIZE];
    struct LeechRequestHeader *request = &state->request;
    for (uint64_t i = 0; i < request->length; i += sizeof(buff)) {
        struct LeechResponseHeader response = { 0 };
        const uint64_t readlen = (request->length - i) <= sizeof(buff) ?
                                    (request->length - i) : sizeof(buff);
        MemTxResult result = pci_dma_read(&state->device, request->address + i,
                                                            buff, readlen);
        if (result != MEMTX_OK) {
            printf("PCILeech: Address 0x%lX Read Error! MemTxResult: 0x%X\n",
                    request->address + i, result);
        }
        response.result = cpu_to_le32(result);
        response.length = cpu_to_le64(readlen);
        qemu_chr_fe_write_all(&state->chardev, (uint8_t *)&response,
                            sizeof(struct LeechResponseHeader));
        qemu_chr_fe_write_all(&state->chardev, buff, readlen);
    }
}

static void pci_leech_chardev_read_handler(void *opaque, const uint8_t *buf,
                                            int size)
{
    PciLeechState *state = PCILEECH(opaque);
    uint8_t* req_buff = (uint8_t *)&state->request;
    printf("PCILeech: Incoming %u bytes...\n", size);
    if (state->write_pending) {
        /* Complete pending write operation.*/
        puts("PCILeech: Dispatching to pending-write handler...");
        pci_leech_process_write_request(state, buf, size);
    } else {
        /* Copy request to internal state. */
        puts("PCILeech: Dispatching to general handler...");
        memcpy(&req_buff[state->pos], buf, sizeof(struct LeechRequestHeader) -
                                                                state->pos);
        state->request.address = le64_to_cpu(state->request.address);
        state->request.length = le64_to_cpu(state->request.length);
        state->pos = 0;
        switch (state->request.command) {
        case PCILEECH_REQUEST_READ:
            pci_leech_process_read_request(state);
            break;
        case PCILEECH_REQUEST_WRITE:
            /* Set to write-pending state */
            state->write_pending = true;
            state->written_length = 0;
            break;
        default:
            printf("PCILeech: unknown request command (%u) is received!\n",
                                                state->request.command);
            break;
        }
    }
}

static int pci_leech_chardev_can_read_handler(void *opaque)
{
    PciLeechState *state = PCILEECH(opaque);
    if (state->write_pending) {
        /* Calculate the remaining pending length to write. */
        const uint64_t remainder = state->request.length -
                                    state->written_length;
        /* Limit receiving buffer to PCILEECH_BUFFER_SIZE. */
        return (remainder > PCILEECH_BUFFER_SIZE) ? PCILEECH_BUFFER_SIZE :
                                                        (int)remainder;
    } else {
        /* No pending operations, so let's just receive a request header. */
        return sizeof(struct LeechRequestHeader);
    }
}

static void pci_leech_realize(PCIDevice *pdev, Error **errp)
{
    PciLeechState *state = PCILEECH(pdev);
    qemu_chr_fe_set_handlers(&state->chardev,
                            pci_leech_chardev_can_read_handler,
                            pci_leech_chardev_read_handler,
                            NULL, NULL, state, NULL, true);
}

static Property leech_properties[] = {
    DEFINE_PROP_CHR("chardev", PciLeechState, chardev),
    DEFINE_PROP_END_OF_LIST(),
};

static void pci_leech_class_init(ObjectClass *class, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(class);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(class);
    k->realize = pci_leech_realize;
    /* Change the Vendor/Device ID to your favor. */
    /* These are the default values from PCILeech-FPGA. */
    k->vendor_id = PCI_VENDOR_ID_XILINX;
    k->device_id = 0x0666;
    k->revision = 0;
    k->class_id = PCI_CLASS_NETWORK_ETHERNET;
    device_class_set_props(dc, leech_properties);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static void pci_leech_register_types(void)
{
    static InterfaceInfo interfaces[] = {
        {INTERFACE_CONVENTIONAL_PCI_DEVICE},
        {},
    };
    static const TypeInfo leech_info = {
        .name = TYPE_PCILEECH_DEVICE,
        .parent = TYPE_PCI_DEVICE,
        .instance_size = sizeof(PciLeechState),
        .class_init = pci_leech_class_init,
        .interfaces = interfaces,
    };
    type_register_static(&leech_info);
}

type_init(pci_leech_register_types)
