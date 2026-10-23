#pragma once
#include "transfer.h"
#include <libtorrent/error_code.hpp>
#include <libtorrent/torrent_status.hpp>

namespace downloader {
inline TransferError classifyTorrentError(const libtorrent::error_code& code,
    libtorrent::file_index_t file=libtorrent::torrent_status::error_file_none)
{
    namespace lt=libtorrent;
    namespace system=boost::system::errc;
    if(!code)return TransferError::None;
    if(file==lt::torrent_status::error_file_ssl_ctx)return TransferError::Tls;
    if(code==lt::errors::file_collision)return TransferError::Destination;
    if(code==lt::errors::file_too_short)return TransferError::ResourceChanged;
    if(code==system::no_space_on_device||code==system::permission_denied||code==system::read_only_file_system
        ||code==system::io_error||code==system::file_too_large)return TransferError::Disk;
    if(static_cast<int>(file)>=0||file==lt::torrent_status::error_file_partfile)return TransferError::Disk;
    if(file==lt::torrent_status::error_file_exception)return TransferError::Internal;
    if(file==lt::torrent_status::error_file_metadata||code==lt::errors::duplicate_torrent
        ||code==lt::errors::invalid_torrent_handle||code==lt::errors::invalid_swarm_metadata
        ||code==lt::errors::invalid_bencoding)return TransferError::InvalidRequest;
    if(code==system::connection_refused||code==system::connection_reset||code==system::connection_aborted
        ||code==system::timed_out||code==system::host_unreachable||code==system::network_unreachable)
        return TransferError::Network;
    return TransferError::Internal;
}
}
