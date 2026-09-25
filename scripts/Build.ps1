param(
    [Parameter(Mandatory=$true)][string]$DependencyRoot,
    [Parameter(Mandatory=$true)][ValidatePattern('^[0-9a-f]{40}$')][string]$Revision
)
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path $PSScriptRoot -Parent
$DependencyRoot = (Resolve-Path -LiteralPath $DependencyRoot).Path
$build = Join-Path $repoRoot '.ci-build'
$dist = Join-Path $repoRoot 'dist'
& cmake -S (Join-Path $repoRoot 'Opengl_Physx') -B $build -G Ninja -DCMAKE_BUILD_TYPE=Release `
    "-DPHYSX_ROOT=$DependencyRoot/PhysX-main" `
    "-DFLOW_ROOT=$DependencyRoot/PhysX-main/flow" `
    "-DGLFW_ROOT=$DependencyRoot/packages/glfw.3.4.0/build/native" `
    "-DGLM_ROOT=$DependencyRoot/packages/glm.1.0.3/build/native" -DAPP_PORTABLE_RUNTIME=ON
if ($LASTEXITCODE -ne 0) { throw 'CMake configuration failed.' }
& cmake --build $build --parallel 4
if ($LASTEXITCODE -ne 0) { throw 'Compilation failed.' }
$bin = Join-Path $build 'bin'
foreach ($name in @('Opengl_Physx.exe','PhysXGpu_64.dll','PhysX_64.dll','nvflow.dll','nvflowext.dll','glfw3.dll','vcruntime140.dll','msvcp140.dll','Assets')) {
    if (!(Test-Path -LiteralPath (Join-Path $bin $name))) { throw "Missing runtime file: $name" }
}
Copy-Item -LiteralPath (Join-Path $DependencyRoot 'licenses') -Destination $bin -Recurse -Force
Copy-Item -LiteralPath (Join-Path $repoRoot 'Opengl_Physx/ThirdParty/imgui/LICENSE.txt') -Destination (Join-Path $bin 'licenses/imgui-LICENSE.txt')
@{ revision=$Revision; builtAt=[DateTime]::UtcNow.ToString('o'); platform='windows-x64' } | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $bin 'build-info.json') -Encoding UTF8
New-Item -ItemType Directory -Force -Path $dist | Out-Null
$zip = Join-Path $dist 'runtime.zip'
Compress-Archive -Path (Join-Path $bin '*') -DestinationPath $zip -Force
$stream = [IO.File]::OpenRead($zip)
$sha = [Security.Cryptography.SHA256]::Create()
try { $digest = [BitConverter]::ToString($sha.ComputeHash($stream)).Replace('-','').ToLowerInvariant() }
finally { $stream.Dispose(); $sha.Dispose() }
$digest | Set-Content -LiteralPath (Join-Path $dist 'runtime.sha256') -Encoding ASCII
