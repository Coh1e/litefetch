# Project dependency restriction

Based on the libtorrent 2.1.2 recipe at vcpkg baseline
e182cb4dd2df2ab02f66a1aabd5f35bbdc9522c7. Source hashes and patches are unchanged.
Only Boost dependency declarations differ: declare the observed MSVC header
modules, omit unused legacy-module roots, and disable Asio default features.
The application manifest also explicitly requests Asio without default features.

No WebTorrent, Python, examples, tools or deprecated ABI features are enabled.
Independent Debug/Release builds and actual local BT v1/v2/hybrid, metadata,
resume and file-selection checks passed before adoption. This is a validated
reduced closure, not a claim of a mathematically minimal Boost implementation.
See analysis/notes/libtorrent-minimal-boost.md in the workspace for evidence.
