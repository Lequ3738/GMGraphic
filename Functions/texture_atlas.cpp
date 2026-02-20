#include "load_resources.h"
#include "MaxRectsBinPack.h"
#include "texture_atlas.h"
#include "lodepng.h"

std::unordered_map<uint, std::unique_ptr<texture_atlas>> game_texture_atlas;
std::unordered_map<uint, texture_atlas::images*> game_images;
std::unordered_map<uint, texture_atlas::images::sub_image*> game_textures;

uint texture_atlas_id_position = 0, 
	atlas_image_id = 100000, 
	atlas_texture_id = 100000;

enum class file_type { gmspr, png };

texture_atlas::texture_atlas(uint size, uint id)
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
				texture_atlas::size = size;
				data = std::vector<uchar>(size * size * 4, 0);
				bin = rbp::MaxRectsBinPack(size, size, true);
			}
			break;

			default:
				throw std::runtime_error("Invalid texture atlas size. "
					"Supported sizes are 256, 512, 1024, and 2048.");
		}
	}
	transpond_catch("texture_atlas::texture_atlas(uint)")
}

texture_atlas::~texture_atlas()
{
	for (auto& images : images_list)
	{
		game_images.erase(images->image_id);

		for (auto& sub_image : images->frames)
			game_textures.erase(sub_image->texture_id);
	}

	if (texture == nullptr)
		return;

	texture->Release();
	texture = nullptr;
}

gm::image_rect texture_atlas::crop_blank_area(gm::image_data& image)
{
	try
	{
		std::vector<uchar>& data = std::get<0>(image);
		int width = (int)std::get<1>(image);
		int height = (int)std::get<2>(image);

		if (data.size() != width * height * 4)
			throw std::runtime_error("Image data size does not match width and height.");

		int top = 0, bottom = 0, left = 0, right = 0;

	calculate_top:
		for (int y = 0; y < height; ++y)
		{
			for (int x = 0; x < width; ++x)
			{
				if (data[(y * width + x) * 4 + 3] > 0)
				{
					top = y;
					goto calculate_bottom;
				}
			}
		}

		return { 0, 0, 0, 0 };

	calculate_bottom:
		for (int y = height - 1; y >= top; --y)
		{
			for (int x = 0; x < width; ++x)
			{
				if (data[(y * width + x) * 4 + 3] > 0)
				{
					bottom = y + 1;
					goto calculate_left;
				}
			}
		}

	calculate_left:
		for (int x = 0; x < width; ++x)
		{
			for (int y = 0; y < height; ++y)
			{
				if (data[(y * width + x) * 4 + 3] > 0)
				{
					left = x;
					goto calculate_right;
				}
			}
		}

	calculate_right:
		for (int x = width - 1; x >= left; --x)
		{
			for (int y = 0; y < height; ++y)
			{
				if (data[(y * width + x) * 4 + 3] > 0)
				{
					right = x + 1;
					goto calculate_result;
				}
			}
		}

	calculate_result:
		return { left, top, right, bottom };
	}
	transpond_catch("texture_atlas::crop_blank_area(gm::image_data&)")
}

void texture_atlas::add_image_to_memory(std::vector<uchar>& image_data, rbp::Rect& rect, 
	texture_atlas::images::sub_image& image)
{
	try
	{
		const int img_w = (int)image.texture_width;
		const int img_h = (int)image.texture_height;

		if (!image.is_rotated)
		{
			const int dst_x = rect.x + 1;
			const int dst_y = rect.y + 1;
			const int copy_w = std::min(rect.width - 2, img_w);
			const int copy_h = std::min(rect.height - 2, img_h);

			for (int y = 0; y < copy_h; ++y)
			{
				uchar* const dst_ptr = data.data() + ((dst_y + y) * size + dst_x) * 4;
				const uchar* const src_ptr = image_data.data() + (y * img_w) * 4;
				std::memcpy(dst_ptr, src_ptr, copy_w * 4);
			}
		}
		else
		{
			const int dst_x = rect.x + 1;
			const int dst_y = rect.y + 1;
			const int copy_w = std::min(rect.width - 2, img_h);
			const int copy_h = std::min(rect.height - 2, img_w);

			for (int v = 0; v < copy_h; ++v)
			{
				for (int u = 0; u < copy_w; ++u)
				{
					uchar* const dst_pixel = data.data() + ((dst_y + v) * size + 
						dst_x + u) * 4;

					const int src_x = v;
					const int src_y = img_h - 1 - u;

					if (src_x < img_w && src_y < img_h)
					{
						const uchar* src_pixel = image_data.data() + (src_y * img_w + src_x) * 4;
						*(uint32_t*)dst_pixel = *(uint32_t*)src_pixel;
					}
				}
			}
		}
	}
	transpond_catch("add_image_to_memory(texture_atlas&, std::vector<uchar>&, "
		"rbp::Rect&, images::image&)")
}

