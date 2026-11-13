param([switch]$Test, [string]$Compiler)
$ErrorActionPreference = 'Stop'
$project = $PSScriptRoot
$build = Join-Path $project 'build-native'
New-Item -ItemType Directory -Force -Path $build | Out-Null
if (!$Compiler) {
    $available = Get-Command clang++,g++ -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($available) { $Compiler = $available.Source }
    else {
        $portable = Get-ChildItem (Join-Path $project 'build-toolchain\*\bin\clang++.exe') -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($portable) { $Compiler = $portable.FullName }
    }
}
if ($Compiler) {
    $common = @('-std=c++17', '-Wall', '-Wextra', '-Wpedantic', '-O2', '-static', "-I$project\include",
                "$project\src\sql.cpp", "$project\src\database.cpp")
    & $Compiler @common "$project\src\main.cpp" '-o' "$build\mini-rdbms.exe"
    if ($LASTEXITCODE -ne 0) { throw 'C++ CLI build failed.' }
    & $Compiler @common "$project\tests\tests.cpp" '-o' "$build\rdbms-tests.exe"
    if ($LASTEXITCODE -ne 0) { throw 'C++ test build failed.' }
} else {
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (!(Test-Path -LiteralPath $vswhere)) { throw 'Visual Studio C++ Build Tools is required. Alternatively use CMake with a C++17 compiler.' }
$installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$installation) { throw 'Install the Visual Studio Desktop development with C++ workload.' }
$devcmd = Join-Path $installation 'Common7\Tools\VsDevCmd.bat'
$batch = Join-Path $build 'compile.cmd'
$commands = @"
@echo off
call "$devcmd" -arch=x64 -host_arch=x64 >nul
if errorlevel 1 exit /b 1
pushd "$build"
cl /nologo /std:c++17 /EHsc /W4 /utf-8 /permissive- /O2 /I"$project\include" "$project\src\sql.cpp" "$project\src\database.cpp" "$project\src\main.cpp" /Fe:mini-rdbms.exe
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /EHsc /W4 /utf-8 /permissive- /O2 /I"$project\include" "$project\src\sql.cpp" "$project\src\database.cpp" "$project\tests\tests.cpp" /Fe:rdbms-tests.exe
if errorlevel 1 exit /b 1
popd
"@
[System.IO.File]::WriteAllText($batch, $commands, [System.Text.Encoding]::Default)
& $env:ComSpec /d /c $batch
if ($LASTEXITCODE -ne 0) { throw 'C++ build failed.' }
}
if ($Test) {
    & (Join-Path $build 'rdbms-tests.exe')
    if ($LASTEXITCODE -ne 0) { throw 'Unit/engine tests failed.' }
    & python (Join-Path $project 'tests\cli.py') (Join-Path $build 'mini-rdbms.exe')
    if ($LASTEXITCODE -ne 0) { throw 'CLI tests failed.' }
}
Write-Host "Built: $build\mini-rdbms.exe"
