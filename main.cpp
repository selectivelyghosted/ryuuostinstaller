#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <bcrypt.h>
#include <intrin.h>
#include <shellapi.h>
#include <shlobj.h>
#include <tlhelp32.h>
#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")

namespace fs = std::filesystem;
using Bytes = std::vector<uint8_t>;

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

namespace cfg {
constexpr const char *kRyuuManifestUrl =
    "https://generator.ryuu.lol/api/ryuuost_manifest";
constexpr const char *kVanillaManifestUrl =
    "https://generator.ryuu.lol/api/ost_manifest";
constexpr const char *kHookManifestUrl =
    "https://generator.ryuu.lol/api/hook_manifest";
constexpr const char *kPrimaryHost = "https://generator.ryuu.lol";
constexpr const char *kFallbackHost = "https://gen.ryuu.lol";

constexpr const wchar_t *kUserAgent = L"";
constexpr const wchar_t *kToolKeyHeader =
    L"X-Tool-Key: \r\n";

constexpr size_t kMaxManifestBytes = 0x100000;  // 1 MiB
constexpr size_t kMaxDownloadBytes = 0x2000000; // 32 MiB

constexpr const char *kMainDll = "OpenSteamTool.dll";
constexpr const char *kCloudRedirectDll = "cloud_redirect.dll";
constexpr const char *kManagedFiles[] = {"dwmapi.dll", "OpenSteamTool.dll",
                                         "xinput1_4.dll"};

constexpr const wchar_t *kSteamProcesses[] = {
    L"steam.exe", L"steamwebhelper.exe", L"steamerrorreporter.exe",
    L"GameOverlayUI.exe"};

constexpr const char *kCacheDirName = "RyuuOpenSteamTool";
constexpr const char *kCachedExeName = "ryuuostinstaller.exe";
constexpr const char *kMarkerFile = "ryuuost.txt";
constexpr const char *kMarkerContent = "ryuuopensteamtool";

constexpr const char *kDefaultManifestLua =
    "function fetch_manifest_code(gid)\n"
    "    local body, st = http_get(\"https://manifest.opensteamtool.com/\" .. "
    "gid)\n"
    "    if st == 200 and body and body:match(\"^%d+$\") then return body end\n"
    "    body, st = http_get(\"http://gmrc.wudrm.com/manifest/\" .. gid)\n"
    "    if st == 200 and body and body:match(\"^%d+$\") then return body end\n"
    "    body, st = http_get(\"https://manifest.steam.run/api/manifest/\" .. "
    "gid)\n"
    "    if st == 200 and body then\n"
    "        local code = body:match('\"content\":\"(%d+)\"')\n"
    "        if code then return code end\n"
    "    end\n"
    "    return nil\n"
    "end\n";

constexpr const char *kDefaultToml = "[manifest]\n"
                                     "url = \"auto\"\n"
                                     "\n"
                                     "[activation]\n"
                                     "enabled = true\n"
                                     "port = 27182\n"
                                     "api_base = \"https://drm.ryuu.lol\"\n"
                                     "cors_origin = \"https://drm.ryuu.lol\"\n"
                                     "\n"
                                     "[update]\n"
                                     "auto = true\n"
                                     "check_on_start = true\n";
} // namespace cfg

// ---------------------------------------------------------------------------
// Options / logging
// ---------------------------------------------------------------------------

struct Options {
  bool silent =
      false; // --silent      : no console, no prompts, log to update.log
  bool wait =
      false; // --wait        : wait for Steam to exit instead of killing it
  bool noRestart = false; // --no-restart  : don't relaunch Steam at the end
  bool staging =
      false; // --staging     : prefer files from the local staging folder
};

static Options g_opts;
static std::ofstream g_log;

static Options parseArgs(int argc, char **argv) {
  Options o;
  for (int i = 1; i < argc; ++i) {
    std::string_view a = argv[i];
    if (a == "--silent")
      o.silent = true;
    else if (a == "--wait")
      o.wait = true;
    else if (a == "--no-restart")
      o.noRestart = true;
    else if (a == "--staging")
      o.staging = true;
  }
  return o;
}

static void logInfo(const std::string &msg) {
  if (!g_opts.silent)
    std::cout << msg << '\n';
  if (g_log.is_open())
    g_log << msg << '\n' << std::flush;
}

static void logError(const std::string &msg) {
  if (!g_opts.silent)
    std::cerr << msg << '\n';
  if (g_log.is_open())
    g_log << msg << '\n' << std::flush;
}

static void pressEnterToExit() {
  if (g_opts.silent)
    return;
  logInfo("Press Enter to exit.");
  std::cin.get();
}

static bool promptYesNo(const char *question, bool defaultYes) {
  if (g_opts.silent)
    return true;
  std::cout << question << (defaultYes ? " [Y/n] " : " [y/N] ") << std::flush;

  bool answer = defaultYes;
  std::string line;
  if (std::getline(std::cin, line)) {
    size_t i = line.find_first_not_of(" \t");
    if (i != std::string::npos) {
      char c =
          static_cast<char>(std::tolower(static_cast<unsigned char>(line[i])));
      if (c == 'y')
        answer = true;
      else if (c == 'n')
        answer = false;
    }
  }
  return answer;
}

