# SPDX-License-Identifier: Apache-2.0
#
# Copyright (c) 2022  Panasonic Automotive Systems, Co., Ltd.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#


cmake_minimum_required(VERSION 3.5)
project(rvgpu C)
set(CMAKE_C_STANDARD 11)

set(CMAKE_C_FLAGS       "${CMAKE_C_FLAGS} -Wall -Wextra")
set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} -Werror")

set(LIBRVGPU_VERSION 0.1.0)
set(LIBRVGPU_SOVERSION 0)

if (CMAKE_SYSROOT AND NOT ENV{PKG_CONFIG_LIBDIR})
	set(ENV{PKG_CONFIG_DIR} "")
	set(ENV{PKG_CONFIG_LIBDIR} "${CMAKE_SYSROOT}/usr/lib/pkgconfig:${CMAKE_SYSROOT}/usr/share/pkgconfig")
	set(ENV{PKG_CONFIG_SYSROOT_DIR} ${CMAKE_SYSROOT})
endif(CMAKE_SYSROOT AND NOT ENV{PKG_CONFIG_LIBDIR})

include(FindPkgConfig)
pkg_check_modules(extlibs REQUIRED virglrenderer egl glesv2 wayland-client wayland-egl gbm libdrm libinput)

add_subdirectory(src)

include(GNUInstallDirs)
install(FILES settings/virgl.capset
        DESTINATION ${CMAKE_INSTALL_SYSCONFDIR}
)
install(DIRECTORY settings/udev/
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/udev/rules.d
        FILES_MATCHING PATTERN "*.rules"
)
