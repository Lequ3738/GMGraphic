#include "buffer.h"
#include "load_resources.h"
#include "lodepng.h"

gm::sprite gm::decode_gmspr(std::string& file)
{
	try
	{
		if (BufferDLL == nullptr)
			throw std::runtime_error("Buffer library is not loaded.");

		gm_real buffer = gm::buffer_create();
		bool result = (bool)gm::buffer_read_from_file(buffer, file.c_str());
		if (!result)
		{
			gm::buffer_destroy(buffer);
			throw std::runtime_error("The specified file cannot be opened.\n"
				"File: " + std::string(file));
		}

		if (gm::buffer_read_int32(buffer) != 1234321)
		{
			gm::buffer_destroy(buffer);
			throw std::runtime_error("Invalid GMSPR file format.\n"
				"File: " + std::string(file));
		}

		uint size = (uint)gm::buffer_read_int32(buffer);

		gm_real data = gm::buffer_create();
		gm::buffer_write_buffer_part(data, buffer, gm::buffer_get_pos(buffer), size);
		gm::buffer_destroy(buffer);

		gm::buffer_zlib_uncompress(data);

		gm::buffer_jump(data, 4);  // 跳过版本号 800
		int xorig = (int)gm::buffer_read_int32(data);
		int yorig = (int)gm::buffer_read_int32(data);
		int count = (int)gm::buffer_read_int32(data);

		if (count <= 0)
		{
			gm::buffer_destroy(buffer);
			throw std::runtime_error("This GMSPR file does not contain any image frames.\n"
				"File: " + std::string(file));
		}

		uint width = 0, height = 0;
		std::vector<std::vector<uchar>> images;
		images.resize(count);

		for (int i = 0; i < count; ++i)
		{
			gm::buffer_jump(data, 4);  // 跳过版本号 800
			width = (uint)gm::buffer_read_int32(data);
			height = (uint)gm::buffer_read_int32(data);
			int frame_size = (int)gm::buffer_read_int32(data);

			uchar* image_data = (uchar*)(int)(gm::buffer_get_address(data, false) +
				gm::buffer_get_pos(data));
			if (image_data == nullptr)
			{
				gm::buffer_destroy(data);
				throw std::runtime_error("Invalid image data block.\nFile: " + std::string(file)
					+ "\nIndex: " + std::to_string(i));
			}

			images[i] = std::vector<uchar>(image_data, image_data + frame_size);
			gm::buffer_jump(data, frame_size);
		}

		using mask_kind = gm::sprite::mask_kind;
		mask_kind mask = (mask_kind)gm::buffer_read_int32(data);
		uchar mask_tolerance = (uchar)gm::buffer_read_int32(data);
		bool separate_mask = (bool)gm::buffer_read_int32(data);

		using bounding_kind = gm::sprite::bounding_kind;
		bounding_kind bounding = (bounding_kind)gm::buffer_read_int32(data);
		int bounding_left = (int)gm::buffer_read_int32(data);
		int bounding_right = (int)gm::buffer_read_int32(data);
		int bounding_bottom = (int)gm::buffer_read_int32(data);
		int bounding_top = (int)gm::buffer_read_int32(data);

		gm::buffer_destroy(data);

		return gm::sprite {
			.width = width, .height = height,
			.xorig = xorig, .yorig = yorig,

			.data = std::move(images),

			.mask = mask,
			.mask_tolerance = mask_tolerance,
			.separate_mask = separate_mask,

			.bounding = bounding,
			.bounding_left = bounding_left,
			.bounding_right = bounding_right,
			.bounding_top = bounding_top,
			.bounding_bottom = bounding_bottom
		};
	}
	transpond_catch("gm::decode_gmspr(std::string&)")
}

gm::sprite gm::decode_png(std::string& file)
{
	try
	{
		std::vector<uchar> image, d3dimage;
		uint width, height;
		uint error = lodepng::decode(image, width, height, file);
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

		std::vector<std::vector<uchar>> images;
		images.push_back(std::move(d3dimage));

		return gm::sprite {
			.width = width, .height = height,
			.data = std::move(images)
		};
	}
	transpond_catch("gm::decode_png(std::string&)")
}

gm::image_data gm::get_image_data(IDirect3DTexture8* texture)
{
	try
	{
		IDirect3DDevice8* device = gmapi->GetDirect3DDevice();
		IDirect3DSurface8* surface = nullptr, *surface_mem = nullptr;

		D3DSURFACE_DESC desc{};
		D3DCheck(texture->GetLevelDesc(0, &desc), 0);
		D3DCheck(texture->GetSurfaceLevel(0, &surface), 1);

		uint width = desc.Width, height = desc.Height;
		D3DCheck(device->CreateImageSurface(width, height, D3DFMT_A8R8G8B8, &surface_mem), 2);
		D3DCheck(D3DXLoadSurfaceFromSurface(surface_mem, nullptr, nullptr, surface, nullptr,
			nullptr, D3DX_FILTER_NONE, 0), 3);

		D3DLOCKED_RECT lock{};
		surface_mem->LockRect(&lock, nullptr, 0);

		uchar* data = (uchar*)lock.pBits;
		std::vector<uchar> dest(width * height * 4);

		uint src_pos = 0, dst_pos = 0, buffer_stride = width * 4;
		for (uint i = 0; i < height; ++i)
		{
			std::memcpy(dest.data() + dst_pos, &data[src_pos], buffer_stride);
			src_pos += lock.Pitch;
			dst_pos += buffer_stride;
		}

		D3DCheck(surface_mem->UnlockRect(), 4);
		surface_mem->Release();
		surface->Release();

		return std::make_tuple(std::move(dest), width, height);
	}
	transpond_catch("get_image_data(IDirect3DTexture8*)")
}

gm::sprite gm::get_sprite_data(uint id)
{
	try
	{
		ISprite spr = gmapi->Sprites[id];
		std::vector<std::vector<uchar>> images(spr.Subimages.GetCount());

		for (uint i = 0; i < (uint)spr.Subimages.GetCount(); ++i)
		{
			IDirect3DTexture8* texture = spr.Subimages[id].GetTexture();
			auto [data, w, h] = get_image_data(texture);

			images[i] = std::move(data);
		}

		return gm::sprite{
			.width = (uint)spr.GetWidth(), .height = (uint)spr.GetHeight(),
			.xorig = spr.GetOffsetX(), .yorig = spr.GetOffsetY(),

			.data = std::move(images)
		};
	}
	transpond_catch("gm::get_sprite_data(uint)")
}

gm::sprite gm::get_background_data(uint id)
{
	try
	{
		IDirect3DTexture8* texture = gmapi->Backgrounds[id].GetTexture();
		auto [data, width, height] = get_image_data(texture);
		
		width = std::min((uint)gmapi->Backgrounds[id].GetWidth(), width);
		height = std::min((uint)gmapi->Backgrounds[id].GetHeight(), height);

		std::vector<std::vector<uchar>> images;
		images.push_back(std::move(data));
		
		return gm::sprite {
			.width = width, .height = height,
			.data = std::move(images)
		};
	}
	transpond_catch("gm::get_background_data(uint)")
}