// Opens <steam>\opensteamtool\update.log for appending (used in --silent mode).
static bool openLogFile(const fs::path &steam) {
  fs::path dir = steam / "opensteamtool";
  std::error_code ec;
  fs::create_directories(dir, ec);
  g_log.open(dir / "update.log", std::ios::out | std::ios::app);
  return g_log.is_open();
}

// ---------------------------------------------------------------------------
// Small string helpers
// ---------------------------------------------------------------------------

static std::string toLower(std::string s) {
  for (char &c : s)
    if (c >= 'A' && c <= 'Z')
      c += 32;
  return s;
}

static bool startsWith(std::string_view s, std::string_view prefix) {
  return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

// ---------------------------------------------------------------------------
// Steam location
// ---------------------------------------------------------------------------

static std::optional<std::wstring>
readRegString(HKEY root, const wchar_t *subkey, const wchar_t *value) {
  HKEY key = nullptr;
  if (RegOpenKeyExW(root, subkey, 0, KEY_READ, &key) != ERROR_SUCCESS)
    return std::nullopt;

  wchar_t data[MAX_PATH + 1] = {};
  DWORD cb = sizeof(data) - sizeof(wchar_t);
  DWORD type = 0;
  std::optional<std::wstring> out;
  if (RegQueryValueExW(key, value, nullptr, &type,
                       reinterpret_cast<LPBYTE>(data), &cb) == ERROR_SUCCESS &&
      type == REG_SZ)
    out = std::wstring(data);
  RegCloseKey(key);
  return out;
}

static std::optional<fs::path> findSteamPath() {
  // The original bails out if the HKCU key can't be opened at all, and only
  // falls back to HKLM InstallPath when HKCU SteamPath is missing/empty.
  HKEY probe = nullptr;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", 0, KEY_READ,
                    &probe) != ERROR_SUCCESS)
    return std::nullopt;
  RegCloseKey(probe);

  std::wstring raw =
      readRegString(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath")
          .value_or(L"");
  if (raw.empty()) {
    if (auto v = readRegString(HKEY_LOCAL_MACHINE,
                               L"SOFTWARE\\WOW6432Node\\Valve\\Steam",
                               L"InstallPath"))
      raw = *v;
  }
  if (raw.empty())
    return std::nullopt;

  std::error_code ec;
  fs::path p = fs::weakly_canonical(fs::path(raw), ec);
  if (ec)
    return std::nullopt;
  if (!fs::is_directory(p, ec))
    return std::nullopt;
  return p;
}

// ---------------------------------------------------------------------------
// HTTP (WinHTTP)
// ---------------------------------------------------------------------------

struct ParsedUrl {
  std::wstring host;
  std::wstring path;
  INTERNET_PORT port = 0;
  bool secure = false;
};

static std::optional<ParsedUrl> parseUrl(const std::string &url) {
  ParsedUrl u;
  std::string rest;
  if (startsWith(url, "https://")) {
    u.secure = true;
    u.port = 443;
    rest = url.substr(8);
  } else if (startsWith(url, "http://")) {
    u.port = 80;
    rest = url.substr(7);
  } else {
    return std::nullopt;
  }

  size_t slash = rest.find('/');
  std::string hostPort = rest.substr(0, slash);
  std::string path = slash == std::string::npos ? "/" : rest.substr(slash);

  size_t colon = hostPort.find(':');
  if (colon != std::string::npos) {
    u.port = static_cast<INTERNET_PORT>(std::stoul(hostPort.substr(colon + 1)));
    hostPort.resize(colon);
  }
  if (hostPort.empty())
    return std::nullopt;

  u.host.assign(hostPort.begin(), hostPort.end());
  u.path.assign(path.begin(), path.end());
  return u;
}

