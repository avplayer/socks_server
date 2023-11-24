﻿//
// proxy_server.hpp
// ~~~~~~~~~~~~~~~~
//
// Copyright (c) 2019 Jack (jack dot wgm at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#ifndef INCLUDE__2023_10_18__PROXY_SERVER_HPP
#define INCLUDE__2023_10_18__PROXY_SERVER_HPP

#include <filesystem>
#include <memory>
#include <optional>
#include <cstdint>
#include <span>
#include <string>
#include <array>
#include <vector>
#include <unordered_map>

#include <boost/asio/io_context.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/read_until.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ip/udp.hpp>
#include <boost/asio/ssl.hpp>

#include <boost/asio/detached.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>

#ifdef _MSC_VER
# pragma warning(push)
# pragma warning(disable: 4702)
#endif // _MSC_VER

#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/version.hpp>
#include <boost/beast/core/detail/base64.hpp>

#ifdef _MSC_VER
# pragma warning(pop)
#endif

#include <boost/url.hpp>
#include <boost/regex.hpp>

#include <boost/nowide/convert.hpp>

#include <fmt/xchar.h>
#include <fmt/format.h>


#include "proxy/use_awaitable.hpp"
#include "proxy/scoped_exit.hpp"
#include "proxy/async_connect.hpp"
#include "proxy/logging.hpp"
#include "proxy/base_stream.hpp"
#include "proxy/default_cert.hpp"
#include "proxy/fileop.hpp"
#include "proxy/strutil.hpp"

#include "proxy/socks_enums.hpp"
#include "proxy/socks_client.hpp"
#include "proxy/http_proxy_client.hpp"
#include "proxy/socks_io.hpp"

#include "proxy/xxhash.hpp"
#include "proxy/scramble.hpp"


namespace proxy {

	namespace net = boost::asio;

	using namespace net::experimental::awaitable_operators;
	using namespace util;
	using namespace strutil;

	using tcp = net::ip::tcp;               // from <boost/asio/ip/tcp.hpp>
	using udp = net::ip::udp;               // from <boost/asio/ip/udp.hpp>

	namespace beast = boost::beast;			// from <boost/beast.hpp>
	namespace http = beast::http;           // from <boost/beast/http.hpp>

	namespace urls = boost::urls;			// form <boost/url.hpp>

	namespace fs = std::filesystem;

	using string_body = http::string_body;
	using dynamic_body = http::dynamic_body;
	using buffer_body = http::buffer_body;

	using dynamic_request = http::request<dynamic_body>;
	using string_request = http::request<string_body>;

	using string_response = http::response<string_body>;
	using buffer_response = http::response<buffer_body>;

	using request_parser = http::request_parser<string_request::body_type>;

	using response_serializer = http::response_serializer<buffer_body, http::fields>;
	using string_response_serializer = http::response_serializer<string_body, http::fields>;

	using ssl_stream = net::ssl::stream<tcp_socket>;

	using io_util::read;
	using io_util::write;


	//////////////////////////////////////////////////////////////////////////

	inline const char* version_string =
R"x*x*x(nginx/1.20.2)x*x*x";

	inline const char* fake_400_content_fmt =
R"x*x*x(HTTP/1.1 400 Bad Request
Server: nginx/1.20.2
Date: {}
Content-Type: text/html
Content-Length: 165
Connection: close

<html>
<head><title>400 Bad Request</title></head>
<body bgcolor="white">
<center><h1>400 Bad Request</h1></center>
<hr><center>nginx/1.20.2</center>
</body>
</html>)x*x*x";

	inline const char* fake_400_content =
R"x*x*x(<html>
<head><title>400 Bad Request</title></head>
<body bgcolor="white">
<center><h1>400 Bad Request</h1></center>
<hr><center>nginx/1.20.2</center>
</body>
</html>)x*x*x";

	inline const char* fake_403_content =
R"x*x*x(<html>
<head><title>403 Forbidden</title></head>
<body>
<center><h1>403 Forbidden</h1></center>
<hr><center>nginx/1.20.2</center>
</body>
</html>
)x*x*x";

	inline const char* fake_404_content_fmt =
R"x*x*x(HTTP/1.1 404 Not Found
Server: nginx/1.20.2
Date: {}
Content-Type: text/html
Content-Length: 145
Connection: close

<html><head><title>404 Not Found</title></head>
<body>
<center><h1>404 Not Found</h1></center>
<hr>
<center>nginx/1.20.2</center>
</body>
</html>)x*x*x";

	inline const char* fake_407_content_fmt =
R"x*x*x(HTTP/1.1 407 Proxy Authentication Required
Server: nginx/1.20.2
Date: {}
Connection: close
Proxy-Authenticate: Basic realm="proxy"
Proxy-Connection: close
Content-Length: 0

)x*x*x";

	inline const char* fake_416_content =
R"x*x*x(<html>
<head><title>416 Requested Range Not Satisfiable</title></head>
<body>
<center><h1>416 Requested Range Not Satisfiable</h1></center>
<hr><center>nginx/1.20.2</center>
</body>
</html>
)x*x*x";

	inline const char* fake_302_content =
