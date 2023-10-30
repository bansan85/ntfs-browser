// ntfsattr.cpp : Defines the class behaviors for the application.
//

#include "StdAfx.h"
#include "ntfsattr.h"
#include "ntfsattrDlg.h"

#ifdef _DEBUG
  #define new DEBUG_NEW
  #undef THIS_FILE
static char THIS_FILE[] = __FILE__;
#endif

BEGIN_MESSAGE_MAP(CNtfsattrApp, CWinApp)
ON_COMMAND(ID_HELP, &CWinApp::OnHelp)
END_MESSAGE_MAP()

CNtfsattrApp::CNtfsattrApp() {}

CNtfsattrApp theApp;

BOOL CNtfsattrApp::InitInstance()
{
  AfxEnableControlContainer();

  CNtfsattrDlg dlg;
  m_pMainWnd = &dlg;
  dlg.DoModal();

  return FALSE;
}
