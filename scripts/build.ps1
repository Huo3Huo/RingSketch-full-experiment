param([string]$Compiler="C:/msys64/ucrt64/bin/clang++.exe")
$ErrorActionPreference="Stop"
$projectRoot=Split-Path -Parent $PSScriptRoot
$buildRoot=Join-Path $projectRoot "build"
New-Item -ItemType Directory -Force -Path $buildRoot | Out-Null
$env:PATH="$(Split-Path -Parent $Compiler);$env:PATH"
$env:TMP=$buildRoot
$env:TEMP=$buildRoot
foreach($target in @(@("core","tests/core.cpp"),@("experiment","src/experiment.cpp"),@("benchmark","src/benchmark.cpp"))) {
    $object=Join-Path $buildRoot "$($target[0]).o"
    & $Compiler -std=c++14 -O2 -g -mavx2 -D_GLIBCXX_ASSERTIONS "-I$projectRoot/src" -c (Join-Path $projectRoot $target[1]) -o $object
    if($LASTEXITCODE -ne 0){throw "Compilation failed: $($target[0])"}
    & $Compiler $object -o (Join-Path $buildRoot "$($target[0]).exe")
    if($LASTEXITCODE -ne 0){throw "Link failed: $($target[0])"}
}
& (Join-Path $buildRoot "core.exe")
if($LASTEXITCODE -ne 0){throw "Core verification failed"}
$sources=@{}
foreach($folder in @("src","tests")) {
    Get-ChildItem (Join-Path $projectRoot $folder) -Recurse -File | ForEach-Object {
        $relative=$_.FullName.Substring($projectRoot.Length+1).Replace('\','/')
        $sources[$relative]=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    }
}
$executables=@{}
foreach($name in @("core","experiment","benchmark")) {
    $executables[$name]=(Get-FileHash -LiteralPath (Join-Path $buildRoot "$name.exe") -Algorithm SHA256).Hash.ToLowerInvariant()
}
@{sources=$sources;executables=$executables;compiler=(& $Compiler --version | Out-String);flags=@('-std=c++14','-O2','-g','-mavx2','-D_GLIBCXX_ASSERTIONS')} | ConvertTo-Json -Depth 6 | Set-Content -Encoding UTF8 (Join-Path $buildRoot 'build_manifest.json')
Write-Output "BUILD AND CORE VERIFICATION PASSED"
