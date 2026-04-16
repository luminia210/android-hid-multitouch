/*
 * libaoa_hid.c — AOA 2.0 Multi-Touch HID Library
 *
 * SPDX-License-Identifier: MIT
 *
 * See libaoa_hid.h for the public API and usage documentation.
 *
 * =========================================================================
 * Building
 * =========================================================================
 *
 * Linux — bundled static libusb:
 *   gcc -O3 -Wall -Wextra -std=c11 \
 *       -I./libusb/include/libusb-1.0 \
 *       myapp.c libaoa_hid.c \
 *       ./libusb/linux_x64/static/libusb-1.0.a -o myapp
 *
 * Linux — system libusb:
 *   sudo apt install libusb-1.0-0-dev
 *   gcc -O3 -Wall -Wextra -std=c11 myapp.c libaoa_hid.c -lusb-1.0 -o myapp
 *
 * macOS:
 *   brew install libusb
 *   clang -O3 -Wall -Wextra -std=c11 \
 *       -I$(brew --prefix libusb)/include/libusb-1.0 \
 *       -L$(brew --prefix libusb)/lib \
 *       myapp.c libaoa_hid.c -lusb-1.0 -o myapp
 *
 * Windows (MinGW-w64) — bundled static libusb:
 *   gcc -O3 -Wall -Wextra -std=c11 \
 *       -I./libusb/windows_x64/include/libusb-1.0 \
 *       -L./libusb/windows_x64/static \
 *       myapp.c libaoa_hid.c \
 *       -lusb-1.0 -lsetupapi -lole32 -luuid \
 *       -static-libgcc -static -s -o myapp.exe
 *
 * =========================================================================
 * Internal architecture
 * =========================================================================
 *
 * AoaDevice (opaque, heap-allocated)
 *   ctx               libusb_context*
 *   handle            libusb_device_handle*
 *   interface_claimed int      — 1 if libusb_claim_interface succeeded
 *   hid_desc          uint8_t* — heap-allocated HID descriptor
 *   hid_desc_len      int
 *
 * HID descriptor layout (built by build_hid_descriptor):
 *   Header    8 bytes              — Digitizer Application Collection + Report ID 1
 *   Body     75 bytes × max_contacts — one Logical Collection per finger slot
 *   Footer   19 bytes              — Contact Count + Max Contacts feature + End Collection
 *
 * Internal call flow:
 *   probe_and_open()       — opens a USB device, checks AOA version, returns handle
 *   build_hid_descriptor() — allocates and assembles the HID descriptor
 *   send_hid_desc()        — uploads descriptor in 64-byte chunks (AOA cmd 56)
 *   aoa_hid_connect_by_address() — claims interface, REGISTER_HID, SET_HID_REPORT_DESC
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/*
 * ssize_t portability:
 *   Linux / macOS : defined in <sys/types.h> (POSIX).
 *   MinGW-w64     : defined in <sys/types.h> (provided by the runtime).
 *   MSVC          : not defined; a fallback typedef is provided below.
 * This library targets MinGW-w64 on Windows, so the include is sufficient.
 * The MSVC guard is a courtesy for projects that mix toolchains.
 */
#include <sys/types.h>
#if defined(_MSC_VER) && !defined(ssize_t)
typedef __int64 ssize_t;
#endif

#include <libusb.h>

#include "libaoa_hid.h"

/* =========================================================================
 * Log macro
 * ====================================================================== */

#ifndef AOA_HID_SILENT
#  define AOA_LOG(...)  fprintf(stderr, __VA_ARGS__)
#else
#  define AOA_LOG(...)  ((void)0)
#endif

/* =========================================================================
 * AOA protocol constants
 * ====================================================================== */

#define AOA_VENDOR_ID            0x18D1u  /* Google USB vendor ID           */
#define AOA_PRODUCT_ID_ACC       0x2D00u  /* AOA accessory mode, no ADB     */
#define AOA_PRODUCT_ID_ACC_ADB   0x2D01u  /* AOA accessory mode + ADB       */

