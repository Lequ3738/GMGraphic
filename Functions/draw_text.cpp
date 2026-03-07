#include "lodepng.h"
#include "utf8.h"
#include <fstream>
#include "shader.h"
#include "draw_text.h"

double sdf::game_font_size = 16.0;
double sdf::line_spacing = 1.0;
bool sdf::per_line_halign = false;
bool sdf::use_shader = true;
dword sdf::shader = (dword)-1;

double sdf::font_sharpness = 24;		// 字体的锐度。在 0 - 32 之间
double sdf::font_thickness = 0.5;	// 字体的粗细度。在 0 - 1 之间

sdf::glyphs* current_sdf_glyphs = nullptr;

static std::vector<std::string> string_token(std::string& str, std::string&& sep)
{
	std::vector<std::string> tokens;
	size_t pos = 0, found = 0;
	while ((found = str.find(",", pos)) != std::string::npos)
	{
		std::string token = str.substr(pos, found - pos);
		pos = found + sep.length();

		if (!token.empty())
			tokens.push_back(std::move(token));
	}

	if (pos <= str.length())
	{
		std::string token = str.substr(pos);
		if (!token.empty())
			tokens.push_back(std::move(token));
	}

	return tokens;
}

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

		IDirect3DDevice8* device = gmapi->GetDirect3DDevice();

		D3DCheck(device->CreateTexture(width, height, 1, 0, D3DFMT_A8, D3DPOOL_DEFAULT, 
			&texture), 0);

		IDirect3DSurface8* surface = nullptr;
		D3DCheck(texture->GetSurfaceLevel(0, &surface), 1);

		RECT pos_rect = { .left = 0, .top = 0, .right = (long)width, .bottom = (long)height };
		D3DCheck(D3DXLoadSurfaceFromMemory(surface, nullptr, &pos_rect, alp_image.data(),
			D3DFMT_A8, width, nullptr, &pos_rect, D3DX_FILTER_NONE, 0), 2);

		D3DCheck(texture->AddDirtyRect(&pos_rect), 3);
		surface->Release();

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
				.advance = std::stod(tokens[1]),
				.plane_bound = {
					std::stod(tokens[2]), std::stod(tokens[3]), 
					std::stod(tokens[4]), std::stod(tokens[5])
				},
				.atlas_bound = {
					std::stod(tokens[6]), std::stod(tokens[7]),
					std::stod(tokens[8]), std::stod(tokens[9])
				}
			};

			glaph_map[unicode] = std::move(charset);
		}
	}
	transpond_catch("sdf::glyphs::glyphs(std::string&, std::string&)")
}

sdf::glyphs::~glyphs()
{
	if (texture == nullptr)
		return;

	texture->Release();
	texture = nullptr;

	if (current_sdf_glyphs == this)
		current_sdf_glyphs = nullptr;
}

int* game_text_halign = (int*)0x58F2B4;
int* game_text_valign = (int*)0x58F2B8;
d3dcolor* game_d3dcolor = (d3dcolor*)0x58D344;

constexpr uint absence_character_unicode = '?';

static float pt_to_px(float pt) { return pt * 96.0f / 72.0f; }

