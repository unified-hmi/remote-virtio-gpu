#/**
# * Copyright (c) 2022  Panasonic Automotive Systems, Co., Ltd.
# *
# * Licensed under the Apache License, Version 2.0 (the "License");
# * you may not use this file except in compliance with the License.
# * You may obtain a copy of the License at
# *
# *     http://www.apache.org/licenses/LICENSE-2.0
# *
# * Unless required by applicable law or agreed to in writing, software
# * distributed under the License is distributed on an "AS IS" BASIS,
# * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# * See the License for the specific language governing permissions and
# * limitations under the License.
# */

add_executable(rvgpu-proxy
	gpu/rvgpu-gpu-device.c
	gpu/rvgpu-input-device.c
	gpu/rvgpu-iov.c
	gpu/rvgpu-map-guest.c
	gpu/rvgpu-vqueue.c
	rvgpu-proxy.c
	$<TARGET_OBJECTS:rvgpu-sanity>)
target_include_directories(rvgpu-proxy PRIVATE
	${PROJECT_SOURCE_DIR}/include
	${extlibs_INCLUDE_DIRS}
	${VIRTIO_LO_DIR}/include/uapi)
target_compile_definitions(rvgpu-proxy PRIVATE
	_FILE_OFFSET_BITS=64
	_DEFAULT_SOURCE
	_GNU_SOURCE)
target_link_libraries(rvgpu-proxy PRIVATE pthread dl)
install(TARGETS rvgpu-proxy RUNTIME DESTINATION bin)
