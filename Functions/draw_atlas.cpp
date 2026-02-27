#include "texture_atlas.h"
#include "shader.h"
#include "draw_atlas.h"
#include "math_s.h"

static texture_atlas* current_atlas = nullptr;

static void draw_image(atlas::draw_info& info)
{
	try
	{
		// -0.5 为半像素偏移修正，详细内容见 https://zhuanlan.zhihu.com/p/639407618
		double left = -info.sub_image.orig_x * info.trans.xscale - 0.5;
		double top = -info.sub_image.orig_y * info.trans.yscale - 0.5;
		double right = (info.sub_image.texture_width - info.sub_image.orig_x) *
			info.trans.xscale - 0.5;
		double bottom = (info.sub_image.texture_height - info.sub_image.orig_y) *
			info.trans.yscale - 0.5;

		if (&info.atlas != current_atlas)
		{
			if (current_atlas != nullptr)
			{
				IDirect3DDevice8* device = gmapi->GetDirect3DDevice();

				d3d_set_tex_all(-1);
				D3DCheck(device->SetTexture(0, current_atlas->texture), 0);
				D3DCheck(device->SetTextureStageState(0, D3DTSS_ADDRESSU, D3DTADDRESS_CLAMP), 1);
				D3DCheck(device->SetTextureStageState(0, D3DTSS_ADDRESSV, D3DTADDRESS_CLAMP), 2);

				vertex::end();
			}

			current_atlas = &info.atlas;
			if (current_atlas->texture == nullptr)
				throw std::runtime_error("Cannot find the texture atlas of this sprite.");

			vertex::begin(D3DPT_TRIANGLELIST, true);
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
		vert_ext* vert = vertex::get_struct();
		vert->x = x_lt; vert->y = y_lt; vert->c = info.color;
		if (!info.sub_image.is_rotated)
		{
			vert->uv[0] = (float)info.sub_image.texture_left / (float)info.atlas.size;
			vert->uv[1] = (float)info.sub_image.texture_top / (float)info.atlas.size;
		}
		else
		{
			vert->uv[0] = (float)(info.sub_image.texture_left + 
				info.sub_image.texture_height) / (float)info.atlas.size;
			vert->uv[1] = (float)info.sub_image.texture_top / (float)info.atlas.size;
		}

		vert = vertex::get_struct();
		vert->x = x_rt; vert->y = y_rt; vert->c = info.color;
		if (!info.sub_image.is_rotated)
		{
			vert->uv[0] = (float)(info.sub_image.texture_left +
				info.sub_image.texture_width) / (float)info.atlas.size;
			vert->uv[1] = (float)info.sub_image.texture_top / (float)info.atlas.size;
		}
		else
		{
			vert->uv[0] = (float)(info.sub_image.texture_left +
				info.sub_image.texture_height) / (float)info.atlas.size;
			vert->uv[1] = (float)(info.sub_image.texture_top + 
				info.sub_image.texture_width) / (float)info.atlas.size;
		}

		vert = vertex::get_struct();
		vert->x = x_rb; vert->y = y_rb; vert->c = info.color;
		if (!info.sub_image.is_rotated)
		{
			vert->uv[0] = (float)(info.sub_image.texture_left +
				info.sub_image.texture_width) / (float)info.atlas.size;
			vert->uv[1] = (float)(info.sub_image.texture_top +
				info.sub_image.texture_height) / (float)info.atlas.size;
		}
		else
		{
			vert->uv[0] = (float)info.sub_image.texture_left / (float)info.atlas.size;
			vert->uv[1] = (float)(info.sub_image.texture_top +
				info.sub_image.texture_width) / (float)info.atlas.size;
		}

		// 三角形 2
		vert = vertex::get_struct();
		vert->x = x_lt; vert->y = y_lt; vert->c = info.color;
		if (!info.sub_image.is_rotated)
		{
			vert->uv[0] = (float)info.sub_image.texture_left / (float)info.atlas.size;
			vert->uv[1] = (float)info.sub_image.texture_top / (float)info.atlas.size;
		}
		else
		{
			vert->uv[0] = (float)(info.sub_image.texture_left +
				info.sub_image.texture_height) / (float)info.atlas.size;
			vert->uv[1] = (float)info.sub_image.texture_top / (float)info.atlas.size;
		}

		vert = vertex::get_struct();
		vert->x = x_rb; vert->y = y_rb; vert->c = info.color;
		if (!info.sub_image.is_rotated)
		{
			vert->uv[0] = (float)(info.sub_image.texture_left +
				info.sub_image.texture_width) / (float)info.atlas.size;
			vert->uv[1] = (float)(info.sub_image.texture_top +
				info.sub_image.texture_height) / (float)info.atlas.size;
		}
		else
		{
			vert->uv[0] = (float)info.sub_image.texture_left / (float)info.atlas.size;
			vert->uv[1] = (float)(info.sub_image.texture_top +
				info.sub_image.texture_width) / (float)info.atlas.size;
		}

		vert = vertex::get_struct();
		vert->x = x_lb; vert->y = y_lb; vert->c = info.color;
		if (!info.sub_image.is_rotated)
		{
			vert->uv[0] = (float)info.sub_image.texture_left / (float)info.atlas.size;
			vert->uv[1] = (float)(info.sub_image.texture_top +
				info.sub_image.texture_height) / (float)info.atlas.size;
		}
		else
		{
			vert->uv[0] = (float)info.sub_image.texture_left / (float)info.atlas.size;
			vert->uv[1] = (float)info.sub_image.texture_top / (float)info.atlas.size;
		}
	}
	transpond_catch("draw_image(atlas::draw_info&)")
}

void atlas::draw_sprite(uint id, uint subimg, double x, double y)
{
	try
	{
		texture_atlas::images* images_ptr = game_images.at(id);
		texture_atlas* atlas_ptr = nullptr;
		texture_atlas::images::sub_image* sub_image_ptr = nullptr;

		if (images_ptr != nullptr)
		{
			atlas_ptr = game_texture_atlas.at(images_ptr->atlas_id).get();
			sub_image_ptr = images_ptr->frames.at(subimg).get();
		}
		else
			throw std::runtime_error("Cannot find the sprite.");

		if (atlas_ptr == nullptr)
			throw std::runtime_error("Cannot find the texture atlas of this sprite.");

		if (sub_image_ptr == nullptr)  // 如果该子图像不存在（可能是空白图），则不进行绘制
			return;

		draw_info info = {
			.atlas = *atlas_ptr, .images = *images_ptr, .sub_image = *sub_image_ptr,
			.trans = {.x = x, .y = y}
		};
		draw_image(info);
	}
	transpond_catch("atlas::draw_sprite(uint, uint, double, double)")
}

void atlas::draw_sprite_ext(uint id, uint subimg, double x, double y, double xscale, 
	double yscale, double rot, d3dcolor color)
{
	try
	{
		texture_atlas::images* images_ptr = game_images.at(id);
		texture_atlas* atlas_ptr = nullptr;
		texture_atlas::images::sub_image* sub_image_ptr = nullptr;

		if (images_ptr != nullptr)
		{
			atlas_ptr = game_texture_atlas.at(images_ptr->atlas_id).get();
			sub_image_ptr = images_ptr->frames.at(subimg).get();
		}
		else
			throw std::runtime_error("Cannot find the sprite.");

		if (atlas_ptr == nullptr)
			throw std::runtime_error("Cannot find the texture atlas of this sprite.");

		if (sub_image_ptr == nullptr)  // 如果该子图像不存在（可能是空白图），则不进行绘制
			return;

		draw_info info = {
			.atlas = *atlas_ptr, .images = *images_ptr, .sub_image = *sub_image_ptr,
			.trans = {.x = x, .y = y, .xscale = xscale, .yscale = yscale, .rot = rot},
			.color = color
		};
		draw_image(info);
	}
	transpond_catch("atlas::draw_sprite_ext(uint, uint, double, double, double, "
		"double, double, d3dcolor)")
}

void atlas::force_draw_to_screen()
{
	try
	{
		if (current_atlas != nullptr)
		{
			IDirect3DDevice8* device = gmapi->GetDirect3DDevice();

			d3d_set_tex_all(-1);
			D3DCheck(device->SetTexture(0, current_atlas->texture), 0);
			D3DCheck(device->SetTextureStageState(0, D3DTSS_ADDRESSU, D3DTADDRESS_CLAMP), 1);
			D3DCheck(device->SetTextureStageState(0, D3DTSS_ADDRESSV, D3DTADDRESS_CLAMP), 2);

			vertex::end();
		}
		current_atlas = nullptr;
	}
	transpond_catch("atlas::force_draw_to_screen()")
}

exp_real atlas_draw_sprite(gm_real id, gm_real subimg, gm_real x, gm_real y)
{
	try
	{
		if (id < IMAGE_START_POSITION)
		{
			gm::draw_sprite((int)id, (int)subimg, x, y);
			return gtrue;
		}
		
		atlas::draw_sprite((uint)id, (uint)subimg, x, y);
		return gtrue;
	}
	simple_catch("atlas_draw_sprite", gerror)
}

exp_real atlas_draw_sprite_ext(gm_real id, gm_real subimg, gm_real x, gm_real y,
	gm_real xscale, gm_real yscale, gm_real rot, gm_real color, gm_real alpha)
{
	try
	{
		if (id < IMAGE_START_POSITION)
		{
			gm::draw_sprite_ext((int)id, (int)subimg, x, y, xscale, yscale, rot,
				(int)color, alpha);
			return gtrue;
		}

		atlas::draw_sprite_ext((uint)id, (uint)subimg, x, y, xscale, yscale, 
			rot * pi / 180, col_d3d((int)color, alpha));
		return gtrue;
	}
	simple_catch("atlas_draw_sprite_ext", gerror)
}

exp_real atlas_draw_background(gm_real id, gm_real x, gm_real y)
{
	try
	{
		if (id < IMAGE_START_POSITION)
		{
			gm::draw_background((int)id, x, y);
			return gtrue;
		}

		atlas::draw_sprite((uint)id, 0, x, y);
		return gtrue;
	}
	simple_catch("atlas_draw_background", gerror)
}

exp_real atlas_draw_background_ext(gm_real id, gm_real x, gm_real y, gm_real xscale, 
	gm_real yscale, gm_real rot, gm_real color, gm_real alpha)
{
	try
	{
		if (id < IMAGE_START_POSITION)
		{
			gm::draw_background_ext((int)id, x, y, xscale, yscale, rot, (int)color, alpha);
			return gtrue;
		}

		atlas::draw_sprite_ext((uint)id, 0, x, y, xscale, yscale, rot * pi / 180,
			col_d3d((int)color, alpha));
		return gtrue;
	}
	simple_catch("atlas_draw_background_ext", gerror)
}

exp_real force_draw_to_screen()
{
	try
	{
		atlas::force_draw_to_screen();
		return gtrue;
	}
	simple_catch("force_draw_to_screen", gerror)
}