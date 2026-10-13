# Build PoseBridge.dll with the .NET Framework compiler and copy it into Velocidrone.
param(
    [Parameter(Mandatory = $true)]
    [string]$VelocidroneDir
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
$Repo = (Resolve-Path (Join-Path $Root "..\..")).Path
$Out = Join-Path $Root "bin"
New-Item -ItemType Directory -Force -Path $Out | Out-Null

$managed = $null
foreach ($name in @("Velocidrone_Data", "velocidrone_Data")) {
    $candidate = Join-Path $VelocidroneDir "$name\Managed"
    if (Test-Path (Join-Path $candidate "UnityEngine.CoreModule.dll")) {
        $managed = $candidate
        break
    }
}
if (-not $managed) {
    throw "Could not find Velocidrone_Data\Managed under $VelocidroneDir"
}

$bep = Join-Path $VelocidroneDir "BepInEx\core\BepInEx.dll"
if (-not (Test-Path $bep)) {
    throw "Install BepInEx 5 (Unity Mono, 64-bit) into $VelocidroneDir first. Expected $bep"
}

$csc = Join-Path $env:WINDIR "Microsoft.NET\Framework64\v4.0.30319\csc.exe"
if (-not (Test-Path $csc)) {
    throw "csc.exe was not found. Install the .NET Framework 4.x targeting pack."
}

$pluginSources = @(
    "PosePacket.cs",
    "ContactApply.cs",
    "OpenSky.cs",
    "PoseSender.cs",
    "PoseBridgePlugin.cs"
) | ForEach-Object { Join-Path $Root $_ }

$dll = Join-Path $Out "PoseBridge.dll"
& $csc /nologo /target:library "/out:$dll" `
    "/r:$bep" `
    "/r:$(Join-Path $managed 'UnityEngine.dll')" `
    "/r:$(Join-Path $managed 'UnityEngine.CoreModule.dll')" `
    "/r:$(Join-Path $managed 'UnityEngine.PhysicsModule.dll')" `
    "/r:$(Join-Path $managed 'UnityEngine.InputLegacyModule.dll')" `
    $pluginSources
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
Write-Host "Built $dll"

$layout = Join-Path $Out "LayoutCheck.exe"
& $csc /nologo /target:exe "/out:$layout" `
    (Join-Path $Root "PosePacket.cs") `
    (Join-Path $Root "LayoutCheck.cs")
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& $layout | Set-Content -Encoding ascii (Join-Path $Out "layout.txt")

$python = $null
$pythonArgs = @()
if (Get-Command py -ErrorAction SilentlyContinue) {
    $python = "py"
    $pythonArgs = @("-3")
} elseif (Get-Command python -ErrorAction SilentlyContinue) {
    $python = "python"
}
if (-not $python) {
    throw "Python 3 is required to run tools\pose_packet.py"
}
& $python @pythonArgs (Join-Path $Repo "tools\pose_packet.py")
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$plugins = Join-Path $VelocidroneDir "BepInEx\plugins"
New-Item -ItemType Directory -Force -Path $plugins | Out-Null
Copy-Item -Force $dll (Join-Path $plugins "PoseBridge.dll")
Write-Host "Installed -> $plugins\PoseBridge.dll"
