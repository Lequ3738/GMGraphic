#pragma once
#include "../main.h"

namespace atlas
{
	struct draw_info
	{
		struct transform
		{
			double x = 0;
			double y = 0;
			double xscale = 1;
			double yscale = 1;
			
			// 弧度制，且 GM8 的 Y 轴向下，所以旋转方向是相反的，计算时要加上负号
			double rot = 0;
		};

		texture_atlas& atlas;
		texture_atlas::images& images;
		texture_atlas::images::sub_image& sub_image;

		transform trans;
		uint color = 0xFFFFFFFF;  // ARGB
	};

	void draw_sprite(uint id, uint subimg, double x, double y);
	void force_draw_to_screen();
}