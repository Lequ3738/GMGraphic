# GMGraphic

该扩展增强了 GameMaker 8.0 的图形绘制功能。包括基于 DirectX 8 中 Shader Model 1.4 的 Shader 功能；基于纹理图集的图像绘制功能，能极大提高 GM8 的绘制效率；基于 [SDF](https://mapbox.github.io/tiny-sdf/) 的多语言字体绘制功能。

## 目前的进度

✅ 像素着色器和顶点着色器功能<br>
✅ 更多的渲染控制，多重纹理和扩展图元<br>
✅ 纹理图集的创建与纹理的优化(比如纹理的自动剪裁等)<br>
❌ 纹理图集的保存与读取<br>
✅ 基于自动提交的类原生绘制 API<br>
❌ 更高效的字体绘制功能，并支持简单的标记(类似 BBCode)<br>

## 如何编译

使用 Visual Studio 2022 进行编译。<br>
注意要使用 x86 平台进行编译，因为 GameMaker 8.0 是 32 位程序。

## 感谢

感谢以下项目提供的灵感和代码参考：
- [**Shader Extension**](https://web.archive.org/web/20191126155011/http://gmc.yoyogames.com/index.php?showtopic=492876) by LSnK

感谢以下项目提供的技术支持：
- [**GMAPI**](https://github.com/snakedeveloper/gmapi) by Snake
- [**RectangleBinPack**](https://github.com/juj/RectangleBinPack) by Jukka Jylänki