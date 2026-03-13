@echo off
setlocal enabledelayedexpansion

set "V_SDK=%VULKAN_SDK%"
set "V_SDK=%V_SDK:"=%"
set "GLSL_EXE=%V_SDK%\Bin\glslangValidator.exe"


set "OUT_DIR=..\cmake-build-debug-mingw\shaders"

echo ==========================================
echo [Shader Compiler] Starting...
echo ==========================================

if not exist "!GLSL_EXE!" (
    echo [ERROR] Cannot find glslangValidator.exe at: "!GLSL_EXE!"
    pause
    exit /b
)


if not exist "%OUT_DIR%" mkdir "%OUT_DIR%"


for %%i in (*.vert *.frag *.comp) do (
    echo [Compiling] %%i
    "!GLSL_EXE!" -V "%%i" -o "%OUT_DIR%\%%i.spv"

    if !errorlevel! equ 0 (
        echo [SUCCESS] %%i -^> .spv
    ) else (
        echo [FAILED]  %%i
    )
)

echo ==========================================
echo Done!
pause