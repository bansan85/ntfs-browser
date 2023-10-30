// ntfsdump.h : main header file for the NTFSDUMP application
//

#if !defined(AFX_NTFSDUMP_H__4E22D562_0E60_4D30_BB7D_67248FE43EA9__INCLUDED_)
  #define AFX_NTFSDUMP_H__4E22D562_0E60_4D30_BB7D_67248FE43EA9__INCLUDED_

  #if _MSC_VER > 1000
    #pragma once
  #endif  // _MSC_VER > 1000

  #ifndef __AFXWIN_H__
    #error include 'stdafx.h' before including this file for PCH
  #endif

  #include "resource.h"  // main symbols

/////////////////////////////////////////////////////////////////////////////
// CNtfsdumpApp:
// See ntfsdump.cpp for the implementation of this class
//

class CNtfsdumpApp : public CWinApp
{
 public:
  CNtfsdumpApp();

  // Overrides
  // ClassWizard generated virtual function overrides
  //{{AFX_VIRTUAL(CNtfsdumpApp)

 public:
  BOOL InitInstance() override;
  //}}AFX_VIRTUAL

  // Implementation

  //{{AFX_MSG(CNtfsdumpApp)
  // NOTE - the ClassWizard will add and remove member functions here.
  //    DO NOT EDIT what you see in these blocks of generated code !
  //}}AFX_MSG
  DECLARE_MESSAGE_MAP()
};

/////////////////////////////////////////////////////////////////////////////

//{{AFX_INSERT_LOCATION}}
// Microsoft Visual C++ will insert additional declarations immediately before the previous line.

#endif  // !defined(AFX_NTFSDUMP_H__4E22D562_0E60_4D30_BB7D_67248FE43EA9__INCLUDED_)
