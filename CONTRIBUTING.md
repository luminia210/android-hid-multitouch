# Contributing

## Scope

libaoa_hid is intentionally minimal — a pure communication pipe between a host
PC and an Android device.

**In scope:**
- Bug fixes (incorrect AOA commands, descriptor errors, memory leaks)
- Portability fixes for untested compilers or platforms
- Documentation improvements

**Out of scope:**
- Sleep, retry, or warm-up logic (these belong in the caller)
- Higher-level abstractions such as gesture helpers or key mappers
- Dependencies beyond libusb and the C standard library

## Submitting a change

1. Fork the repository and create a descriptive branch.
2. Keep commits focused on one change each.
3. Confirm the code compiles cleanly:
   ```sh
   gcc -Wall -Wextra -std=c11 -fsyntax-only \
       -I./libusb/linux_x64/include/libusb-1.0 libaoa_hid.c
   gcc -Wall -Wextra -std=c11 -DAOA_HID_SILENT -fsyntax-only \
       -I./libusb/linux_x64/include/libusb-1.0 libaoa_hid.c
   ```
   Both commands must produce zero errors and zero warnings.
4. Open a pull request with a clear description of what changed and why.

## Reporting a bug

Open an issue and include:
- OS, compiler version, libusb version
- Android device model and Android version
- A minimal code snippet that reproduces the problem
- The full `[libaoa]` stderr log (remove `AOA_HID_SILENT` if set)

## Code style

- C11, no OS-specific headers in library files
- 4-space indentation, no tabs
- Comments in English
- Do not break the existing public API