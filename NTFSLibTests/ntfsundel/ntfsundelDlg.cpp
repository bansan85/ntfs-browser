#include "stdafx.h"

#include <regex>

#include "ntfsundel.h"
#include "ntfsundelDlg.h"

#include <ntfs-browser/attr-base.h>
#include <ntfs-browser/file-record.h>
#include <ntfs-browser/mft-idx.h>
#include <ntfs-browser/ntfs-volume.h>

using namespace NtfsBrowser;

#ifdef _DEBUG
  #define new DEBUG_NEW
  #undef THIS_FILE
static char THIS_FILE[] = __FILE__;
#endif

CNtfsundelDlg::CNtfsundelDlg(CWnd* pParent)
    : CDialog(CNtfsundelDlg::IDD, pParent)
{
  m_filter = _T("*.*");
  m_hIcon = AfxGetApp()->LoadIcon(IDR_MAINFRAME);
}

void CNtfsundelDlg::DoDataExchange(CDataExchange* pDX)
{
  CDialog::DoDataExchange(pDX);
  DDX_Control(pDX, IDL_FILES, m_files);
  DDX_Control(pDX, IDC_DRIVER, m_driver);
  DDX_Text(pDX, IDE_FILTER, m_filter);
  DDV_MaxChars(pDX, m_filter, 30);
}

BEGIN_MESSAGE_MAP(CNtfsundelDlg, CDialog)
ON_WM_PAINT()
ON_WM_QUERYDRAGICON()
ON_BN_CLICKED(IDB_SEARCH, OnSearch)
ON_BN_CLICKED(IDB_RECOVER, OnRecover)
ON_CBN_SELCHANGE(IDC_DRIVER, OnSelchangeDriver)
END_MESSAGE_MAP()

BOOL CNtfsundelDlg::OnInitDialog()
{
  CDialog::OnInitDialog();

  SetIcon(m_hIcon, TRUE);   // Set big icon
  SetIcon(m_hIcon, FALSE);  // Set small icon

  m_files.SetExtendedStyle(LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES |
                           LVS_EX_HEADERDRAGDROP);
  m_files.InsertColumn(1, _T("Reference"), LVCFMT_LEFT, 80);
  m_files.InsertColumn(2, _T("Name"), LVCFMT_LEFT, 260);
  m_files.InsertColumn(3, _T("Time"), LVCFMT_LEFT, 130);

  std::array<_TCHAR, 4> drvname = {_T('A'), _T(':'), _T('\\'), _T('\0')};

  DWORD bm = 1;                             // bit mask
  const DWORD drives = GetLogicalDrives();  // available drives bitmap
  for (int i = 0; i < sizeof(drives) * 8; i++)
  {
    if ((drives & bm) != 0)
    {
      UINT dt = GetDriveType(&drvname[0]);
      if (dt == DRIVE_FIXED || dt == DRIVE_REMOVABLE)
      {
        drvname[2] = _T('\0');
        m_driver.InsertString(-1, &drvname[0]);
        drvname[2] = _T('\\');
      }
    }

    drvname[0]++;
    bm <<= 1U;
  }

  if (m_driver.GetCount() > 0)
  {
    m_driver.SetCurSel(0);
  }

  return TRUE;  // return TRUE  unless you set the focus to a control
}

