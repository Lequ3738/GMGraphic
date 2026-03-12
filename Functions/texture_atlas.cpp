#include "texture_atlas.h"
#include "lodepng.h"

std::unordered_map<uint, std::unique_ptr<texture_atlas>> game_texture_atlas;
std::unordered_map<uint, texture_atlas::images*> game_images;
std::unordered_map<uint, texture_atlas::images::sub_image*> game_textures;

uint texture_atlas_id_position = 1, 
	atlas_image_id = IMAGE_START_POSITION,
	atlas_texture_id = TEXTURE_START_POSITION;

enum class file_type { gmspr, png };

bool texture_atlas::texture_amplification = false;

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
				texture_atlas::id = id;
				data = std::vector<uchar>(size * size * 4, 0);
				bin = rbp::MaxRectsBinPack(size, size, true);
			}
			break;

			default:
				throw std::runtime_error("Invalid texture atlas size. "
					"Supported sizes are 256, 512, 1024, and 2048.");
		}

#ifdef _DEBUG
		OutputDebugStringA(("Texture atlas " + std::to_string(id) + " created with size " + 
			std::to_string(size) + ".\n").c_str());
#endif
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

#ifdef _DEBUG
	OutputDebugStringA(("Texture atlas " + std::to_string(id) + " destroyed.\n").c_str());
#endif
}

void texture_atlas::add_image_to_memory(std::vector<uchar>& image_data, 
	copy_image_rect& rect)
{
	try
	{
		if (!rect.is_rotated)
		{
			int top = 0, bottom = (int)rect.texture_height - 1;
			if (texture_amplification)
			{
				top = -1;
				bottom = (int)rect.texture_height;
			}

			// 上下各扩增1像素，以匹配 GameMaker 在边缘插值的特性
			for (int y = top; y <= bottom; ++y)
			{
				uint dst_y = rect.draw_y + (uint)y;
				uint src_y = (rect.bleed_y + (uint)std::clamp(y, 0, (int)rect.texture_height - 1));

				uchar* const dst_ptr = data.data() + (dst_y * size + rect.draw_x) * 4;
				const uchar* const src_ptr = image_data.data() + (src_y * rect.image_width + 
					rect.bleed_x) * 4;

				std::memcpy(dst_ptr, src_ptr, rect.texture_width * 4);

				if (!texture_amplification)
					continue;

				*((uint32_t*)(dst_ptr - 4)) = *(uint32_t*)src_ptr;  // 处理左边缘重复

				// 处理右边缘重复
				uint32_t right_pixel = *(uint32_t*)(src_ptr + (rect.texture_width - 1) * 4);
				*(uint32_t*)(dst_ptr + rect.texture_width * 4) = right_pixel;
			}
		}
		else
		{
			uint dest_w = rect.texture_height;
			uint dest_h = rect.texture_width;

			int left = 0, right = (int)dest_w - 1, top = 0, bottom = (int)dest_h - 1;
			if (texture_amplification)
			{
				left = -1;
				right = (int)dest_w;
				top = -1;
				bottom = (int)dest_h;
			}

			for (int v = top; v <= bottom; ++v)
			{
				for (int u = left; u <= right; ++u)
				{
					uchar* const dst_pixel = data.data() + ((rect.draw_y + v) * 
						size + rect.draw_x + u) * 4;

					const uint src_x = rect.bleed_x + (uint)std::clamp(v, 0, (int)dest_h - 1);
					const uint src_y = rect.bleed_y + (rect.texture_height - 1 - 
						(uint)std::clamp(u, 0, (int)dest_w - 1));

					const uchar* src_pixel = image_data.data() + (src_y * 
						rect.image_width + src_x) * 4;

					*(uint32_t*)dst_pixel = *(uint32_t*)src_pixel;
				}
			}
		}
	}
	transpond_catch("texture_atlas::add_image_to_memory(std::vector<uchar>&," 
		"images::sub_image&, copy_image_rect&)")
}

