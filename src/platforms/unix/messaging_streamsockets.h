// The Firmament project
// Copyright (c) 2011-2012 Malte Schwarzkopf <malte.schwarzkopf@cl.cam.ac.uk>
//
// UNIX stream sockets-based messaging adapter.

#ifndef FIRMAMENT_PLATFORMS_UNIX_MESSAGING_STREAMSOCKETS_H
#define FIRMAMENT_PLATFORMS_UNIX_MESSAGING_STREAMSOCKETS_H

#include <string>

#include <boost/asio.hpp>
#include <boost/noncopyable.hpp>
#include <boost/enable_shared_from_this.hpp>

#include "base/common.h"
#include "misc/messaging_interface.h"
#include "platforms/common.h"
#include "platforms/unix/common.h"
#include "platforms/unix/tcp_connection.h"
#include "platforms/unix/async_tcp_server.h"
#include "platforms/unix/stream_sockets_channel-inl.h"

namespace firmament {
namespace platform_unix {
namespace streamsockets {

class TCPConnection;  // forward declaration
class AsyncTCPServer;  // forward declaration

// Messaging adapter.
class StreamSocketsMessaging :
  public firmament::MessagingInterface,
  public boost::enable_shared_from_this<StreamSocketsMessaging>,
  private boost::noncopyable {
 public:
  virtual ~StreamSocketsMessaging();
  Message* AwaitNextMessage();
  void AddChannelForConnection(TCPConnection::connection_ptr connection) {
    shared_ptr<StreamSocketsChannel<Message> > channel(
            new StreamSocketsChannel<Message>(connection->socket()));
    VLOG(1) << "Adding back-channel for connection at " << connection
            << ", channel is " << *channel;
    active_channels_.push_back(channel);
  }
  template <class T>
  void CloseChannel(MessagingChannelInterface<T>* chan) {
    VLOG(1) << "Shutting down channel " << chan;
    chan->Close();
  }
  template <class T>
  void EstablishChannel(const string& endpoint_uri,
                        MessagingChannelInterface<T>* chan) {
    VLOG(1) << "Establishing channel from endpoint " << endpoint_uri
            << ", chan: " << *chan << "!";
    chan->Establish(endpoint_uri);
  }
  shared_ptr<StreamSocketsChannel<Message> > GetChannelForConnection(
      uint64_t connection_id) {
    CHECK_LT(connection_id, active_channels_.size());
    return active_channels_[connection_id];
  }
  void Listen(const string& endpoint_uri);
  bool ListenReady();
  void SendOnConnection(uint64_t connection_id);
  void StopListen();

 private:
  AsyncTCPServer* tcp_server_;
  vector<shared_ptr<StreamSocketsChannel<Message> > > active_channels_;
};

}  // namespace streamsockets
}  // namespace platform_unix
}  // namespace firmament

#endif  // FIRMAMENT_PLATFORMS_UNIX_MESSAGING_STREAMSOCKETS_H
