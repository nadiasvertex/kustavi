# Loads the MSVC (x64) build environment into the current Git Bash session on Windows:
#   source tools/msvc-env.sh
# Afterwards `just build` and friends find cl, link, the Windows SDK and Ninja's inputs.
# Does nothing when cl is already on PATH or when not running under Windows.

if command -v cl >/dev/null 2>&1; then
  return 0 2>/dev/null || exit 0
fi

_vswhere="/c/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe"
if [ ! -x "$_vswhere" ]; then
  echo "msvc-env: vswhere.exe not found; install Visual Studio with the C++ workload" >&2
  return 1 2>/dev/null || exit 1
fi

_vs="$("$_vswhere" -latest -products '*' \
  -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath | tr -d '\r')"
if [ -z "$_vs" ]; then
  echo "msvc-env: no Visual Studio with the C++ x64 tools found" >&2
  return 1 2>/dev/null || exit 1
fi

_vcvars="$(cygpath -w "$_vs")\VC\Auxiliary\Build\vcvars64.bat"
# Run vcvars in cmd, dump the resulting environment, and import it. PATH needs the
# Windows-to-POSIX conversion; other variables are plain strings.
_bat="$(mktemp --suffix=.bat)"
# MSYS drops the ProgramFiles(x86) variable, which vcvars needs to find vswhere.
{
  printf '@set "ProgramFiles(x86)=C:\Program Files (x86)"\r\n'
  printf '@call "%s" >nul\r\n' "$_vcvars"
  printf '@set\r\n'
} > "$_bat"
_dump="$(cmd.exe //d //c "$(cygpath -w "$_bat")" 2>/dev/null | tr -d '\r')"
rm -f "$_bat"
while IFS='=' read -r _k _v; do
  case "$_k" in
    ''|*[!A-Za-z0-9_]*) continue ;;
    [Pp][Aa][Tt][Hh]) export PATH="$(cygpath -u -p "$_v")" ;;
    INCLUDE|LIB|LIBPATH|VCINSTALLDIR|VCToolsInstallDir|VCToolsRedistDir|VSINSTALLDIR|WindowsSdkDir|WindowsSdkVerBinPath|WindowsSDKVersion|UCRTVersion|UniversalCRTSdkDir|VSCMD_ARG_TGT_ARCH|VSCMD_ARG_HOST_ARCH|VisualStudioVersion|DevEnvDir|Platform)
      export "$_k=$_v" ;;
  esac
done <<< "$_dump"
unset _vswhere _vs _vcvars _bat _dump _k _v
command -v cl >/dev/null 2>&1 || { echo "msvc-env: cl still not on PATH" >&2; return 1; }