#define AOA_GET_PROTOCOL         51  /* Query AOA protocol version          */
#define AOA_REGISTER_HID         54  /* Register a HID device               */
#define AOA_UNREGISTER_HID       55  /* Unregister a HID device             */
#define AOA_SET_HID_REPORT_DESC  56  /* Upload the HID report descriptor    */
#define AOA_SEND_HID_EVENT       57  /* Send one HID input report           */

#define HID_DEVICE_ID            1   /* Arbitrary ID for our single HID device */

/* =========================================================================
 * HID descriptor — finger Logical Collection template (75 bytes)
 *
 * Describes one finger slot in the HID report descriptor.
 * Copied once per finger slot into the full descriptor, then patched
 * at the offsets below with the actual screen dimensions and contact count.
 *
 * Template byte map:
 *  [00-05]  Usage Page (Digitizers 0x0D) / Usage (Finger 0x22) /
 *           Collection (Logical 0x02)
 *  [06-17]  TipSwitch (Usage 0x42) — 1 bit, Input (Data, Var, Abs)
 *  [18-21]  InRange   (Usage 0x32) — 1 bit, Input (Data, Var, Abs)
 *  [22-27]  Padding                — 6 bits, Input (Const) → fills one byte
 *  [28-39]  Contact ID (Usage 0x51) — 8 bits, Input (Data, Var, Abs)
 *             Logical Maximum at [32-33]: patched to (num_fingers - 1)
 *  [40-63]  X axis (Usage Page Generic Desktop 0x01, Usage X 0x30)
 *             16-bit little-endian Input (Data, Var, Abs)
 *             Logical  Maximum lo/hi at [47-48]: patched to (width  - 1)
 *             Physical Maximum lo/hi at [52-53]: patched to (width  - 1)
 *  [64-73]  Y axis (Usage Y 0x31)
 *             16-bit little-endian Input (Data, Var, Abs)
 *             Logical  Maximum lo/hi at [67-68]: patched to (height - 1)
 *             Physical Maximum lo/hi at [70-71]: patched to (height - 1)
 *  [74]     End Collection (0xC0)
 *
 * Patch offsets point to the first data byte after each HID item tag.
 * All X/Y offsets are 2-byte little-endian; OFF_CID_LOG_MAX is 1 byte.
 * ====================================================================== */

#define FINGER_COL_LEN   75

#define OFF_CID_LOG_MAX  33  /* Contact ID LOGICAL_MAXIMUM (1 byte): patched to num_fingers - 1  */
#define OFF_X_LOG_MAX    47  /* X axis LOGICAL_MAXIMUM  (2 bytes):   patched to (width  - 1) LE  */
#define OFF_X_PHY_MAX    52  /* X axis PHYSICAL_MAXIMUM (2 bytes):   patched to (width  - 1) LE  */
#define OFF_Y_LOG_MAX    67  /* Y axis LOGICAL_MAXIMUM  (2 bytes):   patched to (height - 1) LE  */
#define OFF_Y_PHY_MAX    70  /* Y axis PHYSICAL_MAXIMUM (2 bytes):   patched to (height - 1) LE  */

static const uint8_t k_finger_col_template[FINGER_COL_LEN] = {
    0x05,0x0D, 0x09,0x22, 0xA1,0x02,           /* Digitizers / Finger / Logical */
    0x09,0x42, 0x15,0x00, 0x25,0x01,
    0x75,0x01, 0x95,0x01, 0x81,0x02,            /* TipSwitch 1-bit input         */
    0x09,0x32, 0x81,0x02,                        /* InRange   1-bit input         */
    0x75,0x06, 0x95,0x01, 0x81,0x03,            /* 6-bit padding                 */
    0x09,0x51, 0x15,0x00, 0x25,0x09,
    0x75,0x08, 0x95,0x01, 0x81,0x02,            /* Contact ID 8-bit input        */
    0x05,0x01, 0x09,0x30, 0x15,0x00,
    0x26, 0x40,0x06,                             /* X Logical  Max — PATCHED      */
    0x35,0x00,
    0x46, 0x40,0x06,                             /* X Physical Max — PATCHED      */
    0x55,0x00, 0x65,0x00,
    0x75,0x10, 0x95,0x01, 0x81,0x02,            /* X 16-bit input                */
    0x09,0x31,
    0x26, 0x00,0x0A,                             /* Y Logical  Max — PATCHED      */
    0x46, 0x00,0x0A,                             /* Y Physical Max — PATCHED      */
    0x81,0x02,                                   /* Y 16-bit input                */
    0xC0                                         /* End Collection                */
};

