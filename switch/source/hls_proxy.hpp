#pragma once

#include <string>
#include <vector>

// Local repair proxy for HLS streams whose media segments are disguised as
// images (some anime CDNs prepend fake image bytes/headers to every segment,
// which libmpv/FFmpeg then fails to demux). Ported from AnikkuNX's proven
// net/hls_proxy implementation:
//   - needsProxy() probes the stream first; it only returns true when the
//     playlist is real HLS and the first segment really looks like an image.
//   - wrap() serves the stream through 127.0.0.1, fetching upstream with the
//     provider's headers and stripping the fake bytes from every segment.
// If the probe fails for any reason, playback must use the original URL.

namespace hlsproxy {

// True when the stream should be wrapped through the repair proxy.
// Never throws; false on any error or when the stream is healthy.
bool needsProxy(const std::string& url, const std::vector<std::string>& headers);

// Start the local proxy and return the wrapped playlist URL for mpv.
// Empty string on failure (caller must fall back to the original URL).
std::string wrap(const std::string& url, const std::vector<std::string>& headers);

// Where the proxy writes its own log ("sdmc:/switch/SaikouTV/proxy.log").
void setLogPath(const std::string& path);

} // namespace hlsproxy
