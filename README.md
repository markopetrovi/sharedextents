# sharedextents
A program that checks what other files on the btrfs filesystem share extents with the file provided in argument, by scanning the extent tree. It is mostly C code, but compiles with C++ because of std::unordered_set and std::vector used in one function.
# Usage
`sharedextents <filename>`
# Dependencies
- btrfs-progs with `btrfs inspect-internal dump-tree` and `btrfs inspect-internal logical-resolve` being available in PATH.
- Linux kernel 6.10 or newer (becuase of the use of statx(2) STATX_SUBVOL; a fallback to ioctl will be added)
# Compiling
- Simple, with something like `g++ sharedextents.cpp -o sharedextents`
- To enable compiler optimizations, use something like `g++ -O3 -march=native -mtune=native sharedextents.cpp -o sharedextents`
