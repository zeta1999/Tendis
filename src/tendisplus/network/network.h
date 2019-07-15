#ifndef SRC_TENDISPLUS_NETWORK_NETWORK_H_
#define SRC_TENDISPLUS_NETWORK_NETWORK_H_

#include <utility>
#include <atomic>
#include <memory>
#include <string>
#include <vector>
#include "asio.hpp"
#include "tendisplus/network/session_ctx.h"
#include "tendisplus/network/blocking_tcp_client.h"
#include "tendisplus/server/session.h"
#include "tendisplus/server/server_entry.h"
#include "tendisplus/utils/status.h"
#include "tendisplus/utils/atomic_utility.h"
#include "gtest/gtest.h"
#include "unistd.h"

namespace tendisplus {

class ServerEntry;

enum class RedisReqMode: std::uint8_t {
    REDIS_REQ_UNKNOWN = 0,
    REDIS_REQ_INLINE = 1,
    REDIS_REQ_MULTIBULK = 2,
};

class NetworkMatrix {
public:
    Atom<uint64_t> stickyPackets{0};
    Atom<uint64_t> connCreated{0};
    Atom<uint64_t> connClients{0};
    Atom<uint64_t> connReleased{0};
    Atom<uint64_t> invalidPackets{0};
    NetworkMatrix operator -(const NetworkMatrix& right);
    std::string toString() const;
};

class RequestMatrix {
public:
    Atom<uint64_t> processed{0};
    Atom<uint64_t> processCost{0};
    Atom<uint64_t> sendPacketCost{0};
    RequestMatrix operator -(const RequestMatrix& right);
    std::string toString() const;
};

class NetworkAsio {
 public:
    NetworkAsio(std::shared_ptr<ServerEntry> server,
            std::shared_ptr<NetworkMatrix> netMatrix,
            std::shared_ptr<RequestMatrix> reqMatrix);
    NetworkAsio(const NetworkAsio&) = delete;
    NetworkAsio(NetworkAsio&&) = delete;

    // blocking client related apis
    std::unique_ptr<BlockingTcpClient> createBlockingClient(size_t readBuf);
    std::unique_ptr<BlockingTcpClient> createBlockingClient(
        asio::ip::tcp::socket, size_t readBuf);
    Expected<uint64_t> client2Session(std::shared_ptr<BlockingTcpClient>);

    Status prepare(const std::string& ip, const uint16_t port);
    Status run();
    void stop();
    std::string getIp() { return _ip;}
    uint16_t getPort() { return _port;}

 private:
    // we envolve a single-thread accept, mutex is not needed.
    void doAccept();
    std::atomic<uint64_t> _connCreated;
    std::shared_ptr<ServerEntry> _server;
    std::unique_ptr<asio::io_context> _acceptCtx;
    std::shared_ptr<asio::io_context> _rwCtx;
    std::unique_ptr<asio::ip::tcp::acceptor> _acceptor;
    std::unique_ptr<std::thread> _acceptThd;
    std::vector<std::thread> _rwThreads;
    std::atomic<bool> _isRunning;
    std::shared_ptr<NetworkMatrix> _netMatrix;
    std::shared_ptr<RequestMatrix> _reqMatrix;
    std::string _ip;
    uint16_t _port;
};

struct SendBuffer {
    std::vector<char> buffer;
    bool closeAfterThis;
};

// represent a ingress tcp-connection
class NetSession: public Session {
 public:
    NetSession(std::shared_ptr<ServerEntry> server,
               asio::ip::tcp::socket sock,
               uint64_t connid,
               bool initSock,
               std::shared_ptr<NetworkMatrix> netMatrix,
               std::shared_ptr<RequestMatrix> reqMatrix);
    NetSession(const NetSession&) = delete;
    NetSession(NetSession&&) = delete;
    virtual ~NetSession() = default;
    std::string getRemoteRepr() const;
    std::string getLocalRepr() const;
    asio::ip::tcp::socket borrowConn();
    void setResponse(const std::string& s) final;
    void setCloseAfterRsp();
    void start() final;
    Status cancel() final;
    std::string getRemote() const final;
    int getFd() final;
    const std::vector<std::string>& getArgs() const;
    void setArgs(const std::vector<std::string>&);

    // network is ok, but client's msg is not ok, reply and close
    void setRspAndClose(const std::string&);

    enum class State {
        Created,
        DrainReqNet,
        DrainReqBuf,
        Process,
    };

 protected:
    // schedule related functions
    virtual void schedule();
    virtual void stepState();
    virtual void setState(State s);


 private:
    FRIEND_TEST(NetSession, drainReqInvalid);
    FRIEND_TEST(NetSession, Completed);
    FRIEND_TEST(Command, common);

    // read data from socket
    void drainReqNet();
    void drainReqBuf();
    void drainReqCallback(const std::error_code& ec, size_t actualLen);
    void processMultibulkBuffer();
    void processInlineBuffer();

    // send data to tcpbuff
    void drainRsp(std::shared_ptr<SendBuffer>);
    void drainRspCallback(const std::error_code& ec, size_t actualLen, std::shared_ptr<SendBuffer> buf);

    // close session, and the socket(by raii)
    void endSession();

    // handle msg parsed from drainReqCallback
    void processReq();

    // utils to shift parsed partial params from _queryBuf
    void shiftQueryBuf(ssize_t start, ssize_t end);

    // cleanup state for next request
    void resetMultiBulkCtx();

    uint64_t _connId;
    bool _closeAfterRsp;
    std::atomic<State> _state;
    asio::ip::tcp::socket _sock;
    std::vector<char> _queryBuf;
    ssize_t _queryBufPos;

    // contexts for RedisReqMode::REDIS_REQ_MULTIBULK
    RedisReqMode _reqType;
    int64_t _multibulklen;
    int64_t _bulkLen;

    // _mutex protects _isSendRunning, _isEnded, _sendBuffer
    // other variables will never be visited in send-threads.
    std::mutex _mutex;
    bool _isSendRunning;
    bool _isEnded;
    bool _first;
    std::list<std::shared_ptr<SendBuffer>> _sendBuffer;

    std::shared_ptr<NetworkMatrix> _netMatrix;
    std::shared_ptr<RequestMatrix> _reqMatrix;
};

}  // namespace tendisplus
#endif  // SRC_TENDISPLUS_NETWORK_NETWORK_H_
