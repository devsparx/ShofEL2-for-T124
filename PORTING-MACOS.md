# macOS / portable libusb-1.0 backend

This branch adds a portable USB I/O backend so `shofel2_t124` builds and runs
natively on macOS (and any other platform with libusb-1.0). The original
sysfs/usbfs backend is preserved unchanged and remains the default on Linux.

## What changed

Three small, surgical edits and one new file:

- **`exploit/mini_libusb_libusb.c`** *(new)* — implements the same four-function
  API (`usb_open_by_vid_pid`, `usb_close`, `usb_send_control_txn`,
  `usb_send_bulk_txn`) on top of libusb-1.0. The handle returned is a slot
  index into a static table; callers in `rcm.c` and `shofel2_t124.c` are
  unchanged.
- **`Makefile`** — adds a `BACKEND` variable (`sysfs` or `libusb`). Defaults
  to `sysfs` on Linux (upstream behaviour preserved) and `libusb` on Darwin.
  When `BACKEND=libusb`, the original `mini_libusb.c` is excluded from the
  build and `pkg-config libusb-1.0` flags are added.
- **`include/mini_libusb.h`** — gates `<linux/usbdevice_fs.h>` behind
  `#ifdef __linux__` so the header is includable on non-Linux hosts.
- **`include/endianness.h`** — uses the compiler-provided `__BYTE_ORDER__`
  macro (defined by both gcc and clang on Linux, macOS, and BSDs) instead
  of glibc's `__BYTE_ORDER` (which isn't defined on macOS). The legacy glibc
  fallback is kept for old toolchains.

## Building on macOS

```sh
brew install libusb arm-none-eabi-gcc
make
```

That's it. `make` will produce `shofel2_t124` plus all six ARM payloads
(`intermezzo.bin`, `boot_bct.bin`, `emmc_server.bin`,
`mem_dumper_usb_server.bin`, `reset_example.bin`, `jtag_example.bin`).

## Building on Linux (no change for upstream users)

```sh
make           # original sysfs backend, unchanged
```

To use libusb on Linux instead (e.g. to avoid running as root, or to run
inside a container that doesn't expose `/sys/bus/usb`):

```sh
make BACKEND=libusb
```

## Caveats

- **macOS USB permissions.** A regular CLI binary should be able to open the
  Tegra in RCM (vid:pid `0955:7740`) without special entitlements. If you hit
  `LIBUSB_ERROR_ACCESS`, no kext should be claiming this device — confirm
  with `system_profiler SPUSBDataType` after holding RCM+reset.
- **Exploit transport on macOS is untested in this commit.** The code builds
  and links cleanly, the libusb wrapper passes the malformed `wLength`
  through unchanged (libusb writes it directly into the setup packet), and
  fusée-launcher uses the same approach successfully on macOS for the
  related Switch exploit. But the actual round-trip through IOKit on a real
  Jibo has not been verified by me — would love a confirmation from anyone
  with hardware.
- **No new dependencies on Linux.** The default `BACKEND=sysfs` path doesn't
  link libusb at all.
