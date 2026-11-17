Unicode true
RequestExecutionLevel user
SetCompressor /SOLID lzma
CRCCheck force
AllowRootDirInstall false
AllowSkipFiles off

!include "MUI2.nsh"
!include "LogicLib.nsh"
!include "FileFunc.nsh"
!include "WordFunc.nsh"
!include "x64.nsh"

!ifndef FAH_VERSION
  !error "FAH_VERSION is required"
!endif
!ifndef FAH_PAYLOAD_INCLUDE
  !error "FAH_PAYLOAD_INCLUDE is required"
!endif
!ifndef FAH_OUTPUT
  !error "FAH_OUTPUT is required"
!endif
!include "${FAH_PAYLOAD_INCLUDE}"

!define PRODUCT "FAH Saver"
!define INSTALL_KEY "Software\FAHSaver\Installer"
!define UNINSTALL_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\FAHSaver"
!define DESKTOP_KEY "Control Panel\Desktop"

Name "${PRODUCT}"
OutFile "${FAH_OUTPUT}"
InstallDir "$LOCALAPPDATA\Programs\FAH Saver"
InstallDirRegKey HKCU "${INSTALL_KEY}" "InstallDir"
BrandingText "FAH Saver"
ShowInstDetails hide
ShowUninstDetails hide
VIProductVersion "${FAH_VERSION}.0"
VIAddVersionKey /LANG=1033 "ProductName" "${PRODUCT}"
VIAddVersionKey /LANG=1033 "FileDescription" "FAH Saver Setup"
VIAddVersionKey /LANG=1033 "FileVersion" "${FAH_VERSION}"
VIAddVersionKey /LANG=1033 "ProductVersion" "${FAH_VERSION}"
VIAddVersionKey /LANG=1033 "LegalCopyright" "FAH Saver contributors"

!define MUI_ABORTWARNING
!define MUI_WELCOMEPAGE_TITLE "Install FAH Saver"
!define MUI_WELCOMEPAGE_TEXT "Setup installs FAH Saver for your Windows account and selects it as your screen saver.$\r$\n$\r$\nYour settings are kept when updating. You can delete this setup file when installation is complete."
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_INSTFILES
!define MUI_FINISHPAGE_RUN
!define MUI_FINISHPAGE_RUN_TEXT "Open FAH Saver settings"
!define MUI_FINISHPAGE_RUN_FUNCTION ConfigureSaver
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

Var Mutex
Var OldInstallDir
Var OldVersion
Var OldSaver
Var HadOldSaver
Var OldActive
Var TransactionStarted
Var RegistrationChanged
Var RollbackFailed
Var MetadataDir
Var MetadataVersion

!macro Fail TEXT
  SetErrorLevel 2
  MessageBox MB_OK|MB_ICONSTOP "${TEXT}" /SD IDOK
  Abort
!macroend

!macro AcquireMutex
  System::Call 'kernel32::CreateMutexW(p 0, i 0, w "Local\FAHSaver.Setup") p.r0 ?e'
  Pop $1
  StrCpy $Mutex $0
  ${If} $0 == 0
  ${OrIf} $1 == 183
    !insertmacro Fail "Another FAH Saver setup or uninstaller is running. Close it and try again."
  ${EndIf}
!macroend

!macro Preflight RELATIVE
  !define /redef LOCK_LABEL "lock_${__LINE__}"
  ${LOCK_LABEL}:
  ${If} ${FileExists} "$INSTDIR\${RELATIVE}"
    System::Call 'kernel32::CreateFileW(w "$INSTDIR\${RELATIVE}", i 0x40000000, i 3, p 0, i 3, i 0, p 0) p.r0'
    ${If} $0 == -1
      MessageBox MB_RETRYCANCEL|MB_ICONEXCLAMATION "Close FAH Saver and Screen Saver Settings, then click Retry. Setup cannot update or remove $INSTDIR\${RELATIVE}." /SD IDCANCEL IDRETRY ${LOCK_LABEL}
      SetErrorLevel 2
      Abort
    ${EndIf}
    System::Call 'kernel32::CloseHandle(p r0)'
  ${EndIf}
  !undef LOCK_LABEL
!macroend

!macro Backup RELATIVE
  ${If} ${FileExists} "$INSTDIR\${RELATIVE}"
    System::Call 'kernel32::CopyFileW(w "$INSTDIR\${RELATIVE}", w "$PLUGINSDIR\backup\${RELATIVE}", i 1) i.r0'
    ${If} $0 == 0
      !insertmacro Fail "Setup could not back up the existing FAH Saver files. No files have been replaced."
    ${EndIf}
  ${EndIf}
