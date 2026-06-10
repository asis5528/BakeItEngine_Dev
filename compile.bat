@echo off
rem Compiles every shader under Source/shaders to a .spv next to its source.
rem Run from the repo root (main() invokes this at startup).
rem Adding a new shader file requires NO edit here - it is picked up by the loop.
setlocal
set FAILED=0
for /r "Source\shaders" %%f in (*.vert *.frag *.comp) do (
    glslc.exe "%%f" -o "%%~dpnf.spv"
    if errorlevel 1 (
        echo SHADER COMPILE FAILED: %%f
        set FAILED=1
    )
)
endlocal & exit /b %FAILED%
