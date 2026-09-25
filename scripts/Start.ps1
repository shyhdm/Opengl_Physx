param([switch]$ResolveOnly)
$ErrorActionPreference = 'Stop'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
$repoRoot = Split-Path $PSScriptRoot -Parent
$repository = 'shyhdm/Opengl_Physx'
$stage = $null
try {
    $revision = (Get-Content -LiteralPath (Join-Path $repoRoot 'source-version.txt') -Raw).Trim()
    if ($revision -notmatch '^[0-9a-f]{40}$') {
        if (!(Test-Path -LiteralPath (Join-Path $repoRoot '.git')) -or !(Get-Command git -ErrorAction SilentlyContinue)) {
            throw 'Cannot identify this source version. Download the repository ZIP again from GitHub.'
        }
        $revision = (& git -C $repoRoot rev-parse HEAD).Trim()
        if ($LASTEXITCODE -ne 0 -or $revision -notmatch '^[0-9a-f]{40}$') { throw 'Cannot read Git revision.' }
        $changes = & git -C $repoRoot status --porcelain --untracked-files=all -- Opengl_Physx/src Opengl_Physx/Assets Opengl_Physx/ThirdParty Opengl_Physx/CMakeLists.txt scripts .github
        if ($LASTEXITCODE -ne 0 -or $changes) { throw 'Source has local changes. Build locally, or commit and push before using this launcher.' }
    }
    if ($ResolveOnly) { Write-Output $revision; exit 0 }
    $runtimeRoot = Join-Path $repoRoot 'Runtime'
    $versionDir = Join-Path $runtimeRoot $revision
    $exe = Join-Path $versionDir 'Opengl_Physx.exe'
    $manifestFile = Join-Path $versionDir 'build-info.json'
    $ready = $false
    if ((Test-Path -LiteralPath $exe) -and (Test-Path -LiteralPath $manifestFile)) {
        $ready = ((Get-Content -LiteralPath $manifestFile -Raw | ConvertFrom-Json).revision -eq $revision)
    }
    if (!$ready) {
        $base = "https://github.com/$repository/releases/download/build-$revision"
        Write-Host "Downloading program for source $revision ..."
        Write-Host 'If this version is still building, wait for GitHub Actions to finish and run again.'
        New-Item -ItemType Directory -Force -Path $runtimeRoot | Out-Null
        # Unique staging folder keeps interrupted downloads away from installed versions.
        $stage = Join-Path $runtimeRoot ('download-' + [guid]::NewGuid().ToString('N'))
        New-Item -ItemType Directory -Path $stage | Out-Null
        $zip = Join-Path $stage 'runtime.zip'
        $checksum = Join-Path $stage 'runtime.sha256'
        Invoke-WebRequest -UseBasicParsing -Uri "$base/runtime.sha256" -OutFile $checksum
        $expected = (Get-Content -LiteralPath $checksum -Raw).Trim()
        if ($expected -notmatch '^[0-9a-fA-F]{64}$') { throw 'Invalid download checksum.' }
        Invoke-WebRequest -UseBasicParsing -Uri "$base/runtime.zip" -OutFile $zip
        $stream = [IO.File]::OpenRead($zip)
        $sha = [Security.Cryptography.SHA256]::Create()
        try { $actual = [BitConverter]::ToString($sha.ComputeHash($stream)).Replace('-','') }
        finally { $stream.Dispose(); $sha.Dispose() }
        if ($actual -ne $expected) { throw 'Download checksum mismatch. Please retry.' }
        $unpacked = Join-Path $stage 'unpacked'
        Expand-Archive -LiteralPath $zip -DestinationPath $unpacked
        $info = Get-Content -LiteralPath (Join-Path $unpacked 'build-info.json') -Raw | ConvertFrom-Json
        if ($info.revision -ne $revision -or !(Test-Path -LiteralPath (Join-Path $unpacked 'Opengl_Physx.exe'))) { throw 'Downloaded program does not match the source version.' }
        if (Test-Path -LiteralPath $versionDir) { throw "Incomplete runtime folder exists: $versionDir . Rename that folder and retry." }
        Move-Item -LiteralPath $unpacked -Destination $versionDir
        Remove-Item -LiteralPath $zip,$checksum
        Remove-Item -LiteralPath $stage
    }
    Write-Host 'Starting Opengl_Physx...'
    Start-Process -FilePath $exe -WorkingDirectory $versionDir
} catch {
    Write-Host ('Startup failed: ' + $_.Exception.Message) -ForegroundColor Red
    Write-Host "Build status: https://github.com/$repository/actions"
    Write-Host 'A missing release can mean the build is pending or failed. An older program is never substituted.'
    exit 1
} finally {
    if ($stage -and (Test-Path -LiteralPath $stage)) {
        $resolvedStage = [IO.Path]::GetFullPath($stage)
        $resolvedRoot = [IO.Path]::GetFullPath((Join-Path $repoRoot 'Runtime')).TrimEnd('\') + '\'
        if ($resolvedStage.StartsWith($resolvedRoot, [StringComparison]::OrdinalIgnoreCase) -and
            [IO.Path]::GetFileName($resolvedStage) -match '^download-[0-9a-f]{32}$') {
            Remove-Item -LiteralPath $resolvedStage -Recurse -Force -ErrorAction SilentlyContinue
        }
    }
}
