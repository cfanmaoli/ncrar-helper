// ncrar-helper - a standalone RAR reader for Nimble Commander's optional RAR support.
//
// The UnRAR sources are used under the UnRAR license (see LICENSE): this program may only be used
// to handle RAR archives, never to build a RAR-compatible archiver. It is a separate program on
// purpose - the GPLv3 file manager must not link or embed any UnRAR code.

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <exception>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

#define _UNIX
#include <dll.hpp>
#undef _UNIX

namespace {

constexpr int kExitOk = 0;
constexpr int kExitUsage = 2;
constexpr int kExitOpen = 3;
constexpr int kExitRead = 4;
constexpr int kExitPassword = 5;

// Values of RARHeaderDataEx::RedirType. They live in the library's private headers, so mirror the
// small public part of the enum here instead of including internal UnRAR headers.
enum : unsigned int {
    kRedirNone = 0,
    kRedirUnixSymlink = 1,
    kRedirWinSymlink = 2,
    kRedirJunction = 3,
    kRedirHardlink = 4,
    kRedirFileCopy = 5,
};

bool IsLinkType(unsigned int _type)
{
    return _type == kRedirUnixSymlink || _type == kRedirWinSymlink || _type == kRedirJunction ||
           _type == kRedirHardlink;
}

struct Context {
    const char *password = nullptr;
    int output_fd = -1;
    bool write_failed = false;
};

int PASCAL Callback(UINT _msg, LPARAM _user, LPARAM _p1, LPARAM _p2)
{
    auto &ctx = *reinterpret_cast<Context *>(_user);
    switch( _msg ) {
        case UCM_CHANGEVOLUME:
        case UCM_CHANGEVOLUMEW:
            // Let UnRAR continue with the volume name it computed; multi-volume archives sit next
            // to the first volume and UnRAR derives their names itself.
            return 1;
        case UCM_NEEDPASSWORD:
        case UCM_NEEDPASSWORDW: {
            if( ctx.password == nullptr )
                return 0;
            const size_t size = static_cast<size_t>(_p2);
            if( size == 0 )
                return 0;
            if( _msg == UCM_NEEDPASSWORD ) {
                std::snprintf(reinterpret_cast<char *>(_p1), size, "%s", ctx.password);
            }
            else {
                if( size < sizeof(wchar_t) )
                    return 0;
                auto *dst = reinterpret_cast<wchar_t *>(_p1);
                const size_t len = std::min(size / sizeof(wchar_t) - 1, std::strlen(ctx.password));
                for( size_t i = 0; i != len; ++i )
                    dst[i] = static_cast<unsigned char>(ctx.password[i]);
                dst[len] = 0;
            }
            return 1;
        }
        case UCM_PROCESSDATA: {
            if( ctx.output_fd < 0 )
                return 1;
            const auto *data = reinterpret_cast<const char *>(_p1);
            size_t left = static_cast<size_t>(_p2);
            while( left != 0 ) {
                const ssize_t written = ::write(ctx.output_fd, data, left);
                if( written < 0 && errno == EINTR )
                    continue;
                if( written <= 0 ) {
                    ctx.write_failed = true;
                    return 0;
                }
                data += written;
                left -= static_cast<size_t>(written);
            }
            return 1;
        }
        case UCM_LARGEDICT:
            return 1;
        default:
            return 0;
    }
}

void AppendUtf8(std::string &_out, uint32_t _cp)
{
    if( _cp < 0x80 ) {
        _out += static_cast<char>(_cp);
    }
    else if( _cp < 0x800 ) {
        _out += static_cast<char>(0xC0 | (_cp >> 6));
        _out += static_cast<char>(0x80 | (_cp & 0x3F));
    }
    else if( _cp < 0x10000 ) {
        _out += static_cast<char>(0xE0 | (_cp >> 12));
        _out += static_cast<char>(0x80 | ((_cp >> 6) & 0x3F));
        _out += static_cast<char>(0x80 | (_cp & 0x3F));
    }
    else {
        _out += static_cast<char>(0xF0 | (_cp >> 18));
        _out += static_cast<char>(0x80 | ((_cp >> 12) & 0x3F));
        _out += static_cast<char>(0x80 | ((_cp >> 6) & 0x3F));
        _out += static_cast<char>(0x80 | (_cp & 0x3F));
    }
}

std::string Utf32ToUtf8(const wchar_t *_str, size_t _max)
{
    std::string out;
    for( size_t i = 0; i != _max && _str[i] != 0; ++i )
        AppendUtf8(out, static_cast<uint32_t>(_str[i]));
    return out;
}

void AppendJsonString(std::string &_out, std::string_view _str)
{
    _out += '"';
    for( const char c : _str ) {
        switch( c ) {
            case '"':
                _out += "\\\"";
                break;
            case '\\':
                _out += "\\\\";
                break;
            case '\n':
                _out += "\\n";
                break;
            case '\r':
                _out += "\\r";
                break;
            case '\t':
                _out += "\\t";
                break;
            default:
                if( static_cast<unsigned char>(c) < 0x20 ) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
                    _out += buf;
                }
                else {
                    _out += c;
                }
        }
    }
    _out += '"';
}

