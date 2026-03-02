#include "buffer.h"
#include "load_resources.h"
#include "lodepng.h"

bool gm::crop_blank = true;

#pragma warning(disable: 4102)  // unreferenced label
static gm::image_rect crop_blank_area(gm::image_data& image)
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
					bottom = y;
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
					right = x;
					goto calculate_result;
				}
			}
		}

	calculate_result:
		return { left, top, right, bottom };
	}
	transpond_catch("crop_blank_area(gm::image_data&)")
}
#pragma warning(default: 4102)  // unreferenced label

gm::sprite gm::decode_gmspr(const std::string& file)
{
	try
	{
		if (BufferDLL == nullptr)
			throw std::runtime_error("Buffer library is not loaded.");

		// 解析 gmspr 文件
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

		// 进行空白裁剪计算
		std::vector<gm::image_rect> cropped_rects(count);

		for (uint i = 0; i < (uint)count; ++i)
		{
			if (crop_blank)
			{
				gm::image_data image{ std::move(images[i]), width, height };
				cropped_rects[i] = crop_blank_area(image);

				if (cropped_rects[i].right <= cropped_rects[i].left ||
					cropped_rects[i].bottom <= cropped_rects[i].top)
				{
					cropped_rects[i] = { 0, 0, 0, 0 };
				}

				images[i] = std::move(std::get<0>(image));
			}
			else
				cropped_rects[i] = { 0, 0, (int)width - 1, (int)height - 1 };
		}

		// 构建结果
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
			.bounding_bottom = bounding_bottom,

			.cropped_rects = std::move(cropped_rects)
		};
	}
	transpond_catch("gm::decode_gmspr(std::string&)")
}

gm::sprite gm::decode_png(const std::string& file)
{
	try
	{
		// PNG 文件解码
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

		// 进行空白裁剪计算
		std::vector<gm::image_rect> cropped_rects(1);
		if (crop_blank)
		{
			// 直接将不需要的 image vector 移动到 image_data 里面
			// crop_blank_area() 只分析 alpha 通道，所以 RGB 通道并不重要
			gm::image_data data{ std::move(image), width, height };
			cropped_rects[0] = crop_blank_area(data);

			if (cropped_rects[0].right <= cropped_rects[0].left ||
				cropped_rects[0].bottom <= cropped_rects[0].top)
			{
				cropped_rects[0] = { 0, 0, 0, 0 };
			}
		}
		else
			cropped_rects[0] = { 0, 0, (int)width - 1, (int)height - 1 };

		// 构建结果
		std::vector<std::vector<uchar>> images;
		images.push_back(std::move(d3dimage));

		return gm::sprite {
			.width = width, .height = height,
			.data = std::move(images),

			.cropped_rects = std::move(cropped_rects)
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
		std::vector<gm::image_rect> cropped_rects(spr.Subimages.GetCount());

		for (uint i = 0; i < (uint)spr.Subimages.GetCount(); ++i)
		{
			IDirect3DTexture8* texture = spr.Subimages[id].GetTexture();
			gm::image_data data = get_image_data(texture);

			// 进行空白裁剪计算
			if (crop_blank)
			{
				cropped_rects[i] = crop_blank_area(data);

				if (cropped_rects[i].right <= cropped_rects[i].left ||
					cropped_rects[i].bottom <= cropped_rects[i].top)
				{
					cropped_rects[i] = { 0, 0, 0, 0 };
				}
			}
			else
			{
				cropped_rects[i] = { 0, 0,
					(int)std::get<1>(data) - 1,  // width
					(int)std::get<2>(data) - 1   // height
				};
			}

			images[i] = std::move(std::get<0>(data));
		}

		return gm::sprite {
			.width = (uint)spr.GetWidth(), .height = (uint)spr.GetHeight(),
			.xorig = spr.GetOffsetX(), .yorig = spr.GetOffsetY(),

			.data = std::move(images),
			.cropped_rects = std::move(cropped_rects)
		};
	}
	transpond_catch("gm::get_sprite_data(uint)")
}

gm::sprite gm::get_background_data(uint id)
{
	try
	{
		IDirect3DTexture8* texture = gmapi->Backgrounds[id].GetTexture();
		gm::image_data data = get_image_data(texture);
		
		uint width = std::min((uint)gmapi->Backgrounds[id].GetWidth(), 
			std::get<1>(data));
		uint height = std::min((uint)gmapi->Backgrounds[id].GetHeight(), 
			std::get<2>(data));

		// 进行空白裁剪计算
		std::vector<gm::image_rect> cropped_rects(1);
		if (crop_blank)
		{
			cropped_rects[0] = crop_blank_area(data);

			if (cropped_rects[0].right <= cropped_rects[0].left ||
				cropped_rects[0].bottom <= cropped_rects[0].top)
			{
				cropped_rects[0] = { 0, 0, 0, 0 };
			}
		}
		else
			cropped_rects[0] = { 0, 0, (int)width - 1, (int)height - 1 };

		std::vector<std::vector<uchar>> images;
		images.push_back(std::move(std::get<0>(data)));
		
		return gm::sprite {
			.width = width, .height = height,
			.data = std::move(images),

			.cropped_rects = std::move(cropped_rects)
		};
	}
	transpond_catch("gm::get_background_data(uint)")
}