!macroend

!macro CopyPayload RELATIVE
  System::Call 'kernel32::CopyFileW(w "$PLUGINSDIR\payload\${RELATIVE}", w "$INSTDIR\${RELATIVE}", i 0) i.r0'
  ${If} $0 == 0
    Call RollbackInstallation
    !insertmacro Fail "Setup could not update FAH Saver. Close its preview and settings, then try again."
  ${EndIf}
!macroend

!macro RestoreFile RELATIVE
  ${If} ${FileExists} "$PLUGINSDIR\backup\${RELATIVE}"
    System::Call 'kernel32::CopyFileW(w "$PLUGINSDIR\backup\${RELATIVE}", w "$INSTDIR\${RELATIVE}", i 0) i.r0'
    ${If} $0 == 0
      StrCpy $RollbackFailed 1
    ${EndIf}
  ${Else}
    ClearErrors
    Delete "$INSTDIR\${RELATIVE}"
    ${If} ${Errors}
      StrCpy $RollbackFailed 1
    ${EndIf}
  ${EndIf}
!macroend

!macro DeleteManaged RELATIVE
  ClearErrors
  Delete "$INSTDIR\${RELATIVE}"
  ${If} ${Errors}
    !insertmacro Fail "FAH Saver could not remove $INSTDIR\${RELATIVE}. Close its preview and settings, then run Uninstall again."
  ${EndIf}
!macroend

Function .onInit
  SetShellVarContext current
  SetRegView 64
  ${IfNot} ${RunningX64}
    !insertmacro Fail "This FAH Saver build requires 64-bit Windows."
  ${EndIf}
  !insertmacro AcquireMutex
  GetFullPathName $INSTDIR "$INSTDIR"
  ${GetRoot} "$INSTDIR" $0
  ${If} $INSTDIR == "$0\"
  ${OrIf} $INSTDIR == "$WINDIR"
  ${OrIf} $INSTDIR == "$SYSDIR"
  ${OrIf} $INSTDIR == "$LOCALAPPDATA"
    !insertmacro Fail "Choose an application folder for FAH Saver."
  ${EndIf}
  ReadRegStr $OldInstallDir HKCU "${INSTALL_KEY}" "InstallDir"
  ReadRegStr $OldVersion HKCU "${INSTALL_KEY}" "Version"
  ${If} $OldVersion != ""
    ${VersionCompare} "${FAH_VERSION}" "$OldVersion" $0
    ${If} $0 == 2
      !insertmacro Fail "A newer FAH Saver version is already installed. Use its setup file to update or repair it."
    ${EndIf}
  ${EndIf}
  StrCpy $HadOldSaver 1
  ClearErrors
  ReadRegStr $OldSaver HKCU "${DESKTOP_KEY}" "SCRNSAVE.EXE"
  ${If} ${Errors}
    StrCpy $HadOldSaver 0
  ${EndIf}
  System::Call 'user32::SystemParametersInfoW(i 0x10, i 0, *i .r0, i 0) i.r1'
  ${If} $1 == 0
    !insertmacro Fail "Setup could not read the current screen saver state."
  ${EndIf}
  StrCpy $OldActive $0
FunctionEnd

Function WriteMetadata
  ClearErrors
  WriteRegStr HKCU "${INSTALL_KEY}" "InstallDir" "$MetadataDir"
  WriteRegStr HKCU "${INSTALL_KEY}" "Version" "$MetadataVersion"
  WriteRegStr HKCU "${UNINSTALL_KEY}" "DisplayName" "${PRODUCT}"
  WriteRegStr HKCU "${UNINSTALL_KEY}" "DisplayVersion" "$MetadataVersion"
  WriteRegStr HKCU "${UNINSTALL_KEY}" "Publisher" "FAH Saver contributors"
  WriteRegStr HKCU "${UNINSTALL_KEY}" "InstallLocation" "$MetadataDir"
  WriteRegStr HKCU "${UNINSTALL_KEY}" "DisplayIcon" '$\"$MetadataDir\FAHSaver.scr$\",0'
  WriteRegStr HKCU "${UNINSTALL_KEY}" "UninstallString" '$\"$MetadataDir\Uninstall.exe$\"'
  WriteRegStr HKCU "${UNINSTALL_KEY}" "QuietUninstallString" '$\"$MetadataDir\Uninstall.exe$\" /S'
  WriteRegDWORD HKCU "${UNINSTALL_KEY}" "NoModify" 1
  WriteRegDWORD HKCU "${UNINSTALL_KEY}" "NoRepair" 1
