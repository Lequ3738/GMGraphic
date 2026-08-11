// GMAPI setup

#include "main.h"
#include "texture_atlas.h"
#include "draw_text.h"
#include "string_make.h"
#include "shader.h"

gm::CGMAPI* gmapi;
std::string str_ret = "BABEBEEF"; // Used to return strings by macro.

bool WINAPI DllMain(HINSTANCE aModuleHandle, int aReason, int aReserved)
{
	switch (aReason)
	{
		case DLL_PROCESS_ATTACH:
		{
			ulong result = 0;
			gmapi = gm::CGMAPI::Create(&result);
			
			// Check the initialization
			if (result == gm::GMAPI_INITIALIZATION_FAILED)
			{
				complain("Unable to initialize GMAPI.");
				return FALSE;
			}

#ifdef _DEBUG
			gm::show_message("Debug Mode.");
#endif
		}
		break;

		case DLL_PROCESS_DETACH:
		{
			// 如下内容提前清理，确保不会发生全局变量析构顺序问题。
			game_texture_atlas.clear();
			game_sdf_glyphs.clear();

			shader_destroy((double)sdf_shader);
			shader_destroy((double)sdf_shader_premul);

			gmapi->Destroy();
		}
		break;
	}

	return true;
}

atlas::texture_info current_texture;

void atlas::start_draw(void* texture, D3DFORMAT format)
{
	try
	{
		if (texture == nullptr)
			throw std::runtime_error("The texture atlas is null.");

		vertex::begin(D3DPT_TRIANGLELIST, true);
		current_texture = { texture, format };
	}
	transpond_catch("atlas::start_draw(void*)")
}

void atlas::end_draw()
{
	try
	{
		if (current_texture.texture == nullptr)
			return;

		int prev_shader = -1;

		texture_clear_all();
		D3DCheck(d3d::set_texture(0, current_texture.texture), 0);
		D3DCheck(d3d::set_tex_stage_state(0, D3DTSS_ADDRESSU, D3DTADDRESS_CLAMP), 1);
		D3DCheck(d3d::set_tex_stage_state(0, D3DTSS_ADDRESSV, D3DTADDRESS_CLAMP), 2);

		if (current_texture.format == D3DFMT_A8)  // 字体纹理
		{
			if (sdf::use_shader)
			{
				prev_shader = (int)shader_current();
				shader_set(sdf::shader);

				// 保持旧 d3d_set_ps_const 的 ps_1.4 寄存器 [-1,1] clamp 行为,
				// 避免新 shader_set_uniform_f(不 clamp)改变 SDF 文字锐度。
				double sharpness = std::clamp(sdf::font_sharpness * sdf::game_font_size * 0.005,
					-1.0, 1.0);
				double thickness = std::clamp((-(sdf::font_thickness - 500.0f) +
					500.0f) / 1000.0f, 0.1f, 0.9f);

				// uniform 按后端分发: DX8 asm 写 c0(scale/thickness); DX9 HLSL 写
				// u_buffer/thickness(SDF 阈值) + u_gamma/边缘软度。
				if (d3d::version() == d3d::V9)
				{
					double scale = pt_to_px(sdf::game_font_size) / std::max(sdf::font_sharpness, 0.01f);
					double gamma = std::clamp(0.044 / scale, 0.005, 0.2);
					shader_set_uniform_f(sdf_shader_uniform_buffer, thickness, 0, 0, 0);
					shader_set_uniform_f(sdf_shader_uniform_gamma,  gamma,     0, 0, 0);
				}
				else
					shader_set_uniform_f(sdf_shader_uniform, sharpness, thickness, 0, 0);
			}
			else
			{
				// 颜色 = 直接使用顶点颜色 (忽略纹理中不存在的RGB)
				D3DCheck(d3d::set_tex_stage_state(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1), 5);
				D3DCheck(d3d::set_tex_stage_state(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE), 6);
			}
		}
		else if (current_texture.format != D3DFMT_A8R8G8B8)
			throw std::runtime_error("Unsupported texture format.");

		vertex::end();

		if (current_texture.format == D3DFMT_A8)
		{
			if (sdf::use_shader)
				shader_set(prev_shader);
			else
			{
				D3DCheck(d3d::set_tex_stage_state(0, D3DTSS_COLOROP, D3DTOP_MODULATE), 8);
				D3DCheck(d3d::set_tex_stage_state(0, D3DTSS_COLORARG1, D3DTA_TEXTURE), 9);
				D3DCheck(d3d::set_tex_stage_state(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE), 10);
			}
		}

		current_texture = { nullptr, D3DFMT_A8R8G8B8 };
	}
	transpond_catch("atlas::end_draw()")
}