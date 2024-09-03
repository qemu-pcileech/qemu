# QEMU Fork for PCILeech 9.0.x
QEMU is a generic and open source machine & userspace emulator and virtualizer. \
This fork is intended for adding a virtual PCILeech device into the QEMU.

This branch is currently based on QEMU 9.0.2.

## PCILeech Device
[PCILeech](https://github.com/ufrisk/pcileech) is a PCI hardware device that uses DMA to read and write target system memory. This project aims to implement PCILeech device inside a QEMU guest. \
This will enable security researchers to practice DMA attacking and defending.

## Build
This chapter contains detailed information for building QEMU on all platforms.

You will need to clone QEMU repository regardless of your platform:
```
git clone https://github.com/qemu-pcileech/qemu.git
cd qemu
```

### Linux (Ubuntu LTS 24.04)
Update `apt` repository.
```
sudo apt update -y
```
Install prerequisites:
```
sudo apt install -y git libglib2.0-dev libfdt-dev libpixman-1-dev zlib1g-dev libbz2-dev liblzo2-dev ninja-build python3-pip meson ovmf libsdl2-dev libgtk-3-dev libvte-dev libspice-protocol-dev libspice-server-dev libslirp-dev libcapstone-dev python3-sphinx python3-sphinx-rtd-theme
```
Configure compilation arguments:
```
./configure --disable-werror --enable-kvm --enable-tools --enable-lzo --enable-bzip2 --enable-sdl --enable-gtk --enable-vdi --enable-qcow1 --enable-spice --enable-slirp --enable-capstone
```
Build QEMU:
```
make -j$(nproc)
```
Install into the system:
```
sudo make install
```

### Windows
Install [MSYS2](https://www.msys2.org/).

Update MSYS2 repository:
```
pacman -Syu
pacman -Su
```
Install prerequisites:
```
pacman -S base-devel mingw-w64-x86_64-toolchain git python ninja mingw-w64-x86_64-glib2 mingw-w64-x86_64-pixman python-setuptools mingw-w64-x86_64-gtk3 mingw-w64-x86_64-SDL2 mingw-w64-x86_64-libslirp mingw-w64-x86_64-libcapstone
```
Configure compilation arguments:
```
./configure --disable-werror --enable-whpx --enable-tools --enable-lzo --enable-bzip2 --enable-sdl --enable-gtk --enable-vdi --enable-qcow1 --enable-slirp --enable-capstone
```
Build QEMU:
```
make -j$(nproc)
```

## Run
The virtual PCILeech device relies on QEMU chardev backend. Thus, you have to create a chardev for PCILeech device.
```
qemu-system-x86_64 -device pcileech,chardev=pcileech -chardev socket,id=pcileech,wait=off,server=on,host=0.0.0.0,port=6789
```
Append more arguments (e.g.: `-accel kvm`) to fit your VM settings.

Then the virtual PCILeech device will be listening on 0.0.0.0:6789. Use [PCILeech software](https://github.com/ufrisk/pcileech/releases) with [QEMU-PCILeech plugin](https://github.com/ufrisk/LeechCore/releases):
```
pcileech -device qemupcileech://127.0.0.1:6789 display -min 0x3800000
```
Make sure you have placed the `leechcore_device_qemupcileech.[so|dll]` file alongside `leechcore.[dll|so]` before you run `pcileech`!

## Maintenance Schedule
This project will only update when a stable version of QEMU is released. The release-candidate (-rc) versions are ignored.

## License
See [LICENSE](./LICENSE) file.