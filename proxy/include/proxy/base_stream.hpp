﻿//
// Copyright (C) 2022 Jack.
//
// Author: jack
// Email:  jack.wgm at gmail dot com
//

#ifndef INCLUDE__2023_10_18__BASE_STREAM_HPP
#define INCLUDE__2023_10_18__BASE_STREAM_HPP


#include <type_traits>

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ip/udp.hpp>
#include <boost/variant2.hpp>
#include <boost/system/error_code.hpp>

#include <boost/asio/socket_base.hpp>
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/ssl.hpp>


namespace util {
	namespace net = boost::asio;

	using tcp = net::ip::tcp;               // from <boost/asio/ip/tcp.hpp>

	//////////////////////////////////////////////////////////////////////////

	template<typename... T>
	class base_stream : public boost::variant2::variant<T...>
	{
		base_stream(const base_stream&) = delete;
		base_stream& operator=(base_stream const&) = delete;

	public:
		template <typename S>
		explicit base_stream(S device)
			: boost::variant2::variant<T...>(std::move(device))
		{
			static_assert(std::is_move_constructible<S>::value
				, "must be move constructible");
		}
		~base_stream() = default;

		base_stream& operator=(base_stream&&) = default;
		base_stream(base_stream&&) = default;

		using executor_type = net::any_io_executor;
		using lowest_layer_type = tcp::socket::lowest_layer_type;

		executor_type get_executor()
		{
			return boost::variant2::visit([&](auto& t) mutable
				{ return t.get_executor(); }, *this);
		}

		lowest_layer_type& lowest_layer()
		{
			return boost::variant2::visit([&](auto& t) mutable -> lowest_layer_type&
				{ return t.lowest_layer(); }, *this);
		}

		template <typename MutableBufferSequence, typename ReadHandler>
		BOOST_ASIO_INITFN_AUTO_RESULT_TYPE(ReadHandler,
			void(boost::system::error_code, std::size_t))
			async_read_some(const MutableBufferSequence& buffers,
				ReadHandler&& handler)
		{
			return boost::variant2::visit([&, handler = std::move(handler)](auto& t) mutable
				{ return t.async_read_some(buffers,
					std::forward<ReadHandler>(handler)); }, *this);
		}

		template <typename ConstBufferSequence, typename WriteHandler>
		BOOST_ASIO_INITFN_AUTO_RESULT_TYPE(WriteHandler,
			void(boost::system::error_code, std::size_t))
			async_write_some(const ConstBufferSequence& buffers,
				WriteHandler&& handler)
		{
			return boost::variant2::visit([&](auto& t) mutable
				{ return t.async_write_some(buffers,
					std::forward<WriteHandler>(handler)); }, *this);
		}

		tcp::endpoint remote_endpoint()
		{
			return boost::variant2::visit([&](auto& t) mutable
				{
					return t.lowest_layer().remote_endpoint();
				}, *this);
		}

		void shutdown(net::socket_base::shutdown_type what,
			boost::system::error_code& ec)
		{
			boost::variant2::visit([&](auto& t) mutable
				{
					t.lowest_layer().shutdown(what, ec);
				}, *this);
		}

		bool is_open() const
		{
			return boost::variant2::visit([&](auto& t)
				{
					return t.lowest_layer().is_open();
				}, *this);
		}

		void close(boost::system::error_code& ec)
		{
			boost::variant2::visit([&](auto& t) mutable
				{
					t.lowest_layer().close(ec);
				}, *this);
		}
	};
}

#endif // INCLUDE__2023_10_18__BASE_STREAM_HPP
