; Hub Volume installer (NSIS 3.x, Modern UI 2)
; Build: makensis installer.nsi

Unicode true

!define APP_NAME     "Hub Volume"
!define APP_EXE      "HubVolume.exe"
!define APP_VERSION  "1.0.0"
!define PUBLISHER    "CraftKal"
!define WND_CLASS    "HubVolume.Main"
!define UNINST_KEY   "Software\Microsoft\Windows\CurrentVersion\Uninstall\HubVolume"
!define RUN_KEY      "Software\Microsoft\Windows\CurrentVersion\Run"
!define APPROVED_KEY "Software\Microsoft\Windows\CurrentVersion\Explorer\StartupApproved\Run"
!define STAGE        "..\dist\staging"

Name "${APP_NAME}"
OutFile "..\release\HubVolume-Setup-${APP_VERSION}.exe"
InstallDir "$LOCALAPPDATA\Programs\HubVolume"
InstallDirRegKey HKCU "Software\HubVolume" "InstallDir"
RequestExecutionLevel user
SetCompressor /SOLID lzma
BrandingText "${APP_NAME} ${APP_VERSION}  -  ${PUBLISHER}"

!include "MUI2.nsh"
!include "LogicLib.nsh"
!include "WinMessages.nsh"

!define MUI_ICON   "${STAGE}\HubVolume.ico"
!define MUI_UNICON "${STAGE}\HubVolume.ico"
!define MUI_ABORTWARNING
!define MUI_HEADERIMAGE
!define MUI_HEADERIMAGE_BITMAP "${STAGE}\header.bmp"
!define MUI_HEADERIMAGE_RIGHT
!define MUI_WELCOMEFINISHPAGE_BITMAP   "${STAGE}\welcome.bmp"
!define MUI_UNWELCOMEFINISHPAGE_BITMAP "${STAGE}\welcome.bmp"
!define MUI_COMPONENTSPAGE_SMALLDESC

!define MUI_WELCOMEPAGE_TITLE "Selamat datang di Hub Volume"
!define MUI_WELCOMEPAGE_TEXT "Hub Volume mengunci volume perangkat, mikrofon, dan aplikasi. Jika ada yang mengubah volume, nilainya langsung dikembalikan ke posisi saat dikunci.$\r$\n$\r$\nProgram berjalan di latar belakang dengan ikon di dekat jam dan tidak membutuhkan akses administrator.$\r$\n$\r$\nKlik Selanjutnya untuk melanjutkan."

!define MUI_FINISHPAGE_TITLE "Hub Volume sudah terpasang"
!define MUI_FINISHPAGE_RUN
!define MUI_FINISHPAGE_RUN_TEXT "Jalankan Hub Volume sekarang"
!define MUI_FINISHPAGE_RUN_FUNCTION LaunchApp

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_COMPONENTS
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_WELCOME
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_UNPAGE_FINISH

!insertmacro MUI_LANGUAGE "Indonesian"

VIProductVersion "1.0.0.0"
VIAddVersionKey /LANG=${LANG_INDONESIAN} "ProductName"     "${APP_NAME}"
VIAddVersionKey /LANG=${LANG_INDONESIAN} "CompanyName"     "${PUBLISHER}"
VIAddVersionKey /LANG=${LANG_INDONESIAN} "FileDescription" "Installer ${APP_NAME}"
VIAddVersionKey /LANG=${LANG_INDONESIAN} "FileVersion"     "${APP_VERSION}"
VIAddVersionKey /LANG=${LANG_INDONESIAN} "ProductVersion"  "${APP_VERSION}"
VIAddVersionKey /LANG=${LANG_INDONESIAN} "LegalCopyright"  "Copyright (C) 2026 ${PUBLISHER}"

!macro CloseRunningApp
  FindWindow $0 "${WND_CLASS}"
  ${If} $0 <> 0
    SendMessage $0 ${WM_CLOSE} 0 0 /TIMEOUT=4000
    Sleep 800
  ${EndIf}
!macroend

Function LaunchApp
  Exec '"$INSTDIR\${APP_EXE}"'
FunctionEnd

Function .onInit
  SetShellVarContext current
  !insertmacro CloseRunningApp
