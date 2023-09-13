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

add_library(rvgpu SHARED
	tcp/rvgpu-tcp.c
	res/rvgpu-res.c
	rvgpu.c
)
set_target_properties(rvgpu
	PROPERTIES
	VERSION    ${LIBRVGPU_VERSION}
	SOVERSION  ${LIBRVGPU_SOVERSION}
	)
target_link_libraries(rvgpu PRIVATE pthread)
target_include_directories(rvgpu
	PRIVATE
		${PROJECT_SOURCE_DIR}/include
		${extlibs_INCLUDE_DIRS}
	)
target_compile_definitions(rvgpu PRIVATE
	_DEFAULT_SOURCE
	_GNU_SOURCE)
install(TARGETS rvgpu
	ARCHIVE DESTINATION lib
	LIBRARY DESTINATION lib
	RUNTIME DESTINATION bin)
