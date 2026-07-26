@echo off
cd /d "%~dp0xlln-src"
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars32.bat"
msbuild xlivelessness.sln /t:Build /p:Configuration=Release /p:Platform=x86 ^
  /p:WindowsTargetPlatformVersion=10.0.26100.0 /p:PlatformToolset=v143 /m /v:minimal
echo EXITCODE=%ERRORLEVEL%
