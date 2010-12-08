#include "libsndfilesrc.h"
#include "win32util.h"

static
uint32_t convert_chanmap(uint32_t value)
{
    switch (value) {
	case SF_CHANNEL_MAP_MONO:
	case SF_CHANNEL_MAP_LEFT: case SF_CHANNEL_MAP_FRONT_LEFT:
	    return 1;
	case SF_CHANNEL_MAP_RIGHT: case SF_CHANNEL_MAP_FRONT_RIGHT:
	    return 2;
	case SF_CHANNEL_MAP_CENTER: case SF_CHANNEL_MAP_FRONT_CENTER:
	    return 3;
	case SF_CHANNEL_MAP_LFE: return 4;
	case SF_CHANNEL_MAP_REAR_LEFT: return 5;
	case SF_CHANNEL_MAP_REAR_RIGHT: return 6;
	case SF_CHANNEL_MAP_FRONT_LEFT_OF_CENTER: return 7;
	case SF_CHANNEL_MAP_FRONT_RIGHT_OF_CENTER: return 8;
	case SF_CHANNEL_MAP_REAR_CENTER: return 9;
	case SF_CHANNEL_MAP_SIDE_LEFT: return 10;
	case SF_CHANNEL_MAP_SIDE_RIGHT: return 11;
	case SF_CHANNEL_MAP_TOP_CENTER: return 12;
	case SF_CHANNEL_MAP_TOP_FRONT_LEFT: return 13;
	case SF_CHANNEL_MAP_TOP_FRONT_CENTER: return 15;
	case SF_CHANNEL_MAP_TOP_FRONT_RIGHT: return 15;
	case SF_CHANNEL_MAP_TOP_REAR_LEFT: return 16;
	case SF_CHANNEL_MAP_TOP_REAR_CENTER: return 17;
	case SF_CHANNEL_MAP_TOP_REAR_RIGHT: return 18;
	default:
	    throw std::runtime_error(format("Unknown channel: %d", value));
    }
}

#define CHECK(expr) do { if (!(expr)) throw std::runtime_error("ERROR"); } \
    while (0)

LibSndfileModule::LibSndfileModule(const std::wstring &path)
{
    HMODULE hDll;
    hDll = LoadLibraryW(path.c_str());
    m_loaded = (hDll != NULL);
    if (!m_loaded)
	return;
    try {
	CHECK(wchar_open = ProcAddress(hDll, "sf_wchar_open"));
	CHECK(open_fd = ProcAddress(hDll, "sf_open_fd"));
	CHECK(close = ProcAddress(hDll, "sf_close"));
	CHECK(strerror = ProcAddress(hDll, "sf_strerror"));
	CHECK(command = ProcAddress(hDll, "sf_command"));
	CHECK(seek = ProcAddress(hDll, "sf_seek"));
	CHECK(read_short = ProcAddress(hDll, "sf_read_short"));
	CHECK(read_int = ProcAddress(hDll, "sf_read_int"));
	CHECK(read_float = ProcAddress(hDll, "sf_read_float"));
	CHECK(read_double = ProcAddress(hDll, "sf_read_double"));
	CHECK(close = ProcAddress(hDll, "sf_close"));
    } catch (...) {
	FreeLibrary(hDll);
	m_loaded = false;
	return;
    }
    m_module.swap(module_t(hDll, FreeLibrary));
}


