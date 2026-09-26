#include "Update.h"

#include <windows.h>
#include <winhttp.h>
#include <bcrypt.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// update.txt (ASCII, one "key value" per line):
//
//   Crysis Coop update
//   version 0.6.2
//   package Crysis-Coop-v0.6.2-update.bin
//   size 4512345
//   sha256 <64 hex digits>
//   signature <128 hex digits: ECDSA P-256 r|s over everything above this line>
//
// The package: "CCUP1\n", u32 file count, then per file u32 path length, the
// path (UTF-8, '/', relative to Mods\Coop), u32 size, the bytes (all u32
// little endian). tools/package.ps1 writes both and signs the manifest with
// the key made by tools/update_key.ps1.

namespace
{
	// the release key's public half (tools/update_key.ps1 -Show): X then Y
	const unsigned char UPDATE_PUBLIC_KEY[64] =
	{
		0xd1, 0x64, 0x22, 0x51, 0x14, 0xbd, 0x87, 0x23, 0xfb, 0xac, 0x16, 0x16, 0x46, 0x98, 0x09, 0x4f,
		0xa9, 0x28, 0x30, 0x1e, 0xbb, 0xb3, 0xfa, 0xd6, 0x49, 0xbf, 0x61, 0x16, 0x71, 0x1c, 0x51, 0x55,
		0x07, 0xfc, 0xf1, 0x11, 0xf5, 0x21, 0x1c, 0xb5, 0xe7, 0x0a, 0x97, 0xa3, 0x4f, 0x33, 0xfc, 0xcd,
		0xb5, 0x98, 0xf1, 0x75, 0x72, 0x48, 0xad, 0x94, 0x0c, 0x43, 0x21, 0x64, 0x2f, 0xc4, 0x88, 0x39,
	};

	// where update.txt is looked for, in this order: the mod's VPS (the
	// relay's host: reachable wherever the game is), then GitHub's latest
	// release. The package is next to it ("%V" = the version).
	struct SSource
	{
		const wchar_t* manifest;
		const wchar_t* packages;
	};
	const SSource SOURCES[] =
	{
		{ L"https://crysis.46-225-103-75.sslip.io/update/update.txt", L"https://crysis.46-225-103-75.sslip.io/update/" },
		{ L"https://github.com/SeniorStackOverflow/crysis-coop/releases/latest/download/update.txt",
			L"https://github.com/SeniorStackOverflow/crysis-coop/releases/download/v%V/" },
	};

	const size_t MAX_MANIFEST = 4096;
	const size_t MAX_PACKAGE = 64 * 1024 * 1024;
	const int MANIFEST_TIMEOUT_MS = 4000;
	const int PACKAGE_TIMEOUT_MS = 20000;

	std::wstring s_game;            // the Crysis folder
	std::wstring s_mod;             // Mods\Coop
	FILE* s_log = nullptr;

	void Log(const char* format, ...)
	{
		if (!s_log)
			return;
		SYSTEMTIME t;
		GetLocalTime(&t);
		fprintf(s_log, "%04d-%02d-%02d %02d:%02d:%02d ", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
		va_list args;
		va_start(args, format);
		vfprintf(s_log, format, args);
		va_end(args);
		fputc('\n', s_log);
		fflush(s_log);
	}

	std::string Narrow(const std::wstring& s)
	{
		std::string out;
		for (wchar_t c : s)
			out += c < 128 ? (char)c : '?';
		return out;
	}

	std::wstring Widen(const std::string& s)
	{
		const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
		std::wstring out(n, 0);
		if (n)
			MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &out[0], n);
		return out;
	}

	bool Exists(const std::wstring& path)
	{
		return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
	}

	std::wstring Lower(std::wstring s)
	{
		for (wchar_t& c : s)
			c = (wchar_t)towlower(c);
		return s;
	}