typedef char _aoa_col_len_check[sizeof(k_finger_col_template) == FINGER_COL_LEN ? 1 : -1];

/* =========================================================================
 * Opaque device structure
 * ====================================================================== */

struct AoaDevice {
    libusb_context       *ctx;
    libusb_device_handle *handle;
    int                   interface_claimed;
    uint8_t              *hid_desc;
    int                   hid_desc_len;
};

/* =========================================================================
 * Internal helpers
 * ====================================================================== */

/* Vendor OUT control transfer on endpoint 0. */
static int ctrl_out(libusb_device_handle *h,
                    uint8_t req, uint16_t val, uint16_t idx,
                    uint8_t *data, uint16_t len)
{
    return libusb_control_transfer(h,
        LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_VENDOR |
        LIBUSB_RECIPIENT_DEVICE,
        req, val, idx, data, len, AOA_USB_TIMEOUT_MS);
}

/* Query AOA protocol version (AOA command 51).
 * Returns the version number (>= 1) on success, -1 on failure.
 * Version >= 2 is required for HID injection. */
static int aoa_get_protocol(libusb_device_handle *h)
{
    uint8_t buf[2] = {0, 0};
    int r = libusb_control_transfer(h,
        LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_VENDOR |
        LIBUSB_RECIPIENT_DEVICE,
        AOA_GET_PROTOCOL, 0, 0, buf, 2, AOA_USB_TIMEOUT_MS);
    if (r < 2) return -1;
    return (int)(buf[0] | ((unsigned)buf[1] << 8));
}

/* Returns 1 if the VID/PID pair indicates a device already in AOA accessory mode.
 * These devices skip GET_PROTOCOL probing since they are already switched. */
static int is_aoa_accessory_pid(uint16_t vid, uint16_t pid)
{
    return vid == AOA_VENDOR_ID &&
           (pid == AOA_PRODUCT_ID_ACC || pid == AOA_PRODUCT_ID_ACC_ADB);
}

/* Open `device`, check its AOA version, fill `info`, and return the handle.
 * Returns the open handle if the device supports AOA >= 2, NULL otherwise.
 * On failure the handle is always closed before returning. */
static libusb_device_handle *probe_and_open(libusb_device *device,
                                             AoaDeviceInfo *info)
{
    struct libusb_device_descriptor dd;
    if (libusb_get_device_descriptor(device, &dd) < 0) return NULL;

    libusb_device_handle *tmp = NULL;
    if (libusb_open(device, &tmp) < 0) return NULL;

    int proto = is_aoa_accessory_pid(dd.idVendor, dd.idProduct)
                    ? 2                       /* already in accessory mode  */
                    : aoa_get_protocol(tmp);  /* probe with GET_PROTOCOL    */

    if (proto < 2) {
        libusb_close(tmp);
        return NULL;
    }

    info->bus         = libusb_get_bus_number(device);
    info->address     = libusb_get_device_address(device);
    info->vid         = dd.idVendor;
    info->pid         = dd.idProduct;
    info->aoa_version = proto;
    return tmp;
}

/* Allocate and build a complete HID digitizer descriptor for `num_fingers`
 * touch slots with the given screen dimensions.
 * Total size: 8 + num_fingers*75 + 19 bytes (max 1227 for 16 fingers).
 * Returns a heap-allocated buffer and sets *out_len, or NULL on OOM. */
