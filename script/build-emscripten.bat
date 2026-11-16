@echo off

echo [1/2] Configuring CMake for Emscripten (krkrsdl3 + openartemis)...
cmake --preset="Emscripten Config" -DUSE_FFMPEG=OFF -DUSE_RENDER_VK=OFF
echo[

echo [2/2] Building Release version...
cmake --build --preset="Emscripten Release Build"
echo[
echo Outputs:
echo   out\emscripten\krkrsdl3\Release\index.html
echo   out\emscripten\openartemis\Release\index.html
