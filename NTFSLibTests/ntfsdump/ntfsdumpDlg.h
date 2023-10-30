// ntfsdumpDlg.h : header file
//

#if !defined(AFX_NTFSDUMPDLG_H__9818E5B9_3982_4E73_BBD5_1CFA221620BC__INCLUDED_)
  #define AFX_NTFSDUMPDLG_H__9818E5B9_3982_4E73_BBD5_1CFA221620BC__INCLUDED_

  #if _MSC_VER > 1000
    #pragma once
  #endif  // _MSC_VER > 1000

/////////////////////////////////////////////////////////////////////////////
// CNtfsdumpDlg dialog

class CNtfsdumpDlg : public CDialog
{
  // Construction

 public:
  CNtfsdumpDlg(CWnd* pParent = NULL);  // standard constructor

  // Dialog Data
  //{{AFX_DATA(CNtfsdumpDlg)
  enum
  {
    IDD = IDD_NTFSDUMP_DIALOG
  };
  CString m_filename;
  CString m_dump;
  //}}AFX_DATA

  // ClassWizard generated virtual function overrides
  //{{AFX_VIRTUAL(CNtfsdumpDlg)

 protected:
  void DoDataExchange(CDataExchange* pDX) override;  // DDX/DDV support
                                                     //}}AFX_VIRTUAL

  // Implementation

 protected:
  HICON m_hIcon;

  // Generated message map functions
  //{{AFX_MSG(CNtfsdumpDlg)
  BOOL OnInitDialog() override;
  afx_msg void OnSysCommand(UINT nID, LPARAM lParam);
  afx_msg void OnPaint();
  afx_msg HCURSOR OnQueryDragIcon();
  void OnOK() override;
  //}}AFX_MSG
  DECLARE_MESSAGE_MAP()
};

//{{AFX_INSERT_LOCATION}}
// Microsoft Visual C++ will insert additional declarations immediately before the previous line.

#endif  // !defined(AFX_NTFSDUMPDLG_H__9818E5B9_3982_4E73_BBD5_1CFA221620BC__INCLUDED_)
