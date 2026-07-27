; NSIS installer for the Windows build.
;
; Deliberately a per-user install: it lands in %LOCALAPPDATA%\Programs, needs
; no administrator rights and raises no UAC prompt. The build is unsigned, so
; users already have one SmartScreen warning to click through; asking for
; elevation on top of that is a worse first run for no benefit. It also keeps
; the install directory writable, which matches how the app provisions its
; FujiNet runtime tree.
;
; Built in the same MSYS2 job that builds the app (mingw-w64-*-nsis), from the
; staged folder that also becomes the portable zip -- so the two artifacts are
; the same bits, packaged twice.
;
; makensis -DVERSION=x.y.z -DSRCDIR=<staged folder> -DOUTFILE=<exe> installer.nsi

Unicode true

!ifndef VERSION
  !define VERSION "0.0.0"
!endif
!ifndef SRCDIR
  !define SRCDIR "dist\FujiNet-Go-Apple2"
!endif
!ifndef OUTFILE
  !define OUTFILE "FujiNet-Go-Apple2-Setup.exe"
!endif
; CI passes this absolute; the default assumes makensis runs from the repo root.
!ifndef APPICON
  !define APPICON "frontends\windows\app.ico"
!endif

!define APPNAME    "FujiNet Go Apple II"
!define PUBLISHER  "Thomas Cherryhomes"
!define EXENAME    "fujinet-go-apple2-windows.exe"
!define HOMEPAGE   "https://fujinet.online/"
!define REGAPP     "Software\FujiNetGoApple2"
!define REGUNINST  "Software\Microsoft\Windows\CurrentVersion\Uninstall\FujiNetGoApple2"

Name "${APPNAME} ${VERSION}"
OutFile "${OUTFILE}"
RequestExecutionLevel user
InstallDir "$LOCALAPPDATA\Programs\${APPNAME}"
InstallDirRegKey HKCU "${REGAPP}" "InstallDir"
SetCompressor /SOLID lzma

VIProductVersion "${VERSION}.0"
VIAddVersionKey "ProductName"     "${APPNAME}"
VIAddVersionKey "FileDescription" "${APPNAME} installer"
VIAddVersionKey "FileVersion"     "${VERSION}"
VIAddVersionKey "ProductVersion"  "${VERSION}"
VIAddVersionKey "CompanyName"     "${PUBLISHER}"
VIAddVersionKey "LegalCopyright"  "GPL-3.0-or-later"

!include "MUI2.nsh"

; The application's own icon, so the installer and the Add/Remove Programs
; entry look like the thing being installed.
!define MUI_ICON   "${APPICON}"
!define MUI_UNICON "${APPICON}"
!define MUI_FINISHPAGE_RUN "$INSTDIR\${EXENAME}"
!define MUI_FINISHPAGE_RUN_TEXT "Run ${APPNAME}"

; The shipped build has no Apple II system ROMs in it -- they are Apple
; copyrighted firmware and cannot be redistributed (see COMPLIANCE.md), so
; the machine will not boot until the user supplies them. Saying so on the
; welcome page is the difference between "first run needs one more step" and
; "I installed it and got a black screen".
!define MUI_WELCOMEPAGE_TEXT \
  "This will install ${APPNAME} ${VERSION}.$\r$\n$\r$\n\
The Apple II system ROMs are Apple copyrighted firmware and are not included \
in this download. After installing, use Media > Import System ROMs... to \
supply them; FujiNet itself works without them.$\r$\n$\r$\n\
Click Next to continue."

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "English"

Section "Install"
  SetOutPath "$INSTDIR"
  ; The whole staged folder: the exe, fujinet.dll beside it (where the session
  ; looks first) and the pristine FujiNet runtime tree it provisions from.
  File /r "${SRCDIR}\*"

  CreateShortCut "$SMPROGRAMS\${APPNAME}.lnk" "$INSTDIR\${EXENAME}" "" \
                 "$INSTDIR\${EXENAME}" 0

  WriteRegStr HKCU "${REGAPP}" "InstallDir" "$INSTDIR"
  WriteRegStr HKCU "${REGAPP}" "Version"    "${VERSION}"

  ; Add/Remove Programs.
  WriteRegStr   HKCU "${REGUNINST}" "DisplayName"     "${APPNAME}"
  WriteRegStr   HKCU "${REGUNINST}" "DisplayVersion"  "${VERSION}"
  WriteRegStr   HKCU "${REGUNINST}" "Publisher"       "${PUBLISHER}"
  WriteRegStr   HKCU "${REGUNINST}" "DisplayIcon"     "$INSTDIR\${EXENAME}"
  WriteRegStr   HKCU "${REGUNINST}" "URLInfoAbout"    "${HOMEPAGE}"
  WriteRegStr   HKCU "${REGUNINST}" "InstallLocation" "$INSTDIR"
  WriteRegStr   HKCU "${REGUNINST}" "UninstallString" "$\"$INSTDIR\uninstall.exe$\""
  WriteRegDWORD HKCU "${REGUNINST}" "NoModify" 1
  WriteRegDWORD HKCU "${REGUNINST}" "NoRepair" 1
  WriteUninstaller "$INSTDIR\uninstall.exe"
SectionEnd

Section "Uninstall"
  Delete "$SMPROGRAMS\${APPNAME}.lnk"
  Delete "$INSTDIR\uninstall.exe"
  Delete "$INSTDIR\${EXENAME}"
  Delete "$INSTDIR\fujinet.dll"
  RMDir /r "$INSTDIR\fujinet"
  RMDir "$INSTDIR"

  DeleteRegKey HKCU "${REGUNINST}"
  DeleteRegKey HKCU "${REGAPP}"

  ; Settings, imported ROMs, mounted-image state and the provisioned FujiNet
  ; tree live under %LOCALAPPDATA%\fujinet-go-apple2 and are deliberately left
  ; behind: an uninstall should not throw away someone's configuration -- or
  ; the system ROMs they had to go and find -- and a reinstall picks it
  ; straight back up.
SectionEnd
