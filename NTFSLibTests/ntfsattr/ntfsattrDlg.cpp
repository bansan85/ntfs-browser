// ntfsattrDlg.cpp : implementation file
//

#include "stdafx.h"

#include <array>
#include <string_view>

#include <ntfs-browser/attr-base.h>
#include <ntfs-browser/file-record.h>
#include <ntfs-browser/index-entry.h>
#include <ntfs-browser/mft-idx.h>
#include <ntfs-browser/ntfs-volume.h>

#include "ntfsattr.h"
#include "ntfsattrDlg.h"

using namespace NtfsBrowser;

#ifdef _DEBUG
  #define new DEBUG_NEW
  #undef THIS_FILE
static char THIS_FILE[] = __FILE__;
#endif

class CAboutDlg : public CDialog
{
 public:
  CAboutDlg();

  enum
  {
    IDD = IDD_ABOUTBOX
  };

 protected:
  virtual void DoDataExchange(CDataExchange* pDX);

 protected:
  DECLARE_MESSAGE_MAP()
};

CAboutDlg::CAboutDlg() : CDialog(CAboutDlg::IDD) {}

void CAboutDlg::DoDataExchange(CDataExchange* pDX)
{
  CDialog::DoDataExchange(pDX);
}

BEGIN_MESSAGE_MAP(CAboutDlg, CDialog)
END_MESSAGE_MAP()

CNtfsattrDlg::CNtfsattrDlg(CWnd* pParent /*=nullptr*/)
    : CDialog(CNtfsattrDlg::IDD, pParent),
      m_filename(_T("")),
      m_dump(_T("")),
      m_dir(FALSE),
      // Note that LoadIcon does not require a subsequent DestroyIcon in Win32
      m_hIcon(AfxGetApp()->LoadIcon(IDR_MAINFRAME))
{
}

void CNtfsattrDlg::DoDataExchange(CDataExchange* pDX)
{
  CDialog::DoDataExchange(pDX);
  DDX_Text(pDX, IDC_FILENAME, m_filename);
  DDX_Text(pDX, IDE_DUMP, m_dump);
  DDX_Check(pDX, IDC_DIR, m_dir);
}

BEGIN_MESSAGE_MAP(CNtfsattrDlg, CDialog)
ON_WM_SYSCOMMAND()
ON_WM_PAINT()
ON_WM_QUERYDRAGICON()
END_MESSAGE_MAP()

