// list files and directories
// usage: ntfsdir "path"
// eg. ntfsdir c:\windows
// eg. ntfsdir "c:\program files\common files"

#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/attr-base.h>
#include <ntfs-browser/mft-idx.h>
#include <ntfs-browser/file-record.h>
#include <ntfs-browser/index-entry.h>

using namespace NtfsBrowser;

#include <stdio.h>

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
  char volname;

  // skip leading blank and "
  while (*p)
  {
    if (*p == ' ' || *p == '"')
      p++;
    else
      break;
  }
  if (*p == '\0')
    return '\0';
  else
  {
    volname = *p;
    p++;
  }

  // skip blank
  while (*p)
  {
    if (*p == ' ')
      p++;
    else
      break;
  }
  if (*p == '\0') return '\0';

  if (*p != ':') return '\0';

  // forward to '\' or string end
  while (*p)
  {
    if (*p != '\\')
      p++;
    else
      break;
  }
  // forward to not '\' and not ", or string end
  while (*p)
  {
    if (*p == '\\' || *p == '"')
      p++;
    else
      break;
  }

  *ppath = p;
  return volname;
}

// get sub directory name
// *ppath -> "program files\common files"
int getpathname(char** ppath, char* pathname)
{
  int len = 0;
  char* p = *ppath;

  // copy until '\' or " or string ends or buffer full
  while (*p && len < MAX_PATH)
  {
    pathname[len] = *p;
    len++;
    p++;

    if (*p == '\\' || *p == '\"') break;
  }
  pathname[len] = '\0';

  // forward to not '\' and not ", or string end
  while (*p)
  {
    if (*p == '\\' || *p == '\"')
      p++;
    else
      break;
  }

  *ppath = p;
  return len;
}

int totalfiles = 0;
int totaldirs = 0;

void printfile(const IndexEntry* ie)
{
  // Hide system metafiles
  if (ie->GetFileReference() < static_cast<ULONGLONG>(Enum::MftIdx::USER))
    return;

  // Ignore DOS alias file names
  if (!ie->IsWin32Name()) return;

  FILETIME ft;
  char fn[MAX_PATH];
  int fnlen = ie->GetFilename(fn, MAX_PATH);
  if (fnlen > 0)
  {
    ie->GetFileTime(&ft);
    SYSTEMTIME st;
    if (FileTimeToSystemTime(&ft, &st))
    {
      printf("%d-%02d-%02d  %02d:%02d\t%s    ", st.wYear, st.wMonth, st.wDay,
             st.wHour, st.wMinute, ie->IsDirectory() ? "<DIR>" : "     ");

      if (!ie->IsDirectory())
        printf("%I64u\t", ie->GetFileSize());
      else
        printf("\t");

      printf("<%c%c%c>\t%s\n", ie->IsReadOnly() ? 'R' : ' ',
             ie->IsHidden() ? 'H' : ' ', ie->IsSystem() ? 'S' : ' ', fn);
    }

    if (ie->IsDirectory())
      totaldirs++;
    else
      totalfiles++;
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

  char volname;
  volname = getvolume(&path);
  if (!volname)
  {
    usage();
    return -1;
  }

  NtfsVolume volume(volname);
  if (!volume.IsVolumeOK())
  {
    printf("Cannot get NTFS BPB from boot sector of volume %c\n", volname);
    return -1;
  }

  // get root directory info

  FileRecord fr(&volume);

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

  char pathname[MAX_PATH];
  int pathlen;

  while (1)
  {
    pathlen = getpathname(&path, pathname);
    if (pathlen < 0)  // parameter syntax error
    {
      usage();
      return -1;
    }
    if (pathlen == 0) break;  // no subdirectories

    IndexEntry ie;
    if (fr.FindSubEntry(pathname, ie))
    {
      if (ie.IsDirectory())
      {
        if (!fr.ParseFileRecord(ie.GetFileReference()))
        {
          printf("Cannot read directory %s\n", pathname);
          return -1;
        }
        if (!fr.ParseAttrs())
        {
          printf("Cannot parse attributes\n");
          return -1;
        }
      }
      else
      {
        printf("%s is not a directory\n", pathname);
        return -1;
      }
    }
    else
    {
      printf("Cannot find directory %s\n", pathname);
      return -1;
    }
  }

  // list it !

  fr.TraverseSubEntries(printfile);

  printf("Files: %d, Directories: %d\n", totalfiles, totaldirs);

  return 0;
}
