# ntfs-browser
C++20 Fast NTFS browser under Windows.



## Based on Code Project [An NTFS Parser Lib](https://www.codeproject.com/Articles/81456/An-NTFS-Parser-Lib)

Project licensed under BSD-3c.

In `documentation/old` folder, there is an archive of the main web page of the project.

This project improves the historical software by:

  - fixing minor bugs,
  - rewriting it with C++20 coding style,
  - caching `ReadFile` in `CAttrNonResident::ReadClusters`.
