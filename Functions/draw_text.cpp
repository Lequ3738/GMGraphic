#include "lodepng.h"
#include "utf8.h"
#include <fstream>
#include "shader.h"
#include "draw_text.h"

double sdf::game_font_size = 24.0;
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

sdf::glyphs::glyphs(std::string& image_path, std::string& csv_path, uint font_size)
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
		glyphs::font_size = font_size;

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

static int* game_text_halign = (int*)0x58F2B4;
static int* game_text_valign = (int*)0x58F2B8;
static d3dcolor* game_d3dcolor = (d3dcolor*)0x58D344;

constexpr uint absence_character_unicode = '?';

static void draw_text_line(sdf::draw_info& info)
{
	try
	{
		if (current_sdf_glyphs == nullptr)
			throw std::runtime_error("No font is currently set for drawing text.");

		sdf::glyphs& glyphs = *current_sdf_glyphs;
		auto cur_it = info.str.begin();
		auto end_it = info.str.end();

		while (cur_it != end_it)
		{
			// 获取一个 utf-8 字符码点
			uint unicode = utf8::next(cur_it, end_it);
			
			// 在字形图集中查找该字符的字形数据
			auto glyph_it = glyphs.glaph_map.find(unicode);
			if (glyph_it == glyphs.glaph_map.end())
				glyph_it = glyphs.glaph_map.find(absence_character_unicode);
			if (glyph_it == glyphs.glaph_map.end())
			{
				throw std::runtime_error("The font does not contain the absence "
					"character (?).");
			}
			sdf::glyphs::glyph glyph = glyph_it->second;
		}
	}
	transpond_catch("draw_text_line(sdf::draw_info&)")
}

void sdf::draw_text(double x, double y, std::string& str)
{
	try
	{

	}
	transpond_catch("sdf::draw_text(double, double, std::string&)")
}

// ============================================================================
// Export Functions
// ============================================================================

std::unordered_map<uint, sdf::glyphs_ptr> game_sdf_glyphs;
uint glyphs_id_position = 1;

exp_real sdf_add_font(gm_string image_path, gm_string csv_path, gm_real font_size)
{
	try
	{
		std::string image(image_path), csv(csv_path);
		uint size = (uint)font_size;
		uint id = glyphs_id_position++;

		game_sdf_glyphs[id] = std::make_unique<sdf::glyphs>(image, csv, size);
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

exp_real sdf_draw_text(gm_real x, gm_real y, gm_string str)
{
	try
	{
	}
	simple_catch("sdf_draw_text", gfalse)
}