// Force-included into Crypto++ on the newest MSVC only. Crypto++ 8.9 calls
// stdext::make_checked_array_iterator and friends, which that STL no longer
// has. On raw pointers they are the identity, which is all Crypto++ passes.
#pragma once

// 1950: the first _MSC_VER (Visual Studio "18", toolset v145) whose STL
// dropped stdext::checked_array_iterator. Older toolsets still have it, so
// this workaround only needs to apply from that version on.
#if defined(_MSC_VER) && !defined(__clang__) && _MSC_VER >= 1950

  #include <algorithm>
  #include <cstddef>

namespace stdext
{
template <class T>
T* make_checked_array_iterator(T* ptr, std::size_t /*size*/)
{
  return ptr;
}

template <class T>
T* make_unchecked_array_iterator(T* ptr)
{
  return ptr;
}

template <class It1, class It2>
auto unchecked_mismatch(It1 first1, It1 last1, It2 first2)
{
  return std::mismatch(first1, last1, first2);
}
}  // namespace stdext

#endif
