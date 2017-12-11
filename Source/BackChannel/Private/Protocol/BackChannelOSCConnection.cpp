// Copyright 2017 Andrew Grant
// Unless explicitly stated otherwise all files in this repository 
// are licensed under BSD License 2.0. All Rights Reserved.

#include "BackChannel/Private/BackChannelCommon.h"
#include "BackChannel/Transport/IBackChannelTransport.h"
#include "BackChannel/Protocol/OSC/BackChannelOSCConnection.h"
#include "BackChannel/Protocol/OSC/BackChannelOSC.h"

FBackChannelOSCConnection::FBackChannelOSCConnection(TSharedRef<IBackChannelConnection> InConnection)
	: Connection(InConnection)
{
	LastActivityTime = 0;
	LastPingTime = 0;
}

FBackChannelOSCConnection::~FBackChannelOSCConnection()
{
	UE_LOG(LogBackChannel, Verbose, TEXT("Destroying OSC Connection"));
	if (IsRunning)
	{
		Stop();
	}
}

bool FBackChannelOSCConnection::Start()
{
	check(IsRunning == false);

	ExitRequested = false;

	FRunnableThread* Thread = FRunnableThread::Create(this, TEXT("OSCHostConnection"));

	if (Thread)
	{
		IsRunning = true;
	}

	UE_LOG(LogBackChannel, Verbose, TEXT("Started OSC Connection"));

	return Thread != nullptr;
}

bool FBackChannelOSCConnection::Init()
{
	return true;
}

uint32 FBackChannelOSCConnection::Run()
{
	const int32 kDefaultBufferSize = 4096;

	TArray<uint8> Buffer;
	Buffer.AddUninitialized(kDefaultBufferSize);

	// OSC connections expect a size followed by payload for TCP connections
	int32 ExpectedDataSize = 4;
	int32 ReceivedDataSize = 0;

	const int kPingTime = 5;
	const int kTimeout = 10;

	LastActivityTime = FPlatformTime::Seconds();
	LastPingTime = FPlatformTime::Seconds();

	UE_LOG(LogBackChannel, Verbose, TEXT("OSC Connection is Running."));

	while (ExitRequested == false)
	{		
		int32 Received = Connection->ReceiveData(Buffer.GetData() + ReceivedDataSize, ExpectedDataSize-ReceivedDataSize);

		if (Received > 0)
		{
			LastActivityTime = FPlatformTime::Seconds();

			ReceivedDataSize += Received;

			if (ReceivedDataSize == ExpectedDataSize)
			{
				if (ExpectedDataSize == 4)
				{
					int32 Size(0);
					FMemory::Memcpy(&Size, Buffer.GetData(), sizeof(int32));

					if (Size > Buffer.Num())
					{
						Buffer.AddUninitialized(Size - Buffer.Num());
					}

					ExpectedDataSize = Size;
				}
				else
				{
					// read packet
					TSharedPtr<FBackChannelOSCPacket> Packet = FBackChannelOSCPacket::CreateFromBuffer(Buffer.GetData(), ExpectedDataSize);

					if (Packet.IsValid())
					{
						FScopeLock Lock(&ReceiveMutex);
						ReceivedPackets.Add(Packet);
					}

					ExpectedDataSize = 4;
				}

				ReceivedDataSize = 0;
			}
		}
		else
		{
			const float TimeNow = FPlatformTime::Seconds();
			if (TimeNow - LastPingTime >= kPingTime)
			{
				FBackChannelOSCMessage Msg(TEXT("/ping"));
				SendPacket(Msg);
				LastPingTime = TimeNow;
			}

			const float TimeSinceActivity = TimeNow - LastActivityTime;
			if (TimeSinceActivity >= kTimeout)
			{
				UE_LOG(LogBackChannel, Error, TEXT("Connection timed out after %.02 seconds"), TimeSinceActivity);
				ExitRequested = true;
			}
		}

		// switch to blocking?
		FPlatformProcess::SleepNoStats(0.05);
	}

	UE_LOG(LogBackChannel, Verbose, TEXT("OSC Connection is exiting."));
	IsRunning = false;
	return 0;
}

void FBackChannelOSCConnection::Stop()
{
	if (IsRunning)
	{
		UE_LOG(LogBackChannel, Verbose, TEXT("Requesting OSC Connection to stop.."));

		ExitRequested = true;

		while (IsRunning)
		{
			FPlatformProcess::SleepNoStats(0.01);
		}
	}

	UE_LOG(LogBackChannel, Verbose, TEXT("OSC Connection is stopped"));
	Connection = nullptr;
}

bool FBackChannelOSCConnection::IsConnected() const
{
	return IsRunning;
}

bool FBackChannelOSCConnection::SendPacket(FBackChannelOSCPacket& Packet)
{
	TArray<uint8> Data = Packet.WriteToBuffer();
	return SendPacketData(Data.GetData(), Data.Num());
}

bool FBackChannelOSCConnection::SendPacketData(const void* Data, const int32 DataLen)
{
	FScopeLock Lock(&SendMutex);

	if (!IsConnected())
	{
		return false;
	}

	// write size
	int32 Sent = 0;

	// TODO - differentiate between TCP / UDP 
	if (DataLen > 0)
	{
		// OSC over TCP requires a size followed by the packet (TODO - combine these?)
		Connection->SendData(&DataLen, sizeof(DataLen));
		Sent = Connection->SendData(Data, DataLen);

		if (Sent > 0)
		{
			LastActivityTime = FPlatformTime::Seconds();
			UE_LOG(LogBackChannel, Verbose, TEXT("Sent %d bytes of data"), DataLen);
			
		}
		else
		{
			UE_LOG(LogBackChannel, Error, TEXT("Failed to send %d bytes of data"), DataLen);
		}
	}

	return Sent > 0;
}


FBackChannelOSCDispatch& FBackChannelOSCConnection::GetDispatchMap()
{
	return DispatchMap;
}

void FBackChannelOSCConnection::DispatchMessages()
{
	FScopeLock Lock(&ReceiveMutex);

	for (auto& Packet : ReceivedPackets)
	{
		if (Packet->GetType() == OSCPacketType::Message)
		{
			TSharedPtr<FBackChannelOSCMessage> Msg = StaticCastSharedPtr<FBackChannelOSCMessage>(Packet);

			DispatchMap.DispatchMessage(*Msg.Get());
		}
	}

	ReceivedPackets.Empty();
}