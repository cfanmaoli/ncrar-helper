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
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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

        std::string target;
        if( hdr.RedirType != kRedirNone && hdr.RedirName != nullptr && hdr.RedirName[0] != 0 )
            target = Utf32ToUtf8(hdr.RedirName, hdr.RedirNameSize);

        Entry entry;
        entry.line = BuildEntryLine(hdr, index, target);
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
