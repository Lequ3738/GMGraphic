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

// 应用边缘颜色膨胀，避免在平滑插值下图像出现黑边问题
static void apply_color_bleeding(std::vector<uchar>& image_data, uint width, uint height,
	const gm::image_rect& cropped_rect)
{
	if (cropped_rect.right < cropped_rect.left || cropped_rect.bottom < cropped_rect.top)
		return;

	std::vector<uchar> buffer = image_data;  // 复制一份原始数据作为只读源，防止级联污染

	int process_left = cropped_rect.left - 1;
	int process_top = cropped_rect.top - 1;
	int process_right = cropped_rect.right + 1;
	int process_bottom = cropped_rect.bottom + 1;

	process_left = std::max(process_left, 0);
	process_top = std::max(process_top, 0);
	process_right = std::min(process_right, (int)width - 1);
	process_bottom = std::min(process_bottom, (int)height - 1);

	for (int y = process_top; y <= process_bottom; ++y)
	{
		for (int x = process_left; x <= process_right; ++x)
		{
			uint idx = (y * (int)width + x) * 4;
			uchar a = buffer[idx + 3];

			if (a == 0)
			{
				int b_sum = 0, g_sum = 0, r_sum = 0;
				int count = 0;

				// 采样周围 3x3 范围的 8 个邻居
				for (int dy = -1; dy <= 1; ++dy)
				{
					for (int dx = -1; dx <= 1; ++dx)
					{
						if (dx == 0 && dy == 0) continue;

						int nx = x + dx;
						int ny = y + dy;

						// 边界检查
						if (nx >= 0 && nx < (int)width && ny >= 0 && ny < (int)height)
						{
							uint n_idx = (ny * (int)width + nx) * 4;
							uchar n_a = buffer[n_idx + 3];

							if (n_a > 0)
							{
								b_sum += buffer[n_idx];     // B
								g_sum += buffer[n_idx + 1]; // G
								r_sum += buffer[n_idx + 2]; // R
								count++;
							}
						}
					}
				}

				if (count > 0)
				{
					image_data[idx] = (uchar)(b_sum / count);
					image_data[idx + 1] = (uchar)(g_sum / count);
					image_data[idx + 2] = (uchar)(r_sum / count);
					// Alpha 保持为 0，不修改
				}
			}
		}
	}
}

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

				if (cropped_rects[i].right < cropped_rects[i].left ||
					cropped_rects[i].bottom < cropped_rects[i].top)
				{
					cropped_rects[i] = { 0, 0, 0, 0 };
				}

				images[i] = std::move(std::get<0>(image));
			}
			else
				cropped_rects[i] = { 0, 0, (int)width - 1, (int)height - 1 };

			apply_color_bleeding(images[i], width, height, cropped_rects[i]);  // 进行边缘颜色膨胀
		}

		// 构建结果
		std::string filename(gm::filename_name(file));
		std::string name = filename.substr(0, filename.rfind('.'));

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

			.cropped_rects = std::move(cropped_rects),

			.name = std::move(name)
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

			if (cropped_rects[0].right < cropped_rects[0].left ||
				cropped_rects[0].bottom < cropped_rects[0].top)
			{
				cropped_rects[0] = { 0, 0, 0, 0 };
			}
		}
		else
			cropped_rects[0] = { 0, 0, (int)width - 1, (int)height - 1 };

		apply_color_bleeding(d3dimage, width, height, cropped_rects[0]);  // 进行边缘颜色膨胀

		// 构建结果
		std::vector<std::vector<uchar>> images;
		images.push_back(std::move(d3dimage));

		std::string filename(gm::filename_name(file));
		std::string name = filename.substr(0, filename.rfind('.'));

		return gm::sprite {
			.width = width, .height = height,
			.data = std::move(images),

			.cropped_rects = std::move(cropped_rects),

			.name = std::move(name)
		};
	}
	transpond_catch("gm::decode_png(std::string&)")
}

gm::image_data gm::get_image_data(void* texture)
{
	try
	{
		// 读回纹理像素: 取 level0 表面 → 拷到系统内存表面 → LockRect 复制。
		// 整链经适配器, 双后端通用(内部处理 D3D8/9 的表面类型与 D3DX)。
		std::vector<uchar> dest;
		uint width = 0, height = 0;
		D3DCheck(d3d::read_texture(texture, dest, width, height), 0);

		return std::make_tuple(std::move(dest), width, height);
	}
	transpond_catch("get_image_data(void*)")
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
			void* texture = (void*)spr.Subimages[id].GetTexture();   // 不透明: 只用于读回
			gm::image_data data = get_image_data(texture);

			// 进行空白裁剪计算
			if (crop_blank)
			{
				cropped_rects[i] = crop_blank_area(data);

				if (cropped_rects[i].right < cropped_rects[i].left ||
					cropped_rects[i].bottom < cropped_rects[i].top)
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
		void* texture = (void*)gmapi->Backgrounds[id].GetTexture();   // 不透明: 只用于读回
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

			if (cropped_rects[0].right < cropped_rects[0].left ||
				cropped_rects[0].bottom < cropped_rects[0].top)
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