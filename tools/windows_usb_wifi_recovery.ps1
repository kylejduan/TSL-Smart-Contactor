param([string]$Port = 'COM8', [switch]$SelfTest)
$ErrorActionPreference = 'Stop'
# .Path can be a PowerShell provider path (Microsoft.PowerShell.Core\FileSystem::...)
# which Python cannot use in PYTHONPATH. ProviderPath is a native UNC path.
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..')).ProviderPath
$recovery = Join-Path $repo '.tools\local-recovery\windows-usb'
$python = Join-Path $recovery 'python\cpython-3.12.13-windows-x86_64-none\python.exe'
if (Test-Path $python) {
    $env:PYTHONPATH = Join-Path $recovery 'venv\Lib\site-packages'
} else {
    $python = 'python'
}
& $python -c 'import serial; import cryptography' 2>$null
if ($LASTEXITCODE -ne 0) {
    throw 'Python USB dependencies are missing; install tools/requirements.txt before entering credentials.'
}
if ($SelfTest) {
    Write-Host 'Windows USB Python dependencies: OK'
    exit 0
}
Write-Host 'USB-only Wi-Fi recovery. Keep the branch circuit isolated and verified off.'
Write-Host 'The controller must be uncommissioned, DISABLED and dry-run.'
Write-Host 'Enter the LOCAL administrator password and current 2.4 GHz Wi-Fi details only here.'
Write-Host 'Tesla consent, token and local HTTPS keys are preserved. Output remains inhibited.'
& $python (Join-Path $PSScriptRoot 'onboard.py') usb --port $Port wifi_update
if ($LASTEXITCODE -ne 0) {
    Write-Host 'Recovery did not complete. Check USB status before retrying.'
} else {
    Write-Host 'If committed=true, wait for reboot and verify USB diagnostics.'
}