int64_t DosTimeToUnix(uint32_t _time)
{
    std::tm tm{};
    tm.tm_year = static_cast<int>(((_time >> 25) & 0x7F) + 1980 - 1900);
    tm.tm_mon = static_cast<int>((_time >> 21) & 0x0F) - 1;
    tm.tm_mday = static_cast<int>((_time >> 16) & 0x1F);
    tm.tm_hour = static_cast<int>((_time >> 11) & 0x1F);
    tm.tm_min = static_cast<int>((_time >> 5) & 0x3F);
    tm.tm_sec = static_cast<int>((_time & 0x1F) * 2);
    return static_cast<int64_t>(::timegm(&tm));
}

int64_t WinTimeToUnix(uint64_t _filetime)
{
    // UnRAR hands out Windows FILETIME values: 100ns intervals since 1601-01-01.
    constexpr uint64_t kEpochDiffSeconds = 11644473600ULL;
    if( _filetime < kEpochDiffSeconds * 10000000ULL )
        return 0;
    return static_cast<int64_t>(_filetime / 10000000ULL) - static_cast<int64_t>(kEpochDiffSeconds);
}

int64_t EntryMtime(const RARHeaderDataEx &_hdr)
{
    const uint64_t mtime = (static_cast<uint64_t>(_hdr.MtimeHigh) << 32) | _hdr.MtimeLow;
    if( mtime != 0 )
        return WinTimeToUnix(mtime);
    return DosTimeToUnix(_hdr.FileTime);
}

struct Status {
    int code = kExitOk;
    std::string message;

    bool Ok() const { return code == kExitOk; }
};

Status MakeStatus(int _code, const char *_message)
{
    Status status;
    status.code = _code;
    status.message = _message;
    return status;
}

Status StatusForRarError(int _rc, bool _had_password)
{
    switch( _rc ) {
        case ERAR_SUCCESS:
            return {};
        case ERAR_MISSING_PASSWORD:
            return MakeStatus(kExitPassword, "a password is required");
        case ERAR_BAD_PASSWORD:
            return MakeStatus(kExitPassword, _had_password ? "invalid password" : "a password is required");
        case ERAR_BAD_ARCHIVE:
            return MakeStatus(kExitOpen, "not a valid RAR archive");
        case ERAR_UNKNOWN_FORMAT:
            return MakeStatus(kExitRead, "unsupported archive format");
        case ERAR_EOPEN:
            return MakeStatus(kExitOpen, "cannot open the archive");
        case ERAR_NO_MEMORY:
            return MakeStatus(kExitRead, "out of memory");
        case ERAR_EWRITE:
            return MakeStatus(kExitRead, "cannot write the entry data");
        case ERAR_ECREATE:
            return MakeStatus(kExitRead, "cannot create the entry data");
        case ERAR_BAD_DATA:
            return MakeStatus(kExitRead, "the archive data is damaged");
        default:
            return MakeStatus(kExitRead, "cannot read the entry data");
    }
}

Status StatusForOpenResult(unsigned int _result, bool _had_password)
{
    switch( _result ) {
        case ERAR_MISSING_PASSWORD:
            return MakeStatus(kExitPassword, "a password is required");
        case ERAR_BAD_PASSWORD:
            return MakeStatus(kExitPassword, _had_password ? "invalid password" : "a password is required");
        case ERAR_BAD_ARCHIVE:
            return MakeStatus(kExitOpen, "not a valid RAR archive");
        case ERAR_UNKNOWN_FORMAT:
            return MakeStatus(kExitOpen, "unsupported archive format");
        default:
            return MakeStatus(kExitOpen, "cannot open the archive");
    }
}

struct Archive {
    HANDLE handle = nullptr;
    Context ctx;
    unsigned int open_result = ERAR_SUCCESS;

    ~Archive()
    {
        if( handle != nullptr )
            RARCloseArchive(handle);
    }

    bool Open(const char *_path, unsigned int _mode, const char *_password)
    {
        ctx.password = _password;
        RAROpenArchiveDataEx data{};
        data.ArcName = const_cast<char *>(_path);
        data.OpenMode = _mode;
        data.Callback = &Callback;
        data.UserData = reinterpret_cast<LPARAM>(&ctx);
        handle = RAROpenArchiveEx(&data);
        open_result = data.OpenResult;
        if( handle != nullptr )
            RARSetCallback(handle, &Callback, reinterpret_cast<LPARAM>(&ctx));
        return handle != nullptr;
    }
};

struct Entry {
    std::string line;
    std::string link_target;
    uint64_t size = 0;
    unsigned int redir_type = kRedirNone;
    bool is_dir = false;
    bool is_link = false;
    bool encrypted = false;
};

