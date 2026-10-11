; CLW file contains information for the MFC ClassWizard

[General Info]
Version=1
LastClass=CNtfsdumpDlg
LastTemplate=CDialog
NewFileInclude1=#include "stdafx.h"
NewFileInclude2=#include "ntfsdump.h"

ClassCount=3
Class1=CNtfsdumpApp
Class2=CNtfsdumpDlg
Class3=CAboutDlg

ResourceCount=3
Resource1=IDD_ABOUTBOX
Resource2=IDR_MAINFRAME
Resource3=IDD_NTFSDUMP_DIALOG

[CLS:CNtfsdumpApp]
Type=0
HeaderFile=ntfsdump.h
ImplementationFile=ntfsdump.cpp
Filter=N

[CLS:CNtfsdumpDlg]
Type=0
HeaderFile=ntfsdumpDlg.h
ImplementationFile=ntfsdumpDlg.cpp
Filter=D
BaseClass=CDialog
VirtualFilter=dWC
LastObject=CNtfsdumpDlg

[CLS:CAboutDlg]
Type=0
HeaderFile=ntfsdumpDlg.h
ImplementationFile=ntfsdumpDlg.cpp
Filter=D

[DLG:IDD_ABOUTBOX]
Type=1
Class=CAboutDlg
ControlCount=4
Control1=IDC_STATIC,static,1342177283
Control2=IDC_STATIC,static,1342308480
Control3=IDC_STATIC,static,1342308352
Control4=IDOK,button,1342373889

[DLG:IDD_NTFSDUMP_DIALOG]
Type=1
Class=CNtfsdumpDlg
ControlCount=3
Control1=IDOK,button,1342242816
Control2=IDC_FILENAME,static,1342308352
Control3=IDE_DUMP,edit,1352734916

