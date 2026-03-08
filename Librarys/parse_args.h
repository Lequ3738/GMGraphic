#pragma once
#include "../main.h"

template <int size>
int parse_args(double(&dst)[size])
{
	int list = gm::argument_list;
	int count = std::min(gm::ds_list_size(list), size);

	int i = 0;
	for (; i < count; i++)
	{
		auto var = gm::ds_list_find_value(list, i);
		if (var.IsString())
			break;
		dst[i] = var.real();
	}

	return i;
}