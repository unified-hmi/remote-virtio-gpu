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

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <jansson.h>

#include <arpa/inet.h>

#include <rvgpu-utils/rvgpu-utils.h>

void insert_integar_json_array_with_index(json_t *json_array, size_t index,
					  json_t *integar)
{
	while (json_array_size(json_array) <= index) {
		json_array_append_new(json_array, json_null());
	}

	if (json_array_set_new(json_array, index, integar) == 0) {
		//printf("Value set at index %zu successfully.\n", index);
	} else {
		printf("Failed to set value at index %zu.\n", index);
	}
}

void insert_jsonarray_with_index(json_t *src_array, json_t *dst_array,
				 int target_index)
{
	size_t index;
	json_t *value;
	json_array_foreach(src_array, index, value)
	{
		json_array_insert_new(dst_array, target_index,
				      json_deep_copy(value));
	}
}

json_t *get_jsonobj_with_str_key(json_t *array, const char *json_key,
				 const char *key_value)
{
	size_t index;
	json_t *value;
	json_array_foreach(array, index, value)
	{
		json_t *json_str_obj = json_object_get(value, json_key);
		if (json_str_obj && json_is_string(json_str_obj)) {
			const char *str = json_string_value(json_str_obj);
			if (strcmp(str, key_value) == 0) {
				return value;
			}
		}
	}
	return NULL;
}

json_t *get_jsonobj_with_int_key(json_t *array, const char *json_key,
				 int key_value)
{
	size_t index;
	json_t *value;
	json_array_foreach(array, index, value)
	{
		if (key_value ==
		    json_integer_value(json_object_get(value, json_key))) {
			return value;
		}
	}
	return NULL;
}

int get_str_from_jsonobj(json_t *jsonobj, const char *key, const char **value)
{
	json_t *json_value = json_object_get(jsonobj, key);
	if (json_value == NULL) {
		return -1;
	} else if (json_is_string(json_value)) {
		*value = json_string_value(json_value);
	} else {
		//fprintf(stderr, "cannot get string value with %s json key, json_value: %s\n", key, json_dumps(json_value, 0));
		return -1;
	}
	return 0;
}

int get_int_from_jsonobj(json_t *jsonobj, const char *key, int *value)
{
	json_t *json_value = json_object_get(jsonobj, key);
	if (json_value == NULL) {
		return -1;
	} else if (json_is_integer(json_value)) {
		*value = json_integer_value(json_value);
	} else if (json_is_real(json_value)) {
		*value = (int)json_real_value(json_value);
	} else {
		fprintf(stderr, "cannot get value with %s json key\n", key);
		return -1;
	}
	return 0;
}

int get_uintptr_from_jsonobj(json_t *jsonobj, const char *key, uintptr_t *value)
{
	json_t *json_value = json_object_get(jsonobj, key);
	if (json_value == NULL) {
		return -1;
	} else if (json_is_integer(json_value)) {
		*value = (uintptr_t)json_integer_value(json_value);
	} else if (json_is_real(json_value)) {
		*value = (uintptr_t)json_real_value(json_value);
	} else {
		//fprintf(stderr, "cannot get value with %s json key\n", key);
		return -1;
	}
	return 0;
}

int get_double_from_jsonobj(json_t *jsonobj, const char *key, double *value)
{
	json_t *json_value = json_object_get(jsonobj, key);
	if (json_value == NULL) {
		return -1;
	} else if (json_is_real(json_value)) {
		*value = json_real_value(json_value);
	} else if (json_is_integer(json_value)) {
		*value = (double)json_integer_value(json_value);
	} else {
		return -1;
	}
	return 0;
}

void remove_jsonobj_with_int_key(json_t *array, const char *json_key,
				 int key_value)
{
	size_t index;
	json_t *value;
	json_array_foreach(array, index, value)
	{
		if (key_value ==
		    json_integer_value(json_object_get(value, json_key))) {
			json_array_remove(array, index);
			index--;
		}
	}
}

void remove_jsonobj_with_str_key(json_t *array, const char *json_key,
				 const char *key_value)
{
	size_t index;
	json_t *value;
	json_array_foreach(array, index, value)
	{
		json_t *key_value_json = json_object_get(value, json_key);
		if (json_is_string(key_value_json)) {
			const char *str_key_value =
				json_string_value(key_value_json);
			if (strcmp(str_key_value, key_value) == 0) {
				json_array_remove(array, index);
				index--;
			}
		}
	}
}

bool int_value_in_json_array(json_t *json_array, int int_value)
{
	int index;
	json_t *value;
	json_array_foreach(json_array, index, value)
	{
		if (json_integer_value(value) == int_value) {
			return true;
		}
	}
	return false;
}

bool int_value_in_json_array_with_key(json_t *json_array, char *json_key,
				      int key_value)
{
	size_t index;
	json_t *value;
	json_array_foreach(json_array, index, value)
	{
		if (key_value ==
		    json_integer_value(json_object_get(value, json_key))) {
			return true;
		}
	}
	return false;
}

bool str_value_in_json_array_with_key(json_t *json_array, const char *json_key,
				      const char *key_value)
{
	size_t index;
	json_t *value;
	json_array_foreach(json_array, index, value)
	{
		json_t *key_value_obj = json_object_get(value, json_key);
		if (json_is_string(key_value_obj) &&
		    strcmp(json_string_value(key_value_obj), key_value) == 0) {
			return true;
		}
	}
	return false;
}

json_t *recv_json(int client_fd)
{
	json_t *json_obj;
	char *data = recv_str_all(client_fd);
	if (data != NULL) {
		//		printf("recv_json data: %s\n", data);
		json_error_t error;
		json_obj = json_loads(data, 0, &error);
		if (!json_obj) {
			fprintf(stderr, "recv_json data: %s, error: %s\n", data,
				error.text);
			free(data);
			return NULL;
		}
	} else {
		return NULL;
	}
	free(data);
	return json_obj;
}
