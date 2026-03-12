#pragma once
#include "../main.h"

namespace gm
{
	extern bool crop_blank;

	// data + width + height
	typedef std::tuple<std::vector<uchar>, uint, uint> image_data;

	struct image_rect
	{
		int left = 0;
		int top = 0;
		int right = 0;
		int bottom = 0;

		bool operator==(const image_rect&) const = default;
	};

	struct sprite
	{
		uint width = 0;
		uint height = 0;
		int xorig = 0;  // x origin
		int yorig = 0;  // y origin

		std::vector<std::vector<uchar>> data;

		enum mask_kind { precise = 0, rectangular, disk, diamond };
		mask_kind mask = precise;		// collision mask kind
		uchar mask_tolerance = 0;		// collision mask tolerance (0-255)
		bool separate_mask = false;		// whether to use separate masks per frame

		enum bounding_kind { automatic = 0, full_image, user_defined };
		bounding_kind bounding = automatic;  // bounding box kind
		int bounding_left = 0;
		int bounding_right = 0;
		int bounding_top = 0;
		int bounding_bottom = 0;

		std::vector<image_rect> cropped_rects;  // 裁剪后的图像区域，供图集打包使用

		std::string name;

		inline uint frame_count() const { return data.size(); }
	};

	sprite decode_gmspr(const std::string& file);
	sprite decode_png(const std::string& file);
	sprite get_sprite_data(uint id);
	sprite get_background_data(uint id);

	image_data get_image_data(IDirect3DTexture8* texture);
}