#include <stack>
#include <optional>
#include "math_s.h"
#include "utf8.h"
#include "parse_args.h"
#include "string_make.h"
#include "shader.h"
#include "draw_text.h"

struct rich_char
{
	struct style
	{
		float offset_x = 0;
		float offset_y = 0;

		d3dcolor color = *game_d3dcolor;

		float linespac = sdf::line_spacing;
		float thickness = sdf::font_thickness;
		float sharpness = sdf::font_sharpness;
		float size = sdf::game_font_size;
		float gap = sdf::font_gap;

		void apply() const
		{
			gm::draw_set_color((int)d3dcol_to_col(color));
			gm::draw_set_alpha(d3dcol_to_alpha(color));
			sdf::line_spacing = linespac;
			sdf::font_thickness = thickness;
			sdf::font_sharpness = sharpness;
			sdf::game_font_size = size;
			sdf::font_gap = gap;
		}
	};

	uint unicode;
	style str_style;
};

// 去除字符串首尾空白符
static std::string trim_spaces(const std::string& str)
{
	auto start = std::find_if_not(str.begin(), str.end(),
		[](uchar c) { return std::isspace(c); }
	);
	auto end = std::find_if_not(str.rbegin(), str.rend(),
		[](uchar c) { return std::isspace(c); }
	).base();

	return (start < end ? std::string(start, end) : std::string());
}

template<typename T>
static std::optional<T> parse_attr(std::string& attr)
{
	if constexpr (std::is_same_v<T, std::string>)  // 去除双引号
	{
		auto start = std::find_if_not(attr.begin(), attr.end(),
			[](uchar c) { return c == '"'; }
		);
		auto end = std::find_if_not(attr.rbegin(), attr.rend(),
			[](uchar c) { return c == '"'; }
		).base();

		std::string a = (start < end ? std::string(start, end) : std::string());
		a = trim_spaces(a);
		return a.empty() ? std::nullopt : std::optional(a);
	}
	else if constexpr (std::is_same_v<T, float>)  // 返回普通浮点数
	{
		try
		{
			return std::stof(attr);
		}
		catch (...)
		{
			return std::nullopt;
		}
	}
	else if constexpr (std::is_same_v<T, d3dcolor>)  // 颜色值
	{
		if (attr.empty())
			return std::nullopt;

		if (attr[0] == '#' || attr[0] == '$')  // 从十六进制值转换
		{
			std::string col_str = attr.substr(1);

			try
			{
				uint col = std::clamp((uint)std::stol("0x" + col_str), 0U, 0xFFFFFFFF);
				if (attr.length() <= 2)
					return col_d3d((int)d3dcol_to_col(*game_d3dcolor), (double)col / 256.0);
				else if (attr.length() <= 6)
				{
					if (attr[0] == '#')  // css 格式：#RRGGBB
					{
						int r = (col >> 16);
						int g = ((col >> 8) % 256);
						int b = (col % 256);

						return col_d3d(col_make(r, g, b), d3dcol_to_alpha(*game_d3dcolor));
					}
					else  // GM 格式：$BBGGRR
						return col_d3d((int)col, d3dcol_to_alpha(*game_d3dcolor));
				}
				else
				{
					if (attr[0] == '#')  // css 格式：#RRGGBBAA
					{
						int r = (col >> 24);
						int g = ((col >> 16) % 256);
						int b = ((col >> 8) % 256);
						double a = double(col % 256) / 256.0;

						return col_d3d(col_make(r, g, b), a);
					}
					else  // GM 格式：$AABBGGRR
					{
						int r = (col % 256);
						int g = ((col >> 8) % 256);
						int b = ((col >> 16) % 256);
						double a = double(col >> 24) / 256.0;

						return col_d3d(col_make(r, g, b), a);
					}
				}
			}
			catch (...)
			{
				return std::nullopt;
			}
		}
		else if (attr.length() > 1 && attr[0] == 'a')  // 从 a 函数转换
		{
			std::vector<std::string> tokens = string_token(attr, " ");
			if (tokens.size() < 2)
				return std::nullopt;

			double a = clamp(std::stof(tokens[1]), 0.0, 1.0);
			return col_d3d((int)d3dcol_to_col(*game_d3dcolor), a);
		}
		else if (attr.length() > 3 && attr.substr(0, 3) == "rgb")  // 从 rgb 函数转换
		{
			std::vector<std::string> tokens = string_token(attr, " ");
			if (tokens.size() < 4)
				return std::nullopt;

			int r = std::clamp(std::stoi(tokens[1]), 0, 255);
			int g = std::clamp(std::stoi(tokens[2]), 0, 255);
			int b = std::clamp(std::stoi(tokens[3]), 0, 255);
			int a = uint(clamp(d3dcol_to_alpha(*game_d3dcolor), 0.0, 1.0) * 255.0);

			return (a << 24) + (r << 16) + (g << 8) + b;
		}
		else if (attr.length() > 4 && attr.substr(0, 4) == "rgba")  // 从 rgba 函数转换
		{
			std::vector<std::string> tokens = string_token(attr, " ");
			if (tokens.size() < 5)
				return std::nullopt;

			int r = std::clamp(std::stoi(tokens[1]), 0, 255);
			int g = std::clamp(std::stoi(tokens[2]), 0, 255);
			int b = std::clamp(std::stoi(tokens[3]), 0, 255);
			int a = uint(clamp(std::stof(tokens[4]), 0.0, 1.0) * 255.0);

			return (a << 24) + (r << 16) + (g << 8) + b;
		}
		else if (attr.length() > 2 && attr.substr(0, 2) == "c_")  // 从颜色常量转换
		{
			int col = (int)d3dcol_to_col(*game_d3dcolor);

			if (attr == "c_aqua")			col = gm::c_aqua;
			else if (attr == "c_black")		col = gm::c_black;
			else if (attr == "c_blue")		col = gm::c_blue;
			else if (attr == "c_dkgray")	col = gm::c_dkgray;
			else if (attr == "c_fuchsia")	col = gm::c_fuchsia;
			else if (attr == "c_gray")		col = gm::c_gray;
			else if (attr == "c_green")		col = gm::c_green;
			else if (attr == "c_lime")		col = gm::c_lime;
			else if (attr == "c_ltgray")	col = gm::c_ltgray;
			else if (attr == "c_maroon")	col = gm::c_maroon;
			else if (attr == "c_navy")		col = gm::c_navy;
			else if (attr == "c_olive")		col = gm::c_olive;
			else if (attr == "c_orange")	col = gm::c_orange;
			else if (attr == "c_purple")	col = gm::c_purple;
			else if (attr == "c_red")		col = gm::c_red;
			else if (attr == "c_silver")	col = gm::c_silver;
			else if (attr == "c_teal")		col = gm::c_teal;
			else if (attr == "c_white")		col = gm::c_white;
			else if (attr == "c_yellow")	col = gm::c_yellow;

			return col_d3d((int)col, d3dcol_to_alpha(*game_d3dcolor));
		}
	}

	return std::nullopt;
}

