# Vajra-Neo

Custom kernel for Xiaomi **redwood** (POCO X5 Pro 5G).

`Linux 6.12.92` · `Android clang 19.0.1` · `arm64`

Built on Google's android16-6.12 common kernel, with one core change for suspend and every Qualcomm and Xiaomi driver as a vendor module.

## Features

- **CASS** task placement by Sultan Alsawaf, as a vendor module
- **MGLRU**, per-VMA locks, EEVDF, sched_ext
- **zram** with lz4 and zstd, EROFS, f2fs compression, io_uring
- **Clang CFI**, shadow call stack, pointer authentication

## Build

```
make ARCH=arm64 LLVM=1 LLVM_IAS=1 O=out redwood_defconfig
make ARCH=arm64 LLVM=1 LLVM_IAS=1 O=out Image modules dtbs
```

Clang and Rust versions are pinned in `build.config.constants`.

## Changelog

[vajra-neo-redwood-6.12.md](https://github.com/genie1997/vajra-kernel-changelog/blob/main/vajra-neo-redwood-6.12.md)

## License

GPL-2.0, see `COPYING`.