static uint8_t *build_hid_descriptor(int width, int height,
                                     int num_fingers, int *out_len)
{
    int total = 8 + num_fingers * FINGER_COL_LEN + 19;
    uint8_t *d = (uint8_t *)malloc((size_t)total);
    if (!d) return NULL;

    int pos      = 0;
    uint8_t x_lo = (uint8_t)((width  - 1) & 0xFF);
    uint8_t x_hi = (uint8_t)((width  - 1) >> 8);
    uint8_t y_lo = (uint8_t)((height - 1) & 0xFF);
    uint8_t y_hi = (uint8_t)((height - 1) >> 8);
    uint8_t mc   = (uint8_t)num_fingers;

    /* Application Collection header, Report ID 1 */
    d[pos++] = 0x05; d[pos++] = 0x0D;  /* Usage Page (Digitizers)   */
    d[pos++] = 0x09; d[pos++] = 0x04;  /* Usage (Touch Screen)      */
    d[pos++] = 0xA1; d[pos++] = 0x01;  /* Collection (Application)  */
    d[pos++] = 0x85; d[pos++] = 0x01;  /* Report ID 1               */

    /* One Logical Collection per finger slot */
    for (int f = 0; f < num_fingers; f++) {
        memcpy(d + pos, k_finger_col_template, FINGER_COL_LEN);
        /* Patch Contact ID maximum to (num_fingers - 1) */
        d[pos + OFF_CID_LOG_MAX]     = (uint8_t)(num_fingers - 1);
        /* Patch X Logical and Physical Maximum to (screen_width - 1) */
        d[pos + OFF_X_LOG_MAX]       = x_lo;
        d[pos + OFF_X_LOG_MAX + 1]   = x_hi;
        d[pos + OFF_X_PHY_MAX]       = x_lo;
        d[pos + OFF_X_PHY_MAX + 1]   = x_hi;
        /* Patch Y Logical and Physical Maximum to (screen_height - 1) */
        d[pos + OFF_Y_LOG_MAX]       = y_lo;
        d[pos + OFF_Y_LOG_MAX + 1]   = y_hi;
        d[pos + OFF_Y_PHY_MAX]       = y_lo;
        d[pos + OFF_Y_PHY_MAX + 1]   = y_hi;
        pos += FINGER_COL_LEN;
    }

    /* Contact Count input (Usage 0x54) — Report ID 1 continued */
    d[pos++] = 0x05; d[pos++] = 0x0D;  /* Usage Page (Digitizers)   */
    d[pos++] = 0x09; d[pos++] = 0x54;  /* Usage (Contact Count)     */
    d[pos++] = 0x15; d[pos++] = 0x00;  /* Logical Minimum 0         */
    d[pos++] = 0x25; d[pos++] = mc;    /* Logical Maximum           */
    d[pos++] = 0x75; d[pos++] = 0x08;  /* Report Size 8             */
    d[pos++] = 0x95; d[pos++] = 0x01;  /* Report Count 1            */
    d[pos++] = 0x81; d[pos++] = 0x02;  /* Input (Data, Var, Abs)    */

    /* Max Contacts feature (Usage 0x55) — Report ID 2 */
    d[pos++] = 0x85; d[pos++] = 0x02;  /* Report ID 2               */
    d[pos++] = 0x09; d[pos++] = 0x55;  /* Usage (Contact Count Max) */
    d[pos++] = 0x15; d[pos++] = 0x00;  /* Logical Minimum 0         */
    d[pos++] = 0x25; d[pos++] = mc;    /* Logical Maximum           */
    d[pos++] = 0x75; d[pos++] = 0x08;  /* Report Size 8             */
    d[pos++] = 0x95; d[pos++] = 0x01;  /* Report Count 1            */
    d[pos++] = 0xB1; d[pos++] = 0x02;  /* Feature (Data, Var, Abs)  */

    d[pos++] = 0xC0; /* End Application Collection */

    *out_len = pos;
    return d;
}

/* Upload the HID descriptor to Android in 64-byte chunks (AOA command 56).
 * wIndex carries the byte offset of each chunk.
 * Returns 0 on success, negative libusb error code on failure. */
