#include "texture_atlas.h"
#include "load_resources.h"

struct atlas_image
{
	struct rect
	{
		uint left = 0;
		uint top = 0;
		uint width = 0;
		uint height = 0;
	};

	std::vector<rect> frames;

	int orig_x = 0;  // 该偏移为相对 left 的
	int orig_y = 0;  // 该偏移为相对 top 的

	// infomation
	uint atlas_id = 0;
};

struct texture_atlas
{
	enum atlas_kind { Sprite, Background };

	uint size = 0;
	std::vector<uchar> data;

	atlas_kind kind = Sprite;
	std::vector<atlas_image> images;
};

std::unordered_map<uint, texture_atlas> game_texture_atlas;
uint texture_atlas_id_position = 0;

uint texture_atlas_create(uint size, texture_atlas::atlas_kind kind)
{
	try
	{
		switch (size)
		{
		case 256:
		case 512:
		case 1024:
		case 2048:
		{
			texture_atlas atlas = {
				.size = size,
				.data = std::vector<uchar>(size * size * 4, 0),
				.kind = kind
			};

			game_texture_atlas[texture_atlas_id_position] = atlas;
			return texture_atlas_id_position++;
		}

		default:
			throw std::runtime_error("Invalid texture atlas size. "
				"Supported sizes are 256, 512, 1024, and 2048.");
		}
	}
	simple_catch("texture_atlas_create", -1)
}

bool texture_atlas_add_sprite(uint id, std::string& gmspr_file)
{
	try
	{
		texture_atlas& atlas = game_texture_atlas.at(id);
		if (atlas.kind == texture_atlas::atlas_kind::Background)
			throw std::runtime_error("Cannot add sprite to a background atlas.");

		gm::sprite spr = gm::decode_gmspr(gmspr_file);

		return true;
	}
	simple_catch("texture_atlas_add_sprite", false)
}