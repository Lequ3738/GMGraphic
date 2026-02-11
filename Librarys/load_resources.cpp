#include "load_resources.h"
#include "buffer.h"

gm::sprite gm::decode_gmspr(std::string& file)
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