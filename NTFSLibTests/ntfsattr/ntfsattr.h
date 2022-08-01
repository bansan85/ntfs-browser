// ntfsattr.h : main header file for the NTFSATTR application
//

#if !defined(AFX_NTFSATTR_H__AC7D1972_E501_4372_92FF_C01F78585AD6__INCLUDED_)
  #define AFX_NTFSATTR_H__AC7D1972_E501_4372_92FF_C01F78585AD6__INCLUDED_

  #if _MSC_VER > 1000
    #pragma once
  #endif  // _MSC_VER > 1000

  #ifndef __AFXWIN_H__
    #error include 'stdafx.h' before including this file for PCH
  #endif

  #include "resource.h"  // main symbols

/////////////////////////////////////////////////////////////////////////////
// CNtfsattrApp:
// See ntfsattr.cpp for the implementation of this class
//

class CNtfsattrApp : public CWinApp
{
 public:
  CNtfsattrApp();

  // Overrides
  // ClassWizard generated virtual function overrides
  //{{AFX_VIRTUAL(CNtfsattrApp)

 public:
  virtual BOOL InitInstance();
  //}}AFX_VIRTUAL

  // Implementation

  //{{AFX_MSG(CNtfsattrApp)
  // NOTE - the ClassWizard will add and remove member functions here.
  //    DO NOT EDIT what you see in these blocks of generated code !
  //}}AFX_MSG
  DECLARE_MESSAGE_MAP()
};

/////////////////////////////////////////////////////////////////////////////

//{{AFX_INSERT_LOCATION}}
// Microsoft Visual C++ will insert additional declarations immediately before the previous line.

#endif  // !defined(AFX_NTFSATTR_H__AC7D1972_E501_4372_92FF_C01F78585AD6__INCLUDED_)