FunctionEnd

Function ConfigureSaver
  Exec '$\"$INSTDIR\FAHSaver.scr$\" /c'
  ${If} ${Errors}
    MessageBox MB_OK|MB_ICONEXCLAMATION "FAH Saver is installed. Open Screen Saver Settings in Windows to configure it."
  ${EndIf}
FunctionEnd

!macro CurrentSaverOwnership PREFIX
Function ${PREFIX}IsCurrentSaver
  StrCpy $9 0
  ReadRegStr $0 HKCU "${DESKTOP_KEY}" "SCRNSAVE.EXE"
  StrCpy $1 $0 1
  ${If} $1 == '$\"'
    StrCpy $1 $0 1 -1
    ${If} $1 == '$\"'
      StrCpy $0 $0 -1 1
    ${EndIf}
  ${EndIf}
  ${If} $0 == ""
    Return
  ${EndIf}
  GetFullPathName $0 "$0"
  GetFullPathName $1 "$INSTDIR\FAHSaver.scr"
  ${If} $0 == $1
    StrCpy $9 1
    Return
  ${EndIf}
  ; Control Panel can store an 8.3 path for the same installed screen saver.
  System::Call 'kernel32::GetLongPathNameW(w r0, w .r1, i ${NSIS_MAX_STRLEN}) i.r2'
  ${If} $2 == 0
    Return
  ${EndIf}
  System::Call 'kernel32::GetLongPathNameW(w "$INSTDIR\FAHSaver.scr", w .r2, i ${NSIS_MAX_STRLEN}) i.r3'
  ${If} $3 != 0
  ${AndIf} $1 == $2
    StrCpy $9 1
  ${EndIf}
FunctionEnd
!macroend
!insertmacro CurrentSaverOwnership ""
!insertmacro CurrentSaverOwnership "un."

!macro MetadataRemoval PREFIX
Function ${PREFIX}RemoveMetadata
  StrCpy $8 0
  ReadRegStr $0 HKCU "${UNINSTALL_KEY}" "DisplayName"
  ${If} $0 == "${PRODUCT}"
    ClearErrors
    DeleteRegKey HKCU "${UNINSTALL_KEY}"
    ${If} ${Errors}
      StrCpy $8 1
    ${EndIf}
  ${EndIf}
  ; Keep the ownership record if Apps removal failed, so Uninstall can be retried.
  ${If} $8 == 0
    ReadRegStr $0 HKCU "${INSTALL_KEY}" "InstallDir"
    ${If} $0 != ""
      ClearErrors
      DeleteRegKey HKCU "${INSTALL_KEY}"
      ${If} ${Errors}
        StrCpy $8 1
      ${EndIf}
    ${EndIf}
  ${EndIf}
  ClearErrors
  ${If} $8 != 0
    SetErrors
  ${EndIf}
FunctionEnd
!macroend
!insertmacro MetadataRemoval ""
!insertmacro MetadataRemoval "un."

Section "FAH Saver"
  InitPluginsDir
  !insertmacro FAH_ExtractPayload
  ClearErrors
  WriteUninstaller "$PLUGINSDIR\payload\Uninstall.exe"
  ${If} ${Errors}
    !insertmacro Fail "Setup could not prepare its uninstaller."
  ${EndIf}
  !insertmacro FAH_ManagedFiles Preflight
  ClearErrors
  CreateDirectory "$PLUGINSDIR\backup\shaders"
  CreateDirectory "$PLUGINSDIR\backup\licences"
  ${If} ${Errors}
    !insertmacro Fail "Setup could not prepare a backup folder."
  ${EndIf}
  !insertmacro FAH_ManagedFiles Backup
  ClearErrors
  CreateDirectory "$INSTDIR\shaders"
  CreateDirectory "$INSTDIR\licences"
  ${If} ${Errors}
    !insertmacro Fail "Setup cannot write to its installation folder."
  ${EndIf}
  StrCpy $TransactionStarted 1
  !insertmacro FAH_ManagedFiles CopyPayload
  StrCpy $RegistrationChanged 1
  ClearErrors
  WriteRegStr HKCU "${DESKTOP_KEY}" "SCRNSAVE.EXE" "$INSTDIR\FAHSaver.scr"
  ${If} ${Errors}
    Call RollbackInstallation
    !insertmacro Fail "Setup could not register FAH Saver for your account."
  ${EndIf}
  System::Call 'user32::SystemParametersInfoW(i 0x11, i 1, p 0, i 3) i.r0'
  ${If} $0 == 0
    Call RollbackInstallation
    !insertmacro Fail "Setup could not activate FAH Saver for your account."
  ${EndIf}
  StrCpy $MetadataDir $INSTDIR
  StrCpy $MetadataVersion "${FAH_VERSION}"
  Call WriteMetadata
  ${If} ${Errors}
    Call RollbackInstallation
    !insertmacro Fail "Setup could not create the Windows uninstall entry."
  ${EndIf}
  StrCpy $TransactionStarted 0
  StrCpy $RegistrationChanged 0
  SetErrorLevel 0
