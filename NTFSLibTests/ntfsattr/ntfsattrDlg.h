// ntfsattrDlg.h : header file
//

#if !defined(AFX_NTFSATTRDLG_H__0A2C5040_6755_450E_BB6E_4E00B0737621__INCLUDED_)
  #define AFX_NTFSATTRDLG_H__0A2C5040_6755_450E_BB6E_4E00B0737621__INCLUDED_

  #if _MSC_VER > 1000
    #pragma once
  #endif  // _MSC_VER > 1000

/////////////////////////////////////////////////////////////////////////////
// CNtfsattrDlg dialog

class CNtfsattrDlg : public CDialog
{
  // Construction

 public:
  CNtfsattrDlg(CWnd* pParent = NULL);  // standard constructor

  // Dialog Data
  //{{AFX_DATA(CNtfsattrDlg)
  enum
  {
    IDD = IDD_NTFSATTR_DIALOG
  };
  CString m_filename;
  CString m_dump;
  BOOL m_dir;
  //}}AFX_DATA

  // ClassWizard generated virtual function overrides
  //{{AFX_VIRTUAL(CNtfsattrDlg)

 protected:
  void DoDataExchange(CDataExchange* pDX) override;  // DDX/DDV support
                                                     //}}AFX_VIRTUAL

  // Implementation

 protected:
  HICON m_hIcon;

  // Generated message map functions
  //{{AFX_MSG(CNtfsattrDlg)
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

#endif  // !defined(AFX_NTFSATTRDLG_H__0A2C5040_6755_450E_BB6E_4E00B0737621__INCLUDED_)
