#include <fstream>
#include "lodepng.h"
#include "math_s.h"
#include "utf8.h"
#include "shader.h"
#include "parse_args.h"
#include "linebreak.h"
#include "xxhash.hpp"
#include "string_make.h"
#include "draw_text.h"

int* game_text_halign = (int*)0x58F2B4;
int* game_text_valign = (int*)0x58F2B8;
d3dcolor* game_d3dcolor = (d3dcolor*)0x58D344;

float sdf::game_font_size = 16.0f;
float sdf::line_spacing = 1.0f;
bool sdf::per_line_halign = false;
bool sdf::use_shader = true;
int sdf::shader = -1;

float sdf::font_sharpness = 24.0f;		// 字体的锐度。最好与字体纹理中单个字体大小相同
float sdf::font_thickness = 500.0f;		// 字体的粗细度。100~900之间
float sdf::font_gap = 0.0f;

sdf::glyphs* current_sdf_glyphs = nullptr;

sdf::glyphs::glyphs(std::string& image_path, std::string& csv_path)
{
	try
	{
		// Load image
		std::vector<uchar> image, alp_image;
		uint width, height;
		uint error = lodepng::decode(image, width, height, image_path);
		if (error)
			throw std::runtime_error(lodepng_error_text(error));

		glyphs::width = width;
		glyphs::height = height;

		alp_image.resize(image.size() / 4);
		for (size_t i = 0; i < image.size() / 4; i += 1)
			alp_image[i] = image[i * 4];

		// 建纹理 + 上传像素(整链经适配器, 双后端通用)
		D3DCheck(d3d::create_texture(width, height, 1, 0, D3DFMT_A8, D3DPOOL_DEFAULT,
			&texture), 0);
		D3DCheck(d3d::upload_texture(texture, width, height, D3DFMT_A8,
			alp_image.data(), width), 1);

		// Load CSV
		std::ifstream csv_stream(csv_path);
		if (!csv_stream.is_open())
			throw std::runtime_error("Fail to open the file (" + csv_path + ").");

		std::string data = {
			std::istreambuf_iterator<char>(csv_stream),
			std::istreambuf_iterator<char>()
		};

		std::istringstream csv(data);
		std::string line;

		while (std::getline(csv, line))
		{
			if (line == "")
				continue;

			std::vector<std::string> tokens = string_token(line, ",");
			if (tokens.size() < 10)
				continue;

			uint unicode = (uint)std::stoi(tokens[0]);

			glyphs::glyph charset = {
				.unicode = unicode,
				.advance = std::stof(tokens[1]),
				.plane_bound = {
					std::stof(tokens[2]), std::stof(tokens[3]),
					std::stof(tokens[4]), std::stof(tokens[5])
				},
				.atlas_bound = {
					std::stof(tokens[6]), std::stof(tokens[7]),
					std::stof(tokens[8]), std::stof(tokens[9])
				}
			};

			if (max_glyph_height < -charset.plane_bound.top)
				max_glyph_height = -charset.plane_bound.top;

			glaph_map[unicode] = std::move(charset);
		}

		// Add char '\0'
		glaph_map[0] = glyphs::glyph();
	}
	transpond_catch("sdf::glyphs::glyphs(std::string&, std::string&)")
}

sdf::glyphs::~glyphs()
{
	if (texture == nullptr)
		return;

	d3d::release(texture);
	texture = nullptr;

	if (current_sdf_glyphs == this)
		current_sdf_glyphs = nullptr;
}