std::string BuildEntryLine(const RARHeaderDataEx &_hdr, unsigned int _index, const std::string &_link_target)
{
    const std::string name = Utf32ToUtf8(_hdr.FileNameW, 1024);
    const bool is_dir = (_hdr.Flags & RHDF_DIRECTORY) != 0;
    const bool encrypted = (_hdr.Flags & RHDF_ENCRYPTED) != 0;
    const bool solid = (_hdr.Flags & RHDF_SOLID) != 0;
    const uint64_t size = (static_cast<uint64_t>(_hdr.UnpSizeHigh) << 32) | _hdr.UnpSize;
    const uint64_t packed = (static_cast<uint64_t>(_hdr.PackSizeHigh) << 32) | _hdr.PackSize;

    std::string line = "{\"index\":";
    line += std::to_string(_index);
    line += ",\"name\":";
    AppendJsonString(line, name);
    line += is_dir ? ",\"dir\":true" : ",\"dir\":false";
    line += encrypted ? ",\"encrypted\":true" : ",\"encrypted\":false";
    line += solid ? ",\"solid\":true" : ",\"solid\":false";
    line += ",\"size\":" + std::to_string(size);
    line += ",\"packed\":" + std::to_string(packed);
    line += ",\"mtime\":" + std::to_string(EntryMtime(_hdr));
    line += ",\"crc\":" + std::to_string(_hdr.FileCRC);
    line += ",\"link_type\":" + std::to_string(_hdr.RedirType);
    if( _hdr.RedirType != kRedirNone && _hdr.RedirName != nullptr && !_link_target.empty() ) {
        line += ",\"link_target\":";
        AppendJsonString(line, _link_target);
    }
    // A Unix-written archive stores the file mode (st_mode) in FileAttr; S_IFREG itself is 0x8000,
    // so that bit marks the presence of Unix attributes. Some producers put the mode in the high
    // half instead and mark it the same way - use that when it's non-zero. Windows-written entries
    // keep DOS attributes in the low bits, so the field is omitted for them and stays absent from
    // older consumers' point of view.
    constexpr unsigned int kFileAttrUnixExtension = 0x8000;
    unsigned int mode = 0;
    if( (_hdr.FileAttr & kFileAttrUnixExtension) != 0 ) {
        mode = _hdr.FileAttr >> 16;
        if( mode == 0 )
            mode = _hdr.FileAttr & 0xFFFF;
    }
    if( mode != 0 )
        line += ",\"mode\":" + std::to_string(mode);
    line += '}';
    return line;
}

Status CollectEntries(Archive &_archive, std::vector<Entry> &_entries)
{
    RARHeaderDataEx hdr{};
    wchar_t link_target[1024]{};
    for( unsigned int index = 0;; ++index ) {
        // The library copies the target of a symlink/junction into this buffer when it is set up.
        // It must be re-armed before every header read: the previous header may have left a
        // pointer, and a malformed archive can return before filling the structure.
        hdr.RedirName = link_target;
        hdr.RedirNameSize = 1024;
        link_target[0] = 0;
        const int rc = RARReadHeaderEx(_archive.handle, &hdr);
        if( rc == ERAR_END_ARCHIVE )
            break;
        if( rc != ERAR_SUCCESS )
            return StatusForRarError(rc, _archive.ctx.password != nullptr);

        Entry entry;
        entry.is_dir = (hdr.Flags & RHDF_DIRECTORY) != 0;
        entry.encrypted = (hdr.Flags & RHDF_ENCRYPTED) != 0;
        entry.redir_type = hdr.RedirType;
        entry.is_link = hdr.RedirType != kRedirNone;
        entry.size = (static_cast<uint64_t>(hdr.UnpSizeHigh) << 32) | hdr.UnpSize;
        if( hdr.RedirType != kRedirNone && hdr.RedirName != nullptr && hdr.RedirName[0] != 0 )
            entry.link_target = Utf32ToUtf8(hdr.RedirName, hdr.RedirNameSize);
        entry.line = BuildEntryLine(hdr, index, entry.link_target);
        _entries.push_back(std::move(entry));

        const int skip_rc = RARProcessFile(_archive.handle, RAR_SKIP, nullptr, nullptr);
        if( skip_rc != ERAR_SUCCESS )
            return StatusForRarError(skip_rc, _archive.ctx.password != nullptr);
    }
    return {};
}

Status
SeekToEntry(Archive &_archive, unsigned int _index, RARHeaderDataEx &_hdr, wchar_t *_link_target, bool _had_password)
{
    for( unsigned int index = 0;; ++index ) {
        _hdr.RedirName = _link_target;
        _hdr.RedirNameSize = 1024;
        _link_target[0] = 0;
        const int rc = RARReadHeaderEx(_archive.handle, &_hdr);
        if( rc == ERAR_END_ARCHIVE )
            return MakeStatus(kExitRead, "no such entry");
        if( rc != ERAR_SUCCESS )
            return StatusForRarError(rc, _had_password);
        if( index == _index )
            return {};
        const int skip_rc = RARProcessFile(_archive.handle, RAR_SKIP, nullptr, nullptr);
        if( skip_rc != ERAR_SUCCESS )
            return StatusForRarError(skip_rc, _had_password);
    }
}

Status ProcessEntryData(Archive &_archive, int _output_fd, bool _had_password)
{
    _archive.ctx.output_fd = _output_fd;
    _archive.ctx.write_failed = false;
    const int rc = RARProcessFile(_archive.handle, RAR_TEST, nullptr, nullptr);
    if( _archive.ctx.write_failed )
        return MakeStatus(kExitRead, "cannot write the entry data");
    if( rc != ERAR_SUCCESS )
        return StatusForRarError(rc, _had_password);
    return {};
}

std::string TempPathTemplate(const char *_prefix)
{
    const char *tmp_dir = std::getenv("TMPDIR");
    std::string path = tmp_dir != nullptr && *tmp_dir != 0 ? tmp_dir : "/tmp";
    if( path.back() != '/' )
        path += '/';
    path += _prefix;
    path += "XXXXXX";
    return path;
}

class TempFile
{
public:
    TempFile() = default;
    TempFile(const TempFile &) = delete;
    TempFile &operator=(const TempFile &) = delete;

    ~TempFile() { Close(); }

    bool Create()
    {
        const std::string path = TempPathTemplate("ncrar-read-");
        std::vector<char> buffer(path.begin(), path.end());
        buffer.push_back(0);
        fd_ = ::mkstemp(buffer.data());
        if( fd_ < 0 )
            return false;
        // Unlink immediately: the descriptor keeps the data alive and no file is left behind,
        // even if the process is killed.
        if( ::unlink(buffer.data()) != 0 ) {
            ::close(fd_);
            fd_ = -1;
            return false;
        }
        return true;
    }