LibSndfileSource::LibSndfileSource(
	const LibSndfileModule &module, const wchar_t *path)
    : m_module(module)
{
    SF_INFO info;
    SNDFILE *fp;
    if (!std::wcscmp(path, L"-"))
	fp = m_module.open_fd(0, SFM_READ, &info, 0);
    else
	fp = m_module.wchar_open(path, SFM_READ, &info);
    if (!fp)
	throw std::runtime_error(m_module.strerror(0));
    m_handle.swap(handle_t(fp, m_module.close));
    setRange(0, info.frames);

    const char *fmtstr;
    static const char *fmtmap[] = {
	"", "S8LE", "S16LE", "S24LE", "S32LE", "S8LE", "F32LE", "F64LE"
    };
    uint32_t subformat = info.format & SF_FORMAT_SUBMASK;
    if (subformat < array_size(fmtmap))
	fmtstr = fmtmap[subformat];
    else
	throw std::runtime_error("Can't handle this kind of subformat");

    SF_FORMAT_INFO finfo;
    std::memset(&finfo, 0, sizeof finfo);
    int count;
    m_module.command(fp, SFC_GET_FORMAT_MAJOR_COUNT, &count, sizeof count);
    for (int i = 0; i < count; ++i) {
	finfo.format = i;
	m_module.command(fp, SFC_GET_FORMAT_MAJOR, &finfo, sizeof finfo);
	if (finfo.format == (info.format & SF_FORMAT_TYPEMASK)) {
	    m_format_name = finfo.extension;
	    break;
	}
    }

    m_format = SampleFormat(fmtstr, info.channels, info.samplerate);
    m_chanmap.resize(m_format.m_nchannels);
    if (m_module.command(fp, SFC_GET_CHANNEL_MAP_INFO, &m_chanmap[0],
	    m_chanmap.size() * sizeof(uint32_t)) == SF_FALSE)
	m_chanmap.clear();
    else
	std::transform(m_chanmap.begin(), m_chanmap.end(),
		m_chanmap.begin(), convert_chanmap);
}

void LibSndfileSource::skipSamples(int64_t count)
{
    if (m_module.seek(m_handle.get(), count, SEEK_CUR) == -1)
	throw std::runtime_error("sf_seek failed");
}

#define SF_READ(type, handle, buffer, nsamples) \
    m_module.read_##type(handle, reinterpret_cast<type*>(buffer), nsamples)

size_t LibSndfileSource::readSamples(void *buffer, size_t nsamples)
{
    nsamples = adjustSamplesToRead(nsamples);
    if (!nsamples) return 0;
    nsamples *= m_format.m_nchannels;
    sf_count_t rc;
    if (m_format.m_bitsPerSample == 8)
	rc = readSamples8(buffer, nsamples);
    else if (m_format.m_bitsPerSample == 16)
	rc = SF_READ(short, m_handle.get(), buffer, nsamples);
    else if (m_format.m_bitsPerSample == 24)
	rc = readSamples24(buffer, nsamples);
    else if (m_format.m_bitsPerSample == 64)
	rc = SF_READ(double, m_handle.get(), buffer, nsamples);
    else if (m_format.m_type == SampleFormat::kIsSignedInteger)
	rc = SF_READ(int, m_handle.get(), buffer, nsamples);
    else
	rc = SF_READ(float, m_handle.get(), buffer, nsamples);
    nsamples = static_cast<size_t>(rc / m_format.m_nchannels);
    addSamplesRead(nsamples);
    return nsamples;
}

size_t LibSndfileSource::readSamples8(void *buffer, size_t nsamples)
{
    std::vector<short> v(m_format.m_nchannels * nsamples);
    sf_count_t rc = SF_READ(short, m_handle.get(), &v[0], nsamples);
    char *bp = reinterpret_cast<char*>(buffer);
    for (size_t i = 0; i < rc; ++i)
	*bp++ = static_cast<char>(v[i] / 256);
    return static_cast<size_t>(rc);
}

size_t LibSndfileSource::readSamples24(void *buffer, size_t nsamples)
{
    std::vector<int32_t> v(m_format.m_nchannels * nsamples);
    sf_count_t rc = SF_READ(int, m_handle.get(), &v[0], nsamples);
    MemorySink24LE sink(buffer);
    for (size_t i = 0; i < rc; ++i)
	sink.put(v[i] / 256);
    return static_cast<size_t>(rc);
}