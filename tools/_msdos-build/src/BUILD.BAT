@echo off
set CL=
set LINK=
set MASM=
set COUNTRY=usa-ms
set BAKROOT=d:
set LIB=d:\src\tools\bld\lib
set INIT=d:\src\tools
set INCLUDE=d:\src\tools\bld\inc
set PATH=d:\src\tools
d:
cd d:\src
nmake /I /X d:\build.err > d:\build.log
echo NMAKE-DONE >> d:\build.log
cd d:\src
call cpy.bat d:\bin >> d:\build.log
copy boot\msboot.bin d:\bin >> d:\build.log
echo BUILD-FINISHED >> d:\build.log
