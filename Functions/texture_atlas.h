#pragma once

#include "../main.h"

/// <summary>
/// 纹理图集结构体。
/// </summary>
struct texture_atlas
{
	/// <summary>
	/// 纹理图集图片结构体。
	/// </summary>
	struct images
	{
		/// <summary>
		/// 图片位置结构体
		/// </summary>
		struct sub_image
		{
			uint left = 0;
			uint top = 0;
			uint width = 0;  // width 和 height 都按照未进行旋转的情况进行存储
			uint height = 0;

			bool is_rotated = false;  // 是否在图集中旋转了 90 度存储

			// 虚拟纹理 ID，可在所有纹理图集中通过该 ID 找到该唯一图像。
			// 为了和 GM8 内部的 texture ID 区分，从 100,000 开始计数。
			uint texture_id = 0;

			sub_image(uint left, uint top, uint width, uint height, bool is_rotated,
				uint texture_id) : left(left), top(top), width(width), height(height),
				is_rotated(is_rotated), texture_id(texture_id) {}

			sub_image() = delete;
		};

		int orig_x = 0;  // 该偏移为相对 left 的
		int orig_y = 0;  // 该偏移为相对 top 的

		using image_ptr = std::unique_ptr<sub_image>;
		std::vector<image_ptr> frames;

		// draw_* 相关函数使用的 ID。
		// 为了和 GM8 内部的 Sprite 和 Background ID 区分，从 100,000 开始计数。
		uint image_id = 0;
		uint atlas_id = 0;  // 该图像所在的纹理图集 ID

		images(int orig_x, int orig_y, std::vector<image_ptr>&& frames, uint image_id, 
			uint atlas_id) : orig_x(orig_x), orig_y(orig_y), frames(frames), 
			image_id(image_id), atlas_id(atlas_id) {}

		images() = delete;
	};

	uint size = 0;							// 该纹理图集的边长
	std::vector<uchar> data;				// 纹理图集的 ARGB 数据

	using images_ptr = std::unique_ptr<images>;
	std::vector<images_ptr> images_list;	// 该纹理图集包含的图片
	rbp::MaxRectsBinPack bin;				// 用于计算打包位置的矩形打包器

	IDirect3DTexture8* texture = nullptr;	// 该图集对应的 GPU 纹理

	uint id = 0;							// 纹理图集 ID

	/// <summary>
	/// 创建一个新的纹理图集。
	/// </summary>
	/// <param name="size">纹理图集的大小，可以为 256，512，1024 或 2048。</param>
	/// <param name="id">位于 game_texture_atlas 的 ID。</param>
	texture_atlas(uint size, uint id);

	/// <summary>
	/// 默认构造函数，不允许使用。
	/// </summary>
	texture_atlas() = delete;

	/// <summary>
	/// 该结构体的析构函数。
	/// </summary>
	~texture_atlas();

	/// <summary>
	/// 在该纹理图集中添加一个图像文件。不是已导入到 GameMaker 里面的 Sprite 和 Background。
	/// </summary>
	/// <param name="image_file">图像文件。仅支持 gmspr 和 png 文件。</param>
	/// <returns>该图像是否成功添加至纹理图集中。</returns>
	uint add_image(std::string& image_file);

	/// <summary>
	/// 在该纹理图集中添加一个已导入到 GameMaker 里的 Sprite。
	/// 该 Sprite 在添加至纹理图集后，不会自动删除，需要使用 gm::sprite_delete 进行删除。
	/// </summary>
	/// <param name="id">Sprite ID。</param>
	/// <returns>该图像是否成功添加至纹理图集中。</returns>
	bool add_sprite(uint id);

	/// <summary>
	/// 在该纹理图集中添加一个已导入到 GameMaker 里的 Background。<para>
	/// 该 Background 在添加至纹理图集后，不会自动删除，需要使用 gm::background_delete 进行删除。
	/// </para></summary>
	/// <param name="id">Background ID。</param>
	/// <returns>该图像是否成功添加至纹理图集中。</returns>
	bool add_background(uint id);

	/// <summary>
	/// 将纹理图集数据上传至 GPU。
	/// </summary>
	/// <param name="del_memdata">是否删除内存中的纹理图集数据。
	/// 若为 true，使用此函数后纹理图集将变为只读状态。</param>
	/// <returns>是否成功。</returns>
	bool burn(bool del_memdata = true);

	/// <summary>
	/// 将纹理图集保存至一个 png 文件中，该纹理图集必须使用过 burn 方法。
	/// </summary>
	/// <param name="file_path">要保存的文件路径。</param>
	void save(path& file_path);

	/// <summary>
	/// 指示该纹理图集是否为只读模式
	/// </summary>
	inline bool read_only() const
	{
		return texture != nullptr && data.empty();
	}
};

// ============================================================================
// Export Functions
// ============================================================================

exp_real texture_atlas_create(gm_real size);
exp_real texture_atlas_delete(gm_real id);
exp_real texture_atlas_add_sprite(gm_real id, gm_string gmspr_file);
exp_real texture_atlas_burn(gm_real id, gm_real del_memdata);
exp_real texture_atlas_save(gm_real id, gm_string file_path);

exp_real texture_atlas_auto_add(gm_string file);
exp_real texture_atlas_auto_finish(gm_real dont_twice);

exp_real texture_atlas_count();
exp_real texture_atlas_exists(gm_real id);
exp_real texture_atlas_find_first();
exp_real texture_atlas_find_next(gm_real id);
exp_real texture_atlas_find_last();