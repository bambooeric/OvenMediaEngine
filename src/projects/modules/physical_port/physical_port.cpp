//==============================================================================
//
//  OvenMediaEngine
//
//  Created by Hyunjun Jang
//  Copyright (c) 2018 AirenSoft. All rights reserved.
//
//==============================================================================
#include "physical_port.h"

#include <algorithm>

#include "physical_port_private.h"
#include "physical_port_worker.h"

#define PHYSICAL_PORT_WORKER_COUNT 16
#define PHYSICAL_PORT_EPOLL_TIMEOUT_MSEC 500

PhysicalPort::PhysicalPort()
	: _type(ov::SocketType::Unknown),
	  _server_socket(nullptr),
	  _datagram_socket(nullptr),

	  _need_to_stop(true)
{
}

PhysicalPort::~PhysicalPort()
{
	OV_ASSERT2(_observer_list.empty());
}

bool PhysicalPort::Create(ov::SocketType type,
						  const ov::SocketAddress &address,
						  int send_buffer_size,
						  int recv_buffer_size)
{
	OV_ASSERT2((_server_socket == nullptr) && (_datagram_socket == nullptr));

	logtd("Trying to start server...");

	switch (type)
	{
		case ov::SocketType::Srt:
		case ov::SocketType::Tcp: {
			return CreateServerSocket(type, address, send_buffer_size, recv_buffer_size);
		}

		case ov::SocketType::Udp: {
			return CreateDatagramSocket(type, address);
		}

		case ov::SocketType::Unknown:
			break;
	}

	return false;
}

bool PhysicalPort::CreateServerSocket(ov::SocketType type,
									  const ov::SocketAddress &address,
									  int send_buffer_size,
									  int recv_buffer_size)
{
	// Prepare physical port workers
	{
		auto lock_guard = std::lock_guard(_worker_mutex);
		for (int index = 0; index < PHYSICAL_PORT_WORKER_COUNT; index++)
		{
			_worker_list.emplace_back(std::make_shared<PhysicalPortWorker>(GetSharedPtr()));
		}
	}

	{
		auto shared_lock = std::shared_lock(_worker_mutex);
		for (auto &worker : _worker_list)
		{
			worker->Start();
		}
	}

	auto socket = std::make_shared<ov::ServerSocket>();

	if (socket->Prepare(type, address, send_buffer_size, recv_buffer_size, 4096))
	{
		_type = type;
		_server_socket = socket;

		_need_to_stop = false;

		auto proc = [&, socket]() -> void {
			auto client_callback = [&](const std::shared_ptr<ov::ClientSocket> &client, ov::SocketConnectionState state, const std::shared_ptr<ov::Error> &error) -> ov::SocketConnectionState {
				switch (state)
				{
					case ov::SocketConnectionState::Connected: {
						logtd("New client is connected: %s", client->ToString().CStr());

						// Notify observers
						auto func = std::bind(&PhysicalPortObserver::OnConnected, std::placeholders::_1, std::static_pointer_cast<ov::Socket>(client));
						for_each(_observer_list.begin(), _observer_list.end(), func);

						break;
					}

					case ov::SocketConnectionState::Disconnected: {
						logtd("Client is disconnected: %s", client->ToString().CStr());

						// Notify observers
						auto func = bind(&PhysicalPortObserver::OnDisconnected, std::placeholders::_1, std::static_pointer_cast<ov::Socket>(client), PhysicalPortDisconnectReason::Disconnected, nullptr);
						for_each(_observer_list.begin(), _observer_list.end(), func);

						break;
					}

					case ov::SocketConnectionState::Error: {
						logtd("Client is disconnected with error: %s (%s)", client->ToString().CStr(), (error != nullptr) ? error->ToString().CStr() : "N/A");

						// Notify observers
						auto func = bind(&PhysicalPortObserver::OnDisconnected, std::placeholders::_1, std::static_pointer_cast<ov::Socket>(client), PhysicalPortDisconnectReason::Error, error);
						for_each(_observer_list.begin(), _observer_list.end(), func);

						break;
					}
				}

				return state;
			};

			auto data_callback = [&](const std::shared_ptr<ov::ClientSocket> &client, const std::shared_ptr<const ov::Data> &data) -> ov::SocketConnectionState {
				logtd("Received data %d bytes:\n%s", data->GetLength(), data->Dump().CStr());

				auto shared_lock = std::shared_lock(_worker_mutex, std::defer_lock);

				shared_lock.lock();
				auto &worker = _worker_list.at(client->GetSocket().GetSocket() % PHYSICAL_PORT_WORKER_COUNT);
				shared_lock.unlock();

				worker->AddTask(client, data);

				return ov::SocketConnectionState::Connected;
			};

			while ((_need_to_stop == false) && (socket->DispatchEvent(client_callback, data_callback, PHYSICAL_PORT_EPOLL_TIMEOUT_MSEC)))
			{
			}

			socket->Close();

			logtd("Server is stopped");
		};

		_thread = std::thread(proc);

		_address = address;

		return true;
	}

	return false;
}

