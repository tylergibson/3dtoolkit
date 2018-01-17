#pragma once

#include "pch.h"

#include <map>
#include <string>
#include <atomic>

#include "peer_connection_client.h"
#include "peer_conductor.hpp"

#include "webrtc/base/sigslot.h"

using namespace StreamingToolkit;

using namespace std;
using namespace rtc;
using namespace webrtc;
using namespace sigslot;

class MultiPeerConductor : public PeerConnectionClientObserver,
	public MessageHandler,
	public Runnable,
	public has_slots<>
{
public:
	MultiPeerConductor(shared_ptr<WebRTCConfig> config,
		ID3D11Device* d3dDevice,
		bool enableSoftware = false) :
		m_webrtcConfig(config),
		m_d3dDevice(d3dDevice),
		m_enableSoftware(enableSoftware)
	{
		m_signallingClient.RegisterObserver(this);
		m_peerFactory = webrtc::CreatePeerConnectionFactory();
		m_processThread = rtc::Thread::Create();
		m_processThread->Start(this);
	}

	~MultiPeerConductor()
	{
		m_processThread->Quit();
	}

	// Connect the signalling implementation to the signalling server
	void ConnectSignallingAsync(const string& clientName)
	{
		m_signallingClient.Connect(m_webrtcConfig->server, m_webrtcConfig->port, clientName);
	}

	// each peer can emit a signal that will in turn call this method
	void OnIceConnectionChange(int peer_id, PeerConnectionInterface::IceConnectionState new_state)
	{
		// peer disconnected
		if (new_state == PeerConnectionInterface::IceConnectionState::kIceConnectionDisconnected)
		{
			m_connectedPeers.erase(peer_id);
		}
	}

	virtual void OnSignedIn() override
	{
		m_shouldProcessQueue.store(true);
	}

	virtual void OnDisconnected() override
	{
		m_shouldProcessQueue.store(false);
	}

	virtual void OnPeerConnected(int id, const string& name) override
	{
		m_connectedPeers[id] = new RefCountedObject<DirectXPeerConductor>(id,
			name,
			m_webrtcConfig,
			m_peerFactory,
			[&, id](const string& message)
		{
			m_messageQueue.push(MessageEntry(id, message));
			rtc::Thread::Current()->PostDelayed(RTC_FROM_HERE, 500, this, 0);
		},
			m_d3dDevice,
			m_enableSoftware);

		m_connectedPeers[id]->SignalIceConnectionChange.connect(this, &MultiPeerConductor::OnIceConnectionChange);
	}

	virtual void OnPeerDisconnected(int peer_id) override
	{
		m_connectedPeers.erase(peer_id);
	}

	virtual void OnMessageFromPeer(int peer_id, const string& message) override
	{
		m_connectedPeers[peer_id]->HandlePeerMessage(message);
	}

	virtual void OnMessageSent(int err) override
	{
	}

	virtual void OnServerConnectionFailure() override {}

	virtual void OnMessage(Message* msg) override
	{
		if (!m_shouldProcessQueue.load() ||
			m_messageQueue.size() == 0)
		{
			return;
		}

		auto peerMessage = m_messageQueue.front();

		if (m_signallingClient.SendToPeer(peerMessage.peer, peerMessage.message))
		{
			m_messageQueue.pop();
		}

		if (m_messageQueue.size() > 0)
		{
			rtc::Thread::Current()->PostDelayed(RTC_FROM_HERE, 500, this, 0);
		}
	}

	virtual void Run(Thread* thread) override
	{
		while (!thread->IsQuitting())
		{
			thread->ProcessMessages(500);
		}
	}

	const map<int, scoped_refptr<DirectXPeerConductor>>& Peers() const
	{
		return m_connectedPeers;
	}

private:
	PeerConnectionClient m_signallingClient;
	shared_ptr<WebRTCConfig> m_webrtcConfig;
	ID3D11Device* m_d3dDevice;
	bool m_enableSoftware;
	scoped_refptr<PeerConnectionFactoryInterface> m_peerFactory;
	map<int, scoped_refptr<DirectXPeerConductor>> m_connectedPeers;
	
	struct MessageEntry
	{
		int peer;
		string message;
		MessageEntry(int p, const string& s) : peer(p), message(s) {}
	};

	queue<MessageEntry> m_messageQueue;
	atomic_bool m_shouldProcessQueue;
	unique_ptr<Thread> m_processThread;
};