#include "texture_atlas.h"
#include "shader.h"
#include "draw_atlas.h"
#include "math_s.h"
#include "parse_args.h"

static void draw_image(atlas::draw_info& info)
{
	try
	{
		if (info.atlas.texture == nullptr)
			throw std::runtime_error("Cannot find the texture atlas of this sprite.");

		if (current_texture.texture != info.atlas.texture || vbuff_c + 6 >= vb_count)
		{
			atlas::end_draw();
			atlas::start_draw(info.atlas.texture, D3DFMT_A8R8G8B8);
		}

		bool draw_part = (info.region.width != 0 && info.region.height != 0);
		float u0 = 0, v0 = 0, u1 = 0, v1 = 0, u2 = 0, v2 = 0, u3 = 0, v3 = 0;
		if (draw_part)  // Draw a part of the image
		{
			if (!info.sub_image.is_rotated)
			{
				u0 = float(info.sub_image.texture_left + info.region.x) / (float)info.atlas.size;
				v0 = float(info.sub_image.texture_top + info.region.y) / (float)info.atlas.size;
				u1 = float(info.sub_image.texture_left + info.region.x + info.region.width) /
					(float)info.atlas.size;
				v1 = v0;
				u2 = u1;
				v2 = float(info.sub_image.texture_top + info.region.y + info.region.height) /
					(float)info.atlas.size;
				u3 = u0;
				v3 = v2;
			}
			else
			{
				u0 = float(int(info.sub_image.texture_left + info.sub_image.texture_height) - 
					info.region.y) / (float)info.atlas.size;
				v0 = float(info.sub_image.texture_top + info.region.x) / (float)info.atlas.size;
				u1 = u0;
				v1 = float(info.sub_image.texture_top + info.region.x + info.region.width) /
					(float)info.atlas.size;
				u2 = float(int(info.sub_image.texture_left + info.sub_image.texture_height) -
					(info.region.y + info.region.height)) / (float)info.atlas.size;
				v2 = v1;
				u3 = u2;
				v3 = v0;
			}
		}
		else
		{
			if (!info.sub_image.is_rotated)
			{
				u0 = (float)info.sub_image.texture_left / (float)info.atlas.size;
				v0 = (float)info.sub_image.texture_top / (float)info.atlas.size;
				u1 = float(info.sub_image.texture_left + info.sub_image.texture_width) /
					(float)info.atlas.size;
				v1 = v0;
				u2 = u1;
				v2 = float(info.sub_image.texture_top + info.sub_image.texture_height) /
					(float)info.atlas.size;
				u3 = u0;
				v3 = v2;
			}
			else
			{
				u0 = float(info.sub_image.texture_left + info.sub_image.texture_height) / 
					(float)info.atlas.size;
				v0 = (float)info.sub_image.texture_top / (float)info.atlas.size;
				u1 = u0;
				v1 = float(info.sub_image.texture_top + info.sub_image.texture_width) / 
					(float)info.atlas.size;
				u2 = (float)info.sub_image.texture_left / (float)info.atlas.size;
				v2 = v1;
				u3 = u2;
				v3 = v0;
			}
		}

		double left = 0, top = 0, right = 0, bottom = 0;
		// -0.5 为半像素偏移修正，详细内容见 https://zhuanlan.zhihu.com/p/639407618
		if (draw_part)
		{
			left = -0.5;
			top = -0.5;
			right = info.region.width * info.trans.xscale - 0.5;
			bottom = info.region.height * info.trans.yscale - 0.5;
		}
		else
		{
			left = -info.sub_image.orig_x * info.trans.xscale - 0.5;
			top = -info.sub_image.orig_y * info.trans.yscale - 0.5;
			right = ((int)info.sub_image.texture_width - info.sub_image.orig_x) *
				info.trans.xscale - 0.5;
			bottom = ((int)info.sub_image.texture_height - info.sub_image.orig_y) *
				info.trans.yscale - 0.5;
		}

		float x_lt = 0, y_lt = 0, x_rt = 0, y_rt = 0, x_rb = 0, y_rb = 0, x_lb = 0, y_lb = 0;
		if (std::abs(info.trans.rot) > 0.00000001)
		{
			double c = std::cos(info.trans.rot);
			double s = std::sin(info.trans.rot);

			x_lt = static_cast<float>(info.trans.x + left * c + top * s);
			y_lt = static_cast<float>(info.trans.y - left * s + top * c);
			x_rt = static_cast<float>(info.trans.x + right * c + top * s);
			y_rt = static_cast<float>(info.trans.y - right * s + top * c);
			x_rb = static_cast<float>(info.trans.x + right * c + bottom * s);
			y_rb = static_cast<float>(info.trans.y - right * s + bottom * c);
			x_lb = static_cast<float>(info.trans.x + left * c + bottom * s);
			y_lb = static_cast<float>(info.trans.y - left * s + bottom * c);
		}
		else
		{
			x_lt = static_cast<float>(info.trans.x + left);
			y_lt = static_cast<float>(info.trans.y + top);
			x_rt = static_cast<float>(info.trans.x + right);
			y_rt = y_lt;
			x_rb = x_rt;
			y_rb = static_cast<float>(info.trans.y + bottom);
			x_lb = x_lt;
			y_lb = y_rb;
		}
		
		// 三角形 1
		vertex::push_vertex_2d(x_lt, y_lt, u0, v0, info.color_lt);
		vertex::push_vertex_2d(x_rt, y_rt, u1, v1, info.color_rt);
		vertex::push_vertex_2d(x_rb, y_rb, u2, v2, info.color_rb);

		// 三角形 2
		vertex::push_vertex_2d(x_lt, y_lt, u0, v0, info.color_lt);
		vertex::push_vertex_2d(x_rb, y_rb, u2, v2, info.color_rb);
		vertex::push_vertex_2d(x_lb, y_lb, u3, v3, info.color_lb);
	}
	transpond_catch("draw_image(atlas::draw_info&)")
}