bool PhysicalPort::CreateDatagramSocket(ov::SocketType type, const ov::SocketAddress &address)
{
	auto socket = std::make_shared<ov::DatagramSocket>();

	if (socket->Prepare(address))
	{
		_type = type;
		_datagram_socket = socket;

		_need_to_stop = false;

		auto proc = [&, socket]() -> void {
			auto data_callback = [&](const std::shared_ptr<ov::DatagramSocket> &socket, const ov::SocketAddress &remote_address, const std::shared_ptr<const ov::Data> &data) -> bool {
				logtd("Received data %d bytes:\n%s", data->GetLength(), data->Dump().CStr());

				// Notify observers
				auto func = std::bind(&PhysicalPortObserver::OnDataReceived, std::placeholders::_1, socket, remote_address, ref(data));
				for_each(_observer_list.begin(), _observer_list.end(), func);

				return true;
			};

			while ((_need_to_stop == false) && (socket->DispatchEvent(data_callback, 500)))
			{
			}

			socket->Close();

			logtd("Server is stopped");
		};

		_thread = std::thread(proc);

		_address = address;

		return true;
	}

	return false;
}

bool PhysicalPort::Close()
{
	_need_to_stop = true;

	{
		auto shared_lock = std::shared_lock(_worker_mutex);
		for (auto &worker : _worker_list)
		{
			worker->Stop();
		}
	}

	{
		auto lock_guard = std::lock_guard(_worker_mutex);
		_worker_list.clear();
	}

	if (_thread.joinable())
	{
		_thread.join();
	}

	_thread = std::thread();

	switch (_type)
	{
		case ov::SocketType::Tcp:
			if (_server_socket != nullptr)
			{
				if ((_server_socket->GetState() == ov::SocketState::Closed) || (_server_socket->Close()))
				{
					_server_socket = nullptr;
					_observer_list.clear();
					return true;
				}
			}
			break;

		case ov::SocketType::Udp:
			if ((_datagram_socket->GetState() == ov::SocketState::Closed) || (_datagram_socket->Close()))
			{
				_datagram_socket = nullptr;
				_observer_list.clear();
				return true;
			}
			break;

		default:
			break;
	}

	return false;
}

ov::SocketState PhysicalPort::GetState()
{
	switch (_type)
	{
		case ov::SocketType::Tcp:
			OV_ASSERT2(_server_socket != nullptr);

			return _server_socket->GetState();

		case ov::SocketType::Udp:
			OV_ASSERT2(_datagram_socket != nullptr);

			return _datagram_socket->GetState();

		default:
			return ov::SocketState::Closed;
	}
}

bool PhysicalPort::AddObserver(PhysicalPortObserver *observer)
{
	_observer_list.push_back(observer);

	return true;
}

bool PhysicalPort::RemoveObserver(PhysicalPortObserver *observer)
{
	auto item = std::find(_observer_list.begin(), _observer_list.end(), observer);

	if (item == _observer_list.end())
	{
		return false;
	}

	_observer_list.erase(item);

	return true;
}

bool PhysicalPort::DisconnectClient(ov::ClientSocket *client_socket)
{
	return _server_socket->DisconnectClient(client_socket, ov::SocketConnectionState::Disconnected);
}
