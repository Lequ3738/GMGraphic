#pragma once
#include "../main.h"

extern int* game_text_halign;
extern int* game_text_valign;
extern d3dcolor* game_d3dcolor;

namespace sdf
{
	struct glyphs
	{
		struct glyph
		{
			struct rect
			{
				double left = 0;
				double top = 0;
				double right = 0;
				double bottom = 0;
			};

			uint unicode = 0;  // 字符的十进制 Unicode 编码

			// 水平步进。表示绘制完该字符后，光标应向右移动的距离。
			// 如果字体大小为 24px，实际像素步进为 advance * 24。
			double advance = 0.0;

			// 字形在“虚拟排版平面”上的几何形状。它们的数值是相对于基线和原点的偏移量。
			// 单位：标准化字体单位（通常以字号为 1.0 进行缩放）。
			rect plane_bound;

			// 这一组数据直接对应字符集图片上的位置。单位：像素。
			rect atlas_bound;
		};

		uint width = 0;								// 该图集的宽
		uint height = 0;							// 该图集的高
		uint font_size = 0;							// 字体大小（以像素为单位）

		std::unordered_map<uint, glyph> glaph_map;	// 该图集含有的字形
		IDirect3DTexture8* texture = nullptr;		// 该图集对应的 GPU 纹理

		uint id = 0;								// 纹理图集 ID

		glyphs(std::string& image_path, std::string& csv_path, uint font_size);

		glyphs() = delete;

		~glyphs();
	};

	void draw_text(double x, double y, std::string& str);

	struct draw_info
	{
		double x = 0, y = 0;
		double xscale = 1, yscale = 1;
		double rot = 0;

		d3dcolor col_lt = *game_d3dcolor;
		d3dcolor col_rt = *game_d3dcolor;
		d3dcolor col_rb = *game_d3dcolor;
		d3dcolor col_lb = *game_d3dcolor;
	};

	extern double game_font_size;
	extern double line_spacing;
	extern bool per_line_halign;

	typedef std::unique_ptr<glyphs> glyphs_ptr;

	struct composed_string
	{
		struct line
		{
			std::vector<uint> str_unicode;
			float x = 0, width = 0;
		};

		std::vector<line> lines;
		float width = 0, height = 0;
	};
}

extern std::unordered_map<uint, sdf::glyphs_ptr> game_sdf_glyphs;

// ============================================================================
// Export Functions
// ============================================================================

exp_real sdf_add_font(gm_string image_path, gm_string csv_path, gm_real font_size);
exp_real sdf_delete_font(gm_real id);

exp_real draw_get_halign();
exp_real draw_get_valign();
exp_real sdf_draw_set_font(gm_real font_id);
exp_real sdf_draw_get_font();
exp_real sdf_draw_set_font_size(gm_real size);
exp_real sdf_draw_get_font_size();

exp_real sdf_draw_text(gm_real x, gm_real y, gm_string str);