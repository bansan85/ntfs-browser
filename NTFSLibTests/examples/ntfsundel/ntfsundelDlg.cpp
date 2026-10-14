#include "StdAfx.h"

#include <array>
#include <atomic>
#include <memory>
#include <vector>

#include <gsl/narrow>

#include <ntfs-browser/attr/base.h>
#include <ntfs-browser/io/file-record.h>
#include <ntfs-browser/ntfs-volume.h>

#include "ntfsundel-core.h"
#include "ntfsundel.h"
#include "ntfsundelDlg.h"

using namespace NtfsBrowser;

#ifdef _DEBUG
  #define new DEBUG_NEW
  #undef THIS_FILE
static char THIS_FILE[] = __FILE__;
#endif

CNtfsundelDlg::CNtfsundelDlg(CWnd* pParent)
    : CDialog(CNtfsundelDlg::IDD, pParent) {
  m_filter = _T(".*");
  m_hIcon = AfxGetApp()->LoadIcon(IDR_MAINFRAME);
}

void CNtfsundelDlg::DoDataExchange(CDataExchange* pDX) {
  CDialog::DoDataExchange(pDX);
  DDX_Control(pDX, IDL_FILES, m_files);
  DDX_Control(pDX, IDC_DRIVER, m_driver);
  DDX_Text(pDX, IDE_FILTER, m_filter);
  DDV_MaxChars(pDX, m_filter, 30);
}

BEGIN_MESSAGE_MAP(CNtfsundelDlg, CDialog)
ON_WM_PAINT()
ON_WM_QUERYDRAGICON()
ON_BN_CLICKED(IDB_SEARCH, &CNtfsundelDlg::OnSearch)
ON_BN_CLICKED(IDB_RECOVER, &CNtfsundelDlg::OnRecover)
ON_CBN_SELCHANGE(IDC_DRIVER, &CNtfsundelDlg::OnSelchangeDriver)
END_MESSAGE_MAP()

BOOL CNtfsundelDlg::OnInitDialog() {
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
  for (size_t i = 0; i < sizeof(drives) * 8; i++) {
    if ((drives & bm) != 0) {
      UINT dt = GetDriveType(&drvname[0]);
      if (dt == DRIVE_FIXED || dt == DRIVE_REMOVABLE) {
        drvname[2] = _T('\0');
        m_driver.InsertString(-1, &drvname[0]);
        drvname[2] = _T('\\');
      }
    }

    drvname[0]++;
    bm <<= 1U;
  }

  if (m_driver.GetCount() > 0) {
    m_driver.SetCurSel(0);
  }

  return TRUE;  // return TRUE  unless you set the focus to a control
}

void CNtfsundelDlg::OnPaint() {
  if (IsIconic() == TRUE) {
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
  } else {
    CDialog::OnPaint();
  }
}

// The system calls this to obtain the cursor to display while the user drags
//  the minimized window.
HCURSOR CNtfsundelDlg::OnQueryDragIcon() {
  return static_cast<HCURSOR>(m_hIcon);
}

bool PeekAndPump() {
  MSG msg;
  while (PeekMessage(&msg, nullptr, 0, 0, PM_NOREMOVE)) {
    if (AfxGetApp()->PumpMessage() == FALSE) {
      PostQuitMessage(0);
      return false;
    }
  }

  return true;
}

