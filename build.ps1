param(
    [switch]$Clean,
    [ValidateSet("Debug", "Release")]
    [string]$Config = "Debug"
)

$ErrorActionPreference = "Stop"

$root = $PSScriptRoot
$out  = Join-Path $root "build\$Config"
$gen  = Join-Path $root "build\obj-$Config"

if ($Clean) {
    foreach ($d in @($out, $gen)) {
        if (Test-Path $d) { Remove-Item -Recurse -Force $d }
    }
}
New-Item -ItemType Directory -Force -Path $out | Out-Null
New-Item -ItemType Directory -Force -Path $gen | Out-Null

$vc = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat"
if (-not (Test-Path $vc)) { throw "VsDevCmd.bat not found at $vc" }

$src = @(
    "$root\src\core\price_level.cpp",
    "$root\src\core\order_book.cpp",
    "$root\src\core\fast_order_book.cpp",
    "$root\src\core\matching_engine.cpp",
    "$root\tests\test_main.cpp",
    "$root\tests\test_order.cpp",
    "$root\tests\test_price_level.cpp",
    "$root\tests\test_order_book.cpp",
    "$root\tests\test_order_arena.cpp",
    "$root\tests\test_fast_order_book.cpp",
    "$root\tests\test_order_id_map.cpp",
    "$root\tests\test_book_backend.cpp",
    "$root\tests\test_match_types.cpp"
)

$flags = @(
    "/nologo",
    "/std:c++20",
    "/EHsc",
    "/W4",
    "/permissive-",
    "/Zc:__cplusplus",
    "/D_CRT_SECURE_NO_WARNINGS",
    "/I`"$root\src`""
)

if ($Config -eq "Release") {
    $flags += "/O2", "/GL"
} else {
    $flags += "/Od", "/Zi", "/DDEBUG"
}

# Each translation unit is compiled to an .obj in $gen (incremental-friendly).
$objs = @()
foreach ($f in $src) {
    $objName = [System.IO.Path]::GetFileNameWithoutExtension($f) + ".obj"
    $obj = Join-Path $gen $objName
    $compileArgs = $flags + @("/c", "/Fo`"$obj`"", "`"$f`"")
    $cmd = "`"$vc`" -arch=x64 >nul 2>&1 && cl " + ($compileArgs -join " ")
    cmd /c $cmd
    if ($LASTEXITCODE -ne 0) { throw "Compile failed: $f" }
    $objs += $obj
}

# Link all objects into the test executable.
$exe = Join-Path $out "lob_tests.exe"
$linkArgs = $objs + @("/Fe`"$exe`"", "/link", "/INCREMENTAL:NO")
$cmd = "`"$vc`" -arch=x64 >nul 2>&1 && cl " + ($linkArgs -join " ")
cmd /c $cmd
if ($LASTEXITCODE -ne 0) { throw "Link failed with exit code $LASTEXITCODE" }

# Build the benchmark executable (single TU with its own main()).
$benchObj = Join-Path $gen "bench_order_book.obj"
$compileArgs = $flags + @("/c", "/Fo`"$benchObj`"", "`"$root\tests\bench_order_book.cpp`"")
$cmd = "`"$vc`" -arch=x64 >nul 2>&1 && cl " + ($compileArgs -join " ")
cmd /c $cmd
if ($LASTEXITCODE -ne 0) { throw "Compile failed: tests\bench_order_book.cpp" }

$benchExe = Join-Path $out "lob_bench.exe"
$benchLinkArgs = @($benchObj, "/Fe`"$benchExe`"", "/link", "/INCREMENTAL:NO")
$cmd = "`"$vc`" -arch=x64 >nul 2>&1 && cl " + ($benchLinkArgs -join " ")
cmd /c $cmd
if ($LASTEXITCODE -ne 0) { throw "Link failed with exit code $LASTEXITCODE" }

Write-Host "OK: $exe"
Write-Host "OK: $benchExe"