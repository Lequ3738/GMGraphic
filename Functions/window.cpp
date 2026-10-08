#include "../main.h"
#include <cstdint>

// ============================================================================
// window_resize_buffer
//
// 让 runner 按当前房间活动视图的端口矩形重算屏幕区域: 窗口尺寸、region 尺寸
// (每帧屏幕空间重置与鼠标映射用)、绘制区尺寸与视口/屏幕矩阵 (Present 源矩形用)。
// 语义等同房间开始时 runner 自己跑的那一套, 但不重启房间 —— 因此调用方必须先把
// 新尺寸写进当前房间的活动视图 (view_wport / view_hport / view_wview / view_hview)。
//
// 该 runner 函数只读全局状态, 无参数且幂等 (推导尺寸与当前 region 相同则什么都不做),
// 重复调用安全; runner 版本不符时返回失败, 调用方应回落到重启房间的既有路径。
// ============================================================================

// GM8.0 runner 上的"应用屏幕区域"函数址 (镜像基址 0x400000)。
// 签名: push ebx / push esi / push edi / mov eax, [GM80_curRoomPtr] / mov eax, [eax] /
// cmp byte ptr [eax+40h], 0 —— 其中绝对地址操作数随镜像基址变化, 校验时留通配。
static uintptr_t const runner_apply_region_addr = 0x4BF774;

static bool runner_apply_region_ok()
{
	static int cached = -1;
	if (cached >= 0)
		return cached == 1;

	static unsigned char const sig[] = {
		0x53, 0x56, 0x57, 0xA1, 0x00, 0x00, 0x00, 0x00, 0x8B, 0x00, 0x80, 0x78, 0x40, 0x00
	};
	bool ok = false;
	__try
	{
		unsigned char const* p = (unsigned char const*)runner_apply_region_addr;
		ok = true;
		for (size_t i = 0; i < sizeof(sig) && ok; ++i)
			ok = (sig[i] == 0x00) || (p[i] == sig[i]);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		ok = false;
	}

	cached = ok ? 1 : 0;
	return ok;
}

exp_real window_resize_buffer()
{
	if (!runner_apply_region_ok())
		return gfalse;

	((void(__cdecl*)())runner_apply_region_addr)();
	return gtrue;
}
