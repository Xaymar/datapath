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
	"--ideal-core off"
	"--ideal-core 1"
	"--priority 0"
	"--mmcss"
	"--yield"
	"--alertable"
	"--spin 0"
	"--spin 0 --ideal-core off"
	"--spin 200"
	"--threads 2"
	"--threads auto"
	"--threads auto --ideal-core off"
	"--clients 8"
	"--clients 8 --threads 4"
	"--clients 8 --threads auto"
	"--clients 8 --threads auto --ideal-core off"
	"--load --threads 2"
	"--stress 0"
	"--stress 0 --ideal-core off"
	"--stress 0 --priority 0"
	"--stress 0 --mmcss"
	"--stress 0 --spin 0"
	"--stress 0 --spin 0 --ideal-core off"
) do (
	echo.>> "%OUT%"
	echo ==== %%~A>> "%OUT%"
	echo Running: %%~A
	"%BENCH%" local --runs %RUNS% %%~A >> "%OUT%" 2>&1
)
rem The same core through the C API (datapath.h). The Rust core is compared
rem against these runs, since it is measured through the same interface.
for %%D in ("%BENCH%") do set "CAPI=%%~dpDdatapath-benchmark-c.exe"
if exist "%CAPI%" (
	for %%A in (
		"--priority 2"
		"--clients 8 --threads auto"
		"--load --threads 2"
		"--stress 0"
	) do (
		echo.>> "%OUT%"
		echo ==== c-api %%~A>> "%OUT%"
		echo Running: c-api %%~A
		"%CAPI%" local --runs %RUNS% %%~A >> "%OUT%" 2>&1
	)
)
rem Same measurements for libuv, if it was built (-DDATAPATH_BUILD_COMPARE=ON).
for %%D in ("%BENCH%") do set "LIBUV=%%~dpDlibuv-benchmark.exe"
if exist "%LIBUV%" (
	for %%A in (
		"--spin 0 --no-mmcss --priority 0"
		"--priority 2"
		"--spin 0"
		"--stress 0 --spin 0 --no-mmcss --priority 0"
		"--stress 0"
	) do (
		echo.>> "%OUT%"
		echo ==== libuv %%~A>> "%OUT%"
		echo Running: libuv %%~A
		"%LIBUV%" local --runs %RUNS% %%~A >> "%OUT%" 2>&1
	)
)
echo Results written to %OUT%
