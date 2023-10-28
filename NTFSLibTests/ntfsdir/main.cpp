// list files and irectories
// usage: ntfsdir "path"
// eg. ntfsdir c:\windows
// eg. ntfsdir "c:\program files\common files"

#include <cstdio>

#include <ntfs-browser/attr-base.h>
#include <ntfs-browser/file-record.h>
#include <ntfs-browser/index-entry.h>
#include <ntfs-browser/mft-idx.h>
#include <ntfs-browser/ntfs-volume.h>

using namespace NtfsBrowser;

void usage()
{
  printf("Invalid parameter\n");
  printf("Usage: ntfsdir \"path\"\n");
  printf("eg. ntfsdir c:\n");
  printf("eg. ntfsdir c:\\windows\n");
  printf("eg. ntfsdir \"c:\\program files\\common files\"\n");
}

// get volume name 'C', 'D', ...
// *ppath -> "c:\program files\common files"
char getvolume(char** ppath)
{
  char* p = *ppath;

  // skip leading blank and "
  while (*p)
  {
    if (*p == ' ' || *p == '"')
    {
      p++;
    }
    else
    {
      break;
    }
  }
  if (*p == '\0')
  {
    return '\0';
  }

  const char volname = *p;
  p++;

  // skip blank
  while (*p)
  {
    if (*p == ' ')
    {
      p++;
    }
    else
    {
      break;
    }
  }
  if (*p == '\0')
  {
    return '\0';
  }

  if (*p != ':')
  {
    return '\0';
  }

  // forward to '\' or string end
  while (*p)
  {
    if (*p != '\\')
    {
      p++;
    }
    else
    {
      break;
    }
  }
  // forward to not '\' and not ", or string end
  while (*p)
  {
    if (*p == '\\' || *p == '"')
    {
      p++;
    }
    else
    {
      break;
    }
  }

  *ppath = p;
  return volname;
}

// get sub directory name
// *ppath -> "program files\common files"
std::wstring getpathname(std::wstring& ppath)
{
  std::wstring pathname;
  int len = 0;
  const wchar_t* p = ppath.c_str();

  // copy until '\' or " or string ends or buffer full
  while (*p && len < ppath.length())
  {
    pathname.append(1, *p);
    len++;
    p++;

    if (*p == '\\' || *p == '\"')
    {
      break;
    }
  }

  // forward to not '\' and not ", or string end
  while (*p)
  {
    if (*p == '\\' || *p == '\"')
    {
      p++;
    }
    else
    {
      break;
    }
  }

  ppath = ppath.substr(p - ppath.c_str());
  return pathname;
}

struct Total
{
  int files = 0;
  int dirs = 0;
};

void printfile(const IndexEntry& ie, void* context)
{
  Total& total = *static_cast<Total*>(context);
  // Hide system metafiles
  if (ie.GetFileReference() < static_cast<ULONGLONG>(Enum::MftIdx::USER))
  {
    return;
  }

  // Ignore DOS alias file names
  if (!ie.IsWin32Name())
  {
    return;
  }

  FILETIME ft;
  std::wstring_view fn = ie.GetFilename();
  if (fn.empty())
  {
    return;
  }

  ie.GetFileTime(&ft, nullptr, nullptr);
  SYSTEMTIME st;
  if (FileTimeToSystemTime(&ft, &st) == TRUE)
  {
    printf("%u-%02u-%02u  %02u:%02u\t%s    ", st.wYear, st.wMonth, st.wDay,
           st.wHour, st.wMinute, ie.IsDirectory() ? "<DIR>" : "     ");

    if (!ie.IsDirectory())
    {
      printf("%I64u\t", ie.GetFileSize());
    }
    else
    {
      printf("\t");
    }

    printf("<%c%c%c>\t%.*ls\n", ie.IsReadOnly() ? 'R' : ' ',
           ie.IsHidden() ? 'H' : ' ', ie.IsSystem() ? 'S' : ' ', fn.size(),
           fn.data());
  }

  if (ie.IsDirectory())
  {
    total.dirs++;
  }
  else
  {
    total.files++;
  }
}

int main(int argc, char* argv[])
{
  if (argc != 2)
  {
    usage();
    return -1;
  }

  char* path = argv[1];

  const char volname = getvolume(&path);
  if (volname == '\0')
  {
    usage();
    return -1;
  }

  NtfsVolume volume(volname, FileReader::Strategy::NO_CACHE);
  if (!volume.IsVolumeOK())
  {
    printf("Cannot get NTFS BPB from boot sector of volume %c\n", volname);
    return -1;
  }

  // get root directory info
  FileRecord fr(volume);

  // we only need INDEX_ROOT and INDEX_ALLOCATION
  // don't waste time and ram to parse unwanted attributes
  fr.SetAttrMask(Mask::INDEX_ROOT | Mask::INDEX_ALLOCATION);

  if (!fr.ParseFileRecord(static_cast<ULONGLONG>(Enum::MftIdx::ROOT)))
  {
    printf("Cannot read root directory of volume %c\n", volname);
    return -1;
  }

  if (!fr.ParseAttrs())
  {
    printf("Cannot parse attributes\n");
    return -1;
  }

  // find subdirectory
  std::wstring wpath(strlen(path) + 1, 0);
  size_t outSize = 0;
  const errno_t err =
      mbstowcs_s(&outSize, wpath.data(), strlen(path) + 1, path, strlen(path));

  if (err != 0)
  {
    printf("Cannot parse requested path\n");
    return -1;
  }
  std::wstring pathname;

  while (true)
  {
    pathname = getpathname(wpath);
    // no subdirectories
    if (pathname.empty())
    {
      break;
    }

    std::optional<IndexEntry> ie = fr.FindSubEntry(pathname.c_str());
    if (!ie)
    {
      printf("Cannot find directory %ls\n", pathname.c_str());
      return -1;
    }

    if (!ie->IsDirectory())
    {
      printf("%ls is not a directory\n", pathname.c_str());
      return -1;
    }

    if (!fr.ParseFileRecord(ie->GetFileReference()))
    {
      printf("Cannot read directory %ls\n", pathname.c_str());
      return -1;
    }
    if (!fr.ParseAttrs())
    {
      printf("Cannot parse attributes\n");
      return -1;
    }
  }

  // list it !

  Total total;
  fr.TraverseSubEntries(printfile, &total);

  printf("Files: %d, Directories: %d\n", total.files, total.dirs);

  return 0;
}