#define GET_ATLAS																	\
	texture_atlas::images* images_ptr = game_images.at(id);							\
	texture_atlas* atlas_ptr = nullptr;												\
	texture_atlas::images::sub_image* sub_image_ptr = nullptr;						\
																					\
	subimg = subimg % images_ptr->frames.size();									\
																					\
	if (images_ptr != nullptr) {													\
		atlas_ptr = game_texture_atlas.at(images_ptr->atlas_id).get();				\
		sub_image_ptr = images_ptr->frames.at(subimg).get();						\
	}																				\
	else																			\
		throw std::runtime_error("Cannot find the sprite.");						\
																					\
	if (atlas_ptr == nullptr)														\
		throw std::runtime_error("Cannot find the texture atlas of this sprite.");	\
																					\
	if (sub_image_ptr == nullptr)													\
		return;

void atlas::draw_sprite(uint id, uint subimg, double x, double y)
{
	try
	{
		GET_ATLAS;

		draw_info info = {
			.atlas = *atlas_ptr, .images = *images_ptr, .sub_image = *sub_image_ptr,
			.trans = {.x = x, .y = y}
		};
		draw_image(info);
	}
	transpond_catch("atlas::draw_sprite(uint, uint, double, double)")
}

void atlas::draw_sprite_stretched(uint id, uint subimg, double x, double y, double w, 
	double h)
{
	try
	{
		GET_ATLAS;

		draw_info info = {
			.atlas = *atlas_ptr, .images = *images_ptr, .sub_image = *sub_image_ptr,
			.trans = {
				.x = x, .y = y,
				.xscale = w / sub_image_ptr->texture_width,
				.yscale = h / sub_image_ptr->texture_height
			}
		};
		draw_image(info);
	}
	transpond_catch("atlas::draw_sprite_stretched(uint, uint, double, double, double, "
		"double)")
}

void atlas::draw_sprite_part(uint id, uint subimg, int left, int top, int width,
	int height, double x, double y)
{
	try
	{
		GET_ATLAS;

		draw_info info = {
			.atlas = *atlas_ptr, .images = *images_ptr, .sub_image = *sub_image_ptr,
			.trans = {.x = x, .y = y},
			.region = {.x = left, .y = top, .width = width, .height = height}
		};
		draw_image(info);
	}
	transpond_catch("atlas::draw_sprite_part(uint, uint, int, int, int, int, double, double)")
}

