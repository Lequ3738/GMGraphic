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
			gmapi->Destroy();

			// 如下内容提前清理，确保不会发生全局变量析构顺序问题。
			game_texture_atlas.clear();  
			game_sdf_glyphs.clear();
		}
		break;
	}

	return true;
}

IDirect3DTexture8* current_texture = nullptr;

void atlas::start_draw(IDirect3DTexture8* texture)
{
	try
	{
		if (texture == nullptr)
			throw std::runtime_error("The texture atlas is null.");

		vertex::begin(D3DPT_TRIANGLELIST, true);
		current_texture = texture;
	}
	transpond_catch("atlas::start_draw(IDirect3DTexture8*)")
}

void atlas::end_draw()
{
	try
	{
		if (current_texture == nullptr)
			return;

		IDirect3DDevice8* device = gmapi->GetDirect3DDevice();

		D3DSURFACE_DESC desc{};
		D3DCheck(current_texture->GetLevelDesc(0, &desc), 0);

		d3d_set_tex_all(-1);
		D3DCheck(device->SetTexture(0, current_texture), 0);
		D3DCheck(device->SetTextureStageState(0, D3DTSS_ADDRESSU, D3DTADDRESS_CLAMP), 1);
		D3DCheck(device->SetTextureStageState(0, D3DTSS_ADDRESSV, D3DTADDRESS_CLAMP), 2);

		if (desc.Format == D3DFMT_A8R8G8B8)
		{
			// 颜色 = 纹理RGB * 顶点RGB
			device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
			device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
			device->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
		}
		else if (desc.Format == D3DFMT_A8)
		{
			// 颜色 = 直接使用顶点颜色 (忽略纹理中不存在的RGB)
			device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
			device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
		}
		else
			throw std::runtime_error("Unsupported texture format.");

		vertex::end();
		current_texture = nullptr;
	}
	transpond_catch("atlas::end_draw()")
}