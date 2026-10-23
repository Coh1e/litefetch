# 当前下载器依赖清单

核对日期：2026-10-20。只计入vcpkg状态为install ok installed的包；已移除的purge记录不计入。已安装不等于实际链接。此前将9个已移除实验包误报为仍安装，已更正。

主程序直接使用Qt Core/Gui/Widgets/Network 6.11.2、libcurl 8.22.0、SQLite 3.53.4、libtorrent-rasterbar 2.1.2。libtorrent带入Boost 1.92.0和OpenSSL 3.6.5；curl使用Windows Schannel。Qt及传输附属库包括zlib、libpng、PCRE2、double-conversion、md4c。

wxWidgets、Crypto++、Expat、libjpeg-turbo、libspng、liblzma、libwebp、NanoSVG、TIFF已从installed移除；历史源码、构建缓存与实验记录不代表产品依赖。产品manifest、主程序链接规则与部署包无wxWidgets。aMule仅为既有实验工具，未接入主程序或发布包。后续不新增wxWidgets，新增Boost用途需先说明。

Boost为libtorrent的传递依赖。当前安装的boost前缀包数：44，包含头文件子包及构建辅助；不等于同等数量的运行时DLL。

## 全部实际安装包

| 包 | 版本 |
|---|---|
| boost-align | 1.92.0 |
| boost-array | 1.92.0 |
| boost-asio | 1.92.0 |
| boost-assert | 1.92.0 |
| boost-bind | 1.92.0 |
| boost-cmake | 1.92.0 |
| boost-compat | 1.92.0 |
| boost-concept-check | 1.92.0 |
| boost-config | 1.92.0 |
| boost-container | 1.92.0 |
| boost-container-hash | 1.92.0 |
| boost-conversion | 1.92.0 |
| boost-core | 1.92.0 |
| boost-crc | 1.92.0 |
| boost-describe | 1.92.0 |
| boost-detail | 1.92.0 |
| boost-function-types | 1.92.0 |
| boost-fusion | 1.92.0 |
| boost-headers | 1.92.0 |
| boost-integer | 1.92.0 |
| boost-intrusive | 1.92.0 |
| boost-io | 1.92.0 |
| boost-iterator | 1.92.0 |
| boost-logic | 1.92.0 |
| boost-move | 1.92.0 |
| boost-mp11 | 1.92.0 |
| boost-mpl | 1.92.0 |
| boost-multi-index | 1.92.0 |
| boost-optional | 1.92.0 |
| boost-pool | 1.92.0 |
| boost-predef | 1.92.0 |
| boost-preprocessor | 1.92.0 |
| boost-range | 1.92.0 |
| boost-regex | 1.92.0 |
| boost-smart-ptr | 1.92.0 |
| boost-system | 1.92.0 |
| boost-throw-exception | 1.92.0 |
| boost-tuple | 1.92.0 |
| boost-type-traits | 1.92.0 |
| boost-typeof | 1.92.0 |
| boost-uninstall | 1.92.0 |
| boost-utility | 1.92.0 |
| boost-variant2 | 1.92.0 |
| boost-winapi | 1.92.0 |
| curl | 8.22.0 |
| double-conversion | 3.4.0 |
| egl-registry | 2025-05-27 |
| libpng | 1.6.58 |
| libtorrent | 2.1.2 |
| md4c | 0.6.0 |
| opengl | 2022-12-04 |
| opengl-registry | 2026-08-03 |
| openssl | 3.6.5 |
| pcre2 | 10.47 |
| qtbase | 6.11.2 |
| sqlite3 | 3.53.4 |
| vcpkg-boost | 2026-08-21 |
| vcpkg-cmake | 2025-08-07 |
| vcpkg-cmake-config | 2026-07-21 |
| vcpkg-cmake-get-vars | 2025-05-29 |
| zlib | 1.3.2 |

## 当前部署DLL

- concrt140d.dll
- d3dcompiler_47.dll
- double-conversion.dll
- dxcompiler.dll
- dxil.dll
- libcrypto-3-x64.dll
- libcurl-d.dll
- libpng16d.dll
- libssl-3-x64.dll
- md4c.dll
- msvcp140_1d.dll
- msvcp140_2d.dll
- msvcp140d_atomic_wait.dll
- msvcp140d_codecvt_ids.dll
- msvcp140d.dll
- pcre2-16d.dll
- Qt6Cored.dll
- Qt6Guid.dll
- Qt6Networkd.dll
- Qt6Widgetsd.dll
- sqlite3.dll
- torrent-rasterbar.dll
- vccorlib140d.dll
- vcruntime140_1d.dll
- vcruntime140_threadsd.dll
- vcruntime140d.dll
- zd.dll