static sdf::composed_string composing_string(std::string& str)
{
	try
	{
		if (current_sdf_glyphs == nullptr)
			throw std::runtime_error("No font is currently set for drawing text.");

		std::vector<std::string> line_str = split_lines(str);
		sdf::glyphs& glyphs = *current_sdf_glyphs;

		sdf::composed_string result;
		float font_size = pt_to_px(sdf::game_font_size);
		float max_width = 0, height = 0;

		for (uint i = 0; i < line_str.size(); ++i)
		{
			sdf::composed_string::line line_glyphs;
			float width = 0;
			
			auto cur_it = line_str[i].begin();
			auto end_it = line_str[i].end();

			while (cur_it != end_it)
			{
				// 获取一个 utf-8 字符码点
				uint unicode = utf8::next(cur_it, end_it);
				
				// 在字形图集中查找该字符的字形数据
				auto glyph_it = glyphs.glaph_map.find(unicode);
				if (glyph_it == glyphs.glaph_map.end())
				{
					unicode = absence_character_unicode;
					glyph_it = glyphs.glaph_map.find(unicode);
				}
				if (glyph_it == glyphs.glaph_map.end())
				{
					throw std::runtime_error("The font does not contain the absence "
						"character (?).");
				}
				sdf::glyphs::glyph& glyph = glyph_it->second;
				line_glyphs.str_unicode.push_back(unicode);

				// 计算该行的宽
				width += (glyph.advance + sdf::font_gap) * font_size;
			}

			if (!line_glyphs.str_unicode.empty())
				width -= sdf::font_gap * font_size;

			height += font_size + sdf::line_spacing;
			if (width > max_width)
				max_width = width;

			line_glyphs.width = width;
			result.lines.push_back(std::move(line_glyphs));
		}

		if (!line_str.empty())
			height -= sdf::line_spacing;
		else
			height = font_size;

		result.width = max_width;
		result.height = height;
		result.raw = str;

		if (sdf::per_line_halign)
		{
			for (uint i = 0; i < result.lines.size(); ++i)
			{
				if (*game_text_halign == gm::fa_center)
					result.lines[i].x = (result.width - result.lines[i].width) / 2.0f;
				else if (*game_text_halign == gm::fa_right)
					result.lines[i].x = result.width - result.lines[i].width;
			}
		}

		return result;
	}
	transpond_catch("composing_string(std::string&)")
}

using hash_map_composed = std::unordered_map<xxh::hash64_t, std::vector<sdf::composed_string>>;
hash_map_composed composed_string_map;

static xxh::hash64_t string_hash(std::string& str, double width)
{
	try
	{
		xxh::hash_state_t<64> hs;

		hs.update(str.data(), str.size());
		hs.update(&width, sizeof(width));

		hs.update(&current_sdf_glyphs, sizeof(current_sdf_glyphs));
		hs.update(&sdf::game_font_size, sizeof(sdf::game_font_size));
		hs.update(&sdf::font_gap, sizeof(sdf::font_gap));
		hs.update(&sdf::line_spacing, sizeof(sdf::line_spacing));
		hs.update(&sdf::per_line_halign, sizeof(sdf::per_line_halign));

		return hs.digest();
	}
	transpond_catch("string_hash(std::string&, double)")
}

