/*
 * libaoa_hid.h — AOA 2.0 Multi-Touch HID Library
 *
 * Injects up to 16-point multi-touch events into an Android device over USB
 * using the Android Open Accessory 2.0 HID-only protocol.
 *
 * SPDX-License-Identifier: MIT
 *
 * =========================================================================
 * Design principles
 * =========================================================================
 *
 *  No time dependencies
 *    No sleep, no retry, no warm-up logic. Timing is the caller's job.
 *
 *  No platform-specific headers
 *    Only <stdint.h> included here. Compiles unchanged on Linux, macOS,
 *    and Windows (MinGW-w64).
 *
 *  Zero-overhead send path
 *    aoa_hid_send_report() makes exactly one libusb_control_transfer() call.
 *    No internal state, no arithmetic. Raw return value forwarded to caller.
 *
 *  Opaque handle
 *    All libusb internals are hidden behind AoaDevice*.
 *    Callers do not need to include <libusb.h>.
 *
 * =========================================================================
 * HidReport memory layout — 98 bytes, tightly packed
 * =========================================================================
 *
 *  Byte  0       report_id           Always 1
 *  Bytes 1–96    finger[0..15]   16 × FingerData (6 bytes each)
 *    +0  tip_inrange_pad             AOA_FINGER_DOWN (0x03) or AOA_FINGER_UP (0x00)
 *    +1  contact_id                  Slot index, 0 to (max_contacts − 1)
 *    +2  x lo                        X coordinate, little-endian uint16
 *    +3  x hi
 *    +4  y lo                        Y coordinate, little-endian uint16
 *    +5  y hi
 *  Byte 97       contact_count       See rules below
 *
 *  contact_count rules (required by Android's HID driver):
 *    0 when all fingers are up.
 *    (highest slot index used in this touch sequence) + 1 while any finger
 *    is down. This is NOT the number of active fingers.
 *    Example: slots 0 and 2 are touching → contact_count must be 3.
 *
 * =========================================================================
 * Log suppression
 * =========================================================================
 *
 *  Define AOA_HID_SILENT before including this header to suppress all
 *  stderr output from the library:
 *
 *    #define AOA_HID_SILENT
 *    #include "libaoa_hid.h"
 *
 *  Or pass -DAOA_HID_SILENT as a compiler flag.
 *
 * =========================================================================
 * USB timeout
 * =========================================================================
 *
 *  AOA_USB_TIMEOUT_MS controls the timeout passed to every libusb control
 *  transfer. Define it before including this header to override the default:
 *
 *    #define AOA_USB_TIMEOUT_MS  2000   // 2 s
 *    #include "libaoa_hid.h"
 *
 *  Or pass -DAOA_USB_TIMEOUT_MS=2000 as a compiler flag.
 *  Set to 0 for no timeout (unlimited wait).
 *
 * =========================================================================
 * Quick start
 * =========================================================================
 *
 *  // List devices to get bus/address, then connect to a specific one.
 *  AoaDeviceInfo list[8];
 *  int n = aoa_hid_list_devices(list, 8);
 *  AoaDevice *dev = aoa_hid_connect_by_address(
 *                       list[0].bus, list[0].address, 1600, 2560, 10);
 *
 *  if (!dev) return 1;
 *  your_sleep_ms(500);  // Android needs ~500 ms to enumerate the HID device
 *
 *  HidReport r;
 *  memset(&r, 0, sizeof(r));
 *  // memset is required: unused finger slots must have tip_inrange_pad = 0
 *  // (AOA_FINGER_UP). Android treats any non-zero slot as an active touch.
 *  r.report_id                 = 1;
 *  r.finger[0].tip_inrange_pad = AOA_FINGER_DOWN;
 *  r.finger[0].contact_id      = 0;
 *  r.finger[0].x               = 400;
 *  r.finger[0].y               = 800;
 *  r.contact_count             = 1;
 *  aoa_hid_send_report(dev, &r);
 *
 *  r.finger[0].tip_inrange_pad = AOA_FINGER_UP;
 *  r.contact_count             = 0;
 *  aoa_hid_send_report(dev, &r);
 *
 *  aoa_hid_disconnect(dev);
 *
 * =========================================================================
 * Multi-finger example
 * =========================================================================
 *
 *  // Two fingers down at the same time.
 *  // contact_count = highest slot index used + 1, NOT the number of fingers.
 *
 *  HidReport r;
 *  memset(&r, 0, sizeof(r));
 *  r.report_id = 1;
 *
 *  r.finger[0].tip_inrange_pad = AOA_FINGER_DOWN;
 *  r.finger[0].contact_id      = 0;
 *  r.finger[0].x               = 300;
 *  r.finger[0].y               = 500;
 *
 *  r.finger[1].tip_inrange_pad = AOA_FINGER_DOWN;
 *  r.finger[1].contact_id      = 1;
 *  r.finger[1].x               = 700;
 *  r.finger[1].y               = 500;
 *
 *  r.contact_count = 2;  // highest slot (1) + 1
 *  aoa_hid_send_report(dev, &r);
 *
 *  // Lift finger 0, keep finger 1 down.
 *  // contact_count stays 2 because slot 1 is still the highest used slot.
 *  r.finger[0].tip_inrange_pad = AOA_FINGER_UP;
 *  r.contact_count             = 2;
 *  aoa_hid_send_report(dev, &r);
 *
 *  // Lift finger 1 — all fingers up.
 *  r.finger[1].tip_inrange_pad = AOA_FINGER_UP;
 *  r.contact_count             = 0;
 *  aoa_hid_send_report(dev, &r);
 *
 * =========================================================================
 * Coordinates and performance
 * =========================================================================
 *
 *  Passing coordinates
 *    x and y are plain pixel values. Just assign them as decimal integers —
 *    no hex conversion needed:
 *
 *      r.finger[0].x = 960;   // pixels, decimal is fine
 *      r.finger[0].y = 540;
 *
 *    The struct uses uint16_t with #pragma pack(1). On x86 and ARM (all
 *    practical targets for this library), the compiler lays the two bytes
 *    out in little-endian order automatically, which is exactly what the
 *    HID descriptor declares. aoa_hid_send_report() copies the struct
 *    verbatim into the USB buffer with a single memcpy — no arithmetic,
 *    no byte-swapping, no per-field loops.
 *
 *  Where the time actually goes
 *    Assigning x and y costs roughly 0.3 ns per call on a modern CPU.
 *    A USB control transfer (AOA command 57) takes 500,000–2,000,000 ns
 *    (0.5–2 ms) depending on the host controller and Android device.
 *    The coordinate assignment is about 1/1,500,000 of the USB cost —
 *    it will never appear in a profile.
 *
 *  Reducing overhead for fixed patterns (macros, key mappings, recordings)
 *    If the coordinates are known before the send loop starts, build the
 *    HidReport structs once at startup and reuse them. The send loop then
 *    becomes a single function call with no computation at all:
 *
 *      // --- build once at startup ---
 *      HidReport tap_down, tap_up;
 *      memset(&tap_down, 0, sizeof(tap_down));
 *      tap_down.report_id                 = 1;
 *      tap_down.finger[0].tip_inrange_pad = AOA_FINGER_DOWN;
 *      tap_down.finger[0].contact_id      = 0;
 *      tap_down.finger[0].x               = 960;
 *      tap_down.finger[0].y               = 540;
 *      tap_down.contact_count             = 1;
 *
 *      memset(&tap_up, 0, sizeof(tap_up));
 *      tap_up.report_id                   = 1;
 *      tap_up.contact_count               = 0;
 *
 *      // --- hot loop: zero computation per iteration ---
 *      aoa_hid_send_report(dev, &tap_down);
 *      your_sleep_ms(50);
 *      aoa_hid_send_report(dev, &tap_up);
 *
 *  Dynamic coordinates (CSV playback, real-time tracking)
 *    Update only the fields that change each frame and reuse the rest.
 *    The per-frame cost of writing x and y (~0.3 ns) is immeasurable
 *    compared to the USB round-trip, so no special optimisation is needed.
 */