void atlas::draw_sprite_ext(uint id, uint subimg, double x, double y, double xscale, 
	double yscale, double rot, d3dcolor color)
{
	try
	{
		GET_ATLAS;

		draw_info info = {
			.atlas = *atlas_ptr, .images = *images_ptr, .sub_image = *sub_image_ptr,
			.trans = {.x = x, .y = y, .xscale = xscale, .yscale = yscale, .rot = rot},
			.color_lt = color, .color_rt = color, .color_rb = color, .color_lb = color
		};
		draw_image(info);
	}
	transpond_catch("atlas::draw_sprite_ext(uint, uint, double, double, double, "
		"double, double, d3dcolor)")
}

void atlas::draw_sprite_stretched_ext(uint id, uint subimg, double x, double y, double w,
	double h, d3dcolor color)
{
	try
	{
		GET_ATLAS;

		draw_info info = {
			.atlas = *atlas_ptr, .images = *images_ptr, .sub_image = *sub_image_ptr,
			.trans = {
				.x = x, .y = y,
				.xscale = w / sub_image_ptr->texture_width,
				.yscale = h / sub_image_ptr->texture_height
			},
			.color_lt = color, .color_rt = color, .color_rb = color, .color_lb = color
		};
		draw_image(info);
	}
	transpond_catch("atlas::draw_sprite_stretched_ext(uint, uint, double, double, "
		"double, double, d3dcolor)")
}

void atlas::draw_sprite_part_ext(uint id, uint subimg, int left, int top, int width,
	int height, double x, double y, double xscale, double yscale, d3dcolor color)
{
	try
	{
		GET_ATLAS;

		draw_info info = {
			.atlas = *atlas_ptr, .images = *images_ptr, .sub_image = *sub_image_ptr,
			.trans = {.x = x, .y = y},
			.region = {.x = left, .y = top, .width = width, .height = height},
			.color_lt = color, .color_rt = color, .color_rb = color, .color_lb = color
		};
		draw_image(info);
	}
	transpond_catch("atlas::draw_sprite_part_ext(uint, uint, int, int, int, int, "
		"double, double, double, double, d3dcolor)")
}

void atlas::draw_sprite_general(uint id, uint subimg, int left, int top, int width,
	int height, double x, double y, double xscale, double yscale, double rot,
	d3dcolor c1, d3dcolor c2, d3dcolor c3, d3dcolor c4)
{
	try
	{
		GET_ATLAS;

		draw_info info = {
			.atlas = *atlas_ptr, .images = *images_ptr, .sub_image = *sub_image_ptr,
			.trans = {.x = x, .y = y, .xscale = xscale, .yscale = yscale, .rot = rot},
			.region = {.x = left, .y = top, .width = width, .height = height},
			.color_lt = c1, .color_rt = c2, .color_rb = c3, .color_lb = c4
		};
		draw_image(info);
	}
	transpond_catch("atlas::draw_sprite_general(uint, uint, int, int, int, int, "
		"double, double, double, double, double, d3dcolor, d3dcolor, d3dcolor, "
		"d3dcolor)")
}

#undef GET_ATLAS

void atlas::force_draw_to_screen()
{
	try
	{
		atlas::end_draw();
	}
	transpond_catch("atlas::force_draw_to_screen()")
}

// ==================== Export Functions ====================

exp_real atlas_sprite_exists(gm_real id)
{
	if (id < IMAGE_START_POSITION)
		return (gm_real)gm::sprite_exists((int)id);

	return (gm_real)game_images.count((uint)id);
}

exp_real atlas_background_exists(gm_real id)
{
	if (id < IMAGE_START_POSITION)
		return (gm_real)gm::background_exists((int)id);

	return (gm_real)game_images.count((uint)id);
}

exp_real atlas_sprite_get_number(gm_real id)
{
	try
	{
		if (id < IMAGE_START_POSITION)
			return (gm_real)gm::sprite_get_number((int)id);

		return (gm_real)game_images.at((uint)id)->frames.size();
	}
	simple_catch("atlas_sprite_get_number", -1)
}