uint texture_atlas::write_image_data(gm::sprite& spr)
{
	// 裁剪空白区域
	std::vector<gm::image_rect> cropped_rects(spr.frame_count());
	std::vector<rbp::RectSize> pack_rect_sizes(spr.frame_count());

	for (uint i = 0; i < spr.frame_count(); ++i)
	{
		gm::image_data image{ spr.data[i], spr.width, spr.height };
		cropped_rects[i] = crop_blank_area(image);

		if (cropped_rects[i].right <= cropped_rects[i].left || 
			cropped_rects[i].bottom <= cropped_rects[i].top)
		{
			cropped_rects[i] = { 0, 0, 0, 0 };
			continue;
		}
		
		// 每个子图增加 1 像素边距，以避免采样时的边界问题
		pack_rect_sizes[i] = { cropped_rects[i].right - cropped_rects[i].left + 2, 
			cropped_rects[i].bottom - cropped_rects[i].top + 2 };
	}

	// 面积预检查
	uint needed_area = 0;
	for (uint i = 0; i < spr.frame_count(); ++i)
		needed_area += pack_rect_sizes[i].width * pack_rect_sizes[i].height;

	if (bin.Occupancy() + (double)needed_area / (size * size) > 1.0)
		return -1;

	// 进行打包计算
	rbp::MaxRectsBinPack backup = bin;  // 备份当前打包器状态
	std::vector<rbp::Rect> result(spr.frame_count());

	for (uint i = 0; i < spr.frame_count(); ++i)
	{
		rbp::Rect node = bin.Insert(pack_rect_sizes[i].width, pack_rect_sizes[i].height,
			rbp::MaxRectsBinPack::RectBestShortSideFit);

		if (node.height == 0)  // 若任一子图打包失败，则整体打包失败
		{
			bin = std::move(backup);  // 恢复打包器状态
			return -1;
		}

		result[i] = node;
	}

	// 写入图集数据
	uint image_id = atlas_image_id++;
	images_ptr atlas_images = std::make_unique<images>(spr.width, spr.height,
		std::vector<images::sub_image_ptr>(spr.frame_count()), image_id, id);

	for (uint i = 0; i < spr.frame_count(); ++i)
	{
		int orig_x = spr.xorig - cropped_rects[i].left;
		int orig_y = spr.yorig - cropped_rects[i].top - 1;

		atlas_images->frames[i] = std::make_unique<images::sub_image>(
			(uint)result[i].x + 1, (uint)result[i].y + 1, 
			(uint)pack_rect_sizes[i].width - 2, (uint)pack_rect_sizes[i].height - 2,
			orig_x, orig_y, result[i].width != pack_rect_sizes[i].width, 
			atlas_texture_id);

		game_textures[atlas_texture_id] = atlas_images->frames[i].get();
		atlas_texture_id++;

		add_image_to_memory(spr.data[i], result[i], *atlas_images->frames[i]);
	}

	game_images[image_id] = atlas_images.get();
	images_list.emplace_back(std::move(atlas_images));

	return image_id;
}

