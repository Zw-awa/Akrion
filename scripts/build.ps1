param(
    [string]$QtRoot,
    [string]$QtVersion
)
$arguments = @("build")
if ($QtRoot) { $arguments += @("-QtRoot", $QtRoot) }
if ($QtVersion) { $arguments += @("-QtVersion", $QtVersion) }
& (Join-Path (Split-Path -Parent $PSScriptRoot) "tools\qt.ps1") @arguments
exit $LASTEXITCODE
