#pragma once

#include "../main.h"

/// <summary>
/// 纹理图集图片结构体。
/// </summary>
struct atlas_images
{
	struct image
	{
		uint left = 0;
		uint top = 0;
		uint width = 0;
		uint height = 0;

		bool is_rotated = false;  // 是否在图集中旋转了 90 度存储

		// infomation
		uint atlas_id = 0;
	};

	int orig_x = 0;  // 该偏移为相对 left 的
	int orig_y = 0;  // 该偏移为相对 top 的

	std::vector<image> frames;
};

/// <summary>
/// 纹理图集结构体。
/// </summary>
struct texture_atlas
{
	enum atlas_kind { sprite, background };

	uint size = 0;							// 该纹理图集的边长
	std::vector<uchar> data;				// 纹理图集的 ARGB 数据

	atlas_kind kind = sprite;				// 该纹理图集的类型
	std::vector<atlas_images> images;		// 该纹理图集包含的图片
	rbp::MaxRectsBinPack bin;				// 用于计算打包位置的矩形打包器

	IDirect3DTexture8* texture = nullptr;	// 该图集对应的 GPU 纹理

	inline bool read_only() const			// 指示该纹理图集是否为只读模式
	{
		return texture != nullptr && data.empty();
	}
};

/// <summary>
/// 创建一个新的纹理图集。
/// </summary>
/// <param name="size">纹理图集的大小，可以为 256，512，1024 或 2048。</param>
/// <param name="kind">纹理图集的类型。</param>
/// <returns>纹理图集的id。</returns>
uint texture_atlas_create(uint size, texture_atlas::atlas_kind kind);

/// <summary>
/// 在指定的纹理图集中添加一个 Sprite。
/// </summary>
/// <param name="id">纹理图集的id。</param>
/// <param name="gmspr_file">GameMaker Sprite 文件。</param>
/// <returns>该 Sprite 是否成功添加至纹理图集中。</returns>
bool texture_atlas_add_sprite(uint id, std::string& gmspr_file);

/// <summary>
/// 将纹理图集数据上传至 GPU。
/// </summary>
/// <param name="id">纹理图集的id。</param>
/// <param name="del_memdata">是否删除内存中的纹理图集数据。
/// 若为 true，使用此函数后纹理图集将变为只读状态。</param>
/// <returns>是否成功。</returns>
bool texture_atlas_burn(uint id, bool del_memdata = true);