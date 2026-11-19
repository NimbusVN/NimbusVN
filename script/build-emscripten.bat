@echo off

echo [1/2] Configuring CMake for Emscripten (krkrsdl3 + openartemis + onscripter)...
cmake --preset="Emscripten Config" -DUSE_FFMPEG=OFF -DUSE_RENDER_VK=OFF
echo[

echo [2/2] Building Release version...
cmake --build --preset="Emscripten Release Build"
echo[
echo Outputs:
echo   out\emscripten\krkrsdl3\Release\index.html      (krkrsdl3，预载 Res\)
echo   out\emscripten\openartemis\Release\index.html   (openartemis，页面选包/目录)
echo   out\emscripten\onscripter\Release\index.html    (onscripter，页面选目录/NScripter 资源)
echo[
echo 说明：
echo   * onscripter 在 Emscripten 下不链接 Freetype/PNG/JPEG/FFmpeg（CMake 已条件跳过），
echo     因此字体/图像/音视频能力取决于 engine/onscripter/src/media 下的 WASM 实现，
echo     当前页面壳可配置、可加载数据，但完整媒体链未在无 emsdk 环境下验证。
echo   * 页面需要带 COOP/COEP 头才能用多线程：
echo       python platform\emscripten\serve.py out\emscripten\onscripter\Release 8080
