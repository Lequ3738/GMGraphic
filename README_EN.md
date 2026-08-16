# GMGraphic

English | [中文](README.md)

> [!Warning]
> You are viewing the documentation of the master branch, which may contain unreleased features. For a specific release, please select the corresponding tag.

> [!Warning]
> This extension only supports GameMaker 8.0. It does not support GameMaker 8.1 or any newer GameMaker version, nor GameMaker 7 or older versions.

## Features

This extension enhances the graphics drawing capabilities of GameMaker 8.0, including but not limited to:
- **Pixel shader** and **vertex shader** support based on Shader Model 1.4 in DirectX 8.
- When used together with the [GMDirectX9](https://github.com/Lequ3738/GMDirectX9) plugin, supports **pixel shaders** and **vertex shaders** up to SM3.0, as well as writing HLSL code.
- **Custom vertex formats** and **vertex buffers** identical to modern GameMaker (DX9 mode).
- More **render control**, **multi-texturing** and **extended primitives**.
- Image drawing based on **texture atlases**. Used by all major game engines, this effectively reduces DrawCall overhead and greatly improves drawing efficiency.
- **Multi-language font drawing** based on the [SDF](https://mapbox.github.io/tiny-sdf/) (Signed Distance Field) algorithm. It is widely used in Unity's TextMeshPro and the font rendering systems of other major game engines. Its characteristics:
  - Supports arbitrary glyph scaling and glyph thickness adjustment (accurate to fractions).
  - Fast font loading and low texture memory usage (1/4 of a regular texture).
  - More text effect options: outlined text, inner/outer glow, hollow fonts, soft-edged text shadows, etc.
- Drawing text with various effects using **rich text tags**.
- More efficient **GPU particles** (DX9 mode). Supports running a large number of particles (100,000+ in total across multiple systems) at the same time without any lag.

## Usage

Go to the `Release` page and download the latest release.<br>
For importing and using the extension, see the `GMGraphic.chm` documentation in the extension folder.

You may also compile the source code with Visual Studio 2022.<br>
Note that you must build with the x86 platform, because GameMaker 8.0 is a 32-bit program.

## Credits

Thanks to the following projects for inspiration and code references:
- [**Shader Extension**](https://web.archive.org/web/20191126155011/http://gmc.yoyogames.com/index.php?showtopic=492876) by LSnK
- [**Noisyfox Writing**](https://github.com/Noisyfox/FoxWriting) by Noisyfox
- [**GMParty**](https://github.com/Fanatrick/GMParty) by Fanatrick

Thanks to the following projects for technical support:
- [**GMAPI**](https://github.com/snakedeveloper/gmapi) by Snake
- [**RectangleBinPack**](https://github.com/juj/RectangleBinPack) by Jukka Jylänki
- [**LodePNG**](https://github.com/lvandeve/lodepng) by Lode Vandevenne
- [**UTF8-CPP**](https://github.com/nemtrif/utfcpp) by Nemanja Trifunovic
- [**libunibreak**](https://github.com/adah1972/libunibreak) by Wu Yongwei
- [**xxhash_cpp**](https://github.com/redspah/xxhash_cpp) by Yann Collet & Red Gavin
