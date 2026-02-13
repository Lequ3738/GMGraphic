#pragma once
#include "../main.h"

namespace gm
{
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

		inline uint frame_count() const { return data.size(); }
	};

	sprite decode_gmspr(std::string& file);
}