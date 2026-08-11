#pragma once
#include "../main.h"

extern int* game_text_halign;		// GM8 中被 draw_set_halign 修改的全局变量地址
extern int* game_text_valign;		// GM8 中被 draw_set_valign 修改的全局变量地址
extern d3dcolor* game_d3dcolor;		// GM8 中被 draw_set_color 和 draw_set_alpha 修改的全局变量地址

constexpr uint absence_character_unicode = '?';

namespace sdf
{
	/// 字体图集结构体
	struct glyphs
	{
		/// <summary>
		/// 字形结构体，包含一个字符的所有绘制信息
		/// </summary>
		struct glyph
		{
			struct rect
			{
				float left = 0.0f;
				float top = 0.0f;
				float right = 0.0f;
				float bottom = 0.0f;
			};

			uint unicode = 0;  // 字符的十进制 Unicode 编码

			// 水平步进。表示绘制完该字符后，光标应向右移动的距离。
			// 如果字体大小为 24px，实际像素步进为 advance * 24。
			float advance = 0.0f;

			// 字形在“虚拟排版平面”上的几何形状。它们的数值是相对于基线和原点的偏移量。
			// 单位：标准化字体单位（通常以字号为 1.0 进行缩放）。
			rect plane_bound;

			// 这一组数据直接对应字符集图片上的位置。单位：像素。
			rect atlas_bound;
		};

		uint width = 0;								// 该图集的宽
		uint height = 0;							// 该图集的高

		std::unordered_map<uint, glyph> glaph_map;	// 该图集含有的字形
		void* texture = nullptr;					// 该图集对应的 GPU 纹理(不透明: D3D8/9 纹理对象)

		uint id = 0;								// 纹理图集 ID

		// (可选)字体的偏移值
		float xoffset = 0;
		float yoffset = 0;

		float max_glyph_height = 0;  // 最大升部(基线上方),单位: 字号=1
		float max_glyph_depth = 0;   // 最大降部(基线下方),单位: 字号=1

		glyphs(std::string& image_path, std::string& csv_path);

		glyphs() = delete;

		~glyphs();
	};
	
	float string_width(std::string& str);
	float string_height(std::string& str);
	float string_width_ext(std::string& str, double w);
	float string_height_ext(std::string& str, double w);

	void draw_text(double x, double y, std::string& str);
	void draw_text_ext(double x, double y, std::string& str, double w);
	void draw_text_transformed(double x, double y, std::string& str, double xscale, 
		double yscale, double angle);
	void draw_text_ext_transformed(double x, double y, std::string& str, double w,
		double xscale, double yscale, double angle);
	void draw_text_color(double x, double y, std::string& str, int c1, int c2,
		int c3, int c4, double alpha);
	void draw_text_ext_color(double x, double y, std::string& str, double w, int c1, 
		int c2, int c3, int c4, double alpha);
	void draw_text_transformed_color(double x, double y, std::string& str, double xscale,
		double yscale, double angle, int c1, int c2, int c3, int c4,
		double alpha);
	void draw_text_ext_transformed_color(double x, double y, std::string& str, double w,
		double xscale, double yscale, double angle, int c1, int c2, int c3,
		int c4, double alpha);
	void draw_text_rich(double x, double y, std::string& str, double xscale, double yscale);

	/// 传入 inner_draw_text 函数的结构体，包含所有绘制信息
	struct draw_info
	{
		float x = 0, y = 0;
		float xscale = 1, yscale = 1;
		float rot = 0;

		d3dcolor col_lt = *game_d3dcolor;
		d3dcolor col_rt = *game_d3dcolor;
		d3dcolor col_rb = *game_d3dcolor;
		d3dcolor col_lb = *game_d3dcolor;
	};

	extern float game_font_size;		// 字体大小（单位：pt）
	extern float line_spacing;			// 行距（单位：像素）。默认值为 1
	extern bool per_line_halign;		// 是否逐行对齐。默认值为 false，即整段文本作为一个整体进行水平对齐
	extern bool use_shader;				// 是否使用着色器进行 SDF 字体的绘制。默认值为 true
	extern int shader;					// 当前使用的着色器 id(shader_create_asm 返回)。默认为 sdf_shader

