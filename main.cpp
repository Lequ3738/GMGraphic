// GMAPI setup

#include "main.h"
#include "texture_atlas.h"
#include "draw_text.h"
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

			d3d_ps_destroy((double)ps_sdf_comp);
			d3d_ps_destroy((double)ps_sdf_premul_comp);
			
			gmapi->Destroy();
		}
		break;
	}

	return true;
}

atlas::texture_info current_texture;

void atlas::start_draw(IDirect3DTexture8* texture, D3DFORMAT format)
{
	try
	{
		if (texture == nullptr)
			throw std::runtime_error("The texture atlas is null.");

		vertex::begin(D3DPT_TRIANGLELIST, true);
		current_texture = { texture, format };
	}
	transpond_catch("atlas::start_draw(IDirect3DTexture8*)")
}

void atlas::end_draw()
{
	try
	{
		if (current_texture.texture == nullptr)
			return;

		IDirect3DDevice8* device = gmapi->GetDirect3DDevice();
		dword prev_pixel_shader = 0;

		d3d_set_tex_all(-1);
		D3DCheck(device->SetTexture(0, current_texture.texture), 0);
		D3DCheck(device->SetTextureStageState(0, D3DTSS_ADDRESSU, D3DTADDRESS_CLAMP), 1);
		D3DCheck(device->SetTextureStageState(0, D3DTSS_ADDRESSV, D3DTADDRESS_CLAMP), 2);

		if (current_texture.format == D3DFMT_A8)  // 字体纹理
		{
			if (sdf::use_shader)
			{
				D3DCheck(device->GetPixelShader(&prev_pixel_shader), 3);
				D3DCheck(device->SetPixelShader(sdf::shader), 4);

				double deviation = std::abs(sdf::font_thickness - 0.5f);
				double sharpness = sdf::font_sharpness * 0.007 *
					sdf::game_font_size * (1.0 + deviation * 1.5);

				d3d_set_ps_const(0, sharpness, sdf::font_thickness, 0, 0);
			}
			else
			{
				// 颜色 = 直接使用顶点颜色 (忽略纹理中不存在的RGB)
				D3DCheck(device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1), 5);
				D3DCheck(device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE), 6);
			}
		}
		else if (current_texture.format != D3DFMT_A8R8G8B8)
			throw std::runtime_error("Unsupported texture format.");

		vertex::end();
		
		if (current_texture.format == D3DFMT_A8)
		{
			if (sdf::use_shader)
				D3DCheck(device->SetPixelShader(prev_pixel_shader), 7);
			else
			{
				D3DCheck(device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE), 8);
				D3DCheck(device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE), 9);
				D3DCheck(device->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE), 10);
			}
		}
		
		current_texture = { nullptr, D3DFMT_A8R8G8B8 };
	}
	transpond_catch("atlas::end_draw()")
}