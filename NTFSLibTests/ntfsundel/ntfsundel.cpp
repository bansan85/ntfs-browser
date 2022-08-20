#include "stdafx.h"
#include "ntfsundel.h"
#include "ntfsundelDlg.h"

#ifdef _DEBUG
  #define new DEBUG_NEW
  #undef THIS_FILE
static char THIS_FILE[] = __FILE__;
#endif

BEGIN_MESSAGE_MAP(CNtfsundelApp, CWinApp)
ON_COMMAND(ID_HELP, CWinApp::OnHelp)
END_MESSAGE_MAP()

CNtfsundelApp::CNtfsundelApp() {}

CNtfsundelApp theApp;

BOOL CNtfsundelApp::InitInstance()
{
  CNtfsundelDlg dlg;
  m_pMainWnd = &dlg;
  dlg.DoModal();

  return FALSE;
}
