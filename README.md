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
  - [Installation Requirements for RVGPU](#installation-requirements-for-rvgpu)
  - [Run RVGPU](#run-rvgpu)
    - [Run `rvgpu-renderer` on Wayland](#run-rvgpu-renderer-on-wayland)
    - [Run `rvgpu-proxy`](#run-rvgpu-proxy)
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

The install instructions described here are tested on Ubuntu 24.04 LTS AMD64.
However you can try it with different Linux distros with Wayland display
server. Assuming, you have a clean Ubuntu 24.04 installed, perform the
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
  For example: `virtio-lo-dev_1.1.0.deb`.

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
  For example: `remote-virtio-gpu_1.2.0.deb`.

  **Note:**
  An error about unresolved dependencies will occur when executing the following 2nd command, `sudo dpkg -i remote-virtio-gpu_X.X.X.deb`. This error can be resolved by running the 3rd one, `sudo apt -f install`.

  ```
  wget https://github.com/unified-hmi/remote-virtio-gpu/releases/latest/download/remote-virtio-gpu_X.X.X.deb
  sudo dpkg -i remote-virtio-gpu_X.X.X.deb
  sudo apt -f install
  ```

# Installation Requirements for RVGPU

To use RVGPU for remote display, you need to install the following components:

## Virtio-loopback-driver

This module is essential for RVGPU operation. For installation instructions, please refer to the [README](https://github.com/unified-hmi/virtio-loopback-driver).

## Rvgpu-wlproxy

If you wish to display Wayland applications remotely, you can optionally install `rvgpu-wlproxy`, which can be used as a lightweight Wayland server instead of Weston for similar functionality. For detailed installation, configuration, and usage instructions, please refer to the [README](https://github.com/unified-hmi/rvgpu-wlproxy).

# Run RVGPU

RVGPU software consists of client (`rvgpu-proxy`) and server (`rvgpu-renderer`).
Let's describe how to run them on the same machine via the localhost interface.
We will start with `rvgpu-renderer`.  
To use RVGPU, you should be able to load the kernel, so turn **Secure Boot** off.

## Run rvgpu-renderer on Wayland

`rvgpu-renderer` with Wayland backend creates a window in the Wayland environment
and renders into it.  So you should have a window system supporting Wayland protocol
(such as Gnome with Wayland protocol or Weston) running.

Open a terminal and run this command:

```
rvgpu-renderer -b 1280x720@0,0 -p 55667
```

After this command, `rvgpu-renderer` will create a window. The `-a` option enables transparency for the window.

### Using weston
You can launch a dedicated instance of Weston and run `rvgpu-renderer`
inside it.

```
weston --width 2200 --height 1200 &
rvgpu-renderer -b 1280x720@0,0 -p 55667
```

This command will create a `weston` window, and within that environment, `rvgpu-renderer` will generate its own window on top.
If your system does not natively support the Wayland protocol, you can use `weston` as an alternative to enable `rvgpu-renderer` to run on window systems like the X Window System or others.

## Run rvgpu-proxy

`rvgpu-proxy` should be able to access the kernel modules `virtio-lo` and `virtio-gpu`
so it should be run with superuser privileges.

```
sudo -i
modprobe virtio-gpu
modprobe virtio-lo
```

**Note:**
Those who have performed "Building from source" please follow the instructions below.

```
rvgpu-proxy -s 1280x720@0,0 -n 127.0.0.1:55667 -c /usr/local/etc/virgl.capset
```

After running this command, a new GPU node `/dev/dri/cardX` will appear. Additionally, a symbolic link named `rvgpu_virtio` will be created, pointing to this new GPU node. To customize the symbolic link name with an index, run the following command before starting `rvgpu-proxy`.

```
echo "0" > /tmp/rvgpu-index
```

This allows you to set a specific index for easier identification of the symbolic link.

### Using Multiple rvgpu-proxy with rvgpu-renderer

`rvgpu-renderer` supports connecting multiple rvgpu-proxy for composite display. To achieve this, you can start additional `rvgpu-proxy` with different configurations. For example:

```
echo "0" > /tmp/rvgpu-index
rvgpu-proxy -s 1280x720@0,0 -n 127.0.0.1:55667 -c /usr/local/etc/virgl.capset
echo "1" > /tmp/rvgpu-index
rvgpu-proxy -s 800x600@0,0 -n 127.0.0.1:55667 -c /usr/local/etc/virgl.capset
```

Each `rvgpu-proxy` can render different content, and `rvgpu-renderer` will composite these outputs into a single display, providing a unified view of all connected proxies.

**Note:**
`rvgpu-renderer` composites the rendering outputs from each rvgpu-proxy directly, starting from the top-left corner, without clipping or scaling.

### Run Wayland Server on RVGPU

To test the new GPU node, you can run `rvgpu-wlproxy` as a lightweight Wayland server. Set the necessary environment variables and execute the following command:

```
export EGLWINSYS_DRM_DEV_NAME="/dev/dri/rvgpu_virtio"
export EGLWINSYS_DRM_MOUSE_DEV="/dev/input/rvgpu_mouse"
export EGLWINSYS_DRM_MOUSEABS_DEV="/dev/input/rvgpu_mouse_abs"
export EGLWINSYS_DRM_KEYBOARD_DEV="/dev/input/rvgpu_keyboard"
export EGLWINSYS_DRM_TOUCH_DEV="/dev/input/rvgpu_touch"
export XDG_RUNTIME_DIR="/tmp"
rvgpu-wlproxy -S wayland-rvgpu
```

Alternatively, if you have Weston version 8.0.93 or higher, you can use Weston as the Wayland server:

```
export XDG_RUNTIME_DIR=/tmp
weston --backend drm-backend.so --drm-device=<your cardX> --seat=seat_virtual  -S wayland-rvgpu -i 0
```

**Note:**
The index specified in your setup can affect the naming of symbolic links for input devices and DRM devices generated by `rvgpu-proxy`. Ensure that the index matches your intended setup, especially when using multiple `rvgpu-proxy`.

### Launch Wayland Application

Once the wayland server is running, `rvgpu-renderer` will display the content rendered by the wayland server, which is transferred via `rvgpu-proxy` over localhost. You can then launch wayland applications like `glmark2-es2-wayland` to verify that everything is functioning correctly.

```
sudo apt install glmark2-es2-wayland
export XDG_RUNTIME_DIR=/tmp
export WAYLAND_DISPLAY=wayland-rvgpu
glmark2-es2-wayland -s 1280x720
```

### How to run RVGPU remotely

The way is almost the same as to run **RVGPU** locally. Launch
`rvgpu-renderer` on one machine and `rvgpu-proxy` with the kernel modules
on the another one, and pass the IP address and port number on which
rvgpu-renderer is listening to `rvgpu-proxy` through `-n` option.

**Note:**
Some graphical applications generate much network traffic. It is recommended to Configure the network to 1Gbps speed.

## Capsets

Sometimes the software does not work because the provided `virgl.capset` file does not specify
the capset of the rendering side.  It may manifest in black window.
In this case:

- Run `rvgpu-renderer` with `-c virgl.capset.new` option.
- Run `rvgpu-proxy` with the the default old capset.
- Kill the proxy and use the capset written by `rvgpu-renderer` for the next invocations of proxy.