static void inner_draw_text(sdf::composed_string& str, sdf::draw_info& info)
{
	try
	{
		if (str.lines.empty())
			return;

		if (current_sdf_glyphs == nullptr)
			throw std::runtime_error("No font is currently set for drawing text.");

		// 切换到字体图集纹理并准备绘制
		if (current_texture.texture != current_sdf_glyphs->texture)
		{
			atlas::end_draw();
			atlas::start_draw(current_sdf_glyphs->texture, D3DFMT_A8);
		}

		sdf::glyphs& glyphs = *current_sdf_glyphs;
		float font_size = pt_to_px(sdf::game_font_size);

		// 计算对齐后的绘制位置
		float offset_x = glyphs.xoffset * font_size * info.xscale;
		float offset_y = glyphs.yoffset * font_size * info.yscale;

		if (*game_text_valign == gm::fa_middle)
			offset_y -= str.height / 2.0f * info.yscale;
		else if (*game_text_valign == gm::fa_bottom)
			offset_y -= str.height * info.yscale;

		if (*game_text_halign == gm::fa_center)
			offset_x -= str.width / 2.0f * info.xscale;
		else if (*game_text_halign == gm::fa_right)
			offset_x -= str.width * info.xscale;

		float cursor_y = offset_y + glyphs.max_glyph_height * font_size * info.yscale;

		float rot_c = 0, rot_s = 0;
		if (std::abs(info.rot) > 0.00000001)
		{
			rot_c = std::cos(info.rot);
			rot_s = std::sin(info.rot);
		}

		// 进行逐字符绘制
		for (uint line_i = 0; line_i < str.lines.size(); ++line_i)
		{
			auto& line = str.lines[line_i];
			float cursor_x = offset_x + line.x * info.xscale;

			for (uint i = 0; i < line.str_unicode.size(); ++i)
			{
				if (vbuff_c + 6 >= vb_count)
				{
					atlas::end_draw();
					atlas::start_draw(current_sdf_glyphs->texture, D3DFMT_A8);
				}

				uint unicode = line.str_unicode[i];
				auto glyph_it = glyphs.glaph_map.find(unicode);
				sdf::glyphs::glyph& glyph = glyph_it->second;

				// 添加顶点
				float u0 = glyph.atlas_bound.left / (float)glyphs.width;
				float v0 = glyph.atlas_bound.top / (float)glyphs.height;
				float u1 = glyph.atlas_bound.right / (float)glyphs.width;
				float v1 = glyph.atlas_bound.bottom / (float)glyphs.height;

				float left = glyph.plane_bound.left * font_size * info.xscale + cursor_x - 0.5f;
				float top = glyph.plane_bound.top * font_size * info.yscale + cursor_y - 0.5f;
				float right = glyph.plane_bound.right * font_size * info.xscale + cursor_x - 0.5f;
				float bottom = glyph.plane_bound.bottom * font_size * info.yscale + cursor_y - 0.5f;

				float x_lt = 0, y_lt = 0, x_rt = 0, y_rt = 0, x_rb = 0, y_rb = 0, 
					x_lb = 0, y_lb = 0;

				if (std::abs(info.rot) > 0.00000001)
				{
					x_lt = info.x + left * rot_c + top * rot_s;
					y_lt = info.y - left * rot_s + top * rot_c;
					x_rt = info.x + right * rot_c + top * rot_s;
					y_rt = info.y - right * rot_s + top * rot_c;
					x_rb = info.x + right * rot_c + bottom * rot_s;
					y_rb = info.y - right * rot_s + bottom * rot_c;
					x_lb = info.x + left * rot_c + bottom * rot_s;
					y_lb = info.y - left * rot_s + bottom * rot_c;
				}
				else
				{
					x_lt = info.x + left;
					y_lt = info.y + top;
					x_rt = info.x + right;
					y_rt = y_lt;
					x_rb = x_rt;
					y_rb = info.y + bottom;
					x_lb = x_lt;
					y_lb = y_rb;
				}

				// 三角形 1
				vertex::push_vertex_2d(x_lt, y_lt, u0, v0, info.col_lt);
				vertex::push_vertex_2d(x_rt, y_rt, u1, v0, info.col_rt);
				vertex::push_vertex_2d(x_rb, y_rb, u1, v1, info.col_rb);

				// 三角形 2
				vertex::push_vertex_2d(x_lt, y_lt, u0, v0, info.col_lt);
				vertex::push_vertex_2d(x_rb, y_rb, u1, v1, info.col_rb);
				vertex::push_vertex_2d(x_lb, y_lb, u0, v1, info.col_lb);

				// 根据字形的水平步进调整绘制位置
				cursor_x += (glyph.advance + sdf::font_gap) * info.xscale * font_size;
			}

			cursor_y += (font_size + sdf::line_spacing) * info.yscale;
		}
	}
	transpond_catch("inner_draw_text(sdf::composed_string&, sdf::draw_info&)")
}

static float string_width_nohash(std::string& str)
{
	try
	{
		auto comp = composing_string(str);
		return comp.width;
	}
	transpond_catch("string_width_nohash(std::string&)")
}

