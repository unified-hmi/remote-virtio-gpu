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


add_executable(rvgpu-distrib-com
	rvgpu-distrib-com.c
	../../compositor/rvgpu-connection.c
	$<TARGET_OBJECTS:rvgpu-utils>)
target_include_directories(rvgpu-distrib-com
	PRIVATE
		${PROJECT_SOURCE_DIR}/include
		${extlibs_INCLUDE_DIRS}
	)
target_compile_definitions(rvgpu-distrib-com PRIVATE _GNU_SOURCE)
target_link_libraries(rvgpu-distrib-com
        PRIVATE ${extlibs_LIBRARIES} jansson)
install(TARGETS rvgpu-distrib-com RUNTIME DESTINATION bin)