    int Fd() const { return fd_; }

    void Close()
    {
        if( fd_ >= 0 ) {
            ::close(fd_);
            fd_ = -1;
        }
    }

private:
    int fd_ = -1;
};

class TempDir
{
public:
    TempDir() = default;
    TempDir(const TempDir &) = delete;
    TempDir &operator=(const TempDir &) = delete;

    ~TempDir() { Remove(); }

    bool Create()
    {
        const std::string path = TempPathTemplate("ncrar-link-");
        std::vector<char> buffer(path.begin(), path.end());
        buffer.push_back(0);
        char *created = ::mkdtemp(buffer.data());
        if( created == nullptr )
            return false;
        path_ = created;
        return true;
    }

    const std::string &Path() const { return path_; }

    bool Remove()
    {
        if( path_.empty() )
            return true;
        std::error_code error;
        std::filesystem::remove_all(path_, error);
        if( error )
            return false;
        path_.clear();
        return true;
    }

private:
    std::string path_;
};

bool ReadLinkTarget(const std::string &_path, std::string &_target)
{
    size_t size = 256;
    for( ;; ) {
        std::vector<char> buffer(size);
        const ssize_t length = ::readlink(_path.c_str(), buffer.data(), buffer.size());
        if( length < 0 )
            return false;
        if( static_cast<size_t>(length) < size ) {
            _target.assign(buffer.data(), static_cast<size_t>(length));
            return true;
        }
        if( size >= 1U << 20 )
            return false;
        size *= 2;
    }
}

bool FindExtractedLink(const std::string &_dir, std::string &_target)
{
    std::error_code error;
    std::filesystem::recursive_directory_iterator iterator(_dir, error);
    const std::filesystem::recursive_directory_iterator end;
    if( error )
        return false;
    while( iterator != end ) {
        const std::filesystem::directory_entry &entry = *iterator;
        std::error_code type_error;
        if( entry.is_symlink(type_error) && !type_error && ReadLinkTarget(entry.path().string(), _target) )
            return true;
        iterator.increment(error);
        if( error )
            return false;
    }
    return false;
}

Status ResolveLinkTargetByExtraction(Archive &_archive, bool _had_password, std::string &_target)
{
    TempDir temp_dir;
    if( !temp_dir.Create() )
        return MakeStatus(kExitRead, "cannot create a temporary directory");

    _archive.ctx.output_fd = -1;
    _archive.ctx.write_failed = false;
    const int rc = RARProcessFile(_archive.handle, RAR_EXTRACT, const_cast<char *>(temp_dir.Path().c_str()), nullptr);
    if( rc != ERAR_SUCCESS )
        return StatusForRarError(rc, _had_password);

    if( !FindExtractedLink(temp_dir.Path(), _target) )
        return MakeStatus(kExitRead, "cannot determine the link target");

    if( !temp_dir.Remove() )
        return MakeStatus(kExitRead, "cannot remove the temporary directory");
    return {};
}

bool WriteAll(int _fd, const void *_data, size_t _size)
{
    const char *data = static_cast<const char *>(_data);
    while( _size != 0 ) {
        const ssize_t written = ::write(_fd, data, _size);
        if( written < 0 && errno == EINTR )
            continue;
        if( written <= 0 )
            return false;
        data += written;
        _size -= static_cast<size_t>(written);
    }
    return true;
}

struct Output {
    bool failed = false;

    bool Write(std::string_view _text)
    {
        if( failed )
            return false;
        if( !WriteAll(STDOUT_FILENO, _text.data(), _text.size()) ) {
            failed = true;
            return false;
        }
        return true;
    }

    bool WriteLine(std::string_view _line) { return Write(_line) && Write("\n"); }

    void Error(int _code, std::string_view _message)
    {
        std::string line = "{\"ok\":false,\"code\":";
        line += std::to_string(_code);
        line += ",\"error\":";
        AppendJsonString(line, _message);
        line += '}';
        WriteLine(line);
    }

    void Ok() { WriteLine("{\"ok\":true}"); }

    void OkEntries(size_t _count)
    {
        std::string line = "{\"ok\":true,\"entries\":";
        line += std::to_string(_count);
        line += '}';
        WriteLine(line);
    }

    void Done(size_t _count)
    {
        std::string line = "{\"done\":true,\"entries\":";
        line += std::to_string(_count);
        line += '}';
        WriteLine(line);
    }

    void LinkTarget(std::string_view _target)
    {
        std::string line = "{\"target\":";
        AppendJsonString(line, _target);
        line += '}';
        WriteLine(line);
    }
};

bool CopyToStdout(int _fd, Output &_output)
{
    char buffer[64 * 1024];
    for( ;; ) {
        const ssize_t got = ::read(_fd, buffer, sizeof(buffer));
        if( got == 0 )
            return true;
        if( got < 0 ) {
            if( errno == EINTR )
                continue;
            _output.failed = true;
            return false;
        }
        if( !_output.Write(std::string_view(buffer, static_cast<size_t>(got))) )
            return false;
    }
}

// Minimal JSON reader for the line protocol. The helper only accepts small command objects, so
// this is intentionally strict and does not attempt to preserve duplicate keys or number formats.
struct JsonValue {
    enum class Type {
        Null,
        Bool,
        Number,
        String,
        Object,
        Array
    };

