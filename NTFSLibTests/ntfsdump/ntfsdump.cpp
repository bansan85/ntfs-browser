#include "StdAfx.h"
#include "ntfsdump.h"
#include "ntfsdumpDlg.h"

#ifdef _DEBUG
  #define new DEBUG_NEW
  #undef THIS_FILE
static char THIS_FILE[] = __FILE__;
#endif

BEGIN_MESSAGE_MAP(CNtfsdumpApp, CWinApp)
ON_COMMAND(ID_HELP, &CWinApp::OnHelp)
END_MESSAGE_MAP()

CNtfsdumpApp::CNtfsdumpApp() {}

CNtfsdumpApp theApp;

BOOL CNtfsdumpApp::InitInstance()
{
  AfxEnableControlContainer();

  CNtfsdumpDlg dlg;
  m_pMainWnd = &dlg;
  dlg.DoModal();

  return FALSE;
}