void CNtfsundelDlg::OnPaint()
{
  if (IsIconic() == TRUE)
  {
    CPaintDC dc(this);  // device context for painting

    SendMessage(WM_ICONERASEBKGND, reinterpret_cast<WPARAM>(dc.GetSafeHdc()),
                0);

    // Center icon in client rectangle
    const int cxIcon = GetSystemMetrics(SM_CXICON);
    const int cyIcon = GetSystemMetrics(SM_CYICON);
    CRect rect;
    GetClientRect(&rect);
    const int x = (rect.Width() - cxIcon + 1) / 2;
    const int y = (rect.Height() - cyIcon + 1) / 2;

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
HCURSOR CNtfsundelDlg::OnQueryDragIcon()
{
  return static_cast<HCURSOR>(m_hIcon);
}

bool PeekAndPump()
{
  MSG msg;
  while (PeekMessage(&msg, nullptr, 0, 0, PM_NOREMOVE))
  {
    if (AfxGetApp()->PumpMessage() == FALSE)
    {
      PostQuitMessage(0);
      return false;
    }
  }

  return true;
}

void CNtfsundelDlg::OnSearch()
{
  // Give user a chance to stop the searching loop
  static std::atomic<bool> stop = false;

  CString btntext;
  GetDlgItem(IDB_SEARCH)->GetWindowText(btntext);
  if (btntext == _T("Stop"))
  {
    stop = true;
    return;
  }

  m_files.DeleteAllItems();

  UpdateData(TRUE);

  // Remove leading and trailing space
  if (m_filter.IsEmpty())
  // default to find all deleted files
  {
    m_filter = _T(".*");
  }
  else
  {
    m_filter.TrimLeft();
    m_filter.TrimRight();
  }

  // Volume information
  CString vns;
  const int sel = m_driver.GetCurSel();
  if (sel >= 0)
  {
    m_driver.GetLBText(sel, vns);
  }
  if (vns.IsEmpty())
  {
    MessageBox(_T("Select a disk drive"));
    m_driver.SetFocus();
    return;
  }

  const _TCHAR volname = vns.GetAt(0);

  NtfsVolume volume(volname, FileReader::Strategy::FULL_CACHE);
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
  int count = 0;
  FileRecord fr(volume);

  const auto regx = std::wregex(static_cast<const _TCHAR*>(m_filter));
  for (auto i = static_cast<ULONGLONG>(Enum::MftIdx::USER);
       i < volume.GetRecordsCount(); i++)
  {
    if (stop)
    {
      break;
    }

    if (!PeekAndPump())
    {
      break;
    }

    // Only parse Standard Information and File Name attributes
    // StdInfo will always be parsed
    fr.SetAttrMask(Mask::FILE_NAME);
    if (!fr.ParseFileRecord(i))
    {
      continue;
    }
    if (!fr.ParseAttrs())
    {
      continue;
    }

    // Check if it's deleted and not directory
    if (!fr.IsDeleted())
    {
      continue;
    }
    if (fr.IsDirectory())
    {
      continue;
    }

    // Check file name
    std::wstring fn = fr.GetFileName();
    if (fn.empty())
    {
      continue;
    }

    if (std::regex_match(fn, regx))
    {
      // Add to list
      CString s;
      s.Format(_T("%I64u"), i);

      const int itm = m_files.InsertItem(count, s);
      // File name
      m_files.SetItemText(itm, 1, fn.c_str());
      // Time
      FILETIME ft;
      fr.GetFileTime(&ft, nullptr, nullptr);
      SYSTEMTIME st;
      if (FileTimeToSystemTime(&ft, &st) == FALSE)
      {
        memset(&st, 0, sizeof(SYSTEMTIME));
      }
      s.Format(_T("%04u-%02u-%02u  %02u:%02u"), st.wYear, st.wMonth, st.wDay,
               st.wHour, st.wMinute);
      m_files.SetItemText(itm, 2, s);

      // Prevent showing too many entries, 50,000 maxiam
      count++;
      constexpr DWORD MAX_NUMBER_FILES = 50000;
      if (count >= MAX_NUMBER_FILES)
      {
        MessageBox(
            _T("Too many files found, only the first 50,000 will be shown"));
        break;
      }
    }
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
  if (pos == nullptr)
  {
    return;
  }

  const int itm = m_files.GetNextSelectedItem(pos);

  CString refs = m_files.GetItemText(itm, 0);
  ULONGLONG ref = _ttoi64(static_cast<const _TCHAR*>(refs));

  CString fn = m_files.GetItemText(itm, 1);

  CString vns;
  m_driver.GetLBText(m_driver.GetCurSel(), vns);

  const _TCHAR volname = vns.GetAt(0);

  NtfsVolume volume(volname, FileReader::Strategy::NO_CACHE);
  FileRecord fr(volume);

  if (!fr.ParseFileRecord(ref))
  {
    MessageBox(_T("File Record parse error"));
    return;
  }

  fr.SetAttrMask(Mask::DATA);
  if (!fr.ParseAttrs())
  {
    if (fr.IsCompressed())
    {
      MessageBox(_T("Compressed directory not supported yet"));
    }
    else if (fr.IsEncrypted())
    {
      MessageBox(_T("Encrypted directory not supported yet"));
    }
    else
    {
      MessageBox(_T("File Record attribute parse error"));
    }
    return;
  }

  // Save as
  CFileDialog savedlg(FALSE, nullptr, static_cast<const _TCHAR*>(fn));
  if (savedlg.DoModal() != IDOK)
  {
    return;
  }

  CString path = savedlg.GetPathName();

  if ((path.GetAt(0) == volname) &&
      (MessageBox(_T("You should choose a different drive, do you want to ")
                  _T("continue anyway ?"),
                  nullptr, MB_OKCANCEL) == IDCANCEL))
  {
    return;
  }

  using HandlePtr = std::unique_ptr<std::remove_pointer<HANDLE>::type,
                                    decltype(&::CloseHandle)>;
  HandlePtr hf = HandlePtr(
      CreateFile(static_cast<const _TCHAR*>(path), GENERIC_READ | GENERIC_WRITE,
                 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr),
      &CloseHandle);
  if (hf.get() == INVALID_HANDLE_VALUE)
  {
    MessageBox(_T("File creation failed"));
    return;
  }

  // Save to disk
  // Unnamed Data attribute contains the file data
  const AttrBase* data = fr.FindStream({});
  if (data == nullptr)
  {
    return;
  }

  const ULONGLONG datalen = data->GetDataSize();
  ULONGLONG remain = datalen;

  // Check files with huge size (maybe something is error)
  constexpr ULONGLONG SIZE_CHECK = 100 * 1024 * 1024U;
  if (datalen > SIZE_CHECK)
  {
    if (MessageBox(
            _T("File size exceeds 100M, do you want to continue anyway ?"),
            nullptr, MB_OKCANCEL) == IDCANCEL)
    {
      return;
    }
  }

  // Read 64K once
  constexpr DWORD BUFSIZE = 64 * 1024U;
  for (ULONGLONG i = 0; i < datalen; i += BUFSIZE)
  {
    std::vector<BYTE> vec;
    vec.resize(BUFSIZE, '\0');

    std::optional<ULONGLONG> len = data->ReadData(i, {&vec[0], BUFSIZE});
    if (!len || (*len != BUFSIZE && *len != remain))
    {
      MessageBox(_T("Read data error"));
      return;
    }

    // Save data
    DWORD l = 0;
    WriteFile(hf.get(), &vec[0], static_cast<DWORD>(*len), &l, nullptr);
    remain -= *len;
  }

  CString s;
  s.Format(_T("%I64u bytes recovered"), datalen);
  MessageBox(s);
}

void CNtfsundelDlg::OnSelchangeDriver() { m_files.DeleteAllItems(); }
