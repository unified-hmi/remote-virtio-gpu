#// SPDX-License-Identifier: GPL-2.0
#/**
# * Copyright (c) 2022  Panasonic Automotive Systems, Co., Ltd.
# */

project(virtio-lo-driver VERSION 0.1.0 LANGUAGES C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_definitions(-D__KERNEL__ -DMODULE)

execute_process(
        COMMAND uname -r
        OUTPUT_VARIABLE KERNEL_RELEASE
        OUTPUT_STRIP_TRAILING_WHITESPACE
)

find_path(
        KERNELHEADERS_DIR
        include/linux/user.h
        PATHS /usr/src/linux-headers-${KERNEL_RELEASE}
)

set(KERNELHEADERS_INCLUDE_DIRS
            ${KERNELHEADERS_DIR}/include
            ${KERNELHEADERS_DIR}/arch/x86/include
            CACHE PATH "Kernel headers include dirs"
            )

if(NOT DEFINED KERNELDIR)
    execute_process(COMMAND uname -r OUTPUT_STRIP_TRAILING_WHITESPACE OUTPUT_VARIABLE KERNEL_VERSION)
    set(KERNELDIR /lib/modules/${KERNEL_VERSION}/build)
endif()

include_directories(${KERNELHEADERS_INCLUDE_DIRS})

set(DRIVER_FILE virtio_lo.ko)
set(KBUILD_CMD $(MAKE) -C ${KERNELHEADERS_DIR} modules M=${CMAKE_CURRENT_BINARY_DIR} src=${CMAKE_CURRENT_SOURCE_DIR})

FILE(WRITE ${CMAKE_CURRENT_SOURCE_DIR}/Kbuild "ccflags-y := -I$(src)/include\n")
FILE(APPEND ${CMAKE_CURRENT_SOURCE_DIR}/Kbuild "obj-m += virtio_lo.o\n")
FILE(APPEND ${CMAKE_CURRENT_SOURCE_DIR}/Kbuild "virtio_lo-y := virtio_lo_device.o virtio_lo_driver.o\n")

add_custom_command(OUTPUT ${DRIVER_FILE}
        COMMAND ${KBUILD_CMD}
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
        DEPENDS virtio_lo_device.c virtio_lo_driver.c VERBATIM)

add_custom_target(virtio-lo-driver ALL DEPENDS ${DRIVER_FILE})

install(CODE "execute_process(COMMAND make -C ${KERNELDIR} M=${CMAKE_CURRENT_BINARY_DIR} modules_install)")
install(CODE "execute_process(COMMAND depmod -A)")
