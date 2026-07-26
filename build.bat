@echo off
cd /d "%~dp0"
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars32.bat"
cl /nologo /LD /O2 /MT /EHsc /utf-8 dllmain.cpp /Fe:fable3_couchcoop.dll /link /SUBSYSTEM:WINDOWS /DEF:fable3_couchcoop.def user32.lib
echo EXITCODE=%ERRORLEVEL%
