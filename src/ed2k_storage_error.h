#pragma once
#include <stdexcept>
namespace downloader::ed2k {
class StorageError : public std::runtime_error {public:using std::runtime_error::runtime_error;};
class DestinationError : public std::runtime_error {public:using std::runtime_error::runtime_error;};
}