static std::optional<Bytes> httpGetOnce(const std::string &url,
                                        size_t maxBytes) {
  auto u = parseUrl(url);
  if (!u)
    return std::nullopt;

  HINTERNET session =
      WinHttpOpen(cfg::kUserAgent, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!session)
    return std::nullopt;
  WinHttpSetTimeouts(session, 8000, 8000, 15000, 120000);

  HINTERNET connect = WinHttpConnect(session, u->host.c_str(), u->port, 0);
  if (!connect) {
    WinHttpCloseHandle(session);
    return std::nullopt;
  }

  HINTERNET request = WinHttpOpenRequest(
      connect, L"GET", u->path.c_str(), nullptr, WINHTTP_NO_REFERER,
      WINHTTP_DEFAULT_ACCEPT_TYPES, u->secure ? WINHTTP_FLAG_SECURE : 0);
  if (!request) {
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return std::nullopt;
  }

  auto closeAll = [&] {
    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
  };

  if (!WinHttpSendRequest(request, cfg::kToolKeyHeader, static_cast<DWORD>(-1),
                          WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
      !WinHttpReceiveResponse(request, nullptr)) {
    closeAll();
    return std::nullopt;
  }

  DWORD status = 0, statusLen = sizeof(status);
  WinHttpQueryHeaders(request,
                      WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                      WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusLen,
                      WINHTTP_NO_HEADER_INDEX);
  if (status < 200 || status > 299) {
    closeAll();
    return std::nullopt;
  }

  Bytes body;
  DWORD avail = 0;
  if (WinHttpQueryDataAvailable(request, &avail)) {
    while (avail != 0) {
      if (body.size() + avail > maxBytes) { // response too large
        closeAll();
        return std::nullopt;
      }
      size_t old = body.size();
      body.resize(old + avail);
      DWORD read = 0;
      if (!WinHttpReadData(request, body.data() + old, avail, &read)) {
        body.resize(old);
        break;
      }
      body.resize(old + read);
      if (!WinHttpQueryDataAvailable(request, &avail))
        break;
    }
  }
  closeAll();

  if (body.empty())
    return std::nullopt;
  return body;
}

// generator.ryuu.lol/... -> gen.ryuu.lol/...  (empty if url isn't on the
// primary host)
static std::string fallbackUrl(const std::string &url) {
  if (!startsWith(url, cfg::kPrimaryHost))
    return {};
  return std::string(cfg::kFallbackHost) +
         url.substr(std::strlen(cfg::kPrimaryHost));
}

static std::optional<Bytes> httpGet(const std::string &url, size_t maxBytes) {
  if (auto r = httpGetOnce(url, maxBytes))
    return r;
  std::string alt = fallbackUrl(url);
  if (alt.empty())
    return std::nullopt;
  return httpGetOnce(alt, maxBytes);
}

// ---------------------------------------------------------------------------
// Hashing (CNG)
// ---------------------------------------------------------------------------

static std::optional<std::string> bcryptHashHex(const wchar_t *alg,
                                                ULONG digestLen,
                                                const uint8_t *data, size_t len,
                                                const char *hexDigits) {
  BCRYPT_ALG_HANDLE hAlg = nullptr;
  BCRYPT_HASH_HANDLE hHash = nullptr;
  if (BCryptOpenAlgorithmProvider(&hAlg, alg, nullptr, 0) != 0)
    return std::nullopt;

  DWORD objLen = 0, cb = 0;
  if (BCryptGetProperty(hAlg, BCRYPT_OBJECT_LENGTH,
                        reinterpret_cast<PUCHAR>(&objLen), sizeof(objLen), &cb,
                        0) != 0) {
    BCryptCloseAlgorithmProvider(hAlg, 0);
    return std::nullopt;
  }

  std::vector<UCHAR> obj(objLen);
  std::optional<std::string> result;
  if (BCryptCreateHash(hAlg, &hHash, obj.data(), objLen, nullptr, 0, 0) == 0) {
    UCHAR digest[32] = {};
    if (BCryptHashData(hHash, const_cast<PUCHAR>(data), static_cast<ULONG>(len),
                       0) == 0 &&
        BCryptFinishHash(hHash, digest, digestLen, 0) == 0) {
      std::string hex;
      hex.reserve(digestLen * 2);
      for (ULONG i = 0; i < digestLen; ++i) {
        hex += hexDigits[digest[i] >> 4];
        hex += hexDigits[digest[i] & 0xF];
      }
      result = std::move(hex);
    }
    BCryptDestroyHash(hHash);
  }
  BCryptCloseAlgorithmProvider(hAlg, 0);
  return result;
}

static std::optional<std::string> sha256Hex(const Bytes &data) {
  return bcryptHashHex(BCRYPT_SHA256_ALGORITHM, 32, data.data(), data.size(),
                       "0123456789abcdef");
}

static std::optional<std::string> md5HexUpper(const Bytes &data) {
  return bcryptHashHex(BCRYPT_MD5_ALGORITHM, 16, data.data(), data.size(),
                       "0123456789ABCDEF");
}

// ---------------------------------------------------------------------------
// File helpers
// ---------------------------------------------------------------------------

static std::optional<Bytes> readFile(const fs::path &p) {
  std::ifstream in(p, std::ios::in | std::ios::binary);
  if (!in)
    return std::nullopt;
  return Bytes(std::istreambuf_iterator<char>(in),
               std::istreambuf_iterator<char>());
}

static std::optional<std::string> sha256File(const fs::path &p) {
  auto data = readFile(p);
  if (!data)
    return std::nullopt;
  return sha256Hex(*data);
}

// Writes to <path>.new, then swaps it into place (rename, or copy if rename
// fails).
static bool writeFileAtomic(const fs::path &target, const Bytes &data) {
  fs::path tmp = target;
  tmp += L".new";
  {
    std::ofstream out(tmp, std::ios::out | std::ios::trunc | std::ios::binary);
    if (!out)
      return false;
    out.write(reinterpret_cast<const char *>(data.data()),
              static_cast<std::streamsize>(data.size()));
    if (!out)
      return false;
  }

  std::error_code ec;
  fs::remove(target, ec);
  fs::rename(tmp, target, ec);
  if (ec) {
    fs::copy_file(tmp, target, fs::copy_options::overwrite_existing, ec);
    fs::remove(tmp, ec);
  }
  return fs::exists(target, ec);
}

static bool writeTextFile(const fs::path &p, const char *text) {
  std::ofstream out(p, std::ios::out | std::ios::trunc | std::ios::binary);
  if (!out)
    return false;
  out << text;
  return true;
}

// ---------------------------------------------------------------------------
// Manifest (minimal string-scan JSON reader, as in the original)
// ---------------------------------------------------------------------------

struct ManifestEntry {
  std::string name;
  std::string url;
  std::string sha256; // lowercase hex
  uint64_t size = 0;
};

// Finds "key" and returns the next "quoted" value after the ':'.
static std::optional<std::string> jsonStringField(const std::string &obj,
                                                  const std::string &key) {
  size_t k = obj.find("\"" + key + "\"");
  if (k == std::string::npos)
    return std::nullopt;
  size_t colon = obj.find(':', k + key.size() + 2);
  if (colon == std::string::npos)
    return std::nullopt;
  size_t q1 = obj.find('"', colon + 1);
  if (q1 == std::string::npos)
    return std::nullopt;
  size_t q2 = obj.find('"', q1 + 1);
  if (q2 == std::string::npos)
    return std::nullopt;
  return obj.substr(q1 + 1, q2 - q1 - 1);
}

static std::optional<uint64_t> jsonUIntField(const std::string &obj,
                                             const std::string &key) {
  size_t k = obj.find("\"" + key + "\"");
  if (k == std::string::npos)
    return std::nullopt;
  size_t colon = obj.find(':', k + key.size() + 2);
  if (colon == std::string::npos)
    return std::nullopt;
  try {
    return std::stoull(obj.substr(colon + 1));
  } catch (...) {
    return std::nullopt;
  }
}

// Locates the object for `name` (from "name" up to the next '}') and pulls
// url/sha256/size.
static std::optional<ManifestEntry> findManifestEntry(const std::string &json,
                                                      const char *name) {
  size_t start = json.find("\"" + std::string(name) + "\"");
  if (start == std::string::npos)
    return std::nullopt;
  size_t end = json.find('}', start);
  std::string obj = json.substr(
      start, end == std::string::npos ? std::string::npos : end - start);

  ManifestEntry e;
  e.name = name;
  e.url = jsonStringField(obj, "url").value_or("");
  e.sha256 = toLower(jsonStringField(obj, "sha256").value_or(""));
  e.size = jsonUIntField(obj, "size").value_or(0);

  if (e.url.empty() || e.sha256.size() != 64)
    return std::nullopt;
  return e;
}

static std::string bytesToString(const Bytes &b) {
  return std::string(b.begin(), b.end());
}

// Expected sha256 of OpenSteamTool.dll according to a given manifest endpoint.
static std::optional<std::string> fetchMainDllHash(const char *manifestUrl) {
  auto body = httpGet(manifestUrl, cfg::kMaxManifestBytes);
  if (!body)
    return std::nullopt;
  auto e = findManifestEntry(bytesToString(*body), cfg::kMainDll);
  if (!e)
    return std::nullopt;
  return e->sha256;
}

// ---------------------------------------------------------------------------
// opensteamtool.toml: is [cloud] enabled = true?
// ---------------------------------------------------------------------------

static bool cloudRedirectWanted(const fs::path &steam) {
  std::error_code ec;
  if (fs::is_regular_file(steam / cfg::kCloudRedirectDll, ec))
    return true;

  std::ifstream in(steam / "opensteamtool.toml");
  if (!in)
    return false;

  bool inCloud = false;
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    size_t i = line.find_first_not_of(" \t");
    std::string t = i == std::string::npos ? std::string() : line.substr(i);
    if (t.empty() || t[0] == '#')
      continue;

    if (t.front() == '[' && t.back() == ']') {
      inCloud = toLower(t) == "[cloud]";
      continue;
    }
    if (!inCloud)
      continue;

    // First "enabled = ..." line inside [cloud] decides; other keys are
    // skipped.
    std::string lower = toLower(t);
    if (!startsWith(lower, "enabled"))
      continue;
    size_t eq = lower.find('=');
    if (eq == std::string::npos)
      continue;
    std::string v = lower.substr(eq + 1);
    size_t j = v.find_first_not_of(" \t");
    v = j == std::string::npos ? std::string() : v.substr(j);
    return startsWith(v, "true"); // original compares only the first 4 chars
  }
  return false;
}

