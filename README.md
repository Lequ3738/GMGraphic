# GMGraphic

[English](README_EN.md) | 中文

> [!Warning]
您正在查看主分支 (master)的说明文件，可能包含未发布的功能。对于特定版本，请选择对应标签。

> [!Warning]
该扩展只支持 GameMaker 8.0，不支持 GameMaker 8.1 及以上的 GameMaker 版本，也不支持 GameMaker 7 及以下的版本。

## 扩展特色

该扩展增强了 GameMaker 8.0 的图形绘制功能。包括但不限于如下功能：
- 基于 DirectX 8 中 Shader Model 1.4 的**像素着色器**和**顶点着色器**功能。
- 若和 [GMDirectX9](https://github.com/Lequ3738/GMDirectX9) 插件一起使用，最高可支持 SM3.0 的**像素着色器**和**顶点着色器**功能，并支持编写 HLSL 代码。
- 与现代 GameMaker 相同的**自定义顶点格式**和**顶点缓冲区**功能（DX9 模式）。
- 更多的**渲染控制**，**多重纹理**和**扩展图元**功能。
- 基于**纹理图集**的图像绘制功能。应用于各大主流游戏引擎，能有效减少 DrawCall 开销，大幅提高绘制效率。
- 基于 [SDF](https://mapbox.github.io/tiny-sdf/)（Signed Distance Field, 有向距离场）算法的**多语言字体绘制**功能。目前广泛应用于 Unity 的 TextMeshPro 和其他各大游戏引擎的字体渲染系统。该算法的特点有：
  - 支持任意缩放字形和改变字形的粗细度(可精确到小数)。
  - 字体加载快且纹理占用显存少（是普通纹理的 1/4）。
  - 更多样的文字效果选项：文字描边、内/外发光、空心字体、柔和边缘的文字阴影等。
- 支持使用**富文本标签**来绘制各种效果的文字。
- 更高效的 **GPU 粒子**（DX9 模式）。支持同时运行大量（多系统合计超过 100000+）粒子而一点不卡顿。

## 设备丢失与恢复（DX9 模式，配合 GMDirectX9 2026-09-14 及以后版本）

窗口化的 GM8 游戏在**睡眠唤醒、锁屏、驱动超时恢复（TDR）**等场景会发生 D3D9 设备丢失，需要 Reset 设备。两插件协同处理：

- 纹理图集、SDF 字体纹理、gpart 类型表/图集/矩形表、全部静态顶点缓冲使用 **MANAGED 池**，普通设备 Reset 后内容自动存活。
- gpart 的粒子状态纹理（渲染目标）无法跨 Reset：设备 Reset 时**全部粒子会被清空**（与 GM8 原生行为一致：丢设备后画面重画，粒子重新发射）。
- 极罕见情况下（真 Reset 内部崩溃触发的**整设备重建**）：用户用 `shader_create` 创建的着色器与已删除内存数据的只读图集无法自动恢复（字节码/像素数据未保留），需要游戏重新创建/加载；SDF 内建着色器、gpart、顶点格式、图集（内存数据仍在者）均会自动重建。

## vertex_* 顶点管线的已知边界

- `vertex_submit` 在**没有自定义顶点着色器**时使用内置透传 VS（仅做 WVP 变换）：固定管线的**光照、雾、点大小**不会作用于这类绘制。需要光照的 3D 几何应配自定义 VS（与现代 GameMaker 的顶点缓冲语义一致）。

## 如何使用

转到 `Release` 页面，并下载最新的发布版本。<br>
扩展的导入和使用方法请参见扩展文件夹下的 `GMGraphic.chm` 文档。

也可使用 Visual Studio 2022 对源代码进行编译。<br>
注意要使用 x86 平台进行编译，因为 GameMaker 8.0 是 32 位程序。

## 感谢

感谢以下项目提供的灵感和代码参考：
- [**Shader Extension**](https://web.archive.org/web/20191126155011/http://gmc.yoyogames.com/index.php?showtopic=492876) by LSnK
- [**Noisyfox Writing**](https://github.com/Noisyfox/FoxWriting) by Noisyfox
- [**GMParty**](https://github.com/Fanatrick/GMParty) by Fanatrick

感谢以下项目提供的技术支持：
- [**GMAPI**](https://github.com/snakedeveloper/gmapi) by Snake
- [**RectangleBinPack**](https://github.com/juj/RectangleBinPack) by Jukka Jylänki
- [**LodePNG**](https://github.com/lvandeve/lodepng) by Lode Vandevenne
- [**UTF8-CPP**](https://github.com/nemtrif/utfcpp) by Nemanja Trifunovic
- [**libunibreak**](https://github.com/adah1972/libunibreak) by Wu Yongwei (吴咏炜)
- [**xxhash_cpp**](https://github.com/redspah/xxhash_cpp) by Yann Collet & Red Gavin