int texture_atlas::add_image(gm::sprite& spr)
{
	try
	{
		if (read_only())
			throw std::runtime_error("Cannot add image to a burned atlas.");

		// 计算要绘制的子图的宽和高
		std::vector<rbp::RectSize> pack_rect_sizes(spr.frame_count());
		for (uint i = 0; i < spr.frame_count(); ++i)
		{
			// 每个子图增加 1 像素边距（共2像素），以避免采样时的边界问题
			// 宽 = 右 - 左 + 1；高 = 下 - 上 + 1
			pack_rect_sizes[i] = {
				spr.cropped_rects[i].right - spr.cropped_rects[i].left + 3,
				spr.cropped_rects[i].bottom - spr.cropped_rects[i].top + 3
			};
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
			if (spr.cropped_rects[i] == gm::image_rect{ 0, 0, 0, 0 })  // Is blank image
			{
				result[i] = { 0, 0, 0, 0 };
				continue;
			}

			rbp::Rect node = bin.Insert(pack_rect_sizes[i].width, 
				pack_rect_sizes[i].height,
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
		atlas_images->name = spr.name;

		for (uint i = 0; i < spr.frame_count(); ++i)
		{
			bool is_blank = spr.cropped_rects[i] == gm::image_rect{ 0, 0, 0, 0 };
			if (is_blank)
			{
				atlas_images->frames[i] = nullptr;
				game_textures[atlas_texture_id] = nullptr;
				atlas_texture_id++;

				continue;
			}

			uint img_x = (uint)result[i].x + 1, img_y = (uint)result[i].y + 1;
			uint tx = (uint)pack_rect_sizes[i].width - 2;
			uint ty = (uint)pack_rect_sizes[i].height - 2;
			int orig_x = spr.xorig - spr.cropped_rects[i].left;
			int orig_y = spr.yorig - spr.cropped_rects[i].top;
			bool is_rotated = result[i].width != pack_rect_sizes[i].width;
			
			atlas_images->frames[i] = std::make_unique<images::sub_image>(
				img_x, img_y, tx, ty, orig_x, orig_y, is_rotated, atlas_texture_id, 
				image_id);

			game_textures[atlas_texture_id] = atlas_images->frames[i].get();
			atlas_texture_id++;

			copy_image_rect rect = {
				.draw_x = img_x, .draw_y = img_y,
				.texture_width = tx, .texture_height = ty,

				.bleed_x = (uint)spr.cropped_rects[i].left,
				.bleed_y = (uint)spr.cropped_rects[i].top,
				.image_width = spr.width, .image_height = spr.height,
				
				.is_rotated = is_rotated
			};

			add_image_to_memory(spr.data[i], rect);
		}

		game_images[image_id] = atlas_images.get();
		images_list.push_back(std::move(atlas_images));

		return (int)image_id;
	}
	transpond_catch("texture_atlas::add_image(gm::sprite&)")
}

gm::sprite texture_atlas::decode_image(const std::string& image_file)
{
	try
	{
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

		return spr;
	}
	transpond_catch("texture_atlas::decode_image(std::string&)")
}

gm::sprite texture_atlas::decode_sprite(uint id)
{
	try
	{
		return gm::get_sprite_data(id);
	}
	transpond_catch("texture_atlas::decode_sprite(uint)");
}

gm::sprite texture_atlas::decode_background(uint id)
{
	try
	{
		return gm::get_background_data(id);
	}
	transpond_catch("texture_atlas::decode_background(uint)");
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

		RECT pos_rect = { .left = 0, .top = 0, .right = (long)size, .bottom = (long)size };
		D3DCheck(D3DXLoadSurfaceFromMemory(surface, nullptr, &pos_rect, data.data(),
			D3DFMT_A8R8G8B8, size * 4, nullptr, &pos_rect, D3DX_FILTER_NONE, 0), 2);

		D3DCheck(texture->AddDirtyRect(&pos_rect), 3);
		surface->Release();

		if (del_memdata)
			data.clear();

		return true;
	}
	transpond_catch("texture_atlas::burn(bool)")
}

template <typename T>
static void file_write(std::ofstream& ofs, T value)
{
	if constexpr (std::is_same_v<T, std::string>)
	{
		file_write(ofs, (uint)value.size());
		ofs.write(value.data(), std::streamsize(value.size()));
	}
	else
		ofs.write(reinterpret_cast<const char*>(&value), sizeof(T));
}

template <typename T>
static void file_read(std::ifstream& ifs, T& value)
{
	if constexpr (std::is_same_v<T, std::string>)
	{
		uint size = 0;
		file_read(ifs, size);
		value.resize(size);
		ifs.read(value.data(), std::streamsize(size));
	}
	else
		ifs.read(reinterpret_cast<char*>(&value), sizeof(T));
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

		// 保存纹理图集图像文件
		auto [d3dimage, w, h] = gm::get_image_data(texture);
		std::vector<uchar> image(d3dimage.size());

		for (size_t i = 0; i < d3dimage.size(); i += 4)
		{
			image[i] = d3dimage[i + 2];     // B -> R
			image[i + 1] = d3dimage[i + 1]; // G
			image[i + 2] = d3dimage[i];     // R -> B
			image[i + 3] = d3dimage[i + 3]; // A
		}

		lodepng::encode(file_path.string() + ".png", image.data(), w, h);

		// 保存纹理图集位置信息文件
		std::ofstream ofs(file_path.string() + ".bin", std::ios::binary | std::ios::trunc);
		if (!ofs)
			throw std::runtime_error("Fail to open the file " + file_path.string() + ".bin.");

		ofs.exceptions(std::ios::badbit | std::ios::failbit);

		file_write(ofs, size);
		file_write(ofs, (uint)images_list.size());

		for (auto& pimages : images_list)
		{
			texture_atlas::images& images = *pimages;

			file_write(ofs, images.name);

			file_write(ofs, images.image_width);
			file_write(ofs, images.image_height);

			file_write(ofs, (uint)images.frames.size());

			for (auto& psub_image : images.frames)
			{
				auto& sub_image = *psub_image;

				file_write(ofs, sub_image.texture_left);
				file_write(ofs, sub_image.texture_top);
				file_write(ofs, sub_image.texture_width);
				file_write(ofs, sub_image.texture_height);

				file_write(ofs, sub_image.orig_x);
				file_write(ofs, sub_image.orig_y);

				file_write(ofs, sub_image.is_rotated);
			}
		}
	}
	transpond_catch("texture_atlas::save(path&)")
}