uint texture_atlas::add_image(std::string& image_file)
{
	try
	{
		if (read_only())
			throw std::runtime_error("Cannot add image to a burned atlas.");

		// 解析图像文件
		file_type kind = file_type::gmspr;
		gm::sprite spr;

		gm_string ext = gm::filename_ext(image_file);
		if (strcmp(ext, ".png") == 0)
			kind = file_type::png;
		else if (strcmp(ext, ".gmspr") == 0)
			kind = file_type::gmspr;
		else
		{
			throw std::runtime_error("Unsupported file type for adding to texture atlas: " +
				std::string(ext));
		}

		if (kind == file_type::gmspr)
			spr = gm::decode_gmspr(image_file);
		else
			spr = gm::decode_png(image_file);

		return write_image_data(spr);
	}
	transpond_catch("texture_atlas::add_image(std::string&)")
}

uint texture_atlas::add_sprite(uint id)
{
	try
	{
		if (read_only())
			throw std::runtime_error("Cannot add image to a burned atlas.");

		gm::sprite spr = gm::get_sprite_data(id);
		return write_image_data(spr);
	}
	transpond_catch("texture_atlas::add_sprite(uint)");
}

uint texture_atlas::add_background(uint id)
{
	try
	{
		if (read_only())
			throw std::runtime_error("Cannot add image to a burned atlas.");

		gm::sprite spr = gm::get_background_data(id);
		return write_image_data(spr);
	}
	transpond_catch("texture_atlas::add_background(uint)");
}

bool texture_atlas::burn(bool del_memdata)
{
	try
	{
		if (read_only())
			return false;

		IDirect3DDevice8* device = gmapi->GetDirect3DDevice();
		IDirect3DTexture8* texture = nullptr;

		if (texture == nullptr)
		{
			D3DCheck(device->CreateTexture(size, size, 1, 0, D3DFMT_A8R8G8B8,
				D3DPOOL_DEFAULT, &texture), 0);
			texture_atlas::texture = texture;
		}
		else
			texture = texture_atlas::texture;

		IDirect3DSurface8* surface = nullptr;
		D3DCheck(texture->GetSurfaceLevel(0, &surface), 1);

		RECT rect = { .left = 0, .top = 0, .right = (long)size, .bottom = (long)size };
		D3DCheck(D3DXLoadSurfaceFromMemory(surface, nullptr, &rect, data.data(),
			D3DFMT_A8R8G8B8, size * 4, nullptr, &rect, D3DX_FILTER_NONE, 0), 2);

		D3DCheck(texture->AddDirtyRect(&rect), 3);
		surface->Release();

		if (del_memdata)
			data.clear();

		return true;
	}
	transpond_catch("texture_atlas::burn(bool)")
}

void texture_atlas::save(path& file_path) const
{
	try
	{
		if (texture == nullptr)
		{
			throw std::runtime_error("This texture set has not been uploaded to the GPU end.\r\n"
				"Path: " + file_path.string());
		}

		auto [d3dimage, w, h] = gm::get_image_data(texture);
		std::vector<uchar> image(d3dimage.size());

		for (size_t i = 0; i < d3dimage.size(); i += 4)
		{
			image[i] = d3dimage[i + 2];     // B -> R
			image[i + 1] = d3dimage[i + 1]; // G
			image[i + 2] = d3dimage[i];     // R -> B
			image[i + 3] = d3dimage[i + 3]; // A
		}

		lodepng::encode(file_path.string(), image.data(), w, h);
	}
	transpond_catch("texture_atlas::save(path&)")
}

// ============================================================================
// Export Functions
// ============================================================================

exp_real texture_atlas_create(gm_real size)
{
	try
	{
		uint id = texture_atlas_id_position++;

		game_texture_atlas[id] = std::make_unique<texture_atlas>((uint)size, id);
		return (gm_real)id;
	}
	simple_catch("texture_atlas_create", -1)
}

exp_real texture_atlas_delete(gm_real id)
{
	try
	{
		game_texture_atlas.erase((uint)id);
		return gtrue;
	}
	simple_catch("texture_atlas_delete", gfalse)
}

exp_real texture_atlas_add_file(gm_real id, gm_string file)
{
	try
	{
		texture_atlas& atlas = *game_texture_atlas.at((uint)id);
		std::string file(file);
		return (gm_real)atlas.add_image(file);
	}
	simple_catch("texture_atlas_add_file", -1)
}