static void parse_tag(rich_char::style& cur_style, std::string& tag_name,
	bool has_attr, std::string& attr_value)
{
	try
	{
		if (tag_name == "b")  // 加粗文字
		{
			if (has_attr)
			{
				auto value = parse_attr<float>(attr_value);
				if (value != std::nullopt)
					cur_style.thickness = value.value();
				else
					cur_style.thickness = sdf::font_thickness - 0.05f;
			}
			else
				cur_style.thickness = sdf::font_thickness - 0.05f;
		}
		else if (tag_name == "i")  // 斜体
		{
			if (has_attr)
			{

			}
		}
		else if (tag_name == "color")  // 设置字体颜色 / 颜色+Alpha
		{
			if (has_attr)
			{
				auto value = parse_attr<d3dcolor>(attr_value);
				if (value != std::nullopt)
					cur_style.color = value.value();
			}
		}
		else if (tag_name == "gap")  // 设置段落间隔
		{
			if (has_attr)
			{
				auto value = parse_attr<float>(attr_value);
				if (value != std::nullopt)
					cur_style.offset_x = value.value();
			}
		}
		else if (tag_name == "cgap")  // 设置文字间隔
		{
			if (has_attr)
			{
				auto value = parse_attr<float>(attr_value);
				if (value != std::nullopt)
					cur_style.gap = value.value();
			}
		}
	}
	transpond_catch("parse_tag(rich_string::style&, std::string&, bool, std::string&)")
}

