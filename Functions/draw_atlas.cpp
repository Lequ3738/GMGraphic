#include "texture_atlas.h"
#include "shader.h"
#include "draw_atlas.h"

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
			IDirect3DDevice8* device = gmapi->GetDirect3DDevice();

			if (current_atlas != nullptr)
				vertex::end();

			current_atlas = &info.atlas;
			if (current_atlas->texture == nullptr)
				throw std::runtime_error("");

			vertex::begin(D3DPT_TRIANGLELIST, true);
			D3DCheck(device->SetTexture(0, current_atlas->texture), 0);
		}

		if (std::abs(info.trans.rot) > 0.00000001)
		{

		}
		else
		{
			float x1 = static_cast<float>(info.trans.x + left);
			float y1 = static_cast<float>(info.trans.y + top);
			float x2 = static_cast<float>(info.trans.x + right);
			float y2 = static_cast<float>(info.trans.y + bottom);

			// 三角形 1
			vert_ext& vert = vertex::get_struct();
			vert = { .x = x1, .y = y1, .c = info.color };
			if (!info.sub_image.is_rotated)
			{
				vert.uv[0] = (float)info.sub_image.texture_left / (float)info.atlas.size;
				vert.uv[1] = (float)info.sub_image.texture_top / (float)info.atlas.size;
			}
			else
			{
				vert.uv[0] = (float)(info.sub_image.texture_left + 
					info.sub_image.texture_height) / (float)info.atlas.size;
				vert.uv[1] = (float)info.sub_image.texture_top / (float)info.atlas.size;
			}

			vert = vertex::get_struct();
			vert = { .x = x2, .y = y1, .c = info.color };
			if (!info.sub_image.is_rotated)
			{
				vert.uv[0] = (float)(info.sub_image.texture_left +
					info.sub_image.texture_width) / (float)info.atlas.size;
				vert.uv[1] = (float)info.sub_image.texture_top / (float)info.atlas.size;
			}
			else
			{
				vert.uv[0] = (float)(info.sub_image.texture_left +
					info.sub_image.texture_height) / (float)info.atlas.size;
				vert.uv[1] = (float)(info.sub_image.texture_top + 
					info.sub_image.texture_width) / (float)info.atlas.size;
			}

			vert = vertex::get_struct();
			vert = { .x = x2, .y = y2, .c = info.color };
			if (!info.sub_image.is_rotated)
			{
				vert.uv[0] = (float)(info.sub_image.texture_left +
					info.sub_image.texture_width) / (float)info.atlas.size;
				vert.uv[1] = (float)(info.sub_image.texture_top +
					info.sub_image.texture_height) / (float)info.atlas.size;
			}
			else
			{
				vert.uv[0] = (float)info.sub_image.texture_left / (float)info.atlas.size;
				vert.uv[1] = (float)(info.sub_image.texture_top +
					info.sub_image.texture_width) / (float)info.atlas.size;
			}

			// 三角形 2
			vert = vertex::get_struct();
			vert = { .x = x1, .y = y1, .c = info.color };
			if (!info.sub_image.is_rotated)
			{
				vert.uv[0] = (float)info.sub_image.texture_left / (float)info.atlas.size;
				vert.uv[1] = (float)info.sub_image.texture_top / (float)info.atlas.size;
			}
			else
			{
				vert.uv[0] = (float)(info.sub_image.texture_left +
					info.sub_image.texture_height) / (float)info.atlas.size;
				vert.uv[1] = (float)info.sub_image.texture_top / (float)info.atlas.size;
			}

			vert = vertex::get_struct();
			vert = { .x = x2, .y = y2, .c = info.color };
			if (!info.sub_image.is_rotated)
			{
				vert.uv[0] = (float)(info.sub_image.texture_left +
					info.sub_image.texture_width) / (float)info.atlas.size;
				vert.uv[1] = (float)(info.sub_image.texture_top +
					info.sub_image.texture_height) / (float)info.atlas.size;
			}
			else
			{
				vert.uv[0] = (float)info.sub_image.texture_left / (float)info.atlas.size;
				vert.uv[1] = (float)(info.sub_image.texture_top +
					info.sub_image.texture_width) / (float)info.atlas.size;
			}

			vert = vertex::get_struct();
			vert = { .x = x1, .y = y2, .c = info.color };
			if (!info.sub_image.is_rotated)
			{
				vert.uv[0] = (float)info.sub_image.texture_left / (float)info.atlas.size;
				vert.uv[1] = (float)(info.sub_image.texture_top +
					info.sub_image.texture_height) / (float)info.atlas.size;
			}
			else
			{
				vert.uv[0] = (float)info.sub_image.texture_left / (float)info.atlas.size;
				vert.uv[1] = (float)info.sub_image.texture_top / (float)info.atlas.size;
			}
		}
	}
	transpond_catch("draw_image(atlas::draw_info&)")
}

void atlas::draw_sprite(uint id, uint subimg, double x, double y)
{
	try
	{
		if (id < IMAGE_START_POSITION)
		{
			gm::draw_sprite((int)id, (int)subimg, x, y);
			return;
		}

		texture_atlas::images* images_ptr = game_images.at(id);
		texture_atlas* atlas_ptr = nullptr;
		texture_atlas::images::sub_image* sub_image_ptr = nullptr;
		if (images_ptr)
		{
			atlas_ptr = game_texture_atlas.at(images_ptr->atlas_id).get();
			sub_image_ptr = images_ptr->frames.at(subimg).get();
		}

		draw_info info = {
			.atlas = *atlas_ptr, .images = *images_ptr, .sub_image = *sub_image_ptr,
			.trans = {.x = x, .y = y}
		};
		draw_image(info);
	}
	transpond_catch("atlas::draw_sprite(uint, uint, double, double)")
}

void atlas::force_draw_to_screen()
{
	try
	{
		if (current_atlas != nullptr)
			vertex::end();
		current_atlas = nullptr;
	}
	transpond_catch("atlas::force_draw_to_screen()")
}

exp_real atlas_draw_sprite(gm_real id, gm_real subimg, gm_real x, gm_real y)
{
	try
	{
		atlas::draw_sprite((uint)id, (uint)subimg, x, y);
		return gtrue;
	}
	simple_catch("atlas_draw_sprite", gerror)
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