#ifndef LIBAOA_HID_H
#define LIBAOA_HID_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------
 * USB transfer timeout
 *
 * Applied to every libusb_control_transfer() call in this library.
 * Can be overridden by the caller before including this header, or by
 * passing -DAOA_USB_TIMEOUT_MS=<value> on the compiler command line.
 * Set to 0 for no timeout (unlimited wait).
 * ---------------------------------------------------------------------- */
#ifndef AOA_USB_TIMEOUT_MS
#  define AOA_USB_TIMEOUT_MS  1000
#endif

/* -------------------------------------------------------------------------
 * Finger state constants
 * ---------------------------------------------------------------------- */

/** Finger touching the screen: TipSwitch = 1, InRange = 1. */
#define AOA_FINGER_DOWN  ((uint8_t)0x03)

/** Finger lifted from the screen: TipSwitch = 0, InRange = 0. */
#define AOA_FINGER_UP    ((uint8_t)0x00)

/*
 * tip_inrange_pad bit layout (matches the HID descriptor):
 *   bit 0  — TipSwitch : 1 = finger is touching the screen
 *   bit 1  — InRange   : 1 = finger is detected (touching or hovering)
 *   bit 2-7— padding   : always 0 (6 bits, makes the field one full byte)
 *
 * AOA_FINGER_DOWN (0x03) sets both TipSwitch and InRange.
 * AOA_FINGER_UP   (0x00) clears both — use this to lift a finger.
 * Do not use any other value.
 */

/* -------------------------------------------------------------------------
 * HID report structures
 * ---------------------------------------------------------------------- */