// Fetches the Ryuu manifest. All three core files must be present; the cloud
// redirect hook is appended when the user has it enabled.
static std::optional<std::vector<ManifestEntry>>
fetchManifest(const fs::path &steam) {
  auto body = httpGet(cfg::kRyuuManifestUrl, cfg::kMaxManifestBytes);
  if (!body)
    return std::nullopt;
  std::string json = bytesToString(*body);

  std::vector<ManifestEntry> entries;
  for (const char *name : cfg::kManagedFiles)
    if (auto e = findManifestEntry(json, name))
      entries.push_back(std::move(*e));

  if (entries.size() != std::size(cfg::kManagedFiles))
    return std::nullopt;

  if (cloudRedirectWanted(steam)) {
    if (auto hook = httpGet(cfg::kHookManifestUrl, cfg::kMaxManifestBytes)) {
      std::string hj = bytesToString(*hook);
      size_t pos = hj.find("\"cloudredirect\"");
      if (pos != std::string::npos)
        if (auto e = findManifestEntry(hj.substr(pos), cfg::kCloudRedirectDll))
          entries.push_back(std::move(*e));
    }
  }
  return entries;
}

// ---------------------------------------------------------------------------
// Installed build detection
// ---------------------------------------------------------------------------

enum class InstalledBuild { None = 0, Vanilla = 1, Ryuu = 2, Other = 3 };

