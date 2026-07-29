// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>

#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "http.h"

namespace upnp {

namespace {

std::string ToLower(std::string s)
{
	std::transform(s.begin(), s.end(), s.begin(),
		[](unsigned char c) { return (char)std::tolower(c); });
	return s;
}

// Connects with a timeout; returns the fd or -1.
int ConnectTcp(const std::string& host, uint16_t port, int timeout_ms, std::string& error)
{
	addrinfo hints = {};
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;

	addrinfo* res = nullptr;
	const std::string portstr = std::to_string(port);
	if (getaddrinfo(host.c_str(), portstr.c_str(), &hints, &res) != 0 || !res)
	{
		error = "cannot resolve " + host;
		return -1;
	}

	int fd = -1;
	for (addrinfo* ai = res; ai; ai = ai->ai_next)
	{
		fd = socket(ai->ai_family, ai->ai_socktype | SOCK_CLOEXEC, ai->ai_protocol);
		if (fd < 0)
			continue;

		fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
		if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0)
			break;
		if (errno == EINPROGRESS)
		{
			pollfd pfd = {fd, POLLOUT, 0};
			if (poll(&pfd, 1, timeout_ms) == 1)
			{
				int err = 0;
				socklen_t len = sizeof(err);
				if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) == 0 && err == 0)
					break; // connected
			}
		}
		close(fd);
		fd = -1;
	}
	freeaddrinfo(res);

	if (fd < 0)
	{
		error = "cannot connect to " + host + ":" + portstr;
		return -1;
	}

	const int one = 1;
	setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
	return fd;
}

bool SendAll(int fd, const std::string& data, int timeout_ms)
{
	size_t off = 0;
	while (off < data.size())
	{
		pollfd pfd = {fd, POLLOUT, 0};
		if (poll(&pfd, 1, timeout_ms) != 1)
			return false;
		const ssize_t n = send(fd, data.data() + off, data.size() - off, MSG_NOSIGNAL);
		if (n < 0)
		{
			if (errno == EAGAIN || errno == EINTR)
				continue;
			return false;
		}
		off += (size_t)n;
	}
	return true;
}

} // namespace

bool Url::Parse(const std::string& url, Url& out)
{
	static const std::string scheme = "http://";
	if (url.compare(0, scheme.size(), scheme) != 0)
		return false;

	const size_t host_begin = scheme.size();
	size_t host_end = url.find_first_of("/?", host_begin);
	if (host_end == std::string::npos)
		host_end = url.size();

	std::string hostport = url.substr(host_begin, host_end - host_begin);
	if (hostport.empty())
		return false;

	out.port = 80;
	// IPv6 literals ([::1]:8080) are rare in SSDP LOCATION headers but legal.
	if (hostport[0] == '[')
	{
		const size_t close = hostport.find(']');
		if (close == std::string::npos)
			return false;
		out.host = hostport.substr(1, close - 1);
		if (close + 1 < hostport.size() && hostport[close + 1] == ':')
			out.port = (uint16_t)std::stoi(hostport.substr(close + 2));
	}
	else
	{
		const size_t colon = hostport.rfind(':');
		if (colon != std::string::npos)
		{
			out.host = hostport.substr(0, colon);
			const std::string p = hostport.substr(colon + 1);
			if (p.empty() || p.find_first_not_of("0123456789") != std::string::npos)
				return false;
			out.port = (uint16_t)std::stoi(p);
		}
		else
		{
			out.host = hostport;
		}
	}

	out.path = (host_end < url.size()) ? url.substr(host_end) : "/";
	if (out.path[0] == '?')
		out.path = "/" + out.path;
	return !out.host.empty();
}

std::string ResolveUrl(const std::string& base, const std::string& ref)
{
	if (ref.empty())
		return base;
	if (ref.compare(0, 7, "http://") == 0 || ref.compare(0, 8, "https://") == 0)
		return ref;

	Url b;
	if (!Url::Parse(base, b))
		return ref;

	std::string origin = "http://" + b.host + ":" + std::to_string(b.port);
	if (ref[0] == '/')
		return origin + ref;

	// Relative path: resolve against the base path's directory.
	std::string dir = b.path;
	const size_t q = dir.find('?');
	if (q != std::string::npos)
		dir.resize(q);
	const size_t slash = dir.rfind('/');
	dir = (slash == std::string::npos) ? "/" : dir.substr(0, slash + 1);
	return origin + dir + ref;
}