	// the value after a word of the command line ("-coop_update_url <base>")
	std::wstring ArgValue(const std::wstring& args, const wchar_t* name)
	{
		const std::wstring lower = L" " + Lower(args) + L" ";
		const size_t at = lower.find(L" " + std::wstring(name) + L" ");
		if (at == std::wstring::npos)
			return L"";
		size_t begin = at + wcslen(name) + 1;
		while (begin < args.size() && args[begin] == L' ')
			++begin;
		size_t end = args.find(L' ', begin);
		return args.substr(begin, end == std::wstring::npos ? std::wstring::npos : end - begin);
	}

	// ---- versions
	std::vector<int> ParseVersion(const std::string& s)
	{
		std::vector<int> v;
		int n = -1;
		for (char c : s)
		{
			if (c >= '0' && c <= '9')
				n = (n < 0 ? 0 : n * 10) + (c - '0');
			else if (c == '.' && n >= 0)
			{
				v.push_back(n);
				n = -1;
			}
			else
				return std::vector<int>();
		}
		if (n < 0)
			return std::vector<int>();
		v.push_back(n);
		return v;
	}

	bool Newer(const std::vector<int>& a, const std::vector<int>& b)
	{
		for (size_t i = 0; i < a.size() || i < b.size(); ++i)
		{
			const int x = i < a.size() ? a[i] : 0, y = i < b.size() ? b[i] : 0;
			if (x != y)
				return x > y;
		}
		return false;
	}

	// the installed mod: Coop.dll's file version (major.minor.patch)
	std::string InstalledVersion()
	{
		const std::wstring dll = s_mod + L"\\Bin32\\Coop.dll";
		DWORD handle = 0;
		const DWORD size = GetFileVersionInfoSizeW(dll.c_str(), &handle);
		if (!size)
			return "";
		std::vector<BYTE> info(size);
		VS_FIXEDFILEINFO* fixed = nullptr;
		UINT len = 0;
		if (!GetFileVersionInfoW(dll.c_str(), 0, size, info.data()) ||
			!VerQueryValueW(info.data(), L"\\", (void**)&fixed, &len) || !fixed)
			return "";
		char text[64];
		sprintf_s(text, "%u.%u.%u", HIWORD(fixed->dwFileVersionMS), LOWORD(fixed->dwFileVersionMS), HIWORD(fixed->dwFileVersionLS));
		return text;
	}

