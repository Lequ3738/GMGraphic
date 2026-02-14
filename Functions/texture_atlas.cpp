#include "texture_atlas.h"
#include "load_resources.h"
#include "MaxRectsBinPack.h"

std::unordered_map<uint, texture_atlas> game_texture_atlas;
uint texture_atlas_id_position = 0, atlas_image_id = 0;

texture_atlas::texture_atlas(uint size, atlas_kind kind)
{
	switch (size)
	{
		case 256:
		case 512:
		case 1024:
		case 2048:
		{
			texture_atlas::size = size;
			texture_atlas::kind = kind;
			data = std::vector<uchar>(size * size * 4, 0);
			bin = rbp::MaxRectsBinPack(size, size, true);
		}
		break;

		default:
			throw std::runtime_error("Invalid texture atlas size. "
				"Supported sizes are 256, 512, 1024, and 2048.");
	}
}

texture_atlas::~texture_atlas()
{
	if (texture == nullptr)
		return;

	texture->Release();
	texture = nullptr;
}

static void add_image_to_memory(texture_atlas& atlas, std::vector<uchar>& image_data, 
	rbp::Rect& rect, atlas_images::image& image)
{
	int img_w = (int)image.width;
	int img_h = (int)image.height;

	if (!image.is_rotated)
	{
		int dst_x = rect.x + 1;
		int dst_y = rect.y + 1;
		int copy_w = std::min(rect.width - 2, img_w);
		int copy_h = std::min(rect.height - 2, img_h);

		for (int y = 0; y < copy_h; ++y)
		{
			uchar* dst_ptr = atlas.data.data() + ((dst_y + y) * atlas.size + dst_x) * 4;
			const uchar* src_ptr = image_data.data() + (y * img_w) * 4;
			std::memcpy(dst_ptr, src_ptr, copy_w * 4);
		}
	}
	else
	{
		int dst_x = rect.x + 1;
		int dst_y = rect.y + 1;
		int copy_w = std::min(rect.width - 2, img_h);
		int copy_h = std::min(rect.height - 2, img_w);
		
		for (int v = 0; v < copy_h; ++v)
		{
			for (int u = 0; u < copy_w; ++u)
			{
				uchar* dst_pixel = atlas.data.data() + ((dst_y + v) * atlas.size + dst_x + u) * 4;

				int src_x = v;
				int src_y = img_h - 1 - u;

				if (src_x < img_w && src_y < img_h)
				{
					const uchar* src_pixel = image_data.data() + (src_y * img_w + src_x) * 4;
					*(uint32_t*)dst_pixel = *(uint32_t*)src_pixel;
				}
			}
		}
	}
}

bool texture_atlas::add_sprite(std::string& gmspr_file)
{
	// 获取已创建的纹理图集
	if (kind == texture_atlas::atlas_kind::background)
		throw std::runtime_error("Cannot add sprite to a background atlas.");
	if (read_only())
		throw std::runtime_error("Cannot add sprite to a burned atlas.");

	// 解析 gmspr 文件
	gm::sprite spr = gm::decode_gmspr(gmspr_file);

	// 面积预检查
	uint spr_width = spr.width + 2;  // 每个子图增加 1 像素边距，以避免采样时的边界问题
	uint spr_height = spr.height + 2;

	uint needed_area = spr_width * spr_height * spr.frame_count();
	if (bin.Occupancy() + (double)needed_area / (size * size) > 1.0)
		return false;

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
			return false;
		}

		result[i] = node;
	}

	// 写入图集数据
	atlas_images img = {
		.orig_x = spr.xorig,
		.orig_y = spr.yorig,
		.frames = std::vector<atlas_images::image>(spr.frame_count())
	};

	for (uint i = 0; i < spr.frame_count(); ++i)
	{
		img.frames[i] = atlas_images::image
		{
			.left = (uint)result[i].x + 1, .top = (uint)result[i].y + 1,
			.width = spr.width, .height = spr.height,
			.is_rotated = (result[i].width - 2 != spr.width),
			.atlas_id = atlas_image_id++
		};

		add_image_to_memory(*this, spr.data[i], result[i], img.frames[i]);
	}

	return true;
}

bool texture_atlas::burn(bool del_memdata = true)
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

void texture_atlas::save(path& file_path)
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

// ============================================================================
// Export Functions
// ============================================================================

exp_real texture_atlas_create(gm_real size, gm_real kind)
{
	try
	{
		texture_atlas atlas((uint)size, (texture_atlas::atlas_kind)kind);
		game_texture_atlas[texture_atlas_id_position] = std::move(atlas);
		return (gm_real)texture_atlas_id_position++;
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
		return (gm_real)atlas.add_sprite(file);
	}
	simple_catch("texture_atlas_add_sprite", gfalse)
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