static std::string string_get_ext(gm_string str, gm_real w, gm_string lang)
{
	try
	{
		if (w <= 0)
			return "In function gui_get_string_ext(): The argument is valid.";

		if (*str == '\0')
			return "";

		std::string text(str);

		gm_string l = lang;
		if (lang != nullptr && *lang == '\0')
			l = nullptr;

		// 获取指定字符串的“可合法断点”列表
		size_t len = text.length() + 1;
		std::vector<char> brks(len);
		set_linebreaks_utf8((const utf8_t*)str, len, l, brks.data());

		// 按 utf-8 字符分隔字符串
		std::vector<std::string> tokens = string_token_utf8(text);

		size_t num = tokens.size();

		std::string token,	// 从上一个合法断点到当前处理字符的字符串
			line,			// 从当前行开始到上一个合法断点的字符串
			result;			// 结果字符串

		// 计算要绘制的字符宽度并自动断行
		size_t brk_pos = 0;
		for (size_t i = 0; i < num; i++)
		{
			// 将基于字节的“可合法断点”列表由基于字符的模式读取
			size_t chr_len = tokens[i].length();
			if (chr_len == 0)
			{
				return "In function string_get_ext():"
					"An Error has occurred in function StringToken().";
			}

			brk_pos += chr_len;
			char br = brks[brk_pos - 1];

			token += tokens[i];

			// 遇到库标记出错（断在字符内部）
			if (br == LINEBREAK_INSIDEACHAR)
			{
				return "In function string_get_ext(): "
					"An Error has occurred in character position ("
					+ std::to_string(i) + " - " + std::to_string(i + 1) + ").";
			}

			// 当到达可断点、必须断点或字符串末尾时处理 token
			if (br == LINEBREAK_ALLOWBREAK || br == LINEBREAK_MUSTBREAK || i == num - 1)
			{
				std::string candidate = line + token;
				double width = (double)string_width_nohash(candidate);

				if (width <= w)  // 放得下，直接合并
					line = std::move(candidate);
				else  // 放不下，需要换行
				{
					if (!line.empty())
					{
						result += line + "\n";
						line = token;
					}

					// 检查新起的这一行（原本的 token）是否依然超宽
					// 如果 token 本身极长，这里需要强制拆分
					if ((double)string_width_nohash(line) > w)
					{
						std::string remain_line = "";
						std::string temp_token = line;
						line.clear();

						auto tok_end = temp_token.end();
						auto tok_it = temp_token.begin();
						auto tok_prev = tok_it;

						while (tok_it != tok_end)
						{
							utf8::next(tok_it, tok_end);
							std::string character(tok_prev, tok_it);

							if (!character.empty())
							{
								// 尝试加入字符
								std::string line_plus_char = remain_line + character;
								if ((double)string_width_nohash(line_plus_char) > w)
								{
									// 加上这个字就超了 -> 输出前面的 safe 部分
									result += remain_line + "\n";
									remain_line = character; // 当前字变为下一行开头
								}
								else
								{
									remain_line += character;
								}
							}
							tok_prev = tok_it;
						}
						// 剩下的部分留在 line 中，等待后续处理
						line = remain_line;
					}
				}

				token.clear();

				if (br == LINEBREAK_MUSTBREAK)
				{
					result += line;

					// 如果 line 结尾不是换行符，则手动添加
					if (line.empty() || (line.back() != '\n' && line.back() != '\r'))
						result += "\n";

					line.clear();
				}
			}
		}

		// 将残余内容加入结果（如果有）
		if (!line.empty())
			result += line;
		else if (!token.empty())
			result += token;

		return result;
	}
	transpond_catch("string_get_ext(gm_string, gm_real, gm_string)")
}

static sdf::composed_string& hash_get_composed_string(std::string& str, double w)
{
	xxh::hash64_t hash = string_hash(str, w);

	auto it = composed_string_map.find(hash);
	if (it != composed_string_map.end())
	{
		if (it->second.size() == 1)
			return it->second[0];
		else
		{
			for (auto& comp : it->second)
			{
				if (comp.raw == str && comp.width <= w)
					return comp;
			}
		}
	}

	if (w <= 0)
	{
		auto comp = composing_string(str);
		composed_string_map[hash].push_back(std::move(comp));
		return composed_string_map[hash].back();
	}
	else
	{
		std::string str_w = string_get_ext(str.c_str(), w, nullptr);
		auto comp = composing_string(str_w);
		composed_string_map[hash].push_back(std::move(comp));
		return composed_string_map[hash].back();
	}
}

