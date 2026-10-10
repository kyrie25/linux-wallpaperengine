#pragma once

#include <iostream>
#include <memory>

namespace WallpaperEngine::Data::Utils {
struct MemoryStream : std::istream, private std::streambuf {
    MemoryStream (std::unique_ptr<char[]> buffer, const size_t size) :
	std::istream (this), m_buffer (std::move (buffer)) {
	this->setg (this->m_buffer.get (), this->m_buffer.get (), this->m_buffer.get () + size);
    }

    std::streambuf::pos_type
    seekoff (std::streambuf::off_type off, std::ios_base::seekdir dir, std::ios_base::openmode which) override {
	if (dir == std::ios_base::cur) {
	    if (off < -(gptr () - eback ()) || off > egptr () - gptr ()) return std::streambuf::pos_type (std::streambuf::off_type (-1));
	    setg (eback (), gptr () + off, egptr ());
	} else if (dir == std::ios_base::end) {
	    if (off > 0 || off < -(egptr () - eback ())) return std::streambuf::pos_type (std::streambuf::off_type (-1));
	    setg (eback (), egptr () + off, egptr ());
	} else if (dir == std::ios_base::beg) {
	    if (off < 0 || off > egptr () - eback ()) return std::streambuf::pos_type (std::streambuf::off_type (-1));
	    setg (eback (), eback () + off, egptr ());
	}
	return gptr () - eback ();
    }

    std::streambuf::pos_type seekpos (std::streambuf::pos_type pos, std::ios_base::openmode which) override {
	return seekoff (std::streambuf::off_type (pos), std::ios_base::beg, which);
    }

    std::unique_ptr<char[]> m_buffer;
};

using MemoryStreamSharedPtr = std::shared_ptr<MemoryStream>;
using MemoryStreamUniquePtr = std::unique_ptr<MemoryStream>;
}
