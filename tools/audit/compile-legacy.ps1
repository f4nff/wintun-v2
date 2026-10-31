# Compile every legacy DLL unit, without embedding or installing driver resources.
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '..\build\common.ps1')
Initialize-BuildEnvironment
$dir=Join-Path $script:RepoRoot 'out\legacy-compile'
New-Item -ItemType Directory -Path $dir -Force | Out-Null
$flags=@('/nologo','/c','/MT','/O2','/W4','/WX','/wd4100','/wd4201','/volatile:iso','/DWIN32_LEAN_AND_MEAN','/DUNICODE','/D_UNICODE','/DMAYBE_WOW64','/D_WIN32_WINNT=0x0601','/DNTDDI_VERSION=0x06010000',"/I$script:RepoRoot\Release\amd64\api-intermediate","/Fo$dir\")+$script:VersionArguments
Invoke-BuildTool 'cl.exe' ($flags+@('api\main.c','api\adapter.c','api\driver.c','api\logger.c','api\namespace.c','api\registry.c','api\resource.c','api\session.c','api\rundll32.c'))
