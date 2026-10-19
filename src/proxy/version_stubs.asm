; Jump stubs for the version.dll exports. Each one forwards to the real system DLL
; through g_versionProcs, filled by LoadRealVersionDll() before the game calls anything.

EXTERN g_versionProcs:QWORD

.code

Proxy_GetFileVersionInfoA PROC
    jmp qword ptr [g_versionProcs + 0]
Proxy_GetFileVersionInfoA ENDP

Proxy_GetFileVersionInfoByHandle PROC
    jmp qword ptr [g_versionProcs + 8]
Proxy_GetFileVersionInfoByHandle ENDP

Proxy_GetFileVersionInfoExA PROC
    jmp qword ptr [g_versionProcs + 16]
Proxy_GetFileVersionInfoExA ENDP

Proxy_GetFileVersionInfoExW PROC
    jmp qword ptr [g_versionProcs + 24]
Proxy_GetFileVersionInfoExW ENDP

Proxy_GetFileVersionInfoSizeA PROC
    jmp qword ptr [g_versionProcs + 32]
Proxy_GetFileVersionInfoSizeA ENDP

Proxy_GetFileVersionInfoSizeExA PROC
    jmp qword ptr [g_versionProcs + 40]
Proxy_GetFileVersionInfoSizeExA ENDP

Proxy_GetFileVersionInfoSizeExW PROC
    jmp qword ptr [g_versionProcs + 48]
Proxy_GetFileVersionInfoSizeExW ENDP

Proxy_GetFileVersionInfoSizeW PROC
    jmp qword ptr [g_versionProcs + 56]
Proxy_GetFileVersionInfoSizeW ENDP

Proxy_GetFileVersionInfoW PROC
    jmp qword ptr [g_versionProcs + 64]
Proxy_GetFileVersionInfoW ENDP

Proxy_VerFindFileA PROC
    jmp qword ptr [g_versionProcs + 72]
Proxy_VerFindFileA ENDP

Proxy_VerFindFileW PROC
    jmp qword ptr [g_versionProcs + 80]
Proxy_VerFindFileW ENDP

Proxy_VerInstallFileA PROC
    jmp qword ptr [g_versionProcs + 88]
Proxy_VerInstallFileA ENDP

Proxy_VerInstallFileW PROC
    jmp qword ptr [g_versionProcs + 96]
Proxy_VerInstallFileW ENDP

Proxy_VerLanguageNameA PROC
    jmp qword ptr [g_versionProcs + 104]
Proxy_VerLanguageNameA ENDP

Proxy_VerLanguageNameW PROC
    jmp qword ptr [g_versionProcs + 112]
Proxy_VerLanguageNameW ENDP

Proxy_VerQueryValueA PROC
    jmp qword ptr [g_versionProcs + 120]
Proxy_VerQueryValueA ENDP

Proxy_VerQueryValueW PROC
    jmp qword ptr [g_versionProcs + 128]
Proxy_VerQueryValueW ENDP

END