BOOL CNtfsattrDlg::OnInitDialog()
{
  CDialog::OnInitDialog();

  // Add "About..." menu item to system menu.

  // IDM_ABOUTBOX must be in the system command range.
  ASSERT((IDM_ABOUTBOX & 0xFFF0) == IDM_ABOUTBOX);
  ASSERT(IDM_ABOUTBOX < 0xF000);

  CMenu* pSysMenu = GetSystemMenu(FALSE);
  if (pSysMenu != nullptr)
  {
    CString strAboutMenu;
    strAboutMenu.LoadString(IDS_ABOUTBOX);
    if (!strAboutMenu.IsEmpty())
    {
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

void CNtfsattrDlg::OnSysCommand(UINT nID, LPARAM lParam)
{
  if ((nID & 0xFFF0U) == IDM_ABOUTBOX)
  {
    CAboutDlg dlgAbout;
    dlgAbout.DoModal();
  }
  else
  {
    CDialog::OnSysCommand(nID, lParam);
  }
}

// If you add a minimize button to your dialog, you will need the code below
//  to draw the icon.  For MFC applications using the document/view model,
//  this is automatically done for you by the framework.

void CNtfsattrDlg::OnPaint()
{
  if (IsIconic() != 0)
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
HCURSOR CNtfsattrDlg::OnQueryDragIcon() noexcept
{
  return static_cast<HCURSOR>(m_hIcon);
}

const std::array<const _TCHAR*, kAttrNums> AttrNames = {
    _T("STANDARD_INFORMATION"),
    _T("ATTRIBUTE_LIST"),
    _T("FILE_NAME"),
    _T("OBJECT_ID"),
    _T("SECURITY_DESCRIPTOR"),
    _T("VOLUME_NAME"),
    _T("VOLUME_INFORMATION"),
    _T("DATA"),
    _T("INDEX_ROOT"),
    _T("INDEX_ALLOCATION"),
    _T("BITMAP"),
    _T("REPARSE_POINT"),
    _T("EA_INFORMATION"),
    _T("EA"),
    _T("NULL"),
    _T("LOGGED_UTILITY_STREAM")};

// ugly but work !
void appenddata(CString& lines, const BYTE* data, DWORD datalen)
{
  // "01 02 03 04 05 06 07 08 - 09 0A 0B 0C 0D 0E 0F   123456789ABCDEF";

  CString line;
  DWORD i;

  std::array<BYTE, 16> p;
  for (i = 0; i < ((datalen - 1U) >> 4); i++)
  {
    memcpy(p.data(), data + i * 16, 16);

    line.Format(
        _T("%02X %02X %02X %02X %02X %02X %02X %02X - %02X %02X %02X %02X ")
        _T("%02X %02X %02X %02X   "),
        p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7], p[8], p[9], p[10],
        p[11], p[12], p[13], p[14], p[15]);
    for (int j = 0; j < 16; j++)
    {
      if (p[j] < 0x20)
      {
        p[j] = '.';
      }
    }
    line.Format(_T("%s%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c\r\n"), line, p[0], p[1],
                p[2], p[3], p[4], p[5], p[6], p[7], p[8], p[9], p[10], p[11],
                p[12], p[13], p[14], p[15]);

    lines += line;
  }

  // last line
  memcpy(p.data(), data + i * 16, 16);
  BYTE q[16];
  memset(&q[0], 0xFF, 16);
  memcpy(&q[0], p.data(), 16);
  if ((datalen % 16) == 0)
  {
    line.Format(
        _T("%02X %02X %02X %02X %02X %02X %02X %02X - %02X %02X %02X %02X ")
        _T("%02X %02X %02X %02X   "),
        q[0], q[1], q[2], q[3], q[4], q[5], q[6], q[7], q[8], q[9], q[10],
        q[11], q[12], q[13], q[14], q[15]);
  }
  else
  {
    line.Format(
        _T("%02X %02X %02X %02X %02X %02X %02X %02X                           ")
        _T("  "),
        q[0], q[1], q[2], q[3], q[4], q[5], q[6], q[7]);
  }

  for (int j = 0; j < 16; j++)
  {
    if (q[j] < 0x20)
    {
      q[j] = '.';
    }
  }

  if ((datalen % 16) == 0)
  {
    line.Format(_T("%s%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c\r\n"), line, q[0], q[1],
                q[2], q[3], q[4], q[5], q[6], q[7], q[8], q[9], q[10], q[11],
                q[12], q[13], q[14], q[15]);
  }
  else
  {
    line.Format(_T("%s%c%c%c%c%c%c%c%c\r\n"), line, q[0], q[1], q[2], q[3],
                q[4], q[5], q[6], q[7]);
  }

  lines += line;
}

void printattr(const AttrBase& attr, void* context, bool* /* bStop*/)
{
  CString* dump = static_cast<CString*>(context);

  CString line = _T("\r\n");
  line += AttrNames[ATTR_INDEX(attr.GetAttrType())];

  std::wstring attrname = attr.GetAttrName();
  if (!attrname.empty())
  {
    line += '(';
    line += attrname.c_str();
    line += ')';
  }
  line += _T("\r\n");

  appenddata(line, reinterpret_cast<const BYTE*>(&attr.GetAttrHeader()),
             attr.GetAttrTotalSize());

  *dump += line;
}

void CNtfsattrDlg::OnOK()
{
  CFileDialog fd(TRUE);

  if (fd.DoModal() != IDOK)
  {
    return;
  }

  UpdateData(TRUE);
  m_filename = fd.GetPathName();
  if (m_dir == TRUE)
  {
    m_filename = m_filename.Left(m_filename.ReverseFind(_T('\\')));
  }
  UpdateData(FALSE);

  m_dump.Empty();

  // parse volume

  const _TCHAR volname = m_filename.GetAt(0);

  NtfsVolume volume(volname);
  if (!volume.IsVolumeOK())
  {
    MessageBox(_T("Not a valid NTFS volume or NTFS version < 3.0"));
    return;
  }

  // parse root directory

  FileRecord fr(volume);
  // we only need to parse INDEX_ROOT and INDEX_ALLOCATION
  // don't waste time and ram to parse unwanted attributes
  fr.SetAttrMask(Mask::INDEX_ROOT | Mask::INDEX_ALLOCATION);

  if (!fr.ParseFileRecord(static_cast<ULONGLONG>(Enum::MftIdx::ROOT)))
  {
    MessageBox(_T("Cannot read root directory of volume"));
    return;
  }

  if (!fr.ParseAttrs())
  {
    MessageBox(_T("Cannot parse attributes"));
    return;
  }

  // find subdirectory

  int dirs = m_filename.Find(_T('\\'), 0);
  int dire = m_filename.Find(_T('\\'), dirs + 1);
  while (dire != -1)
  {
    CString pathname = m_filename.Mid(dirs + 1, dire - dirs - 1);

    std::optional<IndexEntry> ie =
        fr.FindSubEntry(static_cast<const _TCHAR*>(pathname));
    if (!ie)
    {
      MessageBox(_T("File not found\n"));
      return;
    }

    if (!fr.ParseFileRecord(ie->GetFileReference()))
    {
      MessageBox(_T("Cannot read root directory of volume"));
      return;
    }

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
        MessageBox(_T("Cannot parse directory attributes"));
      }
      return;
    }

    dirs = dire;
    dire = m_filename.Find(_T('\\'), dirs + 1);
  }

  // dump it !

  CString filename = m_filename.Right(m_filename.GetLength() - dirs - 1);

  // root directory
  if (filename.GetLength() == 2 && (filename.Find(_T(':'), 0) != -1))
  {
    filename = _T('.');
  }

  std::optional<IndexEntry> ie =
      fr.FindSubEntry(static_cast<const _TCHAR*>(filename));
  if (!ie)
  {
    MessageBox(_T("File not found\n"));
    return;
  }

  if (!fr.ParseFileRecord(ie->GetFileReference()))
  {
    MessageBox(_T("Cannot read file"));
    return;
  }

  // parse all attributes
  fr.SetAttrMask(MASK_ALL);
  if (!fr.ParseAttrs())
  {
    if (fr.IsCompressed())
    {
      MessageBox(_T("Compressed file not supported yet"));
    }
    else if (fr.IsEncrypted())
    {
      MessageBox(_T("Encrypted file not supported yet"));
    }
    else
    {
      MessageBox(_T("Cannot parse file attributes"));
    }
    return;
  }

  fr.TraverseAttrs(printattr, &m_dump);
  UpdateData(FALSE);
}