bool HttpRequest(const std::string& method, const std::string& url,
                 const std::string& extra_headers, const std::string& body,
                 HttpResponse& response, std::string& error, int timeout_ms)
{
	Url u;
	if (!Url::Parse(url, u))
	{
		error = "unsupported URL: " + url;
		return false;
	}

	const int fd = ConnectTcp(u.host, u.port, timeout_ms, error);
	if (fd < 0)
		return false;

	std::string req = method + " " + u.path + " HTTP/1.1\r\n";
	req += "Host: " + u.host + ":" + std::to_string(u.port) + "\r\n";
	req += "User-Agent: Linux/1.0 UPnP/1.0 jtplay/1.0\r\n";
	req += "Connection: close\r\n";
	if (!body.empty())
		req += "Content-Length: " + std::to_string(body.size()) + "\r\n";
	if (!extra_headers.empty())
	{
		req += extra_headers;
		if (req.compare(req.size() - 2, 2, "\r\n") != 0)
			req += "\r\n";
	}
	req += "\r\n";
	req += body;

	if (!SendAll(fd, req, timeout_ms))
	{
		close(fd);
		error = "send failed";
		return false;
	}

	// Read until the peer closes ("Connection: close"), then split the
	// response. Chunked bodies are decoded below.
	std::string raw;
	char buf[16384];
	for (;;)
	{
		pollfd pfd = {fd, POLLIN, 0};
		const int pr = poll(&pfd, 1, timeout_ms);
		if (pr != 1)
		{
			close(fd);
			error = pr == 0 ? "timeout" : "poll failed";
			return false;
		}
		const ssize_t n = recv(fd, buf, sizeof(buf), 0);
		if (n < 0)
		{
			if (errno == EAGAIN || errno == EINTR)
				continue;
			close(fd);
			error = "recv failed";
			return false;
		}
		if (n == 0)
			break;
		raw.append(buf, (size_t)n);
		if (raw.size() > 32u * 1024 * 1024)
		{
			close(fd);
			error = "response too large";
			return false;
		}
	}
	close(fd);

	const size_t hdr_end = raw.find("\r\n\r\n");
	if (hdr_end == std::string::npos)
	{
		error = "malformed HTTP response";
		return false;
	}

	response = {};
	// Status line
	{
		const size_t eol = raw.find("\r\n");
		const std::string line = raw.substr(0, eol);
		const size_t sp = line.find(' ');
		if (sp == std::string::npos)
		{
			error = "malformed status line";
			return false;
		}
		response.status = std::atoi(line.c_str() + sp + 1);
	}
	// Headers
	size_t pos = raw.find("\r\n") + 2;
	while (pos < hdr_end)
	{
		size_t eol = raw.find("\r\n", pos);
		if (eol == std::string::npos || eol > hdr_end)
			eol = hdr_end;
		const std::string line = raw.substr(pos, eol - pos);
		const size_t colon = line.find(':');
		if (colon != std::string::npos)
		{
			std::string key = ToLower(line.substr(0, colon));
			size_t v = colon + 1;
			while (v < line.size() && line[v] == ' ')
				v++;
			response.headers[key] = line.substr(v);
		}
		pos = eol + 2;
	}

	std::string payload = raw.substr(hdr_end + 4);

	// Transfer-Encoding: chunked
	auto te = response.headers.find("transfer-encoding");
	if (te != response.headers.end() && ToLower(te->second).find("chunked") != std::string::npos)
	{
		std::string decoded;
		size_t p = 0;
		while (p < payload.size())
		{
			const size_t eol = payload.find("\r\n", p);
			if (eol == std::string::npos)
				break;
			const long len = std::strtol(payload.c_str() + p, nullptr, 16);
			if (len <= 0)
				break;
			p = eol + 2;
			if (p + (size_t)len > payload.size())
				break;
			decoded.append(payload, p, (size_t)len);
			p += (size_t)len + 2; // skip the trailing CRLF
		}
		payload.swap(decoded);
	}

	response.body = std::move(payload);
	return true;
}

} // namespace upnp