    Type type = Type::Null;
    bool boolean = false;
    bool is_integer = false;
    int64_t integer = 0;
    double number = 0;
    std::string string;
    std::vector<std::pair<std::string, JsonValue>> object;
    std::vector<JsonValue> array;

    const JsonValue *Find(std::string_view _key) const
    {
        if( type != Type::Object )
            return nullptr;
        for( const auto &item : object )
            if( item.first == _key )
                return &item.second;
        return nullptr;
    }
};

class JsonParser
{
public:
    explicit JsonParser(std::string_view _text) : text_(_text) {}

    bool Parse(JsonValue &_out)
    {
        SkipSpace();
        if( !ParseValue(_out, 0) )
            return false;
        SkipSpace();
        return pos_ == text_.size();
    }

private:
    bool ParseValue(JsonValue &_out, int _depth)
    {
        if( _depth > 64 )
            return false;
        SkipSpace();
        if( pos_ >= text_.size() )
            return false;
        switch( text_[pos_] ) {
            case '{':
                return ParseObject(_out, _depth);
            case '[':
                return ParseArray(_out, _depth);
            case '"':
                _out = JsonValue();
                _out.type = JsonValue::Type::String;
                return ParseString(_out.string);
            case 't':
                return ParseLiteral("true", _out, true);
            case 'f':
                return ParseLiteral("false", _out, false);
            case 'n':
                return ParseLiteral("null", _out, false);
            default:
                return ParseNumber(_out);
        }
    }

    bool ParseLiteral(std::string_view _literal, JsonValue &_out, bool _boolean)
    {
        if( text_.substr(pos_, _literal.size()) != _literal )
            return false;
        pos_ += _literal.size();
        _out = JsonValue();
        if( _literal == "null" )
            return true;
        _out.type = JsonValue::Type::Bool;
        _out.boolean = _boolean;
        return true;
    }

    bool ParseNumber(JsonValue &_out)
    {
        const size_t start = pos_;
        if( Peek() == '-' )
            ++pos_;
        if( Peek() == '0' ) {
            ++pos_;
        }
        else if( IsDigit(Peek()) ) {
            while( IsDigit(Peek()) )
                ++pos_;
        }
        else {
            return false;
        }

        bool is_integer = true;
        if( Peek() == '.' ) {
            is_integer = false;
            ++pos_;
            if( !IsDigit(Peek()) )
                return false;
            while( IsDigit(Peek()) )
                ++pos_;
        }
        if( Peek() == 'e' || Peek() == 'E' ) {
            is_integer = false;
            ++pos_;
            if( Peek() == '+' || Peek() == '-' )
                ++pos_;
            if( !IsDigit(Peek()) )
                return false;
            while( IsDigit(Peek()) )
                ++pos_;
        }

        const std::string text(text_.substr(start, pos_ - start));
        _out = JsonValue();
        _out.type = JsonValue::Type::Number;
        if( is_integer ) {
            errno = 0;
            char *end = nullptr;
            const long long value = std::strtoll(text.c_str(), &end, 10);
            if( errno == 0 && end != nullptr && *end == 0 ) {
                _out.is_integer = true;
                _out.integer = value;
                _out.number = static_cast<double>(value);
                return true;
            }
        }
        errno = 0;
        char *end = nullptr;
        const double value = std::strtod(text.c_str(), &end);
        if( errno != 0 || end == nullptr || *end != 0 )
            return false;
        _out.number = value;
        return true;
    }

    bool ParseString(std::string &_out)
    {
        if( Peek() != '"' )
            return false;
        ++pos_;
        _out.clear();
        while( pos_ < text_.size() ) {
            const unsigned char c = static_cast<unsigned char>(text_[pos_++]);
            if( c == '"' )
                return true;
            if( c < 0x20 )
                return false;
            if( c != '\\' ) {
                _out += static_cast<char>(c);
                continue;
            }
            if( pos_ >= text_.size() )
                return false;
            const char escaped = text_[pos_++];
            switch( escaped ) {
                case '"':
                    _out += '"';
                    break;
                case '\\':
                    _out += '\\';
                    break;
                case '/':
                    _out += '/';
                    break;
                case 'b':
                    _out += '\b';
                    break;
                case 'f':
                    _out += '\f';
                    break;
                case 'n':
                    _out += '\n';
                    break;
                case 'r':
                    _out += '\r';
                    break;
                case 't':
                    _out += '\t';
                    break;
                case 'u': {
                    uint32_t cp = 0;
                    if( !ParseHex4(cp) )
                        return false;
                    if( cp >= 0xD800 && cp <= 0xDBFF ) {
                        if( pos_ + 1 >= text_.size() || text_[pos_] != '\\' || text_[pos_ + 1] != 'u' )
                            return false;
                        pos_ += 2;
                        uint32_t low = 0;
                        if( !ParseHex4(low) || low < 0xDC00 || low > 0xDFFF )
                            return false;
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                    }
                    else if( cp >= 0xDC00 && cp <= 0xDFFF ) {
                        return false;
                    }
                    AppendUtf8(_out, cp);
                    break;
                }
                default:
                    return false;
            }
        }
        return false;
    }

    bool ParseHex4(uint32_t &_out)
    {
        if( pos_ + 4 > text_.size() )
            return false;
        uint32_t value = 0;
        for( int i = 0; i != 4; ++i ) {
            const char c = text_[pos_++];
            value <<= 4;
            if( c >= '0' && c <= '9' )
                value |= static_cast<uint32_t>(c - '0');
            else if( c >= 'a' && c <= 'f' )
                value |= static_cast<uint32_t>(c - 'a' + 10);
            else if( c >= 'A' && c <= 'F' )
                value |= static_cast<uint32_t>(c - 'A' + 10);
            else
                return false;
        }
        _out = value;
        return true;
    }