static InstalledBuild detectInstalledBuild(const fs::path &steam) {
  auto local = sha256File(steam / cfg::kMainDll);
  if (!local || local->empty())
    return InstalledBuild::None;
  std::string localHash = toLower(*local);

  auto ryuu = fetchMainDllHash(cfg::kRyuuManifestUrl);
  auto vanilla = fetchMainDllHash(cfg::kVanillaManifestUrl);

  if (ryuu && *ryuu == localHash)
    return InstalledBuild::Ryuu;
  if (vanilla && *vanilla == localHash)
    return InstalledBuild::Vanilla;

  // Older Ryuu build: hash doesn't match current manifest, but marker is
  // present.
  std::error_code ec;
  if (fs::is_regular_file(steam / cfg::kMarkerFile, ec))
    return InstalledBuild::Ryuu;
  return InstalledBuild::Other;
}

// ---------------------------------------------------------------------------
// Processes
// ---------------------------------------------------------------------------

static bool isProcessRunning(const wchar_t *exe) {
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snap == INVALID_HANDLE_VALUE)
    return false;
  PROCESSENTRY32W pe{};
  pe.dwSize = sizeof(pe);
  bool found = false;
  if (Process32FirstW(snap, &pe)) {
    do {
      if (_wcsicmp(pe.szExeFile, exe) == 0) {
        found = true;
        break;
      }
    } while (Process32NextW(snap, &pe));
  }
  CloseHandle(snap);
  return found;
}

// Terminates every instance of `exe`. Returns true if at least one was killed.
static bool killProcess(const wchar_t *exe) {
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snap == INVALID_HANDLE_VALUE)
    return false;
  PROCESSENTRY32W pe{};
  pe.dwSize = sizeof(pe);
  bool killed = false;
  if (Process32FirstW(snap, &pe)) {
    do {
      if (_wcsicmp(pe.szExeFile, exe) == 0) {
        if (HANDLE h = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE,
                                   pe.th32ProcessID)) {
          TerminateProcess(h, 0);
          WaitForSingleObject(h, 5000);
          CloseHandle(h);
          killed = true;
        }
      }
    } while (Process32NextW(snap, &pe));
  }
  CloseHandle(snap);
  return killed;
}

static void stopSteam() {
  if (g_opts.wait) {
    bool anyRunning = false;
    for (auto *p : cfg::kSteamProcesses)
      if (isProcessRunning(p)) {
        anyRunning = true;
        break;
      }
    if (!anyRunning)
      return;

    logInfo("Waiting for Steam to exit...");
    while (isProcessRunning(L"steam.exe"))
      Sleep(500);
    Sleep(1000);
    return;
  }

  for (int attempt = 0; attempt < 3; ++attempt) {
    bool killedAny = false;
    for (auto *p : cfg::kSteamProcesses)
      if (killProcess(p))
        killedAny = true;
    if (!killedAny)
      break;
    Sleep(400);
  }
  Sleep(500);
}

static bool startSteam(const fs::path &steam) {
  fs::path exe = steam / "steam.exe";
  std::error_code ec;
  if (!fs::exists(exe, ec))
    return false;
  auto r = reinterpret_cast<INT_PTR>(ShellExecuteW(
      nullptr, L"open", exe.c_str(), nullptr, steam.c_str(), SW_SHOWDEFAULT));
  return r > 32;
}

// ---------------------------------------------------------------------------
// SteamTools detection / removal
// ---------------------------------------------------------------------------

// Reflected CRC-64 variant used for the SteamTools machine id.
static uint64_t crc64Lower(const std::string &s) {
  constexpr uint64_t kPoly = 0x85E1C3D753D46D27ULL;
  if (s.empty() || s[0] == '\0')
    return 0;
  uint64_t crc = ~0ULL;
  for (char ch : s) {
    if (ch == '\0')
      break;
    unsigned char c = static_cast<unsigned char>(ch);
    if (c >= 'A' && c <= 'Z')
      c += 32;
    crc ^= c;
    for (int i = 0; i < 8; ++i) {
      if (crc & 1)
        crc ^= kPoly;
      crc >>= 1;
    }
  }
  return ~crc;
}