exp_str atlas_sprite_get_name(gm_real id)
{
	try
	{
		if (id < IMAGE_START_POSITION)
		{
			return_string(gm::sprite_get_name((int)id).c_str());
		}

		return game_images.at((uint)id)->name.c_str();
	}
	simple_catch("atlas_sprite_get_name", "invalid sprite")
}

exp_str atlas_background_get_name(gm_real id)
{
	try
	{
		if (id < IMAGE_START_POSITION)
		{
			return_string(gm::background_get_name((int)id).c_str());
		}

		return game_images.at((uint)id)->name.c_str();
	}
	simple_catch("atlas_background_get_name", "invalid background")
}

exp_real atlas_sprite_get_xoffset(gm_real id)
{
	try
	{
		if (id < IMAGE_START_POSITION)
			return (gm_real)gm::sprite_get_xoffset((int)id);

		auto sub = game_images.at((uint)id)->frames.at(0).get();
		return gm_real(sub->texture_left + sub->orig_x);
	}
	simple_catch("atlas_sprite_get_xoffset", 0)
}

exp_real atlas_sprite_get_yoffset(gm_real id)
{
	try
	{
		if (id < IMAGE_START_POSITION)
			return (gm_real)gm::sprite_get_yoffset((int)id);

		auto sub = game_images.at((uint)id)->frames.at(0).get();
		return gm_real(sub->texture_top + sub->orig_y);
	}
	simple_catch("atlas_sprite_get_yoffset", 0)
}

exp_real atlas_draw_sprite(gm_real id, gm_real subimg, gm_real x, gm_real y)
{
	try
	{
		if (id < IMAGE_START_POSITION)
		{
			atlas::force_draw_to_screen();
			gm::draw_sprite((int)id, (int)subimg, x, y);
			return gtrue;
		}
		
		atlas::draw_sprite((uint)id, (uint)subimg, x, y);
		return gtrue;
	}
	simple_catch("atlas_draw_sprite", gfalse)
}

exp_real atlas_draw_sprite_stretched(gm_real id, gm_real subimg, gm_real x, gm_real y,
	gm_real width, gm_real height)
{
	try
	{
		if (id < IMAGE_START_POSITION)
		{
			atlas::force_draw_to_screen();
			gm::draw_sprite_stretched((int)id, (int)subimg, x, y, width, height);
			return gtrue;
		}

		atlas::draw_sprite_stretched((uint)id, (uint)subimg, x, y, width, height);
		return gtrue;
	}
	simple_catch("atlas_draw_sprite_stretched", gfalse)
}

exp_real atlas_draw_sprite_part(gm_real id, gm_real subimg, gm_real left, gm_real top,
	gm_real width, gm_real height, gm_real x, gm_real y)
{
	try
	{
		if (id < IMAGE_START_POSITION)
		{
			atlas::force_draw_to_screen();
			gm::draw_sprite_part((int)id, (int)subimg, (int)left, (int)top, (int)width,
				(int)height, x, y);
			return gtrue;
		}

		atlas::draw_sprite_part((uint)id, (uint)subimg, (int)left, (int)top, (int)width,
			(int)height, x, y);
		return gtrue;
	}
	simple_catch("atlas_draw_sprite_part", gfalse)
}

exp_real atlas_draw_sprite_ext(gm_real id, gm_real subimg, gm_real x, gm_real y,
	gm_real xscale, gm_real yscale, gm_real rot, gm_real color, gm_real alpha)
{
	try
	{
		if (id < IMAGE_START_POSITION)
		{
			atlas::force_draw_to_screen();
			gm::draw_sprite_ext((int)id, (int)subimg, x, y, xscale, yscale, rot,
				(int)color, alpha);
			return gtrue;
		}

		atlas::draw_sprite_ext((uint)id, (uint)subimg, x, y, xscale, yscale, 
			rot * pi / 180, col_d3d((int)color, alpha));
		return gtrue;
	}
	simple_catch("atlas_draw_sprite_ext", gfalse)
}

