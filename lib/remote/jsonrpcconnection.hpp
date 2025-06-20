/* Icinga 2 | (c) 2012 Icinga GmbH | GPLv2+ */

#ifndef JSONRPCCONNECTION_H
#define JSONRPCCONNECTION_H

#include "remote/i2-remote.hpp"
#include "remote/endpoint.hpp"
#include "base/atomic.hpp"
#include "base/io-engine.hpp"
#include "base/tlsstream.hpp"
#include "base/wait-group.hpp"
#include "base/timer.hpp"
#include "base/workqueue.hpp"
#include <memory>
#include <vector>
#include <boost/asio/io_context.hpp>
#include <boost/asio/io_context_strand.hpp>
#include <boost/asio/spawn.hpp>

namespace icinga
{

enum ClientRole
{
	ClientInbound,
	ClientOutbound
};

enum ClientType
{
	ClientJsonRpc,
	ClientHttp
};

class MessageOrigin;

/**
 * An API client connection.
 *
 * @ingroup remote
 */
class JsonRpcConnection final : public Object
{
public:
	DECLARE_PTR_TYPEDEFS(JsonRpcConnection);

	JsonRpcConnection(const WaitGroup::Ptr& waitgroup, const String& identity, bool authenticated,
		const Shared<AsioTlsStream>::Ptr& stream, ConnectionRole role);

	void Start();

	double GetTimestamp() const;
	String GetIdentity() const;
	bool IsAuthenticated() const;
	Endpoint::Ptr GetEndpoint() const;
	Shared<AsioTlsStream>::Ptr GetStream() const;
	ConnectionRole GetRole() const;

	void Disconnect();

	void SendMessage(const Dictionary::Ptr& request);
	void SendRawMessage(const String& request);

	static Value HeartbeatAPIHandler(const intrusive_ptr<MessageOrigin>& origin, const Dictionary::Ptr& params);

	static double GetWorkQueueRate();

	static void SendCertificateRequest(const JsonRpcConnection::Ptr& aclient, const intrusive_ptr<MessageOrigin>& origin, const String& path);

private:
	String m_Identity;
	bool m_Authenticated;
	Endpoint::Ptr m_Endpoint;
	Shared<AsioTlsStream>::Ptr m_Stream;
	ConnectionRole m_Role;
	double m_Timestamp;
	double m_Seen;
	boost::asio::io_context::strand m_IoStrand;
	std::vector<String> m_OutgoingMessagesQueue;
	AsioEvent m_OutgoingMessagesQueued;
	AsioEvent m_WriterDone;
	Atomic<bool> m_ShuttingDown;
	WaitGroup::Ptr m_WaitGroup;
	boost::asio::deadline_timer m_CheckLivenessTimer, m_HeartbeatTimer;

	JsonRpcConnection(const WaitGroup::Ptr& waitgroup, const String& identity, bool authenticated,
		const Shared<AsioTlsStream>::Ptr& stream, ConnectionRole role, boost::asio::io_context& io);

	void HandleIncomingMessages(boost::asio::yield_context yc);
	void WriteOutgoingMessages(boost::asio::yield_context yc);
	void HandleAndWriteHeartbeats(boost::asio::yield_context yc);
	void CheckLiveness(boost::asio::yield_context yc);

	bool ProcessMessage();

	void MessageHandler(const Dictionary::Ptr& message);

	void CertificateRequestResponseHandler(const Dictionary::Ptr& message);

	void SendMessageInternal(const Dictionary::Ptr& request);
};

}

#endif /* JSONRPCCONNECTION_H */
