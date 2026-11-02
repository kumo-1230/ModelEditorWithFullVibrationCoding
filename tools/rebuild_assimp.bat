@echo off
rem ---------------------------------------------------------------------------
rem (Maintainers only) Rebuild third_party\assimp from source.
rem Normal builds do NOT need this: the prebuilt static library is already in
rem third_party\assimp\lib. Use it only when upgrading Assimp.
rem Requires: git, CMake 3.20+, Visual Studio 2022.
rem ---------------------------------------------------------------------------
setlocal
set ROOT=%~dp0..
set WORK=%ROOT%\_assimp_work
set TAG=v6.0.2
if not exist "%WORK%\src" git clone --depth 1 --branch %TAG% https://github.com/assimp/assimp.git "%WORK%\src" || goto :error
cmake -S "%WORK%\src" -B "%WORK%\b" -G "Visual Studio 17 2022" -A x64 ^
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_POLICY_DEFAULT_CMP0091=NEW -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded -DUSE_STATIC_CRT=ON ^
  -DBUILD_SHARED_LIBS=OFF -DASSIMP_BUILD_TESTS=OFF -DASSIMP_BUILD_ASSIMP_TOOLS=OFF -DASSIMP_BUILD_SAMPLES=OFF -DASSIMP_INSTALL=OFF ^
  -DASSIMP_WARNINGS_AS_ERRORS=OFF -DASSIMP_BUILD_ZLIB=ON -DASSIMP_INJECT_DEBUG_POSTFIX=OFF -DASSIMP_BUILD_DRACO=OFF -DASSIMP_NO_EXPORT=OFF ^
  -DASSIMP_BUILD_ALL_IMPORTERS_BY_DEFAULT=ON -DASSIMP_BUILD_IFC_IMPORTER=OFF -DASSIMP_BUILD_USD_IMPORTER=OFF ^
  -DASSIMP_BUILD_ALL_EXPORTERS_BY_DEFAULT=OFF ^
  -DASSIMP_BUILD_FBX_EXPORTER=ON -DASSIMP_BUILD_GLTF_EXPORTER=ON -DASSIMP_BUILD_OBJ_EXPORTER=ON -DASSIMP_BUILD_COLLADA_EXPORTER=ON ^
  -DASSIMP_BUILD_PLY_EXPORTER=ON -DASSIMP_BUILD_STL_EXPORTER=ON ^
  -DASSIMP_BUILD_3MF_EXPORTER=ON -DASSIMP_BUILD_X_EXPORTER=ON -DASSIMP_BUILD_3DS_EXPORTER=ON || goto :error
cmake --build "%WORK%\b" --config Release --target assimp -- /m || goto :error
xcopy /y /e /i "%WORK%\src\include\assimp" "%ROOT%\third_party\assimp\include\assimp" >nul
copy /y "%WORK%\b\include\assimp\config.h" "%ROOT%\third_party\assimp\include\assimp\" >nul
copy /y "%WORK%\b\include\assimp\revision.h" "%ROOT%\third_party\assimp\include\assimp\" >nul
copy /y "%WORK%\b\lib\Release\assimp-vc143-mt.lib" "%ROOT%\third_party\assimp\lib\assimp.lib" >nul
for /r "%WORK%\b\contrib\zlib" %%f in (zlibstatic.lib) do if exist "%%f" copy /y "%%f" "%ROOT%\third_party\assimp\lib\zlibstatic.lib" >nul
echo Done. You can delete %WORK%.
exit /b 0
:error
echo Failed.
exit /b 1
