//--------------------------------------------------------------------*- C++ -*-
// CLING - the C++ LLVM-based InterpreterG :)
// author: Roman Zulak
//
// This file is dual-licensed: you can choose to license it under the University
// of Illinois Open Source License or the GNU Lesser General Public License. See
// LICENSE.TXT for details.
//------------------------------------------------------------------------------

#include "cling/Utils/Platform.h"

#if defined(LLVM_ON_WIN32)

#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/ConvertUTF.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"

#include <sstream>
#include <stdlib.h>

#ifndef WIN32_LEAN_AND_MEAN
 #define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOGDI
 #define NOGDI
#endif
#ifndef NOMINMAX
  #define NOMINMAX
#endif

#include <Windows.h>
#include <Psapi.h>   // EnumProcessModulesEx
#include <direct.h>  // _getcwd
#include <shlobj.h>  // SHGetFolderPath
#pragma comment(lib, "Advapi32.lib")

#define MAX_PATHC (MAX_PATH + 1)

namespace cling {
namespace utils {
namespace platform {

inline namespace windows {

static void GetErrorAsString(DWORD Err, std::string& ErrStr, const char* Prefix) {
  llvm::raw_string_ostream Strm(ErrStr);
  if (Prefix)
    Strm << Prefix << ": returned " << Err << " ";

  LPTSTR Message = nullptr;
  const DWORD Size = ::FormatMessage(FORMAT_MESSAGE_ALLOCATE_BUFFER |
            FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
            nullptr, Err, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
            (LPTSTR)&Message, 0, nullptr);

  if (Size && Message) {
    Strm << Message;
    ::LocalFree(Message);
    ErrStr = llvm::StringRef(Strm.str()).rtrim().str();
  }
}

static void ReportError(DWORD Err, const char* Prefix) {
  std::string Message;
  GetErrorAsString(Err, Message, Prefix);
  llvm::errs() << Err << '\n';
}

bool GetLastErrorAsString(std::string& ErrStr, const char* Prefix) {
  if (const DWORD Err = ::GetLastError()) {
    GetErrorAsString(Err, ErrStr, Prefix);
    return true;
  }
  return false;
}

bool ReportLastError(const char* Prefix) {
  if (const DWORD Err = ::GetLastError()) {
    ReportError(Err, Prefix);
    return true;
  }
  return false;
}

namespace {

// Taken from clang/lib/Driver/MSVCToolChain.cpp
static bool readFullStringValue(HKEY hkey, const char *valueName,
                                std::string &value) {
  std::wstring WideValueName;
  if (!llvm::ConvertUTF8toWide(valueName, WideValueName))
    return false;

  // First just query for the required size.
  DWORD valueSize = 0, type = 0;
  DWORD result = ::RegQueryValueExW(hkey, WideValueName.c_str(), NULL, &type,
                                    NULL, &valueSize);
  if (result == ERROR_SUCCESS) {
    if (type != REG_SZ || !valueSize)
      return false;

    llvm::SmallVector<wchar_t, MAX_PATHC> buffer;
    buffer.resize(valueSize/sizeof(wchar_t));
    result = ::RegQueryValueExW(hkey, WideValueName.c_str(), NULL, NULL,
                                reinterpret_cast<BYTE*>(&buffer[0]), &valueSize);
    if (result == ERROR_SUCCESS) {
      // String might be null terminated, which we don't want
      while (!buffer.empty() && buffer.back() == 0)
        buffer.pop_back();

      std::wstring WideValue(buffer.data(), buffer.size());
      // The destination buffer must be empty as an invariant of the conversion
      // function; but this function is sometimes called in a loop that passes
      // in the same buffer, however. Simply clear it out so we can overwrite it
      value.clear();
      return llvm::convertWideToUTF8(WideValue, value);
    }
  }

  ReportError(result, "RegQueryValueEx");
  return false;
}

static void logSearch(const char* Name, const std::string& Value,
                      const char* Found = nullptr) {
  if (Found)
    llvm::errs() << "Found " << Name << " '" << Value << "' that matches "
                 << Found << " version\n";
  else
    llvm::errs() << Name << " '" << Value << "' not found.\n";
}

static void trimString(const char* Value, const char* Sub, std::string& Out) {
  const char* End = ::strstr(Value, Sub);
  Out = End ? std::string(Value, End) : Value;
}

static bool getVSRegistryString(const char* Product, int VSVersion,
                                std::string& Path, const char* Verbose) {
  std::ostringstream Key;
  Key << "HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\" << Product << "\\"
      << VSVersion << ".0";

  std::string IDEInstallDir;
  if (!GetSystemRegistryString(Key.str().c_str(), "InstallDir", IDEInstallDir)
      || IDEInstallDir.empty()) {
    if (Verbose)
      logSearch("Registry", Key.str());
    return false;
  }

  trimString(IDEInstallDir.c_str(), "\\Common7\\IDE", Path);
  if (Verbose)
    logSearch("Registry", Key.str(), Verbose);
  return true;
}

static bool getVSEnvironmentString(int VSVersion, std::string& Path,
                                   const char* Verbose) {
  std::ostringstream Key;
  Key << "VS" << VSVersion * 10 << "COMNTOOLS";
  const char* Tools = ::getenv(Key.str().c_str());
  if (!Tools) {
    if (Verbose)
      logSearch("Environment", Key.str());
    return false;
  }

  trimString(Tools, "\\Common7\\Tools", Path);
  if (Verbose)
    logSearch("Environment", Key.str(), Verbose);
  return true;
}

static bool getVisualStudioVer(int VSVersion, std::string& Path,
                               const char* Verbose) {
  if (getVSRegistryString("VisualStudio", VSVersion, Path, Verbose))
    return true;

  if (getVSRegistryString("VCExpress", VSVersion, Path, Verbose))
    return true;

  if (getVSEnvironmentString(VSVersion, Path, Verbose))
    return true;

  return false;
}

// Find the most recent version of Universal CRT or Windows 10 SDK.
// vcvarsqueryregistry.bat from Visual Studio 2015 sorts entries in the include
// directory by name and uses the last one of the list.
// So we compare entry names lexicographically to find the greatest one.
static bool getWindows10SDKVersion(std::string& SDKPath,
                                   std::string& SDKVersion) {
  SDKVersion.clear();

  std::error_code EC;
  llvm::SmallString<MAX_PATHC> IncludePath(SDKPath);
  llvm::sys::path::append(IncludePath, "Include");
  for (llvm::sys::fs::directory_iterator DirIt(IncludePath, EC), DirEnd;
       DirIt != DirEnd && !EC; DirIt.increment(EC)) {
    if (!llvm::sys::fs::is_directory(DirIt->path()))
      continue;
    llvm::StringRef CandidateName = llvm::sys::path::filename(DirIt->path());
    // If WDK is installed, there could be subfolders like "wdf" in the
    // "Include" directory.
    // Allow only directories which names start with "10.".
    if (!CandidateName.startswith("10."))
      continue;
    if (CandidateName > SDKVersion) {
      SDKPath = DirIt->path();
      SDKVersion = CandidateName;
    }
  }
  return !SDKVersion.empty();
}

static bool getUniversalCRTSdkDir(std::string& Path,
                                  std::string& UCRTVersion) {
  // vcvarsqueryregistry.bat for Visual Studio 2015 queries the registry
  // for the specific key "KitsRoot10". So do we.
  if (!GetSystemRegistryString("HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\"
                               "Windows Kits\\Installed Roots", "KitsRoot10",
                               Path))
    return false;

  return getWindows10SDKVersion(Path, UCRTVersion);
}

bool getWindowsSDKDir(std::string& WindowsSDK) {
  return GetSystemRegistryString("HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\"
                                 "Microsoft SDKs\\Windows\\$VERSION",
                                 "InstallationFolder", WindowsSDK);
}

} // anonymous namespace

bool GetSystemRegistryString(const char *keyPath, const char *valueName,
                             std::string& outValue) {
  HKEY hRootKey = NULL;
  const char* subKey = NULL;

  if (::strncmp(keyPath, "HKEY_CLASSES_ROOT\\", 18) == 0) {
    hRootKey = HKEY_CLASSES_ROOT;
    subKey = keyPath + 18;
  } else if (::strncmp(keyPath, "HKEY_USERS\\", 11) == 0) {
    hRootKey = HKEY_USERS;
    subKey = keyPath + 11;
  } else if (::strncmp(keyPath, "HKEY_LOCAL_MACHINE\\", 19) == 0) {
    hRootKey = HKEY_LOCAL_MACHINE;
    subKey = keyPath + 19;
  } else if (::strncmp(keyPath, "HKEY_CURRENT_USER\\", 18) == 0) {
    hRootKey = HKEY_CURRENT_USER;
    subKey = keyPath + 18;
  } else {
    return false;
  }

  long lResult;
  bool returnValue = false;
  HKEY hKey = NULL;

  // If we have a $VERSION placeholder, do the highest-version search.
  if (const char *placeHolder = ::strstr(subKey, "$VERSION")) {
    char bestName[256];
    bestName[0] = '\0';

    const char *keyEnd = placeHolder - 1;
    const char *nextKey = placeHolder;
    // Find end of previous key.
    while ((keyEnd > subKey) && (*keyEnd != '\\'))
      keyEnd--;
    // Find end of key containing $VERSION.
    while (*nextKey && (*nextKey != '\\'))
      nextKey++;
    size_t partialKeyLength = keyEnd - subKey;
    char partialKey[256];
    if (partialKeyLength > sizeof(partialKey))
      partialKeyLength = sizeof(partialKey);
    ::strncpy(partialKey, subKey, partialKeyLength);
    partialKey[partialKeyLength] = '\0';
    HKEY hTopKey = NULL;
    lResult = ::RegOpenKeyExA(hRootKey, partialKey, 0,
                              KEY_READ | KEY_WOW64_32KEY, &hTopKey);
    if (lResult == ERROR_SUCCESS) {
      char keyName[256];
      // int bestIndex = -1;
      double bestValue = 0.0;
      DWORD size = sizeof(keyName) - 1;
      for (DWORD index = 0; ::RegEnumKeyExA(hTopKey, index, keyName, &size, NULL,
                                            NULL, NULL, NULL) == ERROR_SUCCESS;
           index++) {
        const char *sp = keyName;
        while (*sp && !isdigit(*sp))
          sp++;
        if (!*sp)
          continue;
        const char *ep = sp + 1;
        while (*ep && (isdigit(*ep) || (*ep == '.')))
          ep++;
        char numBuf[32];
        ::strncpy(numBuf, sp, sizeof(numBuf) - 1);
        numBuf[sizeof(numBuf) - 1] = '\0';
        double dvalue = ::strtod(numBuf, NULL);
        if (dvalue > bestValue) {
          // Test that InstallDir is indeed there before keeping this index.
          // Open the chosen key path remainder.
          ::strcpy(bestName, keyName);
          // Append rest of key.
          ::strncat(bestName, nextKey, sizeof(bestName) - 1);
          bestName[sizeof(bestName) - 1] = '\0';
          lResult = ::RegOpenKeyExA(hTopKey, bestName, 0,
                                    KEY_READ | KEY_WOW64_32KEY, &hKey);
          if (lResult == ERROR_SUCCESS) {
            if (readFullStringValue(hKey, valueName, outValue)) {
              // bestIndex = (int)index;
              bestValue = dvalue;
              returnValue = true;
            }
            ::RegCloseKey(hKey);
          }
        }
        size = sizeof(keyName) - 1;
      }
      ::RegCloseKey(hTopKey);
    } else
      ReportError(lResult, "RegOpenKeyEx");
  } else {
    lResult = ::RegOpenKeyExA(hRootKey, subKey, 0, KEY_READ | KEY_WOW64_32KEY,
                              &hKey);
    if (lResult == ERROR_SUCCESS) {
      returnValue = readFullStringValue(hKey, valueName, outValue);
      ::RegCloseKey(hKey);
    } else
      ReportError(lResult, "RegOpenKeyEx");
  }
  return returnValue;
}

int GetVisualStudioVersionCompiledWith() {
#if (_MSC_VER >= 1900)
  return 14;
#endif
  return (_MSC_VER / 100) - 6;
}

static void fixupPath(std::string& Path, const char* Append = nullptr) {
  const char kSep = '\\';
  if (Append) {
    if (Path.empty())
      return;
    if (Path.back() != kSep)
      Path.append(1, kSep);
    Path.append(Append);
  }
  else {
    while (!Path.empty() && Path.back() == kSep)
      Path.pop_back();
  }
}

bool GetVisualStudioDirs(std::string& Path, std::string* WinSDK,
                         std::string* UniversalSDK, bool Verbose) {

  if (WinSDK) {
    if (!getWindowsSDKDir(*WinSDK)) {
      WinSDK->clear();
      if (Verbose)
        llvm::errs() << "Could not get Windows SDK path\n";
    } else
      fixupPath(*WinSDK);
  }

  if (UniversalSDK) {
    std::string UCRTVersion;
    if (!getUniversalCRTSdkDir(*UniversalSDK, UCRTVersion)) {
      UniversalSDK->clear();
      if (Verbose)
        llvm::errs() << "Could not get Universal SDK path\n";
    } else
      fixupPath(*UniversalSDK, "ucrt");
  }

  const char* Msg = Verbose ? "compiled" : nullptr;

  // Try for the version compiled with first
  const int VSVersion = GetVisualStudioVersionCompiledWith();
  if (getVisualStudioVer(VSVersion, Path, Msg)) {
    fixupPath(Path);
    return true;
  }

  // Check the environment variables that vsvars32.bat sets.
  // We don't do this first so we can run from other VSStudio shells properly
  if (const char* VCInstall = ::getenv("VCINSTALLDIR")) {
    trimString(VCInstall, "\\VC", Path);
    if (Verbose)
      llvm::errs() << "Using VCINSTALLDIR '" << VCInstall << "'\n";
    return true;
  }

  // Try for any other version we can get
  Msg = Verbose ? "highest" : nullptr;
  const int Versions[] = { 14, 12, 11, 10, 9, 8, 0 };
  for (unsigned i = 0; Versions[i]; ++i) {
    if (Versions[i] != VSVersion && getVisualStudioVer(Versions[i], Path, Msg)) {
      fixupPath(Path);
      return true;
    }
  }
  return false;
}


bool IsDLL(const std::string& Path) {
  bool isDLL = false;
  HANDLE hFile = ::CreateFileA(Path.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
  if (hFile == INVALID_HANDLE_VALUE) {
    ReportLastError("CreateFile");
    return false;
  }
  HANDLE hFileMapping = ::CreateFileMapping(hFile, NULL, PAGE_READONLY, 0, 0,
                                            NULL);
  if (hFileMapping == INVALID_HANDLE_VALUE) {
    ReportLastError("CreateFileMapping");
    ::CloseHandle(hFile);
    return false;
  }

  LPVOID lpFileBase = ::MapViewOfFile(hFileMapping, FILE_MAP_READ, 0, 0, 0);
  if (!lpFileBase) {
    ReportLastError("CreateFileMapping");
    ::CloseHandle(hFileMapping);
    ::CloseHandle(hFile);
    return false;
  }

  PIMAGE_DOS_HEADER pDOSHeader = static_cast<PIMAGE_DOS_HEADER>(lpFileBase);
  if (pDOSHeader->e_magic == IMAGE_DOS_SIGNATURE) {
    PIMAGE_NT_HEADERS pNTHeader = reinterpret_cast<PIMAGE_NT_HEADERS>(
        (PBYTE)lpFileBase + pDOSHeader->e_lfanew);
    if ((pNTHeader->Signature == IMAGE_NT_SIGNATURE) &&
        ((pNTHeader->FileHeader.Characteristics & IMAGE_FILE_DLL)))
      isDLL = true;
  }

  ::UnmapViewOfFile(lpFileBase);
  ::CloseHandle(hFileMapping);
  ::CloseHandle(hFile);
  return isDLL;
}

} // namespace windows

std::string GetCwd() {
  char Buffer[MAX_PATHC];
  if (::_getcwd(Buffer, sizeof(Buffer)))
    return Buffer;

  ::perror("Could not get current working directory");
  return std::string();
}

std::string NormalizePath(const std::string& Path) {
  char Buf[MAX_PATHC];
  if (const char* Result = ::_fullpath(Buf, Path.c_str(), sizeof(Buf)))
    return std::string(Result);

  ReportLastError("_fullpath");
  return std::string();
}

bool IsMemoryValid(const void *P) {
  MEMORY_BASIC_INFORMATION MBI;
  if (::VirtualQuery(P, &MBI, sizeof(MBI)) == 0) {
    ReportLastError("VirtualQuery");
    return false;
  }
  if (MBI.State != MEM_COMMIT)
    return false;
  return true;
}

const void* DLOpen(const std::string& Path, std::string* Err) {
  HMODULE dyLibHandle = ::LoadLibraryExA(Path.c_str(), NULL,
                                         DONT_RESOLVE_DLL_REFERENCES);
  if (!dyLibHandle && Err)
    GetLastErrorAsString(*Err, "LoadLibraryEx");

  return reinterpret_cast<void*>(dyLibHandle);
}

void DLClose(const void* Lib, std::string* Err) {
  if (::FreeLibrary(reinterpret_cast<HMODULE>(const_cast<void*>(Lib))) == 0) {
    if (Err)
      GetLastErrorAsString(*Err, "FreeLibrary");
  }
}

bool GetSystemLibraryPaths(llvm::SmallVectorImpl<std::string>& Paths) {
  char Buf[MAX_PATHC];
  // Generic form of C:\Windows\System32
  HRESULT result = ::SHGetFolderPathA(NULL, CSIDL_FLAG_CREATE | CSIDL_SYSTEM,
                                      NULL, SHGFP_TYPE_CURRENT, Buf);
  if (result != S_OK) {
    ReportError(result, "SHGetFolderPathA");
    return false;
  }
  Paths.push_back(Buf);
  Buf[0] = 0; // Reset Buf.
  
  // Generic form of C:\Windows
  result = ::SHGetFolderPathA(NULL, CSIDL_FLAG_CREATE | CSIDL_WINDOWS,
                              NULL, SHGFP_TYPE_CURRENT, Buf);
  if (result != S_OK) {
    ReportError(result, "SHGetFolderPathA");
    return false;
  }
  Paths.push_back(Buf);
  return true;
}

} // namespace platform
} // namespace utils
} // namespace cling

#endif // LLVM_ON_WIN32