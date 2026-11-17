// PS3 port: minimal stand-in for <curl/curl.h>.
// The PS3 build only does LAN streaming, so the PSN/AIA code paths that
// really use libcurl are not compiled. Some core headers still name CURL.
#ifndef CHIAKI_PS3_CURL_STUB_H
#define CHIAKI_PS3_CURL_STUB_H
typedef void CURL;
#endif
