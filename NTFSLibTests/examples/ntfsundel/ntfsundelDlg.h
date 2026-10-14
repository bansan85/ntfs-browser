// ntfsundelDlg.h : header file
//

#if !defined( \
    AFX_NTFSUNDELDLG_H__18A9AB66_79B0_4691_95B0_DA24D5C6231F__INCLUDED_)
  #define AFX_NTFSUNDELDLG_H__18A9AB66_79B0_4691_95B0_DA24D5C6231F__INCLUDED_

  #if _MSC_VER > 1000
    #pragma once
  #endif  // _MSC_VER > 1000

  #include <cstdint>

/////////////////////////////////////////////////////////////////////////////
// CNtfsundelDlg dialog

namespace NtfsUndel {

struct CopyResult;
struct DeletedFile;
struct SearchResult;

}  // namespace NtfsUndel

class CNtfsundelDlg : public CDialog {
  // Construction

 public:
  CNtfsundelDlg(CWnd* pParent = NULL);  // standard constructor

  // Dialog Data
  //{{AFX_DATA(CNtfsundelDlg)
  enum : std::uint8_t { IDD = IDD_NTFSUNDEL_DIALOG };

  CListCtrl m_files;
  CComboBox m_driver;
  CString m_filter;
  //}}AFX_DATA

  // ClassWizard generated virtual function overrides
  //{{AFX_VIRTUAL(CNtfsundelDlg)

 protected:
  void DoDataExchange(CDataExchange* pDX) override;  // DDX/DDV support
                                                     //}}AFX_VIRTUAL

  // Implementation

 protected:
  HICON m_hIcon;

  // Generated message map functions
  //{{AFX_MSG(CNtfsundelDlg)
  BOOL OnInitDialog() override;
  afx_msg void OnPaint();
  afx_msg HCURSOR OnQueryDragIcon();
  afx_msg void OnSearch();
  afx_msg void OnRecover();
  afx_msg void OnSelchangeDriver();
  //}}AFX_MSG

  // Helpers for the search and recovery handlers above.
  void SetSearching(bool searching);
  _TCHAR SelectedDriveLetter();
  bool SelectedFile(ULONGLONG& ref, CString& name);
  void ShowSearchResults(const NtfsUndel::SearchResult& result);
  void AddResultRow(const NtfsUndel::DeletedFile& file);
  bool ChooseRecoveryPath(const CString& name, _TCHAR volname, CString& path);
  void ReportCopy(const NtfsUndel::CopyResult& copied);
  DECLARE_MESSAGE_MAP()
};

//{{AFX_INSERT_LOCATION}}
// Microsoft Visual C++ will insert additional declarations immediately before
// the previous line.

#endif  // !defined(AFX_NTFSUNDELDLG_H__18A9AB66_79B0_4691_95B0_DA24D5C6231F__INCLUDED_)
