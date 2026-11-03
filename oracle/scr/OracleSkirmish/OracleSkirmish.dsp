# Microsoft Developer Studio Project File - Name="OracleSkirmish" - Package Owner=<4>
# Microsoft Developer Studio Generated Build File, Format Version 6.00
# ** DO NOT EDIT **

# TARGTYPE "Win32 (x86) Dynamic-Link Library" 0x0102

CFG=OracleSkirmish - Win32 Release
!MESSAGE This is not a valid makefile. To build this project using NMAKE,
!MESSAGE use the Export Makefile command and run
!MESSAGE
!MESSAGE NMAKE /f "OracleSkirmish.mak".
!MESSAGE
!MESSAGE Possible choices for configuration are:
!MESSAGE
!MESSAGE "OracleSkirmish - Win32 Release" (based on "Win32 (x86) Dynamic-Link Library")
!MESSAGE

# Begin Project
# PROP AllowPerConfigDependencies 0
# PROP Scc_ProjName ""
# PROP Scc_LocalPath "."
CPP=cl.exe
MTL=midl.exe
RSC=rc.exe

# Release only.  Flags and link line copied from _ScriptTemplates/Multi/ProjectFile.dsp.template;
# paths assume this directory sits at Scripts/<Type>/OracleSkirmish inside the SDK tree.
# oracle/scr/build.sh feeds this file to toolchain/build_scr.sh, which uses these Release lines and
# handles the SDK's truncated dmp3D_MSVCR.lib (toolchain/README.md). Link line as in the template.

!IF  "$(CFG)" == "OracleSkirmish - Win32 Release"

# PROP BASE Use_MFC 0
# PROP BASE Use_Debug_Libraries 0
# PROP BASE Output_Dir "Release"
# PROP BASE Intermediate_Dir "Release"
# PROP BASE Target_Dir ""
# PROP Use_MFC 0
# PROP Use_Debug_Libraries 0
# PROP Output_Dir "Release"
# PROP Intermediate_Dir "Release"
# PROP Ignore_Export_Lib 0
# PROP Target_Dir ""
# ADD BASE CPP /nologo /MT /W3 /GX /O2 /D "WIN32" /D "NDEBUG" /D "_WINDOWS" /YX /FD /c
# ADD CPP /nologo /MT /W3 /GR /GX /O2 /I "..\..\scriptinterface" /I "..\..\Shared" /I "..\..\Shared\QSI\Projects\QSIInc\Tiger" /I "..\..\Shared\Source\Taldren\Common" /I "..\..\Shared\Source\Taldren\Common\Exceptions" /I "..\..\Shared\Source\Taldren\Common\DataStore" /I "..\..\Shared\Source\Taldren\Common\Log" /I "..\..\Shared\Source\Taldren\Common\Log\MessageDLL" /I "..\..\Shared\Source\Taldren\Common\Thread" /I "..\..\Shared\Source\Taldren\Common\BugSlayer" /I "..\..\Shared\Source\Taldren\Common\Crypto" /I "..\..\Shared\Source\Taldren\Common\Database" /I "..\..\Shared\Source\Include" /I "..\..\Shared\Source\Scripting" /I "..\..\Shared\Source\Network" /I "..\..\Shared\Source\PrecompiledHeaders" /I "..\..\Shared\Meta" /D "WIN32" /D "NDEBUG" /D "_WINDOWS" /YX /FD /c
# ADD BASE MTL /nologo /D "NDEBUG" /mktyplib203 /win32
# ADD MTL /nologo /D "NDEBUG" /mktyplib203 /win32
# ADD BASE RSC /l 0x409 /d "NDEBUG"
# ADD RSC /l 0x409 /d "NDEBUG"
BSC32=bscmake.exe
# ADD BASE BSC32 /nologo
# ADD BSC32 /nologo
LINK32=link.exe
# ADD BASE LINK32 kernel32.lib user32.lib gdi32.lib winspool.lib comdlg32.lib advapi32.lib shell32.lib ole32.lib oleaut32.lib uuid.lib odbc32.lib odbccp32.lib /nologo /subsystem:windows /dll /machine:I386
# ADD LINK32 sharedr.lib scriptinterfaceR.lib kernel32.lib user32.lib gdi32.lib winspool.lib comdlg32.lib advapi32.lib shell32.lib ole32.lib oleaut32.lib uuid.lib odbc32.lib odbccp32.lib dmp3d_MSVCR.lib sfcTiger_MSVCR.lib dmp3D_MSVCR.lib bugslayerutil.lib cryptlibR.lib /nologo /subsystem:windows /dll /machine:I386 /out:"Release/OracleSkirmish.scr" /libpath:"..\..\libraries"

!ENDIF

# Begin Target

# Name "OracleSkirmish - Win32 Release"
# Begin Group "Mission Info"

# PROP Default_Filter ""
# Begin Source File

SOURCE=.\MissionMaps.cpp
# End Source File
# Begin Source File

SOURCE=.\MissionMaps.h
# End Source File
# Begin Source File

SOURCE=.\MissionText.cpp
# End Source File
# Begin Source File

SOURCE=.\MissionText.h
# End Source File
# End Group
# Begin Group "Oracle"

# PROP Default_Filter ""
# Begin Source File

SOURCE=.\OracleConfig.h
# End Source File
# Begin Source File

SOURCE=.\OracleRecorder.cpp
# End Source File
# Begin Source File

SOURCE=.\OracleRecorder.h
# End Source File
# Begin Source File

SOURCE=.\OracleSeed.cpp
# End Source File
# Begin Source File

SOURCE=.\OracleSeed.h
# End Source File
# End Group
# Begin Group "Teams"

# PROP Default_Filter ""
# Begin Source File

SOURCE=.\OracleTeam.cpp
# End Source File
# Begin Source File

SOURCE=.\OracleTeam.h
# End Source File
# End Group
# Begin Source File

SOURCE=.\OracleSkirmish.cpp
# End Source File
# Begin Source File

SOURCE=.\OracleSkirmish.h
# End Source File
# End Target
# End Project