FunctionEnd

Section "Hub Volume (wajib)" SecMain
  SectionIn RO
  SetOutPath "$INSTDIR"
  !insertmacro CloseRunningApp
  File "${STAGE}\${APP_EXE}"
  WriteUninstaller "$INSTDIR\Uninstall.exe"

  CreateShortcut "$SMPROGRAMS\${APP_NAME}.lnk" "$INSTDIR\${APP_EXE}" "" "$INSTDIR\${APP_EXE}" 0

  WriteRegStr   HKCU "Software\HubVolume" "InstallDir" "$INSTDIR"
  WriteRegStr   HKCU "${UNINST_KEY}" "DisplayName"          "${APP_NAME}"
  WriteRegStr   HKCU "${UNINST_KEY}" "DisplayVersion"       "${APP_VERSION}"
  WriteRegStr   HKCU "${UNINST_KEY}" "Publisher"            "${PUBLISHER}"
  WriteRegStr   HKCU "${UNINST_KEY}" "DisplayIcon"          "$INSTDIR\${APP_EXE}"
  WriteRegStr   HKCU "${UNINST_KEY}" "InstallLocation"      "$INSTDIR"
  WriteRegStr   HKCU "${UNINST_KEY}" "UninstallString"      '"$INSTDIR\Uninstall.exe"'
  WriteRegStr   HKCU "${UNINST_KEY}" "QuietUninstallString" '"$INSTDIR\Uninstall.exe" /S'
  WriteRegDWORD HKCU "${UNINST_KEY}" "NoModify" 1
  WriteRegDWORD HKCU "${UNINST_KEY}" "NoRepair" 1
  SectionGetSize ${SecMain} $0
  WriteRegDWORD HKCU "${UNINST_KEY}" "EstimatedSize" $0
SectionEnd

Section /o "Pintasan di Desktop" SecDesktop
  CreateShortcut "$DESKTOP\${APP_NAME}.lnk" "$INSTDIR\${APP_EXE}" "" "$INSTDIR\${APP_EXE}" 0
SectionEnd

Section "Mulai bersama Windows (disarankan)" SecStartup
  WriteRegStr HKCU "${RUN_KEY}" "HubVolume" '"$INSTDIR\${APP_EXE}" --background'
  DeleteRegValue HKCU "${APPROVED_KEY}" "HubVolume"
SectionEnd

!insertmacro MUI_FUNCTION_DESCRIPTION_BEGIN
  !insertmacro MUI_DESCRIPTION_TEXT ${SecMain}    "Program utama, pintasan Start Menu, dan uninstaller."
  !insertmacro MUI_DESCRIPTION_TEXT ${SecDesktop} "Menambahkan ikon Hub Volume di Desktop."
  !insertmacro MUI_DESCRIPTION_TEXT ${SecStartup} "Hub Volume jalan otomatis di latar belakang saat Windows menyala."
!insertmacro MUI_FUNCTION_DESCRIPTION_END

Function un.onInit
  SetShellVarContext current
  !insertmacro CloseRunningApp
FunctionEnd

Section "Uninstall"
  !insertmacro CloseRunningApp

  Delete "$SMPROGRAMS\${APP_NAME}.lnk"
  Delete "$DESKTOP\${APP_NAME}.lnk"
  Delete "$INSTDIR\${APP_EXE}"
  Delete "$INSTDIR\Uninstall.exe"
  RMDir  "$INSTDIR"

  DeleteRegValue HKCU "${RUN_KEY}" "HubVolume"
  DeleteRegValue HKCU "${APPROVED_KEY}" "HubVolume"
  DeleteRegKey   HKCU "${UNINST_KEY}"
  DeleteRegKey   HKCU "Software\HubVolume"

  ${IfNot} ${Silent}
    MessageBox MB_YESNO|MB_ICONQUESTION|MB_DEFBUTTON2 \
      "Hapus juga pengaturan dan daftar volume yang terkunci?$\r$\n$\r$\nPilih Tidak jika Anda berencana memasang Hub Volume lagi." \
      IDNO +2
    RMDir /r "$LOCALAPPDATA\HubVolume"
  ${EndIf}
SectionEnd
