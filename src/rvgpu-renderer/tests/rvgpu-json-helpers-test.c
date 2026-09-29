// SPDX-License-Identifier: Apache-2.0

#include <stdio.h>
#include <stdlib.h>

#include <jansson.h>

#include <rvgpu-renderer/compositor/rvgpu-json-helpers.h>

static json_t *integer_array(int first, int second)
{
	json_t *array = json_array();

	json_array_append_new(array, json_integer(first));
	json_array_append_new(array, json_integer(second));
	return array;
}

static int check_array_order(json_t *array, const int expected[], size_t size)
{
	if (json_array_size(array) != size) {
		fprintf(stderr, "expected %zu entries, got %zu\n", size,
			json_array_size(array));
		return EXIT_FAILURE;
	}

	for (size_t index = 0; index < size; index++) {
		int actual = json_integer_value(json_array_get(array, index));

		if (actual != expected[index]) {
			fprintf(stderr, "entry %zu: expected %d, got %d\n", index,
				expected[index], actual);
			return EXIT_FAILURE;
		}
	}

	return EXIT_SUCCESS;
}

int main(void)
{
	json_t *source = integer_array(1, 2);
	json_t *destination = integer_array(0, 3);
	const int expected[] = { 0, 1, 2, 3 };

	insert_jsonarray_with_index(source, destination, 1);
	int result = check_array_order(destination, expected,
				       sizeof(expected) / sizeof(expected[0]));

	json_decref(source);
	json_decref(destination);
	return result;
}