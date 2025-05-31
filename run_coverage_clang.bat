@echo off
setlocal

rem set name of output files
set SRC=TestCoroutine.cpp
set EXE=CodeConvergeTest.exe
set PROFRAW=default.profraw
set PROFDATA=default.profdata
set OUTPUTFOLDER=code_coverage_test

rem clear old files
if exist %OUTPUTFOLDER% rd /s /q %OUTPUTFOLDER%
rem create output folder
mkdir %OUTPUTFOLDER%

rem use clang to compile the cpp with coverage enabled.
clang++ -std=c++20 -fprofile-instr-generate -fcoverage-mapping -o %OUTPUTFOLDER%\%EXE% %SRC%

rem set profraw file name
set LLVM_PROFILE_FILE=%OUTPUTFOLDER%\%PROFRAW%

rem run the code
%OUTPUTFOLDER%\%EXE%

rem use llvm-profdata to generate profdata
llvm-profdata merge -sparse %OUTPUTFOLDER%\%PROFRAW% -o %OUTPUTFOLDER%\%PROFDATA%

rem Use llvm-cov show the coverage
llvm-cov show %OUTPUTFOLDER%\%EXE% -instr-profile=%OUTPUTFOLDER%\%PROFDATA% > %OUTPUTFOLDER%\coverage.txt

rem Print coverage info on screen
echo --------- COVERAGE SUMMARY ---------
llvm-cov report %OUTPUTFOLDER%\%EXE% -instr-profile=%OUTPUTFOLDER%\%PROFDATA%
echo -------------------------------------

rem Opne coverage.txt
notepad %OUTPUTFOLDER%\coverage.txt

endlocal