static sdf::composed_string composing_string(std::string& str)
{
	try
	{
		if (current_sdf_glyphs == nullptr)
			throw std::runtime_error("No font is currently set for drawing text.");

		std::istringstream str_stream(str);
		std::string line_str;
		sdf::glyphs& glyphs = *current_sdf_glyphs;

		sdf::composed_string result;
		float font_size = pt_to_px((float)sdf::game_font_size);
		float max_width = 0, height = 0;

		while (std::getline(str_stream, line_str))
		{
			sdf::composed_string::line line_glyphs;
			float width = 0;
			
			auto cur_it = line_str.begin();
			auto end_it = line_str.end();

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
				width += (float)glyph.advance * font_size;
			}

			height += font_size + (float)sdf::line_spacing;
			if (width > max_width)
				max_width = width;

			line_glyphs.width = width;
			result.lines.push_back(std::move(line_glyphs));
		}

		height -= (float)sdf::line_spacing;
		result.width = max_width;
		result.height = height;

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

static void inner_draw_text(sdf::composed_string& str, sdf::draw_info& info)
{
	try
	{
		if (current_sdf_glyphs == nullptr)
			throw std::runtime_error("No font is currently set for drawing text.");

		// 切换到字体图集纹理并准备绘制
		if (current_texture.texture != current_sdf_glyphs->texture)
		{
			atlas::end_draw();
			atlas::start_draw(current_sdf_glyphs->texture, D3DFMT_A8);
		}

		sdf::glyphs& glyphs = *current_sdf_glyphs;
		float font_size = pt_to_px((float)sdf::game_font_size);

		// 计算对齐后的绘制位置
		float offset_x = 0, offset_y = 0;

		if (*game_text_valign == gm::fa_middle)
			offset_y = -str.height / 2.0f;
		else if (*game_text_valign == gm::fa_bottom)
			offset_y = -str.height;

		if (*game_text_halign == gm::fa_center)
			offset_x = -str.width / 2.0f;
		else if (*game_text_valign == gm::fa_right)
			offset_x = -str.width;

		float cursor_y = offset_y;

		double rot_c = 0, rot_s = 0;
		if (std::abs(info.rot) > 0.00000001)
		{
			rot_c = std::cos(info.rot);
			rot_s = std::sin(info.rot);
		}

		// 进行逐字符绘制
		for (uint line_i = 0; line_i < str.lines.size(); ++line_i)
		{
			auto& line = str.lines[line_i];
			float cursor_x = offset_x + line.x;

			for (uint i = 0; i < line.str_unicode.size(); ++i)
			{
				uint unicode = line.str_unicode[i];
				auto glyph_it = glyphs.glaph_map.find(unicode);
				sdf::glyphs::glyph& glyph = glyph_it->second;

				// 添加顶点
				float u0 = (float)glyph.atlas_bound.left / (float)glyphs.width;
				float v0 = (float)glyph.atlas_bound.top / (float)glyphs.height;
				float u1 = (float)glyph.atlas_bound.right / (float)glyphs.width;
				float v1 = (float)glyph.atlas_bound.bottom / (float)glyphs.height;

				float left = (float)(glyph.plane_bound.left * font_size * info.xscale) + 
					cursor_x - 0.5f;
				float top = (float)(glyph.plane_bound.top * font_size * info.yscale) + 
					cursor_y - 0.5f;
				float right = (float)(glyph.plane_bound.right * font_size * info.xscale) +
					cursor_x - 0.5f;
				float bottom = (float)(glyph.plane_bound.bottom * font_size * info.yscale) +
					cursor_y - 0.5f;

				float x_lt = 0, y_lt = 0, x_rt = 0, y_rt = 0, x_rb = 0, y_rb = 0, 
					x_lb = 0, y_lb = 0;

				if (std::abs(info.rot) > 0.00000001)
				{
					x_lt = static_cast<float>(info.x + left * rot_c + top * rot_s);
					y_lt = static_cast<float>(info.y - left * rot_s + top * rot_c);
					x_rt = static_cast<float>(info.x + right * rot_c + top * rot_s);
					y_rt = static_cast<float>(info.y - right * rot_s + top * rot_c);
					x_rb = static_cast<float>(info.x + right * rot_c + bottom * rot_s);
					y_rb = static_cast<float>(info.y - right * rot_s + bottom * rot_c);
					x_lb = static_cast<float>(info.x + left * rot_c + bottom * rot_s);
					y_lb = static_cast<float>(info.y - left * rot_s + bottom * rot_c);
				}
				else
				{
					x_lt = static_cast<float>(info.x + left);
					y_lt = static_cast<float>(info.y + top);
					x_rt = static_cast<float>(info.x + right);
					y_rt = y_lt;
					x_rb = x_rt;
					y_rb = static_cast<float>(info.y + bottom);
					x_lb = x_lt;
					y_lb = y_rb;
				}

				// 三角形 1
				vert_ext* vert = vertex::get_struct();
				vert->x = x_lt; vert->y = y_lt; vert->c = info.col_lt;
				vert->uv[0] = u0; vert->uv[1] = v0;

				vert = vertex::get_struct();
				vert->x = x_rt; vert->y = y_rt; vert->c = info.col_rt;
				vert->uv[0] = u1; vert->uv[1] = v0;

				vert = vertex::get_struct();
				vert->x = x_rb; vert->y = y_rb; vert->c = info.col_rb;
				vert->uv[0] = u1; vert->uv[1] = v1;

				// 三角形 2
				vert = vertex::get_struct();
				vert->x = x_lt; vert->y = y_lt; vert->c = info.col_lt;
				vert->uv[0] = u0; vert->uv[1] = v0;

				vert = vertex::get_struct();
				vert->x = x_rb; vert->y = y_rb; vert->c = info.col_rb;
				vert->uv[0] = u1; vert->uv[1] = v1;

				vert = vertex::get_struct();
				vert->x = x_lb; vert->y = y_lb; vert->c = info.col_lb;
				vert->uv[0] = u0; vert->uv[1] = v1;

				// 根据字形的水平步进调整绘制位置
				cursor_x += (float)(glyph.advance * info.xscale) * font_size;
			}

			cursor_y += font_size + (float)sdf::line_spacing;
		}
	}
	transpond_catch("inner_draw_text(sdf::composed_string&, sdf::draw_info&)")
}

void sdf::draw_text(double x, double y, std::string& str)
{
	try
	{
		auto composing = composing_string(str);
		sdf::draw_info info = { .x = x, .y = y };
		inner_draw_text(composing, info);
	}
	transpond_catch("sdf::draw_text(double, double, std::string&)")
}

// ============================================================================
// Export Functions
// ============================================================================

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
	sdf::game_font_size = size;
	return gtrue;
}
exp_real sdf_draw_get_font_size() { return sdf::game_font_size; }

exp_real sdf_draw_set_line_spacing(gm_real spacing)
{
	sdf::line_spacing = spacing;
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
		sdf::shader = ps_sdf_premul_comp;
	else
		sdf::shader = ps_sdf_comp;
	return gtrue;
}
exp_real sdf_draw_get_premul()
{
	return (sdf::shader == ps_sdf_premul_comp) ? gtrue : gfalse;
}

exp_real sdf_draw_set_font_sharpness(gm_real sharpness)
{
	sdf::font_sharpness = sharpness;
	return gtrue;
}
exp_real sdf_draw_get_font_sharpness() { return sdf::font_sharpness; }

exp_real sdf_draw_set_font_thickness(gm_real thickness)
{
	sdf::font_thickness = thickness;
	return gtrue;
}
exp_real sdf_draw_get_font_thickness() { return sdf::font_thickness; }

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
			.size = size,
			.line_spacing = line_spacing,
			.sharpness = sharpness,
			.thickness = thickness
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