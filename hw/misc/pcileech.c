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
#include "qapi/visitor.h"

#define TYPE_PCILEECH_DEVICE "pcileech"

struct LeechRequestHeader {
    uint8_t endianness; /* 0 - Little, 1 - Big */
    uint8_t command;    /* 0 - Read, 1 - Write */
    uint8_t reserved[6];
    /* Variable Endianness */
    uint64_t address;
    uint64_t length;
};

struct LeechResponseHeader {
    uint8_t endianness; /* 0 - Little, 1 - Big */
    uint8_t reserved[3];
    MemTxResult result;
    uint64_t length;    /* Indicates length of data followed by header */
};

/* Verify the header length */
static_assert(sizeof(struct LeechRequestHeader) == 24);
static_assert(sizeof(struct LeechResponseHeader) == 16);

struct PciLeechState {
    /* Internal State */
    PCIDevice device;
    QemuThread thread;
    QemuMutex mutex;
    bool endianness;
    bool stopping;
    /* Communication */
    char *host;
    uint16_t port;
    int sockfd;
};

typedef struct LeechRequestHeader LeechRequestHeader;
typedef struct PciLeechState PciLeechState;

DECLARE_INSTANCE_CHECKER(PciLeechState, PCILEECH, TYPE_PCILEECH_DEVICE)

static void pci_leech_process_write_request(PciLeechState *state,
                                            LeechRequestHeader *request,
                                            int incoming)
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

static void pci_leech_process_read_request(PciLeechState *state,
                                            LeechRequestHeader *request,
                                            int incoming)
{
    char buff[1024];
    for (uint64_t i = 0; i < request->length; i += sizeof(buff)) {
        struct LeechResponseHeader response = { 0 };
        char* response_buffer = (char *)&response;
        const uint64_t readlen = (request->length - i) <= sizeof(buff) ?
                                    (request->length - i) : sizeof(buff);
        ssize_t sendlen = 0;
        response.endianness = state->endianness;
        response.result = pci_dma_read(&state->device, request->address + i,
                                                            buff, readlen);
        if (response.result) {
            printf("PCILeech: Address 0x%lX Read Error! MemTxResult: 0x%X\n",
                    request->address + i, response.result);
        }
        response.length = (request->endianness != state->endianness) ?
                                            bswap64(readlen) : readlen;
        while (sendlen < sizeof(struct LeechResponseHeader)) {
            sendlen += send(incoming, &response_buffer[sendlen],
                            sizeof(struct LeechResponseHeader) - sendlen, 0);
        }
        sendlen = 0;
        while (sendlen < readlen) {
            sendlen += send(incoming, &buff[sendlen], readlen - sendlen, 0);
        }
    }
}

static void *pci_leech_worker_thread(void *opaque)
{
    PciLeechState *state = PCILEECH(opaque);
    while (1) {
        LeechRequestHeader request;
        char *request_buffer = (char *)&request;
        ssize_t received = 0;
        int incoming;
        struct sockaddr address;
        socklen_t addrlen;
        /* Check if we are stopping. */
        qemu_mutex_lock(&state->mutex);
        if (state->stopping) {
            qemu_mutex_unlock(&state->mutex);
            break;
        }
        qemu_mutex_unlock(&state->mutex);
        /* Accept PCILeech requests. */
        /* Use HTTP1.0-like protocol for simplicity. */
        incoming = accept(state->sockfd, &address, &addrlen);
        if (incoming < 0) {
            puts("WARNING: Failed to accept socket for PCILeech! Skipping "
                 "Request...\n");
            continue;
        }
        /* Get PCILeech requests. */
        while (received < sizeof(LeechRequestHeader)) {
            received += recv(incoming, &request_buffer[received],
                            sizeof(LeechRequestHeader) - received, 0);
        }
        /* Swap endianness. */
        if (request.endianness != state->endianness) {
            request.address = bswap64(request.address);
            request.length = bswap64(request.length);
        }
        /* Process PCILeech requests. */
        qemu_mutex_lock(&state->mutex);
        if (request.command) {
            pci_leech_process_write_request(state, &request, incoming);
        } else {
            pci_leech_process_read_request(state, &request, incoming);
        }
        qemu_mutex_unlock(&state->mutex);
        close(incoming);
    }
    return NULL;
}

static void pci_leech_realize(PCIDevice *pdev, Error **errp)
{
    PciLeechState *state = PCILEECH(pdev);
    struct sockaddr_in sock_addr;
    char host_ip[16];
    struct hostent *he = gethostbyname(state->host);
    if (he == NULL) {
        puts("gethostbyname failed!");
        exit(EXIT_FAILURE);
    }
    /* Initialize the socket for PCILeech. */
    state->sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (state->sockfd < 0) {
        puts("Failed to initialize socket for PCILeech!");
        exit(EXIT_FAILURE);
    }
    sock_addr.sin_family = AF_INET;
    sock_addr.sin_addr = *(struct in_addr *)he->h_addr;
    sock_addr.sin_port = htons(state->port);
    inet_ntop(AF_INET, &sock_addr.sin_addr, host_ip, sizeof(host_ip));
    if (bind(state->sockfd, (struct sockaddr *)&sock_addr, sizeof(sock_addr))
                                                                    < 0) {
        puts("Failed to bind socket for PCILeech!");
        close(state->sockfd);
        exit(EXIT_FAILURE);
    }
    if (listen(state->sockfd, 10) < 0) {
        puts("Failed to listen to socket for PCILeech!");
        close(state->sockfd);
        exit(EXIT_FAILURE);
    }
    printf("INFO: PCILeech is listening on %s:%u...\n", host_ip, state->port);
    /* Initialize the thread for PCILeech. */
    qemu_mutex_init(&state->mutex);
    qemu_thread_create(&state->thread, "pcileech", pci_leech_worker_thread,
                                            state, QEMU_THREAD_JOINABLE);
}

static void pci_leech_finalize(PCIDevice *pdev)
{
    PciLeechState *state = PCILEECH(pdev);
    puts("Stopping PCILeech Worker...");
    qemu_mutex_lock(&state->mutex);
    state->stopping = true;
    qemu_mutex_unlock(&state->mutex);
    close(state->sockfd);
    qemu_thread_join(&state->thread);
    qemu_mutex_destroy(&state->mutex);
}

char pci_leech_default_host[] = "0.0.0.0";

static void pci_leech_instance_init(Object *obj)
{
    int x = 1;
    char* y = (char *)&x;
    PciLeechState *state = PCILEECH(obj);
    /* QEMU's String-Property can't specify default value. */
    /* So we have to set the default on our own. */
    if (state->host == NULL) {
        state->host = pci_leech_default_host;
    }
    /* Save Our Endianness. */
    state->endianness = (*y == 0);
}

static Property leech_properties[] = {
    DEFINE_PROP_UINT16("port", PciLeechState, port, 6789),
    DEFINE_PROP_STRING("host", PciLeechState, host),
    DEFINE_PROP_END_OF_LIST(),
};

static void pci_leech_class_init(ObjectClass *class, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(class);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(class);
    k->realize = pci_leech_realize;
    k->exit = pci_leech_finalize;
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
        .instance_init = pci_leech_instance_init,
        .class_init = pci_leech_class_init,
        .interfaces = interfaces,
    };
    type_register_static(&leech_info);
}

type_init(pci_leech_register_types)