static int send_hid_desc(libusb_device_handle *h,
                         const uint8_t *desc, int desc_len)
{
    int offset = 0;
    while (offset < desc_len) {
        int chunk = desc_len - offset;
        if (chunk > 64) chunk = 64;

        int r = libusb_control_transfer(h,
            LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_VENDOR |
            LIBUSB_RECIPIENT_DEVICE,
            AOA_SET_HID_REPORT_DESC,
            HID_DEVICE_ID, (uint16_t)offset,
            (uint8_t *)(desc + offset), (uint16_t)chunk,
            AOA_USB_TIMEOUT_MS);

        if (r < 0) {
            AOA_LOG("[libaoa] Descriptor upload failed at offset %d: %s\n",
                    offset, libusb_error_name(r));
            return r;
        }
        offset += chunk;
    }
    AOA_LOG("[libaoa] HID descriptor uploaded (%d bytes)\n", desc_len);
    return 0;
}

/* =========================================================================
 * Public API
 * ====================================================================== */

int aoa_hid_list_devices(AoaDeviceInfo *out, int out_max)
{
    if (!out || out_max <= 0) return 0;

    libusb_context *ctx = NULL;
    if (libusb_init(&ctx) < 0) {
        AOA_LOG("[libaoa] libusb_init failed\n");
        return -1;
    }

    libusb_device **devs = NULL;
    /*
     * libusb_get_device_list returns ssize_t. Cast to int because the number
     * of USB devices will never exceed INT_MAX in practice, and this avoids
     * ssize_t-related loop warnings on all platforms.
     */
    int cnt = (int)libusb_get_device_list(ctx, &devs);
    if (cnt < 0) {
        AOA_LOG("[libaoa] Failed to get USB device list\n");
        libusb_exit(ctx);
        return -1;
    }

    int found = 0;
    for (int i = 0; i < cnt && found < out_max; i++) {
        AoaDeviceInfo info;
        libusb_device_handle *tmp = probe_and_open(devs[i], &info);
        if (!tmp) continue;
        libusb_close(tmp);
        out[found] = info;
        AOA_LOG("[libaoa] [%d] Bus %03u Device %03u  "
                "VID=0x%04X PID=0x%04X  AOA v%d\n",
                found, info.bus, info.address,
                info.vid, info.pid, info.aoa_version);
        found++;
    }

    libusb_free_device_list(devs, 1);
    libusb_exit(ctx);

    if (found == 0)
        AOA_LOG("[libaoa] No AOA 2.0 devices found\n");

    return found;
}