    bool ParseObject(JsonValue &_out, int _depth)
    {
        _out = JsonValue();
        _out.type = JsonValue::Type::Object;
        ++pos_;
        SkipSpace();
        if( Peek() == '}' ) {
            ++pos_;
            return true;
        }
        for( ;; ) {
            SkipSpace();
            std::string key;
            if( !ParseString(key) )
                return false;
            SkipSpace();
            if( Peek() != ':' )
                return false;
            ++pos_;
            JsonValue value;
            if( !ParseValue(value, _depth + 1) )
                return false;
            _out.object.emplace_back(std::move(key), std::move(value));
            SkipSpace();
            if( Peek() == '}' ) {
                ++pos_;
                return true;
            }
            if( Peek() != ',' )
                return false;
            ++pos_;
        }
    }

    bool ParseArray(JsonValue &_out, int _depth)
    {
        _out = JsonValue();
        _out.type = JsonValue::Type::Array;
        ++pos_;
        SkipSpace();
        if( Peek() == ']' ) {
            ++pos_;
            return true;
        }
        for( ;; ) {
            JsonValue value;
            if( !ParseValue(value, _depth + 1) )
                return false;
            _out.array.push_back(std::move(value));
            SkipSpace();
            if( Peek() == ']' ) {
                ++pos_;
                return true;
            }
            if( Peek() != ',' )
                return false;
            ++pos_;
        }
    }

    void SkipSpace()
    {
        while( pos_ < text_.size() ) {
            const char c = text_[pos_];
            if( c != ' ' && c != '\t' && c != '\n' && c != '\r' )
                break;
            ++pos_;
        }
    }

    static bool IsDigit(char _c) { return _c >= '0' && _c <= '9'; }

    char Peek() const { return pos_ < text_.size() ? text_[pos_] : '\0'; }

    std::string_view text_;
    size_t pos_ = 0;
};

struct Session {
    bool open = false;
    bool has_password = false;
    std::string archive_path;
    std::string password;
    std::vector<Entry> entries;

    void Clear()
    {
        open = false;
        has_password = false;
        archive_path.clear();
        password.clear();
        entries.clear();
    }
};

const char *PasswordPtr(const Session &_session)
{
    return _session.has_password ? _session.password.c_str() : nullptr;
}

Status
LoadListing(const std::string &_archive_path, const char *_password, bool _had_password, std::vector<Entry> &_entries)
{
    Archive archive;
    if( !archive.Open(_archive_path.c_str(), RAR_OM_LIST, _password) )
        return StatusForOpenResult(archive.open_result, _had_password);
    return CollectEntries(archive, _entries);
}

bool ParseEntryIndex(const JsonValue &_command, size_t _count, unsigned int &_index, Output &_output)
{
    const JsonValue *value = _command.Find("index");
    if( value == nullptr || value->type != JsonValue::Type::Number || !value->is_integer ) {
        _output.Error(kExitUsage, "the entry index must be an integer");
        return false;
    }
    if( value->integer < 0 || static_cast<uint64_t>(value->integer) >= _count ) {
        _output.Error(kExitRead, "no such entry");
        return false;
    }
    _index = static_cast<unsigned int>(value->integer);
    return true;
}

void ServeRead(Session &_session, unsigned int _index, Output &_output)
{
    const Entry &entry = _session.entries[_index];
    if( entry.is_dir ) {
        _output.Error(kExitRead, "the entry is a directory");
        return;
    }
    if( entry.is_link ) {
        // Links carry no payload. Their target is served by the 'link' command.
        _output.WriteLine("{\"event\":\"data\",\"size\":0}");
        return;
    }

    Archive archive;
    if( !archive.Open(_session.archive_path.c_str(), RAR_OM_EXTRACT, PasswordPtr(_session)) ) {
        const Status status = StatusForOpenResult(archive.open_result, _session.has_password);
        _output.Error(status.code, status.message);
        return;
    }

    RARHeaderDataEx hdr{};
    wchar_t link_target[1024]{};
    Status status = SeekToEntry(archive, _index, hdr, link_target, _session.has_password);
    if( !status.Ok() ) {
        _output.Error(status.code, status.message);
        return;
    }
    if( hdr.RedirType != kRedirNone ) {
        _output.WriteLine("{\"event\":\"data\",\"size\":0}");
        return;
    }

    TempFile temp_file;
    if( !temp_file.Create() ) {
        _output.Error(kExitRead, "cannot create a temporary file");
        return;
    }
    status = ProcessEntryData(archive, temp_file.Fd(), _session.has_password);
    if( !status.Ok() ) {
        _output.Error(status.code, status.message);
        return;
    }

    struct stat info{};
    if( ::fstat(temp_file.Fd(), &info) != 0 ) {
        _output.Error(kExitRead, "cannot read the entry data");
        return;
    }
    if( ::lseek(temp_file.Fd(), 0, SEEK_SET) < 0 ) {
        _output.Error(kExitRead, "cannot read the entry data");
        return;
    }

    const uint64_t size = static_cast<uint64_t>(info.st_size);
    std::string header = "{\"event\":\"data\",\"size\":";
    header += std::to_string(size);
    header += '}';
    if( !_output.WriteLine(header) )
        return;
    CopyToStdout(temp_file.Fd(), _output);
}

