# Uses Windows' catalog/SIP hashing so adding a PE signature does not change the image identity.
# https://learn.microsoft.com/windows/win32/api/mscat/nf-mscat-cryptcatadmincalchashfromfilehandle2
if (-not ('PacketBridgeBuild.CatalogHash' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.IO;
using System.Runtime.InteropServices;
namespace PacketBridgeBuild {
    public static class CatalogHash {
        [DllImport("wintrust.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        static extern bool CryptCATAdminAcquireContext2(out IntPtr ctx, IntPtr subsystem, string algorithm, IntPtr policy, uint flags);
        [DllImport("wintrust.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        static extern bool CryptCATAdminCalcHashFromFileHandle2(IntPtr ctx, IntPtr file, ref uint size, [Out] byte[] hash, uint flags);
        [DllImport("wintrust.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        static extern bool CryptCATAdminReleaseContext(IntPtr ctx, uint flags);
        public static string Get(string path) {
            IntPtr ctx;
            if (!CryptCATAdminAcquireContext2(out ctx, IntPtr.Zero, "SHA256", IntPtr.Zero, 0))
                throw new Win32Exception(Marshal.GetLastWin32Error());
            try {
                using (var file = File.OpenRead(path)) {
                    uint size = 0;
                    if (!CryptCATAdminCalcHashFromFileHandle2(ctx, file.SafeFileHandle.DangerousGetHandle(), ref size, null, 0))
                        throw new Win32Exception(Marshal.GetLastWin32Error());
                    if (size != 32) throw new InvalidDataException("Unexpected SHA256 catalog hash size");
                    var hash = new byte[size];
                    if (!CryptCATAdminCalcHashFromFileHandle2(ctx, file.SafeFileHandle.DangerousGetHandle(), ref size, hash, 0))
                        throw new Win32Exception(Marshal.GetLastWin32Error());
                    return BitConverter.ToString(hash).Replace("-", "").ToLowerInvariant();
                }
            } finally { CryptCATAdminReleaseContext(ctx, 0); }
        }
    }
}
'@
}
function Get-CatalogMemberHash([string]$Path) {
    [PacketBridgeBuild.CatalogHash]::Get([IO.Path]::GetFullPath($Path))
}