R"x*x*x(<html>
<head><title>301 Moved Permanently</title></head>
<body>
<center><h1>301 Moved Permanently</h1></center>
<hr><center>nginx/1.20.2</center>
</body>
</html>
)x*x*x";


	inline constexpr auto head_fmt =
		LR"(<html><head><meta charset="UTF-8"><title>Index of {}</title></head><body bgcolor="white"><h1>Index of {}</h1><hr><pre>)";
	inline constexpr auto tail_fmt =
		L"</pre><hr></body></html>";
	inline constexpr auto body_fmt =
		L"<a href=\"{}\">{}</a>{} {}       {}\r\n";


	//////////////////////////////////////////////////////////////////////////

	// udp_session_expired_time 用于指定 udp session 的过期时间, 单位为秒.
	inline const int udp_session_expired_time = 600;

	// nosie_injection_max_len 用于指定噪声注入的最大长度, 单位为字节.
	inline const int nosie_injection_max_len = 0x0fff;

	// global_known_proto 用于指定全局已知的协议, 用于噪声注入时避免生成已知的协议头.
	inline std::set<uint8_t> global_known_proto =
		{
			0x04, // socks4
			0x05, // socks5
			0x47, // 'G'
			0x50, // 'P'
			0x43, // 'C'
			0x16, // ssl
		};


	//////////////////////////////////////////////////////////////////////////

	// proxy server 参数选项, 用于指定 proxy server 的各种参数.
	struct proxy_server_option
	{
		// 授权信息.
		// auth_users 的第1个元素为用户名, 第2个元素为密码.
		// auth_users_ 为空时, 表示不需要认证.
		// auth_users_ 可以是多个用户, 例如:
		// { {"user1", "passwd1"}, {"user2", "passwd2"} };
		using auth_users = std::tuple<std::string, std::string>;
		std::vector<auth_users> auth_users_;

		// 多层代理, 当前服务器级连下一个服务器, 对于 client 而言是无感的,
		// 这是当前服务器通过 proxy_pass_ 指定的下一个代理服务器, 为 client
		// 实现多层代理.
		//
		// 例如 proxy_pass_ 可以是:
		// socks5://user:passwd@proxy.server.com:1080
		// 或:
		// https://user:passwd@proxy.server.com:1080
		//
		// 当 proxy_pass_ 是 socks5 代理时, 默认使用 hostname 模式, 即 dns
		// 解析在远程执行.
		//
		// 在配置了 proxy_protocol (haproxy)协议时, proxy_pass_ 通常为
		// 下一个 proxy_protocol 或直接目标服务器(目标服务器需要支持
		// proxy_protocol).
		std::string proxy_pass_;

		// 多层代理模式中, 与下一个代理服务器(next_proxy_)是否使用tls加密(ssl).
		// 该参数只能当 next_proxy_ 是 socks 代理时才有作用, 如果 next_proxy_
		// 是 http proxy，则由 url 指定的 protocol 决定是否使用 ssl.
		bool proxy_pass_use_ssl_{ false };

		// 启用 proxy protocol (haproxy)协议.
		// 当前服务将会在连接到 proxy_pass_ 成功后，首先传递 proxy protocol
		// 以告之 proxy_pass_ 来源 IP/PORT 以及目标 IP/PORT.
		// 注意：此选项当前未实现.
		bool haproxy_{ false };

		// 指定当前proxy server向外发起连接时, 绑定到哪个本地地址, 在多网卡
		// 的服务器上, 可以指定此参数, 默认为空, 表示不指定, 由系统自动选择.
		std::string local_ip_;

		// 启用 TCP 端口重用(仅Linux kernel version 3.9以上支持).
		bool reuse_port_{ false };

		// 是否启用 Happy Eyeballs 连接算法, 默认为使用.
		bool happyeyeballs_{ true };

		// 作为服务器时, 指定ssl证书目录, 使用固定文件名(ssl_crt.pem,
		// ssl_dh.pem, ssl_key.pem, ssl_dh.pem, ssl_crt.pwd)
		// , 这样就不用指定下面: ssl_certificate_、ssl_certificate_key_
		// 以及 ssl_dhparam_、ssl_certificate_passwd_ 这4个参数.
		std::string ssl_cert_path_;

		// 作为服务器时, 指定ssl证书pem文件.
		std::string ssl_certificate_;

		// 作为服务器时, 指定ssl证书密钥文件.
		std::string ssl_certificate_key_;

		// 作为服务器时, 指定ssl证书解密密钥/或密钥文件.
		std::string ssl_certificate_passwd_;

		// 作为服务器时, 指定ssl dh参数文件, 可用命令:
		// openssl dhparam -out dh4096.pem 4096
		// 来生成此文件, 以增强密钥交换安全性.
		std::string ssl_dhparam_;

		// 用于多域名证书下指定具体域名.
		std::string ssl_sni_;

		// 指定允许的加密算法.
		std::string ssl_ciphers_;

		// 优先使用server端加密算法.
		bool ssl_prefer_server_ciphers_;

		// http doc 目录, 用于伪装成web站点, 如果此字段为空, 则表示不启
		// 用此功能, 遇到 http/https 文件请求时则返回错误信息.
		std::string doc_directory_;

		// autoindex 功能, 类似 nginx 中的 autoindex.
		// 打开将会显示目录下的文件列表, 此功能作用在启用 doc_directory_
		// 的时候, 对 doc_directory_ 目录下的文件列表信息是否使用列表展
		// 示.
		bool autoindex_;

		// 禁用 http 服务, 客户端无法通过明文的 http 协议与之通信, 包括
		// ssl 加密的 https 以及不加密的 http 服务, 同时也包括 http(s)
		// proxy 也会被禁用.
		// 在有些时候, 为了安全考虑, 可以禁用 http 服务避免服务器上的信息
		// 意外访问, 或不想启用 http(s) 服务.
		bool disable_http_{ false };

		// 禁用 socks proxy 服务, 服务端不提供 socks4/5 代理服务, 包括
		// 加密的 socks4/5 以及不加密的 socks4/5.
		bool disable_socks_{ false };

		// 禁止非安全连接, 即禁止 http/socks 明文连接, 只允许 https/socks5
		// 加密连接.
		bool disable_insecure_{ false };

		// 启用噪声注入以干扰流量分析, 从而达到数据安全的目的.
		// 此功能必须在 server/client 两端同时启用才有效, 此功能表示在启
		// 用 ssl 协议时, 在 ssl 握手后双方互相发送一段随机长度的随机数据
		// 以干扰流量分析.
		// 在双方接收到对方的随机数据后, 将对整个随机数据进行 hash 计算, 得
		// 到的结果将会作为后续数据的加密密钥, 从而达到加密通信的目的.
		// 加密算法仅仅是简单的异或运算, 但是由于密钥是随机的, 因此即使是
		// 同样的明文, 也会得到不同的密文, 从而达到加密通信的目的.
		// 密钥在一轮(密钥长度)使用完后, 将会通过 hash(hash) 重新计算得到
		// 新的密钥, 用于下一轮的加密通信.
		// hash 算法采用快速的 xxhash, 但是由于 xxhash 本身的特性. 因此
		// 密钥长度不能太长, 否则会影响性能, 所在固定密钥长度为 16 字节.
		// 此功能可以有效的防止流量分析, 但是会增加一定的流量消耗以及延迟,
		// 此选项默认不启用, 除非有确定证据证明代理流量被分析或干扰, 此时可
		// 以启用此选项.
		bool scramble_{ false };
	};


	//////////////////////////////////////////////////////////////////////////

	// proxy server 虚基类, 任何 proxy server 的实现, 必须基于这个基类.
	// 这样 proxy_session 才能通过虚基类指针访问proxy server的具体实
	// 现以及虚函数方法.
	class proxy_server_base {
	public:
		virtual ~proxy_server_base() {}
		virtual void remove_session(size_t id) = 0;
		virtual size_t num_session() = 0;
		virtual const proxy_server_option& option() = 0;
	};


	//////////////////////////////////////////////////////////////////////////

	// proxy session 虚基类.
	class proxy_session_base {
	public:
		virtual ~proxy_session_base() {}
		virtual void start() = 0;
		virtual void close() = 0;
	};


	//////////////////////////////////////////////////////////////////////////

	// proxy_session 抽象类, 它被设计为一个模板抽象类, 模板参数Stream
	// 指定与本地通信的stream对象, 默认使用tcp::socket, 可根据此
	// async_read/async_write等接口实现专用的stream类, 比如实现加密.
	class proxy_session
		: public proxy_session_base
		, public std::enable_shared_from_this<proxy_session>
	{
		proxy_session(const proxy_session&) = delete;
		proxy_session& operator=(const proxy_session&) = delete;

		struct http_context
		{
			std::vector<std::string> command_;
			string_request& request_;
			request_parser& parser_;
			std::string target_;
			std::string target_path_;
		};

		enum {
			PROXY_AUTH_SUCCESS = 0,
			PROXY_AUTH_FAILED,
			PROXY_AUTH_NONE,
			PROXY_AUTH_ILLEGAL,
		};

		inline std::string proxy_auth_error_message(int code) const
		{
			switch (code)
			{
			case PROXY_AUTH_SUCCESS:
				return "auth success";
			case PROXY_AUTH_FAILED:
				return "auth failed";
			case PROXY_AUTH_NONE:
				return "auth none";
			case PROXY_AUTH_ILLEGAL:
				return "auth illegal";
			default:
				return "auth unknown";
			}
		}

	public:
		proxy_session(proxy_stream_type&& socket,
			size_t id, std::weak_ptr<proxy_server_base> server)
			: m_local_socket(std::move(socket))
			, m_remote_socket(instantiate_proxy_stream(
				m_local_socket.get_executor()))
			, m_udp_socket(m_local_socket.get_executor())
			, m_timer(m_local_socket.get_executor())
			, m_connection_id(id)
			, m_proxy_server(server)
		{
		}

		~proxy_session()
		{
			auto server = m_proxy_server.lock();
			if (!server)
				return;

			server->remove_session(m_connection_id);
			auto num = server->num_session();

			XLOG_DBG << "connection id: "
				<< m_connection_id
				<< ", destroyed, remainder: "
				<< num;
		}

	public:
		virtual void start() override
		{
			auto server = m_proxy_server.lock();
			if (!server)
				return;

			m_option = server->option();

			if (!m_option.proxy_pass_.empty())
			{
				try
				{
					m_next_proxy =
						std::make_unique<urls::url_view>(m_option.proxy_pass_);
				}
				catch (const std::exception& e)
				{
					XLOG_ERR << "connection id: "
						<< m_connection_id
						<< ", params next_proxy error: "
						<< m_option.proxy_pass_
						<< ", exception: "
						<< e.what();
					return;
				}
			}

			auto self = shared_from_this();

			net::co_spawn(m_local_socket.get_executor(),
				[this, self, server]() -> net::awaitable<void>
				{
					co_await start_proxy();
				}, net::detached);
		}

		virtual void close() override
		{
			m_abort = true;

			boost::system::error_code ignore_ec;
			m_local_socket.close(ignore_ec);
			m_remote_socket.close(ignore_ec);
		}

	private:
		inline net::awaitable<void> start_proxy()
		{
			// read
			//  +----+----------+----------+
			//  |VER | NMETHODS | METHODS  |
			//  +----+----------+----------+
			//  | 1  |    1     | 1 to 255 |
			//  +----+----------+----------+
			//  [               ]
			// or
			//  +----+----+----+----+----+----+----+----+----+----+....+----+
			//  | VN | CD | DSTPORT |      DSTIP        | USERID       |NULL|
			//  +----+----+----+----+----+----+----+----+----+----+....+----+
			//    1    1      2        4                  variable       1
			//  [         ]
			// 读取[]里的部分.

			boost::system::error_code ec;

			[[maybe_unused]] auto bytes =
				co_await net::async_read(
					m_local_socket,
					m_local_buffer,
					net::transfer_exactly(2),
					net_awaitable[ec]);
			if (ec)
			{
				XLOG_ERR << "connection id: "
					<< m_connection_id
					<< ", read socks version: "
					<< ec.message();
				co_return;
			}
			BOOST_ASSERT(bytes == 2);

			auto p = net::buffer_cast<const char*>(m_local_buffer.data());
			int socks_version = read<uint8_t>(p);

			if (socks_version == SOCKS_VERSION_5)
			{
				if (m_option.disable_socks_)
				{
					XLOG_DBG << "connection id: "
						<< m_connection_id
						<< ", socks5 protocol disabled";
					co_return;
				}

				XLOG_DBG << "connection id: "
					<< m_connection_id
					<< ", socks version: "
					<< socks_version;

				co_await socks_connect_v5();
				co_return;
			}
			if (socks_version == SOCKS_VERSION_4)
			{
				if (m_option.disable_socks_)
				{
					XLOG_DBG << "connection id: "
						<< m_connection_id
						<< ", socks4 protocol disabled";
					co_return;
				}

				XLOG_DBG << "connection id: "
					<< m_connection_id
					<< ", socks version: "
					<< socks_version;

				co_await socks_connect_v4();
				co_return;
			}
			if (socks_version == 'G' || socks_version == 'P')
			{
				if (m_option.disable_http_)
				{
					XLOG_DBG << "connection id: "
						<< m_connection_id
						<< ", http protocol disabled";
					co_return;
				}

				auto ret = co_await http_proxy_get();
				if (!ret)
				{
					auto fake_page =
						fmt::vformat(fake_400_content_fmt,
							fmt::make_format_args(server_date_string()));

					co_await net::async_write(
						m_local_socket,
						net::buffer(fake_page),
						net::transfer_all(),
						net_awaitable[ec]);
				}
			}
			else if (socks_version == 'C')
			{
				if (m_option.disable_http_)
				{
					XLOG_DBG << "connection id: "
						<< m_connection_id
						<< ", http protocol disabled";
					co_return;
				}

				auto ret = co_await http_proxy_connect();
				if (!ret)
				{
					auto fake_page =
						fmt::vformat(fake_400_content_fmt,
							fmt::make_format_args(server_date_string()));

					co_await net::async_write(
						m_local_socket,
						net::buffer(fake_page),
						net::transfer_all(),
						net_awaitable[ec]);
				}
			}

			co_return;
		}

		inline net::awaitable<void> socks_connect_v5()
		{
			auto p = net::buffer_cast<const char*>(m_local_buffer.data());

			auto socks_version = read<int8_t>(p);
			BOOST_ASSERT(socks_version == SOCKS_VERSION_5);
			int nmethods = read<int8_t>(p);
			if (nmethods <= 0 || nmethods > 255)
			{
				XLOG_ERR << "connection id: "
					<< m_connection_id
					<< ", unsupported method : "
					<< nmethods;
				co_return;
			}

			//  +----+----------+----------+
			//  |VER | NMETHODS | METHODS  |
			//  +----+----------+----------+
			//  | 1  |    1     | 1 to 255 |
			//  +----+----------+----------+
			//                  [          ]
			m_local_buffer.consume(m_local_buffer.size());
			boost::system::error_code ec;
			auto bytes = co_await net::async_read(m_local_socket,
				m_local_buffer,
				net::transfer_exactly(nmethods),
				net_awaitable[ec]);
			if (ec)
			{
				XLOG_ERR << "connection id: "
					<< m_connection_id
					<< ", read socks methods: "
					<< ec.message();
				co_return;
			}

			// 服务端是否需要认证.
			auto auth_required = !m_option.auth_users_.empty();

			// 循环读取客户端支持的代理方式.
			p = net::buffer_cast<const char*>(m_local_buffer.data());

			int method = SOCKS5_AUTH_UNACCEPTABLE;
			while (bytes != 0)
			{
				int m = read<int8_t>(p);

				if (auth_required)
				{
					if (m == SOCKS5_AUTH)
					{
						method = m;
						break;
					}
				}
				else
				{
					if (m == SOCKS5_AUTH_NONE || m == SOCKS5_AUTH)
					{
						method = m;
						break;
					}
				}

				bytes--;
			}

			net::streambuf wbuf;

			// 客户端不支持认证, 而如果服务端需要认证, 回复客户端不接受.
			if (method == SOCKS5_AUTH_UNACCEPTABLE)
			{
				// 回复客户端, 不接受客户端的的代理请求.
				auto wp = net::buffer_cast<char*>(wbuf.prepare(1024));
				write<uint8_t>(socks_version, wp);
				write<uint8_t>(SOCKS5_AUTH_UNACCEPTABLE, wp);
			}
			else
			{
				// 回复客户端, server所选择的代理方式.
				auto wp = net::buffer_cast<char*>(wbuf.prepare(1024));
				write<uint8_t>(socks_version, wp);
				write<uint8_t>((uint8_t)method, wp);
			}
			wbuf.commit(2);

			//  +----+--------+
			//  |VER | METHOD |
			//  +----+--------+
			//  | 1  |   1    |
			//  +----+--------+
			//  [             ]
			bytes = co_await net::async_write(m_local_socket,
				wbuf,
				net::transfer_exactly(2),
				net_awaitable[ec]);
			if (ec)
			{
				XLOG_WARN << "connection id: "
					<< m_connection_id
					<< ", write server method error : "
					<< ec.message();
				co_return;
			}

			if (method == SOCKS5_AUTH_UNACCEPTABLE)
			{
				XLOG_WARN << "connection id: "
					<< m_connection_id
					<< ", no acceptable methods for server";
				co_return;
			}

			// 认证模式, 则进入认证子协程.
			if (method == SOCKS5_AUTH)
			{
				auto ret = co_await socks_auth();
				if (!ret)
					co_return;
			}

			//  +----+-----+-------+------+----------+----------+
			//  |VER | CMD |  RSV  | ATYP | DST.ADDR | DST.PORT |
			//  +----+-----+-------+------+----------+----------+
			//  | 1  |  1  | X'00' |  1   | Variable |    2     |
			//  +----+-----+-------+------+----------+----------+
			//  [                          ]
			m_local_buffer.consume(m_local_buffer.size());
			bytes = co_await net::async_read(m_local_socket,
				m_local_buffer,
				net::transfer_exactly(5),
				net_awaitable[ec]);
			if (ec)
			{
				XLOG_WARN << "connection id: "
					<< m_connection_id
					<< ", read client request error: "
					<< ec.message();
				co_return;
			}

			p = net::buffer_cast<const char*>(m_local_buffer.data());
			auto ver = read<int8_t>(p);
			if (ver != SOCKS_VERSION_5)
			{
				XLOG_WARN << "connection id: "
					<< m_connection_id
					<< ", socks requests, invalid protocol: "
					<< ver;
				co_return;
			}

			int command = read<int8_t>(p);		// CONNECT/BIND/UDP
			read<int8_t>(p);					// reserved.
			int atyp = read<int8_t>(p);			// atyp.

			//  +----+-----+-------+------+----------+----------+
			//  |VER | CMD |  RSV  | ATYP | DST.ADDR | DST.PORT |
			//  +----+-----+-------+------+----------+----------+
			//  | 1  |  1  | X'00' |  1   | Variable |    2     |
			//  +----+-----+-------+------+----------+----------+
			//                              [                   ]
			int length = 0;

			// 消费掉前4个字节, 保存第1个字节.
			m_local_buffer.consume(4);

			if (atyp == SOCKS5_ATYP_IPV4)
				length = 5; // 6 - 1
			else if (atyp == SOCKS5_ATYP_DOMAINNAME)
			{
				length = read<uint8_t>(p) + 2;
				m_local_buffer.consume(1);
			}
			else if (atyp == SOCKS5_ATYP_IPV6)
				length = 17; // 18 - 1

			bytes = co_await net::async_read(m_local_socket,
				m_local_buffer,
				net::transfer_exactly(length),
				net_awaitable[ec]);
			if (ec)
			{
				XLOG_WARN << "connection id: "
					<< m_connection_id
					<< ", read client request dst.addr error: "
					<< ec.message();
				co_return;
			}

			tcp::endpoint dst_endpoint;
			std::string domain;
			uint16_t port = 0;

			auto executor = co_await net::this_coro::executor;

			p = net::buffer_cast<const char*>(m_local_buffer.data());
			if (atyp == SOCKS5_ATYP_IPV4)
			{
				dst_endpoint.address(net::ip::address_v4(read<uint32_t>(p)));
				dst_endpoint.port(read<uint16_t>(p));

				domain = dst_endpoint.address().to_string();
				port = dst_endpoint.port();

				XLOG_DBG << "connection id: "
					<< m_connection_id
					<< ", "
					<< m_local_socket.remote_endpoint()
					<< " to ipv4: "
					<< dst_endpoint;
			}
			else if (atyp == SOCKS5_ATYP_DOMAINNAME)
			{
				for (size_t i = 0; i < bytes - 2; i++)
					domain.push_back(read<int8_t>(p));
				port = read<uint16_t>(p);

				XLOG_DBG << "connection id: "
					<< m_connection_id
					<< ", "
					<< m_local_socket.remote_endpoint()
					<< " to domain: "
					<< domain
					<< ":"
					<< port;
			}
			else if (atyp == SOCKS5_ATYP_IPV6)
			{
				net::ip::address_v6::bytes_type addr;
				for (auto i = addr.begin();
					i != addr.end(); ++i)
				{
					*i = read<int8_t>(p);
				}

				dst_endpoint.address(net::ip::address_v6(addr));
				dst_endpoint.port(read<uint16_t>(p));

				domain = dst_endpoint.address().to_string();
				port = dst_endpoint.port();

				XLOG_DBG << "connection id: "
					<< m_connection_id
					<< ", "
					<< m_local_socket.remote_endpoint()
					<< " to ipv6: "
					<< dst_endpoint;
			}

			if (command == SOCKS_CMD_CONNECT)
			{
				// 连接目标主机.
				co_await start_connect_host(
					domain, port, ec, atyp == SOCKS5_ATYP_DOMAINNAME);
			}
			else if (command == SOCKS5_CMD_UDP)
			do {
				if (atyp == SOCKS5_ATYP_DOMAINNAME)
				{
					tcp::resolver resolver{ executor };

					auto targets = co_await resolver.async_resolve(
						domain,
						std::to_string(port),
						net_awaitable[ec]);
					if (ec)
						break;

					for (const auto& target : targets)
					{
						dst_endpoint = target.endpoint();
						break;
					}
				}

				// 创建UDP端口.
				auto protocol = dst_endpoint.address().is_v4()
					? udp::v4() : udp::v6();
				m_udp_socket.open(protocol, ec);
				if (ec)
					break;

				m_udp_socket.bind(
					udp::endpoint(protocol, dst_endpoint.port()), ec);
				if (ec)
					break;

				auto remote_endp = m_local_socket.remote_endpoint();

				// 所有发向 udp socket 的数据, 都将转发到 m_local_udp_address
				// 除非地址是 m_local_udp_address 本身除外.
				m_local_udp_address = remote_endp.address();

				// 开启udp socket数据接收, 并计时, 如果在一定时间内没有接收到数据包
				// 则关闭 udp socket 等相关资源.
				net::co_spawn(executor,
					tick(), net::detached);

				net::co_spawn(executor,
					forward_udp(), net::detached);

				wbuf.consume(wbuf.size());
				auto wp = net::buffer_cast<char*>(
					wbuf.prepare(64 + domain.size()));

				write<uint8_t>(SOCKS_VERSION_5, wp);	// VER
				write<uint8_t>(0, wp);					// REP
				write<uint8_t>(0x00, wp);				// RSV

				auto local_endp = m_udp_socket.local_endpoint(ec);
				if (ec)
					break;

				XLOG_DBG << "connection id: "
					<< m_connection_id
					<< ", local udp address: "
					<< m_local_udp_address.to_string()
					<< ", udp socket: "
					<< local_endp;

				if (local_endp.address().is_v4())
				{
					auto uaddr = local_endp.address().to_v4().to_uint();

					write<uint8_t>(SOCKS5_ATYP_IPV4, wp);
					write<uint32_t>(uaddr, wp);
					write<uint16_t>(local_endp.port(), wp);
				}
				else if (local_endp.address().is_v6())
				{
					write<uint8_t>(SOCKS5_ATYP_IPV6, wp);
					auto data = local_endp.address().to_v6().to_bytes();
					for (auto c : data)
						write<uint8_t>(c, wp);
					write<uint16_t>(local_endp.port(), wp);
				}

				auto len = wp - net::buffer_cast<const char*>(wbuf.data());
				wbuf.commit(len);
				bytes = co_await net::async_write(m_local_socket,
					wbuf,
					net::transfer_exactly(len),
					net_awaitable[ec]);
				if (ec)
				{
					XLOG_WARN << "connection id: "
						<< m_connection_id
						<< ", write server response error: "
						<< ec.message();
					co_return;
				}

				co_return;
			} while (0);

			// 连接成功或失败.
			{
				int8_t error_code = SOCKS5_SUCCEEDED;

				if (ec == net::error::connection_refused)
					error_code = SOCKS5_CONNECTION_REFUSED;
				else if (ec == net::error::network_unreachable)
					error_code = SOCKS5_NETWORK_UNREACHABLE;
				else if (ec)
					error_code = SOCKS5_GENERAL_SOCKS_SERVER_FAILURE;

				//  +----+-----+-------+------+----------+----------+
				//  |VER | REP |  RSV  | ATYP | BND.ADDR | BND.PORT |
				//  +----+-----+-------+------+----------+----------+
				//  | 1  |  1  | X'00' |  1   | Variable |    2     |
				//  +----+-----+-------+------+----------+----------+
				//  [                                               ]

				wbuf.consume(wbuf.size());
				auto wp = net::buffer_cast<char*>(
					wbuf.prepare(64 + domain.size()));

				write<uint8_t>(SOCKS_VERSION_5, wp); // VER
				write<uint8_t>(error_code, wp);		// REP
				write<uint8_t>(0x00, wp);			// RSV

				if (dst_endpoint.address().is_v4())
				{
					auto uaddr = dst_endpoint.address().to_v4().to_uint();

					write<uint8_t>(SOCKS5_ATYP_IPV4, wp);
					write<uint32_t>(uaddr, wp);
					write<uint16_t>(dst_endpoint.port(), wp);
				}
				else if (dst_endpoint.address().is_v6())
				{
					write<uint8_t>(SOCKS5_ATYP_IPV6, wp);
					auto data = dst_endpoint.address().to_v6().to_bytes();
					for (auto c : data)
						write<uint8_t>(c, wp);
					write<uint16_t>(dst_endpoint.port(), wp);
				}
				else if (!domain.empty())
				{
					write<uint8_t>(SOCKS5_ATYP_DOMAINNAME, wp);
					write<uint8_t>(static_cast<int8_t>(domain.size()), wp);
					std::copy(domain.begin(), domain.end(), wp);
					wp += domain.size();
					write<uint16_t>(port, wp);
				}
				else
				{
					write<uint8_t>(0x1, wp);
					write<uint32_t>(0, wp);
					write<uint16_t>(0, wp);
				}

				auto len = wp - net::buffer_cast<const char*>(wbuf.data());
				wbuf.commit(len);
				bytes = co_await net::async_write(m_local_socket,
					wbuf,
					net::transfer_exactly(len),
					net_awaitable[ec]);
				if (ec)
				{
					XLOG_WARN << "connection id: "
						<< m_connection_id
						<< ", write server response error: "
						<< ec.message();
					co_return;
				}

				if (error_code != SOCKS5_SUCCEEDED)
					co_return;
			}

			XLOG_DBG << "connection id: "
				<< m_connection_id
				<< ", connected start transfer";

			// 发起数据传输协程.
			if (command == SOCKS_CMD_CONNECT)
			{
				co_await(
					transfer(m_local_socket, m_remote_socket)
					&&
					transfer(m_remote_socket, m_local_socket)
					);

				XLOG_DBG << "connection id: "
					<< m_connection_id
					<< ", transfer completed";
			}
			else
			{
				XLOG_WARN << "connection id: "
					<< m_connection_id
					<< ", SOCKS_CMD_BIND and SOCKS5_CMD_UDP is unsupported";
			}

			co_return;
		}

		inline net::awaitable<void> forward_udp()
		{
			[[maybe_unused]] auto self = shared_from_this();
			boost::system::error_code ec;
			udp::endpoint remote_endp;
			udp::endpoint local_endp;
			char read_buffer[4096];
			const char* rbuf = &read_buffer[96];
			char* wbuf = &read_buffer[86];
			auto executor = co_await net::this_coro::executor;
			size_t total = 0;

			while (!m_abort)
			{
				m_timeout = udp_session_expired_time;

				auto bytes = co_await m_udp_socket.async_receive_from(
					net::buffer(read_buffer, 1500),
					remote_endp,
					net_awaitable[ec]);
				if (ec)
					break;

				auto rp = rbuf;

				if (remote_endp.address() == m_local_udp_address)
				{
					local_endp = remote_endp;

					//  +----+------+------+----------+-----------+----------+
					//  |RSV | FRAG | ATYP | DST.ADDR | DST.PORT  |   DATA   |
					//  +----+------+------+----------+-----------+----------+
					//  | 2  |  1   |  1   | Variable |    2      | Variable |
					//  +----+------+------+----------+-----------+----------+

					// 去掉包头转发至远程主机.
					read<uint16_t>(rp); // rsv
					auto frag = read<uint8_t>(rp);  // frag

					// 不支持udp分片.
					if (frag != 0)
						continue;

					auto atyp = read<uint8_t>(rp);

					if (atyp == SOCKS5_ATYP_IPV4)
					{
						remote_endp.address(
							net::ip::address_v4(read<uint32_t>(rp)));
						remote_endp.port(read<uint16_t>(rp));
					}
					else if (atyp == SOCKS5_ATYP_DOMAINNAME)
					{
						auto length = read<uint8_t>(rp);
						std::string domain;

						for (size_t i = 0; i < length; i++)
							domain.push_back(read<int8_t>(rp));
						auto port = read<uint16_t>(rp);

						udp::resolver resolver{ executor };

						auto targets =
							co_await resolver.async_resolve(
							domain,
							std::to_string(port),
							net_awaitable[ec]);
						if (ec)
							break;

						for (const auto& target : targets)
						{
							remote_endp = target.endpoint();
							break;
						}
					}
					else if (atyp == SOCKS5_ATYP_IPV6)
					{
						net::ip::address_v6::bytes_type addr;
						for (auto i = addr.begin();
							i != addr.end(); ++i)
						{
							*i = read<int8_t>(rp);
						}

						remote_endp.address(net::ip::address_v6(addr));
						remote_endp.port(read<uint16_t>(rp));
					}

					auto head_size = rp - rbuf;
					auto udp_size = bytes - head_size;

					XLOG_DBG << "connection id: "
						<< m_connection_id
						<< ", udp forward, send "
						<< local_endp
						<< " to "
						<< remote_endp;
					total++;

					co_await m_udp_socket.async_send_to(
						net::buffer(rp, udp_size),
						remote_endp,
						net_awaitable[ec]);
				}
				else
				{
					auto wp = wbuf;

					if (remote_endp.address().is_v6())
						wp = wbuf - 12;

					write<uint16_t>(0x0, wp); // rsv
					write<uint8_t>(0x0, wp); // frag

					if (remote_endp.address().is_v4())
					{
						auto uaddr = remote_endp.address().to_v4().to_uint();
						write<uint8_t>(SOCKS5_ATYP_IPV4, wp); // atyp

						write<uint32_t>(uaddr, wp);
						write<uint16_t>(remote_endp.port(), wp);
					}
					if (remote_endp.address().is_v6())
					{
						write<uint8_t>(SOCKS5_ATYP_IPV6, wp); // atyp

						auto data = remote_endp.address().to_v6().to_bytes();
						for (auto c : data)
							write<uint8_t>(c, wp);
						write<uint16_t>(remote_endp.port(), wp);
					}

					auto head_size = wp - wbuf;
					auto udp_size = bytes + head_size;

					XLOG_DBG << "connection id: "
						<< m_connection_id
						<< ", udp forward, recv "
						<< remote_endp
						<< " to "
						<< local_endp;
					total++;

					co_await m_udp_socket.async_send_to(
						net::buffer(wbuf, udp_size),
						local_endp,
						net_awaitable[ec]);
				}
			}

			XLOG_DBG << "connection id: "
				<< m_connection_id
				<< ", total: "
				<< total
				<< ", forward_udp quit";

			co_return;
		}

		inline net::awaitable<void> tick()
		{
			[[maybe_unused]] auto self = shared_from_this();
			boost::system::error_code ec;

			while (!m_abort)
			{
				m_timer.expires_from_now(std::chrono::seconds(1));
				co_await m_timer.async_wait(net_awaitable[ec]);
				if (ec)
				{
					XLOG_WARN << "connection id: "
						<< m_connection_id
						<< ", ec: "
						<< ec.message();
					break;
				}

				if (--m_timeout <= 0)
				{
					XLOG_DBG << "connection id: "
						<< m_connection_id
						<< ", udp socket expired";
					m_udp_socket.close(ec);
					break;
				}
			}

			XLOG_DBG << "connection id: "
				<< m_connection_id
				<< ", udp expired timer quit";

			co_return;
		}

		inline net::awaitable<void> socks_connect_v4()
		{
			auto self = shared_from_this();
			auto p = net::buffer_cast<const char*>(m_local_buffer.data());

			[[maybe_unused]] auto socks_version = read<int8_t>(p);
			BOOST_ASSERT(socks_version == SOCKS_VERSION_4);
			auto command = read<int8_t>(p);

			//  +----+----+----+----+----+----+----+----+----+----+....+----+
			//  | VN | CD | DSTPORT |      DSTIP        | USERID       |NULL|
			//  +----+----+----+----+----+----+----+----+----+----+....+----+
			//  | 1  | 1  |    2    |         4         | variable     | 1  |
			//  +----+----+----+----+----+----+----+----+----+----+....+----+
			//            [                             ]
			m_local_buffer.consume(m_local_buffer.size());
			boost::system::error_code ec;
			auto bytes = co_await net::async_read(m_local_socket,
				m_local_buffer,
				net::transfer_exactly(6),
				net_awaitable[ec]);
			if (ec)
			{
				XLOG_WARN << "connection id: "
					<< m_connection_id
					<< ", read socks4 dst: "
					<< ec.message();
				co_return;
			}

			tcp::endpoint dst_endpoint;
			p = net::buffer_cast<const char*>(m_local_buffer.data());

			auto port = read<uint16_t>(p);
			dst_endpoint.port(port);
			dst_endpoint.address(net::ip::address_v4(read<uint32_t>(p)));

			bool socks4a = false;
			auto tmp = dst_endpoint.address().to_v4().to_uint() ^ 0x000000ff;
			if (0xff > tmp)
				socks4a = true;

			//  +----+----+----+----+----+----+----+----+----+----+....+----+
			//  | VN | CD | DSTPORT |      DSTIP        | USERID       |NULL|
			//  +----+----+----+----+----+----+----+----+----+----+....+----+
			//  | 1  | 1  |    2    |         4         | variable     | 1  |
			//  +----+----+----+----+----+----+----+----+----+----+....+----+
			//                                          [                   ]
			m_local_buffer.consume(m_local_buffer.size());
			bytes = co_await net::async_read_until(m_local_socket,
				m_local_buffer, '\0', net_awaitable[ec]);
			if (ec)
			{
				XLOG_WARN << "connection id: "
					<< m_connection_id
					<< ", read socks4 userid: "
					<< ec.message();
				co_return;
			}

			std::string userid;
			if (bytes > 1)
			{
				userid.resize(bytes - 1);
				m_local_buffer.sgetn(&userid[0], bytes - 1);
			}
			m_local_buffer.consume(1); // consume `null`

			std::string hostname;
			if (socks4a)
			{
				bytes = co_await net::async_read_until(m_local_socket,
					m_local_buffer, '\0', net_awaitable[ec]);
				if (ec)
				{
					XLOG_WARN << "connection id: "
						<< m_connection_id
						<< ", read socks4a hostname: "
						<< ec.message();
					co_return;
				}

				if (bytes > 1)
				{
					hostname.resize(bytes - 1);
					m_local_buffer.sgetn(&hostname[0], bytes - 1);
				}
			}

			XLOG_DBG << "connection id: "
				<< m_connection_id
				<< ", use "
				<< (socks4a ? "domain: " : "ip: ")
				<< (socks4a ? hostname : dst_endpoint.address().to_string());

			// 用户认证逻辑.
			bool verify_passed = m_option.auth_users_.empty();

			for (auto [user, pwd] : m_option.auth_users_)
			{
				if (user == userid)
				{
					verify_passed = true;
					break;
				}
			}

			if (verify_passed)
				XLOG_DBG << "connection id: "
					<< m_connection_id
					<< ", auth passed";
			else
				XLOG_WARN << "connection id: "
					<< m_connection_id
					<< ", auth no pass";

			if (!verify_passed)
			{
				//  +----+----+----+----+----+----+----+----+
				//  | VN | CD | DSTPORT |      DSTIP        |
				//  +----+----+----+----+----+----+----+----+
				//  | 1  | 1  |    2    |         4         |
				//  +----+----+----+----+----+----+----+----+
				//  [                                       ]

				net::streambuf wbuf;
				auto wp = net::buffer_cast<char*>(wbuf.prepare(16));

				write<uint8_t>(0, wp);
				write<uint8_t>(SOCKS4_REQUEST_REJECTED_USER_NO_ALLOW, wp);

				write<uint16_t>(dst_endpoint.port(), wp);
				write<uint32_t>(dst_endpoint.address().to_v4().to_ulong(), wp);

				wbuf.commit(8);
				bytes = co_await net::async_write(m_local_socket,
					wbuf,
					net::transfer_exactly(8),
					net_awaitable[ec]);
				if (ec)
				{
					XLOG_WARN << "connection id: "
						<< m_connection_id
						<< ", write socks4 no allow: "
						<< ec.message();
					co_return;
				}

				XLOG_WARN << "connection id: "
					<< m_connection_id
					<< ", socks4 "
					<< userid
					<< " auth fail";
				co_return;
			}

			int error_code = SOCKS4_REQUEST_GRANTED;
			if (command == SOCKS_CMD_CONNECT)
			{
				if (socks4a)
					co_await start_connect_host(hostname, port, ec, true);
				else
					co_await start_connect_host(
						dst_endpoint.address().to_string(),
						port,
						ec);
				if (ec)
				{
					XLOG_WFMT("connection id: {},"
						" connect to target {}:{} error: {}",
						m_connection_id,
						dst_endpoint.address().to_string(),
						port,
						ec.message());
					error_code = SOCKS4_CANNOT_CONNECT_TARGET_SERVER;
				}
			}
			else
			{
				error_code = SOCKS4_REQUEST_REJECTED_OR_FAILED;
				XLOG_WFMT("connection id: {},"
					" unsupported command for socks4",
					m_connection_id);
			}

			//  +----+----+----+----+----+----+----+----+
			//  | VN | CD | DSTPORT |      DSTIP        |
			//  +----+----+----+----+----+----+----+----+
			//  | 1  | 1  |    2    |         4         |
			//  +----+----+----+----+----+----+----+----+
			//  [                                       ]

			net::streambuf wbuf;
			auto wp = net::buffer_cast<char*>(wbuf.prepare(16));

			write<uint8_t>(0, wp);
			write<uint8_t>((uint8_t)error_code, wp);

			// 返回IP:PORT.
			write<uint16_t>(dst_endpoint.port(), wp);
			write<uint32_t>(dst_endpoint.address().to_v4().to_ulong(), wp);

			wbuf.commit(8);
			bytes = co_await net::async_write(m_local_socket,
				wbuf,
				net::transfer_exactly(8),
				net_awaitable[ec]);
			if (ec)
			{
				XLOG_WARN << "connection id: "
					<< m_connection_id
					<< ", write socks4 response: "
					<< ec.message();
				co_return;
			}

			if (error_code != SOCKS4_REQUEST_GRANTED)
				co_return;

			co_await(
				transfer(m_local_socket, m_remote_socket)
				&&
				transfer(m_remote_socket, m_local_socket)
				);

			XLOG_DBG << "connection id: "
				<< m_connection_id
				<< ", transfer completed";
			co_return;
		}

		inline int http_proxy_authorization(std::string_view pa)
		{
			if (m_option.auth_users_.empty())
				return PROXY_AUTH_SUCCESS;

			if (pa.empty())
				return PROXY_AUTH_NONE;

			auto pos = pa.find(' ');
			if (pos == std::string::npos)
				return PROXY_AUTH_ILLEGAL;

			auto type = pa.substr(0, pos);
			auto auth = pa.substr(pos + 1);

			if (type != "Basic")
				return PROXY_AUTH_ILLEGAL;

			std::string userinfo(
				beast::detail::base64::decoded_size(auth.size()), 0);
			auto [len, _] = beast::detail::base64::decode(
				(char*)userinfo.data(),
				auth.data(),
				auth.size());
			userinfo.resize(len);

			pos = userinfo.find(':');

			std::string uname = userinfo.substr(0, pos);
			std::string passwd = userinfo.substr(pos + 1);

			bool verify_passed = m_option.auth_users_.empty();

			for (auto [user, pwd] : m_option.auth_users_)
			{
				if (uname == user && passwd == pwd)
				{
					verify_passed = true;
					break;
				}
			}

			auto endp = m_local_socket.remote_endpoint();
			auto client = endp.address().to_string();
			client += ":" + std::to_string(endp.port());

			if (!verify_passed)
				return PROXY_AUTH_FAILED;

			return PROXY_AUTH_SUCCESS;
		}

		inline net::awaitable<bool> http_proxy_get()
		{
			boost::system::error_code ec;
			bool keep_alive = false;
			std::optional<request_parser> parser;
			bool first = true;

			while (!m_abort)
			{
				parser.emplace();
				parser->body_limit(1024 * 512); // 512k
				if (!first)
					m_local_buffer.consume(m_local_buffer.size());

				// 读取 http 请求头.
				co_await http::async_read(
					m_local_socket,
					m_local_buffer,
					*parser,
					net_awaitable[ec]);
				if (ec)
				{
					XLOG_WARN << "connection id: "
						<< m_connection_id
						<< (keep_alive ? ", keepalive" : "")
						<< ", http_proxy_get request async_read: "
						<< ec.message();

					co_return !first;
				}

				auto req = parser->release();
				auto mth = std::string(req.method_string());
				auto target_view = std::string(req.target());
				auto pa = std::string(req[http::field::proxy_authorization]);

				keep_alive = req.keep_alive();

				XLOG_DBG << "connection id: "
					<< m_connection_id
					<< ", method: " << mth
					<< ", target: " << target_view
					<< (pa.empty() ? std::string()
						: ", proxy_authorization: " + pa);

				auto expect_url = urls::parse_uri(target_view);

				// http 代理认证, 如果请求的 rarget 不是 http url 或认证
				// 失败, 则按正常 web 请求处理.
				auto auth = http_proxy_authorization(pa);
				if (auth != PROXY_AUTH_SUCCESS || expect_url.has_error())
				{
					if (!expect_url.has_error())
					{
						XLOG_WARN << "connection id: "
							<< m_connection_id
							<< ", proxy err: "
							<< proxy_auth_error_message(auth);

						co_return !first;
					}

					// 如果 doc 目录为空, 则不允许访问目录
					// 这里直接返回错误页面.
					if (m_option.doc_directory_.empty())
						co_return !first;

					// 如果不允许目录索引, 检查请求的是否为文件, 如果是具体文件则按文
					// 件请求处理, 否则返回 403.
					if (!m_option.autoindex_)
					{
						auto path = target_path(req);

						if (!fs::is_directory(path, ec))
						{
							co_await normal_web_server(req, parser);
							co_return true;
						}

						// 如果不允许目录索引, 则直接返回 403 forbidden.
						co_await forbidden_http_route(req);

						co_return true;
					}

					// 按正常 http 目录请求来处理.
					co_await normal_web_server(req, parser);
					co_return true;
				}

				auto& url = expect_url.value();

				if (!m_remote_socket.is_open())
				{
					auto host = url.host();
					auto port = url.port_number();

					if (port == 0)
						port = urls::default_port(url.scheme_id());

					// 连接到目标主机.
					co_await start_connect_host(host,
						port ? port : 80, ec, true);
					if (ec)
					{
						XLOG_WFMT("connection id: {},"
							" connect to target {}:{} error: {}",
							m_connection_id,
							host,
							port,
							ec.message());

						co_return !first;
					}
				}

				// 处理代理请求头.
				std::string query;
				if (url.query() != "")
				{
					auto q = std::string(url.query());
					if (q[0] == '?')
						query = std::string(url.query());
					else
						query = "?" + std::string(url.query());
				}

				if (std::string(url.path()) == "")
					req.target("/" + query);
				else
					req.target(std::string(url.path()) + query);

				req.set(http::field::host, url.host());

				if (req.find(http::field::connection) == req.end() &&
					req.find(http::field::proxy_connection) != req.end())
					req.set(http::field::connection, req[http::field::proxy_connection]);

				req.erase(http::field::proxy_authorization);
				req.erase(http::field::proxy_connection);

				co_await http::async_write(
					m_remote_socket, req, net_awaitable[ec]);
				if (ec)
				{
					XLOG_WARN << "connection id: "
						<< m_connection_id
						<< ", http_proxy_get request async_write: "
						<< ec.message();
					co_return !first;
				}

				m_local_buffer.consume(m_local_buffer.size());
				string_response resp;
				beast::flat_buffer buf;

				co_await http::async_read(
					m_remote_socket, buf, resp, net_awaitable[ec]);
				if (ec)
				{
					XLOG_WARN << "connection id: "
						<< m_connection_id
						<< ", http_proxy_get response async_read: "
						<< ec.message();
					co_return !first;
				}

				co_await http::async_write(
					m_local_socket, resp, net_awaitable[ec]);
				if (ec)
				{
					XLOG_WARN << "connection id: "
						<< m_connection_id
						<< ", http_proxy_get response async_write: "
						<< ec.message();
					co_return !first;
				}

				XLOG_DBG << "connection id: "
					<< m_connection_id
					<< ", transfer completed";

				first = false;
				if (!keep_alive)
					break;
			}

			co_return true;
		}

		inline net::awaitable<bool> http_proxy_connect()
		{
			http::request<http::string_body> req;
			boost::system::error_code ec;

			// 读取 http 请求头.
			co_await http::async_read(m_local_socket,
				m_local_buffer, req, net_awaitable[ec]);
			if (ec)
			{
				XLOG_ERR << "connection id: "
					<< m_connection_id
					<< ", http_proxy_connect async_read: "
					<< ec.message();

				co_return false;
			}

			auto mth = std::string(req.method_string());
			auto target_view = std::string(req.target());
			auto pa = std::string(req[http::field::proxy_authorization]);

			XLOG_DBG << "connection id: "
				<< m_connection_id
				<< ", method: " << mth
				<< ", target: " << target_view
				<< (pa.empty() ? std::string()
					: ", proxy_authorization: " + pa);

			// http 代理认证.
			auto auth = http_proxy_authorization(pa);
			if (auth != PROXY_AUTH_SUCCESS)
			{
				XLOG_WARN << "connection id: "
					<< m_connection_id
					<< ", proxy err: "
					<< proxy_auth_error_message(auth);

				auto fake_page = fmt::vformat(fake_407_content_fmt,
					fmt::make_format_args(server_date_string()));

				co_await net::async_write(
					m_local_socket,
					net::buffer(fake_page),
					net::transfer_all(),
					net_awaitable[ec]);

				co_return true;
			}

			auto pos = target_view.find(':');
			if (pos == std::string::npos)
			{
				XLOG_ERR  << "connection id: "
					<< m_connection_id
					<< ", illegal target: "
					<< target_view;
				co_return false;
			}

			std::string host(target_view.substr(0, pos));
			std::string port(target_view.substr(pos + 1));

			co_await start_connect_host(host,
				static_cast<uint16_t>(std::atol(port.c_str())), ec, true);
			if (ec)
			{
				XLOG_WFMT("connection id: {},"
					" connect to target {}:{} error: {}",
					m_connection_id,
					host,
					port,
					ec.message());
				co_return false;
			}

			http::response<http::empty_body> res{
				http::status::ok, req.version() };
			res.reason("Connection established");

			co_await http::async_write(
				m_local_socket,
				res,
				net_awaitable[ec]);
			if (ec)
			{
				XLOG_WFMT("connection id: {},"
					" async write response {}:{} error: {}",
					m_connection_id,
					host,
					port,
					ec.message());
				co_return false;
			}

			co_await(
				transfer(m_local_socket, m_remote_socket)
				&&
				transfer(m_remote_socket, m_local_socket)
				);

			XLOG_DBG << "connection id: "
				<< m_connection_id
				<< ", transfer completed";

			co_return true;
		}

		inline net::awaitable<bool> socks_auth()
		{
			//  +----+------+----------+------+----------+
			//  |VER | ULEN |  UNAME   | PLEN |  PASSWD  |
			//  +----+------+----------+------+----------+
			//  | 1  |  1   | 1 to 255 |  1   | 1 to 255 |
			//  +----+------+----------+------+----------+
			//  [           ]

			boost::system::error_code ec;
			m_local_buffer.consume(m_local_buffer.size());
			auto bytes = co_await net::async_read(m_local_socket,
				m_local_buffer,
				net::transfer_exactly(2),
				net_awaitable[ec]);
			if (ec)
			{
				XLOG_WARN << "connection id: "
					<< m_connection_id
					<< ", read client username/passwd error: "
					<< ec.message();
				co_return false;
			}

			auto p = net::buffer_cast<const char*>(m_local_buffer.data());
			int auth_version = read<int8_t>(p);
			if (auth_version != 1)
			{
				XLOG_WARN << "connection id: "
					<< m_connection_id
					<< ", socks negotiation, unsupported socks5 protocol";
				co_return false;
			}
			int name_length = read<uint8_t>(p);
			if (name_length <= 0 || name_length > 255)
			{
				XLOG_WARN << "connection id: "
					<< m_connection_id
					<< ", socks negotiation, invalid name length";
				co_return false;
			}
			name_length += 1;

			//  +----+------+----------+------+----------+
			//  |VER | ULEN |  UNAME   | PLEN |  PASSWD  |
			//  +----+------+----------+------+----------+
			//  | 1  |  1   | 1 to 255 |  1   | 1 to 255 |
			//  +----+------+----------+------+----------+
			//              [                 ]
			m_local_buffer.consume(m_local_buffer.size());
			bytes = co_await net::async_read(m_local_socket,
				m_local_buffer,
				net::transfer_exactly(name_length),
				net_awaitable[ec]);
			if (ec)
			{
				XLOG_WARN << "connection id: "
					<< m_connection_id
					<< ", read client username error: "
					<< ec.message();
				co_return false;
			}

			std::string uname;

			p = net::buffer_cast<const char*>(m_local_buffer.data());
			for (size_t i = 0; i < bytes - 1; i++)
				uname.push_back(read<int8_t>(p));

			int passwd_len = read<uint8_t>(p);
			if (passwd_len <= 0 || passwd_len > 255)
			{
				XLOG_WARN << "connection id: "
					<< m_connection_id
					<< ", socks negotiation, invalid passwd length";
				co_return false;
			}

			//  +----+------+----------+------+----------+
			//  |VER | ULEN |  UNAME   | PLEN |  PASSWD  |
			//  +----+------+----------+------+----------+
			//  | 1  |  1   | 1 to 255 |  1   | 1 to 255 |
			//  +----+------+----------+------+----------+
			//                                [          ]
			m_local_buffer.consume(m_local_buffer.size());
			bytes = co_await net::async_read(m_local_socket,
				m_local_buffer,
				net::transfer_exactly(passwd_len),
				net_awaitable[ec]);
			if (ec)
			{
				XLOG_WARN << "connection id: "
					<< m_connection_id
					<< ", read client passwd error: "
					<< ec.message();
				co_return false;
			}

			std::string passwd;

			p = net::buffer_cast<const char*>(m_local_buffer.data());
			for (size_t i = 0; i < bytes; i++)
				passwd.push_back(read<int8_t>(p));

			// SOCKS5验证用户和密码.
			auto endp = m_local_socket.remote_endpoint();
			auto client = endp.address().to_string();
			client += ":" + std::to_string(endp.port());

			// 用户认证逻辑.
			bool verify_passed = m_option.auth_users_.empty();

			for (auto [user, pwd] : m_option.auth_users_)
			{
				if (uname == user && passwd == pwd)
				{
					verify_passed = true;
					break;
				}
			}

			XLOG_DBG << "connection id: "
				<< m_connection_id
				<< ", auth: "
				<< uname
				<< ", passwd: "
				<< passwd
				<< ", client: "
				<< client;

			net::streambuf wbuf;
			auto wp = net::buffer_cast<char*>(wbuf.prepare(16));
			write<uint8_t>(0x01, wp);			// version 只能是1.
			if (verify_passed)
			{
				write<uint8_t>(0x00, wp);		// 认证通过返回0x00, 其它值为失败.
			}
			else
			{
				write<uint8_t>(0x01, wp);		// 认证返回0x01为失败.
			}

			// 返回认证状态.
			//  +----+--------+
			//  |VER | STATUS |
			//  +----+--------+
			//  | 1  |   1    |
			//  +----+--------+
			wbuf.commit(2);
			co_await net::async_write(m_local_socket,
				wbuf,
				net::transfer_exactly(2),
				net_awaitable[ec]);
			if (ec)
			{
				XLOG_WARN << "connection id: "
					<< m_connection_id
					<< ", server write status error: "
					<< ec.message();
				co_return false;
			}

			co_return verify_passed;
		}

		template<typename S1, typename S2>
		net::awaitable<void> transfer(S1& from, S2& to)
		{
			std::vector<char> data(1024 * 1024, 0);
			boost::system::error_code ec;

			for (; !m_abort;)
			{
				auto bytes = co_await from.async_read_some(
					net::buffer(data), net_awaitable[ec]);
				if (ec || m_abort)
				{
					if (bytes > 0)
						co_await net::async_write(to,
							net::buffer(data, bytes), net_awaitable[ec]);

					to.shutdown(tcp_socket::shutdown_send, ec);
					co_return;
				}

				co_await net::async_write(to,
					net::buffer(data, bytes), net_awaitable[ec]);
				if (ec || m_abort)
				{
					from.shutdown(tcp_socket::shutdown_receive, ec);
					co_return;
				}
			}
		}

		inline net::awaitable<boost::system::error_code>
		start_noise(tcp_socket& socket)
		{
			boost::system::error_code error;

			// 生成 noise 数据, 按 noise_injection_max_len 长度生成.
			std::vector<uint8_t> noise =
				generate_noise(nosie_injection_max_len, global_known_proto);

			XLOG_DBG << "connection id: "
				<< m_connection_id
				<< ", send noise to upstream, length: "
				<< noise.size();

			// 发送 noise 数据.
			co_await net::async_write(
				socket,
				net::buffer(noise),
				net_awaitable[error]);

			if (error)
			{
				XLOG_WARN << "connection id: "
					<< m_connection_id
					<< ", write noise error: "
					<< error.message();

				co_return error;
			}

			// 读取远程返回的 noise 数据.
			size_t len = 0;
			int noise_length = -1;
			int recv_length = 2;
			uint8_t bufs[2];
			uint16_t fvalue = 0;
			uint16_t cvalue = 0;

			while (true)
			{
				if (m_abort)
					co_return net::error::operation_aborted;

				fvalue = cvalue;

				co_await net::async_read(
					socket,
					net::buffer(bufs, recv_length),
					net_awaitable[error]);

				if (error)
				{
					XLOG_WARN << "connection id: "
						<< m_connection_id
						<< ", noise read error: "
						<< error.message();

					co_return error;
				}

				cvalue =
					static_cast<uint16_t>(bufs[1]) |
					(static_cast<uint16_t>(bufs[0]) << 8);

				len += recv_length;
				if (len == 1)
					continue;

				if (len >= nosie_injection_max_len)
				{
					XLOG_WARN << "connection id: "
						<< m_connection_id
						<< ", noise max length reached";

					co_return error;
				}

				if (noise_length != -1)
				{
					recv_length = noise_length - len;
					recv_length = std::min(recv_length, 2);

					if (recv_length != 0)
						continue;

					XLOG_DBG << "connection id: "
						<< m_connection_id
						<< ", noise length: "
						<< noise_length
						<< ", receive completed";

					break;
				}

				noise_length = fvalue & cvalue;
				if (noise_length >= nosie_injection_max_len ||
					noise_length < 4)
				{
					noise_length = -1;
					continue;
				}

				XLOG_DBG << "connection id: "
					<< m_connection_id
					<< ", noise length: "
					<< noise_length
					<< ", receive";
			}

			error = {};
			co_return error;
		}

		inline net::awaitable<bool> start_connect_host(
			std::string target_host,
			uint16_t target_port,
			boost::system::error_code& ec,
			bool resolve = false)
		{
			auto executor = co_await net::this_coro::executor;

			// 获取构造函数中临时创建的tcp::socket.
			tcp_socket& remote_socket =
				boost::variant2::get<tcp_socket>(m_remote_socket);

			auto bind_interface = net::ip::address::from_string(
				m_option.local_ip_, ec);
			if (ec)
			{
				// bind 地址有问题, 忽略bind参数.
				m_option.local_ip_.clear();
			}

			auto check_condition = [this, bind_interface](
				const boost::system::error_code&,
				auto& stream, auto&) mutable
			{
				if (m_option.local_ip_.empty())
					return true;

				tcp::endpoint bind_endpoint(bind_interface, 0);
				boost::system::error_code err;

				stream.open(bind_endpoint.protocol(), err);
				if (err)
					return false;

				stream.bind(bind_endpoint, err);
				if (err)
					return false;

				return true;
			};

			if (m_next_proxy)
			{
				tcp::resolver resolver{ executor };

				auto proxy_host = std::string(m_next_proxy->host());
				auto proxy_port = std::string(m_next_proxy->port());
				if (proxy_port.empty())
					proxy_port = m_next_proxy->scheme();

				auto targets = co_await resolver.async_resolve(
					proxy_host,
					proxy_port,
					net_awaitable[ec]);

				if (ec)
				{
					XLOG_WFMT("connection id: {},"
						" resolver to next proxy {}:{} error: {}",
						m_connection_id,
						std::string(m_next_proxy->host()),
						std::string(m_next_proxy->port()),
						ec.message());

					co_return false;
				}

				if (m_option.happyeyeballs_)
				{
					co_await asio_util::async_connect(
						remote_socket,
						targets,
						check_condition,
						net_awaitable[ec]);
				}
				else
				{
					for (auto endpoint : targets)
					{
						remote_socket.close(ec);

						if (!m_option.local_ip_.empty())
						{
							tcp::endpoint bind_endpoint(
								bind_interface,
								0);

							remote_socket.open(
								bind_endpoint.protocol(),
								ec);
							if (ec)
								break;

							remote_socket.bind(
								bind_endpoint,
								ec);
							if (ec)
								break;
						}

						co_await remote_socket.async_connect(
							endpoint,
							net_awaitable[ec]);
						if (!ec)
							break;
					}
				}

				if (ec)
				{
					XLOG_WFMT("connection id: {},"
						" connect to next proxy {}:{} error: {}",
						m_connection_id,
						std::string(m_next_proxy->host()),
						std::string(m_next_proxy->port()),
						ec.message());

					co_return false;
				}

				// 如果启用了 noise, 则在向上游代理服务器发起 tcp 连接成功后, 发送 noise
				// 数据以及接收 noise 数据.
				if (m_option.scramble_)
				{
					ec = co_await start_noise(remote_socket);
					if (ec)
						co_return false;
				}

				// 使用ssl加密与下一级代理通信.
				if (m_option.proxy_pass_use_ssl_)
				{
					// 设置 ssl cert 证书目录.
					if (fs::exists(m_option.ssl_cert_path_))
					{
						m_ssl_context.add_verify_path(
							m_option.ssl_cert_path_, ec);
						if (ec)
						{
							XLOG_WFMT("connection id: {}, "
								"load cert path: {}, "
								"error: {}",
								m_connection_id,
								m_option.ssl_cert_path_,
								ec.message());

							co_return false;
						}
					}
				}

				auto scheme = m_next_proxy->scheme();

				auto instantiate_stream =
					[this,
					&scheme,
					&proxy_host,
					&remote_socket,
					&ec]
				() mutable -> net::awaitable<proxy_stream_type>
				{
					ec = {};

					if (m_option.proxy_pass_use_ssl_ || scheme == "https")
					{
						m_ssl_context.set_verify_mode(net::ssl::verify_peer);
						auto cert = default_root_certificates();
						m_ssl_context.add_certificate_authority(
							net::buffer(cert.data(), cert.size()),
							ec);
						if (ec)
						{
							XLOG_WFMT("connection id: {},"
								" add_certificate_authority error: {}",
								m_connection_id,
								ec.message());
						}

						m_ssl_context.set_verify_callback(
							net::ssl::rfc2818_verification(proxy_host), ec);
						if (ec)
						{
							XLOG_WFMT("connection id: {},"
								" set_verify_callback error: {}",
								m_connection_id,
								ec.message());
						}

						auto socks_stream = instantiate_proxy_stream(
							std::move(remote_socket), m_ssl_context);

						// get origin ssl stream type.
						ssl_stream& ssl_socket =
							boost::variant2::get<ssl_stream>(socks_stream);

						std::string sni = m_option.ssl_sni_.empty()
							? proxy_host : m_option.ssl_sni_;

						// Set SNI Hostname.
						if (!SSL_set_tlsext_host_name(
							ssl_socket.native_handle(), sni.c_str()))
						{
							XLOG_WFMT("connection id: {},"
							" SSL_set_tlsext_host_name error: {}",
								m_connection_id,
								::ERR_get_error());
						}

						// do async handshake.
						co_await ssl_socket.async_handshake(
							net::ssl::stream_base::client,
							net_awaitable[ec]);
						if (ec)
						{
							XLOG_WFMT("connection id: {},"
								" ssl protocol handshake error: {}",
								m_connection_id,
								ec.message());
						}

						XLOG_FMT("connection id: {}, ssl handshake: {}",
							m_connection_id,
							proxy_host);

						co_return socks_stream;
					}

					co_return instantiate_proxy_stream(
						std::move(remote_socket));
				};

				m_remote_socket = std::move(co_await instantiate_stream());

				if (scheme.starts_with("socks"))
				{
					socks_client_option opt;

					opt.target_host = target_host;
					opt.target_port = target_port;
					opt.proxy_hostname = true;
					opt.username = std::string(m_next_proxy->user());
					opt.password = std::string(m_next_proxy->password());

					if (scheme == "socks4")
						opt.version = socks4_version;
					else if (scheme == "socks4a")
						opt.version = socks4a_version;

					co_await async_socks_handshake(
						m_remote_socket,
						opt,
						net_awaitable[ec]);
				}
				else if (scheme.starts_with("http"))
				{
					http_proxy_client_option opt;

					opt.target_host = target_host;
					opt.target_port = target_port;
					opt.username = std::string(m_next_proxy->user());
					opt.password = std::string(m_next_proxy->password());

					co_await async_http_proxy_handshake(
						m_remote_socket,
						opt,
						net_awaitable[ec]);
				}

				if (ec)
				{
					XLOG_WFMT("connection id: {}"
						", {} connect to next host {}:{} error: {}",
						m_connection_id,
						std::string(scheme),
						target_host,
						target_port,
						ec.message());

					co_return false;
				}
			}
			else
			{
				net::ip::basic_resolver_results<tcp> targets;
				if (resolve)
				{
					tcp::resolver resolver{ executor };

					targets = co_await resolver.async_resolve(
						target_host,
						std::to_string(target_port),
						net_awaitable[ec]);
					if (ec)
					{
						XLOG_WARN << "connection id: "
							<< m_connection_id
							<< ", resolve: "
							<< target_host
							<< ", error: "
							<< ec.message();

						co_return false;
					}
				}
				else
				{
					tcp::endpoint dst_endpoint;

					dst_endpoint.address(
						net::ip::address::from_string(target_host));
					dst_endpoint.port(target_port);

					targets = net::ip::basic_resolver_results<tcp>::create(
						dst_endpoint, "", "");
				}

				co_await asio_util::async_connect(remote_socket,
					targets, check_condition, net_awaitable[ec]);
				if (ec)
				{
					XLOG_WFMT("connection id: {}, connect to target {}:{} error: {}",
						m_connection_id,
						target_host,
						target_port,
						ec.message());

					co_return false;
				}

				m_remote_socket = instantiate_proxy_stream(
					std::move(remote_socket));
			}

			co_return true;
		}

		// is_crytpo_stream 判断当前连接是否为加密连接.
		inline bool is_crytpo_stream() const
		{
			return boost::variant2::holds_alternative<ssl_stream>(m_remote_socket);
		}

		inline net::awaitable<void>
		normal_web_server(http::request<http::string_body>& req, std::optional<request_parser>& parser)
		{
			boost::system::error_code ec;

			bool keep_alive = false;
			bool has_read_header = true;

			for (; !m_abort;)
			{
				if (!has_read_header)
				{
					// normal_web_server 调用是从 http_proxy_get
					// 跳转过来的, 该函数已经读取了请求头, 所以第1次不需
					// 要再次读取请求头, 即 has_read_header 为 true.
					// 当 keepalive 时，需要读取请求头, 此时 has_read_header
					// 为 false, 则在此读取和解析后续的 http 请求头.
					parser.emplace();
					parser->body_limit(1024 * 512); // 512k
					m_local_buffer.consume(m_local_buffer.size());

					co_await http::async_read_header(
						m_local_socket,
						m_local_buffer,
						*parser,
						net_awaitable[ec]);
					if (ec)
					{
						XLOG_DBG << "connection id: "
							<< m_connection_id
							<< (keep_alive ? ", keepalive" : "")
							<< ", web async_read_header: "
							<< ec.message();
						co_return;
					}

					req = parser->release();
				}

				if (req[http::field::expect] == "100-continue")
				{
					http::response<http::empty_body> res;
					res.version(11);
					res.result(http::status::method_not_allowed);

					co_await http::async_write(
						m_local_socket,
						res,
						net_awaitable[ec]);
					if (ec)
					{
						XLOG_DBG << "connection id: "
							<< m_connection_id
							<< ", web expect async_write: "
							<< ec.message();
					}
					co_return;
				}

				has_read_header = false;
				keep_alive = req.keep_alive();

				if (beast::websocket::is_upgrade(req))
				{
					auto fake_page = fmt::vformat(fake_404_content_fmt,
						fmt::make_format_args(server_date_string()));

					co_await net::async_write(
						m_local_socket,
						net::buffer(fake_page),
						net::transfer_all(),
						net_awaitable[ec]);

					co_return;
				}

				std::string target = req.target();
				boost::smatch what;
				http_context http_ctx{ {}, req, *parser, target, target_path(req) };

				#define BEGIN_HTTP_ROUTE() if (false) {}
				#define ON_HTTP_ROUTE(exp, func) \
				else if (boost::regex_match( \
					target, what, boost::regex{ exp })) { \
					for (auto i = 1; i < static_cast<int>(what.size()); i++) \
						http_ctx.command_.emplace_back(what[i]); \
					co_await func(http_ctx); \
				}
				#define END_HTTP_ROUTE() else { \
					co_await default_http_route( \
						req, \
						fake_400_content, \
						http::status::bad_request ); }

				BEGIN_HTTP_ROUTE()
					ON_HTTP_ROUTE("^(.*)?/$", on_http_dir)
					ON_HTTP_ROUTE("^(?!.*\\/$).*$", on_http_get)
				END_HTTP_ROUTE()

				if (!keep_alive) break;
				continue;
			}

			if (!keep_alive)
			{
				m_local_socket.shutdown(
					net::socket_base::shutdown_both, ec);
			}

			co_return;
		}

		inline fs::path path_cat(
			const std::wstring& doc, const std::wstring& target)
		{
			size_t start_pos = 0;
			for (auto& c : target)
			{
				if (!(c == L'/' || c == '\\'))
					break;

				start_pos++;
			}

			std::wstring_view sv;
			std::wstring slash = L"/";

			if (start_pos < target.size())
				sv = std::wstring_view(target.c_str() + start_pos);
#ifdef WIN32
			slash = L"\\";
			if (doc.back() == L'/' ||
				doc.back() == L'\\')
				slash = L"";
			return fs::path(doc + slash + std::wstring(sv));
#else
			if (doc.back() == L'/')
				slash = L"";
			return fs::path(
				boost::nowide::narrow(doc + slash + std::wstring(sv)));
#endif // WIN32
		};

		inline std::string target_path(const string_request& req)
		{
			std::string target = req.target();
			unescape(std::string(target), target);

			auto doc_path = boost::nowide::widen(m_option.doc_directory_);
			auto path = path_cat(doc_path, boost::nowide::widen(target)).string();

			return path;
		}

		inline std::tuple<std::string, fs::path> file_last_wirte_time(const fs::path& file)
		{
			static auto loc_time = [](auto t) -> struct tm*
			{
				using time_type = std::decay_t<decltype(t)>;
				if constexpr (std::is_same_v<time_type, std::filesystem::file_time_type>)
				{
					auto sctp = std::chrono::time_point_cast<
						std::chrono::system_clock::duration>(t -
							std::filesystem::file_time_type::clock::now() +
								std::chrono::system_clock::now());
					auto time = std::chrono::system_clock::to_time_t(sctp);
					return std::localtime(&time);
				}
				else if constexpr (std::is_same_v<time_type, std::time_t>)
				{
					return std::localtime(&t);
				}
				else
				{
					static_assert(!std::is_same_v<time_type, time_type>, "time type required!");
				}
			};

			boost::system::error_code ec;
			std::string time_string;
			fs::path unc_path;

			auto ftime = fs::last_write_time(file, ec);
			if (ec)
			{
		#ifdef WIN32
				if (file.string().size() > MAX_PATH)
				{
					auto str = file.string();
					boost::replace_all(str, "/", "\\");
					unc_path = "\\\\?\\" + str;
					ftime = fs::last_write_time(unc_path, ec);
				}
		#endif
			}

			if (!ec)
			{
				auto tm = loc_time(ftime);

				char tmbuf[64] = { 0 };
				std::strftime(tmbuf,
					sizeof(tmbuf),
					"%m-%d-%Y %H:%M",
					tm);

				time_string = tmbuf;
			}

			return { time_string, unc_path };
		}

		inline std::vector<std::wstring>
		format_path_list(const std::set<fs::path>& paths)
		{
			boost::system::error_code ec;
			std::vector<std::wstring> path_list;

			for (auto it = paths.cbegin(); it != paths.cend() && !m_abort; it++)
			{
				const auto& item = *it;

				auto [ftime, unc_path] = file_last_wirte_time(item);
				std::wstring time_string = boost::nowide::widen(ftime);

				std::wstring rpath;

				if (fs::is_directory(item, ec))
				{
					auto leaf = boost::nowide::narrow(item.filename().wstring());
					leaf = leaf + "/";
					rpath = boost::nowide::widen(leaf);
					int width = 50 - static_cast<int>(rpath.size());
					width = width < 0 ? 0 : width;
					std::wstring space(width, L' ');
					auto show_path = rpath;
					if (show_path.size() > 50) {
						show_path = show_path.substr(0, 47);
						show_path += L"..&gt;";
					}
					auto str = fmt::format(body_fmt,
						rpath,
						show_path,
						space,
						time_string,
						L"-");

					path_list.push_back(str);
				}
				else
				{
					auto leaf =  boost::nowide::narrow(item.filename().wstring());
					rpath = boost::nowide::widen(leaf);
					int width = 50 - (int)rpath.size();
					width = width < 0 ? 0 : width;
					std::wstring space(width, L' ');
					std::wstring filesize;
					if (unc_path.empty())
						unc_path = item;
					auto sz = static_cast<float>(fs::file_size(
						unc_path, ec));
					if (ec)
						sz = 0;
					filesize = boost::nowide::widen(
						add_suffix(sz));
					auto show_path = rpath;
					if (show_path.size() > 50) {
						show_path = show_path.substr(0, 47);
						show_path += L"..&gt;";
					}
					auto str = fmt::format(body_fmt,
						rpath,
						show_path,
						space,
						time_string,
						filesize);

					path_list.push_back(str);
				}
			}

			return path_list;
		}

		inline net::awaitable<void> on_http_dir(const http_context& hctx)
		{
			using namespace std::literals;

			boost::system::error_code ec;
			auto& request = hctx.request_;

			fs::directory_iterator end;
			fs::directory_iterator it(hctx.target_path_, ec);
			if (ec)
			{
				string_response res{ http::status::found, request.version() };
				res.set(http::field::server, version_string);
				res.set(http::field::date, server_date_string());
				res.set(http::field::location, "/");
				res.keep_alive(request.keep_alive());
				res.prepare_payload();

				http::serializer<false, string_body, http::fields> sr(res);
				co_await http::async_write(
					m_local_socket,
					sr,
					net_awaitable[ec]);
				if (ec)
					XLOG_WARN << "connection id: "
					<< m_connection_id
					<< ", http_dir write location err: "
					<< ec.message();

				co_return;
			}

			// 遍历目录, 生成目录列表和文件列表.
			std::set<fs::path> dirs;
			std::set<fs::path> files;

			for (; it != end && !m_abort; it++)
			{
				const auto& item = it->path();
				if (fs::is_directory(item, ec))
					dirs.insert(item);
				else
					files.insert(item);
			}

			std::vector<std::wstring> path_list;

			path_list = format_path_list(dirs);
			auto file_list = format_path_list(files);
			path_list.insert(path_list.end(), file_list.begin(), file_list.end());

			auto target_path = boost::nowide::widen(hctx.target_);
			std::wstring head = fmt::format(head_fmt,
				target_path,
				target_path);

			std::wstring body = fmt::format(body_fmt,
				L"../",
				L"../",
				L"",
				L"",
				L"");

			for (auto& s : path_list)
				body += s;
			body = head + body + tail_fmt;

			string_response res{ http::status::ok, request.version() };
			res.set(http::field::server, version_string);
			res.set(http::field::date, server_date_string());
			res.keep_alive(request.keep_alive());
			res.body() = boost::nowide::narrow(body);
			res.prepare_payload();

			http::serializer<false, string_body, http::fields> sr(res);
			co_await http::async_write(
				m_local_socket,
				sr,
				net_awaitable[ec]);
			if (ec)
				XLOG_WARN << "connection id: "
				<< m_connection_id
				<< ", http dir write body err: "
				<< ec.message();

			co_return;
		}

		inline net::awaitable<void> on_http_get(const http_context& hctx)
		{
			static std::map<std::string, std::string> mimes =
			{
				{ ".html", "text/html; charset=utf-8" },
				{ ".htm", "text/html; charset=utf-8" },
				{ ".js", "application/javascript" },
				{ ".h", "text/javascript" },
				{ ".hpp", "text/javascript" },
				{ ".cpp", "text/javascript" },
				{ ".cxx", "text/javascript" },
				{ ".cc", "text/javascript" },
				{ ".c", "text/javascript" },
				{ ".json", "application/json" },
				{ ".css", "text/css" },
				{ ".txt", "text/plain; charset=utf-8" },
				{ ".md", "text/plain; charset=utf-8" },
				{ ".log", "text/plain; charset=utf-8" },
				{ ".xml", "text/xml" },
				{ ".ico", "image/x-icon" },
				{ ".ttf", "application/x-font-ttf" },
				{ ".eot", "application/vnd.ms-fontobject" },
				{ ".woff", "application/x-font-woff" },
				{ ".pdf", "application/pdf" },
				{ ".png", "image/png" },
				{ ".jpg", "image/jpg" },
				{ ".jpeg", "image/jpg" },
				{ ".gif", "image/gif" },
				{ ".webp", "image/webp" },
				{ ".svg", "image/svg+xml" },
				{ ".wav", "audio/x-wav" },
				{ ".ogg", "video/ogg" },
				{ ".mp4", "video/mp4" },
				{ ".flv", "video/x-flv" },
				{ ".f4v", "video/x-f4v" },
				{ ".ts", "video/MP2T" },
				{ ".mov", "video/quicktime" },
				{ ".avi", "video/x-msvideo" },
				{ ".wmv", "video/x-ms-wmv" },
				{ ".3gp", "video/3gpp" },
				{ ".mkv", "video/x-matroska" },
				{ ".7z", "application/x-7z-compressed" },
				{ ".ppt", "application/vnd.ms-powerpoint" },
				{ ".zip", "application/zip" },
				{ ".xz", "application/x-xz" },
				{ ".xml", "application/xml" },
				{ ".webm", "video/webm" }
			};

			using ranges = std::vector<std::pair<int64_t, int64_t>>;
			static auto get_ranges = [](std::string range) -> ranges
			{
				range = strutil::remove_spaces(range);
				boost::ireplace_first(range, "bytes=", "");

				boost::sregex_iterator it(
					range.begin(), range.end(),
					boost::regex{ "((\\d+)-(\\d+))+" });

				ranges result;
				std::for_each(it, {}, [&result](const auto& what) mutable
					{
						result.emplace_back(
							std::make_pair(
								std::atoll(what[2].str().c_str()),
								std::atoll(what[3].str().c_str())));
					});

				if (result.empty() && !range.empty())
				{
					if (range.front() == '-')
					{
						auto r = std::atoll(range.c_str());
						result.emplace_back(std::make_pair(r, -1));
					}
					else if (range.back() == '-')
					{
						auto r = std::atoll(range.c_str());
						result.emplace_back(std::make_pair(r, -1));
					}
				}

				return result;
			};

			boost::system::error_code ec;

			auto& request = hctx.request_;
			const fs::path path = hctx.target_path_;

			if (!fs::exists(path, ec))
			{
				XLOG_WARN << "connection id: "
					<< m_connection_id
					<< ", http "
					<< hctx.target_
					<< " file not exists";

				auto fake_page = fmt::vformat(fake_404_content_fmt,
					fmt::make_format_args(server_date_string()));

				co_await net::async_write(
					m_local_socket,
					net::buffer(fake_page),
					net::transfer_all(),
					net_awaitable[ec]);

				co_return;
			}

			if (fs::is_directory(path, ec))
			{
				XLOG_DBG << "connection id: "
					<< m_connection_id
					<< ", http "
					<< hctx.target_
					<< " is directory";

				std::string url = "http://";
				if (is_crytpo_stream())
					url = "https://";
				url += request[http::field::host];
				urls::url u(url);
				std::string target = hctx.target_ + "/";
				u.set_path(target);

				co_await location_http_route(request, u.buffer());

				co_return;
			}

			size_t content_length = fs::file_size(path, ec);
			if (ec)
			{
				XLOG_WARN << "connection id: "
					<< m_connection_id
					<< ", http "
					<< hctx.target_
					<< " file size error: "
					<< ec.message();

				co_await default_http_route(
					request, fake_400_content, http::status::bad_request);

				co_return;
			}

			std::fstream file(path.string(),
				std::ios_base::binary |
				std::ios_base::in);

			XLOG_DBG << "connection id: "
				<< m_connection_id
				<< ", http file: "
				<< hctx.target_
				<< ", size: "
				<< content_length;

			auto range = get_ranges(request["Range"]);
			http::status st = http::status::ok;
			if (!range.empty())
			{
				st = http::status::partial_content;
				auto& r = range.front();

				if (r.second == -1)
				{
					if (r.first < 0)
					{
						r.first = content_length + r.first;
						r.second = content_length - 1;
					}
					else if (r.first >= 0)
					{
						r.second = content_length - 1;
					}
				}

				file.seekg(r.first, std::ios_base::beg);
			}

			buffer_response res{ http::status::ok, request.version() };

			res.set(http::field::server, version_string);
			res.set(http::field::date, server_date_string());

			auto ext = to_lower(fs::path(path).extension().string());

			if (mimes.count(ext))
				res.set(http::field::content_type, mimes[ext]);
			else
				res.set(http::field::content_type, "text/plain");

			if (st == http::status::ok)
				res.set(http::field::accept_ranges, "bytes");

			if (st == http::status::partial_content)
			{
				const auto& r = range.front();

				if (r.second < r.first && r.second >= 0)
				{
					co_await default_http_route(request,
						fake_416_content,
						http::status::range_not_satisfiable);
					co_return;
				}

				std::string content_range = fmt::format(
					"bytes {}-{}/{}",
					r.first,
					r.second,
					content_length);
				content_length = r.second - r.first + 1;
				res.set(http::field::content_range, content_range);
			}

			res.keep_alive(hctx.request_.keep_alive());
			res.content_length(content_length);

			response_serializer sr(res);

			res.body().data = nullptr;
			res.body().more = false;

			co_await http::async_write_header(
				m_local_socket,
				sr,
				net_awaitable[ec]);
			if (ec)
			{
				XLOG_WARN << "connection id: "
					<< m_connection_id
					<< ", http async_write_header: "
					<< ec.message();

				co_return;
			}

			const auto buf_size = 5 * 1024 * 1024;
			auto bufs = std::make_unique<char[]>(buf_size);
			char* buf = bufs.get();
			std::streamsize total = 0;

			do
			{
				auto bytes_transferred = fileop::read(file, std::span<char>(buf, buf_size));
				bytes_transferred = std::min<std::streamsize>(
					bytes_transferred,
					content_length - total
				);
				if (bytes_transferred == 0 ||
					total >= (std::streamsize)content_length)
				{
					res.body().data = nullptr;
					res.body().more = false;
				}
				else
				{
					res.body().data = buf;
					res.body().size = bytes_transferred;
					res.body().more = true;
				}

				co_await http::async_write(
					m_local_socket,
					sr,
					net_awaitable[ec]);
				total += bytes_transferred;
				if (ec == http::error::need_buffer)
				{
					ec = {};
					continue;
				}
				if (ec)
				{
					XLOG_WARN << "connection id: "
						<< m_connection_id
						<< ", http async_write: "
						<< ec.message();
					co_return;
				}
			} while (!sr.is_done());

			XLOG_DBG << "connection id: "
				<< m_connection_id
				<< ", http request: "
				<< hctx.target_
				<< ", completed";

			co_return;
		}

		inline std::string server_date_string()
		{
			auto time = std::time(nullptr);
			auto gmt = gmtime((const time_t*)&time);

			std::string str(64, '\0');
			auto ret = strftime((char*)str.data(), 64, "%a, %d %b %Y %H:%M:%S GMT", gmt);
			str.resize(ret);

			return str;
		}

		inline net::awaitable<void> default_http_route(
			const string_request& request, std::string response, http::status status)
		{
			boost::system::error_code ec;

			string_response res{ status, request.version() };
			res.set(http::field::server, version_string);
			res.set(http::field::date, server_date_string());
			res.set(http::field::content_type, "text/html");

			res.keep_alive(true);
			res.body() = response;
			res.prepare_payload();

			http::serializer<false, string_body, http::fields> sr(res);
			co_await http::async_write(m_local_socket, sr, net_awaitable[ec]);
			if (ec)
			{
				XLOG_WARN << "connection id: "
					<< m_connection_id
					<< ", default http route err: "
					<< ec.message();
			}

			co_return;
		}

		inline net::awaitable<void> location_http_route(
			const string_request& request, const std::string& path)
		{
			boost::system::error_code ec;

			string_response res{ http::status::moved_permanently, request.version() };
			res.set(http::field::server, version_string);
			res.set(http::field::date, server_date_string());
			res.set(http::field::content_type, "text/html");
			res.set(http::field::location, path);

			res.keep_alive(true);
			res.body() = fake_302_content;
			res.prepare_payload();

			http::serializer<false, string_body, http::fields> sr(res);
			co_await http::async_write(m_local_socket, sr, net_awaitable[ec]);
			if (ec)
			{
				XLOG_WARN << "connection id: "
					<< m_connection_id
					<< ", location http route err: "
					<< ec.message();
			}

			co_return;
		}

		inline net::awaitable<void> forbidden_http_route(const string_request& request)
		{
			boost::system::error_code ec;

			string_response res{ http::status::forbidden, request.version() };
			res.set(http::field::server, version_string);
			res.set(http::field::date, server_date_string());
			res.set(http::field::content_type, "text/html");

			res.keep_alive(true);
			res.body() = fake_403_content;
			res.prepare_payload();

			http::serializer<false, string_body, http::fields> sr(res);
			co_await http::async_write(
				m_local_socket, sr, net_awaitable[ec]);
			if (ec)
			{
				XLOG_WARN << "connection id: "
					<< m_connection_id
					<< ", forbidden http route err: "
					<< ec.message();
			}
		}

	private:
		proxy_stream_type m_local_socket;
		proxy_stream_type m_remote_socket;
		udp::socket m_udp_socket;
		net::ip::address m_local_udp_address;
		net::steady_timer m_timer;
		int m_timeout{ udp_session_expired_time };
		size_t m_connection_id;
		net::streambuf m_local_buffer{};
		std::weak_ptr<proxy_server_base> m_proxy_server;
		proxy_server_option m_option;
		std::unique_ptr<urls::url_view> m_next_proxy;
		net::ssl::context m_ssl_context{ net::ssl::context::sslv23_client };
		bool m_abort{ false };
	};


	//////////////////////////////////////////////////////////////////////////

	class proxy_server
		: public proxy_server_base
		, public std::enable_shared_from_this<proxy_server>
	{
		proxy_server(const proxy_server&) = delete;
		proxy_server& operator=(const proxy_server&) = delete;

		proxy_server(net::io_context::executor_type executor,
			const tcp::endpoint& endp, proxy_server_option opt)
			: m_executor(executor)
			, m_acceptor(executor)
			, m_option(std::move(opt))
		{
			init_ssl_context();

			boost::system::error_code ec;

			m_acceptor.open(endp.protocol(), ec);
			if (ec)
			{
				XLOG_WARN << "acceptor open: " << endp
					<< ", error: " << ec.message();
				throw std::runtime_error(ec.message());
			}

			m_acceptor.set_option(net::socket_base::reuse_address(true), ec);
			if (ec)
			{
				XLOG_WARN << "acceptor set_option with reuse_address: "
					<< ec.message();
			}

			if (m_option.reuse_port_)
			{
#ifdef ENABLE_REUSEPORT
				using net::detail::socket_option::boolean;
				using reuse_port = boolean<SOL_SOCKET, SO_REUSEPORT>;

				m_acceptor.set_option(reuse_port(true), ec);
				if (ec)
				{
					XLOG_WARN << "acceptor set_option with SO_REUSEPORT: "
						<< ec.message();
				}
#endif
			}

			m_acceptor.bind(endp, ec);
			if (ec)
			{
				XLOG_ERR << "acceptor bind: " << endp
					<< ", error: " << ec.message();
				throw std::runtime_error(ec.message());
			}

			m_acceptor.listen(net::socket_base::max_listen_connections, ec);
			if (ec)
			{
				XLOG_ERR << "acceptor listen: " << endp
					<< ", error: " << ec.message();
				throw std::runtime_error(ec.message());
			}
		}

	public:
		inline static std::shared_ptr<proxy_server> make(
			net::io_context::executor_type executor,
			const tcp::endpoint& endp,
			proxy_server_option opt)
		{
			return std::shared_ptr<proxy_server>(new
				proxy_server(executor, std::cref(endp), opt));
		}

		virtual ~proxy_server() = default;

		inline void init_ssl_context()
		{
			m_ssl_context.set_options(
				net::ssl::context::default_workarounds
				| net::ssl::context::no_sslv2
				| net::ssl::context::no_sslv3
				| net::ssl::context::no_tlsv1
				| net::ssl::context::no_tlsv1_1
				| net::ssl::context::single_dh_use
			);

			if (m_option.ssl_prefer_server_ciphers_)
				m_ssl_context.set_options(SSL_OP_CIPHER_SERVER_PREFERENCE);

			const std::string ssl_ciphers = "HIGH:!aNULL:!MD5:!3DES";
			if (m_option.ssl_ciphers_.empty())
				m_option.ssl_ciphers_ = ssl_ciphers;

			SSL_CTX_set_cipher_list(m_ssl_context.native_handle(),
				m_option.ssl_ciphers_.c_str());

			if (!m_option.ssl_cert_path_.empty())
			{
				auto dir = fs::path(m_option.ssl_cert_path_);
				auto pwd = dir / "ssl_crt.pwd";

				if (fs::exists(pwd))
					m_ssl_context.set_password_callback(
						[&pwd]([[maybe_unused]] auto... args) {
							std::string password;
							fileop::read(pwd, password);
							return password;
						}
				);

				auto cert = dir / "ssl_crt.pem";
				auto key = dir / "ssl_key.pem";
				auto dh = dir / "ssl_dh.pem";

				if (fs::exists(cert))
					m_ssl_context.use_certificate_chain_file(cert.string());

				if (fs::exists(key))
					m_ssl_context.use_private_key_file(
						key.string(), boost::asio::ssl::context::pem);

				if (fs::exists(dh))
					m_ssl_context.use_tmp_dh_file(dh.string());
			}
			else
			{
				m_ssl_context.set_password_callback(
					[&]([[maybe_unused]] auto... args) {
						const auto& pwd = m_option.ssl_certificate_passwd_;
						if (!fs::exists(pwd))
							return pwd;

						std::string password;
						fileop::read(pwd, password);

						return password;
					});

				boost::system::error_code ec;

				m_ssl_context.use_certificate_chain_file(
					m_option.ssl_certificate_, ec);
				m_ssl_context.use_private_key_file(
					m_option.ssl_certificate_key_,
					net::ssl::context::pem, ec);
				m_ssl_context.use_tmp_dh_file(
					m_option.ssl_dhparam_, ec);
			}
		}

	public:
		inline void start()
		{
			// 同时启动32个连接协程, 开始为proxy client提供服务.
			for (int i = 0; i < 32; i++)
			{
				net::co_spawn(m_executor,
					start_proxy_listen(m_acceptor), net::detached);
			}
		}

		inline void close()
		{
			boost::system::error_code ignore_ec;
			m_abort = true;

			m_acceptor.close(ignore_ec);

			for (auto& [id, c] : m_clients)
			{
				auto client = c.lock();
				if (!client)
					continue;
				client->close();
			}
		}

	private:
		virtual void remove_session(size_t id) override
		{
			m_clients.erase(id);
		}

		virtual size_t num_session() override
		{
			return m_clients.size();
		}

		virtual const proxy_server_option& option() override
		{
			return m_option;
		}

	private:
		inline net::awaitable<bool>
		noise_process(tcp_socket socket, size_t connection_id)
		{
			boost::system::error_code error;

			std::vector<uint8_t> noise =
				generate_noise(nosie_injection_max_len, global_known_proto);

			XLOG_DBG << "connection id: "
				<< connection_id
				<< ", send noise, length: "
				<< noise.size();

			// 发送 noise 消息.
			co_await net::async_write(
				socket,
				net::buffer(noise),
				net_awaitable[error]);
			if (error)
			{
				XLOG_WARN << "connection id: "
					<< connection_id
					<< ", noise write error: "
					<< error.message();
				co_return false;
			}

			// 接收客户端发过来的 noise 回应消息.
			size_t len = 0;
			int noise_length = -1;
			int recv_length = 2;
			uint8_t bufs[2];
			uint16_t fvalue = 0;
			uint16_t cvalue = 0;

			while (true)
			{
				if (m_abort)
					co_return false;

				fvalue = cvalue;

				co_await net::async_read(
					socket,
					net::buffer(bufs, recv_length),
					net_awaitable[error]);

				if (error) {
					XLOG_WARN << "connection id: "
						<< connection_id
						<< ", noise read error: "
						<< error.message();

					co_return false;
				}

				cvalue =
					static_cast<uint16_t>(bufs[1]) |
					(static_cast<uint16_t>(bufs[0]) << 8);

				len += recv_length;
				if (len == 1)
					continue;

				if (len >= nosie_injection_max_len)
				{
					XLOG_WARN << "connection id: "
						<< connection_id
						<< ", noise max length reached";

					co_return false;
				}

				if (noise_length != -1)
				{
					recv_length = noise_length - len;
					recv_length = std::min(recv_length, 2);

					if (recv_length != 0)
						continue;

					XLOG_DBG << "connection id: "
						<< connection_id
						<< ", noise length: "
						<< noise_length
						<< ", receive completed";

					break;
				}

				noise_length = fvalue & cvalue;
				if (noise_length >= nosie_injection_max_len ||
					noise_length < 4)
				{
					noise_length = -1;
					continue;
				}

				XLOG_DBG << "connection id: "
					<< connection_id
					<< ", noise length: "
					<< noise_length
					<< ", receive";
			}

			// 在完成 noise 握手后, 重新检测协议.
			co_await socket_detect(std::move(socket), connection_id, false);

			co_return true;
		}

		inline net::awaitable<void>
		socket_detect(tcp_socket socket, size_t connection_id, bool noise = true)
		{
			auto self = shared_from_this();
			auto error = boost::system::error_code{};

			// 等待 read 事件以确保下面 recv 偷看数据时能有数据.
			co_await socket.async_wait(
				tcp_socket::wait_read, net_awaitable[error]);
			if (error)
			{
				XLOG_WARN  << "connection id: "
					<< connection_id
					<< ", socket.async_wait error: "
					<< error.message();
				co_return;
			}

			// 检查协议.
			auto fd = socket.native_handle();
			uint8_t detect[5] = { 0 };

#if defined(WIN32) || defined(__APPLE__)
			auto ret = recv(fd, (char*)detect, sizeof(detect),
				MSG_PEEK);
#else
			auto ret = recv(fd, (void*)detect, sizeof(detect),
				MSG_PEEK | MSG_NOSIGNAL | MSG_DONTWAIT);
#endif
			if (ret <= 0)
			{
				XLOG_WARN << "connection id: "
					<< connection_id
					<< ", peek message return: "
					<< ret;
				co_return;
			}

			// 非安全连接检查.
			if (m_option.disable_insecure_)
			{
				if (detect[0] != 0x16)
				{
					XLOG_DBG << "connection id: "
						<< connection_id
						<< ", insecure protocol disabled";
					co_return;
				}
			}

			// plain socks4/5 protocol.
			if (detect[0] == 0x05 || detect[0] == 0x04)
			{
				if (m_option.disable_socks_)
				{
					XLOG_DBG << "connection id: "
						<< connection_id
						<< ", socks protocol disabled";
					co_return;
				}

				XLOG_DBG << "connection id: "
					<< connection_id
					<< ", socks4/5 protocol";

				auto new_session =
					std::make_shared<proxy_session>(
						instantiate_proxy_stream(std::move(socket)),
							connection_id, self);

				m_clients[connection_id] = new_session;

				new_session->start();
			}
			else if (detect[0] == 0x16) // http/socks proxy with ssl crypto protocol.
			{
				XLOG_DBG << "connection id: "
					<< connection_id
					<< ", socks/https protocol";

				// instantiate socks stream with ssl context.
				auto ssl_socks_stream = instantiate_proxy_stream(
					std::move(socket), m_ssl_context);

				// get origin ssl stream type.
				ssl_stream& ssl_socket =
					boost::variant2::get<ssl_stream>(ssl_socks_stream);

				// do async ssl handshake.
				co_await ssl_socket.async_handshake(
					net::ssl::stream_base::server,
					net_awaitable[error]);
				if (error)
				{
					XLOG_DBG << "connection id: "
						<< connection_id
						<< ", ssl protocol handshake error: "
						<< error.message();
					co_return;
				}

				// make socks session shared ptr.
				auto new_session =
					std::make_shared<proxy_session>(
						std::move(ssl_socks_stream), connection_id, self);
				m_clients[connection_id] = new_session;

				new_session->start();
			}								// plain http protocol.
			else if (detect[0] == 0x47 ||	// 'G'
				detect[0] == 0x50 ||		// 'P'
				detect[0] == 0x43)			// 'C'
			{
				if (m_option.disable_http_)
				{
					XLOG_DBG << "connection id: "
						<< connection_id
						<< ", http protocol disabled";
					co_return;
				}

				XLOG_DBG << "connection id: "
					<< connection_id
					<< ", http protocol";

				auto new_session =
					std::make_shared<proxy_session>(
						instantiate_proxy_stream(std::move(socket)),
							connection_id, self);
				m_clients[connection_id] = new_session;

				new_session->start();
			}
			else if (noise && m_option.scramble_)
			{
				// 进入噪声过滤协议, 同时返回一段噪声给客户端.
				XLOG_DBG << "connection id: "
					<< connection_id
					<< ", noise protocol";

				if (!co_await noise_process(std::move(socket), connection_id))
					co_return;
			}
			else
			{
				XLOG_DBG << "connection id: "
					<< connection_id
					<< ", unknown protocol";
			}

			co_return;
		}

		inline net::awaitable<void> start_proxy_listen(tcp_acceptor& a)
		{
			boost::system::error_code error;
			net::socket_base::keep_alive keep_alive_opt(true);
			net::ip::tcp::no_delay no_delay_opt(true);
			net::ip::tcp::no_delay delay_opt(false);

			auto self = shared_from_this();

			while (!m_abort)
			{
				tcp_socket socket(m_executor);

				co_await a.async_accept(socket, net_awaitable[error]);
				if (error)
				{
					XLOG_ERR << "start_proxy_listen"
						", async_accept: " << error.message();
					co_return;
				}

				socket.set_option(keep_alive_opt, error);

				// 在启用 scramble 时, 刻意开启 Nagle's algorithm 以尽量保证数据包
				// 被重组, 尽最大可能避免观察者通过观察 ip 数据包大小的规律来分析 tcp
				// 数据发送调用, 从而增加噪声加扰的强度.
				if (m_option.scramble_)
					socket.set_option(delay_opt, error);
				else
					socket.set_option(no_delay_opt, error);

				static std::atomic_size_t id{ 1 };
				size_t connection_id = id++;

				auto endp = socket.remote_endpoint(error);
				auto client = endp.address().to_string();
				client += ":" + std::to_string(endp.port());

				XLOG_DBG << "connection id: "
					<< connection_id
					<< ", start client incoming: "
					<< client;

				net::co_spawn(m_executor,
					socket_detect(std::move(socket), connection_id),
					net::detached);
			}

			XLOG_WARN << "start_proxy_listen exit ...";
			co_return;
		}

	private:
		net::io_context::executor_type m_executor;
		tcp_acceptor m_acceptor;
		proxy_server_option m_option;
		using proxy_session_weak_ptr =
			std::weak_ptr<proxy_session>;
		std::unordered_map<size_t, proxy_session_weak_ptr> m_clients;
		net::ssl::context m_ssl_context{ net::ssl::context::sslv23 };
		bool m_abort{ false };
	};

}

#endif // INCLUDE__2023_10_18__PROXY_SERVER_HPP
