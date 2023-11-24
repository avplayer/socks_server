﻿//
// Copyright (C) 2022 Jack.
//
// Author: jack
// Email:  jack.wgm at gmail dot com
//

#ifndef INCLUDE__2023_11_25__PROXY_SOCKET_HPP
#define INCLUDE__2023_11_25__PROXY_SOCKET_HPP

#include <cstdint>
#include <type_traits>

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/asio/buffer.hpp>

#include "boost/asio/write.hpp"
#include "proxy/base_stream.hpp"
#include "proxy/scramble.hpp"

namespace util {

	using tcp = net::ip::tcp;               // from <boost/asio/ip/tcp.hpp>
	using udp = net::ip::udp;               // from <boost/asio/ip/udp.hpp>

	template <typename Stream>
	class proxy_socket;

	using tcp_socket = proxy_socket<tcp::socket>;
	using tcp_acceptor = tcp::acceptor;

	using ssl_stream = net::ssl::stream<tcp_socket>;
	using proxy_stream_type = base_stream<tcp_socket, ssl_stream>;

	//////////////////////////////////////////////////////////////////////////
#if 1

	template <typename Stream>
	class proxy_socket final
	{
		proxy_socket(const proxy_socket&) = delete;
		proxy_socket& operator=(proxy_socket const&) = delete;

	public:

		using next_layer_type = typename std::remove_reference<Stream>::type;
		using lowest_layer_type = typename next_layer_type::lowest_layer_type;
		using executor_type = typename next_layer_type::executor_type;
        using native_handle_type = typename next_layer_type::native_handle_type;
		using endpoint_type = typename next_layer_type::endpoint_type;
		using wait_type = typename next_layer_type::wait_type;
		using protocol_type = typename next_layer_type::protocol_type;

	public:

		explicit proxy_socket(Stream&& s)
			: next_layer_(std::move(s))
		{}

		explicit proxy_socket(executor_type exec)
			: next_layer_(exec)
		{}

		explicit proxy_socket(net::io_context& ioc)
			: next_layer_(ioc)
		{}

		~proxy_socket() = default;

		proxy_socket& operator=(proxy_socket&& other)
		{
			next_layer_ = std::move(other.next_layer_);

			scramble_ = std::move(other.scramble_);
			unscramble_ = std::move(other.unscramble_);
		}

		proxy_socket(proxy_socket&& other)
			: next_layer_(std::move(other.next_layer_))
			, scramble_(std::move(other.scramble_))
			, unscramble_(std::move(other.unscramble_))
		{}

		void set_scramble_key(const std::vector<uint8_t>& key)
		{
			scramble_.set_key(key);
		}

		void set_unscramble_key(const std::vector<uint8_t>& key)
		{
			unscramble_.set_key(key);
		}

		proxy::scramble_stream& scramble()
		{
			return scramble_;
		}

		proxy::scramble_stream& unscramble()
		{
			return unscramble_;
		}

		executor_type get_executor()
		{
			return next_layer_.get_executor();
		}

		Stream& next_layer()
		{
			return next_layer_;
		}

		const Stream& next_layer() const
		{
			return next_layer_;
		}

		lowest_layer_type& lowest_layer()
		{
			return next_layer_.lowest_layer();
		}

		const lowest_layer_type& lowest_layer() const
		{
			return next_layer_.lowest_layer();
		}

		native_handle_type native_handle()
		{
			return next_layer_.lowest_layer().native_handle();
		}

		tcp::endpoint remote_endpoint()
		{
			return next_layer_.lowest_layer().remote_endpoint();
		}

		tcp::endpoint remote_endpoint(boost::system::error_code& ec)
		{
			return next_layer_.lowest_layer().remote_endpoint(ec);
		}

		void shutdown(net::socket_base::shutdown_type what)
		{
			next_layer_.lowest_layer().shutdown(what);
		}

		void shutdown(net::socket_base::shutdown_type what,
			boost::system::error_code& ec)
		{
			next_layer_.lowest_layer().shutdown(what, ec);
		}

		void close()
		{
			next_layer_.close();
		}

		void close(boost::system::error_code& ec)
		{
			next_layer_.close(ec);
		}

		bool is_open() const
		{
			return next_layer_.is_open();
		}

		void open(const protocol_type& protocol = protocol_type())
		{
			next_layer_.open(protocol);
		}

		void open(const protocol_type& protocol, boost::system::error_code& ec)
		{
			next_layer_.open(protocol, ec);
		}

		void bind(const tcp::endpoint& endpoint)
		{
			next_layer_.bind(endpoint);
		}

		void bind(const endpoint_type& endpoint, boost::system::error_code& ec)
		{
			next_layer_.bind(endpoint, ec);
		}

