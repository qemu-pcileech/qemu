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
    int pos;
    /* Communication */
    CharBackend chardev;
};

typedef struct LeechRequestHeader LeechRequestHeader;
typedef struct PciLeechState PciLeechState;

DECLARE_INSTANCE_CHECKER(PciLeechState, PCILEECH, TYPE_PCILEECH_DEVICE)

/*
static void pci_leech_process_write_request(PciLeechState *state,
                                            LeechRequestHeader *request)
{
    char buff[1024];
    for (uint64_t i = 0; i < request->length; i += sizeof(buff)) {
        struct LeechResponseHeader response = { 0 };
        char* response_buffer = (char *)&response;
        const uint64_t writelen = (request->length - i) <= sizeof(buff) ?
                                         (request->length - i) : sizeof(buff);
        ssize_t recvlen = 0, sendlen = 0;
        while (recvlen < writelen) {
            recvlen += recv(incoming, &buff[recvlen], writelen - recvlen, 0);
        }
        response.endianness = state->endianness;
        response.result = pci_dma_write(&state->device, request->address + i,
                                                            buff, writelen);
        if (response.result) {
            printf("PCILeech: Address 0x%lX Write Error! MemTxResult: 0x%X\n",
                    request->address + i, response.result);
        }
        response.length = 0;
        while (sendlen < sizeof(struct LeechResponseHeader)) {
            sendlen += send(incoming, &response_buffer[sendlen],
                            sizeof(struct LeechResponseHeader) - sendlen, 0);
        }
    }
}
*/

static void pci_leech_process_read_request(PciLeechState *state)
{
    uint8_t buff[1024];
    struct LeechRequestHeader *request = &state->request;
    for (uint64_t i = 0; i < request->length; i += sizeof(buff)) {
        struct LeechResponseHeader response = { 0 };
        uint8_t * response_buffer = (uint8_t *)&response;
        const uint64_t readlen = (request->length - i) <= sizeof(buff) ?
                                    (request->length - i) : sizeof(buff);
        ssize_t sendlen = 0;
        MemTxResult result = pci_dma_read(&state->device, request->address + i,
                                                            buff, readlen);
        if (result != MEMTX_OK) {
            printf("PCILeech: Address 0x%lX Read Error! MemTxResult: 0x%X\n",
                    request->address + i, result);
        }
        response.result = cpu_to_le32(result);
        response.length = cpu_to_le64(readlen);
        while (sendlen < sizeof(struct LeechResponseHeader)) {
            sendlen += qemu_chr_fe_write_all(&state->chardev, &response_buffer[sendlen],
                            sizeof(struct LeechResponseHeader) - sendlen);
        }
        sendlen = 0;
        while (sendlen < readlen) {
            sendlen += qemu_chr_fe_write_all(&state->chardev, &buff[sendlen], readlen - sendlen);
        }
    }
}

static void pci_leech_chardev_read_handler(void* opaque, const uint8_t *buf, int size)
{
    PciLeechState *state=PCILEECH(opaque);
    uint8_t* req_buff=(uint8_t*)&state->request;
    if(state->pos+size<sizeof(struct LeechRequestHeader)) {
        memcpy(&req_buff[state->pos],buf,size);
        state->pos+=size;
    } else {
        memcpy(&req_buff[state->pos],buf,sizeof(struct LeechRequestHeader)-state->pos);
        state->request.address=le64_to_cpu(state->request.address);
        state->request.length=le64_to_cpu(state->request.length);
        state->pos=0;
        switch(state->request.command) {
            case PCILEECH_REQUEST_READ:
                pci_leech_process_read_request(state);
                break;
            case PCILEECH_REQUEST_WRITE:
            default:
                printf("PCILeech: unknown request command (%u) is received!\n",state->request.command);
                break;
        }
    }
}

static int pci_leech_chardev_can_read_handler(void* opaque)
{
    return sizeof(struct LeechRequestHeader);
}

static void pci_leech_realize(PCIDevice *pdev, Error **errp)
{
    PciLeechState *state = PCILEECH(pdev);
    puts("PCILeech: Realize...");
    qemu_chr_fe_set_handlers(&state->chardev,pci_leech_chardev_can_read_handler,pci_leech_chardev_read_handler,NULL,NULL,state,NULL,true);
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