std::vector<texture_atlas::images*> texture_atlas::load(path& file_path)
{
	try
	{
		std::vector<images*> result;

		// 加载纹理图集位置信息文件
		std::ifstream ifs(file_path.string() + ".bin", std::ios::binary);
		if (!ifs)
			throw std::runtime_error("Fail to open the file " + file_path.string() + ".bin.");

		ifs.exceptions(std::ios::badbit | std::ios::failbit);

		file_read(ifs, size);

		uint images_list_size;
		file_read(ifs, images_list_size);
		images_list.resize(images_list_size);

		for (uint i = 0; i < images_list_size; ++i)
		{
			uint image_width, image_height, frames_size;
			std::string name;

			file_read(ifs, name);

			file_read(ifs, image_width);
			file_read(ifs, image_height);

			file_read(ifs, frames_size);

			uint image_id = atlas_image_id++;
			images_list[i] = std::make_unique<images>(image_width, image_height,
				std::vector<images::sub_image_ptr>(frames_size), image_id, id);
			images_list[i]->name = name;

			game_images[image_id] = images_list[i].get();
			result.push_back(images_list[i].get());

			images_list[i]->frames.resize(frames_size);

			for (uint j = 0; j < frames_size; ++j)
			{
				uint texture_left, texture_top, texture_width, texture_height;
				int orig_x, orig_y;
				bool is_rotated;

				file_read(ifs, texture_left);
				file_read(ifs, texture_top);
				file_read(ifs, texture_width);
				file_read(ifs, texture_height);

				file_read(ifs, orig_x);
				file_read(ifs, orig_y);

				file_read(ifs, is_rotated);

				images_list[i]->frames[j] = std::make_unique<images::sub_image>(
					texture_left, texture_top, texture_width, texture_height, 
					orig_x, orig_y, is_rotated, atlas_texture_id, image_id);

				game_textures[atlas_texture_id] = images_list[i]->frames[j].get();
				atlas_texture_id++;
			}
		}

		// 加载纹理图集图像文件
		std::vector<uchar> image, d3dimage;
		uint width, height;
		uint error = lodepng::decode(image, width, height, file_path.string() + ".png");
		if (error)
			throw std::runtime_error(lodepng_error_text(error));

		// 将 RGBA 格式的数据转换为 D3D8 所需的 ARGB 格式
		d3dimage.resize(image.size());
		for (size_t i = 0; i < image.size(); i += 4)
		{
			d3dimage[i] = image[i + 2];     // R -> B
			d3dimage[i + 1] = image[i + 1]; // G
			d3dimage[i + 2] = image[i];     // B -> R
			d3dimage[i + 3] = image[i + 3]; // A
		}

		if (read_only())
			throw std::runtime_error("The atlas is read only.");

		IDirect3DDevice8* device = gmapi->GetDirect3DDevice();
		IDirect3DTexture8* texture = nullptr;

		if (texture == nullptr)
		{
			D3DCheck(device->CreateTexture(width, height, 1, 0, D3DFMT_A8R8G8B8,
				D3DPOOL_DEFAULT, &texture), 0);
			texture_atlas::texture = texture;
		}
		else
			texture = texture_atlas::texture;

		IDirect3DSurface8* surface = nullptr;
		D3DCheck(texture->GetSurfaceLevel(0, &surface), 1);

		RECT pos_rect = { .left = 0, .top = 0, .right = (long)width, .bottom = (long)height };
		D3DCheck(D3DXLoadSurfaceFromMemory(surface, nullptr, &pos_rect, d3dimage.data(),
			D3DFMT_A8R8G8B8, width * 4, nullptr, &pos_rect, D3DX_FILTER_NONE, 0), 2);

		D3DCheck(texture->AddDirtyRect(&pos_rect), 3);
		surface->Release();

		return result;
	}
	transpond_catch("texture_atlas::load(path&)")
}