	extern float font_sharpness;		// 字体的锐度。最好与字形纹理大小相同
	extern float font_thickness;		// 字体的粗细度。在 100 - 900 之间。默认值为 500
	extern float font_gap;				// 字与字之间的间隔

	typedef std::unique_ptr<glyphs> glyphs_ptr;

	/// 函数 composing_string 返回的结构体，包含了字符串被切分成多行后的每行信息，以及整段文本的宽高信息
	struct composed_string
	{
		struct line
		{
			std::vector<uint> str_unicode;
			float x = 0;
			float width = 0;

			float max_ascender = 0;   // 该行最大升部(基线上方高度), 单位: 像素
			float max_descender = 0;  // 该行最大降部(基线下方高度), 单位: 像素
		};

		std::vector<line> lines;
		float width = 0, height = 0;

		std::string raw;  // 原始字符串
	};

	/// 用于 sdf_draw_set_conf 等函数的结构体，能一次性配置多个绘制参数
	struct font_info
	{
		glyphs* font = nullptr;
		float size = 16.0f;
		float line_spacing = 1.0f;
		float sharpness = 24.0f;
		float thickness = 0.5f;
	};
}

extern std::unordered_map<uint, sdf::glyphs_ptr> game_sdf_glyphs;
extern std::unordered_map<uint, sdf::font_info> game_font_info;

extern sdf::glyphs* current_sdf_glyphs;

// Export Functions

exp_real sdf_add_font(gm_string image_path, gm_string csv_path);
exp_real sdf_delete_font(gm_real id);
exp_real sdf_release_cache();

exp_real draw_get_halign();
exp_real draw_get_valign();
exp_real sdf_draw_set_font(gm_real font_id);
exp_real sdf_draw_get_font();
exp_real sdf_draw_set_font_size(gm_real size);
exp_real sdf_draw_get_font_size();
exp_real sdf_draw_set_line_spacing(gm_real spacing);
exp_real sdf_draw_get_line_spacing();
exp_real sdf_draw_set_align_by_line(gm_real by_line);
exp_real sdf_draw_get_align_by_line();
exp_real sdf_draw_set_use_shader(gm_real use);
exp_real sdf_draw_get_use_shader();
exp_real sdf_draw_set_premul(gm_real premul);
exp_real sdf_draw_get_premul();
exp_real sdf_draw_set_font_sharpness(gm_real sharpness);
exp_real sdf_draw_get_font_sharpness();
exp_real sdf_draw_set_font_thickness(gm_real thickness);
exp_real sdf_draw_get_font_thickness();
exp_real sdf_draw_set_text_gap(gm_real gap);
exp_real sdf_draw_get_text_gap();
exp_real sdf_set_font_offset(gm_real id, gm_real xoffset, gm_real yoffset);

exp_real sdf_draw_set_conf(gm_real font, gm_real size, gm_real line_spacing,
	gm_real sharpness, gm_real thickness);
exp_real sdf_apply_conf(gm_real conf_id);
exp_real sdf_delete_conf(gm_real conf_id);
exp_real sdf_delete_all_conf();

exp_real sdf_string_width(gm_string str);
exp_real sdf_string_height(gm_string str);
exp_real sdf_string_width_ext(gm_string str, gm_real w);
exp_real sdf_string_height_ext(gm_string str, gm_real w);

exp_real sdf_draw_text(gm_real x, gm_real y, gm_string str);
exp_real sdf_draw_text_ext(gm_real x, gm_real y, gm_string str);
exp_real sdf_draw_text_transformed(gm_real x, gm_real y, gm_string str);
exp_real sdf_draw_text_ext_transformed(gm_real x, gm_real y, gm_string str);
exp_real sdf_draw_text_color(gm_real x, gm_real y, gm_string str);
exp_real sdf_draw_text_ext_color(gm_real x, gm_real y, gm_string str);
exp_real sdf_draw_text_transformed_color(gm_real x, gm_real y, gm_string str);
exp_real sdf_draw_text_ext_transformed_color(gm_real x, gm_real y, gm_string str);
exp_real sdf_draw_text_rich(gm_real x, gm_real y, gm_string str);