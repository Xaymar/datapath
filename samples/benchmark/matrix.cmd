@echo off
rem Runs the DataPath benchmark for every option combination, 5 runs each,
rem and writes everything to benchmark-results.txt in the current directory.
rem Usage: samples\benchmark\matrix.cmd [path\to\datapath-benchmark.exe] [runs]
setlocal
set "BENCH=%~dp0..\..\build\samples\Release\datapath-benchmark.exe"
if not "%~1"=="" set "BENCH=%~1"
set "RUNS=5"
if not "%~2"=="" set "RUNS=%~2"
set "OUT=benchmark-results.txt"
if not exist "%BENCH%" (
	echo Benchmark not found: %BENCH%
	exit /b 1
)
echo DataPath benchmark matrix, %RUNS% runs per size > "%OUT%"
for %%A in (
	"--priority 2"
	"--priority 0"
	"--no-mmcss"
	"--no-mmcss --priority 0"
	"--yield"
	"--alertable"
	"--spin 0"
	"--spin 0 --no-mmcss"
	"--spin 200"
	"--threads 2"
	"--clients 8"
	"--clients 8 --threads 4"
	"--load --threads 2"
	"--stress 0"
	"--stress 0 --no-mmcss"
	"--stress 0 --no-mmcss --priority 0"
	"--stress 0 --priority 3"
	"--stress 0 --spin 0"
	"--stress 0 --spin 0 --no-mmcss"
) do (
	echo.>> "%OUT%"
	echo ==== %%~A>> "%OUT%"
	echo Running: %%~A
	"%BENCH%" local --runs %RUNS% %%~A >> "%OUT%" 2>&1
)
echo Results written to %OUT%