		template <typename SettableSocketOption>
		void set_option(const SettableSocketOption& option)
		{
			next_layer_.set_option(option);
		}

		template <typename SettableSocketOption>
		void set_option(const SettableSocketOption& option,
			boost::system::error_code& ec)
		{
			next_layer_.set_option(option, ec);
		}

		template <typename ConnectHandler>
		BOOST_ASIO_INITFN_RESULT_TYPE(ConnectHandler,
			void(boost::system::error_code))
			async_connect(const tcp::endpoint& endpoint, ConnectHandler&& handler)
		{
			return next_layer_.async_connect(endpoint,
				std::forward<ConnectHandler>(handler));
		}

		template <typename MutableBufferSequence, typename ReadHandler>
		BOOST_ASIO_INITFN_RESULT_TYPE(ReadHandler,
			void(boost::system::error_code, std::size_t))
			async_read_some(const MutableBufferSequence& buffers, ReadHandler&& handler)
			{
				return net::async_initiate<ReadHandler,
						void (boost::system::error_code, std::size_t)>(
							[this] (auto handler, const auto& buffers) mutable
							{
								next_layer_.async_read_some(buffers,
									[this, buffers, handler = std::move(handler)]
										(auto ec, auto bytes) mutable
										{
											if (unscramble_.is_valid()) [[likely]]
											{
												net::mutable_buffer buffer =
													net::detail::buffer_sequence_adapter<
														net::mutable_buffer,
														decltype(buffers)
													>::first(buffers);

												unscramble_.scramble(
													(uint8_t*)buffer.data(), bytes);
											}

											handler(ec, bytes);
										});
							}, handler, buffers);
			}

		template <typename ConstBufferSequence, typename WriteHandler>
		BOOST_ASIO_INITFN_RESULT_TYPE(WriteHandler,
			void(boost::system::error_code, std::size_t))
			async_write_some(const ConstBufferSequence& buffers, WriteHandler&& handler)
			{
				if (scramble_.is_valid()) [[likely]]
				{
					net::const_buffer buffer =
						net::detail::buffer_sequence_adapter<
							net::const_buffer,
							decltype(buffers)>::first(buffers);

					scramble_.scramble(
						(uint8_t*)buffer.data(), buffer.size());
				}

				return net::async_initiate<WriteHandler,
						void (boost::system::error_code, std::size_t)>(
							[this] (auto handler, const auto& buffers) mutable
							{
								net::async_write(
									next_layer_, buffers,
									[this, buffers, handler = std::move(handler)]
										(auto ec, auto bytes) mutable
										{
											handler(ec, bytes);
										});
							}, handler, buffers);
			}

		template <typename WaitHandler>
		BOOST_ASIO_INITFN_RESULT_TYPE(WaitHandler,
			void(boost::system::error_code))
			async_wait(net::socket_base::wait_type w, WaitHandler&& handler)
		{
			return next_layer_.async_wait(w, std::forward<WaitHandler>(handler));
		}

	private:
		Stream next_layer_;

		proxy::scramble_stream scramble_;
		proxy::scramble_stream unscramble_;
	};


#endif

	//////////////////////////////////////////////////////////////////////////

	inline proxy_stream_type init_proxy_stream(
		proxy_stream_type& s)
	{
		return proxy_stream_type(tcp_socket(s.get_executor()));
	}

	inline proxy_stream_type init_proxy_stream(
		net::any_io_executor executor)
	{
		return proxy_stream_type(tcp_socket(executor));
	}

	inline proxy_stream_type init_proxy_stream(
		net::io_context& ioc)
	{
		return proxy_stream_type(tcp_socket(ioc));
	}

	template <typename Stream>
	inline proxy_stream_type init_proxy_stream(
		Stream&& s)
	{
		using StreamType = std::decay_t<Stream>;

		if constexpr (std::same_as<StreamType, tcp_socket>)
			return proxy_stream_type(std::move(s));
		else if constexpr (std::same_as<StreamType, ssl_stream>)
			return proxy_stream_type(std::move(s));
		else
			return proxy_stream_type(tcp_socket(std::move(s)));
	}

	template <typename Stream>
	inline proxy_stream_type init_proxy_stream(
		Stream&& s, net::ssl::context& sslctx)
	{
		if constexpr (std::same_as<std::decay_t<Stream>, tcp::socket>)
		{
			return proxy_stream_type(ssl_stream(
				std::forward<tcp::socket>(s), sslctx));
		}
		else {
			return proxy_stream_type(ssl_stream(
				tcp_socket(std::move(s)), sslctx));
		}
	}

}

#endif // INCLUDE__2023_11_25__PROXY_SOCKET_HPP
