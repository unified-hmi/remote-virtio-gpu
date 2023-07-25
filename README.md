
<picture>
<h1 align="center">
<img src="documentation/png/logo.png" width=60%>
</h1>
</picture>

**"Unified HMI" is a common platform technology for UX innovation in integrated cockpit by flexible information display on multiple displays of various applications. Applications can be rendered to any display via a unified virtual display.**
<picture>
<p align="center"><img src="documentation/png/cockpit.png" width=90% align="center"></p>
</picture>
<br>

# Remote Virtio GPU Device (RVGPU)

**RVGPU is a client-server based rendering engine, which allows to render 3D on one device (client) and display it via network on another device (server)**

<p align="center"><img src="documentation/png/glmark2-ubuntu.png" width=90% align="center"></p>

## Contents

- [Repository structure](#repository-structure)
- [Build instructions](#build-instructions)
- [HowTo run RVGPU locally](#how-to-run-rvgpu-locally)
- [HowTo run RVGPU remotely](#how-to-run-rvgpu-remotely)


## Repository structure

```
.
├── documentation                  # documents
├── include
│    ├── librvgpu                  # librvgpu header files
│    ├── rvgpu-generic             # common header files for all components
│    ├── rvgpu-proxy               # rvgpu-proxy header files
│    ├── rvgpu-renderer            # rvgpu-renderer header files
├── src
│    ├── librvgpu                  # librvgpu source files
│    ├── rvgpu-driver-linux        # rvgpu driver source files
│    ├── rvgpu-proxy               # rvgpu-proxy source files
│    ├── rvgpu-renderer            # rvgpu-renderer source files
│    ├── rvgpu-sanity              # sanity module source files.
```

## Build instructions

The build instructions described here are tested on Ubuntu 20.04 LTS AMD64.
However you can try it with different Linux distros with Wayland display
server. Assuming, you have a clean Ubuntu 20.04 installed, please perform the
following steps:

- Install build tools

  ```
  sudo apt install gcc g++ make cmake
  ```

- Install virglrenderer

  ```
  sudo apt install libvirglrenderer1 libvirglrenderer-dev
  ```

- Install Weston compositor

  ```
  sudo apt install libjpeg-dev libwebp-dev libsystemd-dev libpam-dev libva-dev freerdp2-dev \
                 libxcb-composite0-dev liblcms2-dev libcolord-dev libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev libpipewire-0.2-dev \
                 libxml2-dev meson libxkbcommon-x11-dev libpixman-1-dev libinput-dev libdrm-dev wayland-protocols libcairo2-dev \
                 libpango1.0-dev libdbus-1-dev libgbm-dev libxcursor-dev

  wget https://wayland.freedesktop.org/releases/weston-8.0.93.tar.xz
  tar -xf weston-8.0.93.tar.xz
  cd ~/weston-8.0.93/
  meson build/
  ninja -C build/ install
  ```

- Install remote-virtio-gpu

  ```
  cd ~/remote-virtio-gpu
  mkdir build
  cmake -B build -DCMAKE_BUILD_TYPE=Release -DVIRTIO_LO_DIR={$remote-virtio-gpu-driver}/src/rvgpu-driver-linux
  make -C build
  sudo make install -C build
  ```

- Install mesa 18.2.0 with virgl support to a separate /usr/lib/mesa-virtio folder

  ```
  sudo apt install llvm libwayland-egl-backend-dev libxcb-glx0-dev libx11-xcb-dev libxcb-dri2-0-dev libxcb-dri3-dev \
                 libxcb-present-dev libxshmfence-dev libgbm-dev \
                 libsdl2-dev libgtk-3-dev libgles2-mesa-dev libpixman-1-dev \
                 libtool autoconf libdrm-dev python libinput-dev glmark2-es2-wayland

  wget https://archive.mesa3d.org//mesa-18.2.0.tar.xz
  tar -xf mesa-18.2.0.tar.xz
  cd ~/mesa-18.2.0/
  patch -p1 < ../remote-virtio-gpu/documentation/patches/mesa-virtio/0001-glBufferData-Update-resource-backing-memory.patch
  ./configure --prefix=/usr/lib/mesa-virtio --exec_prefix=/usr/lib/mesa-virtio --libdir=/usr/lib/mesa-virtio \
            --includedir=/usr/include/mesa-virtio --sysconfdir=/etc/mesa-virtio --datadir=/usr/share/mesa-virtio \
            --with-dri-drivers=swrast --with-gallium-drivers=swrast,virgl --enable-dri3=yes --with-platforms=drm,wayland,x11 --disable-glx
  make
  sudo make install
  ```

## How to run RVGPU locally

To use **remote-virtio-gpu**, you need to load the kernel modules **virtio-gpu** and **virtio_lo**, so turn **Secure Boot** off.

**rvgpu-renderer** creates a Wayland backend to display the stream, rendered by **rvgpu-proxy**. Therefore on login screen, please choose [Wayland](https://linuxconfig.org/how-to-enable-disable-wayland-on-ubuntu-20-04-desktop).

Run both RVGPU client (**rvgpu-proxy**) and server (**rvgpu-renderer**) on the same machine via the localhost interface as follow:

- Launch rvgpu-renderer from user, you are currently logged in

  ```
  rvgpu-renderer -b 1280x720@0,0 -p 55667 &
  ```

- Open another terminal and switch to **root** user

  ```
  sudo su
  modprobe virtio-gpu
  modprobe virtio_lo

  mkdir -p /run/user/0
  export XDG_RUNTIME_DIR=/run/user/0
  rvgpu-proxy -s 1280x720@0,0 -n 127.0.0.1:55667 &

  export LD_LIBRARY_PATH=/usr/lib/mesa-virtio
  weston --backend drm-backend.so --tty=2 --seat=seat_virtual -i 0 &
  ```

After that **rvgpu-renderer** will display _weston_ rendered and transferred
via localhost by **rvgpu-proxy**. Now you can launch glmark2-es2-wayland or
some other graphical application to verify that everything works.

**Note**  
To support the VSYNC feature in rvgpu-proxy, apply and rebuild the Linux kernel with the following path remote-virtio-gpu/documentation/patches/kernel

Get the Linux kernel souce code, which must be compatible with your Linux distro, use the patch file corresponding with kernel version you get for performing build and generate the necessary deb files

Here, the build instructions described to support the VSYNC feature to the kernel version 5.8.18 for Ubuntu 20.04.

- Get the kernel version 5.8.18 for Ubuntu 20.04.

  ```
  cd /usr/src
  sudo apt-get install -y build-essential libncurses5-dev \
                        gcc libssl-dev grub2 bc bison flex libelf-dev
  sudo wget https://cdn.kernel.org/pub/linux/kernel/v5.x/linux-5.8.18.tar.xz
  sudo tar -xf linux-5.8.18.tar.xz
  cd linux-5.8.18
  patch -p1 < ~/remote-virtio-gpu/documentation/patches/kernel/0001-drm-virtio-Add-VSYNC-support-linux-5-8.patch
  cp /boot/config-5.8.0-50-generic .config # Other versions of config are acceptable
  vim .config # comment this line CONFIG_SYSTEM_TRUSTED_KEYS=""
  make menuconfig # save config
  make -j`nproc` deb-pkg # the -j`nproc` argument sets the build to use as many cpu's as you have
  ```
  
- After that, some deb packages should be under /usr/src directory:

  ```
  linux-headers-5.8.18_5.8.18-1_amd64.deb
  linux-image-5.8.18_5.8.18-1_amd64.deb
  linux-image-5.8.18-dbg_5.8.18-1_amd64.deb
  linux-libc-dev_5.8.18-1_amd64.deb
  ```
  
- Install packages:

  ```
  dpkg –i linux-*.deb
  update-grub
  ```
  
- If there are no problems, restart your PC
- Make sure the Kernel is updated after reboot. Check Kernel version:
  ```
  uname -r
   ```
   
## How to run RVGPU remotely

The way is almost the same as to run **RVGPU** locally. Launch
**rvgpu-renderer** on one machine and **rvgpu-proxy** with the kernel modules
on the another one, and pass the IP address and port number on which
rvgpu-renderer is listening to **rvgpu-proxy** through "-n" option.

**Note**  
Some graphical applications generate much network traffic. It is recommended to Configure the network to 1Gbps speed.