exp_real texture_atlas_add_sprite(gm_real id, gm_real spr)
{
	try
	{
		texture_atlas& atlas = *game_texture_atlas.at((uint)id);
		return (gm_real)atlas.add_sprite((uint)spr);
	}
	simple_catch("texture_atlas_add_sprite", -1)
}

exp_real texture_atlas_add_background(gm_real id, gm_real back)
{
	try
	{
		texture_atlas& atlas = *game_texture_atlas.at((uint)id);
		return (gm_real)atlas.add_sprite((uint)back);
	}
	simple_catch("texture_atlas_add_sprite", -1)
}

exp_real texture_atlas_burn(gm_real id, gm_real del_memdata)
{
	try
	{
		texture_atlas& atlas = *game_texture_atlas.at((uint)id);
		return (gm_real)atlas.burn((bool)del_memdata);
	}
	simple_catch("texture_atlas_burn", gfalse)
}

exp_real texture_atlas_save(gm_real id, gm_string file_path)
{
	try
	{
		texture_atlas& atlas = *game_texture_atlas.at((uint)id);
		path p(file_path);
		atlas.save(p);
		return gtrue;
	}
	simple_catch("texture_atlas_save", gfalse)
}

exp_real texture_atlas_auto_add_file(gm_string file)
{
	try
	{
		for (auto& [atlas_id, atlas] : game_texture_atlas)
		{
			if (atlas->read_only())
				continue;

			std::string f(file);
			if (atlas->add_image(f))
				return atlas_id;
		}

		uint id = texture_atlas_id_position++;

		game_texture_atlas[id] = std::make_unique<texture_atlas>(1024, id);
		texture_atlas& atlas = *game_texture_atlas[id];

		std::string f(file);
		return atlas.add_image(f) ? id : -1;
	}
	simple_catch("texture_atlas_auto_add_file", -1)
}

exp_real texture_atlas_auto_add_sprite(gm_real spr)
{
	try
	{
		for (auto& [atlas_id, atlas] : game_texture_atlas)
		{
			if (atlas->read_only())
				continue;

			if (atlas->add_sprite((uint)spr))
				return atlas_id;
		}

		uint id = texture_atlas_id_position++;

		game_texture_atlas[id] = std::make_unique<texture_atlas>(1024, id);
		texture_atlas& atlas = *game_texture_atlas[id];

		return atlas.add_sprite((uint)spr) ? id : -1;
	}
	simple_catch("texture_atlas_auto_add_sprite", -1)
}

exp_real texture_atlas_auto_add_background(gm_real back)
{
	try
	{
		for (auto& [atlas_id, atlas] : game_texture_atlas)
		{
			if (atlas->read_only())
				continue;

			if (atlas->add_background((uint)back))
				return atlas_id;
		}

		uint id = texture_atlas_id_position++;

		game_texture_atlas[id] = std::make_unique<texture_atlas>(1024, id);
		texture_atlas& atlas = *game_texture_atlas[id];

		return atlas.add_background((uint)back) ? id : -1;
	}
	simple_catch("texture_atlas_auto_add_sprite", -1)
}

exp_real texture_atlas_auto_finish(gm_real dont_twice)
{
	try
	{
		for (auto& [atlas_id, atlas] : game_texture_atlas)
		{
			if ((atlas->texture == nullptr && dont_twice) || !dont_twice)
				atlas->burn();
		}

		return gtrue;
	}
	simple_catch("texture_atlas_auto_finish", gfalse)
}

exp_real texture_atlas_count()
{
	return (gm_real)game_texture_atlas.size();
}

exp_real texture_atlas_exists(gm_real id)
{
	return game_texture_atlas.count((uint)id) ? gtrue : gfalse;
}

exp_real texture_atlas_find_first()
{
	return game_texture_atlas.empty() ? -1 : 
		(gm_real)game_texture_atlas.begin()->first;
}

exp_real texture_atlas_find_next(gm_real id)
{
	auto it = game_texture_atlas.find((uint)id);
	if (it == game_texture_atlas.end() || std::next(it) == game_texture_atlas.end())
		return -1;
	return (gm_real)std::next(it)->first;
}

exp_real texture_atlas_find_last()
{
	return game_texture_atlas.empty() ? -1 : 
		(gm_real)std::prev(game_texture_atlas.end())->first;
}