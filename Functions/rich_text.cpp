#include <stack>
#include <optional>
#include "math_s.h"
#include "utf8.h"
#include "parse_args.h"
#include "string_make.h"
#include "linebreak.h"
#include "shader.h"
#include "draw_text.h"

enum class text_halign
{
	default_value = -1,
	left = 0,
	middle,
	right,
	justified,
	flush
};

enum class text_casing
{
	normal = 0,
	lower,
	upper
};

struct rich_char
{
	struct style
	{
		float offset_x = 0;  // X绘制偏移，不会影响到字体排版
		float offset_y = 0;  // Y绘制偏移，不会影响到字体排版

		float advance_x = 0;  // 步进(一个标签只应用它之后的一个字符)，会影响到它之后的字体排版
		float advance_y = 0;

		d3dcolor color = *game_d3dcolor;

		float linespac = sdf::line_spacing;
		float thickness = sdf::font_thickness;
		float sharpness = sdf::font_sharpness;
		float size = sdf::game_font_size;
		float gap = sdf::font_gap;

		float italic = 0;
		float line_width = 0;
		float indent = 0;	// 缩进
		float padding = 0;	// 内边距
		uint font_id = 0;	// 字体ID（0表示使用当前默认）
		text_halign halign = text_halign::default_value;
		text_casing casing = text_casing::normal;
		bool nobr = false;	// 是否禁止换行

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

		void reset_special_value()  // 还原只应用一个字符的值
		{
			advance_x = 0;
			advance_y = 0;
		}
	};