// Reproduces SteamTools' per-machine id:
//   CRC64(lower(MD5_HEX(xor("V<vendor>_F<family>_M<model>_C<cpus>",
//   "version"))))
static std::optional<std::string> steamToolsMachineId() {
  int regs[4];
  __cpuid(regs, 0);
  char vendor[13] = {};
  std::memcpy(vendor + 0, &regs[1], 4); // ebx
  std::memcpy(vendor + 4, &regs[3], 4); // edx
  std::memcpy(vendor + 8, &regs[2], 4); // ecx

  __cpuid(regs, 1);
  unsigned eax = static_cast<unsigned>(regs[0]);

  SYSTEM_INFO si = {};
  GetNativeSystemInfo(&si);

  char buf[128] = {};
  int n = std::snprintf(
      buf, sizeof(buf), "V%s_F%X_M%X_C%X", vendor, (eax >> 8) & 0xF,
      (eax & 0xFF) >> 4,
      static_cast<unsigned>(static_cast<uint8_t>(si.dwNumberOfProcessors)));
  if (n <= 0)
    return std::nullopt;

  static constexpr char kKey[] = "version";
  Bytes x(n);
  for (int i = 0; i < n; ++i)
    x[i] = static_cast<uint8_t>(buf[i] ^ kKey[i % 7]);

  auto md5 = md5HexUpper(x);
  if (!md5)
    return std::nullopt;

  char out[32];
  std::snprintf(out, sizeof(out), "%016llX",
                static_cast<unsigned long long>(crc64Lower(*md5)));
  return std::string(out);
}

static std::vector<fs::path> steamToolsMarkers(const fs::path &steam,
                                               const std::string &id) {
  if (id.empty())
    return {};
  fs::path base = steam / "appcache" / "httpcache";
  return {base / "3a" / id, base / "3b" / id};
}

static bool steamCfgSaysSteamTools(const fs::path &steam) {
  std::ifstream in(steam / "Steam.cfg");
  if (!in)
    return false;
  std::string line;
  std::getline(in, line);
  while (!line.empty() &&
         (line.back() == '\r' || line.back() == '\n' || line.back() == ' '))
    line.pop_back();
  return line == "1";
}

static bool isSteamToolsInstalled(const fs::path &steam) {
  HKEY key = nullptr;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Valve\\Steamtools", 0,
                    KEY_READ, &key) == ERROR_SUCCESS) {
    RegCloseKey(key);
    return true;
  }
  if (steamCfgSaysSteamTools(steam))
    return true;

  std::error_code ec;
  if (fs::is_regular_file(steam / "appcache" / "version", ec))
    return true;

  auto id = steamToolsMachineId();
  if (!id)
    return false;
  for (const auto &p : steamToolsMarkers(steam, *id))
    if (fs::is_regular_file(p, ec))
      return true;
  return false;
}

static void uninstallSteamTools(const fs::path &steam) {
  if (!isSteamToolsInstalled(steam) && !isProcessRunning(L"steamtools.exe"))
    return;

  logInfo("Uninstalling SteamTools...");
  killProcess(L"steamtools.exe");

  std::error_code ec;
  for (const fs::path &p :
       {steam / "Steam.cfg", steam / "appcache" / "version"})
    if (fs::is_regular_file(p, ec))
      fs::remove(p, ec);

  if (auto id = steamToolsMachineId())
    for (const auto &p : steamToolsMarkers(steam, *id))
      if (fs::is_regular_file(p, ec))
        fs::remove(p, ec);

  HKEY key = nullptr;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Valve\\Steamtools", 0,
                    KEY_READ, &key) == ERROR_SUCCESS) {
    RegCloseKey(key);
    RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\Valve\\Steamtools");
  }
}

// ---------------------------------------------------------------------------
// %LOCALAPPDATA%\RyuuOpenSteamTool
// ---------------------------------------------------------------------------

static fs::path localAppDataDir() {
  wchar_t buf[MAX_PATH] = {};
  if (FAILED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, buf)))
    return {};
  return fs::path(buf) / cfg::kCacheDirName;
}

static fs::path stagingDir() {
  fs::path base = localAppDataDir();
  return base.empty() ? fs::path() : base / "staging";
}

static void clearStaging() {
  fs::path dir = stagingDir();
  std::error_code ec;
  if (dir.empty() || !fs::exists(dir, ec))
    return;
  for (const auto &entry : fs::directory_iterator(dir, ec))
    if (entry.is_regular_file(ec))
      fs::remove(entry.path(), ec);
}

