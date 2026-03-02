#pragma once
#include "load_resources.h"
#include "MaxRectsBinPack.h"
#include "../main.h"

constexpr uint IMAGE_START_POSITION = 100000;
constexpr uint TEXTURE_START_POSITION = 100000;

struct copy_image_rect
{
	uint draw_x = 0;
	uint draw_y = 0;
	uint texture_width = 0;
	uint texture_height = 0;

	uint bleed_x = 0;
	uint bleed_y = 0;
	uint image_width = 0;
	uint image_height = 0;

	bool is_rotated = false;
};

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
			uint texture_left = 0;
			uint texture_top = 0;
			uint texture_width = 0;  // width 和 height 都按照未进行旋转的情况进行存储
			uint texture_height = 0; // 这里存储的是经过裁剪后的宽高

			int orig_x = 0;  // 该偏移为相对 texture_left 的，可以为负
			int orig_y = 0;  // 该偏移为相对 texture_top 的，可以为负

			bool is_rotated = false;  // 是否在图集中旋转了 90 度存储

			// 虚拟纹理 ID，可在所有纹理图集中通过该 ID 找到该唯一图像。
			// 为了和 GM8 内部的 texture ID 区分，从 100,000 开始计数。
			uint texture_id = 0;
			uint image_id = 0;  // 该子图像所属的 images ID

			sub_image(uint left, uint top, uint width, uint height, int ox, int oy, 
				bool is_rotated, uint texture_id, uint image_id) : 
				texture_left(left), texture_top(top), texture_width(width), 
				texture_height(height), orig_x(ox), orig_y(oy), is_rotated(is_rotated), 
				texture_id(texture_id), image_id(image_id) {}

			sub_image() = delete;
		};

		uint image_width = 0;  // 图像的虚拟宽，为实际使用的时候的宽
		uint image_height = 0; // 图像的虚拟高

		using sub_image_ptr = std::unique_ptr<sub_image>;
		std::vector<sub_image_ptr> frames;

		// draw_* 相关函数使用的 ID。
		// 为了和 GM8 内部的 Sprite 和 Background ID 区分，从 100,000 开始计数。
		uint image_id = 0;
		uint atlas_id = 0;  // 该图像所在的纹理图集 ID

		images(uint width, uint height, std::vector<sub_image_ptr> frames, 
			uint image_id, uint atlas_id) : image_width(width), image_height(height), 
			frames(std::move(frames)), image_id(image_id), atlas_id(atlas_id) {}

		images() = delete;
	};

	uint size = 0;							// 该纹理图集的边长
	std::vector<uchar> data;				// 纹理图集的 ARGB 数据

	using images_ptr = std::unique_ptr<images>;
	std::vector<images_ptr> images_list;	// 该纹理图集包含的图片
	rbp::MaxRectsBinPack bin;				// 用于计算打包位置的矩形打包器

	IDirect3DTexture8* texture = nullptr;	// 该图集对应的 GPU 纹理

	uint id = 0;							// 纹理图集 ID

	static bool texture_amplification;		// 是否按照“边缘像素重复”的方式添加纹理到纹理图集中

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
	/// <returns>若成功，返回 ID，否则返回 -1。</returns>
	static gm::sprite decode_image(const std::string& image_file);

	/// <summary>
	/// 在该纹理图集中添加一个已导入到 GameMaker 里的 Sprite。
	/// 该 Sprite 在添加至纹理图集后，不会自动删除，需要使用 gm::sprite_delete 进行删除。
	/// </summary>
	/// <param name="id">Sprite ID。</param>
	/// <returns>若成功，返回 ID，否则返回 -1。</returns>
	static gm::sprite decode_sprite(uint id);

	/// <summary>
	/// 在该纹理图集中添加一个已导入到 GameMaker 里的 Background。<para>
	/// 该 Background 在添加至纹理图集后，不会自动删除，需要使用 gm::background_delete 进行删除。
	/// </para></summary>
	/// <param name="id">Background ID。</param>
	/// <returns>若成功，返回 ID，否则返回 -1。</returns>
	static gm::sprite decode_background(uint id);
	
	/// <summary>
	/// 
	/// </summary>
	/// <param name="spr"></param>
	/// <returns></returns>
	int add_image(gm::sprite& spr);

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
	void save(path& file_path) const;

	/// <summary>
	/// 指示该纹理图集是否为只读模式
	/// </summary>
	inline bool read_only() const
	{
		return texture != nullptr && data.empty();
	}

private:
	void add_image_to_memory(std::vector<uchar>& image_data, copy_image_rect& rect);
};

extern std::unordered_map<uint, std::unique_ptr<texture_atlas>> game_texture_atlas;
extern std::unordered_map<uint, texture_atlas::images*> game_images;
extern std::unordered_map<uint, texture_atlas::images::sub_image*> game_textures;

// ============================================================================
// Export Functions
// ============================================================================
exp_real texture_atlas_auto_add_file(gm_string file);
exp_real texture_atlas_auto_add_sprite(gm_real spr);
exp_real texture_atlas_auto_add_background(gm_real back);
exp_real texture_atlas_auto_finish(gm_real dont_twice);

exp_real texture_atlas_create(gm_real size);
exp_real texture_atlas_delete(gm_real id);
exp_real texture_atlas_add_file(gm_real id, gm_string file);
exp_real texture_atlas_add_sprite(gm_real id, gm_real spr);
exp_real texture_atlas_add_background(gm_real id, gm_real back);
exp_real texture_atlas_burn(gm_real id, gm_real del_memdata);
exp_real texture_atlas_save(gm_real id, gm_string file_path);

exp_real texture_atlas_count();
exp_real texture_atlas_exists(gm_real id);
exp_real texture_atlas_find_first();
exp_real texture_atlas_find_next(gm_real id);
exp_real texture_atlas_find_last();

exp_real texture_atlas_get_image_count(gm_real atlas_id);
exp_real image_get_texture_atlas(gm_real image_id);
exp_real texture_atlas_get_image(gm_real atlas_id, gm_real pos);
exp_real texture_get_image(gm_real texture_id);
exp_real atlas_sprite_get_texture(gm_real spr, gm_real subimg);
exp_real atlas_background_get_texture(gm_real back);
exp_real atlas_sprite_get_width(gm_real id);
exp_real atlas_sprite_get_height(gm_real id);
exp_real atlas_background_get_width(gm_real id);
exp_real atlas_background_get_height(gm_real id);
exp_real atlas_texture_get_width(gm_real id);
exp_real atlas_texture_get_height(gm_real id);
exp_real texture_get_atlas_x(gm_real id);
exp_real texture_get_atlas_y(gm_real id);

exp_real texture_atlas_set_crop(gm_real crop);
exp_real texture_atlas_get_crop();
exp_real texture_atlas_set_amplificate(gm_real ampl);
exp_real texture_atlas_get_amplificate();