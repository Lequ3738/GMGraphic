#include "buffer.h"

HMODULE BufferDLL = nullptr;

namespace gm
{
	r_v buffer_create;
	r_r buffer_destroy, buffer_exists, buffer_get_pos, buffer_get_size;
	r_r buffer_at_end, buffer_get_error, buffer_clear_error, buffer_clear;
	r_r buffer_zlib_compress, buffer_zlib_uncompress, buffer_read_int8;
	r_r buffer_read_uint8, buffer_read_int16, buffer_read_uint16;
	r_r buffer_read_int32, buffer_read_uint32, buffer_read_int64, buffer_read_uint64;
	r_r buffer_read_intv, buffer_read_uintv, buffer_read_float32, buffer_read_float64;
	s_r buffer_to_string, buffer_read_string;
	r_rr buffer_set_pos, buffer_rc4_crypt_buffer, buffer_write_int8, buffer_write_uint8;
	r_rr buffer_write_int16, buffer_write_uint16, buffer_write_int32;
	r_rr buffer_write_uint32, buffer_write_int64, buffer_write_uint64;
	r_rr buffer_write_intv, buffer_write_uintv, buffer_write_float32, buffer_write_float64;
	r_rr buffer_write_buffer, buffer_get_address, buffer_set_size;
	r_rs buffer_read_from_file, buffer_write_to_file, buffer_append_to_file;
	r_rs buffer_rc4_crypt, buffer_write_string, buffer_write_data, buffer_write_hex;
	r_rsrr buffer_read_from_file_part;
	s_rr buffer_read_data, buffer_read_hex;
	r_rrrr buffer_write_buffer_part;
}

template<typename T> static void load(T& var, gm_string name)
{
	var = (T)GetProcAddress(BufferDLL, name);
	if (var == nullptr) {
		throw std::runtime_error("Failed to obtain function (" +
			std::string(name) + ") when loading Http.dll.");
	}
}

exp_real ImportBufferModule(gm_string name)
{
	try
	{
		std::wstring wname(name, name + strlen(name));
		BufferDLL = GetModuleHandle(wname.c_str());

		if (BufferDLL == nullptr)
			throw std::runtime_error("Failed to load " + std::string(name) + ".");

		load(gm::buffer_create, "buffer_create");
		load(gm::buffer_destroy, "buffer_destroy");
		load(gm::buffer_exists, "buffer_exists");
		load(gm::buffer_to_string, "buffer_to_string");
		load(gm::buffer_get_pos, "buffer_get_pos");
		load(gm::buffer_get_size, "buffer_get_length");
		load(gm::buffer_at_end, "buffer_at_end");
		load(gm::buffer_get_error, "buffer_get_error");
		load(gm::buffer_clear_error, "buffer_clear_error");
		load(gm::buffer_clear, "buffer_clear");
		load(gm::buffer_set_pos, "buffer_set_pos");
		load(gm::buffer_read_from_file, "buffer_read_from_file");
		load(gm::buffer_read_from_file_part, "buffer_read_from_file_part");
		load(gm::buffer_write_to_file, "buffer_write_to_file");
		load(gm::buffer_append_to_file, "buffer_append_to_file");
		load(gm::buffer_rc4_crypt, "buffer_rc4_crypt");
		load(gm::buffer_rc4_crypt_buffer, "buffer_rc4_crypt_buffer");
		load(gm::buffer_zlib_compress, "buffer_zlib_compress");
		load(gm::buffer_zlib_uncompress, "buffer_zlib_uncompress");
		load(gm::buffer_read_int8, "buffer_read_int8");
		load(gm::buffer_read_uint8, "buffer_read_uint8");
		load(gm::buffer_read_int16, "buffer_read_int16");
		load(gm::buffer_read_uint16, "buffer_read_uint16");
		load(gm::buffer_read_int32, "buffer_read_int32");
		load(gm::buffer_read_uint32, "buffer_read_uint32");
		load(gm::buffer_read_int64, "buffer_read_int64");
		load(gm::buffer_read_uint64, "buffer_read_uint64");
		load(gm::buffer_read_intv, "buffer_read_intv");
		load(gm::buffer_read_uintv, "buffer_read_uintv");
		load(gm::buffer_read_float32, "buffer_read_float32");
		load(gm::buffer_read_float64, "buffer_read_float64");
		load(gm::buffer_write_int8, "buffer_write_int8");
		load(gm::buffer_write_uint8, "buffer_write_uint8");
		load(gm::buffer_write_int16, "buffer_write_int16");
		load(gm::buffer_write_uint16, "buffer_write_uint16");
		load(gm::buffer_write_int32, "buffer_write_int32");
		load(gm::buffer_write_uint32, "buffer_write_uint32");
		load(gm::buffer_write_int64, "buffer_write_int64");
		load(gm::buffer_write_uint64, "buffer_write_uint64");
		load(gm::buffer_write_intv, "buffer_write_intv");
		load(gm::buffer_write_uintv, "buffer_write_uintv");
		load(gm::buffer_write_float32, "buffer_write_float32");
		load(gm::buffer_write_float64, "buffer_write_float64");
		load(gm::buffer_write_string, "buffer_write_string");
		load(gm::buffer_read_string, "buffer_read_string");
		load(gm::buffer_write_data, "buffer_write_data");
		load(gm::buffer_read_data, "buffer_read_data");
		load(gm::buffer_write_hex, "buffer_write_hex");
		load(gm::buffer_read_hex, "buffer_read_hex");
		load(gm::buffer_write_buffer, "buffer_write_buffer");
		load(gm::buffer_write_buffer_part, "buffer_write_buffer_part");
		load(gm::buffer_get_address, "buffer_get_address");
		load(gm::buffer_set_size, "buffer_set_size");

		return gtrue;
	}
	simple_catch("ImportBufferModule", gfalse);
}

void gm::buffer_jump(gm_real id, int offset)
{
	gm::buffer_set_pos(id, gm::buffer_get_pos(id) + offset);
}