// Copies the running exe to
// %LOCALAPPDATA%\RyuuOpenSteamTool\ryuuostinstaller.exe
static bool cacheInstaller() {
  wchar_t self[MAX_PATH] = {};
  if (GetModuleFileNameW(nullptr, self, MAX_PATH) == 0)
    return false;

  fs::path dir = localAppDataDir();
  if (dir.empty())
    return false;

  std::error_code ec;
  fs::create_directories(dir, ec);
  fs::copy_file(self, dir / cfg::kCachedExeName,
                fs::copy_options::overwrite_existing, ec);
  return !ec;
}

// ---------------------------------------------------------------------------
// File migration (stplug-in -> lua, depotcache sync)
// ---------------------------------------------------------------------------

struct CopyJob {
  fs::path from;
  fs::path to;
};

// Old SteamTools-style lua scripts in config\stplug-in move to config\lua.
static void collectLuaMigrations(const fs::path &steam,
                                 std::vector<CopyJob> &jobs) {
  fs::path src = steam / "config" / "stplug-in";
  fs::path dst = steam / "config" / "lua";
  std::error_code ec;
  if (!fs::is_directory(src, ec))
    return;
  fs::create_directories(dst, ec);

  for (const auto &entry : fs::directory_iterator(src, ec)) {
    if (!entry.is_regular_file(ec))
      continue;
    const fs::path &p = entry.path();
    if (_stricmp(p.extension().string().c_str(), ".lua") != 0)
      continue;
    if (_stricmp(p.filename().string().c_str(), "manifest.lua") == 0)
      continue;
    fs::path target = dst / p.filename();
    if (!fs::exists(target, ec))
      jobs.push_back({p, target});
  }
}

// Every *.manifest in `from` that `to` lacks gets copied over.
static void collectManifestSync(const fs::path &from, const fs::path &to,
                                std::vector<CopyJob> &jobs) {
  std::error_code ec;
  if (!fs::is_directory(from, ec))
    return;
  for (const auto &entry : fs::directory_iterator(from, ec)) {
    if (!entry.is_regular_file(ec))
      continue;
    const fs::path &p = entry.path();
    if (_stricmp(p.extension().string().c_str(), ".manifest") != 0)
      continue;
    fs::path target = to / p.filename();
    if (!fs::exists(target, ec))
      jobs.push_back({p, target});
  }
}

static void printProgress(const std::string &label, size_t done, size_t total) {
  if (g_opts.silent || total == 0)
    return;
  constexpr int kWidth = 28;
  size_t filled = kWidth * done / total;
  std::cout << '\r' << label << " [";
  for (int i = 0; i < kWidth; ++i)
    std::cout << (static_cast<size_t>(i) < filled ? '#' : '-');
  std::cout << "] " << done << '/' << total << std::flush;
  if (done >= total)
    std::cout << '\n';
}

static void runCopyJobs(const std::vector<CopyJob> &jobs,
                        const std::string &label) {
  if (jobs.empty())
    return;

  std::error_code ec;
  for (const auto &j : jobs)
    fs::create_directories(j.to.parent_path(), ec);

  const size_t total = jobs.size();
  unsigned hw = std::thread::hardware_concurrency();
  size_t threads = hw ? std::clamp<size_t>(size_t(hw) * 4, 32, 128) : 32;
  threads = std::min(threads, total);

  std::atomic<size_t> next{0}, done{0};
  std::mutex progressMutex;

  printProgress(label, 0, total);
  std::vector<std::thread> pool;
  pool.reserve(threads);
  for (size_t t = 0; t < threads; ++t) {
    pool.emplace_back([&] {
      for (size_t i = next.fetch_add(1); i < total; i = next.fetch_add(1)) {
        CopyFileW(jobs[i].from.c_str(), jobs[i].to.c_str(), TRUE);
        size_t d = ++done;
        if (d == total || (d & 0xF) == 0) {
          std::lock_guard lock(progressMutex);
          printProgress(label, d, total);
        }
      }
    });
  }
  for (auto &th : pool)
    th.join();
  printProgress(label, total, total);
}