	// ---- HTTP(S) GET; redirects are followed (GitHub's release links)
	bool HttpGet(const std::wstring& url, std::vector<char>& out, size_t maxSize, int timeoutMs)
	{
		out.clear();
		wchar_t host[256] = {}, path[2048] = {};
		URL_COMPONENTS uc = {};
		uc.dwStructSize = sizeof(uc);
		uc.lpszHostName = host;
		uc.dwHostNameLength = ARRAYSIZE(host);
		uc.lpszUrlPath = path;
		uc.dwUrlPathLength = ARRAYSIZE(path);
		if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc))
		{
			Log("  bad URL %s", Narrow(url).c_str());
			return false;
		}
		bool ok = false;
		DWORD status = 0;
		HINTERNET session = WinHttpOpen(L"CrysisCoop-Launcher", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
		HINTERNET connection = session ? WinHttpConnect(session, host, uc.nPort, 0) : nullptr;
		HINTERNET request = connection ? WinHttpOpenRequest(connection, L"GET", path, nullptr, WINHTTP_NO_REFERER,
			WINHTTP_DEFAULT_ACCEPT_TYPES, uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0) : nullptr;
		if (request)
		{
			WinHttpSetTimeouts(session, timeoutMs, timeoutMs, timeoutMs, timeoutMs);
			WinHttpSetTimeouts(request, timeoutMs, timeoutMs, timeoutMs, timeoutMs);
			DWORD len = sizeof(status);
			if (WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
				WinHttpReceiveResponse(request, nullptr) &&
				WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
					&status, &len, WINHTTP_NO_HEADER_INDEX) && status == 200)
			{
				ok = true;
				for (;;)
				{
					DWORD available = 0, read = 0;
					if (!WinHttpQueryDataAvailable(request, &available))
					{
						ok = false;
						break;
					}
					if (!available)
						break;
					if (out.size() + available > maxSize)
					{
						Log("  %s: larger than %u bytes", Narrow(url).c_str(), (unsigned)maxSize);
						ok = false;
						break;
					}
					const size_t at = out.size();
					out.resize(at + available);
					if (!WinHttpReadData(request, &out[at], available, &read))
					{
						ok = false;
						break;
					}
					out.resize(at + read);
				}
			}
		}
		if (!ok)
			Log("  %s: %s (%lu)", Narrow(url).c_str(), status ? "HTTP status" : "no answer", status ? status : GetLastError());
		if (request)
			WinHttpCloseHandle(request);
		if (connection)
			WinHttpCloseHandle(connection);
		if (session)
			WinHttpCloseHandle(session);
		return ok;
	}

	// ---- SHA-256 and the release signature (CNG: Windows 7+, Wine)
	bool Sha256(const void* data, size_t size, unsigned char hash[32])
	{
		bool ok = false;
		BCRYPT_ALG_HANDLE alg = nullptr;
		BCRYPT_HASH_HANDLE h = nullptr;
		if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0)
		{
			if (BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0) == 0)
			{
				ok = BCryptHashData(h, (PUCHAR)data, (ULONG)size, 0) == 0 && BCryptFinishHash(h, hash, 32, 0) == 0;
				BCryptDestroyHash(h);
			}
			BCryptCloseAlgorithmProvider(alg, 0);
		}
		return ok;
	}

	bool SignedByRelease(const std::string& message, const unsigned char signature[64])
	{
		unsigned char hash[32];
		if (!Sha256(message.data(), message.size(), hash))
			return false;
		bool ok = false;
		BCRYPT_ALG_HANDLE alg = nullptr;
		if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_ECDSA_P256_ALGORITHM, nullptr, 0) != 0)
		{
			Log("  ECDSA P-256 is not available");
			return false;
		}
		unsigned char blob[sizeof(BCRYPT_ECCKEY_BLOB) + sizeof(UPDATE_PUBLIC_KEY)];
		BCRYPT_ECCKEY_BLOB* header = (BCRYPT_ECCKEY_BLOB*)blob;
		header->dwMagic = BCRYPT_ECDSA_PUBLIC_P256_MAGIC;
		header->cbKey = 32;
		memcpy(blob + sizeof(BCRYPT_ECCKEY_BLOB), UPDATE_PUBLIC_KEY, sizeof(UPDATE_PUBLIC_KEY));
		BCRYPT_KEY_HANDLE key = nullptr;
		if (BCryptImportKeyPair(alg, nullptr, BCRYPT_ECCPUBLIC_BLOB, &key, blob, sizeof(blob), 0) == 0)
		{
			ok = BCryptVerifySignature(key, nullptr, hash, sizeof(hash), (PUCHAR)signature, 64, 0) == 0;
			BCryptDestroyKey(key);
		}
		BCryptCloseAlgorithmProvider(alg, 0);
		return ok;
	}

	bool FromHex(const std::string& hex, unsigned char* out, size_t size)
	{
		if (hex.size() != size * 2)
			return false;
		for (size_t i = 0; i < size; ++i)
		{
			unsigned v = 0;
			for (int k = 0; k < 2; ++k)
			{
				const char c = hex[i * 2 + k];
				v <<= 4;
				if (c >= '0' && c <= '9')
					v |= c - '0';
				else if (c >= 'a' && c <= 'f')
					v |= c - 'a' + 10;
				else if (c >= 'A' && c <= 'F')
					v |= c - 'A' + 10;
				else
					return false;
			}
			out[i] = (unsigned char)v;
		}
		return true;
	}

	// ---- update.txt
	struct SManifest
	{
		std::string version, package;
		size_t size = 0;
		unsigned char sha256[32] = {};
	};

	bool ParseManifest(const std::vector<char>& data, SManifest& m)
	{
		const std::string text(data.begin(), data.end());
		const size_t sig = text.find("\nsignature ");
		if (text.compare(0, 18, "Crysis Coop update") != 0 || sig == std::string::npos)
		{
			Log("  not an update manifest");
			return false;
		}
		unsigned char signature[64];
		std::string sigHex = text.substr(sig + 11);
		while (!sigHex.empty() && (sigHex.back() == '\n' || sigHex.back() == '\r' || sigHex.back() == ' '))
			sigHex.pop_back();
		if (!FromHex(sigHex, signature, sizeof(signature)) || !SignedByRelease(text.substr(0, sig + 1), signature))
		{
			Log("  the manifest's signature is NOT the release key's: ignored");
			return false;
		}
		std::string sha;
		size_t at = 0;
		while (at < sig)
		{
			size_t end = text.find('\n', at);
			if (end == std::string::npos || end > sig)
				end = sig;
			const std::string line = text.substr(at, end - at);
			at = end + 1;
			const size_t space = line.find(' ');
			if (space == std::string::npos)
				continue;
			const std::string key = line.substr(0, space), value = line.substr(space + 1);
			if (key == "version")
				m.version = value;
			else if (key == "package")
				m.package = value;
			else if (key == "size")
				m.size = (size_t)_strtoui64(value.c_str(), nullptr, 10);
			else if (key == "sha256")
				sha = value;
		}
		// the package name goes into a URL: plain characters only
		bool plain = !m.package.empty();
		for (char c : m.package)
			plain = plain && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_');
		if (ParseVersion(m.version).empty() || !plain || !m.size || m.size > MAX_PACKAGE || !FromHex(sha, m.sha256, 32))
		{
			Log("  incomplete manifest");
			return false;
		}
		return true;
	}

	// ---- the package
	struct SFile
	{
		std::wstring path;          // relative to Mods\Coop, '\'
		const char* data = nullptr;
		size_t size = 0;
	};

	bool SafePath(const std::string& path)
	{
		if (path.empty() || path.size() > 260 || path[0] == '/' || path[0] == '\\')
			return false;
		std::string part;
		for (size_t i = 0; i <= path.size(); ++i)
		{
			const char c = i < path.size() ? path[i] : '/';
			if (c == '/' || c == '\\')
			{
				if (part.empty() || part == "." || part == "..")
					return false;
				part.clear();
			}
			else if ((unsigned char)c < 32 || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|')
				return false;
			else
				part += c;
		}
		return true;
	}

	bool ParsePackage(const std::vector<char>& data, std::vector<SFile>& files)
	{
		const size_t n = data.size();
		size_t at = 6;
		auto u32 = [&](unsigned& v) -> bool
		{
			if (at + 4 > n)
				return false;
			v = (unsigned char)data[at] | ((unsigned char)data[at + 1] << 8) | ((unsigned char)data[at + 2] << 16) | ((unsigned)(unsigned char)data[at + 3] << 24);
			at += 4;
			return true;
		};
		unsigned count = 0;
		if (n < 10 || memcmp(data.data(), "CCUP1\n", 6) != 0 || !u32(count) || !count || count > 10000)
			return false;
		for (unsigned i = 0; i < count; ++i)
		{
			unsigned pathLen = 0, size = 0;
			if (!u32(pathLen) || at + pathLen > n)
				return false;
			const std::string path(&data[at], pathLen);
			at += pathLen;
			if (!u32(size) || at + size > n || !SafePath(path))
			{
				Log("  bad entry %u in the package", i);
				return false;
			}
			SFile f;
			f.path = Widen(path);
			for (wchar_t& c : f.path)
				if (c == L'/')
					c = L'\\';
			f.data = data.data() + at;
			f.size = size;
			at += size;
			files.push_back(f);
		}
		return at == n;
	}

	// ---- installing
	bool WriteWhole(const std::wstring& path, const char* data, size_t size)
	{
		HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (f == INVALID_HANDLE_VALUE)
			return false;
		DWORD written = 0;
		const bool ok = WriteFile(f, data, (DWORD)size, &written, nullptr) && written == size && FlushFileBuffers(f);
		CloseHandle(f);
		if (!ok)
			DeleteFileW(path.c_str());
		return ok;
	}

	void MakeDirs(const std::wstring& file)
	{
		for (size_t i = s_mod.size() + 1; i < file.size(); ++i)
			if (file[i] == L'\\')
				CreateDirectoryW(file.substr(0, i).c_str(), nullptr);
	}

	// files that could not be removed yet (a running launcher's old copy):
	// tried again on every start
	const wchar_t* CLEANUP = L"\\update_cleanup.txt";

	void Cleanup(std::vector<std::wstring> more = std::vector<std::wstring>())
	{
		std::vector<std::wstring> left;
		if (FILE* f = _wfopen((s_mod + CLEANUP).c_str(), L"rt, ccs=UTF-8"))
		{
			wchar_t line[1024];
			while (fgetws(line, ARRAYSIZE(line), f))
			{
				std::wstring path(line);
				while (!path.empty() && (path.back() == L'\n' || path.back() == L'\r'))
					path.pop_back();
				if (!path.empty())
					more.push_back(path);
			}
			fclose(f);
		}
		for (const std::wstring& path : more)
			if (!DeleteFileW(path.c_str()) && Exists(path))
				left.push_back(path);
		if (left.empty())
		{
			DeleteFileW((s_mod + CLEANUP).c_str());
			return;
		}
		if (FILE* f = _wfopen((s_mod + CLEANUP).c_str(), L"wt, ccs=UTF-8"))
		{
			for (const std::wstring& path : left)
				fwprintf(f, L"%s\n", path.c_str());
			fclose(f);
		}
	}

	// every file of the package next to its target first, then all swapped
	// in; anything failing puts the old files back
	bool Install(const std::vector<SFile>& files)
	{
		// a game started from this folder holds Coop.dll: never under it
		HANDLE dll = CreateFileW((s_mod + L"\\Bin32\\Coop.dll").c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
		if (dll == INVALID_HANDLE_VALUE && GetLastError() == ERROR_SHARING_VIOLATION)
		{
			Log("  the game is running: the update waits for the next start");
			return false;
		}
		if (dll != INVALID_HANDLE_VALUE)
			CloseHandle(dll);

		std::vector<std::wstring> targets;
		for (const SFile& f : files)
		{
			const std::wstring target = s_mod + L"\\" + f.path;
			MakeDirs(target);
			if (!WriteWhole(target + L".cc_new", f.data, f.size))
			{
				Log("  cannot write %s (%lu)", Narrow(target).c_str(), GetLastError());
				for (const std::wstring& t : targets)
					DeleteFileW((t + L".cc_new").c_str());
				return false;
			}
			targets.push_back(target);
		}
		std::vector<int> hadOld(targets.size(), 0);
		size_t done = 0;
		for (; done < targets.size(); ++done)
		{
			const std::wstring& t = targets[done];
			if (Exists(t))
			{
				if (!MoveFileExW(t.c_str(), (t + L".cc_old").c_str(), MOVEFILE_REPLACE_EXISTING))
					break;
				hadOld[done] = 1;
			}
			if (!MoveFileExW((t + L".cc_new").c_str(), t.c_str(), MOVEFILE_REPLACE_EXISTING))
			{
				if (hadOld[done])
					MoveFileExW((t + L".cc_old").c_str(), t.c_str(), MOVEFILE_REPLACE_EXISTING);
				break;
			}
		}
		if (done < targets.size())
		{
			Log("  cannot replace %s (%lu): the old version is put back", Narrow(targets[done]).c_str(), GetLastError());
			for (size_t i = done; i-- > 0;)
			{
				const std::wstring& t = targets[i];
				if (hadOld[i])
					MoveFileExW((t + L".cc_old").c_str(), t.c_str(), MOVEFILE_REPLACE_EXISTING);
				else
					DeleteFileW(t.c_str());
			}
			for (const std::wstring& t : targets)
				DeleteFileW((t + L".cc_new").c_str());
			return false;
		}
		std::vector<std::wstring> old;
		for (size_t i = 0; i < targets.size(); ++i)
			if (hadOld[i])
				old.push_back(targets[i] + L".cc_old");
		Cleanup(old);
		return true;
	}

	bool SameFile(const std::wstring& a, const std::wstring& b)
	{
		std::vector<char> x, y;
		for (int k = 0; k < 2; ++k)
		{
			HANDLE f = CreateFileW((k ? b : a).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
			if (f == INVALID_HANDLE_VALUE)
				return false;
			std::vector<char>& v = k ? y : x;
			v.resize(GetFileSize(f, nullptr));
			DWORD read = 0;
			const bool ok = v.empty() || (ReadFile(f, v.data(), (DWORD)v.size(), &read, nullptr) && read == v.size());
			CloseHandle(f);
			if (!ok)
				return false;
		}
		return x == y;
	}

	// the launcher in the game folder (the one the shortcuts start) follows
	// the mod's copy; a running exe can be renamed, not overwritten
	void UpdateLauncher()
	{
		const std::wstring root = s_game + L"\\CrysisCoop.exe", fresh = s_mod + L"\\CrysisCoop.exe";
		if (!Exists(root) || !Exists(fresh) || SameFile(root, fresh))
			return;
		const std::wstring old = root + L".cc_old";
		DeleteFileW(old.c_str());
		if (!MoveFileExW(root.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING))
		{
			Log("  the launcher in the game folder cannot be replaced (%lu)", GetLastError());
			return;
		}
		if (!CopyFileW(fresh.c_str(), root.c_str(), FALSE))
		{
			Log("  the launcher in the game folder cannot be written (%lu): the old one is put back", GetLastError());
			MoveFileExW(old.c_str(), root.c_str(), MOVEFILE_REPLACE_EXISTING);
			return;
		}
		Cleanup(std::vector<std::wstring>(1, old));
		Log("  the launcher in the game folder updated");
	}

	// ---- a small window while a new version downloads (nothing to click)
	HWND s_window = nullptr, s_label = nullptr;

	LRESULT CALLBACK WindowProc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp)
	{
		if (msg == WM_CLOSE)
			return 0;
		return DefWindowProcW(wnd, msg, wp, lp);
	}

	void ShowProgress(const std::wstring& text)
	{
		if (!s_window)
		{
			WNDCLASSW wc = {};
			wc.lpfnWndProc = WindowProc;
			wc.hInstance = GetModuleHandleW(nullptr);
			wc.hIcon = LoadIconW(wc.hInstance, MAKEINTRESOURCEW(1));
			wc.hCursor = LoadCursorW(nullptr, IDC_WAIT);
			wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
			wc.lpszClassName = L"CrysisCoopUpdate";
			RegisterClassW(&wc);
			const int w = 440, h = 110;
			s_window = CreateWindowExW(WS_EX_TOPMOST, wc.lpszClassName, L"Crysis Coop", WS_POPUP | WS_CAPTION,
				(GetSystemMetrics(SM_CXSCREEN) - w) / 2, (GetSystemMetrics(SM_CYSCREEN) - h) / 2, w, h, nullptr, nullptr, wc.hInstance, nullptr);
			s_label = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_CENTER, 10, 22, w - 20, 40, s_window, nullptr, wc.hInstance, nullptr);
			SendMessageW(s_label, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
			ShowWindow(s_window, SW_SHOWNOACTIVATE);
		}
		SetWindowTextW(s_label, text.c_str());
		MSG msg;
		while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
		{
			TranslateMessage(&msg);
			DispatchMessageW(&msg);
		}
	}

	struct SDownload
	{
		std::wstring url;
		std::vector<char> data;
		bool ok = false;
	};

	DWORD WINAPI DownloadThread(void* p)
	{
		SDownload* d = (SDownload*)p;
		d->ok = HttpGet(d->url, d->data, MAX_PACKAGE, PACKAGE_TIMEOUT_MS);
		return 0;
	}

	// the package download runs aside while the window stays responsive
	bool DownloadWithWindow(SDownload& d, const std::wstring& text)
	{
		ShowProgress(text);
		HANDLE thread = CreateThread(nullptr, 0, DownloadThread, &d, 0, nullptr);
		if (!thread)
			return DownloadThread(&d), d.ok;
		for (;;)
		{
			const DWORD r = MsgWaitForMultipleObjects(1, &thread, FALSE, INFINITE, QS_ALLINPUT);
			if (r == WAIT_OBJECT_0)
				break;
			MSG msg;
			while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
			{
				TranslateMessage(&msg);
				DispatchMessageW(&msg);
			}
		}
		CloseHandle(thread);
		return d.ok;
	}

	bool Russian()
	{
		return PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_RUSSIAN;
	}
}

Update::EResult Update::Run(const std::wstring& game, const std::wstring& args)
{
	s_game = game;
	s_mod = game + L"\\Mods\\Coop";
	const std::wstring logPath = s_mod + L"\\update.log";
	// the log keeps the recent starts only
	WIN32_FILE_ATTRIBUTE_DATA attr;
	if (GetFileAttributesExW(logPath.c_str(), GetFileExInfoStandard, &attr) && attr.nFileSizeLow > 256 * 1024)
		DeleteFileW(logPath.c_str());
	s_log = _wfopen(logPath.c_str(), L"at");
	struct SClose { ~SClose() { if (s_log) fclose(s_log); s_log = nullptr; if (s_window) DestroyWindow(s_window); s_window = nullptr; } } closer;

	Cleanup();
	const std::string installed = InstalledVersion();
	const std::wstring lowerArgs = L" " + Lower(args) + L" ";
	if (lowerArgs.find(L" -coop_noupdate ") != std::wstring::npos || Exists(s_mod + L"\\noupdate"))
	{
		Log("installed %s: updates are off (-coop_noupdate or Mods\\Coop\\noupdate)", installed.c_str());
		return eResult_Skipped;
	}
	if (installed.empty())
	{
		Log("no Mods\\Coop\\Bin32\\Coop.dll version: nothing to update");
		return eResult_Skipped;
	}

	// tests: -coop_update_url <base> (update.txt and the package under it)
	const std::wstring testBase = ArgValue(args, L"-coop_update_url");
	std::vector<SSource> sources;
	if (!testBase.empty())
	{
		static std::wstring manifest, packages;
		packages = testBase.back() == L'/' ? testBase : testBase + L"/";
		manifest = packages + L"update.txt";
		sources.push_back(SSource{ manifest.c_str(), packages.c_str() });
	}
	else
		sources.assign(SOURCES, SOURCES + ARRAYSIZE(SOURCES));

	for (const SSource& source : sources)
	{
		std::vector<char> data;
		SManifest m;
		if (!HttpGet(source.manifest, data, MAX_MANIFEST, MANIFEST_TIMEOUT_MS) || !ParseManifest(data, m))
			continue;
		if (!Newer(ParseVersion(m.version), ParseVersion(installed)))
		{
			Log("installed %s, latest %s (%s): up to date", installed.c_str(), m.version.c_str(), Narrow(source.manifest).c_str());
			return eResult_UpToDate;
		}
		Log("installed %s, latest %s: updating from %s", installed.c_str(), m.version.c_str(), Narrow(source.manifest).c_str());
		std::wstring base = source.packages;
		const size_t v = base.find(L"%V");
		if (v != std::wstring::npos)
			base.replace(v, 2, Widen(m.version));
		SDownload d;
		d.url = base + Widen(m.package);
		const std::wstring text = (Russian() ? L"Обновление Crysis Coop до версии " : L"Updating Crysis Coop to version ") + Widen(m.version) + L"...";
		unsigned char hash[32];
		std::vector<SFile> files;
		if (!DownloadWithWindow(d, text))
			continue;
		if (d.data.size() != m.size || !Sha256(d.data.data(), d.data.size(), hash) || memcmp(hash, m.sha256, 32) != 0)
		{
			Log("  the package does not match the manifest (%u bytes): not installed", (unsigned)d.data.size());
			continue;
		}
		if (!ParsePackage(d.data, files))
		{
			Log("  the package is damaged: not installed");
			continue;
		}
		if (!Install(files))
			return eResult_Failed;
		UpdateLauncher();
		Log("  updated to %s (%u files)", m.version.c_str(), (unsigned)files.size());
		ShowProgress((Russian() ? L"Crysis Coop обновлён до версии " : L"Crysis Coop is updated to version ") + Widen(m.version));
		Sleep(1200);
		return eResult_Updated;
	}
	Log("installed %s: no server gave a valid update this time", installed.c_str());
	return eResult_Failed;
}
