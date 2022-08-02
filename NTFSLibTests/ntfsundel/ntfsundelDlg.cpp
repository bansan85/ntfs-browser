// ntfsundelDlg.cpp : implementation file
//

#include "stdafx.h"
#include "ntfsundel.h"
#include "ntfsundelDlg.h"

#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/attr-base.h>
#include <ntfs-browser/mft-idx.h>
#include <ntfs-browser/file-record.h>

using namespace NtfsBrowser;

#ifdef _DEBUG
  #define new DEBUG_NEW
  #undef THIS_FILE
static char THIS_FILE[] = __FILE__;
#endif

/////////////////////////////////////////////////////////////////////////////
// CNtfsundelDlg dialog

CNtfsundelDlg::CNtfsundelDlg(CWnd* pParent /*=NULL*/)
    : CDialog(CNtfsundelDlg::IDD, pParent)
{
  //{{AFX_DATA_INIT(CNtfsundelDlg)
  m_filter = _T("*.*");
  //}}AFX_DATA_INIT
  // Note that LoadIcon does not require a subsequent DestroyIcon in Win32
  m_hIcon = AfxGetApp()->LoadIcon(IDR_MAINFRAME);
}

void CNtfsundelDlg::DoDataExchange(CDataExchange* pDX)
{
  CDialog::DoDataExchange(pDX);
  //{{AFX_DATA_MAP(CNtfsundelDlg)
  DDX_Control(pDX, IDL_FILES, m_files);
  DDX_Control(pDX, IDC_DRIVER, m_driver);
  DDX_Text(pDX, IDE_FILTER, m_filter);
  DDV_MaxChars(pDX, m_filter, 30);
  //}}AFX_DATA_MAP
}

BEGIN_MESSAGE_MAP(CNtfsundelDlg, CDialog)
//{{AFX_MSG_MAP(CNtfsundelDlg)
ON_WM_PAINT()
ON_WM_QUERYDRAGICON()
ON_BN_CLICKED(IDB_SEARCH, OnSearch)
ON_BN_CLICKED(IDB_RECOVER, OnRecover)
ON_CBN_SELCHANGE(IDC_DRIVER, OnSelchangeDriver)
//}}AFX_MSG_MAP
END_MESSAGE_MAP()

/////////////////////////////////////////////////////////////////////////////
// CNtfsundelDlg message handlers

BOOL CNtfsundelDlg::OnInitDialog()
{
  CDialog::OnInitDialog();

  // Set the icon for this dialog.  The framework does this automatically
  //  when the application's main window is not a dialog
  SetIcon(m_hIcon, TRUE);   // Set big icon
  SetIcon(m_hIcon, FALSE);  // Set small icon

  // TODO: Add extra initialization here

  m_files.SetExtendedStyle(LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES |
                           LVS_EX_HEADERDRAGDROP);
  m_files.InsertColumn(1, _T("Reference"), LVCFMT_LEFT, 80);
  m_files.InsertColumn(2, _T("Name"), LVCFMT_LEFT, 260);
  m_files.InsertColumn(3, _T("Time"), LVCFMT_LEFT, 130);

  _TCHAR drvname[4];  // "C:\"
  drvname[0] = _T('A');
  drvname[1] = _T(':');
  drvname[2] = _T('\\');
  drvname[3] = _T('\0');

  DWORD bm = 1;                       // bit mask
  DWORD drives = GetLogicalDrives();  // available drives bitmap
  for (int i = 0; i < 32; i++)
  {
    if (drives & bm)
    {
      DWORD dt = GetDriveType(drvname);
      if (dt == DRIVE_FIXED || dt == DRIVE_REMOVABLE)
      {
        drvname[2] = _T('\0');
        m_driver.InsertString(-1, drvname);
        drvname[2] = _T('\\');
      }
    }

    drvname[0]++;
    bm <<= 1;
  }

  if (m_driver.GetCount() > 0) m_driver.SetCurSel(0);

  return TRUE;  // return TRUE  unless you set the focus to a control
}

// If you add a minimize button to your dialog, you will need the code below
//  to draw the icon.  For MFC applications using the document/view model,
//  this is automatically done for you by the framework.

void CNtfsundelDlg::OnPaint()
{
  if (IsIconic())
  {
    CPaintDC dc(this);  // device context for painting

    SendMessage(WM_ICONERASEBKGND, (WPARAM)dc.GetSafeHdc(), 0);

    // Center icon in client rectangle
    int cxIcon = GetSystemMetrics(SM_CXICON);
    int cyIcon = GetSystemMetrics(SM_CYICON);
    CRect rect;
    GetClientRect(&rect);
    int x = (rect.Width() - cxIcon + 1) / 2;
    int y = (rect.Height() - cyIcon + 1) / 2;

    // Draw the icon
    dc.DrawIcon(x, y, m_hIcon);
  }
  else
  {
    CDialog::OnPaint();
  }
}