// Prepares config\lua, migrates old files and writes default configs if
// missing.
static void setupConfig(const fs::path &steam) {
  fs::path luaDir = steam / "config" / "lua";
  std::error_code ec;
  fs::create_directories(luaDir, ec);

  std::vector<CopyJob> jobs;
  collectLuaMigrations(steam, jobs);

  fs::path cfgDepot = steam / "config" / "depotcache";
  fs::path depot = steam / "depotcache";
  fs::create_directories(cfgDepot, ec);
  fs::create_directories(depot, ec);
  collectManifestSync(cfgDepot, depot, jobs);
  collectManifestSync(depot, cfgDepot, jobs);

  runCopyJobs(jobs, "Migrating files");

  fs::path manifestLua = luaDir / "manifest.lua";
  if (!fs::exists(manifestLua, ec) &&
      writeTextFile(manifestLua, cfg::kDefaultManifestLua))
    logInfo("Created config/lua/manifest.lua (multi-provider fallback)");

  fs::path toml = steam / "opensteamtool.toml";
  if (!fs::exists(toml, ec) && writeTextFile(toml, cfg::kDefaultToml))
    logInfo(
        "Created opensteamtool.toml (manifest auto + drm.ryuu.lol activation)");
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(int argc, char **argv) {
  g_opts = parseArgs(argc, argv);
  if (!g_opts.silent)
    SetConsoleTitleW(L"Ryuu OST Installer");

  auto steamOpt = findSteamPath();
  if (!steamOpt) {
    if (!g_opts.silent) {
      logError("Could not find Steam install path in registry.");
      pressEnterToExit();
    }
    return 1;
  }
  const fs::path steam = *steamOpt;

  if (g_opts.silent) {
    openLogFile(steam);
    FreeConsole();
  }

  logInfo("Ryuu's OpenSteamTool fork Installer");
  logInfo("");
  logInfo("Steam: " + steam.string());
  logInfo("");
  logInfo("Checking installed OpenSteamTool build...");

  const InstalledBuild build = detectInstalledBuild(steam);
  if (build == InstalledBuild::Vanilla) {
    if (!promptYesNo(
            "Vanilla OpenSteamTool is installed. Switch to Ryuu OpenSteamTool?",
            true)) {
      logInfo("Cancelled.");
      pressEnterToExit();
      return 0;
    }
    logInfo("");
  } else if (build == InstalledBuild::Other) {
    if (!promptYesNo("A different OpenSteamTool build is installed (not "
                     "official vanilla/ryuu). "
                     "Replace it with Ryuu OpenSteamTool?",
                     true)) {
      logInfo("Cancelled.");
      pressEnterToExit();
      return 0;
    }
    logInfo("");
  }

  logInfo("Fetching manifest...");
  auto manifest = fetchManifest(steam);
  if (!manifest) {
    logError("Could not fetch manifest from generator.ryuu.lol / gen.ryuu.lol");
    pressEnterToExit();
    return 1;
  }

  // Decide what needs installing.
  std::vector<ManifestEntry> toInstall;
  for (const auto &e : *manifest) {
    auto local = sha256File(steam / e.name);
    if (local && toLower(*local) == e.sha256) {
      logInfo("[ok] " + e.name + " is up to date");
      continue;
    }
    logInfo("[update] " + e.name +
            (local ? " hash mismatch" : " missing or unreadable"));
    toInstall.push_back(e);
  }

  const bool steamToolsPresent =
      isSteamToolsInstalled(steam) || isProcessRunning(L"steamtools.exe");
  if (toInstall.empty() && !steamToolsPresent &&
      build == InstalledBuild::Ryuu) {
    logInfo("");
    logInfo("All files are already installed. Nothing to do.");
    cacheInstaller();
    pressEnterToExit();
    return 0;
  }

  logInfo("");
  if (!promptYesNo("Steam needs to be restarted to continue. Continue?",
                   true)) {
    logInfo("Cancelled.");
    pressEnterToExit();
    return 0;
  }

  stopSteam();
  uninstallSteamTools(steam);

  if (!toInstall.empty()) {
    logInfo("");
    const fs::path staging = stagingDir();

    for (const auto &e : toInstall) {
      std::optional<Bytes> data;

      if (g_opts.staging && !staging.empty()) {
        data = readFile(staging / e.name);
        if (data) {
          auto h = sha256Hex(*data);
          if (h && *h == e.sha256) {
            logInfo("Installing " + e.name + " from staging");
          } else {
            logInfo("[staging] hash mismatch for " + e.name +
                    ", re-downloading");
            data.reset();
          }
        }
      }

      if (!data) {
        logInfo("Downloading " + e.name);
        data = httpGet(e.url, cfg::kMaxDownloadBytes);
        if (!data) {
          logError("Download failed: " + e.name);
          pressEnterToExit();
          return 1;
        }
      }

      auto h = sha256Hex(*data);
      if (!h || *h != e.sha256) {
        logError("Downloaded file hash mismatch: " + e.name);
        pressEnterToExit();
        return 1;
      }

      fs::path dst = steam / e.name;
      if (!writeFileAtomic(dst, *data)) {
        logError("Could not write: " + dst.string());
        pressEnterToExit();
        return 1;
      }
      logInfo("Installed " + e.name);
    }
    clearStaging();
  }

  setupConfig(steam);

  {
    std::ofstream marker(steam / cfg::kMarkerFile,
                         std::ios::out | std::ios::trunc | std::ios::binary);
    if (marker) {
      marker << cfg::kMarkerContent;
      logInfo("Created ryuuost.txt in Steam folder");
    }
  }

  if (cacheInstaller())
    logInfo("Cached installer in %LOCALAPPDATA%\\RyuuOpenSteamTool");

  if (!g_opts.noRestart) {
    logInfo("");
    if (startSteam(steam))
      logInfo("Steam started.");
    else
      logError("Could not start Steam. Launch it manually from: " +
               steam.string());
  }

  logInfo("");
  logInfo("Done.");
  pressEnterToExit();
  return 0;
}
