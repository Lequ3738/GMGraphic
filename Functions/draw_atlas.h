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

		struct texture_region
		{
			int x = 0;
			int y = 0;
			int width = 0;
			int height = 0;
		};

		texture_atlas& atlas;
		texture_atlas::images& images;
		texture_atlas::images::sub_image& sub_image;

		transform trans;
		texture_region region;

		d3dcolor color_lt = 0xFFFFFFFF;  // ARGB
		d3dcolor color_rt = 0xFFFFFFFF;
		d3dcolor color_rb = 0xFFFFFFFF;
		d3dcolor color_lb = 0xFFFFFFFF;
	};

	void draw_sprite(uint id, uint subimg, double x, double y);
	void draw_sprite_stretched(uint id, uint subimg, double x, double y, double w, double h);
	void draw_sprite_part(uint id, uint subimg, int left, int top, int width,
		int height, double x, double y);
	void draw_sprite_ext(uint id, uint subimg, double x, double y, double xscale, 
		double yscale, double rot, d3dcolor color);
	void draw_sprite_stretched_ext(uint id, uint subimg, double x, double y, double w, 
		double h, d3dcolor color);
	void draw_sprite_part_ext(uint id, uint subimg, int left, int top, int width,
		int height, double x, double y, double xscale, double yscale, d3dcolor color);
	void draw_sprite_general(uint id, uint subimg, int left, int top, int width,
		int height, double x, double y, double xscale, double yscale, double rot,
		d3dcolor c1, d3dcolor c2, d3dcolor c3, d3dcolor c4);

	void force_draw_to_screen();
}

// ============================================================================
// Export Functions
// ============================================================================

exp_real atlas_sprite_exists(gm_real id);
exp_real atlas_background_exists(gm_real id);
exp_real atlas_sprite_get_number(gm_real id);
exp_str atlas_sprite_get_name(gm_real id);
exp_str atlas_background_get_name(gm_real id);
exp_real atlas_sprite_get_xoffset(gm_real id);
exp_real atlas_sprite_get_yoffset(gm_real id);

exp_real atlas_draw_sprite(gm_real id, gm_real subimg, gm_real x, gm_real y);
exp_real atlas_draw_sprite_stretched(gm_real id, gm_real subimg, gm_real x, gm_real y,
	gm_real width, gm_real height);
exp_real atlas_draw_sprite_part(gm_real id, gm_real subimg, gm_real left, gm_real top,
	gm_real width, gm_real height, gm_real x, gm_real y);
exp_real atlas_draw_sprite_ext(gm_real id, gm_real subimg, gm_real x, gm_real y,
	gm_real xscale, gm_real yscale, gm_real rot, gm_real color, gm_real alpha);
exp_real atlas_draw_sprite_stretched_ext(gm_real id, gm_real subimg, gm_real x, gm_real y,
	gm_real width, gm_real height, gm_real color, gm_real alpha);
exp_real atlas_draw_sprite_part_ext(gm_real id, gm_real subimg, gm_real left, gm_real top,
	gm_real width, gm_real height, gm_real x, gm_real y, gm_real xscale, gm_real yscale,
	gm_real color);
exp_real atlas_draw_sprite_general(gm_real id, gm_real subimg, gm_real left, gm_real top,
	gm_real width, gm_real height, gm_real x, gm_real y, gm_real xscale, gm_real yscale,
	gm_real rot);
exp_real atlas_draw_background(gm_real id, gm_real x, gm_real y);
exp_real atlas_draw_background_stretched(gm_real id, gm_real x, gm_real y, gm_real width,
	gm_real height);
exp_real atlas_draw_background_part(gm_real id, gm_real left, gm_real top, gm_real width,
	gm_real height, gm_real x, gm_real y);
exp_real atlas_draw_background_ext(gm_real id, gm_real x, gm_real y, gm_real xscale,
	gm_real yscale, gm_real rot, gm_real color, gm_real alpha);
exp_real atlas_draw_background_stretched_ext(gm_real id, gm_real x, gm_real y,
	gm_real width, gm_real height, gm_real color, gm_real alpha);
exp_real atlas_draw_background_part_ext(gm_real id, gm_real left, gm_real top,
	gm_real width, gm_real height, gm_real x, gm_real y, gm_real xscale, gm_real yscale,
	gm_real color, gm_real alpha);
exp_real atlas_draw_background_general(gm_real id, gm_real left, gm_real top,
	gm_real width, gm_real height, gm_real x, gm_real y, gm_real xscale, gm_real yscale,
	gm_real rot, gm_real c1);
exp_real force_draw_to_screen();