// The system calls this to obtain the cursor to display while the user drags
//  the minimized window.
HCURSOR CNtfsundelDlg::OnQueryDragIcon() { return (HCURSOR)m_hIcon; }

// Check if file name matchs wildchar pattern
BOOL MatchFileName(const _TCHAR* fileName, const _TCHAR* pattern)
{
  // Compare until pattern is not '*' and not '?'
  while (*fileName && *pattern)
  {
    if (*pattern == _T('*') || *pattern == _T('?')) break;

    if (*fileName != *pattern) return FALSE;

    fileName++;
    pattern++;
  }

  if (*pattern == _T('\0'))
  {
    if (*fileName == _T('\0'))
      return TRUE;  // Exactly matched
    else
      return FALSE;
  }
  else if (*pattern == _T('?'))
  {
    do
    {
      if (*fileName == _T('\0'))
        return FALSE;
      else
      {
        fileName++;  // Skip to next
        pattern++;
      }
    }
    while (*pattern == _T('?'));

    return MatchFileName(fileName, pattern);
  }
  else if (*pattern == _T('*'))
  {
    const _TCHAR* p = pattern;

    // Skip to next character, not '?' and not '*"
    pattern++;
    while (*pattern)
    {
      if (*pattern == _T('*') || *pattern == _T('?'))
        pattern++;
      else
        break;
    }

    while (*fileName && *fileName != *pattern) fileName++;
    if (*fileName != *pattern)
      return FALSE;
    else
    {
      if (*pattern)
      {
        const _TCHAR* ff = fileName;
        const _TCHAR* pp = pattern;

        while (*fileName == *pp) fileName++;
        while (*pattern == *pp) pattern++;

        if (*fileName == *pattern && *fileName == _T('\0') &&
            (fileName - ff) >= (pattern - pp))
          return TRUE;

        if (MatchFileName(ff + 1, p))
          return TRUE;
        else
          return MatchFileName(ff, pp);
      }
      else
        return TRUE;
    }
  }
  else
    return FALSE;
}

BOOL PeekAndPump()
{
  MSG msg;
  while (PeekMessage(&msg, NULL, 0, 0, PM_NOREMOVE))
  {
    if (!AfxGetApp()->PumpMessage())
    {
      PostQuitMessage(0);
      return FALSE;
    }
  }

  return TRUE;
}

void CNtfsundelDlg::OnSearch()
{
  static BOOL stop = FALSE;  // Give user a chance to stop the searching loop

  CString btntext;
  GetDlgItem(IDB_SEARCH)->GetWindowText(btntext);
  if (btntext == _T("Stop"))
  {
    stop = TRUE;
    return;
  }

  m_files.DeleteAllItems();

  UpdateData();

  // Remove leading and trailing space
  if (m_filter.IsEmpty())
    m_filter = _T("*.*");  // default to find all deleted files
  else
  {
    m_filter.TrimLeft();
    m_filter.TrimRight();
  }

  // Volume information
  CString vns;
  int sel = m_driver.GetCurSel();
  if (sel >= 0) m_driver.GetLBText(sel, vns);
  if (vns.IsEmpty())
  {
    MessageBox(_T("Select a disk drive"));
    m_driver.SetFocus();
    return;
  }

  _TCHAR volname = vns.GetAt(0);

  NtfsVolume volume(volname);
  if (!volume.IsVolumeOK())
  {
    MessageBox(_T("Not a valid NTFS volume or NTFS version < 3.0"));
    return;
  }
  TRACE1("%I64u File Records total\n", volume.GetRecordsCount());

  // Begin searching
  SetWindowText(_T("ntfsundel - Searching ..."));
  GetDlgItem(IDB_SEARCH)->SetWindowText(_T("Stop"));
  GetDlgItem(IDB_RECOVER)->EnableWindow(FALSE);
  GetDlgItem(IDC_DRIVER)->EnableWindow(FALSE);
  GetDlgItem(IDE_FILTER)->EnableWindow(FALSE);

  // Find deleted files (directory excluded)
  stop = FALSE;
  DWORD count = 0;
  for (ULONGLONG i = static_cast<ULONGLONG>(Enum::MftIdx::USER);
       i < volume.GetRecordsCount(); i++)
  {
    FileRecord fr(&volume);

    // Only parse Standard Information and File Name attributes
    fr.SetAttrMask(Mask::FILE_NAME);       // StdInfo will always be parsed
    if (!fr.ParseFileRecord(i)) continue;  // skip to next
    if (!fr.ParseAttrs()) continue;        // skip to next

    // Check if it's deleted and not directory
    if (!fr.IsDeleted()) continue;
    if (fr.IsDirectory()) continue;

    // Check file name
    _TCHAR fn[MAX_PATH];
    if (fr.GetFileName(fn, MAX_PATH) <= 0) continue;
    // Make UpperCase
    CString fns = fn;
    fns.MakeUpper();
    CString filters = m_filter;
    filters.MakeUpper();
    // Change ".*" to "*" at filter string end
    int fl = filters.GetLength();
    if (fl >= 2)
    {
      if (filters.GetAt(fl - 1) == _T('*') && filters.GetAt(fl - 2) == _T('.'))
      {
        filters.SetAt(fl - 2, _T('*'));
        filters = filters.Left(fl - 1);
      }
    }

    if (MatchFileName((const _TCHAR*)fns, (const _TCHAR*)filters))
    {
      // Add to list
      CString s;
      s.Format(_T("%I64u"), i);

      int itm = m_files.InsertItem(count, s);
      // File name
      m_files.SetItemText(itm, 1, fn);
      // Time
      FILETIME ft;
      fr.GetFileTime(&ft);
      SYSTEMTIME st;
      if (!FileTimeToSystemTime(&ft, &st)) memset(&st, 0, sizeof(SYSTEMTIME));
      s.Format(_T("%04d-%02d-%02d  %02d:%02d"), st.wYear, st.wMonth, st.wDay,
               st.wHour, st.wMinute);
      m_files.SetItemText(itm, 2, s);

      // Prevent showing too many entries, 50,000 maxiam
      count++;
      if (count > 50000)
      {
        MessageBox(
            _T("Too many files found, only the first 50,000 will be shown"));
        break;
      }
    }

    if (stop) break;

    if (!PeekAndPump()) break;
  }

  CString totals;
  totals.Format(_T("%d deleted files processed"), count);
  MessageBox(totals);

  GetDlgItem(IDB_SEARCH)->SetWindowText(_T("Search"));
  GetDlgItem(IDB_RECOVER)->EnableWindow(TRUE);
  GetDlgItem(IDC_DRIVER)->EnableWindow(TRUE);
  GetDlgItem(IDE_FILTER)->EnableWindow(TRUE);
  SetWindowText(_T("ntfsundel"));
}

