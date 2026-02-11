#pragma once
#include "..\main.h"

extern HMODULE BufferDLL;

typedef double (*r_v)();
typedef double (*r_r)(double);
typedef double (*r_rr)(double, double);
typedef double (*r_rrrr)(double, double, double, double);	
typedef double (*r_rs)(double, const char*);
typedef double (*r_rsrr)(double, const char*, double, double);
typedef const char* (*s_r)(double);
typedef const char* (*s_rr)(double, double);

namespace gm
{
	extern r_v buffer_create;
	extern r_r buffer_destroy, buffer_exists, buffer_get_pos, buffer_get_size;
	extern r_r buffer_at_end, buffer_get_error, buffer_clear_error, buffer_clear;
	extern r_r buffer_zlib_compress, buffer_zlib_uncompress, buffer_read_int8;
	extern r_r buffer_read_uint8, buffer_read_int16, buffer_read_uint16;
	extern r_r buffer_read_int32, buffer_read_uint32, buffer_read_int64, buffer_read_uint64;
	extern r_r buffer_read_intv, buffer_read_uintv, buffer_read_float32, buffer_read_float64;
	extern s_r buffer_to_string, buffer_read_string;
	extern r_rr buffer_set_pos, buffer_rc4_crypt_buffer, buffer_write_int8, buffer_write_uint8;
	extern r_rr buffer_write_int16, buffer_write_uint16, buffer_write_int32;
	extern r_rr buffer_write_uint32, buffer_write_int64, buffer_write_uint64;
	extern r_rr buffer_write_intv, buffer_write_uintv, buffer_write_float32, buffer_write_float64;
	extern r_rr buffer_write_buffer, buffer_get_address, buffer_set_size;
	extern r_rs buffer_read_from_file, buffer_write_to_file, buffer_append_to_file;
	extern r_rs buffer_rc4_crypt, buffer_write_string, buffer_write_data, buffer_write_hex;
	extern r_rsrr buffer_read_from_file_part;
	extern s_rr buffer_read_data, buffer_read_hex;
	extern r_rrrr buffer_write_buffer_part;

	void buffer_jump(gm_real id, int offset);
}