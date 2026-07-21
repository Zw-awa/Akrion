[CmdletBinding()]
param(
    [Parameter(Position = 0)]
    [ValidateSet("configure", "build", "test", "run", "cli", "smoke", "deploy", "clean", "help")]
    [string]$Mode = "build",
    [Parameter(Position = 1, ValueFromRemainingArguments = $true)]
    [string[]]$CliArguments,
    [Parameter()]
    [string]$QtRoot,
    [Parameter()]
    [string]$QtVersion,
    [Parameter()]
    [switch]$AgentSafe,
    [Parameter()]
    [switch]$Foreground
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$useAgentSafe = $AgentSafe.IsPresent -or [bool]$env:CODEX_THREAD_ID

function Read-DotEnv([string]$Path) {
    $values = @{}
    if (-not (Test-Path -LiteralPath $Path)) { return $values }
    foreach ($rawLine in Get-Content -LiteralPath $Path) {
        $line = $rawLine.Trim()
        if (-not $line -or $line.StartsWith("#")) { continue }
        if ($line.StartsWith("export ")) { $line = $line.Substring(7).Trim() }
        $parts = $line.Split("=", 2)
        if ($parts.Count -ne 2) { continue }
        $key = $parts[0].Trim()
        $value = $parts[1].Trim()
        if ($value.Length -ge 2 -and (($value.StartsWith('"') -and $value.EndsWith('"')) -or ($value.StartsWith("'") -and $value.EndsWith("'")))) {
            $value = $value.Substring(1, $value.Length - 2)
        }
        if ($key -match '^[A-Za-z_][A-Za-z0-9_]*$') { $values[$key] = $value }
    }
    return $values
}

function Get-QtRoots {
    Get-PSDrive -PSProvider FileSystem | ForEach-Object {
        $candidate = Join-Path $_.Root "Qt"
        if (Test-Path -LiteralPath $candidate) { (Resolve-Path -LiteralPath $candidate).Path }
    }
}

function Get-QtVersions([string]$Root) {
    Get-ChildItem -LiteralPath $Root -Directory -ErrorAction SilentlyContinue | ForEach-Object {
        $runtime = Join-Path $_.FullName "mingw_64\bin\Qt6Core.dll"
        if (Test-Path -LiteralPath $runtime) {
            $parsed = try { [version]$_.Name } catch { [version]"0.0" }
            [pscustomobject]@{ Name = $_.Name; Parsed = $parsed }
        }
    } | Sort-Object Parsed -Descending
}

$dotEnv = Read-DotEnv (Join-Path $repoRoot ".env")
if (-not $QtRoot) {
    if ($env:QT_ROOT) { $QtRoot = $env:QT_ROOT }
    elseif ($dotEnv.ContainsKey("QT_ROOT") -and $dotEnv["QT_ROOT"]) { $QtRoot = $dotEnv["QT_ROOT"] }
}
if (-not $QtVersion) {
    if ($env:QT_VERSION) { $QtVersion = $env:QT_VERSION }
    elseif ($dotEnv.ContainsKey("QT_VERSION") -and $dotEnv["QT_VERSION"]) { $QtVersion = $dotEnv["QT_VERSION"] }
}
if (-not $QtRoot) {
    $roots = @(Get-QtRoots)
    if ($QtVersion) {
        $QtRoot = $roots | Where-Object { Test-Path -LiteralPath (Join-Path $_ "$QtVersion\mingw_64\bin\Qt6Core.dll") } | Select-Object -First 1
    } else {
        $QtRoot = $roots | Where-Object { @(Get-QtVersions $_).Count -gt 0 } | Select-Object -First 1
    }
}
if (-not $QtRoot) { throw "Qt was not found. Set QT_ROOT/QT_VERSION or create .env from .env.example." }
$QtRoot = [System.IO.Path]::GetFullPath($QtRoot)
if (-not $QtVersion) {
    $QtVersion = (Get-QtVersions $QtRoot | Select-Object -First 1).Name
}
if (-not $QtVersion) { throw "No Qt MinGW kit was found under $QtRoot" }

$qtKit = Join-Path $QtRoot "$QtVersion\mingw_64"
$qtBin = Join-Path $qtKit "bin"
$mingwDirectory = Get-ChildItem -LiteralPath (Join-Path $QtRoot "Tools") -Directory -Filter "mingw*_64" -ErrorAction SilentlyContinue | Sort-Object Name -Descending | Select-Object -First 1
$cmakeDirectory = Get-ChildItem -LiteralPath (Join-Path $QtRoot "Tools") -Directory -Filter "CMake*" -ErrorAction SilentlyContinue | Sort-Object Name -Descending | Select-Object -First 1
if (-not $mingwDirectory) { throw "Qt MinGW tools were not found under $QtRoot\Tools" }
if (-not $cmakeDirectory) { throw "Qt CMake tools were not found under $QtRoot\Tools" }
$mingwBin = Join-Path $mingwDirectory.FullName "bin"
$cmakeExe = Join-Path $cmakeDirectory.FullName "bin\cmake.exe"
$ctestExe = Join-Path $cmakeDirectory.FullName "bin\ctest.exe"
$deployExe = Join-Path $qtBin "windeployqt.exe"
$buildName = if ($useAgentSafe) { "qt-agent" } else { "qt-local" }
$buildDir = Join-Path $repoRoot "build\$buildName"
$cliExecutable = Join-Path $buildDir "akrion.exe"
$guiExecutable = Join-Path $buildDir "akrion-gui.exe"
if (-not $env:AKRION_STORE) {
    if ($dotEnv.ContainsKey("AKRION_STORE") -and $dotEnv["AKRION_STORE"]) {
        $configuredStore = $dotEnv["AKRION_STORE"]
        if (-not [IO.Path]::IsPathRooted($configuredStore)) {
            $configuredStore = Join-Path $repoRoot $configuredStore
        }
        $env:AKRION_STORE = [IO.Path]::GetFullPath($configuredStore)
    } elseif ($useAgentSafe) {
        $env:AKRION_STORE = Join-Path $buildDir "runs"
    }
}

function Assert-Tool([string]$Path, [string]$Name) {
    if (-not (Test-Path -LiteralPath $Path)) { throw "$Name was not found at $Path" }
}

function Set-QtEnvironment {
    Assert-Tool $cmakeExe "CMake"
    Assert-Tool (Join-Path $mingwBin "g++.exe") "Qt MinGW"
    Assert-Tool (Join-Path $qtBin "Qt6Core.dll") "Qt runtime"

    $cleanPath = @($env:Path.Split([IO.Path]::PathSeparator) | Where-Object {
        $entry = $_.Trim().Trim('"')
        if (-not $entry) { return $false }
        try {
            -not (Test-Path -LiteralPath ([IO.Path]::Combine($entry, "Qt6Core.dll")) -ErrorAction SilentlyContinue) -and
                -not (Test-Path -LiteralPath ([IO.Path]::Combine($entry, "Qt6Gui.dll")) -ErrorAction SilentlyContinue)
        } catch {
            $true
        }
    })
    $env:Path = (@($qtBin, $mingwBin, (Split-Path -Parent $cmakeExe)) + $cleanPath) -join [IO.Path]::PathSeparator
    $env:QT_PLUGIN_PATH = Join-Path $qtKit "plugins"
    $env:QML_IMPORT_PATH = Join-Path $qtKit "qml"
    $env:QML2_IMPORT_PATH = Join-Path $qtKit "qml"
    $env:QT_QPA_PLATFORM_PLUGIN_PATH = Join-Path $qtKit "plugins\platforms"
}

function Configure-Qt {
    Set-QtEnvironment
    $safeValue = if ($useAgentSafe) { "ON" } else { "OFF" }
    Write-Output "[INFO] Qt kit: $qtKit"
    Write-Output "[INFO] Build directory: $buildDir"
    Write-Output "[INFO] Agent-safe CMake: $safeValue"
    & $cmakeExe -S $repoRoot -B $buildDir -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release "-DCMAKE_PREFIX_PATH=$qtKit" "-DQT_AGENT_SAFE_BUILD=$safeValue"
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

function Build-Qt {
    Configure-Qt
    $parallel = if ($useAgentSafe) { "1" } else { [string][Math]::Max(1, [Environment]::ProcessorCount) }
    & $cmakeExe --build $buildDir --parallel $parallel
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    if (-not (Test-Path -LiteralPath $cliExecutable)) { throw "Build completed but CLI was not found at $cliExecutable" }
    if (-not (Test-Path -LiteralPath $guiExecutable)) { throw "Build completed but GUI was not found at $guiExecutable" }
    Write-Output "[INFO] Built CLI: $cliExecutable"
    Write-Output "[INFO] Built GUI: $guiExecutable"
}

function Assert-Built {
    if (-not (Test-Path -LiteralPath $cliExecutable) -or -not (Test-Path -LiteralPath $guiExecutable)) {
        throw "Akrion is not built. Run tools/qt.ps1 build first."
    }
}

function Test-Qt {
    Set-QtEnvironment
    Assert-Built
    Assert-Tool $ctestExe "CTest"
    & $ctestExe --test-dir $buildDir --output-on-failure
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

function Run-Qt {
    Set-QtEnvironment
    Assert-Built
    if ($Foreground) { & $guiExecutable; exit $LASTEXITCODE }
    $process = Start-Process -FilePath $guiExecutable -WorkingDirectory (Split-Path -Parent $guiExecutable) -PassThru
    Write-Output "[INFO] Started Akrion PID=$($process.Id)"
}

function Run-Cli {
    Set-QtEnvironment
    Assert-Built
    & $cliExecutable @CliArguments
    exit $LASTEXITCODE
}

function Smoke-TestQt {
    Set-QtEnvironment
    Assert-Built
    $smokeStore = Join-Path $buildDir "smoke-store"
    & $cliExecutable doctor --json --store $smokeStore
    if ($LASTEXITCODE -ne 0) { throw "Akrion CLI smoke test failed with exit code $LASTEXITCODE" }
    $process = Start-Process -FilePath $guiExecutable -ArgumentList "--smoke-test" -WorkingDirectory (Split-Path -Parent $guiExecutable) -WindowStyle Hidden -Wait -PassThru
    if ($process.ExitCode -ne 0) { throw "Qt smoke test failed with exit code $($process.ExitCode)" }
    Write-Output "[INFO] CLI and GUI smoke tests passed."
}

function Deploy-Qt {
    Set-QtEnvironment
    if ($useAgentSafe) {
        throw "Qt deployment is unavailable in the restricted agent environment. Run tools/qt.ps1 deploy from a normal PowerShell terminal."
    }

    # Always rebuild the selected local kit so deploy cannot package a stale build tree.
    Build-Qt
    Assert-Tool $deployExe "windeployqt"

    $distRoot = [IO.Path]::GetFullPath((Join-Path $repoRoot "dist"))
    $distDir = [IO.Path]::GetFullPath((Join-Path $distRoot "Akrion"))
    $safePrefix = $distRoot.TrimEnd([IO.Path]::DirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
    if (-not $distDir.StartsWith($safePrefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to deploy outside the repository dist directory: $distDir"
    }
    if (Test-Path -LiteralPath $distDir) {
        Remove-Item -LiteralPath $distDir -Recurse -Force
    }
    New-Item -ItemType Directory -Path $distDir -Force | Out-Null
    $distGui = Join-Path $distDir "akrion-gui.exe"
    $distCli = Join-Path $distDir "akrion.exe"
    Copy-Item -LiteralPath $guiExecutable -Destination $distGui -Force
    Copy-Item -LiteralPath $cliExecutable -Destination $distCli -Force

    & $deployExe --release --force --compiler-runtime --no-translations --qmldir (Join-Path $repoRoot "qml") --dir $distDir $distGui
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

    $qtConf = "[Paths]`r`nPrefix=.`r`nPlugins=plugins`r`nQmlImports=qml`r`n"
    [IO.File]::WriteAllText((Join-Path $distDir "qt.conf"), $qtConf,
                            (New-Object Text.UTF8Encoding($false)))
    Copy-Item -LiteralPath (Join-Path $repoRoot "LICENSE") -Destination $distDir -Force
    $thirdPartyNotice = Join-Path $repoRoot "THIRD_PARTY_NOTICES.md"
    if (Test-Path -LiteralPath $thirdPartyNotice) {
        Copy-Item -LiteralPath $thirdPartyNotice -Destination $distDir -Force
    }

    $requiredFiles = @(
        "Qt6Core.dll",
        "Qt6Gui.dll",
        "Qt6Qml.dll",
        "Qt6Quick.dll",
        "Qt6QuickControls2.dll",
        "Qt6SerialPort.dll",
        "platforms\qwindows.dll"
    )
    foreach ($relativePath in $requiredFiles) {
        if (-not (Test-Path -LiteralPath (Join-Path $distDir $relativePath))) {
            throw "Deployment is incomplete: $relativePath was not produced."
        }
    }
    Write-Output "[INFO] Standalone application: $distGui"
}

function Clean-Qt {
    if (-not (Test-Path -LiteralPath $buildDir)) { return }
    $resolved = (Resolve-Path -LiteralPath $buildDir).Path
    $buildRoot = [System.IO.Path]::GetFullPath((Join-Path $repoRoot "build"))
    if (-not $resolved.StartsWith($buildRoot, [StringComparison]::OrdinalIgnoreCase)) { throw "Refusing to remove path outside the build directory: $resolved" }
    Remove-Item -LiteralPath $resolved -Recurse -Force
    Write-Output "[INFO] Removed $resolved"
}

switch ($Mode) {
    "configure" { Configure-Qt }
    "build" { Build-Qt }
    "test" { Test-Qt }
    "run" { Run-Qt }
    "cli" { Run-Cli }
    "smoke" { Smoke-TestQt }
    "deploy" { Deploy-Qt }
    "clean" { Clean-Qt }
    "help" { Write-Output "tools/qt.ps1 [configure|build|test|run|cli|smoke|deploy|clean]" }
}

exit 0
