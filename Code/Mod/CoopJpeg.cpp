// Crysis Coop: a JPEG from the game's pixels (GDI+, a part of Windows), for
// the AI companion's screenshots. Apart from the game's headers: GDI+ needs
// the Windows min/max the game's headers take away.
#include <vector>
#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>
#pragma comment(lib, "gdiplus.lib")

namespace
{
	bool s_started = false;
	CLSID s_jpeg;

	bool Start()
	{
		if (s_started)
			return true;
		Gdiplus::GdiplusStartupInput in;
		ULONG_PTR token = 0;
		if (Gdiplus::GdiplusStartup(&token, &in, 0) != Gdiplus::Ok)
			return false;
		UINT n = 0, size = 0;
		Gdiplus::GetImageEncodersSize(&n, &size);
		if (!size)
			return false;
		std::vector<unsigned char> buf(size);
		Gdiplus::ImageCodecInfo* codecs = (Gdiplus::ImageCodecInfo*)&buf[0];
		Gdiplus::GetImageEncoders(n, size, codecs);
		for (UINT i = 0; i < n; ++i)
		{
			if (!wcscmp(codecs[i].MimeType, L"image/jpeg"))
			{
				s_jpeg = codecs[i].Clsid;
				s_started = true;
			}
		}
		return s_started;
	}
}

// rgb: width * height pixels, 3 bytes each (red, green, blue), rows bottom up
// (as the renderer's ReadFrameBuffer gives them)
bool CoopEncodeJpeg(const unsigned char* rgb, int width, int height, int quality, std::vector<unsigned char>& out)
{
	out.clear();
	if (!rgb || width <= 0 || height <= 0 || !Start())
		return false;
	// GDI+'s 24-bit pixels: blue, green, red; rows 4-byte aligned
	const int stride = (width * 3 + 3) & ~3;
	std::vector<unsigned char> bgr((size_t)stride * height);
	for (int y = 0; y < height; ++y)
	{
		const unsigned char* s = rgb + (size_t)(height - 1 - y) * width * 3;
		unsigned char* d = &bgr[(size_t)y * stride];
		for (int x = 0; x < width; ++x, s += 3, d += 3)
		{
			d[0] = s[2];
			d[1] = s[1];
			d[2] = s[0];
		}
	}
	Gdiplus::Bitmap bmp(width, height, stride, PixelFormat24bppRGB, &bgr[0]);
	IStream* stream = 0;
	if (FAILED(CreateStreamOnHGlobal(0, TRUE, &stream)))
		return false;
	// EncoderQuality {1d5be4b5-fa4a-452d-9cdd-5db35105e7eb}
	static const GUID quality_ = { 0x1d5be4b5, 0xfa4a, 0x452d, { 0x9c, 0xdd, 0x5d, 0xb3, 0x51, 0x05, 0xe7, 0xeb } };
	ULONG q = quality;
	Gdiplus::EncoderParameters params;
	params.Count = 1;
	params.Parameter[0].Guid = quality_;
	params.Parameter[0].Type = Gdiplus::EncoderParameterValueTypeLong;
	params.Parameter[0].NumberOfValues = 1;
	params.Parameter[0].Value = &q;
	bool ok = bmp.Save(stream, &s_jpeg, &params) == Gdiplus::Ok;
	if (ok)
	{
		STATSTG st = {};
		HGLOBAL mem = 0;
		ok = SUCCEEDED(stream->Stat(&st, STATFLAG_NONAME)) && SUCCEEDED(GetHGlobalFromStream(stream, &mem)) && st.cbSize.LowPart > 0;
		if (ok)
		{
			const unsigned char* p = (const unsigned char*)GlobalLock(mem);
			ok = p != 0;
			if (ok)
				out.assign(p, p + st.cbSize.LowPart);
			GlobalUnlock(mem);
		}
	}
	stream->Release();
	return ok && !out.empty();
}
