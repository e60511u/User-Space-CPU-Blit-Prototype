pushd "%~dp0DXGI_version"
call "build.bat"
popd
call "%~dp0DXGI_version\bin\DesktopCapture.exe"