void CNtfsundelDlg::OnRecover()
{
  POSITION pos = m_files.GetFirstSelectedItemPosition();
  if (pos == NULL) return;

  int itm = m_files.GetNextSelectedItem(pos);

  CString refs = m_files.GetItemText(itm, 0);
  ULONGLONG ref = _ttoi64((const _TCHAR*)refs);

  CString fn = m_files.GetItemText(itm, 1);

  CString vns;
  m_driver.GetLBText(m_driver.GetCurSel(), vns);

  _TCHAR volname = vns.GetAt(0);

  NtfsVolume volume(volname);
  FileRecord fr(&volume);

  if (!fr.ParseFileRecord(ref))
  {
    MessageBox(_T("File Record parse error"));
    return;
  }

  fr.SetAttrMask(Mask::DATA);
  if (!fr.ParseAttrs())
  {
    if (fr.IsCompressed())
      MessageBox(_T("Compressed directory not supported yet"));
    else if (fr.IsEncrypted())
      MessageBox(_T("Encrypted directory not supported yet"));
    else
      MessageBox(_T("File Record attribute parse error"));
    return;
  }

  // Save as
  CFileDialog savedlg(FALSE, NULL, (const _TCHAR*)fn);
  if (savedlg.DoModal() == IDOK)
  {
    CString path = savedlg.GetPathName();
    if (path.GetAt(0) == volname)
    {
      if (MessageBox(_T("You should choose a different drive, do you want to ")
                     _T("continue anyway ?"),
                     NULL, MB_OKCANCEL) == IDCANCEL)
        return;
    }

    HANDLE hf = CreateFile((const _TCHAR*)path, GENERIC_READ | GENERIC_WRITE, 0,
                           NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hf == INVALID_HANDLE_VALUE)
    {
      MessageBox(_T("File creation failed"));
      return;
    }

    // Save to disk
    // Unnamed Data attribute contains the file data
    const AttrBase* data = fr.FindStream();
    if (data)
    {
      ULONGLONG datalen = data->GetDataSize();
      ULONGLONG remain = datalen;

      // Check files with huge size (maybe something is error)
      if (datalen > 100 * 1024 * 1024)
      {
        if (MessageBox(
                _T("File size exceeds 100M, do you want to continue anyway ?"),
                NULL, MB_OKCANCEL) == IDCANCEL)
          return;
      }

#define BUFSIZE 64 * 1024  // Read 64K once
      for (ULONGLONG i = 0; i < datalen; i += BUFSIZE)
      {
        BYTE buf[BUFSIZE];

        DWORD len;
        if (data->ReadData(i, buf, BUFSIZE, &len) &&
            (len == BUFSIZE || len == remain))
        {
          // Save data
          DWORD l;
          WriteFile(hf, buf, len, &l, NULL);
          remain -= len;
        }
        else
        {
          MessageBox(_T("Read data error"));
          return;
        }
      }

      CloseHandle(hf);

      CString s;
      s.Format(_T("%I64u bytes recovered"), datalen);
      MessageBox(s);
    }
  }
}

void CNtfsundelDlg::OnSelchangeDriver() { m_files.DeleteAllItems(); }
