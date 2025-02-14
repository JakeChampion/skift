#include <karm-base/func.h>
#include <karm-base/res.h>
#include <karm-base/time.h>

#include <karm-sys/_embed.h>

#include "externs.h"

namespace Karm::Sys::_Embed {
struct JSConsole : public Sys::Fd {
    enum Proto {
        LOG,
        ERROR
    } _proto;

    Io::BufferWriter _buf;

    JSConsole(Proto proto) : _proto(proto) {}

    Sys::Handle handle() const override {
        return Handle{(usize)_proto};
    }

    Res<usize> read(MutBytes) override {
        notImplemented();
    }

    Res<usize> write(Bytes bytes) override {
        return _buf.write(bytes);
    }

    Res<usize> seek(Io::Seek) override {
        notImplemented();
    }

    Res<usize> flush() override {
        switch (_proto) {
        case LOG:
            embedConsoleLog(_buf.bytes().buf(), _buf.bytes().len());
            break;
        case ERROR:
            embedConsoleError(_buf.bytes().buf(), _buf.bytes().len());
            break;
        }

        return _buf.flush();
    }

    Res<Strong<Fd>> dup() override {
        notImplemented();
    }

    Res<_Accepted> accept() override {
        notImplemented();
    }

    Res<Stat> stat() override {
        return Ok<Stat>();
    }

    Res<_Sent> send(Bytes, Slice<Handle>, SocketAddr) override {
        notImplemented();
    }

    Res<_Received> recv(MutBytes, MutSlice<Handle>) override {
        notImplemented();
    }

    Res<> pack(Io::PackEmit &) override {
        notImplemented();
    }
};

TimeStamp now() {
    auto span = embedGetTimeStamp();
    return TimeStamp::epoch() + TimeSpan::fromMSecs(span);
}

Res<Strong<Sys::Fd>> createIn() {
    return Ok(makeStrong<Sys::NullFd>());
}

Res<Strong<Sys::Fd>> createOut() {
    return Ok(makeStrong<JSConsole>(JSConsole::LOG));
}

Res<Strong<Sys::Fd>> createErr() {
    return Ok(makeStrong<JSConsole>(JSConsole::ERROR));
}

Res<Strong<Sys::Fd>> unpackFd(Io::PackScan &) {
    notImplemented();
}

// MARK: System Informations ---------------------------------------------------

Res<> populate(Sys::SysInfo &) {
    return Ok();
}

Res<> populate(Sys::MemInfo &mem) {
    mem.physicalTotal = (usize)__heap_end - (usize)__heap_base;
    mem.physicalUsed = -1;
    mem.swapTotal = 0;
    mem.swapUsed = 0;
    mem.virtualTotal = 0;
    mem.virtualUsed = 0;
    return Ok();
}

Res<> populate(Vec<Sys::CpuInfo> &) {
    return Ok();
}

Res<> populate(Sys::UserInfo &) {
    return Ok();
}

Res<> populate(Vec<Sys::UserInfo> &) {
    return Ok();
}
} // namespace Karm::Sys::_Embed