static std::vector<rich_char> parse_rich_text(std::string& str)
{
	try
	{
		bool is_noparse = false;	// 是否在不进行解析的块内部

		std::vector<rich_char> result;

		std::stack<rich_char::style> state_stack;
		std::stack<std::string> tag_stack;
		rich_char::style cur_style;

		auto it = str.begin();
		auto end = str.end();

		while (it != end)
		{
			if (is_noparse)  // 不解析标签的状态
			{
				// 严格向前看是否匹配 "</noparse>"
				if (std::distance(it, end) >= 10 && std::string(it, it + 10) == "</noparse>")
				{
					is_noparse = false;
					it += 10;
					continue;
				}

				uint unicode = utf8::next(it, end);
				result.push_back({ unicode, cur_style });
				continue;
			}

			if (*it == '<')  // 标签开始块
			{
				auto tag_start = it;  // 记录起点，如果解析失败可以回退
				it++;

				// '< ' 是非法的，必须紧跟字母或 '/'
				if (it == end || std::isspace(*it))
				{
					it = tag_start;  // 解析失败，把 '<' 当作普通字符

					uint unicode = utf8::next(it, end);
					result.push_back({ unicode, cur_style });

					continue;
				}

				bool is_closing = false;

				if (it != end && *it == '/')  // 判断是否是结束标签 '</'
				{
					is_closing = true;
					it++;
				}

				// 提取标签内容
				std::string tag_name;
				while (it != end && !std::isspace((uchar)*it) &&
					*it != '=' && *it != '>')
				{
					tag_name += *it;
					it++;
				}

				if (tag_name.empty())
				{
					it = tag_start;

					uint unicode = utf8::next(it, end);
					result.push_back({ unicode, cur_style });

					continue;
				}

				while (it != end && std::isspace((uchar)*it))  // 跳过标签名后的空白符
					it++;

				// 提取属性值
				bool has_attr = false;
				std::string attr_value;
				if (it != end && *it == '=')
				{
					has_attr = true;
					it++;

					while (it != end && *it != '>')
					{
						attr_value += *it;
						it++;
					}
					attr_value = trim_spaces(attr_value);  // 删除属性值前后空格
				}
				else  // 若无属性值，继续跳过空白符直到遇到 '>'
				{
					while (it != end && std::isspace((uchar)*it))
						it++;
				}

				// 判断标签是否完美闭合
				if (it != end && *it == '>')
				{
					it++;

					// 处理标签逻辑
					if (!is_closing && tag_name == "noparse")  // 拦截特殊标签 noparse
					{
						is_noparse = true;
						continue;
					}

					if (tag_name == "br")  // 处理单标签换行 <br>
					{
						result.push_back({ '\n', cur_style});
						continue;
					}

					if (is_closing)  // 该标签是闭合标签
					{
						if (!tag_stack.empty() && tag_stack.top() == tag_name &&
							!state_stack.empty())
						{
							cur_style = state_stack.top();
							state_stack.pop();
							tag_stack.pop();
						}
					}
					else  // 该标签是开始标签
					{
						state_stack.push(cur_style);	// 将当前状态压栈备份
						tag_stack.push(tag_name);		// 将当前标签名称压栈

						parse_tag(cur_style, tag_name, has_attr, attr_value);  // 应用标签
					}
				}
				else  // 解析失败，按普通字符处理
				{
					it = tag_start;
					uint unicode = utf8::next(it, end);
					result.push_back({ unicode, cur_style });
				}
			}
			else  // 普通字符直接读取
			{
				uint unicode = utf8::next(it, end);
				result.push_back({ unicode, cur_style });
			}
		}

		return result;
	}
	transpond_catch("parse_rich_text(std::string&)")
}

// 富文本排版与渲染数据结构
struct composed_rich_string
{
	struct line
	{
		std::vector<rich_char> chars;
		float width = 0;
		float max_ascender = 0;   // 该行最大的升部（用于基线对齐）
		float line_height = 0;    // 该行实际占用的垂直高度
		float x = 0;
	};

	std::vector<line> lines;
	float width = 0;
	float height = 0;
};

