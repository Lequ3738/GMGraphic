#pragma once
#include "../main.h"

/// 按照指定的分隔符分割字符串。
std::vector<std::string> string_token(std::string& str, std::string&& sep);

/// 将字符串分割成一列 utf-8 字符
std::vector<std::string> string_token_utf8(std::string& str);

/// 将 pt 转为 px
float pt_to_px(float pt);

/// 按行分割字符串
std::vector<std::string> split_lines(std::string& str);