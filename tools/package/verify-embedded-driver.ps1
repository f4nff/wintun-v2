param([Parameter(Mandatory)][string]$Dll, [Parameter(Mandatory)][string]$DriverDirectory)
$ErrorActionPreference = 'Stop'
if (-not ('PacketBridge.ResourceReader' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
namespace PacketBridge {
 public static class ResourceReader {
  [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)] static extern IntPtr LoadLibraryExW(string p, IntPtr r, uint f);
  [DllImport("kernel32.dll", SetLastError=true)] static extern IntPtr FindResourceW(IntPtr m, IntPtr n, IntPtr t);
  [DllImport("kernel32.dll")] static extern uint SizeofResource(IntPtr m, IntPtr r);
  [DllImport("kernel32.dll")] static extern IntPtr LoadResource(IntPtr m, IntPtr r);
  [DllImport("kernel32.dll")] static extern IntPtr LockResource(IntPtr r);
  [DllImport("kernel32.dll")] static extern bool FreeLibrary(IntPtr m);
  public static byte[] Read(string path, int id) {
   var m=LoadLibraryExW(path, IntPtr.Zero, 0x60);
   if(m==IntPtr.Zero) throw new System.ComponentModel.Win32Exception();
   try {
    var r=FindResourceW(m, (IntPtr)id, (IntPtr)10);
    if(r==IntPtr.Zero) throw new System.ComponentModel.Win32Exception();
    var size=checked((int)SizeofResource(m,r)); var p=LockResource(LoadResource(m,r));
    if(size==0 || p==IntPtr.Zero) throw new Exception("Empty driver resource");
    var bytes=new byte[size]; Marshal.Copy(p,bytes,0,size); return bytes;
   } finally { FreeLibrary(m); }
  }
 }
}
'@
}
$path = (Resolve-Path -LiteralPath $Dll).Path
$index = 101
foreach ($name in @('packetbridge.sys','packetbridge.inf','packetbridge.cat')) {
    $bytes = [PacketBridge.ResourceReader]::Read($path, $index++)
    $hash = [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($bytes)).ToLowerInvariant()
    $expected = (Get-FileHash -LiteralPath (Join-Path $DriverDirectory $name) -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($hash -ne $expected) { throw "Embedded signed input differs: $name" }
    [pscustomobject]@{ name=$name; size=$bytes.Length; sha256=$hash; exact_match=$true }
}
