#pragma once
#include "../main.h"

/// <summary>
/// 按照指定的分隔符分割字符串。
/// </summary>
std::vector<std::string> string_token(std::string& str, std::string&& sep);

/// <summary>
/// 将字符串分割成一列 utf-8 字符
/// </summary>
std::vector<std::string> string_token_utf8(std::string& str);

/// <summary>
/// 将 pt 转为 px
/// </summary>
float pt_to_px(float pt);

/// <summary>
/// 按行分割字符串
/// </summary>
std::vector<std::string> split_lines(std::string& str);