exp_real atlas_draw_sprite_stretched_ext(gm_real id, gm_real subimg, gm_real x, gm_real y,
	gm_real width, gm_real height, gm_real color, gm_real alpha)
{
	try
	{
		if (id < IMAGE_START_POSITION)
		{
			atlas::force_draw_to_screen();
			gm::draw_sprite_stretched_ext((int)id, (int)subimg, x, y, width, height,
				(int)color, alpha);
			return gtrue;
		}

		atlas::draw_sprite_stretched_ext((uint)id, (uint)subimg, x, y, width, height,
			col_d3d((int)color, alpha));
		return gtrue;
	}
	simple_catch("atlas_draw_sprite_stretched_ext", gfalse)
}

exp_real atlas_draw_sprite_part_ext(gm_real id, gm_real subimg, gm_real left, gm_real top,
	gm_real width, gm_real height, gm_real x, gm_real y, gm_real xscale, gm_real yscale,
	gm_real color)
{
	try
	{
		constexpr int argnum = 1;
		gm_real args[argnum]{};
		if (parse_args(args) < argnum)
			return gfalse;

		gm_real alpha = args[0];

		if (id < IMAGE_START_POSITION)
		{
			atlas::force_draw_to_screen();
			gm::draw_sprite_part_ext((int)id, (int)subimg, (int)left, (int)top,
				(int)width, (int)height, x, y, xscale, yscale, (int)color, alpha);
			return gtrue;
		}

		atlas::draw_sprite_part_ext((uint)id, (uint)subimg, (int)left, (int)top,
			(int)width, (int)height, x, y, xscale, yscale, col_d3d((int)color, alpha));
		return gtrue;
	}
	simple_catch("atlas_draw_sprite_part_ext", gfalse)
}

exp_real atlas_draw_sprite_general(gm_real id, gm_real subimg, gm_real left, gm_real top,
	gm_real width, gm_real height, gm_real x, gm_real y, gm_real xscale, gm_real yscale,
	gm_real rot)
{
	try
	{
		constexpr int argnum = 5;
		gm_real args[argnum]{};
		if (parse_args(args) < argnum)
			return gfalse;

		gm_real c1 = args[0];
		gm_real c2 = args[1];
		gm_real c3 = args[2];
		gm_real c4 = args[3];
		gm_real alpha = args[4];

		if (id < IMAGE_START_POSITION)
		{
			atlas::force_draw_to_screen();
			gm::draw_sprite_general((int)id, (int)subimg, (int)left, (int)top, (int)width, 
				(int)height, x, y, xscale, yscale, rot, (int)c1, (int)c2, (int)c3, (int)c4, alpha);
			return gtrue;
		}

		atlas::draw_sprite_general((uint)id, (uint)subimg, (int)left, (int)top,
			(int)width, (int)height, x, y, xscale, yscale, rot * pi / 180,
			col_d3d((int)c1, alpha), col_d3d((int)c2, alpha), col_d3d((int)c3, alpha),
			col_d3d((int)c4, alpha));
		return gtrue;
	}
	simple_catch("atlas_draw_sprite_general", gfalse)
}

exp_real atlas_draw_background(gm_real id, gm_real x, gm_real y)
{
	try
	{
		if (id < IMAGE_START_POSITION)
		{
			atlas::force_draw_to_screen();
			gm::draw_background((int)id, x, y);
			return gtrue;
		}

		atlas::draw_sprite((uint)id, 0, x, y);
		return gtrue;
	}
	simple_catch("atlas_draw_background", gfalse)
}

exp_real atlas_draw_background_stretched(gm_real id, gm_real x, gm_real y, gm_real width,
	gm_real height)
{
	try
	{
		if (id < IMAGE_START_POSITION)
		{
			atlas::force_draw_to_screen();
			gm::draw_background_stretched((int)id, x, y, width, height);
			return gtrue;
		}

		atlas::draw_sprite_stretched((uint)id, 0, x, y, width, height);
		return gtrue;
	}
	simple_catch("atlas_draw_background_stretched", gfalse)
}