SectionEnd

Function RollbackInstallation
  ${If} $TransactionStarted == 1
    StrCpy $RollbackFailed 0
    ${If} $RegistrationChanged == 1
      Call IsCurrentSaver
      ${If} $9 == 1
        ClearErrors
        ${If} $HadOldSaver == 1
          WriteRegStr HKCU "${DESKTOP_KEY}" "SCRNSAVE.EXE" "$OldSaver"
        ${Else}
          DeleteRegValue HKCU "${DESKTOP_KEY}" "SCRNSAVE.EXE"
        ${EndIf}
        ${If} ${Errors}
          StrCpy $RollbackFailed 1
        ${EndIf}
        System::Call 'user32::SystemParametersInfoW(i 0x11, i $OldActive, p 0, i 3) i.r0'
        ${If} $0 == 0
          StrCpy $RollbackFailed 1
        ${EndIf}
      ${EndIf}
      ${If} $OldInstallDir != ""
        StrCpy $MetadataDir $OldInstallDir
        StrCpy $MetadataVersion $OldVersion
        Call WriteMetadata
        ${If} ${Errors}
          StrCpy $RollbackFailed 1
        ${EndIf}
      ${Else}
        Call RemoveMetadata
        ${If} ${Errors}
          StrCpy $RollbackFailed 1
        ${EndIf}
      ${EndIf}
    ${EndIf}
    !insertmacro FAH_ManagedFiles RestoreFile
    ${If} $RollbackFailed == 1
      MessageBox MB_OK|MB_ICONSTOP "Setup could not fully restore the previous installation. Run this setup again after closing FAH Saver and its settings." /SD IDOK
    ${EndIf}
    StrCpy $TransactionStarted 0
    StrCpy $RegistrationChanged 0
  ${EndIf}
FunctionEnd

Function .onInstFailed
  Call RollbackInstallation
FunctionEnd

Function un.onInit
  SetShellVarContext current
  SetRegView 64
  !insertmacro AcquireMutex
  ReadRegStr $0 HKCU "${INSTALL_KEY}" "InstallDir"
  GetFullPathName $0 "$0"
  GetFullPathName $INSTDIR "$INSTDIR"
  ${If} $0 != $INSTDIR
    !insertmacro Fail "This uninstaller does not belong to the registered FAH Saver installation. Use Apps in Windows Settings."
  ${EndIf}
FunctionEnd

Section "Uninstall"
  !insertmacro FAH_ManagedFiles Preflight
  Call un.IsCurrentSaver
  ${If} $9 == 1
    System::Call 'user32::SystemParametersInfoW(i 0x11, i 0, p 0, i 3) i.r0'
    ${If} $0 == 0
      !insertmacro Fail "Close the screen saver and unlock Windows, then run Uninstall again."
    ${EndIf}
    ClearErrors
    DeleteRegValue HKCU "${DESKTOP_KEY}" "SCRNSAVE.EXE"
    ${If} ${Errors}
      !insertmacro Fail "FAH Saver could not remove its screen saver registration."
    ${EndIf}
  ${EndIf}
  !insertmacro FAH_PayloadFiles DeleteManaged
  Call un.RemoveMetadata
  ${If} ${Errors}
    !insertmacro Fail "FAH Saver could not remove its Windows uninstall entry. Run Uninstall again."
  ${EndIf}
  !insertmacro DeleteManaged "Uninstall.exe"
  RMDir "$INSTDIR\shaders"
  RMDir "$INSTDIR\licences"
  RMDir "$INSTDIR"
  SetErrorLevel 0
SectionEnd
