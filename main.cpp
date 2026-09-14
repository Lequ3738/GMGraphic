// GMAPI setup

#include "main.h"
#include "texture_atlas.h"
#include "draw_text.h"
#include "string_make.h"
#include "shader.h"

gm::CGMAPI* gmapi;
std::string str_ret = "BABEBEEF"; // Used to return strings by macro.
HINSTANCE g_dllInstance = nullptr;

bool WINAPI DllMain(HINSTANCE aModuleHandle, int aReason, int aReserved)
{
	switch (aReason)
	{
		case DLL_PROCESS_ATTACH:
		{
			ulong result = 0;
			g_dllInstance = aModuleHandle;   // 供 FindResource 加载内嵌 shader 源码
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
			// 先停掉 shader 异步编译 worker(避免线程引用已卸载模块)。
			shader_compile_shutdown();

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

// ============================================================================
// 批段快照(快照式合批, 2026-09-14)
// 图集批的观感 = 烘焙顶点 + 积累时刻的设备继承态。start_draw 把继承态定格进
// g_batch_snap, end_draw 按快照提交并还原 flush 时刻现场 —— 批与积累之后的任何
// 状态变更(插值/混合/变换/视口/着色器)彻底解耦, flush 时机只剩"顺序"一个语义
// (GMDirectX9 六槽设备钩子在引擎绘制提交动作前调用 atlas_flush_noexcept)。
// premul 是唯一不经过设备状态的继承项(A8 文字批的 shader 选择), 单独定格。
// ============================================================================
static d3d::DeviceStateSnap g_batch_snap;
static bool g_sdf_snap_use_shader = false;
static int  g_sdf_snap_shader = -1;

// 栈上快照的 COM 引用释放守卫(dssnap_free 幂等, 显式释放后再析构是空操作)。
struct SnapReleaser
{
	d3d::DeviceStateSnap* s;
	explicit SnapReleaser(d3d::DeviceStateSnap* p) : s(p) {}
	~SnapReleaser() { if (s) d3d::dssnap_free(*s); }
	SnapReleaser(const SnapReleaser&) = delete;
	SnapReleaser& operator=(const SnapReleaser&) = delete;
};

// GMDirectX9 设备钩子的 flush 入口(自动 force_draw_to_screen)。调用发生在引擎
// 绘制序列中途(vtable 钩子), 异常绝不能穿过钩子(会炸穿引擎汇编帧) —— 全部吞掉,
// 批留在缓冲等下一次机会; end_draw 状态原子化后, 中途触发对引擎绘制零扰动。
void atlas_flush_noexcept(void)
{
	try { atlas::end_draw(); }
	catch (...) {}
}

void atlas::start_draw(void* texture, D3DFORMAT format)
{
	try
	{
		if (texture == nullptr)
			throw std::runtime_error("The texture atlas is null.");

		vertex::begin(D3DPT_TRIANGLELIST, true);
		current_texture = { texture, format };

		// 积累时刻观感定格。仅 V9: V8 无自动 flush 钩子, 历史行为不快照。
		if (d3d::version() == d3d::V9)
		{
			d3d::dssnap_capture(g_batch_snap);
			g_sdf_snap_use_shader = sdf::use_shader;
			g_sdf_snap_shader = sdf::shader;
		}
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
		const bool atomic = (d3d::version() == d3d::V9);

		// 状态原子化: 自动 flush 发生在引擎绘制序列中途(引擎已设好自己的纹理/采样/
		// 变换/着色器), 提交完批必须原样还回。旧实现除 A8 的 TSS 分支外一概不还原,
		// 手工 flush 靠"引擎下个绘制自带状态设置"侥幸成立, 自动化后必须显式还原。
		d3d::DeviceStateSnap now = {};
		SnapReleaser now_guard{ &now };   // 异常路径也释放捕获期的 COM 引用(dssnap_free 幂等)
		if (atomic)
		{
			d3d::dssnap_capture(now);
			d3d::dssnap_apply(g_batch_snap);
		}

		texture_clear_all();
		D3DCheck(d3d::set_texture(0, current_texture.texture), 0);
		D3DCheck(d3d::set_tex_stage_state(0, D3DTSS_ADDRESSU, D3DTADDRESS_CLAMP), 1);
		D3DCheck(d3d::set_tex_stage_state(0, D3DTSS_ADDRESSV, D3DTADDRESS_CLAMP), 2);

		if (current_texture.format == D3DFMT_A8)  // 字体纹理
		{
			if (g_sdf_snap_use_shader)
			{
				prev_shader = (int)shader_current();
				shader_set(g_sdf_snap_shader);

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
			if (g_sdf_snap_use_shader)
				shader_set(prev_shader);
			else
			{
				D3DCheck(d3d::set_tex_stage_state(0, D3DTSS_COLOROP, D3DTOP_MODULATE), 8);
				D3DCheck(d3d::set_tex_stage_state(0, D3DTSS_COLORARG1, D3DTA_TEXTURE), 9);
				D3DCheck(d3d::set_tex_stage_state(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE), 10);
			}
		}

		// 还原 flush 时刻现场(纹理/采样/TSS/PS/VS/RS/变换/视口整组; A8 分支上面的
		// TSS 恢复被此处覆盖, 属冗余而非冲突; COM 引用由 now_guard 析构释放)。
		if (atomic)
			d3d::dssnap_apply(now);
		current_texture = { nullptr, D3DFMT_A8R8G8B8 };
	}
	transpond_catch("atlas::end_draw()")
}