void ServeLink(const Session &_session, unsigned int _index, Output &_output)
{
    const Entry &entry = _session.entries[_index];
    if( !IsLinkType(entry.redir_type) ) {
        _output.Error(kExitRead, "the entry is not a link");
        return;
    }
    if( !entry.link_target.empty() ) {
        _output.LinkTarget(entry.link_target);
        return;
    }

    Archive archive;
    if( !archive.Open(_session.archive_path.c_str(), RAR_OM_EXTRACT, PasswordPtr(_session)) ) {
        const Status status = StatusForOpenResult(archive.open_result, _session.has_password);
        _output.Error(status.code, status.message);
        return;
    }

    RARHeaderDataEx hdr{};
    wchar_t link_target[1024]{};
    Status status = SeekToEntry(archive, _index, hdr, link_target, _session.has_password);
    if( !status.Ok() ) {
        _output.Error(status.code, status.message);
        return;
    }
    if( !IsLinkType(hdr.RedirType) ) {
        _output.Error(kExitRead, "the entry is not a link");
        return;
    }

    std::string target = hdr.RedirName != nullptr ? Utf32ToUtf8(hdr.RedirName, hdr.RedirNameSize) : std::string{};
    if( target.empty() ) {
        // RAR4 unix symlinks keep the target in the entry payload. Extract into an anonymous
        // temporary directory, read the created symlink, and remove the directory again.
        status = ResolveLinkTargetByExtraction(archive, _session.has_password, target);
        if( !status.Ok() ) {
            _output.Error(status.code, status.message);
            return;
        }
    }
    _output.LinkTarget(target);
}

bool HandleServeLine(Session &_session, Output &_output, const std::string &_line)
{
    JsonValue command;
    if( !JsonParser(_line).Parse(command) || command.type != JsonValue::Type::Object ) {
        _output.Error(kExitUsage, "malformed command");
        return true;
    }

    const JsonValue *cmd_value = command.Find("cmd");
    if( cmd_value == nullptr || cmd_value->type != JsonValue::Type::String ) {
        _output.Error(kExitUsage, "missing cmd");
        return true;
    }
    const std::string &cmd = cmd_value->string;

    if( cmd == "quit" )
        return false;

    if( cmd == "open" ) {
        const JsonValue *archive_value = command.Find("archive");
        if( archive_value == nullptr || archive_value->type != JsonValue::Type::String ||
            archive_value->string.empty() ) {
            _output.Error(kExitUsage, "open requires an archive path");
            return true;
        }
        std::string password;
        bool has_password = false;
        const JsonValue *password_value = command.Find("password");
        if( password_value != nullptr && password_value->type != JsonValue::Type::Null ) {
            if( password_value->type != JsonValue::Type::String ) {
                _output.Error(kExitUsage, "the password must be a string");
                return true;
            }
            password = password_value->string;
            has_password = !password.empty();
        }

        _session.Clear();
        std::vector<Entry> entries;
        Status status =
            LoadListing(archive_value->string, has_password ? password.c_str() : nullptr, has_password, entries);
        if( !status.Ok() ) {
            _output.Error(status.code, status.message);
            return true;
        }
        bool encrypted = false;
        for( const Entry &entry : entries ) {
            if( entry.encrypted ) {
                encrypted = true;
                break;
            }
        }
        if( encrypted && !has_password ) {
            _output.Error(kExitPassword, "a password is required");
            return true;
        }

        _session.open = true;
        _session.has_password = has_password;
        _session.archive_path = archive_value->string;
        _session.password = std::move(password);
        _session.entries = std::move(entries);
        _output.OkEntries(_session.entries.size());
        return true;
    }

    if( cmd == "list" ) {
        if( !_session.open ) {
            _output.Error(kExitUsage, "no archive is open");
            return true;
        }
        for( const Entry &entry : _session.entries )
            _output.WriteLine(entry.line);
        _output.Done(_session.entries.size());
        return true;
    }

    if( cmd == "read" ) {
        if( !_session.open ) {
            _output.Error(kExitUsage, "no archive is open");
            return true;
        }
        unsigned int index = 0;
        if( !ParseEntryIndex(command, _session.entries.size(), index, _output) )
            return true;
        ServeRead(_session, index, _output);
        return true;
    }

    if( cmd == "link" ) {
        if( !_session.open ) {
            _output.Error(kExitUsage, "no archive is open");
            return true;
        }
        unsigned int index = 0;
        if( !ParseEntryIndex(command, _session.entries.size(), index, _output) )
            return true;
        ServeLink(_session, index, _output);
        return true;
    }

    if( cmd == "close" ) {
        _session.Clear();
        _output.Ok();
        return true;
    }

    _output.Error(kExitUsage, "unknown command");
    return true;
}

int CommandServe()
{
    Session session;
    Output output;
    std::string line;
    while( !output.failed && std::getline(std::cin, line) ) {
        if( !HandleServeLine(session, output, line) )
            break;
    }
    return output.failed ? kExitRead : kExitOk;
}

int Fail(const char *_what, int _code)
{
    std::fprintf(stderr, "ncrar-helper: %s (code %d)\n", _what, _code);
    return _code;
}

int FailStatus(const Status &_status)
{
    return Fail(_status.message.c_str(), _status.code);
}

int CommandVersion()
{
    std::printf("{\"protocol\":1,\"unrar\":%d}\n", RARGetDllVersion());
    return kExitOk;
}