float sdf::string_width(std::string& str)
{
	auto& comp = hash_get_composed_string(str, 0);
	return comp.width;
}
float sdf::string_height(std::string& str)
{
	auto& comp = hash_get_composed_string(str, 0);
	return comp.height;
}

float sdf::string_width_ext(std::string& str, double w)
{
	auto& comp = hash_get_composed_string(str, w);
	return comp.width;
}
float sdf::string_height_ext(std::string& str, double w)
{
	auto& comp = hash_get_composed_string(str, w);
	return comp.height;
}

void sdf::draw_text(double x, double y, std::string& str)
{
	try
	{
		auto& composing = hash_get_composed_string(str, 0);
		sdf::draw_info info = { .x = (float)x, .y = (float)y };
		inner_draw_text(composing, info);
	}
	transpond_catch("sdf::draw_text(double, double, std::string&)")
}

void sdf::draw_text_ext(double x, double y, std::string& str, double w)
{
	try
	{
		auto& composing = hash_get_composed_string(str, w);
		sdf::draw_info info = { .x = (float)x, .y = (float)y };
		inner_draw_text(composing, info);
	}
	transpond_catch("sdf::draw_text_ext(double, double, std::string&, double)")
}

void sdf::draw_text_transformed(double x, double y, std::string& str, double xscale,
	double yscale, double angle)
{
	try
	{
		auto& composing = hash_get_composed_string(str, 0);
		sdf::draw_info info = {
			.x = (float)x, .y = (float)y,
			.xscale = (float)xscale, .yscale = (float)yscale, 
			.rot = float(angle * pi / 180)
		};
		inner_draw_text(composing, info);
	}
	transpond_catch("sdf::draw_text_transformed(double, double, std::string&, double, "
		"double, double)")
}

void sdf::draw_text_ext_transformed(double x, double y, std::string& str, double w,
	double xscale, double yscale, double angle)
{
	try
	{
		auto& composing = hash_get_composed_string(str, w);
		sdf::draw_info info = {
			.x = (float)x, .y = (float)y, 
			.xscale = (float)xscale, .yscale = (float)yscale, 
			.rot = float(angle * pi / 180)
		};
		inner_draw_text(composing, info);
	}
	transpond_catch("sdf::draw_text_ext_transformed(double, double, std::string&, double, "
		"double, double, double)")
}

void sdf::draw_text_color(double x, double y, std::string& str, int c1, int c2, int c3, 
	int c4, double alpha)
{
	try
	{
		d3dcolor col1 = col_d3d(c1, alpha);
		d3dcolor col2 = col_d3d(c2, alpha);
		d3dcolor col3 = col_d3d(c3, alpha);
		d3dcolor col4 = col_d3d(c4, alpha);

		auto& composing = hash_get_composed_string(str, 0);
		sdf::draw_info info = {
			.x = (float)x, .y = (float)y,
			.col_lt = col1, .col_rt = col2, .col_rb = col3, .col_lb = col4
		};
		inner_draw_text(composing, info);
	}
	transpond_catch("sdf::draw_text_color(double, double, std::string&, int, int, "
		"int, int, double)")
}

void sdf::draw_text_ext_color(double x, double y, std::string& str, double w, int c1,
	int c2, int c3, int c4, double alpha)
{
	try
	{
		d3dcolor col1 = col_d3d(c1, alpha);
		d3dcolor col2 = col_d3d(c2, alpha);
		d3dcolor col3 = col_d3d(c3, alpha);
		d3dcolor col4 = col_d3d(c4, alpha);

		auto& composing = hash_get_composed_string(str, w);
		sdf::draw_info info = {
			.x = (float)x, .y = (float)y,
			.col_lt = col1, .col_rt = col2, .col_rb = col3, .col_lb = col4
		};
		inner_draw_text(composing, info);
	}
	transpond_catch("sdf::draw_text_ext_color(double, double, std::string&, double, "
		"int, int, int, int, double)")
}

