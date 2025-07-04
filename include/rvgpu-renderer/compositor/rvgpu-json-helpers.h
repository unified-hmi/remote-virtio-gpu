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

#ifndef RVGPU_JSON_HELPERS_H
#define RVGPU_JSON_HELPERS_H

#include <stdbool.h>

#include <jansson.h>

void insert_integar_json_array_with_index(json_t *json_array, int index,
					  json_t *integar);

void insert_jsonarray_with_index(json_t *src_array, json_t *dst_array,
				 int target_index);

json_t *get_jsonobj_with_str_key(json_t *array, const char *json_key,
				 const char *key_value);

json_t *get_jsonobj_with_int_key(json_t *array, const char *json_key,
				 int key_value);

int get_str_from_jsonobj(json_t *jsonobj, const char *key, const char **value);

int get_int_from_jsonobj(json_t *jsonobj, const char *key, int *value);

int get_double_from_jsonobj(json_t *jsonobj, const char *key, double *value);

int get_uintptr_from_jsonobj(json_t *jsonobj, const char *key,
			     uintptr_t *value);

void remove_jsonobj_with_int_key(json_t *array, const char *json_key,
				 int key_value);

void remove_jsonobj_with_str_key(json_t *array, const char *json_key,
				 const char *key_value);

bool int_value_in_json_array(json_t *json_array, int int_value);

bool int_value_in_json_array_with_key(json_t *json_array, char *json_key,
				      int key_value);

bool str_value_in_json_array_with_key(json_t *json_array, const char *json_key,
				      const char *key_value);

json_t *recv_json(int client_fd);

#endif /* RVGPU_JSON_HELPERS_H */
