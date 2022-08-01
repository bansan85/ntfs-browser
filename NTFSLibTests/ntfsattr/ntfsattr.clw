; CLW file contains information for the MFC ClassWizard

[General Info]
Version=1
LastClass=CNtfsattrDlg
LastTemplate=CDialog
NewFileInclude1=#include "stdafx.h"
NewFileInclude2=#include "ntfsattr.h"

ClassCount=3
Class1=CNtfsattrApp
Class2=CNtfsattrDlg
Class3=CAboutDlg

ResourceCount=3
Resource1=IDD_ABOUTBOX
Resource2=IDR_MAINFRAME
Resource3=IDD_NTFSATTR_DIALOG

[CLS:CNtfsattrApp]
Type=0
HeaderFile=ntfsattr.h
ImplementationFile=ntfsattr.cpp
Filter=N

[CLS:CNtfsattrDlg]
Type=0
HeaderFile=ntfsattrDlg.h
ImplementationFile=ntfsattrDlg.cpp
Filter=D
BaseClass=CDialog
VirtualFilter=dWC
LastObject=IDC_DIR

[CLS:CAboutDlg]
Type=0
HeaderFile=ntfsattrDlg.h
ImplementationFile=ntfsattrDlg.cpp
Filter=D

[DLG:IDD_ABOUTBOX]
Type=1
Class=CAboutDlg
ControlCount=4
Control1=IDC_STATIC,static,1342177283
Control2=IDC_STATIC,static,1342308480
Control3=IDC_STATIC,static,1342308352
Control4=IDOK,button,1342373889

[DLG:IDD_NTFSATTR_DIALOG]
Type=1
Class=CNtfsattrDlg
ControlCount=4
Control1=IDOK,button,1342242816
Control2=IDC_FILENAME,static,1342308352
Control3=IDE_DUMP,edit,1352734916
Control4=IDC_DIR,button,1342242819

