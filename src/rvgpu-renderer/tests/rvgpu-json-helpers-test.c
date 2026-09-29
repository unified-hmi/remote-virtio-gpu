// SPDX-License-Identifier: Apache-2.0

#include <assert.h>

#include <jansson.h>

#include <rvgpu-renderer/compositor/rvgpu-json-helpers.h>

static json_t *integer_array(int first, int second)
{
	json_t *array = json_array();

	json_array_append_new(array, json_integer(first));
	json_array_append_new(array, json_integer(second));
	return array;
}

int main(void)
{
	json_t *source = integer_array(1, 2);
	json_t *destination = integer_array(0, 3);

	insert_jsonarray_with_index(source, destination, 1);

	assert(json_array_size(destination) == 4);
	assert(json_integer_value(json_array_get(destination, 0)) == 0);
	assert(json_integer_value(json_array_get(destination, 1)) == 1);
	assert(json_integer_value(json_array_get(destination, 2)) == 2);
	assert(json_integer_value(json_array_get(destination, 3)) == 3);

	json_decref(source);
	json_decref(destination);
	return 0;
}