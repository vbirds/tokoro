@echo off
setlocal

rem ==========================================================
rem 1. Set variables
rem ==========================================================
rem Source file name (change as needed)
set SRC=TestCoroutine.cpp

rem Executable file name (without path)
set EXE=CodeConvergeTest.exe

rem Output directory for coverage results (will be cleaned and recreated)
set OUTPUTFOLDER=code_coverage_gcc

rem Output HTML report name (relative to OUTPUTFOLDER)
set HTML_REPORT=coverage.html

rem ==========================================================
rem 2. Clean and create output directory
rem ==========================================================
if exist "%OUTPUTFOLDER%" (
    rd /s /q "%OUTPUTFOLDER%"
)
mkdir "%OUTPUTFOLDER%"

rem ==========================================================
rem 3. Compile with GCC coverage flags
rem    -fprofile-arcs: insert code to record branch/path execution
rem    -ftest-coverage: generate .gcno files for coverage analysis
rem    -O0 -g: disable optimization and include debug info
rem ==========================================================
echo [*] Compiling
g++ -std=c++20 -O0 -g -fprofile-arcs -ftest-coverage -fno-inline -I./include "%SRC%" -o "%OUTPUTFOLDER%\%EXE%"
if errorlevel 1 (
    echo [!] Compilation failed, exiting
    exit /b 1
)

rem ==========================================================
rem 4. Run the executable to generate .gcda data
rem    Runtime will produce .gcda files in the same folder as the executable
rem ==========================================================
echo [*] Running executable to generate coverage data
"%OUTPUTFOLDER%\%EXE%"
if errorlevel 1 (
    echo [!] Execution failed, .gcda files may not have been generated
    exit /b 1
)

rem ==========================================================
rem 5. Use gcovr to generate HTML report
rem    -r . sets the project root to the current directory
rem    --object-directory points to where .gcda/.gcno files are
rem    --html --html-details generates detailed HTML report
rem    --output specifies the output report file
rem ==========================================================
echo [*] Generating HTML coverage report
gcovr -r . --exclude-throw-branches --exclude-lines-by-pattern "^\s*assert\s*\(" --object-directory "%OUTPUTFOLDER%" --html --html-details --output "%OUTPUTFOLDER%\%HTML_REPORT%"
if errorlevel 1 (
    echo [!] Failed to generate report, check if gcovr is installed and in PATH
    exit /b 1
)

rem ==========================================================
rem 6. Optionally open the HTML report (uncomment the line below)
rem ==========================================================
rem start "" "%OUTPUTFOLDER%\%HTML_REPORT%"

echo.
echo [¡Ì] Coverage report generated successfully. See "%OUTPUTFOLDER%\%HTML_REPORT%"

start "" "code_coverage_gcc\coverage.html"

endlocal
exit /b 0
