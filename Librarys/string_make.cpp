#include "utf8.h"
#include "string_make.h"

std::vector<std::string> string_token(std::string& str, std::string&& sep)
{
	try
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
	transpond_catch("string_token(std::string&, std::string&&)")
}

std::vector<std::string> string_token_utf8(std::string& str)
{
	try
	{
		std::vector<std::string> tokens;
		auto end = str.end();
		auto cur_it = str.begin();
		auto prev_it = str.begin();

		while (cur_it != end)
		{
			utf8::next(cur_it, end);  // 获取下一个字符

			std::string character(prev_it, cur_it);
			if (!character.empty())
				tokens.push_back(std::move(character));

			prev_it = cur_it;
		}

		return tokens;
	}
	transpond_catch("string_token_utf8(std::string&)")
}

float pt_to_px(float pt) { return pt * 96.0f / 72.0f; }

std::vector<std::string> split_lines(std::string& str)
{
	try
	{
		std::vector<std::string> line_str;
		uint start = 0, i = 0;

		while (i < str.length())
		{
			if (str[i] == '\r')
			{
				line_str.emplace_back(str.substr(start, i - start));
				if (i + 1 < str.length() && str[i + 1] == '\n') i += 2;
				else ++i;
				start = i;
			}
			else if (str[i] == '\n')
			{
				line_str.emplace_back(str.substr(start, i - start));
				++i;
				start = i;
			}
			else
				++i;
		}

		if (start < str.length())
			line_str.emplace_back(str.substr(start, str.length() - start));

		return line_str;
	}
	transpond_catch("split_lines(std::string&)")
}
