// A stream over a Windows anonymous pipe. The browser keeps its side open after
// sending one message, so a host that waits for a full block never answers; this
// stream reproduces that shape in process, without a second executable.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdio>
#include <iostream>
#include <streambuf>

namespace litefetch::test {

class PipeStreamBuf : public std::streambuf {
public:
    explicit PipeStreamBuf(HANDLE read) : read_(read) {}
    ~PipeStreamBuf() override
    {
        if (read_ != INVALID_HANDLE_VALUE) CloseHandle(read_);
    }

protected:
    int_type underflow() override
    {
        DWORD got = 0;
        if (!ReadFile(read_, &current_, 1, &got, nullptr) || got == 0) return traits_type::eof();
        setg(&current_, &current_, &current_ + 1);
        return traits_type::to_int_type(current_);
    }

private:
    HANDLE read_ = INVALID_HANDLE_VALUE;
    char current_ = 0;
};

} // namespace litefetch::test
