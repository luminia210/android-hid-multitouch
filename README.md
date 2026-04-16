# libaoa_hid

**Version:** 1.0.0

A small, cross-platform C library for injecting multi-touch events into an
Android device over a USB cable, using the Android Open Accessory 2.0 protocol.

No root. No Developer options. No adb. No Android app. No Wi-Fi. No screen mirroring.

---

## What it does

libaoa_hid registers a virtual HID digitizer on Android over a plain USB cable
and streams raw touch reports to it. Up to 16 simultaneous touch points.

The library is a pure communication pipe:
- `aoa_hid_list_devices()` — enumerate connected AOA 2.0 devices
- `aoa_hid_connect_by_address()` — connect to a specific device by bus/address
- `aoa_hid_send_report()` — send one touch report
- `aoa_hid_error_is_fatal(rc)` — check whether an error means the device is gone
- `aoa_hid_disconnect()` — release everything

No sleep calls, no retry logic, no warm-up sends. Timing and error handling
are entirely the caller's responsibility.

---

## Requirements

| | |
|---|---|
| **Android** | Android 4.1 or later.  |
| **Host OS** | Linux, macOS, Windows (MinGW-w64) |
| **Compiler** | gcc or clang with C11 support |
| **libusb** | 1.0.x — see [Building](#building) |

---

## Repository structure

```
libaoa_hid_Library/
├── libaoa_hid.h              Public header
├── libaoa_hid.c              Implementation
├── example/
│   └── touch_example.c       Working example
├── libusb/
│   ├── linux_x64/
│   │   ├── include/libusb-1.0/
│   │   │   └── libusb.h      Bundled libusb header (Linux)
│   │   └── static/
│   │       └── libusb-1.0.a  Bundled Linux x64 static library
│   ├── windows_x64/
│   │   ├── include/libusb-1.0/
│   │   │   └── libusb.h      Bundled libusb header (Windows)
│   │   └── static/
│   │       └── libusb-1.0.a  Bundled Windows x64 static library (MinGW-w64)
│   └── LICENSE_libusb.txt    libusb license (LGPL 2.1)
├── LICENSE               libaoa_hid license (MIT)
├── CONTRIBUTING.md
└── README.md
```

---

## Building

### Linux — bundled static libusb

```sh
gcc -O3 -Wall -Wextra -std=c11 \
    -I./libusb/linux_x64/include/libusb-1.0 \
    myapp.c libaoa_hid.c \
    ./libusb/linux_x64/static/libusb-1.0.a \
    -o myapp
```

### Linux — system libusb

```sh
sudo apt install libusb-1.0-0-dev
gcc -O3 -Wall -Wextra -std=c11 \
    myapp.c libaoa_hid.c -lusb-1.0 -o myapp
```

### macOS

```sh
brew install libusb
clang -O3 -Wall -Wextra -std=c11 \
    -I$(brew --prefix libusb)/include/libusb-1.0 \
    -L$(brew --prefix libusb)/lib \
    myapp.c libaoa_hid.c -lusb-1.0 -o myapp
```

### Windows (MinGW-w64) — bundled static libusb

```bat
gcc -O3 -Wall -Wextra -std=c11 ^
    -I./libusb/windows_x64/include/libusb-1.0 ^
    -L./libusb/windows_x64/static ^
    myapp.c libaoa_hid.c ^
    -lusb-1.0 -lsetupapi -lole32 -luuid ^
    -static-libgcc -static -s ^
    -o myapp.exe
```

> **WinUSB driver required.** Use [Zadig](https://zadig.akeo.ie) to install
> WinUSB for your Android device before running the built binary.
> Open Zadig → Options → List All Devices → select your Android device →
> install **WinUSB**.

---

## Quick start

```c
#include "libaoa_hid.h"
#include <string.h>

/* Portable sleep helper — replace with your platform's equivalent,
   or copy this snippet directly from example/touch_example.c. */
#ifdef _WIN32
#  include <windows.h>
static void sleep_ms(unsigned ms) { Sleep(ms); }
#else
#  define _POSIX_C_SOURCE 199309L
#  include <time.h>
static void sleep_ms(unsigned ms) {
    struct timespec ts = { (time_t)(ms / 1000),
                           (long)((ms % 1000) * 1000000L) };
    nanosleep(&ts, NULL);
}
#endif

int main(void) {
    // List devices, then connect to a specific one by bus/address.
    AoaDeviceInfo list[8];
    int n = aoa_hid_list_devices(list, 8);
    if (n <= 0) return 1;

    AoaDevice *dev = aoa_hid_connect_by_address(
        list[0].bus, list[0].address, 1600, 2560, 10);
    if (!dev) return 1;

    // Android needs ~500 ms to register the virtual HID device.
    sleep_ms(500);

    HidReport r;
    memset(&r, 0, sizeof(r));
    // memset is required: unused slots must have tip_inrange_pad = 0
    // (AOA_FINGER_UP). Android treats any non-zero slot as an active touch.
    r.report_id                 = 1;
    r.finger[0].tip_inrange_pad = AOA_FINGER_DOWN;
    r.finger[0].contact_id      = 0;
    r.finger[0].x               = 400;  // plain decimal, no conversion needed
    r.finger[0].y               = 800;
    r.contact_count             = 1;
    aoa_hid_send_report(dev, &r);

    r.finger[0].tip_inrange_pad = AOA_FINGER_UP;
    r.contact_count             = 0;
    aoa_hid_send_report(dev, &r);

    aoa_hid_disconnect(dev);
    return 0;
}
```

---

## ADB integration — automatic screen resolution detection

> **This section is entirely optional.**
> The core library requires no adb, no Developer options, and no USB debugging.
> If you are hardcoding the screen dimensions, skip this section.

`aoa_hid_connect_by_address()` requires the screen width and height in pixels.
Hardcoding them works fine for a single device, but breaks across different
models. The pattern below uses `adb shell wm size` to read the resolution
automatically before connecting.

### Prerequisites

- `adb` on PATH — download [Android SDK Platform Tools](https://developer.android.com/studio/releases/platform-tools).
- USB debugging enabled on the Android device:
  Settings → About phone → tap Build number 7 times → Developer options → USB debugging → ON.
- Tap **Allow USB debugging** on the dialog that appears when you connect the cable.

### Why kill the ADB server before connecting?

ADB and libusb cannot share the same USB device simultaneously. If the ADB
server is running when `aoa_hid_connect_by_address()` is called, libusb will
fail to open the device. Killing the server first releases the USB handle.
After your program exits, restart it with `adb start-server` if needed.

### Pattern

```c
/* 1. Read resolution via adb (starts the server automatically). */
int w = 1600, h = 2560;  /* fallback */
adb_get_resolution(&w, &h);

/* 2. Kill the ADB server so libusb can claim the USB device.
      On Windows use "adb kill-server 2>nul",
      on Linux/macOS use "adb kill-server 2>/dev/null".
      Then wait ~1200 ms for the OS to release the USB handle. */
adb_kill();

/* 3. Connect and use libaoa_hid as usual. */
AoaDeviceInfo list[8];
int n = aoa_hid_list_devices(list, 8);
AoaDevice *dev = aoa_hid_connect_by_address(
    list[0].bus, list[0].address, w, h, 16);
sleep_ms(500);
/* ... send touch reports ... */
aoa_hid_disconnect(dev);
```

`adb_get_resolution()` calls `adb shell wm size` and prefers "Override size"
over "Physical size". Both helpers are simple wrappers: `adb_get_resolution`
uses `popen("adb shell wm size ...")` and `adb_kill` calls
`system("adb kill-server ...")` followed by a ~1200 ms sleep.

---

## Log suppression

```c
#define AOA_HID_SILENT
#include "libaoa_hid.h"
```

Or: `gcc -DAOA_HID_SILENT ...`

---

## USB timeout

The default transfer timeout is 1000 ms. Override it before including the
header, or with a compiler flag:

```c
#define AOA_USB_TIMEOUT_MS  2000   // 2 s
#include "libaoa_hid.h"
```

```sh
gcc -DAOA_USB_TIMEOUT_MS=2000 ...
```

Set to `0` for no timeout (unlimited wait).

---

## API reference

### `aoa_hid_list_devices(out, out_max)`

Scans all USB devices and fills `out` with info about each one that supports
AOA 2.0. Prints each found device to stderr. Returns the number found, or -1
on libusb failure.

### `aoa_hid_connect_by_address(bus, address, width, height, max_contacts)`

Connects to the device at the given USB bus number and device address.
Builds and uploads a HID digitizer descriptor for the given screen dimensions.
Obtain bus/address from `aoa_hid_list_devices()`. Returns `AoaDevice*` on
success, `NULL` on failure.

### `aoa_hid_send_report(dev, report)`

Sends one 98-byte `HidReport` over USB (AOA command 57). One
`libusb_control_transfer()` call — no retry, no sleep. The report struct is
copied to a stack buffer, so it may be reused or freed immediately after this
call returns. Returns `>= 0` on success, or a negative error code on failure.

### `aoa_hid_error_is_fatal(rc)`

Returns `1` if the error code means the device is gone and retrying will never
succeed. Returns `0` if the error is transient. Use this inside a send loop to
decide whether to abort immediately:

```c
int rc = aoa_hid_send_report(dev, &r);
if (rc < 0 && aoa_hid_error_is_fatal(rc)) {
    // device unplugged — stop immediately
}
```

### `aoa_hid_disconnect(dev)`

Unregisters the HID device, releases USB resources, frees memory. `NULL` is
safe.

---

## HidReport layout

```
Byte  0      report_id         Always 1
Bytes 1–96   finger[0..15]     16 × FingerData (6 bytes each)
  +0  tip_inrange_pad          AOA_FINGER_DOWN (0x03) or AOA_FINGER_UP (0x00)
  +1  contact_id               Slot index 0–(max_contacts−1)
  +2  x lo                     X coordinate, little-endian uint16
  +3  x hi
  +4  y lo                     Y coordinate, little-endian uint16
  +5  y hi
Byte 97      contact_count     See rules below
```

`contact_count` rules:
- `0` when all fingers are up.
- `(highest slot index used in this touch sequence) + 1` while any finger is
  down. This is **not** a simple count of active fingers.
  Example: slots 0 and 2 are touching → `contact_count` must be `3`.

### Coordinates

Assign pixel coordinates as plain integers — no hex conversion needed:

```c
r.finger[0].x = 960;
r.finger[0].y = 540;
```

The struct uses `uint16_t` with `#pragma pack(1)`. On x86 and ARM the compiler
lays out the bytes in little-endian order automatically, matching the HID
descriptor. `aoa_hid_send_report()` copies the struct verbatim with a single
`memcpy` — no arithmetic, no byte-swapping.

Coordinate assignment costs ~0.3 ns. A USB control transfer costs 0.5–2 ms.
The ratio is roughly 1:1,500,000 — coordinate math never contributes to
latency in any measurable way.

---

## Troubleshooting

### `aoa_hid_connect_by_address` returns NULL

1. Check stderr output (remove `AOA_HID_SILENT` if set).
2. **Windows only:** use [Zadig](https://zadig.akeo.ie) → Options → List All
   Devices → select your Android device → install **WinUSB**.

### Touch events not registering

- Wait at least 500 ms after `aoa_hid_connect_by_address` before the first send.
- Verify `contact_count` follows the rules above.

---

## Known limitations

- **Android only.** iOS is not supported.
- **Android 4.1+** (API level 16).
- **USB cable required.**

---

## License

MIT — see [LICENSE](LICENSE).

## Dependencies

[libusb-1.0](https://libusb.info) — LGPL 2.1
- Linux x64 static library bundled in `libusb/linux_x64/static/`.
- Windows x64 static library bundled in `libusb/windows_x64/static/`.
- macOS: `brew install libusb`.

---

## Source

https://github.com/luminia210/android-hid-multitouch

---

## Acknowledgements

Development of this library was assisted by [Claude](https://claude.ai) (Anthropic).