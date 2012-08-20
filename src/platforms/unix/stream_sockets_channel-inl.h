// The Firmament project
// Copyright (c) 2011-2012 Malte Schwarzkopf <malte.schwarzkopf@cl.cam.ac.uk>
//
// UNIX stream sockets communication channel.

#ifndef FIRMAMENT_PLATFORMS_UNIX_STREAM_SOCKETS_CHANNEL_INL_H
#define FIRMAMENT_PLATFORMS_UNIX_STREAM_SOCKETS_CHANNEL_INL_H

#include "platforms/unix/stream_sockets_channel.h"

#include <vector>
#include <string>

#include <boost/asio.hpp>
#include <boost/bind.hpp>
#include <boost/noncopyable.hpp>
#include <boost/thread.hpp>

#include "base/common.h"
#include "misc/messaging_interface.h"
#include "misc/uri_tools.h"
#include "platforms/common.h"
#include "platforms/unix/common.h"
#include "platforms/unix/tcp_connection.h"
#include "platforms/unix/async_tcp_server.h"

namespace firmament {
namespace platform_unix {
namespace streamsockets {

using boost::asio::ip::tcp;
using boost::asio::io_service;
using boost::asio::socket_base;

// ----------------------------
// StreamSocketsChannel
// ----------------------------

template <class T>
StreamSocketsChannel<T>::StreamSocketsChannel(StreamSocketType type)
  : client_io_service_(new io_service),
    channel_ready_(false),
    type_(type) {
  switch (type) {
    case SS_TCP: {
      // Set up two TCP endpoints (?)
      // TODO(malte): investigate if we can use a scoped pointer here
      VLOG(2) << "Setup for TCP endpoints";
      break; }
    case SS_UNIX:
      LOG(FATAL) << "Unimplemented!";
      break;
    default:
      LOG(FATAL) << "Unknown stream socket type!";
  }
}

template <class T>
StreamSocketsChannel<T>::StreamSocketsChannel(
    TCPConnection::connection_ptr connection)
  : client_io_service_(&(connection->socket()->get_io_service())),
    io_service_work_(new io_service::work(*client_io_service_)),
    client_connection_(connection),
    client_socket_(connection->socket()),
    channel_ready_(false),
    type_(SS_TCP) {
  VLOG(2) << "Creating new channel around socket at " << socket;
  if (client_socket_->is_open()) {
    channel_ready_ = true;
  }
}

template <class T>
StreamSocketsChannel<T>::~StreamSocketsChannel() {
  // The user may already have manually cleaned up. If not, we do so now.
  if (Ready()) {
    channel_ready_ = false;
    Close();
  }
  VLOG(2) << "Channel at " << this << " destroyed.";
}

template <class T>
bool StreamSocketsChannel<T>::Establish(const string& endpoint_uri) {
  // If this channel already has an active socket, issue a warning and close it
  // down before establishing a new one.
  if (client_socket_.get() != NULL && client_socket_->is_open()) {
    LOG(WARNING) << "Establishing a new connection on channel " << this
                 << ", despite already having one established. The previous "
                 << "connection will be terminated.";
    client_socket_->shutdown(socket_base::shutdown_both);
    channel_ready_ = false;
  }

  // Parse endpoint URI into hostname and port
  string hostname = URITools::GetHostnameFromURI(endpoint_uri);
  string port = URITools::GetPortFromURI(endpoint_uri);

  // Now make the connection
  VLOG(1) << "Establishing a new channel (TCP connection), "
          << "remote endpoint is " << endpoint_uri;
  tcp::resolver resolver(*client_io_service_);
  tcp::resolver::query query(hostname, port);
  tcp::resolver::iterator endpoint_iterator = resolver.resolve(query);
  tcp::resolver::iterator end;

  client_socket_.reset(new tcp::socket(*client_io_service_));
  boost::system::error_code error = boost::asio::error::host_not_found;
  while (error && endpoint_iterator != end) {
    client_socket_->close();
    client_socket_->connect(*endpoint_iterator++, error);
  }
  if (error) {
    LOG(ERROR) << "Failed to establish a stream socket channel to remote "
               << "endpoint " << endpoint_uri << ". Error: "
               << error.message();
    return false;
  } else {
    io_service_work_.reset(new io_service::work(*client_io_service_));
    VLOG(2) << "Client: we appear to have connected successfully...";
    boost::shared_ptr<boost::thread> thread(new boost::thread(
        boost::bind(&boost::asio::io_service::run, client_io_service_)));
    VLOG(2) << "Created IO service thread " << thread->get_id();
    channel_ready_ = true;
    return true;
  }
}

// Ready check
template <class T>
bool StreamSocketsChannel<T>::Ready() {
  return (channel_ready_ && client_socket_->is_open());
}

// Synchronous send
template <class T>
bool StreamSocketsChannel<T>::SendS(const Envelope<T>& message) {
  VLOG(2) << "Trying to send message of size " << message.size()
          << " on channel " << *this;
  size_t msg_size = message.size();
  vector<char> buf(msg_size);
  CHECK(message.Serialize(&buf[0], message.size()));
  // Send data size
  boost::system::error_code error;
  size_t len;
  len = boost::asio::write(
      *client_socket_, boost::asio::buffer(
          reinterpret_cast<char*>(&msg_size), sizeof(msg_size)),
             boost::asio::transfer_at_least(sizeof(size_t)), error);
  if (error || len != sizeof(size_t)) {
    VLOG(1) << "Error sending size preamble on connection: "
            << error.message();
    return false;
  }
  // Send the data
  len = boost::asio::write(
      *client_socket_, boost::asio::buffer(buf, msg_size),
             boost::asio::transfer_at_least(msg_size), error);
  if (error || len != msg_size) {
    VLOG(1) << "Error sending message on connection: "
            << error.message();
    return false;
  } else {
    VLOG(2) << "Sent " << len << " bytes of protobuf data...";
  }
  return true;
}

// Asynchronous send
// N.B.: error handling is deferred to the callback handler, which takes a
// boost::system:error_code.
template <class T>
bool StreamSocketsChannel<T>::SendA(const Envelope<T>& message,
                                    GenericAsyncSendHandler callback) {
  VLOG(2) << "Trying to asynchronously send message: " << message;
  size_t msg_size = message.size();
  vector<char> buf(msg_size);
  CHECK(message.Serialize(&buf[0], message.size()));
  // Synchronously send data size first
  boost::asio::async_write(
      *client_socket_, boost::asio::buffer(
          reinterpret_cast<char*>(&msg_size), sizeof(msg_size)),
      callback);
  // Send the data
  boost::asio::async_write(
      *client_socket_, boost::asio::buffer(buf, msg_size), callback);
  return true;
}

// Synchronous recieve -- blocks until the next message is received.
template <class T>
bool StreamSocketsChannel<T>::RecvS(Envelope<T>* message) {
  VLOG(2) << "In RecvS, polling for next message";
  if (!Ready()) {
    LOG(WARNING) << "Tried to read from channel " << this
                 << ", which is not ready; read failed.";
    return false;
  }
  size_t len;
  vector<char> size_buf(sizeof(size_t));
  boost::asio::mutable_buffers_1 size_m_buf(
      reinterpret_cast<char*>(&size_buf[0]), sizeof(size_t));
  boost::system::error_code error;
  // Read the incoming protobuf message length
  // N.B.: read() blocks until the buffer has been filled, i.e. an entire size_t
  // has been read.
  len = read(*client_socket_, size_m_buf,
             boost::asio::transfer_at_least(sizeof(size_t)), error);
  if (error || len != sizeof(size_t)) {
    VLOG(1) << "Error reading from connection: " << error.message();
    return false;
  }
  // ... we can get away with a simple CHECK here and assume that we have some
  // incoming data available.
  CHECK_EQ(sizeof(size_t), len);
  size_t msg_size = *reinterpret_cast<size_t*>(&size_buf[0]);
  CHECK_GT(msg_size, 0);
  VLOG(2) << "Size of incoming protobuf is " << msg_size << " bytes.";
  vector<char> buf(msg_size);
  len = read(*client_socket_,
             boost::asio::mutable_buffers_1(&buf[0], msg_size),
             boost::asio::transfer_at_least(msg_size), error);
  VLOG(2) << "Read " << len << " bytes.";

  if (error == boost::asio::error::eof) {
    VLOG(1) << "Received EOF, connection terminating!";
    return false;
  } else if (error) {
    VLOG(1) << "Error reading from connection: "
            << error.message();
    return false;
  } else {
    VLOG(2) << "Read " << len << " bytes of protobuf data...";
  }
  CHECK_GT(len, 0);
  CHECK_EQ(len, msg_size);
  // XXX(malte): data copy here -- optimize away if possible.
  return (message->Parse(&buf[0], len));
}

// Asynchronous receive -- does not block.
template <class T>
bool StreamSocketsChannel<T>::RecvA(Envelope<T>* message,
                                    GenericAsyncRecvHandler callback) {
  VLOG(2) << "In RecvA, waiting for next message";
  if (!Ready()) {
    LOG(WARNING) << "Tried to read from channel " << this
                 << ", which is not ready; read failed.";
    return false;
  }
  size_t len;
  // Obtain the lock on the async receive buffer.
  async_recv_lock_.lock();
  async_recv_message_ptr_ = message;
  async_recv_callback_ = callback;
  async_recv_buffer_vec_.reset(new vector<char>(sizeof(size_t)));
  async_recv_buffer_.reset(new boost::asio::mutable_buffers_1(
      reinterpret_cast<char*>(&(*async_recv_buffer_vec_)[0]), sizeof(size_t)));
  // Asynchronously read the incoming protobuf message length and invoke the
  // second stage of the receive call once we have it.
  async_read(*client_socket_, *async_recv_buffer_,
             boost::asio::transfer_at_least(sizeof(size_t)),
             boost::bind(&StreamSocketsChannel<T>::RecvASecondStage, this,
                         boost::asio::placeholders::error,
                         boost::asio::placeholders::bytes_transferred));
  // First stage of RecvA always succeeds.
  return true;
}

// Second stage of asynchronous receive, which calls async_recv again in order
// to get the actual message data.
// Called with the async_recv_lock_ mutex held. It is either released before
// returning (error path) or maintained for RecvAThirdStage to release.
template <class T>
void StreamSocketsChannel<T>::RecvASecondStage(
    const boost::system::error_code& error,
    const size_t bytes_read) {
  if (error || bytes_read != sizeof(size_t)) {
    VLOG(1) << "Error reading from connection: " << error.message();
    async_recv_lock_.unlock();
    return;
  }
  // ... we can get away with a simple CHECK here and assume that we have some
  // incoming data available.
  CHECK_EQ(sizeof(size_t), bytes_read);
  size_t msg_size = *reinterpret_cast<size_t*>(&(*async_recv_buffer_vec_)[0]);
  CHECK_GT(msg_size, 0);
  VLOG(2) << "Size of incoming protobuf is " << msg_size << " bytes.";
  // We still hold the async_recv_lock_ mutex here.
  async_recv_buffer_vec_.reset(new vector<char>(msg_size));
  async_recv_buffer_.reset(new boost::asio::mutable_buffers_1(
      reinterpret_cast<char*>(&(*async_recv_buffer_vec_)[0]), msg_size));
  async_read(*client_socket_, *async_recv_buffer_,
             boost::asio::transfer_at_least(msg_size),
             boost::bind(&StreamSocketsChannel<T>::RecvAThirdStage, this,
                         boost::asio::placeholders::error,
                         boost::asio::placeholders::bytes_transferred));
  // Second stage done.
}

// Third stage of asynchronous receive, which finalizes the message reception by
// parsing the received data.
// Called with the async_recv_lock_ mutex held, but releases it before
// returning.
template <class T>
void StreamSocketsChannel<T>::RecvAThirdStage(
    const boost::system::error_code& error,
    const size_t bytes_read) {
  VLOG(2) << "Read " << bytes_read << " bytes.";
  if (error == boost::asio::error::eof) {
    VLOG(1) << "Received EOF, connection terminating!";
    async_recv_lock_.unlock();
    return;
  } else if (error) {
    VLOG(1) << "Error reading from connection: "
            << error.message();
    async_recv_lock_.unlock();
    return;
  } else {
    VLOG(2) << "Read " << bytes_read << " bytes of protobuf data...";
  }
  CHECK_GT(bytes_read, 0);
  //CHECK_EQ(bytes_read, msg_size);
  VLOG(2) << "About to parse message";
  async_recv_message_ptr_->Parse(&(*async_recv_buffer_vec_)[0],
                                 bytes_read);
  // Drop the lock
  VLOG(2) << "Unlocking mutex";
  async_recv_lock_.unlock();
  // Invoke the original callback
  // XXX(malte): potential race condition -- if someone else overwrites the
  // callback before we invoke it (but after we atomically unlock the mutex), we
  // lose. Put in a local mutex?
  VLOG(2) << "About to invoke final async recv callback!";
  async_recv_callback_(error, bytes_read);
}

template <class T>
void StreamSocketsChannel<T>::Close() {
  // end the connection
  VLOG(2) << "Shutting down channel " << *this << "'s socket...";
  client_socket_->shutdown(socket_base::shutdown_both);
  channel_ready_ = false;
}

template <class T>
ostream& StreamSocketsChannel<T>::ToString(ostream* stream) const {
  return *stream << "(StreamSocket,type=" << type_ << ",at=" << this << ")";
}


}  // namespace streamsockets
}  // namespace platform_unix
}  // namespace firmament

#endif  // FIRMAMENT_PLATFORMS_UNIX_MESSAGING_STREAMSOCKETS_INL_H
