@echo off
rem QSimpleScada Demo launcher: set Qt/MinGW PATH then start the demo exe.
set "PATH=D:\Qt\Tools\mingw1310_64\bin;D:\Qt\6.12.0\mingw_64\bin;%PATH%"
start "" "%~dp0..\build-mingw\demo\release\QSimpleScadaDemo.exe"