int CommandList(const char *_archive_path, const char *_password)
{
    Archive archive;
    if( !archive.Open(_archive_path, RAR_OM_LIST, _password) )
        return FailStatus(StatusForOpenResult(archive.open_result, _password != nullptr));

    std::vector<Entry> entries;
    const Status status = CollectEntries(archive, entries);
    if( !status.Ok() )
        return FailStatus(status);

    for( const Entry &entry : entries ) {
        std::fwrite(entry.line.data(), 1, entry.line.size(), stdout);
        std::fputc('\n', stdout);
    }
    std::printf("{\"done\":true,\"entries\":%zu}\n", entries.size());
    if( std::fflush(stdout) != 0 )
        return Fail("cannot write to stdout", kExitRead);
    return kExitOk;
}

int CommandRead(const char *_archive_path, unsigned int _entry_index, const char *_password)
{
    Archive archive;
    if( !archive.Open(_archive_path, RAR_OM_EXTRACT, _password) )
        return FailStatus(StatusForOpenResult(archive.open_result, _password != nullptr));

    RARHeaderDataEx hdr{};
    wchar_t link_target[1024]{};
    const Status status = SeekToEntry(archive, _entry_index, hdr, link_target, _password != nullptr);
    if( !status.Ok() )
        return FailStatus(status);

    if( (hdr.Flags & RHDF_DIRECTORY) != 0 )
        return Fail("the entry is a directory", kExitRead);
    // Symlinks, junctions and hardlinks carry no payload - their target is in the listing.
    if( hdr.RedirType != kRedirNone )
        return kExitOk;

    const Status process_status = ProcessEntryData(archive, STDOUT_FILENO, _password != nullptr);
    if( !process_status.Ok() )
        return FailStatus(process_status);
    return kExitOk;
}

int CommandLink(const char *_archive_path, unsigned int _entry_index, const char *_password)
{
    Archive archive;
    if( !archive.Open(_archive_path, RAR_OM_EXTRACT, _password) )
        return FailStatus(StatusForOpenResult(archive.open_result, _password != nullptr));

    RARHeaderDataEx hdr{};
    wchar_t link_target[1024]{};
    Status status = SeekToEntry(archive, _entry_index, hdr, link_target, _password != nullptr);
    if( !status.Ok() )
        return FailStatus(status);

    if( !IsLinkType(hdr.RedirType) )
        return Fail("the entry is not a link", kExitRead);

    std::string target = hdr.RedirName != nullptr ? Utf32ToUtf8(hdr.RedirName, hdr.RedirNameSize) : std::string{};
    if( target.empty() ) {
        status = ResolveLinkTargetByExtraction(archive, _password != nullptr, target);
        if( !status.Ok() )
            return FailStatus(status);
    }

    std::string line = "{\"target\":";
    AppendJsonString(line, target);
    line += "}\n";
    std::fwrite(line.data(), 1, line.size(), stdout);
    if( std::fflush(stdout) != 0 )
        return Fail("cannot write to stdout", kExitRead);
    return kExitOk;
}

std::string ReadPasswordFromStdin()
{
    std::string password;
    char ch = 0;
    while( std::fread(&ch, 1, 1, stdin) == 1 ) {
        if( ch == '\n' )
            break;
        password += ch;
    }
    return password;
}

const char *PasswordFromEnv()
{
    return std::getenv("NCRAR_PASSWORD");
}

void Usage()
{
    std::fprintf(stderr,
                 "usage: ncrar-helper <command> [args]\n"
                 "  version\n"
                 "  serve\n"
                 "  list <archive>\n"
                 "  read <archive> <entry-index>\n"
                 "  link <archive> <entry-index>\n"
                 "The password is read from NCRAR_PASSWORD, or from stdin when --password-stdin is "
                 "given.\n");
}

} // namespace

int main(int _argc, char **_argv)
{
    std::vector<std::string> args(_argv + 1, _argv + _argc);
    if( args.empty() ) {
        Usage();
        return kExitUsage;
    }

    bool password_stdin = false;
    std::vector<std::string> rest;
    for( const std::string &arg : args ) {
        if( arg == "--password-stdin" )
            password_stdin = true;
        else
            rest.push_back(arg);
    }

    const std::string command = rest.empty() ? std::string{} : rest[0];
    if( command == "serve" ) {
        if( rest.size() != 1 || password_stdin ) {
            Usage();
            return kExitUsage;
        }
        return CommandServe();
    }

    std::string password;
    const char *password_ptr = PasswordFromEnv();
    if( password_stdin ) {
        password = ReadPasswordFromStdin();
        password_ptr = password.empty() ? nullptr : password.c_str();
    }

    if( command == "version" )
        return CommandVersion();
    if( command == "list" && rest.size() == 2 )
        return CommandList(rest[1].c_str(), password_ptr);
    if( command == "read" && rest.size() == 3 ) {
        try {
            const unsigned int index = static_cast<unsigned int>(std::stoul(rest[2]));
            return CommandRead(rest[1].c_str(), index, password_ptr);
        } catch( const std::exception & ) {
            return Fail("invalid entry index", kExitUsage);
        }
    }
    if( command == "link" && rest.size() == 3 ) {
        try {
            const unsigned int index = static_cast<unsigned int>(std::stoul(rest[2]));
            return CommandLink(rest[1].c_str(), index, password_ptr);
        } catch( const std::exception & ) {
            return Fail("invalid entry index", kExitUsage);
        }
    }

    Usage();
    return kExitUsage;
}
