#include "torrent_error.h"
#include <iostream>
int main()
{
    namespace lt=libtorrent;namespace system=boost::system::errc;
    using downloader::TransferError;using downloader::classifyTorrentError;
    if(classifyTorrentError({})!=TransferError::None)return 1;
    if(classifyTorrentError(system::make_error_code(system::no_space_on_device))!=TransferError::Disk)return 2;
    if(classifyTorrentError(system::make_error_code(system::connection_reset))!=TransferError::Network)return 3;
    if(classifyTorrentError(system::make_error_code(system::io_error),lt::torrent_status::error_file_ssl_ctx)!=TransferError::Tls)return 4;
    if(classifyTorrentError(lt::errors::invalid_bencoding,lt::torrent_status::error_file_metadata)!=TransferError::InvalidRequest)return 5;
    if(classifyTorrentError(lt::errors::file_collision)!=TransferError::Destination)return 6;
    if(classifyTorrentError(lt::errors::file_too_short)!=TransferError::ResourceChanged)return 7;
    if(classifyTorrentError(lt::errors::invalid_entry_type,lt::file_index_t{2})!=TransferError::Disk)return 8;
    if(classifyTorrentError(lt::errors::invalid_entry_type,lt::torrent_status::error_file_partfile)!=TransferError::Disk)return 9;
    if(classifyTorrentError(lt::errors::invalid_entry_type)!=TransferError::Internal)return 10;
    std::cout<<"BT errors distinguish storage, transport, TLS, metadata and unknown failures\n";
}
