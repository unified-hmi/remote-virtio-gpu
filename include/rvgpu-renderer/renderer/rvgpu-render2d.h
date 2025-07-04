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

#ifndef RVGPU_RENDER2D_H
#define RVGPU_RENDER2D_H

#define RENDER2D_FLIP_V (1 << 0)
#define RENDER2D_FLIP_H (1 << 1)
#define M_PId180f (3.1415926f / 180.0f)

int set_2d_projection_matrix(int w, int h);
int init_2d_renderer(int w, int h);
int draw_2d_texture(int texid, int x, int y, int w, int h, int upsidedown);
int draw_2d_texture_layout(int texid, int width, int height, double src_x,
			   double src_y, double src_w, double src_h,
			   double dst_x, double dst_y, double dst_w,
			   double dst_h, int upsidedown);

#endif /* RVGPU_RENDER2D_H */
