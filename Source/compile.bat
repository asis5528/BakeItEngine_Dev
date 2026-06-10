@echo off
rem Same as the repo-root compile.bat but for running from inside Source/.
rem The root script is the canonical one (main() runs it at startup).
setlocal
set FAILED=0
for /r "shaders" %%f in (*.vert *.frag *.comp) do (
    glslc.exe "%%f" -o "%%~dpnf.spv"
    if errorlevel 1 (
        echo SHADER COMPILE FAILED: %%f
        set FAILED=1
    )
)
endlocal & exit /b %FAILED%