	uint unicode;
	style char_style;
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
		else if (attr.length() > 3 &&
			(attr.substr(0, 3) == "rgb" || attr.substr(0, 4) == "rgba"))  // 从 rgb/rgba 函数转换
		{
			// 支持 CSS 风格 rgb(r,g,b) / rgba(r,g,b,a), 也兼容空格分隔无括号写法。
			// 统一把括号、逗号、分号归一化为空格后再切分。
			std::string inner;
			size_t lp = attr.find('(');
			size_t rp = attr.rfind(')');
			if (lp != std::string::npos && rp != std::string::npos && rp > lp)
				inner = attr.substr(lp + 1, rp - lp - 1);
			else
				inner = attr.substr(attr[3] == 'a' ? 4 : 3);

			for (auto& ch : inner)
			{
				if (ch == ',' || ch == ';')
					ch = ' ';
			}

			std::vector<std::string> tokens = string_token(inner, " ");
			if (tokens.size() < 3)
				return std::nullopt;

			int r, g, b;
			double a;
			try
			{
				r = std::clamp(std::stoi(tokens[0]), 0, 255);
				g = std::clamp(std::stoi(tokens[1]), 0, 255);
				b = std::clamp(std::stoi(tokens[2]), 0, 255);
			}
			catch (...)
			{
				return std::nullopt;
			}

			if (tokens.size() >= 4)
			{
				try
				{
					a = std::clamp(std::stof(tokens[3]), 0.0f, 1.0f);
				}
				catch (...)
				{
					return std::nullopt;
				}
			}
			else
				a = d3dcol_to_alpha(*game_d3dcolor);

			return col_d3d(col_make(r, g, b), a);
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

	try
	{
		if constexpr (std::is_same_v<T, float>)  // 返回普通浮点数
			return std::stof(attr);
		else if constexpr (std::is_same_v<T, uint>)  // 返回uint
			return (uint)std::stol(attr);
	}
	catch (...)
	{
		return std::nullopt;
	}

	return std::nullopt;
}

static void parse_tag(rich_char::style& cur_style, std::string& tag_name,
	bool has_attr, std::string& attr_value)
{
	try
	{
		auto get_float = [&]() {
			return has_attr ? parse_attr<float>(attr_value) : std::nullopt;
		};

		if (tag_name == "b")  // 加粗文字
			cur_style.thickness = get_float().value_or(sdf::font_thickness + 70.0f);
		else if (tag_name == "i")  // 斜体
			cur_style.italic = get_float().value_or(0.3f);
		else if (tag_name == "cgap")  // 设置文字间隔
			cur_style.gap = get_float().value_or(cur_style.gap);
		else if (tag_name == "nobr")  // 使被该标签包裹的文本避免因换行而被分割开来
			cur_style.nobr = true;
		else if (tag_name == "indent")
			cur_style.indent = get_float().value_or(cur_style.indent);
		else if (tag_name == "lowercase")
			cur_style.casing = text_casing::lower;
		else if (tag_name == "uppercase")
			cur_style.casing = text_casing::upper;
		else if (tag_name == "size")
			cur_style.size = get_float().value_or(cur_style.size);
		else if (tag_name == "yoffset")
			cur_style.advance_y = get_float().value_or(0.0f);
		else if (tag_name == "color")  // 设置字体颜色 / 颜色+Alpha
		{
			if (has_attr)
			{
				auto value = parse_attr<d3dcolor>(attr_value);
				if (value != std::nullopt)
					cur_style.color = value.value();
			}
		}
		else if (tag_name == "font")  // 字体
		{
			if (has_attr)
			{
				auto value = parse_attr<uint>(attr_value);
				if (value != std::nullopt)
					cur_style.font_id = value.value();
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

		auto push_char = [&](uint unicode) {
			// 处理大小写转换
			if (cur_style.casing == text_casing::lower && unicode >= 'A' && unicode <= 'Z')
				unicode += 32;
			else if (cur_style.casing == text_casing::upper && unicode >= 'a' && unicode <= 'z')
				unicode -= 32;

			result.push_back({ unicode, cur_style });
			cur_style.reset_special_value();
		};

		while (it != end)
		{
			if (is_noparse)  // 不解析标签的状态
			{
				// 严格向前看是否匹配 "</noparse>"
				if (std::distance(it, end) >= 10 && std::equal(it, it + 10, "</noparse>"))
				{
					is_noparse = false;
					it += 10;
					continue;
				}

				push_char(utf8::next(it, end));
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
					push_char(utf8::next(it, end));
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
					push_char(utf8::next(it, end));
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

					// 单标签
					if (tag_name == "br")  // 处理单标签换行 <br>
					{
						push_char('\n');
						continue;
					}
					else if (tag_name == "gap")  // 设置段落间隔
					{
						if (has_attr)
						{
							auto value = parse_attr<float>(attr_value);
							if (value != std::nullopt)
								cur_style.advance_x = value.value();
						}

						push_char('\0');
						continue;
					}
					else if (tag_name == "padding")
					{
						if (has_attr)
						{
							auto value = parse_attr<float>(attr_value);
							if (value != std::nullopt)
								cur_style.padding = value.value();
						}

						push_char('\0');
						continue;
					}
					else if (tag_name == "width")  // 约束宽度
					{
						if (has_attr)
						{
							auto value = parse_attr<float>(attr_value);
							if (value != std::nullopt)
								cur_style.line_width = value.value();
						}

						push_char('\0');
						continue;
					}
					else if (tag_name == "align")  // 文本对齐
					{
						if (has_attr)
						{
							auto value = parse_attr<std::string>(attr_value);
							if (value != std::nullopt)
							{
								if (*value == "left")
									cur_style.halign = text_halign::default_value;
								else if (*value == "center" || *value == "middle")
									cur_style.halign = text_halign::middle;
								else if (*value == "right")
									cur_style.halign = text_halign::right;
								else if (*value == "justified")
									cur_style.halign = text_halign::justified;
								else if (*value == "flush")
									cur_style.halign = text_halign::flush;
							}
						}

						push_char('\0');
						continue;
					}

					// 闭合标签
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
						if (tag_stack.empty() || tag_stack.top() != tag_name)
						{
							state_stack.push(cur_style);	// 将当前状态压栈备份
							tag_stack.push(tag_name);	// 将当前标签名称压栈
						}

						parse_tag(cur_style, tag_name, has_attr, attr_value);  // 应用标签
					}
				}
				else  // 解析失败，按普通字符处理
				{
					it = tag_start;
					push_char(utf8::next(it, end));
				}
			}
			else  // 普通字符直接读取
				push_char(utf8::next(it, end));
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
		float max_ascender = 0;   // 该行最大的升部（基线上方高度）
		float max_descender = 0;  // 该行最大的降部（基线下方高度）
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

	// 获取整个富文本序列的安全换行点
	size_t len = rich_chars.size();
	std::vector<utf32_t> utf32_chars(len);
	for (size_t i = 0; i < len; ++i)
		utf32_chars[i] = (utf32_t)rich_chars[i].unicode;

	std::vector<char> brks(len, 0);
	if (len > 0)
		set_linebreaks_utf32(utf32_chars.data(), len, nullptr, brks.data());

	composed_rich_string::line current_line;
	float max_width = 0, total_height = 0;

	// 将当前行结算并压入结果集中
	auto commit_line = [&](bool is_last_paragraph_line)
	{
		if (current_line.chars.empty())
		{
			total_height += current_line.line_height + sdf::line_spacing;
			result.lines.push_back(std::move(current_line));
			current_line = composed_rich_string::line();
			return;
		}

		auto& first_style = current_line.chars.front().char_style;
		float l_width = first_style.line_width;
		float padding = first_style.padding;
		float indent = first_style.indent;
		int align = (int)first_style.halign >= 0 ? (int)first_style.halign : *game_text_halign;

		// 扣除行尾多余的间隙
		current_line.width -= current_line.chars.back().char_style.gap * 
			pt_to_px(current_line.chars.back().char_style.size);

		float target_content_w = l_width > 0 ? (l_width - indent - padding * 2) : 0;

		// 分散对齐 (Justified & Flush)
		if (target_content_w > 0 && current_line.width < target_content_w)
		{
			if (align == 4 || (align == 3 && !is_last_paragraph_line))
			{
				int spaces = 0;
				for (auto& c : current_line.chars) if (c.unicode == ' ') spaces++;
				if (spaces > 0)
				{
					float extra = (target_content_w - current_line.width) / spaces;
					for (auto& c : current_line.chars)
					{
						if (c.unicode == ' ')
							c.char_style.advance_x += extra / pt_to_px(c.char_style.size);
					}
					current_line.width = target_content_w;
				}
			}
		}

		// X 轴起始坐标计算
		if (l_width > 0)
		{
			if (align == 1)
				current_line.x = padding + indent + (target_content_w - current_line.width) / 2.0f;
			else if (align == 2)
				current_line.x = l_width - padding - current_line.width;
			else
				current_line.x = padding + indent;
		}
		else
		{
			if (align == 1)
				current_line.x = -current_line.width / 2.0f + padding + indent;
			else if (align == 2)
				current_line.x = -current_line.width + padding + indent;
			else
				current_line.x = padding + indent;
		}

		if (current_line.width > max_width)
			max_width = current_line.width;

		total_height += current_line.line_height + first_style.linespac;

		result.lines.push_back(std::move(current_line));
		current_line = composed_rich_string::line();
	};

	auto recalc_line_metrics = [&]()
	{
		current_line.width = current_line.max_ascender = current_line.max_descender = 0;
		for (auto& c : current_line.chars)
		{
			sdf::glyphs* cg = current_sdf_glyphs;
			if (c.char_style.font_id != 0)
			{
				auto fit = game_sdf_glyphs.find(c.char_style.font_id);
				if (fit != game_sdf_glyphs.end())
					cg = fit->second.get();
			}

			float f_size = pt_to_px(c.char_style.size);
			auto git = cg->glaph_map.find(c.unicode);
			float advance = (git != cg->glaph_map.end()) ? git->second.advance : 0.0f;
			current_line.width += (advance + c.char_style.advance_x +
				c.char_style.gap) * f_size;

			float a = cg->max_glyph_height * f_size - c.char_style.advance_y;
			float d = f_size - cg->max_glyph_height * f_size + c.char_style.advance_y;
			current_line.max_ascender = std::max(current_line.max_ascender, a);
			current_line.max_descender = std::max(current_line.max_descender, d);
		}
		current_line.line_height = current_line.max_ascender + current_line.max_descender;
	};
	
	int last_safe_break = -1; // 上一个安全换行点

	for (uint i = 0; i < rich_chars.size(); ++i)
	{
		const auto& rc = rich_chars[i];
		char br = brks[i];

		if (rc.unicode == '\r' || rc.unicode == '\n')
		{
			if (rc.unicode == '\r' && i + 1 < rich_chars.size() && rich_chars[i + 1].unicode == '\n')
				++i;

			if (current_line.line_height == 0)
			{
				float font_size_px = pt_to_px(rc.char_style.size);
				current_line.line_height = font_size_px;

				// 为空行赋予标准的升部和降部，防止基线塌陷导致连续换行重叠
				current_line.max_ascender = current_sdf_glyphs->max_glyph_height * font_size_px;
				current_line.max_descender = font_size_px - current_line.max_ascender;
			}

			commit_line(true); // 明确是段落末尾行
			last_safe_break = -1;
			continue;
		}

		sdf::glyphs* target_glyphs = current_sdf_glyphs;
		if (rc.char_style.font_id != 0)
		{
			auto fit = game_sdf_glyphs.find(rc.char_style.font_id);
			if (fit != game_sdf_glyphs.end())
				target_glyphs = fit->second.get();
		}

		uint unicode = rc.unicode;
		auto glyph_it = target_glyphs->glaph_map.find(unicode);
		if (glyph_it == target_glyphs->glaph_map.end())
			glyph_it = target_glyphs->glaph_map.find(absence_character_unicode);

		if (glyph_it != target_glyphs->glaph_map.end())
		{
			float font_size_px = pt_to_px(rc.char_style.size);
			float char_w = (glyph_it->second.advance + rc.char_style.advance_x + 
				rc.char_style.gap) * font_size_px;

			// 读取行的宽度限制
			float l_width = current_line.chars.empty() ?
				rc.char_style.line_width :
				current_line.chars.front().char_style.line_width;
			float padding = current_line.chars.empty() ?
				rc.char_style.padding :
				current_line.chars.front().char_style.padding;
			float indent = current_line.chars.empty() ?
				rc.char_style.indent :
				current_line.chars.front().char_style.indent;
			float target_content_w = l_width > 0 ? (l_width - indent - padding * 2) : -1;

			// 判断是否为空白字符（普通空格、制表符、全角空格）
			bool is_space = (rc.unicode == ' ' || rc.unicode == '\t' || rc.unicode == 0x3000);

			// 自动换行逻辑
			if (target_content_w > 0 && current_line.width + char_w > target_content_w && 
				!current_line.chars.empty() && !is_space)
			{
				if (last_safe_break != -1)
				{
					// 倒退回安全换行点，其余部分推入下一行
					std::vector<rich_char> carry(current_line.chars.begin() + last_safe_break, 
						current_line.chars.end());
					current_line.chars.erase(current_line.chars.begin() + last_safe_break, 
						current_line.chars.end());

					recalc_line_metrics();

					commit_line(false); // 非段落末尾
					current_line.chars = carry; // 继承
				}
				else
				{
					// 找不到安全点，强制直接折行
					commit_line(false);
				}

				last_safe_break = -1;
				recalc_line_metrics();
			}

			// 标记安全换行点 (不被 <nobr> 包含)
			if ((br == LINEBREAK_ALLOWBREAK || br == LINEBREAK_MUSTBREAK) && !rc.char_style.nobr)
			{
				last_safe_break = current_line.chars.size(); // 截断位置设为当前字符之后
			}

			current_line.chars.push_back(rc);
			current_line.chars.back().unicode = glyph_it->first;
			current_line.width += char_w;

			// yoffset 会动态改变文字向上或向下的升部与降部边界
			float ascender = target_glyphs->max_glyph_height * font_size_px - 
				rc.char_style.advance_y;
			float descender = font_size_px - target_glyphs->max_glyph_height * 
				font_size_px + rc.char_style.advance_y;

			if (ascender > current_line.max_ascender)
				current_line.max_ascender = ascender;

			if (descender > current_line.max_descender)
				current_line.max_descender = descender;

			current_line.line_height = current_line.max_ascender + current_line.max_descender;
		}
	}

	if (!current_line.chars.empty())
		commit_line(true);

	result.width = max_width;
	result.height = total_height;
	return result;
}

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

		// 计算全局偏移
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

		// 迭代绘制每一行
		for (const auto& line : str.lines)
		{
			float cursor_x = offset_x + line.x * xscale;

			// 基于该行最大的升部确定物理基线位置
			float baseline_y = cursor_y + line.max_ascender * yscale;

			for (size_t ci = 0; ci < line.chars.size(); ++ci)
			{
				const auto& rc = line.chars[ci];
				bool is_last_char = (ci + 1 == line.chars.size());

				sdf::glyphs* target_glyphs = current_sdf_glyphs;
				if (rc.char_style.font_id != 0)  // 动态加载 <font> 的贴图
				{
					auto fit = game_sdf_glyphs.find(rc.char_style.font_id);
					if (fit != game_sdf_glyphs.end())
						target_glyphs = fit->second.get();
				}

				// 由于粗细度和锐度是通过 Shader 常量控制的，如果遇到不同配置的字符，
				// 我们必须先渲染之前的顶点，更新常量，再开启新的批次。
				if (std::abs(rc.char_style.thickness - active_thickness) > 0.001f ||
					std::abs(rc.char_style.sharpness - active_sharpness) > 0.001f ||
					current_texture.texture != target_glyphs->texture)
				{
					atlas::end_draw();

					sdf::font_thickness = rc.char_style.thickness;
					sdf::font_sharpness = rc.char_style.sharpness;
					active_thickness = rc.char_style.thickness;
					active_sharpness = rc.char_style.sharpness;

					atlas::start_draw(target_glyphs->texture, D3DFMT_A8);
				}

				if (vbuff_c + 6 >= vb_count)
				{
					atlas::end_draw();
					atlas::start_draw(target_glyphs->texture, D3DFMT_A8);
				}

				float font_size_px = pt_to_px(rc.char_style.size);
				// 行内最后一个字符不追加字间距, 与排版时扣除的尾 gap 保持一致
				float step_gap = is_last_char ? 0.0f : rc.char_style.gap;

				// 占位符字符(<gap>/<padding>/<width>/<align>): 只推进光标, 不产生顶点
				if (rc.unicode == 0)
				{
					cursor_x += (rc.char_style.advance_x + step_gap) * xscale * font_size_px;
					continue;
				}

				auto glyph_it = target_glyphs->glaph_map.find(rc.unicode);
				sdf::glyphs::glyph& glyph = glyph_it->second;

				// 纹理坐标
				float u0 = glyph.atlas_bound.left / (float)target_glyphs->width;
				float v0 = glyph.atlas_bound.top / (float)target_glyphs->height;
				float u1 = glyph.atlas_bound.right / (float)target_glyphs->width;
				float v1 = glyph.atlas_bound.bottom / (float)target_glyphs->height;

				float draw_baseline_y = baseline_y + rc.char_style.advance_y * yscale;

				// 局部包围盒 (基线对齐)
				float left = glyph.plane_bound.left * font_size_px * xscale + cursor_x - 0.5f;
				float top = glyph.plane_bound.top * font_size_px * yscale + draw_baseline_y - 0.5f;
				float right = glyph.plane_bound.right * font_size_px * xscale + cursor_x - 0.5f;
				float bottom = glyph.plane_bound.bottom * font_size_px * yscale + draw_baseline_y - 0.5f;

				// 全局坐标 (加上绘制起点与用户设定的 tag offset)
				float x_lt = draw_x + left + rc.char_style.offset_x * xscale + 
					rc.char_style.italic * font_size_px * xscale;
				float y_lt = draw_y + top + rc.char_style.offset_y * yscale;
				float x_rt = draw_x + right + rc.char_style.offset_x * xscale + 
					rc.char_style.italic * font_size_px * xscale;
				float y_rt = y_lt;
				float x_rb = draw_x + right + rc.char_style.offset_x * xscale;
				float y_rb = draw_y + bottom + rc.char_style.offset_y * yscale;
				float x_lb = draw_x + left + rc.char_style.offset_x * xscale;
				float y_lb = y_rb;

				d3dcolor col = rc.char_style.color;

				// 压入三角形顶点 (颜色直接取自富文本样式)
				vertex::push_vertex_2d(x_lt, y_lt, u0, v0, col);
				vertex::push_vertex_2d(x_rt, y_rt, u1, v0, col);
				vertex::push_vertex_2d(x_rb, y_rb, u1, v1, col);

				vertex::push_vertex_2d(x_lt, y_lt, u0, v0, col);
				vertex::push_vertex_2d(x_rb, y_rb, u1, v1, col);
				vertex::push_vertex_2d(x_lb, y_lb, u0, v1, col);

				// 推进 X 光标
				cursor_x += (glyph.advance + rc.char_style.advance_x + step_gap) *
					xscale * font_size_px;
			}

			// 推进 Y 光标至下一行
			float linespac = line.chars.empty()
				? sdf::line_spacing
				: line.chars.front().char_style.linespac;

			cursor_y += (line.line_height + linespac) * yscale;
		}

		// 恢复全局着色器状态
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
	rich_char::style initial_style;  // 捕获当前全局绘制状态

	try
	{
		std::vector<rich_char> rich_chars = parse_rich_text(str);
		composed_rich_string composed = composing_rich_string(rich_chars);
		inner_draw_text_rich(composed, (float)x, (float)y, (float)xscale, (float)yscale);
	}
	catch (const std::exception& e)
	{
		initial_style.apply();  // 异常时也恢复全局状态, 避免污染后续绘制
		throw std::runtime_error("    in function sdf::draw_text_rich(double, double, "
			"std::string&, double, double):\r\n" + std::string(e.what()));
	}
	catch (...)
	{
		initial_style.apply();
		throw;
	}

	initial_style.apply();
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