/**
 * One touch slot. Layout is fixed by the HID descriptor — do not reorder.
 */
#pragma pack(push, 1)
typedef struct {
    uint8_t  tip_inrange_pad; /**< AOA_FINGER_DOWN or AOA_FINGER_UP        */
    uint8_t  contact_id;      /**< Slot index, 0 to (max_contacts - 1)     */
    uint16_t x;               /**< X coordinate in pixels — plain decimal fine */
    uint16_t y;               /**< Y coordinate in pixels — plain decimal fine */
} FingerData;

/**
 * 98-byte report sent verbatim to Android.
 * Always set report_id = 1. Zero-initialise unused finger slots.
 */
typedef struct {
    uint8_t    report_id;     /**< Always 1                                 */
    FingerData finger[16];    /**< All 16 touch slots                       */
    uint8_t    contact_count; /**< See contact_count rules in the header    */
} HidReport;
#pragma pack(pop)

/* Compile-time size guards. */
typedef char _aoa_chk_finger[sizeof(FingerData) == 6  ? 1 : -1];
typedef char _aoa_chk_report[sizeof(HidReport)  == 98 ? 1 : -1];

/* -------------------------------------------------------------------------
 * Device enumeration
 * ---------------------------------------------------------------------- */

/**
 * Info about one AOA 2.0 capable USB device, returned by aoa_hid_list_devices().
 */
typedef struct {
    uint8_t  bus;         /**< USB bus number                               */
    uint8_t  address;     /**< USB device address                           */
    uint16_t vid;         /**< USB Vendor ID                                */
    uint16_t pid;         /**< USB Product ID                               */
    int      aoa_version; /**< AOA protocol version reported by the device  */
} AoaDeviceInfo;

/* -------------------------------------------------------------------------
 * Opaque device handle
 * ---------------------------------------------------------------------- */

/** Returned by aoa_hid_connect_by_address(). Treat as opaque. */
typedef struct AoaDevice AoaDevice;

/* -------------------------------------------------------------------------
 * API
 * ---------------------------------------------------------------------- */

/**
 * aoa_hid_list_devices() — enumerate all AOA 2.0 capable USB devices.
 *
 * Use this to find bus/address values before calling
 * aoa_hid_connect_by_address(). Each found device is logged to stderr.
 *
 * @param out      Caller-allocated array to receive results.
 * @param out_max  Capacity of `out`.
 * @return  Number of devices found (0 = none), -1 on libusb init failure.
 */
int aoa_hid_list_devices(AoaDeviceInfo *out, int out_max);

/**
 * aoa_hid_connect_by_address() — connect to a specific USB device.
 *
 * Registers a HID digitizer on Android (AOA commands 54 + 56).
 * Does not sleep — wait ~500 ms after this call before the first send.
 * Obtain bus/address from aoa_hid_list_devices() first.
 *
 * @param bus          USB bus number.
 * @param address      USB device address.
 * @param width        Screen width  in pixels (1–65535).
 * @param height       Screen height in pixels (1–65535).
 * @param max_contacts Touch slots (1–16).
 * @return  AoaDevice* on success, NULL if not found or not AOA 2.0.
 */
AoaDevice *aoa_hid_connect_by_address(uint8_t bus, uint8_t address,
                                      int width, int height,
                                      int max_contacts);

/**
 * aoa_hid_send_report() — send one HID input report (AOA command 57).
 *
 * Exactly one libusb_control_transfer() call. No retry, no sleep.
 * `report` is copied to a stack buffer — the caller's struct is not modified
 * and may be reused or freed immediately after this call returns.
 *
 * @param dev     Handle from aoa_hid_connect_by_address(). Must not be NULL.
 * @param report  Populated HidReport. Must not be NULL.
 * @return  >= 0 bytes transferred on success.
 * @return  Negative error code if dev or report is NULL.
 * @return  Negative error code on USB failure.
 */
int aoa_hid_send_report(AoaDevice *dev, const HidReport *report);

/**
 * aoa_hid_error_is_fatal() — check whether an error means the device is gone.
 *
 * Returns 1 if the device is disconnected and retrying will never succeed.
 * Returns 0 if the error is transient and a retry may work.
 *
 * Use this to decide whether to abort a send loop:
 *
 *   int rc = aoa_hid_send_report(dev, &r);
 *   if (rc < 0 && aoa_hid_error_is_fatal(rc)) {
 *       // device unplugged — stop immediately
 *   }
 *
 * @param rc  Return value from aoa_hid_send_report().
 * @return    1 = device gone (fatal), 0 = transient error.
 */
int aoa_hid_error_is_fatal(int rc);

/**
 * aoa_hid_disconnect() — unregister HID, release USB, free memory.
 *
 * NULL is a safe no-op. The pointer is invalid after this call.
 */
void aoa_hid_disconnect(AoaDevice *dev);

#ifdef __cplusplus
}
#endif

#endif /* LIBAOA_HID_H */