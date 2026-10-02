#!/usr/bin/env bash
# Sanitizer build matrix for ntfs-browser (clang 23, Ubuntu 24.04 / WSL).
#
# usage: san-builds.sh [configure|build|test|all] [name...]
#   no name = every build but tysan. Names: asan asan-shared asan-ptr ubsan tsan msan cfi scudo hwasan nsan
#   tysan (-fsanitize=type) is experimental and noisy on C++ with the STL (std::array and frozen
#   tables, structs overlaid on byte buffers): it only runs when named, eg. san-builds.sh all tysan
#
# msan needs a libc++ built with -DLLVM_USE_SANITIZER=MemoryWithOrigins (libstdc++ and
# the packaged libc++ are not instrumented and give false positives). The first msan
# configure builds only libc++ and libc++abi, not LLVM, into $LIBCXX_MSAN.
# The sources must be a checkout inside WSL (Unix line endings, not a Windows checkout:
# CRLF breaks llvm/cmake/config.guess), at a tag matching clang 23.1, e.g.
#   git clone --depth 1 --branch llvmorg-23.1.0 https://github.com/llvm/llvm-project ~/llvm-project
#
# env overrides:
#   REPO         source tree          (default ~/ntfs-browser)
#   ROOT         build + log root     (default ~/ntfs-san-builds, keep it on ext4)
#   LLVM_SRC     llvm-project checkout, msan only (default ~/llvm-project)
#   LIBCXX_MSAN  install dir of the MSan-instrumented libc++ (default $ROOT/libcxx-msan)
#   JOBS         parallel jobs        (default nproc)
#
# prerequisites: sudo apt install clang-23 lld-23 llvm-23 libclang-rt-23-dev ninja-build cmake

set -uo pipefail

REPO=${REPO:-$HOME/ntfs-browser}
ROOT=${ROOT:-$HOME/ntfs-san-builds}
LLVM_SRC=${LLVM_SRC:-$HOME/llvm-project}
LIBCXX_MSAN=${LIBCXX_MSAN:-$ROOT/libcxx-msan}
JOBS=${JOBS:-$(nproc)}

IGNORELIST=$ROOT/ubsan-3rdparty.ignore
ASAN_IGNORELIST=$ROOT/asan-3rdparty.ignore
LOGS=$ROOT/logs
ALL_NAMES=(asan asan-shared asan-ptr ubsan tsan msan cfi scudo hwasan nsan)

# -O1 keeps stack traces readable; TySan needs optimisation to emit TBAA.
BASE_FLAGS="-O1 -g -fno-omit-frame-pointer -fno-optimize-sibling-calls"

ASAN_FLAGS="-fsanitize=address,undefined,float-divide-by-zero,local-bounds,vptr -fsanitize-address-use-after-return=always -fno-sanitize-recover=all -D_GLIBCXX_ASSERTIONS -D_GLIBCXX_SANITIZE_STD_ALLOCATOR -D_GLIBCXX_SANITIZE_VECTOR"

# asan-ptr adds pointer-compare/pointer-subtract. Three things keep it free of false positives:
# - -O0: at -O1 InstCombine rewrites integer compares into pointer compares against bogus
#   constants (0xfffffffffffffff4), which show up as invalid-pointer-pair in code that never
#   compares pointers.
# - no container annotations: _GLIBCXX_SANITIZE_VECTOR poisons a vector's spare capacity, so
#   end_of_storage - finish (libstdc++) is reported as a pair of unrelated objects.
# - the ignorelist below: std::less<T*> is the one sanctioned cross-object pointer comparison
#   (string::_M_disjunct, shared_ptr::owner_before), but libstdc++ builds it on a plain '<'.
ASAN_PTR_FLAGS="-O0 -U_GLIBCXX_SANITIZE_VECTOR -U_GLIBCXX_SANITIZE_STD_ALLOCATOR -fsanitize=pointer-compare,pointer-subtract -fsanitize-ignorelist=$ASAN_IGNORELIST"

mkdir -p "$ROOT" "$LOGS"
# -fsanitize=integer flags the unsigned wrap-around that libstdc++ and the 3rdparty code rely on.
cat >"$IGNORELIST" <<'EOF'
src:*/3rdparty/*
src:*/include/c++/*
EOF
cat >"$ASAN_IGNORELIST" <<'EOF'
src:*/bits/stl_function.h
EOF

export ASAN_SYMBOLIZER_PATH=/usr/bin/llvm-symbolizer-23
export ASAN_OPTIONS=detect_stack_use_after_return=1:strict_string_checks=1:detect_invalid_pointer_pairs=2:check_initialization_order=1:strict_init_order=1:detect_odr_violation=2:alloc_dealloc_mismatch=1:detect_leaks=1:halt_on_error=1
export UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1
export TSAN_OPTIONS=halt_on_error=1:second_deadlock_stack=1
export MSAN_OPTIONS=halt_on_error=1
export TYSAN_OPTIONS=print_stacktrace=1
export HWASAN_OPTIONS=halt_on_error=1

