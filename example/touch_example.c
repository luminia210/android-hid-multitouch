/*
 * touch_example.c — libaoa_hid usage example
 *
 * Connects to an Android device and performs a single tap-and-release
 * at the centre of a 1600 x 2560 screen.
 *
 * Build (Linux / macOS):
 *   gcc -O2 -std=c11 touch_example.c ../libaoa_hid.c -lusb-1.0 -o touch_example
 *
 * Build (Windows, MinGW-w64):
 *   gcc -O2 -std=c11 touch_example.c ../libaoa_hid.c \
 *       -I../libusb/windows_x64/include/libusb-1.0 \
 *       -L../libusb/windows_x64/static \
 *       -lusb-1.0 -lsetupapi -lole32 -luuid \
 *       -static-libgcc -static \
 *       -o touch_example.exe
 */

#define _POSIX_C_SOURCE 199309L

#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#  include <windows.h>
static void sleep_ms(unsigned ms) { Sleep(ms); }
#else
#  include <time.h>
static void sleep_ms(unsigned ms) {
    struct timespec ts = { (time_t)(ms / 1000),
                           (long)((ms % 1000) * 1000000L) };
    nanosleep(&ts, NULL);
}
#endif

#include "../libaoa_hid.h"

#define SCREEN_W  1600
#define SCREEN_H  2560

int main(void)
{
    /* ── Connect ── */
    AoaDeviceInfo list[8];
    int n = aoa_hid_list_devices(list, 8);
    if (n <= 0) {
        fprintf(stderr, "No AOA 2.0 device found. See messages above.\n");
        return 1;
    }

    AoaDevice *dev = aoa_hid_connect_by_address(
        list[0].bus, list[0].address, SCREEN_W, SCREEN_H, 10);
    if (!dev) {
        fprintf(stderr, "Connection failed. See messages above.\n");
        return 1;
    }

    /* ── Wait for Android to recognise the HID device ── */
    fprintf(stderr, "Waiting for Android HID enumeration (~500 ms)...\n");
    sleep_ms(500);

    /* ── Prepare a blank report ── */
    HidReport r;
    memset(&r, 0, sizeof(r));
    r.report_id = 1;

    /* ── Press finger 0 at the centre of the screen ── */
    fprintf(stderr, "Tap at (%d, %d)\n", SCREEN_W / 2, SCREEN_H / 2);

    r.finger[0].tip_inrange_pad = AOA_FINGER_DOWN;
    r.finger[0].contact_id      = 0;
    r.finger[0].x               = (uint16_t)(SCREEN_W / 2);
    r.finger[0].y               = (uint16_t)(SCREEN_H / 2);
    r.contact_count             = 1;

    int rc = aoa_hid_send_report(dev, &r);
    if (rc < 0) {
        fprintf(stderr, "Send failed (rc=%d). Cable connected?\n", rc);
        aoa_hid_disconnect(dev);
        return 1;
    }

    sleep_ms(50); /* Hold the touch for 50 ms */

    /* ── Lift finger 0 ── */
    r.finger[0].tip_inrange_pad = AOA_FINGER_UP;
    r.contact_count             = 0;
    aoa_hid_send_report(dev, &r);

    fprintf(stderr, "Done.\n");

    /* ── Clean up ── */
    aoa_hid_disconnect(dev);
    return 0;
}