static composed_rich_string composing_rich_string(const std::vector<rich_char>& rich_chars)
{
	composed_rich_string result;
	if (current_sdf_glyphs == nullptr)
		return result;

	sdf::glyphs& glyphs = *current_sdf_glyphs;
	composed_rich_string::line current_line;
	float max_width = 0;
	float total_height = 0;

	for (const auto& rc : rich_chars)
	{
		// 处理换行符
		if (rc.unicode == '\n')
		{
			if (!current_line.chars.empty())
			{
				current_line.width -= current_line.chars.back().str_style.gap *
					pt_to_px(current_line.chars.back().str_style.size);
			}

			// 如果是空行，给一个默认高度
			if (current_line.line_height == 0)
				current_line.line_height = pt_to_px(rc.str_style.size);

			total_height += current_line.line_height + rc.str_style.linespac;

			if (current_line.width > max_width)
				max_width = current_line.width;

			result.lines.push_back(std::move(current_line));
			current_line = composed_rich_string::line();
			continue;
		}

		// 获取字形
		uint unicode = rc.unicode;
		auto glyph_it = glyphs.glaph_map.find(unicode);
		if (glyph_it == glyphs.glaph_map.end())
		{
			unicode = absence_character_unicode;
			glyph_it = glyphs.glaph_map.find(unicode);
		}

		if (glyph_it != glyphs.glaph_map.end())
		{
			current_line.chars.push_back(rc);
			current_line.chars.back().unicode = unicode; // 修正为 fallback 字符（如果发生）

			float font_size_px = pt_to_px(rc.str_style.size);
			current_line.width += (glyph_it->second.advance + rc.str_style.gap) * font_size_px;

			// 计算该字符的升部，刷新当前行的最大基线高度
			float ascender = glyphs.max_glyph_height * font_size_px;
			if (ascender > current_line.max_ascender)
				current_line.max_ascender = ascender;

			if (font_size_px > current_line.line_height)
				current_line.line_height = font_size_px;
		}
	}

	// 推入最后一行
	if (!current_line.chars.empty())
	{
		current_line.width -= current_line.chars.back().str_style.gap *
			pt_to_px(current_line.chars.back().str_style.size);
	}

	if (current_line.line_height == 0 && !rich_chars.empty())
		current_line.line_height = pt_to_px(rich_chars.back().str_style.size);

	total_height += current_line.line_height;

	if (current_line.width > max_width)
		max_width = current_line.width;

	result.lines.push_back(std::move(current_line));
	result.width = max_width;
	result.height = total_height;

	// 处理单行水平对齐 (fa_center / fa_right)
	if (sdf::per_line_halign)
	{
		for (auto& l : result.lines)
		{
			if (*game_text_halign == gm::fa_center)
				l.x = (result.width - l.width) / 2.0f;
			else if (*game_text_halign == gm::fa_right)
				l.x = result.width - l.width;
		}
	}

	return result;
}