exp_real atlas_draw_background_part(gm_real id, gm_real left, gm_real top, gm_real width,
	gm_real height, gm_real x, gm_real y)
{
	try
	{
		if (id < IMAGE_START_POSITION)
		{
			atlas::force_draw_to_screen();
			gm::draw_background_part((int)id, (int)left, (int)top, (int)width,
				(int)height, x, y);
			return gtrue;
		}

		atlas::draw_sprite_part((uint)id, 0, (int)left, (int)top, (int)width,
			(int)height, x, y);
		return gtrue;
	}
	simple_catch("atlas_draw_background_part", gfalse)
}

exp_real atlas_draw_background_ext(gm_real id, gm_real x, gm_real y, gm_real xscale, 
	gm_real yscale, gm_real rot, gm_real color, gm_real alpha)
{
	try
	{
		if (id < IMAGE_START_POSITION)
		{
			atlas::force_draw_to_screen();
			gm::draw_background_ext((int)id, x, y, xscale, yscale, rot, (int)color, alpha);
			return gtrue;
		}

		atlas::draw_sprite_ext((uint)id, 0, x, y, xscale, yscale, rot * pi / 180,
			col_d3d((int)color, alpha));
		return gtrue;
	}
	simple_catch("atlas_draw_background_ext", gfalse)
}

exp_real atlas_draw_background_stretched_ext(gm_real id, gm_real x, gm_real y, 
	gm_real width, gm_real height, gm_real color, gm_real alpha)
{
	try
	{
		if (id < IMAGE_START_POSITION)
		{
			atlas::force_draw_to_screen();
			gm::draw_background_stretched_ext((int)id, x, y, width, height,
				(int)color, alpha);
			return gtrue;
		}

		atlas::draw_sprite_stretched_ext((uint)id, 0, x, y, width, height,
			col_d3d((int)color, alpha));
		return gtrue;
	}
	simple_catch("atlas_draw_background_stretched_ext", gfalse)
}

exp_real atlas_draw_background_part_ext(gm_real id, gm_real left, gm_real top, 
	gm_real width, gm_real height, gm_real x, gm_real y, gm_real xscale, gm_real yscale, 
	gm_real color, gm_real alpha)
{
	try
	{
		if (id < IMAGE_START_POSITION)
		{
			atlas::force_draw_to_screen();
			gm::draw_background_part_ext((int)id, (int)left, (int)top, (int)width,
				(int)height, x, y, xscale, yscale, (int)color, alpha);
			return gtrue;
		}

		atlas::draw_sprite_part_ext((uint)id, 0, (int)left, (int)top, (int)width,
			(int)height, x, y, xscale, yscale, col_d3d((int)color, alpha));
		return gtrue;
	}
	simple_catch("atlas_draw_background_part_ext", gfalse)
}

exp_real atlas_draw_background_general(gm_real id, gm_real left, gm_real top, 
	gm_real width, gm_real height, gm_real x, gm_real y, gm_real xscale, gm_real yscale, 
	gm_real rot, gm_real c1)
{
	try
	{
		constexpr int argnum = 4;
		gm_real args[argnum]{};
		if (parse_args(args) < argnum)
			return gfalse;

		gm_real c2 = args[0];
		gm_real c3 = args[1];
		gm_real c4 = args[2];
		gm_real alpha = args[3];

		if (id < IMAGE_START_POSITION)
		{
			atlas::force_draw_to_screen();
			gm::draw_background_general((int)id, (int)left, (int)top, (int)width,
				(int)height, x, y, xscale, yscale, rot, (int)c1, (int)c2,
				(int)c3, (int)c4, alpha);
			return gtrue;
		}

		atlas::draw_sprite_general((uint)id, 0, (int)left, (int)top,
			(int)width, (int)height, x, y, xscale, yscale, rot * pi / 180,
			col_d3d((int)c1, alpha), col_d3d((int)c2, alpha), col_d3d((int)c3, alpha),
			col_d3d((int)c4, alpha));
		return gtrue;
	}
	simple_catch("atlas_draw_background_general", gfalse)
}

exp_real force_draw_to_screen()
{
	try
	{
		atlas::force_draw_to_screen();
		return gtrue;
	}
	simple_catch("force_draw_to_screen", gfalse)
}