void CNtfsundelDlg::OnSearch() {
  // Give user a chance to stop the searching loop
  static std::atomic<bool> stop = false;

  CString btntext;
  GetDlgItem(IDB_SEARCH)->GetWindowText(btntext);
  if (btntext == _T("Stop")) {
    stop = true;
    return;
  }

  m_files.DeleteAllItems();

  UpdateData(TRUE);

  // Volume information
  CString vns;
  const int sel = m_driver.GetCurSel();
  if (sel >= 0) {
    m_driver.GetLBText(sel, vns);
  }
  if (vns.IsEmpty()) {
    MessageBox(_T("Select a disk drive"));
    m_driver.SetFocus();
    return;
  }

  const _TCHAR volname = vns.GetAt(0);

  // A deleted-file finder needs to see freed records: without this, every
  // one of them would be invisible from the moment it's parsed.
  NtfsVolume<Cache::Strategy::FullCache> volume(volname,
                                                {.include_deleted = true});
  if (!volume.IsVolumeOK()) {
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

  stop = FALSE;
  const NtfsUndel::SearchControl control{
      .stop_requested = [] { return static_cast<bool>(stop); },
      .pump_messages = [] { return PeekAndPump(); }};
  const std::wstring filter = static_cast<const _TCHAR*>(m_filter);
  const std::optional<NtfsUndel::SearchResult> result =
      NtfsUndel::SearchDeletedFiles(volume, filter, control);
  if (!result) {
    MessageBox(_T("Invalid filter expression"));
    GetDlgItem(IDB_SEARCH)->SetWindowText(_T("Search"));
    GetDlgItem(IDB_RECOVER)->EnableWindow(TRUE);
    GetDlgItem(IDC_DRIVER)->EnableWindow(TRUE);
    GetDlgItem(IDE_FILTER)->EnableWindow(TRUE);
    SetWindowText(_T("ntfsundel"));
    return;
  }

  for (const NtfsUndel::DeletedFile& file : result->files) {
    CString s;
    s.Format(_T("%I64u"), file.reference);
    const int itm = m_files.InsertItem(m_files.GetItemCount(), s);
    m_files.SetItemText(itm, 1, file.full_path.c_str());
    m_files.SetItemText(itm, 2,
                        NtfsUndel::FormatWriteTime(file.write_time).c_str());
  }
  if (result->truncated) {
    MessageBox(_T("Too many files found, only the first 50,000 will be shown"));
  }

  CString totals;
  totals.Format(_T("%lldus"), result->scan_us);
  MessageBox(totals);

  GetDlgItem(IDB_SEARCH)->SetWindowText(_T("Search"));
  GetDlgItem(IDB_RECOVER)->EnableWindow(TRUE);
  GetDlgItem(IDC_DRIVER)->EnableWindow(TRUE);
  GetDlgItem(IDE_FILTER)->EnableWindow(TRUE);
  SetWindowText(_T("ntfsundel"));
}

void CNtfsundelDlg::OnRecover() {
  POSITION pos = m_files.GetFirstSelectedItemPosition();
  if (pos == nullptr) {
    return;
  }

  const int itm = m_files.GetNextSelectedItem(pos);

  CString refs = m_files.GetItemText(itm, 0);
  ULONGLONG ref = _ttoi64(static_cast<const _TCHAR*>(refs));

  CString fn = m_files.GetItemText(itm, 1);

  CString vns;
  m_driver.GetLBText(m_driver.GetCurSel(), vns);

  const _TCHAR volname = vns.GetAt(0);

  // The selected file came from OnSearch()'s deleted-inclusive listing: it
  // must still be visible here, or recovery could never find its record.
  NtfsVolume<Cache::Strategy::NoCache> volume(volname,
                                              {.include_deleted = true});
  NtfsUndel::OpenedRecord opened = NtfsUndel::OpenDeletedFile(volume, ref);
  if (opened.error == NtfsUndel::RecoverError::RecordParse) {
    MessageBox(_T("File Record parse error"));
    return;
  }
  if (opened.error == NtfsUndel::RecoverError::AttributeParse) {
    MessageBox(_T("File Record attribute parse error"));
    return;
  }

  // Save as
  CFileDialog savedlg(FALSE, nullptr, static_cast<const _TCHAR*>(fn));
  if (savedlg.DoModal() != IDOK) {
    return;
  }

  CString path = savedlg.GetPathName();

  if ((path.GetAt(0) == volname) &&
      (MessageBox(_T("You should choose a different drive, do you want to ")
                  _T("continue anyway ?"),
                  nullptr, MB_OKCANCEL) == IDCANCEL)) {
    return;
  }

  using HandlePtr = std::unique_ptr<std::remove_pointer<HANDLE>::type,
                                    decltype(&::CloseHandle)>;
  HandlePtr hf = HandlePtr(
      CreateFile(static_cast<const _TCHAR*>(path), GENERIC_READ | GENERIC_WRITE,
                 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr),
      &CloseHandle);
  if (hf.get() == INVALID_HANDLE_VALUE) {
    MessageBox(_T("File creation failed"));
    return;
  }

  const NtfsUndel::CopyResult copied = NtfsUndel::CopyUnnamedData(
      *opened.record,
      [this](std::uint64_t) {
        return MessageBox(
                   _T("File size exceeds 100M, do you want to continue anyway ")
                   _T("?"),
                   nullptr, MB_OKCANCEL) != IDCANCEL;
      },
      [&hf](std::span<const BYTE> chunk) {
        // A failed or partial write must not be reported as success.
        DWORD written = 0;
        return WriteFile(hf.get(), chunk.data(),
                         gsl::narrow<DWORD>(chunk.size()), &written,
                         nullptr) != FALSE &&
               written == chunk.size();
      });
  switch (copied.error) {
    case NtfsUndel::RecoverError::NoDataStream:
    case NtfsUndel::RecoverError::Declined:
      return;
    case NtfsUndel::RecoverError::Read:
      MessageBox(_T("Read data error"));
      return;
    case NtfsUndel::RecoverError::Write:
      MessageBox(_T("Write data error"));
      return;
    default:
      break;
  }

  CString s;
  s.Format(_T("%I64u bytes recovered"), copied.bytes);
  MessageBox(s);
}

void CNtfsundelDlg::OnSelchangeDriver() { m_files.DeleteAllItems(); }