void sdf::draw_text_transformed_color(double x, double y, std::string& str, double xscale,
	double yscale, double angle, int c1, int c2, int c3, int c4,
	double alpha)
{
	try
	{
		d3dcolor col1 = col_d3d(c1, alpha);
		d3dcolor col2 = col_d3d(c2, alpha);
		d3dcolor col3 = col_d3d(c3, alpha);
		d3dcolor col4 = col_d3d(c4, alpha);

		auto& composing = hash_get_composed_string(str, 0);
		sdf::draw_info info = {
			.x = (float)x, .y = (float)y, 
			.xscale = (float)xscale, .yscale = (float)yscale, 
			.rot = float(angle * pi / 180),
			.col_lt = col1, .col_rt = col2, .col_rb = col3, .col_lb = col4
		};
		inner_draw_text(composing, info);
	}
	transpond_catch("sdf::draw_text_transformed_color(double, double, std::string&, "
		"double, double, double, int, int, int, int, double)")
}

void sdf::draw_text_ext_transformed_color(double x, double y, std::string& str, double w,
	double xscale, double yscale, double angle, int c1, int c2, int c3,
	int c4, double alpha)
{
	try
	{
		d3dcolor col1 = col_d3d(c1, alpha);
		d3dcolor col2 = col_d3d(c2, alpha);
		d3dcolor col3 = col_d3d(c3, alpha);
		d3dcolor col4 = col_d3d(c4, alpha);

		auto& composing = hash_get_composed_string(str, w);
		sdf::draw_info info = {
			.x = (float)x, .y = (float)y,
			.xscale = (float)xscale, .yscale = (float)yscale,
			.rot = float(angle * pi / 180),
			.col_lt = col1, .col_rt = col2, .col_rb = col3, .col_lb = col4
		};
		inner_draw_text(composing, info);
	}
	transpond_catch("sdf::draw_text_ext_transformed_color(double, double, std::string&, "
		"double, double, double, double, int, int, int, int, double)")
}

// ======================= Export Functions =======================

std::unordered_map<uint, sdf::glyphs_ptr> game_sdf_glyphs;
uint glyphs_id_position = 1;

exp_real sdf_add_font(gm_string image_path, gm_string csv_path)
{
	try
	{
		std::string image(image_path), csv(csv_path);
		uint id = glyphs_id_position++;

		game_sdf_glyphs[id] = std::make_unique<sdf::glyphs>(image, csv);
		game_sdf_glyphs[id].get()->id = id;

		return (gm_real)id;
	}
	simple_catch("sdf_add_font", gm::noone)
}

exp_real sdf_delete_font(gm_real id)
{
	try
	{
		game_sdf_glyphs.erase((uint)id);
		return gtrue;
	}
	simple_catch("sdf_delete_font", gfalse)
}

exp_real sdf_release_cache()
{
	composed_string_map.clear();
	return gtrue;
}

exp_real draw_get_halign() { return (gm_real)(*game_text_halign); }
exp_real draw_get_valign() { return (gm_real)(*game_text_valign); }

exp_real sdf_draw_set_font(gm_real font_id)
{
	try
	{
		current_sdf_glyphs = game_sdf_glyphs.at((uint)font_id).get();
		return gtrue;
	}
	simple_catch("sdf_draw_set_font", gfalse)
}

exp_real sdf_draw_get_font() 
{ 
	if (current_sdf_glyphs == nullptr)
		return gm::noone;
	return (gm_real)current_sdf_glyphs->id; 
}

exp_real sdf_draw_set_font_size(gm_real size)
{
	sdf::game_font_size = (float)size;
	return gtrue;
}
exp_real sdf_draw_get_font_size() { return sdf::game_font_size; }

exp_real sdf_draw_set_line_spacing(gm_real spacing)
{
	sdf::line_spacing = (float)spacing;
	return gtrue;
}
exp_real sdf_draw_get_line_spacing() { return sdf::line_spacing; }

exp_real sdf_draw_set_align_by_line(gm_real by_line)
{
	sdf::per_line_halign = (by_line >= 0.5);
	return gtrue;
}
exp_real sdf_draw_get_align_by_line() { return sdf::per_line_halign ? gtrue : gfalse; }

exp_real sdf_draw_set_use_shader(gm_real use)
{
	sdf::use_shader = (use >= 0.5);
	return gtrue;
}
exp_real sdf_draw_get_use_shader() { return sdf::use_shader ? gtrue : gfalse; }

