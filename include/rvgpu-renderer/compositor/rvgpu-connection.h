// SPDX-License-Identifier: Apache-2.0
/**
 * Copyright (c) 2022  Panasonic Automotive Systems, Co., Ltd.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef RVGPU_CONNECTION_H
#define RVGPU_CONNECTION_H

#define UHMI_RVGPU_COMPOSITOR_SOCK "uhmi-rvgpu_compositor_sock"
#define UHMI_RVGPU_LAYOUT_SOCK "uhmi-rvgpu_layout_sock"

int create_server_socket(const char *domain);
int connect_to_client(int socket);
int connect_to_server(char *domain);

#endif /* RVGPU_CONNECTION_H */