// ----------------------------------------------------------------------------
// 富文本渲染引擎
// ----------------------------------------------------------------------------
static void inner_draw_text_rich(composed_rich_string& str, float draw_x, float draw_y, 
	float xscale, float yscale)
{
	try
	{
		if (str.lines.empty() || current_sdf_glyphs == nullptr)
			return;

		sdf::glyphs& glyphs = *current_sdf_glyphs;

		if (current_texture.texture != glyphs.texture)
		{
			atlas::end_draw();
			atlas::start_draw(glyphs.texture, D3DFMT_A8);
		}

		// 1. 计算全局偏移 (受 fa_left, fa_top 等影响)
		float offset_x = glyphs.xoffset * pt_to_px(sdf::game_font_size) * xscale;
		float offset_y = glyphs.yoffset * pt_to_px(sdf::game_font_size) * yscale;

		if (*game_text_valign == gm::fa_middle)
			offset_y -= str.height / 2.0f * yscale;
		else if (*game_text_valign == gm::fa_bottom)
			offset_y -= str.height * yscale;

		if (*game_text_halign == gm::fa_center)
			offset_x -= str.width / 2.0f * xscale;
		else if (*game_text_halign == gm::fa_right)
			offset_x -= str.width * xscale;

		float cursor_y = offset_y;

		// 备份当前的全局着色器常量
		float base_thickness = sdf::font_thickness;
		float base_sharpness = sdf::font_sharpness;
		float active_thickness = base_thickness;
		float active_sharpness = base_sharpness;

		// 2. 迭代绘制每一行
		for (const auto& line : str.lines)
		{
			float cursor_x = offset_x + line.x * xscale;

			// 核心：基于该行最大的升部确定物理基线位置
			float baseline_y = cursor_y + line.max_ascender * yscale;

			for (const auto& rc : line.chars)
			{
				// --- 批次打断逻辑 ---
				// 由于粗细度和锐度是通过 Shader 常量控制的，如果遇到不同配置的字符，
				// 我们必须先渲染之前的顶点，更新常量，再开启新的批次。
				if (std::abs(rc.str_style.thickness - active_thickness) > 0.001f ||
					std::abs(rc.str_style.sharpness - active_sharpness) > 0.001f)
				{
					atlas::end_draw(); // 提交当前批次

					// 更新全局常量供下次提交使用
					sdf::font_thickness = rc.str_style.thickness;
					sdf::font_sharpness = rc.str_style.sharpness;
					active_thickness = rc.str_style.thickness;
					active_sharpness = rc.str_style.sharpness;

					atlas::start_draw(glyphs.texture, D3DFMT_A8); // 开启新批次
				}

				// 处理缓冲区溢出
				if (vbuff_c + 6 >= vb_count)
				{
					atlas::end_draw();
					atlas::start_draw(glyphs.texture, D3DFMT_A8);
				}

				auto glyph_it = glyphs.glaph_map.find(rc.unicode);
				sdf::glyphs::glyph& glyph = glyph_it->second;

				float font_size_px = pt_to_px(rc.str_style.size);

				// 纹理坐标
				float u0 = glyph.atlas_bound.left / (float)glyphs.width;
				float v0 = glyph.atlas_bound.top / (float)glyphs.height;
				float u1 = glyph.atlas_bound.right / (float)glyphs.width;
				float v1 = glyph.atlas_bound.bottom / (float)glyphs.height;

				// 局部包围盒 (基线对齐)
				float left = glyph.plane_bound.left * font_size_px * xscale + cursor_x - 0.5f;
				float top = glyph.plane_bound.top * font_size_px * yscale + baseline_y - 0.5f;
				float right = glyph.plane_bound.right * font_size_px * xscale + cursor_x - 0.5f;
				float bottom = glyph.plane_bound.bottom * font_size_px * yscale + baseline_y - 0.5f;

				// 全局坐标 (加上绘制起点与用户设定的 tag offset)
				float x_lt = draw_x + left + rc.str_style.offset_x * xscale;
				float y_lt = draw_y + top + rc.str_style.offset_y * yscale;
				float x_rt = draw_x + right + rc.str_style.offset_x * xscale;
				float y_rt = y_lt;
				float x_rb = x_rt;
				float y_rb = draw_y + bottom + rc.str_style.offset_y * yscale;
				float x_lb = x_lt;
				float y_lb = y_rb;

				d3dcolor col = rc.str_style.color;

				// 压入三角形顶点 (颜色直接取自富文本样式)
				vertex::push_vertex_2d(x_lt, y_lt, u0, v0, col);
				vertex::push_vertex_2d(x_rt, y_rt, u1, v0, col);
				vertex::push_vertex_2d(x_rb, y_rb, u1, v1, col);

				vertex::push_vertex_2d(x_lt, y_lt, u0, v0, col);
				vertex::push_vertex_2d(x_rb, y_rb, u1, v1, col);
				vertex::push_vertex_2d(x_lb, y_lb, u0, v1, col);

				// 推进 X 光标
				cursor_x += (glyph.advance + rc.str_style.gap) * xscale * font_size_px;
			}

			// 推进 Y 光标至下一行
			float linespac = line.chars.empty()
				? sdf::line_spacing
				: line.chars.front().str_style.linespac;

			cursor_y += (line.line_height + linespac) * yscale;
		}

		// 3. 恢复全局着色器状态
		if (active_thickness != base_thickness || active_sharpness != base_sharpness)
		{
			atlas::end_draw();
			sdf::font_thickness = base_thickness;
			sdf::font_sharpness = base_sharpness;
		}
	}
	transpond_catch("inner_draw_text_rich(composed_rich_string&, double, double, double, double)")
}

void sdf::draw_text_rich(double x, double y, std::string& str, double xscale, double yscale)
{
	try
	{
		rich_char::style initial_style;

		std::vector<rich_char> rich_chars = parse_rich_text(str);
		composed_rich_string composed = composing_rich_string(rich_chars);
		inner_draw_text_rich(composed, (float)x, (float)y, (float)xscale, (float)yscale);

		initial_style.apply();
	}
	transpond_catch("sdf::draw_text_rich(double, double, std::string&, double, double)")
}

exp_real sdf_draw_text_rich(gm_real x, gm_real y, gm_string str)
{
	try
	{
		gm_real args[2]{};
		if (parse_args(args) < 2)
			return gfalse;

		gm_real xscale = args[0];
		gm_real yscale = args[1];

		std::string text(str);
		sdf::draw_text_rich(x, y, text, xscale, yscale);
		return gtrue;
	}
	simple_catch("sdf_draw_text_rich", gfalse)
}