exp_real sdf_draw_set_premul(gm_real premul)
{
	if (premul >= 0.5)
		sdf::shader = sdf_shader_premul;
	else
		sdf::shader = sdf_shader;
	return gtrue;
}
exp_real sdf_draw_get_premul()
{
	return (sdf::shader == sdf_shader_premul) ? gtrue : gfalse;
}

exp_real sdf_draw_set_font_sharpness(gm_real sharpness)
{
	sdf::font_sharpness = (float)sharpness;
	return gtrue;
}
exp_real sdf_draw_get_font_sharpness() { return sdf::font_sharpness; }

exp_real sdf_draw_set_font_thickness(gm_real thickness)
{
	sdf::font_thickness = (float)thickness;
	return gtrue;
}
exp_real sdf_draw_get_font_thickness() { return sdf::font_thickness; }

exp_real sdf_draw_set_text_gap(gm_real gap)
{
	sdf::font_gap = (float)gap;
	return gtrue;
}
exp_real sdf_draw_get_text_gap() { return sdf::font_gap; }

exp_real sdf_set_font_offset(gm_real id, gm_real xoffset, gm_real yoffset)
{
	try
	{
		sdf::glyphs* font = game_sdf_glyphs.at((uint)id).get();
		font->xoffset = (float)xoffset;
		font->yoffset = (float)yoffset;

		return gtrue;
	}
	simple_catch("sdf_set_font_offset", gfalse)
}

std::unordered_map<uint, sdf::font_info> game_font_info;
uint font_info_position = 10000;

exp_real sdf_draw_set_conf(gm_real font, gm_real size, gm_real line_spacing,
	gm_real sharpness, gm_real thickness)
{
	try
	{
		uint id = font_info_position++;

		game_font_info[id] = {
			.font = game_sdf_glyphs.at((uint)font).get(),
			.size = (float)size,
			.line_spacing = (float)line_spacing,
			.sharpness = (float)sharpness,
			.thickness = (float)thickness
		};
		return id;
	}
	simple_catch("sdf_draw_set_conf", gm::noone)
}
exp_real sdf_apply_conf(gm_real conf_id)
{
	try
	{
		sdf::font_info& conf = game_font_info.at((uint)conf_id);

		current_sdf_glyphs = conf.font;
		sdf::game_font_size = conf.size;
		sdf::line_spacing = conf.line_spacing;
		sdf::font_sharpness = conf.sharpness;
		sdf::font_thickness = conf.thickness;
		return gtrue;
	}
	simple_catch("sdf_apply_conf", gfalse)
}
exp_real sdf_delete_conf(gm_real conf_id)
{
	try
	{
		game_font_info.erase((uint)conf_id);
		return gtrue;
	}
	simple_catch("sdf_delete_conf", gfalse)
}
exp_real sdf_delete_all_conf()
{
	game_font_info.clear();
	return gtrue;
}

exp_real sdf_string_width(gm_string str)
{
	try
	{
		std::string text(str);
		return sdf::string_width(text);
	}
	simple_catch("sdf_string_width", 0.0)
}

exp_real sdf_string_height(gm_string str)
{
	try
	{
		std::string text(str);
		return sdf::string_height(text);
	}
	simple_catch("sdf_string_height", 0.0)
}

exp_real sdf_string_width_ext(gm_string str, gm_real w)
{
	try
	{
		std::string text(str);
		return sdf::string_width_ext(text, w);
	}
	simple_catch("sdf_string_width_ext", 0.0)
}

exp_real sdf_string_height_ext(gm_string str, gm_real w)
{
	try
	{
		std::string text(str);
		return sdf::string_height_ext(text, w);
	}
	simple_catch("sdf_string_height_ext", 0.0)
}

exp_real sdf_draw_text(gm_real x, gm_real y, gm_string str)
{
	try
	{
		std::string text(str);
		sdf::draw_text(x, y, text);
		return gtrue;
	}
	simple_catch("sdf_draw_text", gfalse)
}