// ============================================================================
// Export Functions
// ============================================================================

#pragma region Texture Atlas Create / Add image

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
		std::string f(file);
		gm::sprite sprite = atlas.decode_image(f);
		return (gm_real)atlas.add_image(sprite);
	}
	simple_catch("texture_atlas_add_file", -1)
}

exp_real texture_atlas_add_sprite(gm_real id, gm_real spr)
{
	try
	{
		texture_atlas& atlas = *game_texture_atlas.at((uint)id);
		gm::sprite sprite = atlas.decode_sprite((uint)spr);
		return (gm_real)atlas.add_image(sprite);
	}
	simple_catch("texture_atlas_add_sprite", -1)
}

exp_real texture_atlas_add_background(gm_real id, gm_real back)
{
	try
	{
		texture_atlas& atlas = *game_texture_atlas.at((uint)id);
		gm::sprite sprite = atlas.decode_background((uint)back);
		return (gm_real)atlas.add_image(sprite);
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

exp_real texture_atlas_load(gm_string file_path)
{
	try
	{
		uint id = texture_atlas_id_position++;
		path p(file_path);

		game_texture_atlas[id] = std::make_unique<texture_atlas>(1024, id);
		auto images = game_texture_atlas[id]->load(p);

		int result = gm::ds_map_create();
		gm::ds_map_add(result, "/Result/", (gm_real)id);
		for (auto& i : images)
			gm::ds_map_add(result, i->name, (gm_real)i->image_id);

		return (gm_real)result;
	}
	simple_catch("texture_atlas_load", -1)
}

exp_real texture_atlas_auto_add_file(gm_string file)
{
	try
	{
		std::string f(file);
		gm::sprite sprite = texture_atlas::decode_image(f);

		for (auto& [atlas_id, atlas] : game_texture_atlas)
		{
			if (atlas->read_only())
				continue;

			int image_id = atlas->add_image(sprite);
			if (image_id != -1)
				return (gm_real)image_id;
		}

		uint id = texture_atlas_id_position++;

		game_texture_atlas[id] = std::make_unique<texture_atlas>(1024, id);
		texture_atlas& atlas = *game_texture_atlas[id];

		return static_cast<gm_real>(atlas.add_image(sprite));
	}
	simple_catch("texture_atlas_auto_add_file", -1)
}

exp_real texture_atlas_auto_add_sprite(gm_real spr)
{
	try
	{
		gm::sprite sprite = texture_atlas::decode_sprite((uint)spr);

		for (auto& [atlas_id, atlas] : game_texture_atlas)
		{
			if (atlas->read_only())
				continue;

			int image_id = atlas->add_image(sprite);
			if (image_id != -1)
				return (gm_real)image_id;
		}

		uint id = texture_atlas_id_position++;

		game_texture_atlas[id] = std::make_unique<texture_atlas>(1024, id);
		texture_atlas& atlas = *game_texture_atlas[id];

		return static_cast<gm_real>(atlas.add_image(sprite));
	}
	simple_catch("texture_atlas_auto_add_sprite", -1)
}

exp_real texture_atlas_auto_add_background(gm_real back)
{
	try
	{
		gm::sprite sprite = texture_atlas::decode_background((uint)back);

		for (auto& [atlas_id, atlas] : game_texture_atlas)
		{
			if (atlas->read_only())
				continue;

			int image_id = atlas->add_image(sprite);
			if (image_id != -1)
				return (gm_real)image_id;
		}

		uint id = texture_atlas_id_position++;

		game_texture_atlas[id] = std::make_unique<texture_atlas>(1024, id);
		texture_atlas& atlas = *game_texture_atlas[id];

		return static_cast<gm_real>(atlas.add_image(sprite));
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

#pragma endregion

#pragma region Get

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

exp_real texture_atlas_get_image_count(gm_real atlas_id)
{
	try
	{
		texture_atlas* atlas = game_texture_atlas.at((uint)atlas_id).get();
		return (gm_real)atlas->images_list.size();
	}
	simple_catch("texture_atlas_get_image_count", -1)
}

exp_real image_get_texture_atlas(gm_real image_id)
{
	try
	{
		texture_atlas::images* image = game_images.at((uint)image_id);
		return (gm_real)image->atlas_id;
	}
	simple_catch("image_get_texture_atlas", -1)
}

exp_real texture_atlas_get_image(gm_real atlas_id, gm_real pos)
{
	try
	{
		texture_atlas* atlas = game_texture_atlas.at((uint)atlas_id).get();
		return (gm_real)atlas->images_list.at((uint)pos)->image_id;
	}
	simple_catch("image_get_texture_atlas", -1)
}

exp_real texture_get_image(gm_real texture_id)
{
	try
	{
		texture_atlas::images::sub_image* sub_image = game_textures.at((uint)texture_id);
		return (gm_real)sub_image->image_id;
	}
	simple_catch("texture_get_sub_image", -1)
}

exp_real atlas_sprite_get_texture(gm_real spr, gm_real subimg)
{
	try
	{
		if ((uint)spr < IMAGE_START_POSITION)
			return gm::sprite_get_texture((int)spr, (int)subimg);

		texture_atlas::images* image = game_images.at((uint)spr);
		texture_atlas::images::sub_image* sub_image = image->frames.at((uint)subimg).get();
		return sub_image->texture_id;
	}
	simple_catch("sprite_get_texture", -1)
}

exp_real atlas_background_get_texture(gm_real back)
{
	try
	{
		if ((uint)back < IMAGE_START_POSITION)
			return gm::background_get_texture((int)back);

		texture_atlas::images* image = game_images.at((uint)back);
		texture_atlas::images::sub_image* sub_image = image->frames.at(0).get();
		return sub_image->texture_id;
	}
	simple_catch("sprite_get_texture", -1)
}

exp_real atlas_sprite_get_width(gm_real id)
{
	try
	{
		if ((uint)id < IMAGE_START_POSITION)
			return gm::sprite_get_width((int)id);

		texture_atlas::images* image = game_images.at((uint)id);
		return (gm_real)image->image_width;
	}
	simple_catch("atlas_sprite_get_width", 0)
}

exp_real atlas_sprite_get_height(gm_real id)
{
	try
	{
		if ((uint)id < IMAGE_START_POSITION)
			return gm::sprite_get_height((int)id);

		texture_atlas::images* image = game_images.at((uint)id);
		return (gm_real)image->image_height;
	}
	simple_catch("atlas_sprite_get_height", 0)
}

exp_real atlas_background_get_width(gm_real id)
{
	try
	{
		if ((uint)id < IMAGE_START_POSITION)
			return gm::background_get_width((int)id);

		texture_atlas::images* image = game_images.at((uint)id);
		return (gm_real)image->image_width;
	}
	simple_catch("atlas_sprite_get_width", 0)
}

exp_real atlas_background_get_height(gm_real id)
{
	try
	{
		if ((uint)id < IMAGE_START_POSITION)
			return gm::background_get_height((int)id);

		texture_atlas::images* image = game_images.at((uint)id);
		return (gm_real)image->image_height;
	}
	simple_catch("atlas_sprite_get_height", 0)
}

exp_real atlas_texture_get_width(gm_real id)
{
	try
	{
		if ((uint)id < TEXTURE_START_POSITION)
			return gm::texture_get_width((int)id);

		texture_atlas::images::sub_image* subimg = game_textures.at((uint)id);
		texture_atlas::images* image = game_images.at(subimg->image_id);
		texture_atlas* atlas = game_texture_atlas.at(image->atlas_id).get();

		return (gm_real)subimg->texture_width / (gm_real)atlas->size;
	}
	simple_catch("atlas_texture_get_width", 0)
}

exp_real atlas_texture_get_height(gm_real id)
{
	try
	{
		if ((uint)id < TEXTURE_START_POSITION)
			return gm::texture_get_height((int)id);

		texture_atlas::images::sub_image* subimg = game_textures.at((uint)id);
		texture_atlas::images* image = game_images.at(subimg->image_id);
		texture_atlas* atlas = game_texture_atlas.at(image->atlas_id).get();

		return (gm_real)subimg->texture_height / (gm_real)atlas->size;
	}
	simple_catch("atlas_texture_get_width", 0)
}

exp_real texture_get_atlas_x(gm_real id)
{
	try
	{
		if ((uint)id < TEXTURE_START_POSITION)
			return 0;

		texture_atlas::images::sub_image* subimg = game_textures.at((uint)id);
		texture_atlas::images* image = game_images.at(subimg->image_id);
		texture_atlas* atlas = game_texture_atlas.at(image->atlas_id).get();

		return (gm_real)subimg->texture_left / (gm_real)atlas->size;
	}
	simple_catch("texture_get_atlas_x", -1)
}

exp_real texture_get_atlas_y(gm_real id)
{
	try
	{
		if ((uint)id < TEXTURE_START_POSITION)
			return 0;

		texture_atlas::images::sub_image* subimg = game_textures.at((uint)id);
		texture_atlas::images* image = game_images.at(subimg->image_id);
		texture_atlas* atlas = game_texture_atlas.at(image->atlas_id).get();

		return (gm_real)subimg->texture_top / (gm_real)atlas->size;
	}
	simple_catch("texture_get_atlas_y", -1)
}

#pragma endregion

#pragma region Configs

exp_real texture_atlas_set_crop(gm_real crop)
{
	gm::crop_blank = (crop >= 0.5);
	return gtrue;
}

exp_real texture_atlas_get_crop() { return gm::crop_blank; }

exp_real texture_atlas_set_amplificate(gm_real ampl)
{
	texture_atlas::texture_amplification = (ampl >= 0.5);
	return gtrue;
}

exp_real texture_atlas_get_amplificate()
{
	return texture_atlas::texture_amplification;
}

#pragma endregion