AoaDevice *aoa_hid_connect_by_address(uint8_t bus, uint8_t address,
                                      int width, int height,
                                      int max_contacts)
{
    if (width  < 1 || width  > 65535 ||
        height < 1 || height > 65535 ||
        max_contacts < 1 || max_contacts > 16) {
        AOA_LOG("[libaoa] Invalid arguments: width=%d height=%d "
                "max_contacts=%d\n", width, height, max_contacts);
        return NULL;
    }

    AoaDevice *dev = (AoaDevice *)calloc(1, sizeof(AoaDevice));
    if (!dev) { AOA_LOG("[libaoa] Out of memory\n"); return NULL; }

    if (libusb_init(&dev->ctx) < 0) {
        AOA_LOG("[libaoa] libusb_init failed\n");
        free(dev);
        return NULL;
    }

    AOA_LOG("[libaoa] Looking for Bus %03u Device %03u...\n", bus, address);

    libusb_device **devs = NULL;
    int cnt = (int)libusb_get_device_list(dev->ctx, &devs);
    if (cnt < 0) {
        AOA_LOG("[libaoa] Failed to get USB device list\n");
        aoa_hid_disconnect(dev);
        return NULL;
    }

    libusb_device_handle *handle = NULL;
    for (int i = 0; i < cnt && !handle; i++) {
        if (libusb_get_bus_number(devs[i])     != bus)     continue;
        if (libusb_get_device_address(devs[i]) != address) continue;

        AoaDeviceInfo info;
        handle = probe_and_open(devs[i], &info);
        if (!handle) {
            AOA_LOG("[libaoa] Bus %03u Device %03u does not support AOA 2.0\n",
                    bus, address);
            break;
        }
        AOA_LOG("[libaoa] Opened: Bus %03u Device %03u  "
                "VID=0x%04X PID=0x%04X  AOA v%d\n",
                info.bus, info.address, info.vid, info.pid, info.aoa_version);
    }
    libusb_free_device_list(devs, 1);

    if (!handle) {
        aoa_hid_disconnect(dev);
        return NULL;
    }

    dev->handle = handle;

    dev->hid_desc = build_hid_descriptor(width, height,
                                         max_contacts, &dev->hid_desc_len);
    if (!dev->hid_desc) {
        AOA_LOG("[libaoa] Failed to build HID descriptor\n");
        goto fail;
    }

    /*
     * Claim interface — best-effort; ep0 transfers work without it.
     * libusb_set_auto_detach_kernel_driver returns LIBUSB_ERROR_NOT_SUPPORTED
     * on macOS and Windows, which is safe to ignore.
     */
    libusb_set_auto_detach_kernel_driver(dev->handle, 1);
    if (libusb_claim_interface(dev->handle, 0) == 0) {
        dev->interface_claimed = 1;
    } else {
        AOA_LOG("[libaoa] Interface claim failed — continuing in ep0-only mode\n");
    }

    /* AOA command 54: announce HID device and descriptor size */
    {
        int r = ctrl_out(dev->handle, AOA_REGISTER_HID, HID_DEVICE_ID,
                         (uint16_t)dev->hid_desc_len, NULL, 0);
        if (r < 0) {
            AOA_LOG("[libaoa] REGISTER_HID failed: %s\n", libusb_error_name(r));
            goto fail;
        }
        AOA_LOG("[libaoa] HID registered (id=%d, desc=%d bytes)\n",
                HID_DEVICE_ID, dev->hid_desc_len);
    }

    /* AOA command 56: upload the descriptor */
    if (send_hid_desc(dev->handle, dev->hid_desc, dev->hid_desc_len) < 0)
        goto fail;

    AOA_LOG("[libaoa] Ready (%dx%d, %d contacts). "
            "Sleep ~500 ms before first send.\n",
            width, height, max_contacts);

    return dev;

fail:
    aoa_hid_disconnect(dev);
    return NULL;
}

int aoa_hid_send_report(AoaDevice *dev, const HidReport *report)
{
    if (!dev || !report) return LIBUSB_ERROR_INVALID_PARAM;

    /* Copy to a local buffer — avoids a const-discarding cast to libusb. */
    uint8_t buf[sizeof(HidReport)];
    memcpy(buf, report, sizeof(HidReport));

    return libusb_control_transfer(dev->handle,
        LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_VENDOR |
        LIBUSB_RECIPIENT_DEVICE,
        AOA_SEND_HID_EVENT, HID_DEVICE_ID, 0,
        buf, (uint16_t)sizeof(HidReport),
        AOA_USB_TIMEOUT_MS);
}

int aoa_hid_error_is_fatal(int rc)
{
    return rc == LIBUSB_ERROR_NO_DEVICE  ||
           rc == LIBUSB_ERROR_NOT_FOUND  ||
           rc == LIBUSB_ERROR_ACCESS;
}

void aoa_hid_disconnect(AoaDevice *dev)
{
    if (!dev) return;

    if (dev->handle) {
        ctrl_out(dev->handle, AOA_UNREGISTER_HID, HID_DEVICE_ID, 0, NULL, 0);
        AOA_LOG("[libaoa] HID unregistered\n");

        if (dev->interface_claimed)
            libusb_release_interface(dev->handle, 0);

        libusb_close(dev->handle);
        dev->handle = NULL;
    }

    if (dev->ctx) {
        libusb_exit(dev->ctx);
        dev->ctx = NULL;
    }

    free(dev->hid_desc);
    free(dev);

    AOA_LOG("[libaoa] Disconnected\n");
}