exp_real sdf_draw_text_ext(gm_real x, gm_real y, gm_string str)
{
	try
	{
		gm_real args[1]{};
		if (parse_args(args) < 1)
			return gfalse;

		gm_real w = args[0];

		std::string text(str);
		sdf::draw_text_ext(x, y, text, w);
		return gtrue;
	}
	simple_catch("sdf_draw_text_ext", gfalse)
}

exp_real sdf_draw_text_transformed(gm_real x, gm_real y, gm_string str)
{
	try
	{
		gm_real args[3]{};
		if (parse_args(args) < 3)
			return gfalse;

		gm_real xscale = args[0];
		gm_real yscale = args[1];
		gm_real angle = args[2];

		std::string text(str);
		sdf::draw_text_transformed(x, y, text, xscale, yscale, angle);
		return gtrue;
	}
	simple_catch("sdf_draw_text_transformed", gfalse)
}

exp_real sdf_draw_text_ext_transformed(gm_real x, gm_real y, gm_string str)
{
	try
	{
		gm_real args[4]{};
		if (parse_args(args) < 4)
			return gfalse;

		gm_real w = args[0];
		gm_real xscale = args[1];
		gm_real yscale = args[2];
		gm_real angle = args[3];

		std::string text(str);
		sdf::draw_text_ext_transformed(x, y, text, w, xscale, yscale, angle);
		return gtrue;
	}
	simple_catch("sdf_draw_text_ext_transformed", gfalse)
}

exp_real sdf_draw_text_color(gm_real x, gm_real y, gm_string str)
{
	try
	{
		gm_real args[5]{};
		if (parse_args(args) < 5)
			return gfalse;

		int c1 = (int)args[0];
		int c2 = (int)args[1];
		int c3 = (int)args[2];
		int c4 = (int)args[3];
		gm_real alpha = args[4];

		std::string text(str);
		sdf::draw_text_color(x, y, text, c1, c2, c3, c4, alpha);
		return gtrue;
	}
	simple_catch("sdf_draw_text_color", gfalse)
}

exp_real sdf_draw_text_ext_color(gm_real x, gm_real y, gm_string str)
{
	try
	{
		gm_real args[6]{};
		if (parse_args(args) < 6)
			return gfalse;

		gm_real w = args[0];
		int c1 = (int)args[1];
		int c2 = (int)args[2];
		int c3 = (int)args[3];
		int c4 = (int)args[4];
		gm_real alpha = args[5];

		std::string text(str);
		sdf::draw_text_ext_color(x, y, text, w, c1, c2, c3, c4, alpha);
		return gtrue;
	}
	simple_catch("sdf_draw_text_ext_color", gfalse)
}

exp_real sdf_draw_text_transformed_color(gm_real x, gm_real y, gm_string str)
{
	try
	{
		gm_real args[8]{};
		if (parse_args(args) < 8)
			return gfalse;

		gm_real xscale = args[0];
		gm_real yscale = args[1];
		gm_real angle = args[2];
		int c1 = (int)args[3];
		int c2 = (int)args[4];
		int c3 = (int)args[5];
		int c4 = (int)args[6];
		gm_real alpha = args[7];

		std::string text(str);
		sdf::draw_text_transformed_color(x, y, text, xscale, yscale, angle, c1, c2, c3,
			c4, alpha);
		return gtrue;
	}
	simple_catch("sdf_draw_text_transformed_color", gfalse)
}

exp_real sdf_draw_text_ext_transformed_color(gm_real x, gm_real y, gm_string str)
{
	try
	{
		gm_real args[9]{};
		if (parse_args(args) < 9)
			return gfalse;

		gm_real w = args[0];
		gm_real xscale = args[1];
		gm_real yscale = args[2];
		gm_real angle = args[3];
		int c1 = (int)args[4];
		int c2 = (int)args[5];
		int c3 = (int)args[6];
		int c4 = (int)args[7];
		gm_real alpha = args[8];

		std::string text(str);
		sdf::draw_text_ext_transformed_color(x, y, text, w, xscale, yscale, angle, 
			c1, c2, c3, c4, alpha);
		return gtrue;
	}
	simple_catch("sdf_draw_text_ext_transformed_color", gfalse)
}