# Sets F (sanitizer flags) and EXTRA (extra cmake args) for a build name.
select_build() {
  F=""
  LF=""
  EXTRA=()
  case $1 in
    asan) F=$ASAN_FLAGS ;;
    asan-shared)
      F=$ASAN_FLAGS
      EXTRA=(-DBUILD_SHARED_LIBS=ON)
      ;;
    asan-ptr) F="$ASAN_FLAGS $ASAN_PTR_FLAGS" ;;
    ubsan)
      F="-fsanitize=undefined,float-divide-by-zero,local-bounds,vptr,integer,implicit-conversion -fno-sanitize-recover=all -fsanitize-ignorelist=$IGNORELIST -D_GLIBCXX_DEBUG"
      ;;
    tsan) F="-fsanitize=thread" ;;
    msan)
      F="-fsanitize=memory -fsanitize-memory-track-origins=2 -fsanitize-memory-use-after-dtor -fno-sanitize-recover=all -stdlib=libc++ -nostdinc++ -isystem $LIBCXX_MSAN/include/c++/v1"
      LF="$F -L$LIBCXX_MSAN/lib -Wl,-rpath,$LIBCXX_MSAN/lib -lc++abi"
      # Crypto++ asm is not instrumented: it would only produce false positives.
      EXTRA=(-DCRYPTOPP_DISABLE_ASM=ON)
      ;;
    tysan) F="-fsanitize=type" ;;
    cfi)
      F="-flto=thin -fvisibility=hidden -fsanitize=cfi,cfi-cast-strict,safe-stack -fno-sanitize-trap=cfi"
      EXTRA=(-DBUILD_SHARED_LIBS=OFF)
      ;;
    scudo) F="-fsanitize=scudo" ;;
    hwasan) F="-fsanitize=hwaddress -fsanitize-hwaddress-experimental-aliasing" ;;
    nsan) F="-fsanitize=numerical" ;;
    *)
      echo "unknown build: $1" >&2
      return 1
      ;;
  esac
}

# Builds and installs the MSan-instrumented libc++ into $LIBCXX_MSAN (once).
ensure_libcxx_msan() {
  [ -d "$LIBCXX_MSAN/include/c++/v1" ] && return 0
  if [ ! -d "$LLVM_SRC/runtimes" ]; then
    echo "msan: no llvm-project checkout at $LLVM_SRC (set LLVM_SRC)" >&2
    return 1
  fi
  cmake -S "$LLVM_SRC/runtimes" -B "$ROOT/libcxx-msan-build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER=clang-23 -DCMAKE_CXX_COMPILER=clang++-23 \
    -DLLVM_HOST_TRIPLE=x86_64-unknown-linux-gnu -DCMAKE_INSTALL_PREFIX="$LIBCXX_MSAN" \
    -DLLVM_ENABLE_RUNTIMES="libcxx;libcxxabi" -DLLVM_USE_SANITIZER=MemoryWithOrigins \
    -DLLVM_INCLUDE_TESTS=OFF -DLIBCXX_INCLUDE_TESTS=OFF -DLIBCXX_INCLUDE_BENCHMARKS=OFF \
    -DLIBCXXABI_INCLUDE_TESTS=OFF -DLIBCXXABI_USE_LLVM_UNWINDER=OFF || return 1
  ninja -C "$ROOT/libcxx-msan-build" install-cxx install-cxxabi install-cxx-headers || return 1
  [ -d "$LIBCXX_MSAN/include/c++/v1" ]
}

do_configure() {
  local n=$1
  [ "$n" = msan ] && { ensure_libcxx_msan || return 1; }
  select_build "$n" || return 1
  cmake -S "$REPO" -B "$ROOT/$n" -G Ninja -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_C_COMPILER=clang-23 -DCMAKE_CXX_COMPILER=clang++-23 \
    -DCMAKE_AR=/usr/bin/llvm-ar-23 -DCMAKE_AS=/usr/bin/llvm-as-23 -DCMAKE_RANLIB=/usr/bin/llvm-ranlib-23 \
    -DCMAKE_LINKER_TYPE=LLD -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    -DNTFS_BROWSER_ENABLE_TESTING=ON -DNTFS_BROWSER_ENABLE_DECOMPRESSION=ON \
    -DNTFS_BROWSER_ENABLE_EFS_CRYPTOPP=ON -DNTFS_BROWSER_ENABLE_EFS_BCRYPT=ON \
    -DCMAKE_C_FLAGS="$BASE_FLAGS $F" -DCMAKE_CXX_FLAGS="$BASE_FLAGS $F" \
    -DCMAKE_EXE_LINKER_FLAGS="${LF:-$F}" -DCMAKE_SHARED_LINKER_FLAGS="${LF:-$F}" "${EXTRA[@]}"
}

do_build() { cmake --build "$ROOT/$1" --parallel "$JOBS"; }

do_test() { ctest --test-dir "$ROOT/$1" --output-on-failure -j "$JOBS"; }

# Runs one step for one build, logging to $LOGS and recording the result.
run_step() {
  local step=$1 n=$2 log="$LOGS/$2.$1.log"
  echo "=== $n: $step (log: $log)"
  if "do_$step" "$n" >"$log" 2>&1; then
    RESULTS+=("OK    $n $step")
  else
    RESULTS+=("FAIL  $n $step  -> $log")
    return 1
  fi
}

STEP=${1:-all}
shift || true
NAMES=("$@")
[ ${#NAMES[@]} -eq 0 ] && NAMES=("${ALL_NAMES[@]}")

case $STEP in configure | build | test | all) ;; *)
  echo "usage: $0 [configure|build|test|all] [name...]" >&2
  exit 2
  ;;
esac

RESULTS=()
for n in "${NAMES[@]}"; do
  if [ "$STEP" = all ]; then
    # A failed step skips the later ones for that build only.
    run_step configure "$n" && run_step build "$n" && run_step test "$n"
  else
    run_step "$STEP" "$n"
  fi
done

echo
echo "=== summary"
printf '%s\n' "${RESULTS[@]}"
