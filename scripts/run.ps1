param(
    [string]$QtRoot,
    [string]$QtVersion,
    [switch]$SmokeTest
)
$arguments = @($(if ($SmokeTest) { "smoke" } else { "run" }))
if ($QtRoot) { $arguments += @("-QtRoot", $QtRoot) }
if ($QtVersion) { $arguments += @("-QtVersion", $QtVersion) }
& (Join-Path (Split-Path -Parent $PSScriptRoot) "tools\qt.ps1") @arguments
exit $LASTEXITCODE
