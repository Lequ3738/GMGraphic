#include "load_resources.h"
#include "MaxRectsBinPack.h"
#include "texture_atlas.h"

std::unordered_map<uint, texture_atlas> game_texture_atlas;
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
	try
	{
		if (texture == nullptr)
			return;

		texture->Release();
		texture = nullptr;
	}
	transpond_catch("texture_atlas::~texture_atlas()")
}

static void add_image_to_memory(texture_atlas& atlas, std::vector<uchar>& image_data, 
	rbp::Rect& rect, texture_atlas::images::sub_image& image)
{
	try
	{
		const int img_w = (int)image.width;
		const int img_h = (int)image.height;

		if (!image.is_rotated)
		{
			const int dst_x = rect.x + 1;
			const int dst_y = rect.y + 1;
			const int copy_w = std::min(rect.width - 2, img_w);
			const int copy_h = std::min(rect.height - 2, img_h);

			for (int y = 0; y < copy_h; ++y)
			{
				uchar* const dst_ptr = atlas.data.data() + ((dst_y + y) * 
					atlas.size + dst_x) * 4;
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
					uchar* const dst_pixel = atlas.data.data() + ((dst_y + v) * 
						atlas.size + dst_x + u) * 4;

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

		// 面积预检查
		uint spr_width = spr.width + 2;  // 每个子图增加 1 像素边距，以避免采样时的边界问题
		uint spr_height = spr.height + 2;

		uint needed_area = spr_width * spr_height * spr.frame_count();
		if (bin.Occupancy() + (double)needed_area / (size * size) > 1.0)
			return -1;

		// 进行打包计算
		rbp::MaxRectsBinPack backup = bin;  // 备份当前打包器状态
		std::vector<rbp::Rect> result(spr.frame_count());

		for (uint i = 0; i < spr.frame_count(); ++i)
		{
			rbp::Rect node = bin.Insert((int)spr_width, (int)spr_height,
				rbp::MaxRectsBinPack::RectBestShortSideFit);

			if (node.height == 0)  // 若任一子图打包失败，则整体打包失败
			{
				bin = std::move(backup);  // 恢复打包器状态
				return -1;
			}

			result[i] = node;
		}

		// 写入图集数据
		auto img = std::make_unique<images>(spr.xorig, spr.yorig,
			std::vector<images::sub_image>(spr.frame_count()),
			atlas_image_id++, id);

		for (uint i = 0; i < spr.frame_count(); ++i)
		{
			img->frames[i] = std::make_unique<images::sub_image>((uint)result[i].x + 1,
				(uint)result[i].y + 1, spr.width, spr.height,
				(result[i].width - 2 != spr.width), atlas_texture_id);
			game_textures[atlas_texture_id] = img->frames[i].get();
			atlas_texture_id++;

			add_image_to_memory(*this, spr.data[i], result[i], *img->frames[i]);
		}

		images_list.push_back(std::move(img));
		game_images[id] = images_list.back().get();

		return true;
	}
	transpond_catch("texture_atlas::add_image(std::string&)")
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

void texture_atlas::save(path& file_path)
{
	try
	{
		if (texture == nullptr)
		{
			throw std::runtime_error("This texture set has not been uploaded to the GPU end.\n"
				"Path: " + file_path.string());
		}

		IDirect3DSurface8* surface = nullptr;
		D3DCheck(texture->GetSurfaceLevel(0, &surface), 0);

		RECT rect = { .left = 0, .top = 0, .right = (long)size, .bottom = (long)size };
		D3DCheck(D3DXSaveSurfaceToFile(file_path.wstring().c_str(), D3DXIFF_PNG, surface,
			nullptr, &rect), 1);
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

		texture_atlas atlas((uint)size, id);
		game_texture_atlas[id] = std::move(atlas);
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

exp_real texture_atlas_add_sprite(gm_real id, gm_string gmspr_file)
{
	try
	{
		texture_atlas& atlas = game_texture_atlas.at((uint)id);
		std::string file(gmspr_file);
		return (gm_real)atlas.add_image(file);
	}
	simple_catch("texture_atlas_add_sprite", -1)
}

exp_real texture_atlas_burn(gm_real id, gm_real del_memdata)
{
	try
	{
		texture_atlas& atlas = game_texture_atlas.at((uint)id);
		return (gm_real)atlas.burn((bool)del_memdata);
	}
	simple_catch("texture_atlas_burn", gfalse)
}

exp_real texture_atlas_save(gm_real id, gm_string file_path)
{
	try
	{
		texture_atlas& atlas = game_texture_atlas.at((uint)id);
		path p(file_path);
		atlas.save(p);
		return gtrue;
	}
	simple_catch("texture_atlas_save", gfalse)
}

exp_real texture_atlas_auto_add(gm_string file)
{
	try
	{
		for (auto& [atlas_id, atlas] : game_texture_atlas)
		{
			if (atlas.read_only())
				continue;

			std::string f(file);
			if (atlas.add_image(f))
				return atlas_id;
		}

		uint id = texture_atlas_id_position++;

		game_texture_atlas[id] = texture_atlas(1024, id);
		texture_atlas& atlas = game_texture_atlas[id];

		std::string f(file);
		return atlas.add_image(f) ? id : -1;
	}
	simple_catch("texture_atlas_auto_add", -1)
}

exp_real texture_atlas_auto_finish(gm_real dont_twice)
{
	try
	{
		for (auto& [atlas_id, atlas] : game_texture_atlas)
		{
			if ((atlas.texture == nullptr && dont_twice) || !dont_twice)
				atlas.burn();
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