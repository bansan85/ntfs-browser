#include "StdAfx.h"

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>

#include "ntfsdump-core.h"
#include "ntfsdump.h"
#include "ntfsdumpDlg.h"

using namespace NTFS_BROWSER_NAMESPACE;

#ifdef _DEBUG
  #define new DEBUG_NEW
  #undef THIS_FILE
static char THIS_FILE[] = __FILE__;
#endif

class CAboutDlg : public CDialog {
 public:
  CAboutDlg();

  enum : std::uint8_t { IDD = IDD_ABOUTBOX };

 protected:
  void DoDataExchange(CDataExchange* pDX) override;

 protected:
  DECLARE_MESSAGE_MAP()
};

CAboutDlg::CAboutDlg() : CDialog(CAboutDlg::IDD) {}

void CAboutDlg::DoDataExchange(CDataExchange* pDX) {
  CDialog::DoDataExchange(pDX);
}

BEGIN_MESSAGE_MAP(CAboutDlg, CDialog)
END_MESSAGE_MAP()

CNtfsdumpDlg::CNtfsdumpDlg(CWnd* pParent)
    : CDialog(CNtfsdumpDlg::IDD, pParent),
      m_filename(_T("")),
      m_dump(_T("")),
      m_hIcon(AfxGetApp()->LoadIcon(IDR_MAINFRAME)) {}

void CNtfsdumpDlg::DoDataExchange(CDataExchange* pDX) {
  CDialog::DoDataExchange(pDX);
  DDX_Text(pDX, IDC_FILENAME, m_filename);
  DDX_Text(pDX, IDE_DUMP, m_dump);
}

BEGIN_MESSAGE_MAP(CNtfsdumpDlg, CDialog)
ON_WM_SYSCOMMAND()
ON_WM_PAINT()
ON_WM_QUERYDRAGICON()
END_MESSAGE_MAP()

BOOL CNtfsdumpDlg::OnInitDialog() {
  CDialog::OnInitDialog();

  // Add "About..." menu item to system menu.

  // IDM_ABOUTBOX must be in the system command range.
  ASSERT((IDM_ABOUTBOX & 0xFFF0U) == IDM_ABOUTBOX);
  ASSERT(IDM_ABOUTBOX < 0xF000);

  CMenu* pSysMenu = GetSystemMenu(FALSE);
  if (pSysMenu != nullptr) {
    CString strAboutMenu;
    strAboutMenu.LoadString(IDS_ABOUTBOX);
    if (!strAboutMenu.IsEmpty()) {
      pSysMenu->AppendMenu(MF_SEPARATOR);
      pSysMenu->AppendMenu(MF_STRING, IDM_ABOUTBOX, strAboutMenu);
    }
  }

  // Set the icon for this dialog.  The framework does this automatically
  //  when the application's main window is not a dialog
  SetIcon(m_hIcon, TRUE);   // Set big icon
  SetIcon(m_hIcon, FALSE);  // Set small icon

  return TRUE;  // return TRUE  unless you set the focus to a control
}

void CNtfsdumpDlg::OnSysCommand(UINT nID, LPARAM lParam) {
  if ((nID & 0xFFF0U) == IDM_ABOUTBOX) {
    CAboutDlg dlgAbout;
    dlgAbout.DoModal();
  } else {
    CDialog::OnSysCommand(nID, lParam);
  }
}

void CNtfsdumpDlg::OnPaint() {
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
HCURSOR CNtfsdumpDlg::OnQueryDragIcon() { return (HCURSOR)m_hIcon; }

void CNtfsdumpDlg::OnOK() {
  CFileDialog fd(TRUE);

  if (fd.DoModal() != IDOK) {
    return;
  }

  m_filename = fd.GetPathName();
  m_dump.Empty();
  UpdateData(FALSE);

  // The drive letter of the path names the volume to open.
  NTFS_BROWSER_NAMESPACE::NtfsVolume<
      NTFS_BROWSER_NAMESPACE::Cache::Strategy::NoCache>
      volume(m_filename.GetAt(0));
  const NtfsDump::DumpResult result =
      NtfsDump::DumpFile(volume, m_filename.GetString());
  if (!result.message.empty()) {
    MessageBox(result.message.c_str());
    return;
  }

  m_dump = result.dump.c_str();
  UpdateData(FALSE);
}
