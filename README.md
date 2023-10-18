# Remote VIRTIO GPU (RVGPU)

> Remote VIRTIO GPU (RVGPU) is a client-server based rendering engine,
> which allows to render 3D on one device (client) and display it via network
> on another device (server)

## Contents

- [Remote VIRTIO GPU (RVGPU)](#remote-virtio-gpu-rvgpu)
  - [Contents](#contents)
  - [Repository structure](#repository-structure)
  - [How to install RVGPU](#how-to-install-rvgpu)
    - [Building from Source](#building-from-source)
    - [Binary-only Install](#binary-only-install)
  - [How to install Virtio-loopback-driver](#how-to-install-virtio-loopback-driver)
  - [How to Install Weston](#how-to-install-weston)
  - [Run RVGPU](#run-rvgpu)
    - [Run `rvgpu-renderer` on Wayland](#run-rvgpu-renderer-on-wayland)
    - [Run `rvgpu-proxy`](#run-rvgpu-proxy)
  - [VSYNC feature](#vsync-feature)
  - [How to run RVGPU remotely](#how-to-run-rvgpu-remotely)
  - [Capsets](#capsets)

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
│    ├── rvgpu-proxy               # rvgpu-proxy source files
│    ├── rvgpu-renderer            # rvgpu-renderer source files
│    ├── rvgpu-sanity              # sanity module source files
```
# How to Install RVGPU

The install instructions described here are tested on Ubuntu 20.04 LTS AMD64.
However you can try it with different Linux distros with Wayland display
server. Assuming, you have a clean Ubuntu 20.04 installed, perform the
following steps.  
You have two options for installing RVGPU: either build from source code ([Building from Source](#building-from-source)) or install from a binary ([Binary-only Install](#binary-only-install)).

## Building from Source

- Install the build prerequisites

  ```
  sudo apt install cmake pkg-config libvirglrenderer-dev libegl-dev libgles-dev libwayland-dev libgbm-dev libdrm-dev libinput-dev
  ```

- Install the header for `virtio-loopback-driver`.  

  Download the `virtio-lo-dev_X.X.X.deb` development ubuntu package from the latest release builds from its github repository and install it.  
  Specify the correct version of `X.X.X` by referring to https://github.com/unified-hmi/virtio-loopback-driver/releases/latest  
  For example: `virtio-lo-dev_1.0.0.deb`.  

  ```
  wget https://github.com/unified-hmi/virtio-loopback-driver/releases/latest/download/virtio-lo-dev_X.X.X.deb
  sudo dpkg -i virtio-lo-dev_X.X.X.deb
  ```
  
  Alternatively you can just copy the header to `/usr/include/linux`:

  ```
  wget https://github.com/unified-hmi/virtio-loopback-driver/raw/main/virtio_lo.h
  sudo cp virtio_lo.h /usr/include/linux
  ```

- Build and install remote-virtio-gpu

    ```
    git clone https://github.com/unified-hmi/remote-virtio-gpu.git
    cd ./remote-virtio-gpu
    cmake -B build -DCMAKE_BUILD_TYPE=Release
    make -C build
    sudo make install -C build
    ```

## Binary-only Install
  Download the `remote-virtio-gpu_X.X.X.deb` ubuntu package from the latest
  release builds from this github repository and install it.  
  Specify the correct version of `X.X.X` by referring to https://github.com/unified-hmi/remote-virtio-gpu/releases/latest  
  For example: `remote-virtio-gpu_1.0.0.deb`.  

  **Note:** An error about unresolved dependencies will occur when executing the following 2nd command, `sudo dpkg -i remote-virtio-gpu_X.X.X.deb`. This error can be resolved by running the 3rd one, `sudo apt -f install`.

  ```
  wget https://github.com/unified-hmi/remote-virtio-gpu/releases/latest/download/remote-virtio-gpu_X.X.X.deb
  sudo dpkg -i remote-virtio-gpu_X.X.X.deb
  sudo apt -f install
  ```
# How to Install Virtio-loopback-driver
When using RVGPU, this module is also necessary.  
For instructions on how to install Virtio-loopback-driver, please refer to the [README](https://github.com/unified-hmi/virtio-loopback-driver).

# How to Install Weston

The operation of RVGPU can be tested using Weston version 8.0.93 or above.  
Install Weston-8.0.93
```
sudo apt install libjpeg-dev libwebp-dev libsystemd-dev libpam-dev libva-dev freerdp2-dev \
               libxcb-composite0-dev liblcms2-dev libcolord-dev libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev libpipewire-0.2-dev \
               libxml2-dev meson libxkbcommon-x11-dev libpixman-1-dev libinput-dev libdrm-dev wayland-protocols libcairo2-dev \
               libpango1.0-dev libdbus-1-dev libgbm-dev libxcursor-dev

wget https://wayland.freedesktop.org/releases/weston-8.0.93.tar.xz
tar -xf weston-8.0.93.tar.xz
cd ~/weston-8.0.93/
meson build/
sudo ninja -C build/ install
sudo ldconfig
``` 

# Run RVGPU

RVGPU software consists of client (`rvgpu-proxy`) and server (`rvgpu-renderer`).
Let's describe how to run them on the same machine via the localhost interface.
We will start with `rvgpu-renderer`.  
To use RVGPU, you should be able to load the kernel, so turn **Secure Boot** off.


## Run `rvgpu-renderer` on Wayland

`rvgpu-renderer` with Wayland backend creates a window in the Wayland environment
and renders into it.  So you should have a window system supporting Wayland protocol
(such as Gnome with Wayland protocol or Weston) running.
To make this work, you can choose from [Using Ubuntu on Wayland](#using-ubuntu-on-wayland) or [Using Ubuntu (default)](#using-ubuntu-default).


### Using Ubuntu on Wayland
Select [Ubuntu on Wayland](https://linuxconfig.org/how-to-enable-disable-wayland-on-ubuntu-20-04-desktop) at the login screen.
Open a terminal and run this command:

```
rvgpu-renderer -b 1280x720@0,0 -p 55667
```
After this command, launching rvgpu-proxy will make the rvgpu-renderer create a window.

### Using Ubuntu (default)
You can launch a dedicated instance of Weston and run `rvgpu-renderer`
inside it.

```
weston --width 2200 --height 1200 &
rvgpu-renderer -b 1280x720@0,0 -p 55667
```

This command will create a weston window. Launching rvgpu-proxy will make the rvgpu-renderer create nested subwindow.
The script does not require the window system uses Wayland protocol,
so it could be run under X Window system.

## Run `rvgpu-proxy`

`rvgpu-proxy` should be able to access the kernel modules `virtio-lo` and `virtio-gpu`
so it should be run with superuser privileges.

```
sudo -i
modprobe virtio-gpu
modprobe virtio-lo
rvgpu-proxy -s 1280x720@0,0 -n 127.0.0.1:55667
```
**Note:** Those who have performed "Building from source" please follow the instructions below.
```
rvgpu-proxy -s 1280x720@0,0 -n 127.0.0.1:55667 -c /usr/local/etc/virgl.capset
```

After you run this, another GPU node `/dev/dri/cardX` appear.
Also, if you are running `rvgpu-renderer` in Wayland mode, it create 
a new window.

You can test the new gpu node for example running Weston-8.0.93 on it: 
```
export XDG_RUNTIME_DIR=/tmp
weston --backend drm-backend.so --tty=2 --seat=seat_virtual -i 0
```

After that `rvgpu-renderer` will display weston rendered and transferred
via localhost by `rvgpu-proxy`. Now you can launch `glmark2-es2-wayland` or
some other graphical application to verify that everything works.
```
sudo apt install glmark2-es2-wayland
glmark2-es2-wayland
```

## VSYNC feature

`rvgpu-proxy` can emulate VSYNC feature (see the `-f` command option).
To support the VSYNC feature in `rvgpu-proxy`, apply and rebuild the Linux kernel with the following path remote-virtio-gpu/documentation/patches/kernel.
The patches could be applied to the linux kernel with versions before 5.15.
The software still could be compiled or run on the recent kernels without support for VSYNC feature.

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
`rvgpu-renderer` on one machine and `rvgpu-proxy` with the kernel modules
on the another one, and pass the IP address and port number on which
rvgpu-renderer is listening to `rvgpu-proxy` through `-n` option.

**Note**  
Some graphical applications generate much network traffic. It is recommended to Configure the network to 1Gbps speed.

## Capsets

Sometimes the software does not work because the provided `virgl.capset` file does not specify
the capset of the rendering side.  It may manifest in black window.
In this case:

- Run `rvgpu-renderer` with `-c virgl.capset.new` option.
- Run `rvgpu-proxy` with the the default old capset.
- Kill the proxy and use the capset written by `rvgpu-renderer` for the next invocations of proxy.
